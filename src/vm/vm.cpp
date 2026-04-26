#include "qppjs/vm/vm.h"

#include "qppjs/base/error.h"
#include "qppjs/frontend/ast.h"
#include "qppjs/runtime/completion.h"
#include "qppjs/runtime/environment.h"
#include "qppjs/runtime/js_function.h"
#include "qppjs/runtime/js_object.h"
#include "qppjs/runtime/module_loader.h"
#include "qppjs/runtime/module_record.h"
#include "qppjs/runtime/native_errors.h"
#include "qppjs/runtime/value.h"
#include "qppjs/vm/bytecode.h"
#include "qppjs/vm/compiler.h"
#include "qppjs/vm/opcode.h"

#include <cassert>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <span>
#include <string>

namespace qppjs {

// ============================================================
// Type conversion helpers
// ============================================================

bool VM::to_boolean(const Value& v) {
    switch (v.kind()) {
    case ValueKind::Undefined: return false;
    case ValueKind::Null:      return false;
    case ValueKind::Bool:      return v.as_bool();
    case ValueKind::Number: {
        double n = v.as_number();
        return n != 0.0 && !std::isnan(n);
    }
    case ValueKind::String:  return !v.as_string().empty();
    case ValueKind::Object:  return true;
    }
    return false;
}

EvalResult VM::to_number(const Value& v) {
    switch (v.kind()) {
    case ValueKind::Undefined:
        return EvalResult::ok(Value::number(std::numeric_limits<double>::quiet_NaN()));
    case ValueKind::Null:
        return EvalResult::ok(Value::number(0.0));
    case ValueKind::Bool:
        return EvalResult::ok(Value::number(v.as_bool() ? 1.0 : 0.0));
    case ValueKind::Number:
        return EvalResult::ok(v);
    case ValueKind::String: {
        const std::string& s = v.as_string();
        if (s.empty()) {
            return EvalResult::ok(Value::number(0.0));
        }
        char* end = nullptr;
        double result = std::strtod(s.c_str(), &end);
        if (end == s.c_str() || *end != '\0') {
            return EvalResult::ok(Value::number(std::numeric_limits<double>::quiet_NaN()));
        }
        return EvalResult::ok(Value::number(result));
    }
    case ValueKind::Object:
        return EvalResult::ok(Value::number(std::numeric_limits<double>::quiet_NaN()));
    }
    return EvalResult::ok(Value::number(std::numeric_limits<double>::quiet_NaN()));
}

static std::string strip_error_prefix(const std::string& msg) {
    auto pos = msg.find(": ");
    if (pos != std::string::npos) return msg.substr(pos + 2);
    return msg;
}

static std::string number_to_string(double d) {
    if (d == 0.0) return "0";
    if (d == std::floor(d) && d >= -9007199254740992.0 && d <= 9007199254740992.0) {
        return std::to_string(static_cast<int64_t>(d));
    }
    char buf[32];
    auto [ptr, ec] = std::to_chars(buf, buf + 32, d, std::chars_format::general, 17);
    return std::string(buf, ptr);
}

std::string VM::to_string_val(const Value& v) {
    switch (v.kind()) {
    case ValueKind::Undefined: return "undefined";
    case ValueKind::Null:      return "null";
    case ValueKind::Bool:      return v.as_bool() ? "true" : "false";
    case ValueKind::Number: {
        double n = v.as_number();
        if (std::isnan(n))  return "NaN";
        if (std::isinf(n))  return n > 0 ? "Infinity" : "-Infinity";
        return number_to_string(n);
    }
    case ValueKind::String: return v.as_string();
    case ValueKind::Object: {
        RcObject* obj = v.as_object_raw();
        if (obj && obj->object_kind() == ObjectKind::kFunction) {
            return "function";
        }
        return "[object Object]";
    }
    }
    return "undefined";
}

static bool strict_eq(const Value& a, const Value& b) {
    if (a.kind() != b.kind()) return false;
    switch (a.kind()) {
    case ValueKind::Undefined: return true;
    case ValueKind::Null:      return true;
    case ValueKind::Bool:      return a.as_bool() == b.as_bool();
    case ValueKind::Number: {
        double na = a.as_number(), nb = b.as_number();
        if (std::isnan(na) || std::isnan(nb)) return false;
        return na == nb;
    }
    case ValueKind::String: return a.as_string() == b.as_string();
    case ValueKind::Object: return a.as_object_raw() == b.as_object_raw();
    }
    return false;
}

bool VM::abstract_eq(const Value& a, const Value& b) {
    if (a.kind() == b.kind()) return strict_eq(a, b);
    bool a_nullish = a.is_null() || a.is_undefined();
    bool b_nullish = b.is_null() || b.is_undefined();
    if (a_nullish && b_nullish) return true;
    if (a_nullish || b_nullish)  return false;
    if (a.is_bool()) return abstract_eq(Value::number(a.as_bool() ? 1.0 : 0.0), b);
    if (b.is_bool()) return abstract_eq(a, Value::number(b.as_bool() ? 1.0 : 0.0));
    if (a.is_string() && b.is_number()) {
        char* end = nullptr;
        const std::string& s = a.as_string();
        double n = s.empty() ? 0.0 : std::strtod(s.c_str(), &end);
        if (!s.empty() && (end == s.c_str() || *end != '\0'))
            n = std::numeric_limits<double>::quiet_NaN();
        return abstract_eq(Value::number(n), b);
    }
    if (a.is_number() && b.is_string()) {
        char* end = nullptr;
        const std::string& s = b.as_string();
        double n = s.empty() ? 0.0 : std::strtod(s.c_str(), &end);
        if (!s.empty() && (end == s.c_str() || *end != '\0'))
            n = std::numeric_limits<double>::quiet_NaN();
        return abstract_eq(a, Value::number(n));
    }
    return false;
}

// ============================================================
// VM constructor
// ============================================================

VM::VM() : object_prototype_(RcPtr<JSObject>::make()) {
    global_env_ = RcPtr<Environment>::make(RcPtr<Environment>());
}

static std::string value_to_message_string(const Value& v) {
    if (v.is_string()) return v.as_string();
    if (v.is_undefined()) return "";
    if (v.is_null()) return "null";
    if (v.is_bool()) return v.as_bool() ? "true" : "false";
    if (v.is_number()) {
        double n = v.as_number();
        if (std::isnan(n)) return "NaN";
        if (std::isinf(n)) return n > 0 ? "Infinity" : "-Infinity";
        return number_to_string(n);
    }
    return "[object Object]";
}

Value VM::make_error_value(NativeErrorType type, const std::string& message) {
    const auto& proto = error_protos_[static_cast<size_t>(type)];
    return MakeNativeErrorValue(proto, message);
}

void VM::init_global_env() {
    // Build Error.prototype (proto = object_prototype_)
    auto error_proto = RcPtr<JSObject>::make();
    error_proto->set_proto(object_prototype_);
    error_proto->set_property("name", Value::string("Error"));
    error_proto->set_property("message", Value::string(""));
    error_protos_[static_cast<size_t>(NativeErrorType::kError)] = error_proto;

    // Build Error constructor
    auto error_fn = RcPtr<JSFunction>::make();
    error_fn->set_name(std::string("Error"));
    error_fn->set_prototype_obj(error_proto);
    error_proto->set_constructor_property(error_fn.get());
    error_fn->set_native_fn([this](Value /*this_val*/, std::vector<Value> args, bool /*is_new_call*/) -> EvalResult {
        std::string msg = args.empty() ? "" : value_to_message_string(args[0]);
        return EvalResult::ok(make_error_value(NativeErrorType::kError, msg));
    });
    global_env_->define("Error", VarKind::Const);
    global_env_->initialize("Error", Value::object(ObjectPtr(error_fn)));

    // Helper to build a sub-error class
    struct SubErrorSpec {
        NativeErrorType type;
        const char* name;
    };
    static constexpr SubErrorSpec kSubErrors[] = {
        {NativeErrorType::kTypeError,      "TypeError"},
        {NativeErrorType::kReferenceError, "ReferenceError"},
        {NativeErrorType::kRangeError,     "RangeError"},
    };

    for (const auto& spec : kSubErrors) {
        auto sub_proto = RcPtr<JSObject>::make();
        sub_proto->set_proto(error_proto);  // inherits from Error.prototype
        sub_proto->set_property("name", Value::string(spec.name));
        sub_proto->set_property("message", Value::string(""));
        error_protos_[static_cast<size_t>(spec.type)] = sub_proto;

        auto sub_fn = RcPtr<JSFunction>::make();
        sub_fn->set_name(std::string(spec.name));
        sub_fn->set_prototype_obj(sub_proto);
        sub_proto->set_constructor_property(sub_fn.get());
        NativeErrorType captured_type = spec.type;
        sub_fn->set_native_fn([this, captured_type](Value /*this_val*/, std::vector<Value> args, bool /*is_new_call*/) -> EvalResult {
            std::string msg = args.empty() ? "" : value_to_message_string(args[0]);
            return EvalResult::ok(make_error_value(captured_type, msg));
        });

        global_env_->define(spec.name, VarKind::Const);
        global_env_->initialize(spec.name, Value::object(ObjectPtr(sub_fn)));
    }

    // Build console.log
    auto log_fn = RcPtr<JSFunction>::make();
    log_fn->set_name(std::string("log"));
    log_fn->set_native_fn([](Value /*this_val*/, std::vector<Value> args, bool is_new_call) -> EvalResult {
        if (is_new_call) {
            return EvalResult::err(Error{ErrorKind::Runtime, "TypeError: console.log is not a constructor"});
        }
        std::string result;
        for (size_t i = 0; i < args.size(); ++i) {
            if (i > 0) result += " ";
            result += VM::to_string_val(args[i]);
        }
        std::cout << result << "\n";
        return EvalResult::ok(Value::undefined());
    });

    // Build console object
    auto console_obj = RcPtr<JSObject>::make();
    console_obj->set_proto(object_prototype_);
    console_obj->set_property("log", Value::object(ObjectPtr(log_fn)));
    global_env_->define("console", VarKind::Const);
    global_env_->initialize("console", Value::object(ObjectPtr(console_obj)));

    // Build Array.prototype
    array_prototype_ = RcPtr<JSObject>::make();
    array_prototype_->set_proto(object_prototype_);

    // Array.prototype.push
    auto push_fn = RcPtr<JSFunction>::make();
    push_fn->set_name(std::string("push"));
    push_fn->set_native_fn([](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        RcObject* raw = this_val.as_object_raw();
        if (!raw || raw->object_kind() != ObjectKind::kArray) {
            return EvalResult::err(Error{ErrorKind::Runtime, "TypeError: push called on non-array"});
        }
        auto* arr = static_cast<JSObject*>(raw);
        for (auto& arg : args) {
            if (arr->array_length_ == UINT32_MAX) {
                return EvalResult::err(Error{ErrorKind::Runtime, "RangeError: Invalid array length"});
            }
            arr->elements_[arr->array_length_] = std::move(arg);
            arr->array_length_++;
        }
        return EvalResult::ok(Value::number(static_cast<double>(arr->array_length_)));
    });
    array_prototype_->set_property("push", Value::object(ObjectPtr(push_fn)));

    // Array.prototype.pop
    auto pop_fn = RcPtr<JSFunction>::make();
    pop_fn->set_name(std::string("pop"));
    pop_fn->set_native_fn([](Value this_val, std::vector<Value> /*args*/, bool) -> EvalResult {
        RcObject* raw = this_val.as_object_raw();
        if (!raw || raw->object_kind() != ObjectKind::kArray) {
            return EvalResult::err(Error{ErrorKind::Runtime, "TypeError: pop called on non-array"});
        }
        auto* arr = static_cast<JSObject*>(raw);
        if (arr->array_length_ == 0) {
            return EvalResult::ok(Value::undefined());
        }
        uint32_t last_idx = arr->array_length_ - 1;
        Value last = Value::undefined();
        auto it = arr->elements_.find(last_idx);
        if (it != arr->elements_.end()) {
            last = std::move(it->second);
            arr->elements_.erase(it);
        }
        arr->array_length_ = last_idx;
        return EvalResult::ok(std::move(last));
    });
    array_prototype_->set_property("pop", Value::object(ObjectPtr(pop_fn)));

    // Array.prototype.forEach — captured VM* for call_function_val
    auto foreach_fn = RcPtr<JSFunction>::make();
    foreach_fn->set_name(std::string("forEach"));
    foreach_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        RcObject* raw = this_val.as_object_raw();
        if (!raw || raw->object_kind() != ObjectKind::kArray) {
            return EvalResult::err(Error{ErrorKind::Runtime, "TypeError: forEach called on non-array"});
        }
        auto* arr = static_cast<JSObject*>(raw);
        if (args.empty() || !args[0].is_object()) {
            return EvalResult::err(Error{ErrorKind::Runtime, "TypeError: callback is not a function"});
        }
        Value callback = args[0];
        uint32_t len = arr->array_length_;
        for (uint32_t i = 0; i < len; i++) {
            auto elem_it = arr->elements_.find(i);
            Value elem = elem_it != arr->elements_.end() ? elem_it->second : Value::undefined();
            Value call_args[3] = {elem, Value::number(static_cast<double>(i)), this_val};
            std::span<Value> arg_span(call_args, 3);
            auto res = call_function_val(callback, Value::undefined(), arg_span);
            if (!res.is_ok()) return res;
        }
        return EvalResult::ok(Value::undefined());
    });
    array_prototype_->set_property("forEach", Value::object(ObjectPtr(foreach_fn)));

    // Build Object.keys
    auto keys_fn = RcPtr<JSFunction>::make();
    keys_fn->set_name(std::string("keys"));
    keys_fn->set_native_fn([this](Value /*this_val*/, std::vector<Value> args, bool) -> EvalResult {
        if (args.empty() || !args[0].is_object()) {
            return EvalResult::err(Error{ErrorKind::Runtime, "TypeError: Object.keys called on non-object"});
        }
        RcObject* raw = args[0].as_object_raw();
        if (raw->object_kind() == ObjectKind::kFunction) {
            auto arr = RcPtr<JSObject>::make(ObjectKind::kArray);
            gc_heap_.Register(arr.get());
            arr->set_proto(array_prototype_);
            arr->array_length_ = 0;
            return EvalResult::ok(Value::object(ObjectPtr(arr)));
        }
        auto* obj = static_cast<JSObject*>(raw);
        auto keys = obj->own_enumerable_string_keys();
        auto arr = RcPtr<JSObject>::make(ObjectKind::kArray);
        gc_heap_.Register(arr.get());
        arr->set_proto(array_prototype_);
        for (size_t i = 0; i < keys.size(); ++i) {
            arr->elements_[static_cast<uint32_t>(i)] = Value::string(keys[i]);
        }
        arr->array_length_ = static_cast<uint32_t>(keys.size());
        return EvalResult::ok(Value::object(ObjectPtr(arr)));
    });

    // Build Object.assign
    auto assign_fn = RcPtr<JSFunction>::make();
    assign_fn->set_name(std::string("assign"));
    assign_fn->set_native_fn([](Value /*this_val*/, std::vector<Value> args, bool) -> EvalResult {
        if (args.empty() || !args[0].is_object()) {
            return EvalResult::err(Error{ErrorKind::Runtime, "TypeError: Object.assign called on non-object"});
        }
        RcObject* target_raw = args[0].as_object_raw();
        if (target_raw->object_kind() == ObjectKind::kFunction) {
            return EvalResult::ok(args[0]);
        }
        auto* target = static_cast<JSObject*>(target_raw);
        bool target_is_array = target_raw->object_kind() == ObjectKind::kArray;
        for (size_t i = 1; i < args.size(); ++i) {
            const Value& source = args[i];
            if (source.is_null() || source.is_undefined()) continue;
            if (!source.is_object()) continue;
            RcObject* src_raw = source.as_object_raw();
            if (src_raw->object_kind() == ObjectKind::kFunction) continue;
            auto* src = static_cast<JSObject*>(src_raw);
            auto keys = src->own_enumerable_string_keys();
            for (const auto& key : keys) {
                if (target_is_array) {
                    auto res = target->set_property_ex(key, src->get_property(key));
                    if (!res.is_ok()) return res;
                } else {
                    target->set_property(key, src->get_property(key));
                }
            }
        }
        return EvalResult::ok(args[0]);
    });

    // Build Object.create
    auto create_fn = RcPtr<JSFunction>::make();
    create_fn->set_name(std::string("create"));
    create_fn->set_native_fn([this](Value /*this_val*/, std::vector<Value> args, bool) -> EvalResult {
        if (args.empty()) {
            return EvalResult::err(Error{ErrorKind::Runtime, "TypeError: Object.create requires an argument"});
        }
        const Value& proto_arg = args[0];
        if (!proto_arg.is_null() && !proto_arg.is_object()) {
            return EvalResult::err(Error{ErrorKind::Runtime,
                                        "TypeError: Object prototype may only be an Object or null"});
        }
        auto new_obj = RcPtr<JSObject>::make();
        gc_heap_.Register(new_obj.get());
        if (!proto_arg.is_null()) {
            RcObject* proto_raw = proto_arg.as_object_raw();
            ObjectKind kind = proto_raw->object_kind();
            if (kind == ObjectKind::kFunction) {
                return EvalResult::err(Error{ErrorKind::Runtime,
                                            "TypeError: Object prototype may only be an Object or null"});
            }
            new_obj->set_proto(RcPtr<JSObject>(static_cast<JSObject*>(proto_raw)));
        }
        return EvalResult::ok(Value::object(ObjectPtr(new_obj)));
    });

    // Build Object constructor function
    object_constructor_ = RcPtr<JSFunction>::make();
    object_constructor_->set_name(std::string("Object"));
    object_constructor_->set_prototype_obj(object_prototype_);
    object_constructor_->set_native_fn([this](Value /*this_val*/, std::vector<Value> args, bool) -> EvalResult {
        bool wrap = args.empty() || args[0].is_null() || args[0].is_undefined();
        if (wrap) {
            auto obj = RcPtr<JSObject>::make();
            gc_heap_.Register(obj.get());
            obj->set_proto(object_prototype_);
            return EvalResult::ok(Value::object(ObjectPtr(obj)));
        }
        return EvalResult::ok(args[0]);
    });
    object_constructor_->set_property("keys", Value::object(ObjectPtr(keys_fn)));
    object_constructor_->set_property("assign", Value::object(ObjectPtr(assign_fn)));
    object_constructor_->set_property("create", Value::object(ObjectPtr(create_fn)));

    global_env_->define_initialized("Object");
    global_env_->set("Object", Value::object(ObjectPtr(object_constructor_)));

    // Build Function.prototype with call/apply/bind
    function_prototype_ = RcPtr<JSObject>::make();
    function_prototype_->set_proto(object_prototype_);

    // Function.prototype.call
    auto call_fn = RcPtr<JSFunction>::make();
    call_fn->set_name(std::string("call"));
    call_fn->set_property("length", Value::number(1.0));
    call_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        if (!this_val.is_object() || !this_val.as_object_raw() ||
            this_val.as_object_raw()->object_kind() != ObjectKind::kFunction) {
            return EvalResult::err(Error(ErrorKind::Runtime,
                                        "TypeError: Function.prototype.call called on non-function"));
        }
        Value new_this = args.empty() ? Value::undefined() : args[0];
        std::span<Value> call_args;
        if (args.size() > 1) {
            call_args = std::span<Value>(args.data() + 1, args.size() - 1);
        }
        return call_function_val(this_val, std::move(new_this), call_args);
    });
    function_prototype_->set_property("call", Value::object(ObjectPtr(call_fn)));

    // Function.prototype.apply
    auto apply_fn = RcPtr<JSFunction>::make();
    apply_fn->set_name(std::string("apply"));
    apply_fn->set_property("length", Value::number(2.0));
    apply_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        if (!this_val.is_object() || !this_val.as_object_raw() ||
            this_val.as_object_raw()->object_kind() != ObjectKind::kFunction) {
            return EvalResult::err(Error(ErrorKind::Runtime,
                                        "TypeError: Function.prototype.apply called on non-function"));
        }
        Value new_this = args.empty() ? Value::undefined() : args[0];
        Value args_array = args.size() > 1 ? args[1] : Value::undefined();
        if (args_array.is_null() || args_array.is_undefined()) {
            std::span<Value> empty_span;
            return call_function_val(this_val, std::move(new_this), empty_span);
        }
        if (!args_array.is_object()) {
            return EvalResult::err(Error(ErrorKind::Runtime,
                "TypeError: apply argument must be an array or array-like object"));
        }
        RcObject* arr_raw = args_array.as_object_raw();
        std::vector<Value> call_args;
        if (arr_raw->object_kind() == ObjectKind::kArray) {
            auto* arr = static_cast<JSObject*>(arr_raw);
            uint32_t len = arr->array_length_;
            call_args.reserve(len);
            for (uint32_t i = 0; i < len; ++i) {
                auto it = arr->elements_.find(i);
                call_args.push_back(it != arr->elements_.end() ? it->second : Value::undefined());
            }
        } else if (arr_raw->object_kind() == ObjectKind::kOrdinary) {
            auto* obj = static_cast<JSObject*>(arr_raw);
            Value len_val = obj->get_property("length");
            double len_num = len_val.is_number() ? len_val.as_number() : 0.0;
            uint32_t len = 0;
            if (!std::isnan(len_num) && len_num > 0.0) {
                if (len_num > 65535.0) {
                    return EvalResult::err(Error(ErrorKind::Runtime,
                        "RangeError: apply argsArray length exceeds limit"));
                }
                len = static_cast<uint32_t>(len_num);
            }
            call_args.reserve(len);
            for (uint32_t i = 0; i < len; ++i) {
                call_args.push_back(obj->get_property(std::to_string(i)));
            }
        } else {
            return EvalResult::err(Error(ErrorKind::Runtime,
                "TypeError: apply argument must be an array or array-like object"));
        }
        return call_function_val(this_val, std::move(new_this),
                                 std::span<Value>(call_args.data(), call_args.size()));
    });
    function_prototype_->set_property("apply", Value::object(ObjectPtr(apply_fn)));

    // Function.prototype.bind
    auto bind_fn = RcPtr<JSFunction>::make();
    bind_fn->set_name(std::string("bind"));
    bind_fn->set_property("length", Value::number(1.0));
    bind_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        if (!this_val.is_object() || !this_val.as_object_raw() ||
            this_val.as_object_raw()->object_kind() != ObjectKind::kFunction) {
            return EvalResult::err(Error(ErrorKind::Runtime,
                                        "TypeError: Function.prototype.bind called on non-function"));
        }
        Value bound_this = args.empty() ? Value::undefined() : args[0];
        std::vector<Value> bound_args;
        if (args.size() > 1) {
            bound_args.assign(args.begin() + 1, args.end());
        }

        auto* target_raw = static_cast<JSFunction*>(this_val.as_object_raw());
        double target_length = 0.0;
        Value len_prop = target_raw->get_property("length");
        if (len_prop.is_number()) {
            target_length = len_prop.as_number();
        } else {
            target_length = static_cast<double>(target_raw->params().size());
        }
        double bound_length = std::max(0.0, target_length - static_cast<double>(bound_args.size()));

        // Compute bound name — prefer own_properties_["name"] for chained bind
        std::string target_name;
        {
            Value name_prop = target_raw->get_property("name");
            if (name_prop.is_string()) {
                target_name = name_prop.as_string();
            } else if (target_raw->name().has_value()) {
                target_name = target_raw->name().value();
            }
        }

        auto new_fn = RcPtr<JSFunction>::make();
        new_fn->set_bound(this_val, std::move(bound_this), std::move(bound_args));
        JSFunction* self_raw = new_fn.get();
        new_fn->set_native_fn([this, self_raw]
                              (Value /*this_val*/, std::vector<Value> call_args, bool is_new_call) -> EvalResult {
            const Value& captured_target = self_raw->bound_target();
            const Value& captured_this = self_raw->bound_this_val();
            const std::vector<Value>& captured_args = self_raw->bound_args();
            std::vector<Value> merged;
            merged.reserve(captured_args.size() + call_args.size());
            merged = captured_args;
            merged.insert(merged.end(), call_args.begin(), call_args.end());
            if (is_new_call) {
                // new semantics: ignore captured_this, create new instance from target
                auto* target_fn_raw = static_cast<JSFunction*>(captured_target.as_object_raw());
                auto target_fn = RcPtr<JSFunction>(target_fn_raw);
                if (target_fn->is_native()) {
                    return target_fn->native_fn()(Value::undefined(),
                                                  std::vector<Value>(merged.begin(), merged.end()),
                                                  /*is_new_call=*/true);
                }
                auto instance = RcPtr<JSObject>::make();
                gc_heap_.Register(instance.get());
                const auto& proto_obj = target_fn->prototype_obj();
                if (proto_obj) {
                    instance->set_proto(proto_obj);
                } else {
                    instance->set_proto(object_prototype_);
                }
                Value instance_val = Value::object(ObjectPtr(instance));
                Value instance_copy = instance_val;
                size_t exit_depth = call_stack_.size();
                auto push_res = push_call_frame(target_fn, instance_val,
                                                std::span<Value>(merged.data(), merged.size()),
                                                /*is_new=*/true, std::move(instance_copy));
                if (!push_res.is_ok()) return push_res;
                return run(exit_depth);
            }
            return call_function_val(captured_target, captured_this,
                                     std::span<Value>(merged.data(), merged.size()));
        });
        new_fn->set_property("length", Value::number(bound_length));
        new_fn->set_property("name", Value::string("bound " + target_name));
        gc_heap_.Register(new_fn.get());

        return EvalResult::ok(Value::object(ObjectPtr(new_fn)));
    });
    function_prototype_->set_property("bind", Value::object(ObjectPtr(bind_fn)));

    // Register the global environment with GcHeap.
    gc_heap_.Register(global_env_.get());
}

// ============================================================
// exec (public entry)
// ============================================================

EvalResult VM::exec(std::shared_ptr<BytecodeFunction> bytecode) {
    global_env_ = RcPtr<Environment>::make(RcPtr<Environment>());
    init_global_env();

    // Pre-define var_decls for the top-level scope
    for (uint16_t idx : bytecode->var_decls) {
        global_env_->define_initialized(bytecode->names[idx]);
    }
    for (uint16_t idx : bytecode->function_decls) {
        global_env_->define_function(bytecode->names[idx]);
    }

    CallFrame frame;
    frame.bytecode = bytecode.get();
    frame.pc = 0;
    frame.env = global_env_;
    frame.this_val = Value::undefined();

    call_stack_.push_back(std::move(frame));
    EvalResult result = run(0);

    // GC: collect unreachable objects (resolves P3-2 closure circular references).
    // Run GC before clear_function_bindings so all reachable objects are correctly identified.
    {
        std::vector<RcObject*> roots;
        auto add_obj = [&](RcObject* p) { if (p) roots.push_back(p); };
        auto add_val = [&](const Value& v) { if (v.is_object()) add_obj(v.as_object_raw()); };

        add_obj(global_env_.get());
        add_obj(object_prototype_.get());
        add_obj(array_prototype_.get());
        add_obj(function_prototype_.get());
        add_obj(object_constructor_.get());
        for (auto& ep : error_protos_) add_obj(ep.get());
        // Include call stack frames
        for (auto& cf : call_stack_) {
            add_obj(cf.env.get());
            add_val(cf.this_val);
            add_val(cf.new_instance);
            for (const auto& v : cf.stack) add_val(v);
            if (cf.pending_throw.has_value()) add_val(*cf.pending_throw);
            if (cf.caught_exception.has_value()) add_val(*cf.caught_exception);
        }
        // Include the result value
        if (result.is_ok()) add_val(result.value());

        gc_heap_.Collect(roots);
    }

    global_env_->clear_function_bindings();
    object_prototype_->clear_function_properties();
    if (array_prototype_) array_prototype_->clear_function_properties();
    if (function_prototype_) function_prototype_->clear_function_properties();
    if (object_constructor_) object_constructor_->clear_own_properties();

    return result;
}

// ============================================================
// Bytecode read helpers
// ============================================================

static uint8_t read_u8(const BytecodeFunction* bc, size_t& pc) {
    return bc->code[pc++];
}

static uint16_t read_u16(const BytecodeFunction* bc, size_t& pc) {
    uint16_t hi = bc->code[pc++];
    uint16_t lo = bc->code[pc++];
    return static_cast<uint16_t>((hi << 8) | lo);
}

static int32_t read_i32(const BytecodeFunction* bc, size_t& pc) {
    uint32_t b0 = bc->code[pc++];
    uint32_t b1 = bc->code[pc++];
    uint32_t b2 = bc->code[pc++];
    uint32_t b3 = bc->code[pc++];
    uint32_t v = (b0 << 24) | (b1 << 16) | (b2 << 8) | b3;
    return static_cast<int32_t>(v);
}

// ============================================================
// push_call_frame
// ============================================================

EvalResult VM::push_call_frame(RcPtr<JSFunction> fn, Value this_val, std::span<Value> args,
                               bool is_new, Value new_instance) {
    if (call_depth_ >= kMaxCallDepth) {
        return EvalResult::err(Error(ErrorKind::Runtime, "RangeError: Maximum call stack size exceeded"));
    }
    call_depth_++;

    const auto& bc = fn->bytecode();
    if (!bc) {
        call_depth_--;
        return EvalResult::err(Error(ErrorKind::Runtime, "Internal: function has no bytecode"));
    }

    RcPtr<Environment> outer = fn->closure_env() ? fn->closure_env() : global_env_;
    auto fn_env = RcPtr<Environment>::make(outer);
    gc_heap_.Register(fn_env.get());
    if (fn->is_named_expr() && fn->name().has_value()) {
        fn_env->define(fn->name().value(), VarKind::Const);
        auto init_result = fn_env->initialize(fn->name().value(), Value::object(ObjectPtr(fn)));
        if (!init_result.is_ok()) {
            call_depth_--;
            return init_result;
        }
    }

    // Bind parameters
    const auto& params = fn->params();
    for (size_t i = 0; i < params.size(); ++i) {
        Value arg = (i < args.size()) ? args[i] : Value::undefined();
        fn_env->define(params[i], VarKind::Var);
        fn_env->initialize(params[i], std::move(arg));
    }

    // Pre-define var_decls bindings
    for (uint16_t idx : bc->var_decls) {
        fn_env->define_initialized(bc->names[idx]);
    }
    for (uint16_t idx : bc->function_decls) {
        fn_env->define_function(bc->names[idx]);
    }

    CallFrame frame;
    frame.bytecode = bc.get();
    frame.pc = 0;
    frame.env = fn_env;
    frame.this_val = std::move(this_val);
    frame.is_new_call = is_new;
    if (is_new) frame.new_instance = std::move(new_instance);

    call_stack_.push_back(std::move(frame));
    return EvalResult::ok(Value::undefined());
}

// ============================================================
// call_function_val — call a JS/native function from within a NativeFn
// ============================================================

EvalResult VM::call_function_val(Value fn_val, Value this_val, std::span<Value> args) {
    if (!fn_val.is_object() || !fn_val.as_object_raw() ||
        fn_val.as_object_raw()->object_kind() != ObjectKind::kFunction) {
        return EvalResult::err(Error(ErrorKind::Runtime, "TypeError: value is not a function"));
    }
    auto* fn_raw = static_cast<JSFunction*>(fn_val.as_object_raw());
    auto fn = RcPtr<JSFunction>(fn_raw);

    if (fn->is_native()) {
        return fn->native_fn()(this_val, std::vector<Value>(args.begin(), args.end()), false);
    }

    size_t exit_depth = call_stack_.size();
    auto push_res = push_call_frame(fn, std::move(this_val), args);
    if (!push_res.is_ok()) return push_res;
    return run(exit_depth);
}

// ============================================================
// Main dispatch loop
// ============================================================

EvalResult VM::run(size_t exit_depth) {
    while (call_stack_.size() > exit_depth) {
        // Re-fetch the top frame each iteration to avoid stale references after push_back
        CallFrame& frame = call_stack_.back();

        // ---- Exception propagation check ----
        // If pending_throw is set at the top of the loop, try to find a handler.
        if (frame.pending_throw.has_value()) {
            if (!frame.handler_stack.empty()) {
                ExceptionHandler handler = frame.handler_stack.back();
                frame.handler_stack.pop_back();
                // Restore operand stack depth (truncate — stack_depth <= current size)
                while (frame.stack.size() > handler.stack_depth) {
                    frame.stack.pop_back();
                }
                // Restore scope depth (pop extra scopes)
                while (frame.scope_depth > handler.scope_depth) {
                    RcPtr<Environment> parent = frame.env->outer();
                    frame.env = std::move(parent);
                    frame.scope_depth--;
                }
                frame.pc = handler.catch_target;
                // Transfer pending_throw to caught_exception so that
                // pending_throw is cleared (dispatch can run without re-triggering exception logic),
                // and GetException can retrieve the value.
                frame.caught_exception = std::move(frame.pending_throw);
                frame.pending_throw = std::nullopt;
                goto dispatch_begin;
            } else {
                // No handler in current frame — propagate to caller
                Value thrown = std::move(*frame.pending_throw);
                frame.pending_throw = std::nullopt;
                bool is_top = (call_stack_.size() <= exit_depth + 1);
                call_stack_.pop_back();
                call_depth_--;
                if (!is_top && call_stack_.size() > exit_depth) {
                    // Set pending_throw in the caller frame
                    call_stack_.back().pending_throw = std::move(thrown);
                    continue;
                }
                // Reached top level — convert to EvalResult error
                std::string msg;
                if (thrown.is_string()) {
                    msg = thrown.as_string();
                } else if (thrown.is_number()) {
                    msg = to_string_val(thrown);
                } else if (thrown.is_object()) {
                    RcObject* raw = thrown.as_object_raw();
                    if (raw && raw->object_kind() == ObjectKind::kOrdinary) {
                        auto* obj = static_cast<JSObject*>(raw);
                        Value name_val = obj->get_property("name");
                        Value m = obj->get_property("message");
                        std::string name_str = name_val.is_string() ? name_val.as_string() : "";
                        std::string msg_str = m.is_string() ? m.as_string() : "[object Object]";
                        if (!name_str.empty()) {
                            msg = name_str + ": " + msg_str;
                        } else {
                            msg = msg_str;
                        }
                    } else {
                        msg = "[object]";
                    }
                } else {
                    msg = to_string_val(thrown);
                }
                return EvalResult::err(Error(ErrorKind::Runtime, msg));
            }
        }

        dispatch_begin:

        const BytecodeFunction* bc = frame.bytecode;

        if (frame.pc >= bc->code.size()) {
            // Implicit ReturnUndefined at end of bytecode
            Value ret = Value::undefined();
            bool is_new = frame.is_new_call;
            Value instance = is_new ? std::move(frame.new_instance) : Value::undefined();
            call_stack_.pop_back();
            call_depth_--;
            if (call_stack_.size() > exit_depth) {
                if (is_new) {
                    // do_new: constructor ran to completion with no explicit return
                    call_stack_.back().stack.push_back(std::move(instance));
                } else {
                    call_stack_.back().stack.push_back(std::move(ret));
                }
                continue;
            }
            if (is_new) return EvalResult::ok(instance);
            return EvalResult::ok(ret);
        }

        size_t& pc = frame.pc;
        std::vector<Value>& stack = frame.stack;
        RcPtr<Environment>& env = frame.env;

        auto op = static_cast<Opcode>(read_u8(bc, pc));

        switch (op) {

        // ---- Value loading ----

        case Opcode::kLoadUndefined:
            stack.push_back(Value::undefined());
            break;

        case Opcode::kLoadNull:
            stack.push_back(Value::null());
            break;

        case Opcode::kLoadTrue:
            stack.push_back(Value::boolean(true));
            break;

        case Opcode::kLoadFalse:
            stack.push_back(Value::boolean(false));
            break;

        case Opcode::kLoadNumber: {
            uint16_t idx = read_u16(bc, pc);
            stack.push_back(bc->constants[idx]);
            break;
        }

        case Opcode::kLoadString: {
            uint16_t idx = read_u16(bc, pc);
            stack.push_back(bc->constants[idx]);
            break;
        }

        case Opcode::kLoadThis:
            stack.push_back(frame.this_val);
            break;

        // ---- Variables ----

        case Opcode::kGetVar: {
            uint16_t idx = read_u16(bc, pc);
            const std::string& name = bc->names[idx];
            if (name == "undefined") {
                stack.push_back(Value::undefined());
                break;
            }
            auto result = env->get(name);
            if (!result.is_ok()) {
                const std::string& msg = result.error().message();
                NativeErrorType err_type = NativeErrorType::kReferenceError;
                if (msg.rfind("TypeError:", 0) == 0) {
                    err_type = NativeErrorType::kTypeError;
                }
                frame.pending_throw = make_error_value(err_type, strip_error_prefix(msg));
                continue;
            }
            stack.push_back(result.value());
            break;
        }

        case Opcode::kSetVar: {
            uint16_t idx = read_u16(bc, pc);
            const std::string& name = bc->names[idx];
            auto result = env->set(name, stack.back());
            if (!result.is_ok()) {
                const std::string& msg = result.error().message();
                NativeErrorType err_type = NativeErrorType::kTypeError;
                if (msg.rfind("ReferenceError:", 0) == 0) {
                    err_type = NativeErrorType::kReferenceError;
                }
                frame.pending_throw = make_error_value(err_type, strip_error_prefix(msg));
                continue;
            }
            // value stays on stack
            break;
        }

        case Opcode::kDefVar: {
            uint16_t idx = read_u16(bc, pc);
            // 模块环境中 Link 阶段已建立 Binding（共享 Cell），跳过 define
            if (env->bindings().find(bc->names[idx]) == nullptr) {
                env->define_initialized(bc->names[idx]);
            }
            break;
        }

        case Opcode::kDefLet: {
            uint16_t idx = read_u16(bc, pc);
            // 模块环境中 Link 阶段已建立 Binding（共享 Cell），跳过 define
            if (env->bindings().find(bc->names[idx]) == nullptr) {
                env->define(bc->names[idx], VarKind::Let);
            }
            break;
        }

        case Opcode::kDefConst: {
            uint16_t idx = read_u16(bc, pc);
            // 模块环境中 Link 阶段已建立 Binding（共享 Cell），跳过 define
            if (env->bindings().find(bc->names[idx]) == nullptr) {
                env->define(bc->names[idx], VarKind::Const);
            }
            break;
        }

        case Opcode::kInitVar: {
            uint16_t idx = read_u16(bc, pc);
            Value val = std::move(stack.back());
            stack.pop_back();
            auto result = env->initialize(bc->names[idx], val);
            if (!result.is_ok()) {
                const std::string& msg = result.error().message();
                NativeErrorType err_type = NativeErrorType::kTypeError;
                if (msg.rfind("ReferenceError:", 0) == 0) {
                    err_type = NativeErrorType::kReferenceError;
                }
                frame.pending_throw = make_error_value(err_type, strip_error_prefix(msg));
                continue;
            }
            stack.push_back(result.value());
            break;
        }

        // ---- Scope ----

        case Opcode::kPushScope: {
            auto new_env = RcPtr<Environment>::make(env);
            gc_heap_.Register(new_env.get());
            env = std::move(new_env);
            frame.scope_depth++;
            break;
        }

        case Opcode::kPopScope: {
            RcPtr<Environment> parent = env->outer();
            env = std::move(parent);
            frame.scope_depth--;
            break;
        }

        // ---- Object properties ----

        case Opcode::kNewObject: {
            auto obj = RcPtr<JSObject>::make();
            gc_heap_.Register(obj.get());
            obj->set_proto(object_prototype_);
            stack.push_back(Value::object(ObjectPtr(obj)));
            break;
        }

        case Opcode::kNewArray: {
            auto arr = RcPtr<JSObject>::make(ObjectKind::kArray);
            gc_heap_.Register(arr.get());
            arr->set_proto(array_prototype_);
            stack.push_back(Value::object(ObjectPtr(arr)));
            break;
        }

        case Opcode::kGetProp: {
            uint16_t idx = read_u16(bc, pc);
            const std::string& name = bc->names[idx];
            Value obj_val = std::move(stack.back());
            stack.pop_back();
            if (obj_val.is_undefined() || obj_val.is_null()) {
                frame.pending_throw = make_error_value(NativeErrorType::kTypeError,
                    "Cannot read property '" + name + "' of " + to_string_val(obj_val));
                continue;
            }
            if (!obj_val.is_object()) {
                stack.push_back(Value::undefined());
                break;
            }
            // Handle JSFunction specially (e.g., Fn.prototype, Object.keys)
            RcObject* raw_obj = obj_val.as_object_raw();
            if (raw_obj->object_kind() == ObjectKind::kFunction) {
                auto* fn = static_cast<JSFunction*>(raw_obj);
                if (name == "prototype") {
                    const auto& proto = fn->prototype_obj();
                    stack.push_back(proto ? Value::object(ObjectPtr(proto)) : Value::undefined());
                } else {
                    Value own = fn->get_property(name);
                    if (!own.is_undefined()) {
                        stack.push_back(std::move(own));
                    } else if (function_prototype_) {
                        stack.push_back(function_prototype_->get_property(name));
                    } else {
                        stack.push_back(Value::undefined());
                    }
                }
                break;
            }
            if (raw_obj->object_kind() != ObjectKind::kOrdinary && raw_obj->object_kind() != ObjectKind::kArray) {
                stack.push_back(Value::undefined());
                break;
            }
            auto* obj = static_cast<JSObject*>(raw_obj);
            stack.push_back(obj->get_property(name));
            break;
        }

        case Opcode::kSetProp: {
            uint16_t idx = read_u16(bc, pc);
            const std::string& name = bc->names[idx];
            Value val = std::move(stack.back());
            stack.pop_back();
            Value obj_val = std::move(stack.back());
            stack.pop_back();
            if (!obj_val.is_object()) {
                frame.pending_throw = make_error_value(NativeErrorType::kTypeError,
                    "Cannot set property '" + name + "' on non-object");
                continue;
            }
            // Handle JSFunction specially (e.g., Fn.prototype = ...)
            RcObject* raw_set = obj_val.as_object_raw();
            if (raw_set->object_kind() == ObjectKind::kFunction) {
                auto* fn = static_cast<JSFunction*>(raw_set);
                if (name == "prototype" && val.is_object()) {
                    RcObject* proto_raw = val.as_object_raw();
                    if (proto_raw && proto_raw->object_kind() == ObjectKind::kOrdinary) {
                        fn->set_prototype_obj(RcPtr<JSObject>(static_cast<JSObject*>(proto_raw)));
                    }
                }
                // Other properties on functions are silently ignored for now
                stack.push_back(std::move(val));
                break;
            }
            if (raw_set->object_kind() != ObjectKind::kOrdinary && raw_set->object_kind() != ObjectKind::kArray) {
                frame.pending_throw = make_error_value(NativeErrorType::kTypeError,
                    "Cannot set property '" + name + "' on non-JSObject");
                continue;
            }
            auto* obj = static_cast<JSObject*>(raw_set);
            auto set_ex_res = obj->set_property_ex(name, val);
            if (!set_ex_res.is_ok()) {
                const std::string& msg = set_ex_res.error().message();
                NativeErrorType err_type = NativeErrorType::kRangeError;
                if (msg.rfind("TypeError:", 0) == 0) err_type = NativeErrorType::kTypeError;
                frame.pending_throw = make_error_value(err_type, strip_error_prefix(msg));
                continue;
            }
            stack.push_back(std::move(val));
            break;
        }

        case Opcode::kGetElem: {
            Value key_val = std::move(stack.back());
            stack.pop_back();
            Value obj_val = std::move(stack.back());
            stack.pop_back();
            if (!obj_val.is_object()) {
                frame.pending_throw = make_error_value(NativeErrorType::kTypeError,
                    "Cannot read element of non-object");
                continue;
            }
            RcObject* raw_elem = obj_val.as_object_raw();
            if (!raw_elem) {
                frame.pending_throw = make_error_value(NativeErrorType::kTypeError,
                    "Cannot read element of non-JSObject");
                continue;
            }
            if (raw_elem->object_kind() == ObjectKind::kArray) {
                auto* arr = static_cast<JSObject*>(raw_elem);
                if (key_val.is_number()) {
                    double d = key_val.as_number();
                    if (d >= 0.0 && d == std::floor(d) && d < static_cast<double>(UINT32_MAX)) {
                        uint32_t idx = static_cast<uint32_t>(d);
                        auto it = arr->elements_.find(idx);
                        stack.push_back(it != arr->elements_.end() ? it->second : Value::undefined());
                        break;
                    }
                }
                stack.push_back(arr->get_property(to_string_val(key_val)));
                break;
            }
            if (raw_elem->object_kind() != ObjectKind::kOrdinary) {
                frame.pending_throw = make_error_value(NativeErrorType::kTypeError,
                    "Cannot read element of non-JSObject");
                continue;
            }
            auto* obj = static_cast<JSObject*>(raw_elem);
            stack.push_back(obj->get_property(to_string_val(key_val)));
            break;
        }

        case Opcode::kSetElem: {
            Value key_val = std::move(stack.back());
            stack.pop_back();
            Value val = std::move(stack.back());
            stack.pop_back();
            Value obj_val = std::move(stack.back());
            stack.pop_back();
            if (!obj_val.is_object()) {
                frame.pending_throw = make_error_value(NativeErrorType::kTypeError,
                    "Cannot set element on non-object");
                continue;
            }
            RcObject* raw_setelem = obj_val.as_object_raw();
            if (!raw_setelem) {
                frame.pending_throw = make_error_value(NativeErrorType::kTypeError,
                    "Cannot set element on non-JSObject");
                continue;
            }
            if (raw_setelem->object_kind() == ObjectKind::kArray) {
                auto* arr = static_cast<JSObject*>(raw_setelem);
                std::string key_str = to_string_val(key_val);
                auto set_ex_res = arr->set_property_ex(key_str, val);
                if (!set_ex_res.is_ok()) {
                    const std::string& msg = set_ex_res.error().message();
                    NativeErrorType err_type = NativeErrorType::kRangeError;
                    if (msg.rfind("TypeError:", 0) == 0) err_type = NativeErrorType::kTypeError;
                    frame.pending_throw = make_error_value(err_type, strip_error_prefix(msg));
                    continue;
                }
                stack.push_back(std::move(val));
                break;
            }
            if (raw_setelem->object_kind() != ObjectKind::kOrdinary) {
                frame.pending_throw = make_error_value(NativeErrorType::kTypeError,
                    "Cannot set element on non-JSObject");
                continue;
            }
            auto* obj = static_cast<JSObject*>(raw_setelem);
            obj->set_property(to_string_val(key_val), val);
            stack.push_back(std::move(val));
            break;
        }

        // ---- Functions and calls ----

        case Opcode::kMakeFunction: {
            uint16_t fn_idx = read_u16(bc, pc);
            const auto& fn_bc = bc->functions[fn_idx];
            auto fn = RcPtr<JSFunction>::make();
            fn->set_name(fn_bc->name);
            fn->set_params(fn_bc->params);
            fn->set_bytecode(fn_bc);
            fn->set_closure_env(env);
            fn->set_is_named_expr(fn_bc->is_named_expr);
            auto proto_obj = RcPtr<JSObject>::make();
            proto_obj->set_proto(object_prototype_);
            proto_obj->set_constructor_property(fn.get());
            fn->set_prototype_obj(proto_obj);
            gc_heap_.Register(fn.get());
            gc_heap_.Register(proto_obj.get());
            stack.push_back(Value::object(ObjectPtr(fn)));
            break;
        }

        case Opcode::kCall: {
            uint8_t argc = read_u8(bc, pc);
            constexpr int kSmallArgBuf = 8;
            Value small_buf[kSmallArgBuf];
            std::vector<Value> large_buf;
            Value* arg_data;
            if (argc <= kSmallArgBuf) {
                for (int i = argc - 1; i >= 0; --i) {
                    small_buf[i] = std::move(stack[stack.size() - argc + i]);
                }
                stack.resize(stack.size() - argc);
                arg_data = small_buf;
            } else {
                large_buf.resize(argc);
                for (int i = argc - 1; i >= 0; --i) {
                    large_buf[i] = std::move(stack[stack.size() - argc + i]);
                }
                stack.resize(stack.size() - argc);
                arg_data = large_buf.data();
            }
            std::span<Value> args(arg_data, argc);
            Value callee_val = std::move(stack.back());
            stack.pop_back();

            if (!callee_val.is_object()) {
                frame.pending_throw = make_error_value(NativeErrorType::kTypeError, "not a function");
                continue;
            }
            RcObject* call_raw = callee_val.as_object_raw();
            if (!call_raw || call_raw->object_kind() != ObjectKind::kFunction) {
                frame.pending_throw = make_error_value(NativeErrorType::kTypeError, "not a function");
                continue;
            }
            auto fn = RcPtr<JSFunction>(static_cast<JSFunction*>(call_raw));
            if (fn->is_native()) {
                auto res = fn->native_fn()(Value::undefined(), std::vector<Value>(args.begin(), args.end()), /*is_new_call=*/false);
                if (!res.is_ok()) {
                    const std::string& msg = res.error().message();
                    NativeErrorType err_type = NativeErrorType::kTypeError;
                    if (msg.rfind("ReferenceError:", 0) == 0) err_type = NativeErrorType::kReferenceError;
                    else if (msg.rfind("RangeError:", 0) == 0) err_type = NativeErrorType::kRangeError;
                    frame.pending_throw = make_error_value(err_type, strip_error_prefix(msg));
                    continue;
                }
                stack.push_back(res.value());
                break;
            }
            // Push new frame; the flat loop will execute it, then return value lands on our stack
            auto push_res = push_call_frame(fn, Value::undefined(), args);
            if (!push_res.is_ok()) {
                const std::string& msg = push_res.error().message();
                NativeErrorType err_type = NativeErrorType::kRangeError;
                if (msg.rfind("TypeError:", 0) == 0) err_type = NativeErrorType::kTypeError;
                frame.pending_throw = make_error_value(err_type, strip_error_prefix(msg));
                continue;
            }
            break;
        }

        case Opcode::kCallMethod: {
            uint8_t argc = read_u8(bc, pc);
            constexpr int kSmallArgBuf = 8;
            Value small_buf[kSmallArgBuf];
            std::vector<Value> large_buf;
            Value* arg_data;
            if (argc <= kSmallArgBuf) {
                for (int i = argc - 1; i >= 0; --i) {
                    small_buf[i] = std::move(stack[stack.size() - argc + i]);
                }
                stack.resize(stack.size() - argc);
                arg_data = small_buf;
            } else {
                large_buf.resize(argc);
                for (int i = argc - 1; i >= 0; --i) {
                    large_buf[i] = std::move(stack[stack.size() - argc + i]);
                }
                stack.resize(stack.size() - argc);
                arg_data = large_buf.data();
            }
            std::span<Value> args(arg_data, argc);
            Value callee_val = std::move(stack.back());
            stack.pop_back();
            Value receiver = std::move(stack.back());
            stack.pop_back();

            if (!callee_val.is_object()) {
                frame.pending_throw = make_error_value(NativeErrorType::kTypeError, "not a function");
                continue;
            }
            RcObject* callm_raw = callee_val.as_object_raw();
            if (!callm_raw || callm_raw->object_kind() != ObjectKind::kFunction) {
                frame.pending_throw = make_error_value(NativeErrorType::kTypeError, "not a function");
                continue;
            }
            auto fn = RcPtr<JSFunction>(static_cast<JSFunction*>(callm_raw));
            if (fn->is_native()) {
                auto res = fn->native_fn()(receiver, std::vector<Value>(args.begin(), args.end()), /*is_new_call=*/false);
                if (!res.is_ok()) {
                    const std::string& msg = res.error().message();
                    NativeErrorType err_type = NativeErrorType::kTypeError;
                    if (msg.rfind("ReferenceError:", 0) == 0) err_type = NativeErrorType::kReferenceError;
                    else if (msg.rfind("RangeError:", 0) == 0) err_type = NativeErrorType::kRangeError;
                    frame.pending_throw = make_error_value(err_type, strip_error_prefix(msg));
                    continue;
                }
                stack.push_back(res.value());
                break;
            }
            auto push_res = push_call_frame(fn, std::move(receiver), args);
            if (!push_res.is_ok()) {
                const std::string& msg = push_res.error().message();
                NativeErrorType err_type = NativeErrorType::kRangeError;
                if (msg.rfind("TypeError:", 0) == 0) err_type = NativeErrorType::kTypeError;
                frame.pending_throw = make_error_value(err_type, strip_error_prefix(msg));
                continue;
            }
            break;
        }

        case Opcode::kNewCall: {
            uint8_t argc = read_u8(bc, pc);
            constexpr int kSmallArgBuf = 8;
            Value small_buf[kSmallArgBuf];
            std::vector<Value> large_buf;
            Value* arg_data;
            if (argc <= kSmallArgBuf) {
                for (int i = argc - 1; i >= 0; --i) {
                    small_buf[i] = std::move(stack[stack.size() - argc + i]);
                }
                stack.resize(stack.size() - argc);
                arg_data = small_buf;
            } else {
                large_buf.resize(argc);
                for (int i = argc - 1; i >= 0; --i) {
                    large_buf[i] = std::move(stack[stack.size() - argc + i]);
                }
                stack.resize(stack.size() - argc);
                arg_data = large_buf.data();
            }
            std::span<Value> args(arg_data, argc);
            Value ctor_val = std::move(stack.back());
            stack.pop_back();

            if (!ctor_val.is_object()) {
                frame.pending_throw = make_error_value(NativeErrorType::kTypeError, "not a constructor");
                continue;
            }
            RcObject* ctor_raw = ctor_val.as_object_raw();
            if (!ctor_raw || ctor_raw->object_kind() != ObjectKind::kFunction) {
                frame.pending_throw = make_error_value(NativeErrorType::kTypeError, "not a constructor");
                continue;
            }
            auto fn = RcPtr<JSFunction>(static_cast<JSFunction*>(ctor_raw));
            if (fn->is_native()) {
                auto res = fn->native_fn()(Value::undefined(), std::vector<Value>(args.begin(), args.end()), /*is_new_call=*/true);
                if (!res.is_ok()) {
                    const std::string& msg = res.error().message();
                    NativeErrorType err_type = NativeErrorType::kTypeError;
                    if (msg.rfind("ReferenceError:", 0) == 0) err_type = NativeErrorType::kReferenceError;
                    else if (msg.rfind("RangeError:", 0) == 0) err_type = NativeErrorType::kRangeError;
                    frame.pending_throw = make_error_value(err_type, strip_error_prefix(msg));
                    continue;
                }
                stack.push_back(res.value());
                break;
            }

            // Create instance
            auto instance = RcPtr<JSObject>::make();
            gc_heap_.Register(instance.get());
            const auto& proto_obj = fn->prototype_obj();
            if (proto_obj) {
                instance->set_proto(proto_obj);
            } else {
                instance->set_proto(object_prototype_);
            }
            Value instance_val = Value::object(ObjectPtr(instance));
            Value instance_copy = instance_val;  // keep a copy for do_new logic

            auto push_res = push_call_frame(fn, instance_val, args,
                                            /*is_new=*/true, std::move(instance_copy));
            if (!push_res.is_ok()) {
                const std::string& msg = push_res.error().message();
                NativeErrorType err_type = NativeErrorType::kRangeError;
                if (msg.rfind("TypeError:", 0) == 0) err_type = NativeErrorType::kTypeError;
                frame.pending_throw = make_error_value(err_type, strip_error_prefix(msg));
                continue;
            }
            break;
        }

        case Opcode::kReturn: {
            Value ret = std::move(stack.back());
            stack.pop_back();
            bool is_new = frame.is_new_call;
            Value instance = is_new ? std::move(frame.new_instance) : Value::undefined();
            call_stack_.pop_back();
            call_depth_--;
            if (call_stack_.size() > exit_depth) {
                if (is_new) {
                    // NewCall: if constructor returned an object, use it; otherwise use instance
                    Value result = (ret.is_object() && !ret.is_null()) ? std::move(ret) : std::move(instance);
                    call_stack_.back().stack.push_back(std::move(result));
                } else {
                    call_stack_.back().stack.push_back(std::move(ret));
                }
                continue;
            }
            if (is_new) {
                Value result = (ret.is_object() && !ret.is_null()) ? std::move(ret) : std::move(instance);
                return EvalResult::ok(result);
            }
            return EvalResult::ok(ret);
        }

        case Opcode::kReturnUndefined: {
            bool is_new = frame.is_new_call;
            Value instance = is_new ? std::move(frame.new_instance) : Value::undefined();
            call_stack_.pop_back();
            call_depth_--;
            if (call_stack_.size() > exit_depth) {
                call_stack_.back().stack.push_back(is_new ? std::move(instance) : Value::undefined());
                continue;
            }
            if (is_new) return EvalResult::ok(instance);
            return EvalResult::ok(Value::undefined());
        }

        // ---- Arithmetic ----

        case Opcode::kAdd: {
            Value rv = std::move(stack.back()); stack.pop_back();
            Value lv = std::move(stack.back()); stack.pop_back();
            if (lv.is_string() || rv.is_string()) {
                stack.push_back(Value::string(to_string_val(lv) + to_string_val(rv)));
            } else {
                auto ln = to_number(lv); if (!ln.is_ok()) return ln;
                auto rn = to_number(rv); if (!rn.is_ok()) return rn;
                stack.push_back(Value::number(ln.value().as_number() + rn.value().as_number()));
            }
            break;
        }

        case Opcode::kSub: {
            Value rv = std::move(stack.back()); stack.pop_back();
            Value lv = std::move(stack.back()); stack.pop_back();
            auto ln = to_number(lv); if (!ln.is_ok()) return ln;
            auto rn = to_number(rv); if (!rn.is_ok()) return rn;
            stack.push_back(Value::number(ln.value().as_number() - rn.value().as_number()));
            break;
        }

        case Opcode::kMul: {
            Value rv = std::move(stack.back()); stack.pop_back();
            Value lv = std::move(stack.back()); stack.pop_back();
            auto ln = to_number(lv); if (!ln.is_ok()) return ln;
            auto rn = to_number(rv); if (!rn.is_ok()) return rn;
            stack.push_back(Value::number(ln.value().as_number() * rn.value().as_number()));
            break;
        }

        case Opcode::kDiv: {
            Value rv = std::move(stack.back()); stack.pop_back();
            Value lv = std::move(stack.back()); stack.pop_back();
            auto ln = to_number(lv); if (!ln.is_ok()) return ln;
            auto rn = to_number(rv); if (!rn.is_ok()) return rn;
            stack.push_back(Value::number(ln.value().as_number() / rn.value().as_number()));
            break;
        }

        case Opcode::kMod: {
            Value rv = std::move(stack.back()); stack.pop_back();
            Value lv = std::move(stack.back()); stack.pop_back();
            auto ln = to_number(lv); if (!ln.is_ok()) return ln;
            auto rn = to_number(rv); if (!rn.is_ok()) return rn;
            stack.push_back(Value::number(std::fmod(ln.value().as_number(), rn.value().as_number())));
            break;
        }

        // ---- Unary ----

        case Opcode::kNeg: {
            Value v = std::move(stack.back()); stack.pop_back();
            auto n = to_number(v); if (!n.is_ok()) return n;
            stack.push_back(Value::number(-n.value().as_number()));
            break;
        }

        case Opcode::kPos: {
            Value v = std::move(stack.back()); stack.pop_back();
            auto n = to_number(v); if (!n.is_ok()) return n;
            stack.push_back(n.value());
            break;
        }

        case Opcode::kBitNot: {
            Value v = std::move(stack.back()); stack.pop_back();
            auto n = to_number(v); if (!n.is_ok()) return n;
            int32_t i = static_cast<int32_t>(n.value().as_number());
            stack.push_back(Value::number(static_cast<double>(~i)));
            break;
        }

        case Opcode::kNot: {
            Value v = std::move(stack.back()); stack.pop_back();
            stack.push_back(Value::boolean(!to_boolean(v)));
            break;
        }

        // ---- Comparison ----

        case Opcode::kLt: {
            Value rv = std::move(stack.back()); stack.pop_back();
            Value lv = std::move(stack.back()); stack.pop_back();
            if (lv.is_string() && rv.is_string()) {
                stack.push_back(Value::boolean(lv.as_string() < rv.as_string()));
            } else {
                auto ln = to_number(lv); if (!ln.is_ok()) return ln;
                auto rn = to_number(rv); if (!rn.is_ok()) return rn;
                double a = ln.value().as_number(), b = rn.value().as_number();
                stack.push_back(Value::boolean(!std::isnan(a) && !std::isnan(b) && a < b));
            }
            break;
        }

        case Opcode::kLtEq: {
            Value rv = std::move(stack.back()); stack.pop_back();
            Value lv = std::move(stack.back()); stack.pop_back();
            if (lv.is_string() && rv.is_string()) {
                stack.push_back(Value::boolean(lv.as_string() <= rv.as_string()));
            } else {
                auto ln = to_number(lv); if (!ln.is_ok()) return ln;
                auto rn = to_number(rv); if (!rn.is_ok()) return rn;
                double a = ln.value().as_number(), b = rn.value().as_number();
                stack.push_back(Value::boolean(!std::isnan(a) && !std::isnan(b) && a <= b));
            }
            break;
        }

        case Opcode::kGt: {
            Value rv = std::move(stack.back()); stack.pop_back();
            Value lv = std::move(stack.back()); stack.pop_back();
            if (lv.is_string() && rv.is_string()) {
                stack.push_back(Value::boolean(lv.as_string() > rv.as_string()));
            } else {
                auto ln = to_number(lv); if (!ln.is_ok()) return ln;
                auto rn = to_number(rv); if (!rn.is_ok()) return rn;
                double a = ln.value().as_number(), b = rn.value().as_number();
                stack.push_back(Value::boolean(!std::isnan(a) && !std::isnan(b) && a > b));
            }
            break;
        }

        case Opcode::kGtEq: {
            Value rv = std::move(stack.back()); stack.pop_back();
            Value lv = std::move(stack.back()); stack.pop_back();
            if (lv.is_string() && rv.is_string()) {
                stack.push_back(Value::boolean(lv.as_string() >= rv.as_string()));
            } else {
                auto ln = to_number(lv); if (!ln.is_ok()) return ln;
                auto rn = to_number(rv); if (!rn.is_ok()) return rn;
                double a = ln.value().as_number(), b = rn.value().as_number();
                stack.push_back(Value::boolean(!std::isnan(a) && !std::isnan(b) && a >= b));
            }
            break;
        }

        case Opcode::kEq: {
            Value rv = std::move(stack.back()); stack.pop_back();
            Value lv = std::move(stack.back()); stack.pop_back();
            stack.push_back(Value::boolean(abstract_eq(lv, rv)));
            break;
        }

        case Opcode::kNEq: {
            Value rv = std::move(stack.back()); stack.pop_back();
            Value lv = std::move(stack.back()); stack.pop_back();
            stack.push_back(Value::boolean(!abstract_eq(lv, rv)));
            break;
        }

        case Opcode::kStrictEq: {
            Value rv = std::move(stack.back()); stack.pop_back();
            Value lv = std::move(stack.back()); stack.pop_back();
            stack.push_back(Value::boolean(strict_eq(lv, rv)));
            break;
        }

        case Opcode::kStrictNEq: {
            Value rv = std::move(stack.back()); stack.pop_back();
            Value lv = std::move(stack.back()); stack.pop_back();
            stack.push_back(Value::boolean(!strict_eq(lv, rv)));
            break;
        }

        // ---- Type ----

        case Opcode::kTypeof: {
            Value v = std::move(stack.back()); stack.pop_back();
            std::string type_str;
            switch (v.kind()) {
            case ValueKind::Undefined: type_str = "undefined"; break;
            case ValueKind::Null:      type_str = "object";    break;
            case ValueKind::Bool:      type_str = "boolean";   break;
            case ValueKind::Number:    type_str = "number";    break;
            case ValueKind::String:    type_str = "string";    break;
            case ValueKind::Object: {
                RcObject* obj = v.as_object_raw();
                type_str = (obj && obj->object_kind() == ObjectKind::kFunction) ? "function" : "object";
                break;
            }
            }
            stack.push_back(Value::string(type_str));
            break;
        }

        case Opcode::kTypeofVar: {
            uint16_t idx = read_u16(bc, pc);
            const std::string& name = bc->names[idx];
            Binding* b = env->lookup(name);
            if (b == nullptr) {
                stack.push_back(Value::string("undefined"));
                break;
            }
            if (!b->initialized) {
                frame.pending_throw = make_error_value(NativeErrorType::kReferenceError,
                    "Cannot access '" + name + "' before initialization");
                continue;
            }
            const Value& v = b->cell->value;
            std::string type_str;
            switch (v.kind()) {
            case ValueKind::Undefined: type_str = "undefined"; break;
            case ValueKind::Null:      type_str = "object";    break;
            case ValueKind::Bool:      type_str = "boolean";   break;
            case ValueKind::Number:    type_str = "number";    break;
            case ValueKind::String:    type_str = "string";    break;
            case ValueKind::Object: {
                RcObject* obj = v.as_object_raw();
                type_str = (obj && obj->object_kind() == ObjectKind::kFunction) ? "function" : "object";
                break;
            }
            }
            stack.push_back(Value::string(type_str));
            break;
        }

        case Opcode::kInstanceof: {
            Value ctor_val = std::move(stack.back());
            stack.pop_back();
            Value obj_val = std::move(stack.back());
            stack.pop_back();

            // Non-object left side → false
            if (!obj_val.is_object()) {
                stack.push_back(Value::boolean(false));
                break;
            }
            // Right side must be a Function
            if (!ctor_val.is_object()) {
                frame.pending_throw = make_error_value(NativeErrorType::kTypeError,
                    "Right-hand side of instanceof is not callable");
                continue;
            }
            RcObject* ctor_raw = ctor_val.as_object_raw();
            if (!ctor_raw || ctor_raw->object_kind() != ObjectKind::kFunction) {
                frame.pending_throw = make_error_value(NativeErrorType::kTypeError,
                    "Right-hand side of instanceof is not callable");
                continue;
            }
            auto* ctor_fn = static_cast<JSFunction*>(ctor_raw);
            const RcPtr<JSObject>& ctor_proto = ctor_fn->prototype_obj();
            if (!ctor_proto) {
                stack.push_back(Value::boolean(false));
                break;
            }
            // Walk the prototype chain of obj_val
            RcObject* cur_raw = obj_val.as_object_raw();
            bool found = false;
            while (cur_raw && cur_raw->object_kind() == ObjectKind::kOrdinary) {
                auto* cur_obj = static_cast<JSObject*>(cur_raw);
                const RcPtr<JSObject>& proto = cur_obj->proto();
                if (!proto) break;
                if (proto.get() == ctor_proto.get()) {
                    found = true;
                    break;
                }
                cur_raw = proto.get();
            }
            stack.push_back(Value::boolean(found));
            break;
        }

        // ---- Control flow ----

        case Opcode::kJump: {
            int32_t offset = read_i32(bc, pc);
            pc = static_cast<size_t>(static_cast<int64_t>(pc) + offset);
            break;
        }

        case Opcode::kJumpIfFalse: {
            int32_t offset = read_i32(bc, pc);
            Value v = std::move(stack.back()); stack.pop_back();
            if (!to_boolean(v)) {
                pc = static_cast<size_t>(static_cast<int64_t>(pc) + offset);
            }
            break;
        }

        case Opcode::kJumpIfTrue: {
            int32_t offset = read_i32(bc, pc);
            Value v = std::move(stack.back()); stack.pop_back();
            if (to_boolean(v)) {
                pc = static_cast<size_t>(static_cast<int64_t>(pc) + offset);
            }
            break;
        }

        // ---- Stack ----

        case Opcode::kPop:
            stack.pop_back();
            break;

        case Opcode::kDup:
            stack.push_back(stack.back());
            break;

        // ---- Exception control flow ----

        case Opcode::kThrow: {
            Value thrown = std::move(stack.back());
            stack.pop_back();
            frame.pending_throw = std::move(thrown);
            continue;  // re-enter loop top to trigger exception handler
        }

        case Opcode::kEnterTry: {
            int32_t offset = read_i32(bc, pc);
            // catch_target = pc + offset  (pc is already past the operand)
            size_t catch_target = static_cast<size_t>(static_cast<int64_t>(pc) + offset);
            frame.handler_stack.push_back({
                catch_target,
                frame.stack.size(),
                frame.scope_depth
            });
            break;
        }

        case Opcode::kLeaveTry: {
            if (!frame.handler_stack.empty()) {
                frame.handler_stack.pop_back();
            }
            break;
        }

        case Opcode::kGetException: {
            Value exc = frame.caught_exception.value_or(Value::undefined());
            frame.caught_exception = std::nullopt;
            stack.push_back(std::move(exc));
            break;
        }

        case Opcode::kGosub: {
            int32_t offset = read_i32(bc, pc);
            // push return address (= pc after reading the operand)
            frame.finally_return_stack.push_back(pc);
            // jump to finally subroutine
            pc = static_cast<size_t>(static_cast<int64_t>(pc) + offset);
            break;
        }

        case Opcode::kRet: {
            if (!frame.finally_return_stack.empty()) {
                pc = frame.finally_return_stack.back();
                frame.finally_return_stack.pop_back();
            } else {
                // Reached via exception path (finally_handler_label + Gosub + Ret):
                // finally completed normally; restore pending_throw from caught_exception
                // and continue exception propagation.
                frame.pending_throw = std::move(frame.caught_exception);
                frame.caught_exception = std::nullopt;
                continue;  // re-enter loop top → exception_handler will propagate
            }
            break;
        }

        case Opcode::kSetExportDefault: {
            // 从栈顶取值写入当前模块的 "default" Cell（不弹栈，值留在栈上）
            Value& top = stack.back();
            if (frame.current_module) {
                Cell* cell = frame.current_module->find_export("default");
                if (cell) {
                    cell->value = top;
                    cell->initialized = true;
                }
            }
            // 弹出栈顶（export default 不是表达式语句，不留值）
            stack.pop_back();
            break;
        }

        default:
            return EvalResult::err(Error(ErrorKind::Runtime, "Internal: unknown opcode"));
        }
    }

    return EvalResult::ok(Value::undefined());
}

// ============================================================
// ESM 模块执行
// ============================================================

EvalResult VM::exec_module(const std::string& entry_path) {
    global_env_ = RcPtr<Environment>::make(RcPtr<Environment>());
    init_global_env();

    std::string abs_path = std::filesystem::weakly_canonical(entry_path).string();
    std::string base_dir = std::filesystem::path(abs_path).parent_path().string();

    // Load 入口模块
    auto load_result = module_loader_.Load(abs_path, base_dir);
    if (!load_result.ok()) {
        return EvalResult::err(load_result.error());
    }
    auto entry_mod = load_result.value();

    // Link 阶段
    auto link_result = link_module(*entry_mod);
    if (!link_result.is_ok()) {
        return link_result;
    }

    // Evaluate 阶段
    auto eval_result = evaluate_module(*entry_mod);

    // GC
    {
        std::vector<RcObject*> roots;
        auto add_obj = [&](RcObject* p) { if (p) roots.push_back(p); };
        auto add_val = [&](const Value& v) { if (v.is_object()) add_obj(v.as_object_raw()); };

        add_obj(global_env_.get());
        add_obj(object_prototype_.get());
        add_obj(array_prototype_.get());
        add_obj(function_prototype_.get());
        add_obj(object_constructor_.get());
        for (auto& ep : error_protos_) add_obj(ep.get());
        for (auto& cf : call_stack_) {
            add_obj(cf.env.get());
            add_val(cf.this_val);
            add_val(cf.new_instance);
            for (const auto& v : cf.stack) add_val(v);
            if (cf.pending_throw.has_value()) add_val(*cf.pending_throw);
            if (cf.caught_exception.has_value()) add_val(*cf.caught_exception);
        }
        if (eval_result.is_ok()) add_val(eval_result.value());
        module_loader_.TraceRoots(gc_heap_);

        gc_heap_.Collect(roots);
    }

    global_env_->clear_function_bindings();
    object_prototype_->clear_function_properties();
    if (array_prototype_) array_prototype_->clear_function_properties();
    if (function_prototype_) function_prototype_->clear_function_properties();
    if (object_constructor_) object_constructor_->clear_own_properties();
    // 清理所有模块环境中的函数引用（打破 module_env ↔ JSFunction 循环引用）
    module_loader_.ClearModuleEnvs();
    module_loader_.Clear();

    return eval_result;
}

EvalResult VM::link_module(ModuleRecord& mod) {
    if (mod.status == ModuleStatus::kLinked ||
        mod.status == ModuleStatus::kEvaluated ||
        mod.status == ModuleStatus::kEvaluating) {
        return EvalResult::ok(Value::undefined());
    }
    if (mod.status == ModuleStatus::kLinking) {
        return EvalResult::ok(Value::undefined());  // 循环依赖，正常
    }
    mod.status = ModuleStatus::kLinking;

    std::string base_dir = std::filesystem::path(mod.specifier).parent_path().string();

    // 加载并 Link 所有依赖
    for (const auto& dep_specifier : mod.requested_modules) {
        auto load_result = module_loader_.Load(dep_specifier, base_dir);
        if (!load_result.ok()) {
            return EvalResult::err(load_result.error());
        }
        auto dep = load_result.value();
        mod.dependencies.push_back(dep);
        auto link_result = link_module(*dep);
        if (!link_result.is_ok()) return link_result;
    }

    // 创建模块环境（outer = global_env_）
    auto module_env = RcPtr<Environment>::make(global_env_);
    gc_heap_.Register(module_env.get());
    mod.module_env = module_env;

    // 建立导出变量 Binding（共享 Cell）
    for (const auto& stmt : mod.ast.body) {
        if (const auto* exp = std::get_if<ExportNamedDeclaration>(&stmt.v)) {
            if (exp->source.has_value()) continue;  // re-export
            if (exp->declaration) {
                std::string name;
                bool is_mutable = true;
                bool initialized = false;
                if (const auto* vd = std::get_if<VariableDeclaration>(&exp->declaration->v)) {
                    name = vd->name;
                    is_mutable = (vd->kind != VarKind::Const);
                    initialized = (vd->kind == VarKind::Var);  // var 无 TDZ
                } else if (const auto* fd = std::get_if<FunctionDeclaration>(&exp->declaration->v)) {
                    name = fd->name;
                    is_mutable = true;
                    initialized = true;  // function 声明提升，无 TDZ
                }
                if (!name.empty()) {
                    Cell* cell = mod.find_export(name);
                    if (cell) {
                        cell->initialized = initialized;
                        module_env->define_binding_with_cell(name, RcPtr<Cell>(cell), is_mutable, initialized);
                    }
                }
            } else {
                // export { x as y }（本地 specifiers，无 source）：live binding
                // Load 阶段已为 export_name 分配 Cell
                // Link 阶段将该 Cell 以 local_name 为 key 注入 module_env，实现共享
                for (const auto& spec : exp->specifiers) {
                    Cell* cell = mod.find_export(spec.export_name);
                    if (cell) {
                        module_env->define_binding_with_cell(spec.local_name, RcPtr<Cell>(cell), true, false);
                    }
                }
            }
        }
    }

    // 建立 import Binding
    for (const auto& stmt : mod.ast.body) {
        if (const auto* imp = std::get_if<ImportDeclaration>(&stmt.v)) {
            RcPtr<ModuleRecord> dep_mod;
            for (const auto& dep : mod.dependencies) {
                std::string resolved = std::filesystem::weakly_canonical(
                    std::filesystem::path(base_dir) / imp->specifier).string();
                if (dep->specifier == resolved) {
                    dep_mod = dep;
                    break;
                }
            }
            if (!dep_mod) {
                return EvalResult::err(Error{ErrorKind::Runtime,
                    "Error: Cannot find dependency for '" + imp->specifier + "'"});
            }

            for (const auto& spec : imp->specifiers) {
                if (spec.is_namespace) continue;
                const std::string& imported_name = spec.imported_name;
                const std::string& local_name = spec.local_name;

                Cell* cell = dep_mod->find_export(imported_name);
                if (cell == nullptr) {
                    // 尝试 re-export 解析
                    for (const auto& re : dep_mod->re_exports) {
                        if (re.export_name == imported_name) {
                            std::string re_base = std::filesystem::path(dep_mod->specifier).parent_path().string();
                            std::string re_resolved = std::filesystem::weakly_canonical(
                                std::filesystem::path(re_base) / re.source_specifier).string();
                            for (const auto& re_dep : dep_mod->dependencies) {
                                if (re_dep->specifier == re_resolved) {
                                    cell = re_dep->find_export(re.import_name);
                                    break;
                                }
                            }
                            break;
                        }
                    }
                }
                if (cell == nullptr) {
                    return EvalResult::err(Error{ErrorKind::Syntax,
                        "SyntaxError: The requested module '" + imp->specifier +
                        "' does not provide an export named '" + imported_name + "'"});
                }
                module_env->define_import_binding(local_name, RcPtr<Cell>(cell));
            }
        }
    }

    mod.status = ModuleStatus::kLinked;
    return EvalResult::ok(Value::undefined());
}

EvalResult VM::evaluate_module(ModuleRecord& mod) {
    if (mod.status == ModuleStatus::kEvaluated) {
        return EvalResult::ok(Value::undefined());
    }
    if (mod.status == ModuleStatus::kErrored) {
        if (mod.eval_exception.has_value()) {
            // 通过 pending_throw 机制传递错误
            if (!call_stack_.empty()) {
                call_stack_.back().pending_throw = mod.eval_exception;
            }
            return EvalResult::err(Error{ErrorKind::Runtime, "Error: module evaluation failed (cached)"});
        }
        return EvalResult::err(Error{ErrorKind::Runtime, "Error: module evaluation failed"});
    }
    if (mod.status == ModuleStatus::kEvaluating) {
        return EvalResult::ok(Value::undefined());  // 循环依赖
    }

    mod.status = ModuleStatus::kEvaluating;

    // 先求值所有依赖
    for (auto& dep : mod.dependencies) {
        auto dep_result = evaluate_module(*dep);
        if (!dep_result.is_ok()) {
            mod.status = ModuleStatus::kErrored;
            return dep_result;
        }
    }

    // 执行模块体
    auto body_result = exec_module_body(mod);
    if (!body_result.is_ok()) {
        mod.status = ModuleStatus::kErrored;
        return body_result;
    }

    mod.status = ModuleStatus::kEvaluated;
    return body_result;
}

EvalResult VM::exec_module_body(ModuleRecord& mod) {
    // 编译模块 AST
    Compiler compiler;
    auto bytecode = compiler.compile(mod.ast);

    // 预定义 var_decls（只处理非导出的 var）
    for (uint16_t idx : bytecode->var_decls) {
        const std::string& name = bytecode->names[idx];
        Binding* b = mod.module_env->find_local(name);
        if (b == nullptr) {
            mod.module_env->define_initialized(name);
        } else if (!b->initialized && !b->cell->initialized) {
            // Link 阶段建立的 Binding 是 TDZ，但 var 无 TDZ，需要标记为已初始化
            b->initialized = true;
            b->cell->initialized = true;
        }
    }
    for (uint16_t idx : bytecode->function_decls) {
        const std::string& name = bytecode->names[idx];
        Binding* b = mod.module_env->find_local(name);
        if (b == nullptr) {
            mod.module_env->define_function(name);
        } else if (!b->cell->initialized) {
            // Link 阶段为 export { fn } 建立的 Binding，函数声明提升后无 TDZ
            b->initialized = true;
            b->cell->initialized = true;
        }
    }
    // export default function foo() {}：为 foo 建立模块作用域 Binding
    for (const auto& stmt : mod.ast.body) {
        if (const auto* def = std::get_if<ExportDefaultDeclaration>(&stmt.v)) {
            if (def->local_name.has_value()) {
                const std::string& name = *def->local_name;
                if (mod.module_env->find_local(name) == nullptr) {
                    mod.module_env->define_function(name);
                }
            }
        }
    }

    // 创建 CallFrame
    CallFrame frame;
    frame.bytecode = bytecode.get();
    frame.pc = 0;
    frame.env = mod.module_env;
    frame.this_val = Value::undefined();
    frame.current_module = &mod;

    call_stack_.push_back(std::move(frame));
    size_t exit_depth = call_stack_.size() - 1;

    EvalResult result = run(exit_depth);

    return result;
}

}  // namespace qppjs
