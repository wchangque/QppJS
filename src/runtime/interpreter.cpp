#include "qppjs/runtime/interpreter.h"

#include "qppjs/base/error.h"
#include "qppjs/frontend/ast.h"
#include "qppjs/runtime/completion.h"
#include "qppjs/runtime/environment.h"
#include "qppjs/runtime/js_function.h"
#include "qppjs/runtime/js_object.h"
#include "qppjs/runtime/native_errors.h"
#include "qppjs/runtime/value.h"

#include <optional>

#include <cassert>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>

namespace qppjs {

static std::string strip_error_prefix(const std::string& msg) {
    auto pos = msg.find(": ");
    if (pos != std::string::npos) return msg.substr(pos + 2);
    return msg;
}

// ============================================================
// Completion
// ============================================================

Completion Completion::normal(Value v) {
    return Completion{CompletionType::kNormal, std::move(v), std::nullopt};
}

Completion Completion::return_(Value v) {
    return Completion{CompletionType::kReturn, std::move(v), std::nullopt};
}

Completion Completion::throw_(Value v) {
    return Completion{CompletionType::kThrow, std::move(v), std::nullopt};
}

Completion Completion::break_(std::optional<std::string> label) {
    return Completion{CompletionType::kBreak, Value::undefined(), std::move(label)};
}

Completion Completion::continue_(std::optional<std::string> label) {
    return Completion{CompletionType::kContinue, Value::undefined(), std::move(label)};
}

bool Completion::is_normal() const { return type == CompletionType::kNormal; }

bool Completion::is_return() const { return type == CompletionType::kReturn; }

bool Completion::is_throw() const { return type == CompletionType::kThrow; }

bool Completion::is_break() const { return type == CompletionType::kBreak; }

bool Completion::is_continue() const { return type == CompletionType::kContinue; }

bool Completion::is_abrupt() const { return type != CompletionType::kNormal; }

// ============================================================
// EvalResult
// ============================================================

EvalResult EvalResult::ok(Value v) { return EvalResult{std::variant<Value, Error>(std::move(v))}; }

EvalResult EvalResult::err(Error e) { return EvalResult{std::variant<Value, Error>(std::move(e))}; }

bool EvalResult::is_ok() const { return std::holds_alternative<Value>(data); }

Value& EvalResult::value() { return std::get<Value>(data); }

const Value& EvalResult::value() const { return std::get<Value>(data); }

Error& EvalResult::error() { return std::get<Error>(data); }

const Error& EvalResult::error() const { return std::get<Error>(data); }

// ============================================================
// StmtResult
// ============================================================

StmtResult StmtResult::ok(Completion c) { return StmtResult{std::variant<Completion, Error>(std::move(c))}; }

StmtResult StmtResult::err(Error e) { return StmtResult{std::variant<Completion, Error>(std::move(e))}; }

bool StmtResult::is_ok() const { return std::holds_alternative<Completion>(data); }

Completion& StmtResult::completion() { return std::get<Completion>(data); }

const Completion& StmtResult::completion() const { return std::get<Completion>(data); }

Error& StmtResult::error() { return std::get<Error>(data); }

const Error& StmtResult::error() const { return std::get<Error>(data); }

// ============================================================
// ScopeGuard
// ============================================================

Interpreter::ScopeGuard::ScopeGuard(Interpreter& i, RcPtr<Environment> new_env,
                                     RcPtr<Environment> new_var_env, Value new_this,
                                     bool is_call)
    : interp(i), saved_env(i.current_env_), saved_var_env(i.var_env_),
      saved_this(i.current_this_), owns_call_depth(is_call) {
    interp.current_env_ = std::move(new_env);
    interp.var_env_ = std::move(new_var_env);
    interp.current_this_ = std::move(new_this);
    if (owns_call_depth) {
        ++interp.call_depth_;
    }
}

Interpreter::ScopeGuard::~ScopeGuard() {
    interp.current_env_ = std::move(saved_env);
    interp.var_env_ = std::move(saved_var_env);
    interp.current_this_ = std::move(saved_this);
    if (owns_call_depth) {
        --interp.call_depth_;
    }
}

// ============================================================
// Interpreter
// ============================================================

Value Interpreter::make_error_value(NativeErrorType type, const std::string& message) {
    const auto& proto = error_protos_[static_cast<size_t>(type)];
    return MakeNativeErrorValue(proto, message);
}

void Interpreter::init_runtime() {
    global_env_ = RcPtr<Environment>::make(RcPtr<Environment>());
    current_env_ = global_env_;
    var_env_ = global_env_;
    current_this_ = Value::undefined();
    object_prototype_ = RcPtr<JSObject>::make();
    pending_throw_ = std::nullopt;
    call_depth_ = 0;

    // object_prototype_.proto_ stays nullptr (end of chain)

    // Build Error.prototype
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
        std::string msg = args.empty() ? "" : to_string_val(args[0]);
        return EvalResult::ok(make_error_value(NativeErrorType::kError, msg));
    });
    global_env_->define_initialized("Error");
    global_env_->set("Error", Value::object(ObjectPtr(error_fn)));

    // Build Error sub-classes
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
        sub_proto->set_proto(error_proto);
        sub_proto->set_property("name", Value::string(spec.name));
        sub_proto->set_property("message", Value::string(""));
        error_protos_[static_cast<size_t>(spec.type)] = sub_proto;

        auto sub_fn = RcPtr<JSFunction>::make();
        sub_fn->set_name(std::string(spec.name));
        sub_fn->set_prototype_obj(sub_proto);
        sub_proto->set_constructor_property(sub_fn.get());
        NativeErrorType captured_type = spec.type;
        sub_fn->set_native_fn([this, captured_type](Value /*this_val*/, std::vector<Value> args, bool /*is_new_call*/) -> EvalResult {
            std::string msg = args.empty() ? "" : to_string_val(args[0]);
            return EvalResult::ok(make_error_value(captured_type, msg));
        });
        global_env_->define_initialized(spec.name);
        global_env_->set(spec.name, Value::object(ObjectPtr(sub_fn)));
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
            result += Interpreter::to_string_val(args[i]);
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

    // Array.prototype.forEach
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
            auto res = call_function_val(callback, Value::undefined(), {call_args, 3});
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
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                              "Object.keys called on non-object");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
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
    assign_fn->set_native_fn([this](Value /*this_val*/, std::vector<Value> args, bool) -> EvalResult {
        if (args.empty() || !args[0].is_object()) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                              "Object.assign called on non-object");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
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
                    if (!res.is_ok()) {
                        const std::string& msg = res.error().message();
                        NativeErrorType err_type = NativeErrorType::kRangeError;
                        if (msg.rfind("TypeError:", 0) == 0) err_type = NativeErrorType::kTypeError;
                        pending_throw_ = make_error_value(err_type, strip_error_prefix(msg));
                        return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
                    }
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
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                              "Object.create requires an argument");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        const Value& proto_arg = args[0];
        if (!proto_arg.is_null() && !proto_arg.is_object()) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                              "Object prototype may only be an Object or null");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        auto new_obj = RcPtr<JSObject>::make();
        gc_heap_.Register(new_obj.get());
        if (!proto_arg.is_null()) {
            RcObject* proto_raw = proto_arg.as_object_raw();
            ObjectKind kind = proto_raw->object_kind();
            if (kind == ObjectKind::kFunction) {
                pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                                  "Object prototype may only be an Object or null");
                return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
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
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                              "Function.prototype.call called on non-function");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
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
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                              "Function.prototype.apply called on non-function");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        Value new_this = args.empty() ? Value::undefined() : args[0];
        Value args_array = args.size() > 1 ? args[1] : Value::undefined();
        if (args_array.is_null() || args_array.is_undefined()) {
            std::span<Value> empty_span;
            return call_function_val(this_val, std::move(new_this), empty_span);
        }
        if (!args_array.is_object()) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                              "apply argument must be an array or array-like object");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
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
                    pending_throw_ = make_error_value(NativeErrorType::kRangeError,
                                                      "apply argsArray length exceeds limit");
                    return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
                }
                len = static_cast<uint32_t>(len_num);
            }
            call_args.reserve(len);
            for (uint32_t i = 0; i < len; ++i) {
                call_args.push_back(obj->get_property(std::to_string(i)));
            }
        } else {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                              "apply argument must be an array or array-like object");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
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
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                              "Function.prototype.bind called on non-function");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        Value bound_this = args.empty() ? Value::undefined() : args[0];
        std::vector<Value> bound_args;
        if (args.size() > 1) {
            bound_args.assign(args.begin() + 1, args.end());
        }

        auto* target_raw = static_cast<JSFunction*>(this_val.as_object_raw());
        // Compute bound length
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
                RcPtr<JSObject> proto = target_fn->prototype_obj() ? target_fn->prototype_obj() : object_prototype_;
                auto new_obj = RcPtr<JSObject>::make();
                gc_heap_.Register(new_obj.get());
                new_obj->set_proto(proto);
                Value new_this = Value::object(ObjectPtr(new_obj));
                auto call_result = call_function(target_fn, new_this, std::move(merged), /*is_new_call=*/true);
                if (!call_result.is_ok()) {
                    return EvalResult::err(call_result.error());
                }
                if (call_result.completion().is_throw()) {
                    pending_throw_ = call_result.completion().value;
                    return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
                }
                const Completion& c = call_result.completion();
                if (c.is_return() && c.value.is_object() && c.value.as_object_raw() != nullptr) {
                    return EvalResult::ok(c.value);
                }
                return EvalResult::ok(new_this);
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

    // Register the global environment with GcHeap so user-created closures reachable
    // from it are treated as roots and not swept.
    gc_heap_.Register(global_env_.get());
}

Interpreter::Interpreter() {
    init_runtime();
}

// ---- Type conversions ----

bool Interpreter::to_boolean(const Value& v) {
    switch (v.kind()) {
    case ValueKind::Undefined:
        return false;
    case ValueKind::Null:
        return false;
    case ValueKind::Bool:
        return v.as_bool();
    case ValueKind::Number: {
        double n = v.as_number();
        return n != 0.0 && !std::isnan(n);
    }
    case ValueKind::String:
        return !v.as_string().empty();
    case ValueKind::Object:
        return true;
    }
    return false;
}

EvalResult Interpreter::to_number(const Value& v) {
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
        // Use strtod to avoid exceptions
        char* end = nullptr;
        double result = std::strtod(s.c_str(), &end);
        // If end didn't advance to end-of-string, it's NaN
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

std::string Interpreter::to_string_val(const Value& v) {
    switch (v.kind()) {
    case ValueKind::Undefined:
        return "undefined";
    case ValueKind::Null:
        return "null";
    case ValueKind::Bool:
        return v.as_bool() ? "true" : "false";
    case ValueKind::Number: {
        double n = v.as_number();
        if (std::isnan(n)) {
            return "NaN";
        }
        if (std::isinf(n)) {
            return n > 0 ? "Infinity" : "-Infinity";
        }
        // Show integer values without decimal point
        if (n == static_cast<double>(static_cast<long long>(n)) && std::abs(n) < 1e15) {
            std::ostringstream oss;
            oss << static_cast<long long>(n);
            return oss.str();
        }
        std::ostringstream oss;
        oss << n;
        return oss.str();
    }
    case ValueKind::String:
        return v.as_string();
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

// ---- Var hoisting ----

void Interpreter::hoist_vars_stmt(const StmtNode& stmt, Environment& var_target) {
    if (std::holds_alternative<VariableDeclaration>(stmt.v)) {
        const auto& decl = std::get<VariableDeclaration>(stmt.v);
        if (decl.kind == VarKind::Var) {
            var_target.define_initialized(decl.name);
        } else {
            current_env_->define(decl.name, decl.kind);
        }
    } else if (std::holds_alternative<FunctionDeclaration>(stmt.v)) {
        const auto& fdecl = std::get<FunctionDeclaration>(stmt.v);
        var_target.define_function(fdecl.name);
    } else if (std::holds_alternative<ForStatement>(stmt.v)) {
        const auto& for_stmt = std::get<ForStatement>(stmt.v);
        if (for_stmt.init.has_value()) {
            const auto& init_node = *for_stmt.init.value();
            if (std::holds_alternative<VariableDeclaration>(init_node.v)) {
                const auto& decl = std::get<VariableDeclaration>(init_node.v);
                if (decl.kind == VarKind::Var) {
                    var_target.define_initialized(decl.name);
                }
            }
        }
        hoist_vars_stmt(*for_stmt.body, var_target);
    } else if (std::holds_alternative<TryStatement>(stmt.v)) {
        const auto& try_stmt = std::get<TryStatement>(stmt.v);
        hoist_vars(try_stmt.block.body, var_target);
        if (try_stmt.handler.has_value()) {
            hoist_vars(try_stmt.handler->body.body, var_target);
        }
        if (try_stmt.finalizer.has_value()) {
            hoist_vars(try_stmt.finalizer->body, var_target);
        }
    } else if (std::holds_alternative<LabeledStatement>(stmt.v)) {
        const auto& labeled = std::get<LabeledStatement>(stmt.v);
        hoist_vars_stmt(*labeled.body, var_target);
    }
}

void Interpreter::hoist_vars(const std::vector<StmtNode>& stmts, Environment& var_target) {
    for (const auto& stmt : stmts) {
        hoist_vars_stmt(stmt, var_target);
    }
}

// ---- exec ----

EvalResult Interpreter::exec(const Program& program) {
    init_runtime();
    hoist_vars(program.body, *var_env_);

    // Run the program, collecting the final result or error.
    EvalResult final_result = EvalResult::ok(Value::undefined());
    bool has_result = false;

    Value last = Value::undefined();
    for (const auto& stmt : program.body) {
        auto result = eval_stmt(stmt);
        if (!result.is_ok()) {
            // Propagate C++ error; if it's a pending_throw_ sentinel, format as "Name: message"
            const std::string& emsg = result.error().message();
            if (emsg == kPendingThrowSentinel && pending_throw_.has_value()) {
                Value thrown = std::move(*pending_throw_);
                pending_throw_ = std::nullopt;
                std::string name = "Error";
                std::string message;
                if (thrown.is_object()) {
                    RcObject* raw = thrown.as_object_raw();
                    if (raw && raw->object_kind() == ObjectKind::kOrdinary) {
                        auto* obj = static_cast<JSObject*>(raw);
                        Value n = obj->get_property("name");
                        Value m = obj->get_property("message");
                        if (n.is_string()) name = n.as_string();
                        if (m.is_string()) message = m.as_string();
                    }
                }
                final_result = EvalResult::err(Error(ErrorKind::Runtime, name + ": " + message));
            } else {
                final_result = EvalResult::err(result.error());
            }
            has_result = true;
            break;
        }
        const Completion& c = result.completion();
        if (c.is_return()) {
            final_result = EvalResult::ok(c.value);
            has_result = true;
            break;
        }
        if (c.is_throw()) {
            // Uncaught throw at top level → propagate as error
            const Value& thrown = c.value;
            if (thrown.is_object()) {
                RcObject* raw = thrown.as_object_raw();
                if (raw && raw->object_kind() == ObjectKind::kOrdinary) {
                    auto* obj = static_cast<JSObject*>(raw);
                    Value n = obj->get_property("name");
                    Value m = obj->get_property("message");
                    std::string name = n.is_string() ? n.as_string() : "Error";
                    std::string message = m.is_string() ? m.as_string() : "";
                    final_result = EvalResult::err(Error(ErrorKind::Runtime, name + ": " + message));
                    has_result = true;
                    break;
                }
            }
            final_result = EvalResult::err(Error(ErrorKind::Runtime, to_string_val(thrown)));
            has_result = true;
            break;
        }
        if (c.is_normal()) {
            last = c.value;
        }
    }
    if (!has_result) {
        final_result = EvalResult::ok(last);
    }

    // GC: collect unreachable objects (resolves P3-2 closure circular references).
    // Run GC first (before clear_function_bindings) so that all reachable objects
    // are correctly identified. Roots include all interpreter members and the result value.
    {
        std::vector<RcObject*> roots;
        auto add_obj = [&](RcObject* p) { if (p) roots.push_back(p); };
        auto add_val = [&](const Value& v) { if (v.is_object()) add_obj(v.as_object_raw()); };

        add_obj(global_env_.get());
        add_obj(current_env_.get());
        add_obj(var_env_.get());
        add_obj(object_prototype_.get());
        add_obj(array_prototype_.get());
        add_obj(function_prototype_.get());
        add_obj(object_constructor_.get());
        for (auto& ep : error_protos_) add_obj(ep.get());
        add_val(current_this_);
        if (pending_throw_.has_value()) add_val(*pending_throw_);
        // Include the result value so it is not swept
        if (final_result.is_ok()) add_val(final_result.value());

        gc_heap_.Collect(roots);
    }

    // Cleanup: break remaining RC cycles from runtime objects.
    global_env_->clear_function_bindings();
    object_prototype_->clear_function_properties();
    if (array_prototype_) array_prototype_->clear_function_properties();
    if (function_prototype_) function_prototype_->clear_function_properties();
    if (object_constructor_) object_constructor_->clear_own_properties();

    return final_result;
}

// ---- Statement dispatch ----

StmtResult Interpreter::eval_stmt(const StmtNode& stmt) {
    return std::visit(
        overloaded{
            [this](const ExpressionStatement& s) { return eval_expression_stmt(s); },
            [this](const VariableDeclaration& s) { return eval_var_decl(s); },
            [this](const BlockStatement& s) { return eval_block_stmt(s); },
            [this](const IfStatement& s) { return eval_if_stmt(s); },
            [this](const WhileStatement& s) { return eval_while_stmt(s); },
            [this](const ReturnStatement& s) { return eval_return_stmt(s); },
            [this](const FunctionDeclaration& s) { return eval_function_decl(s); },
            [this](const ThrowStatement& s) { return eval_throw_stmt(s); },
            [this](const TryStatement& s) { return eval_try_stmt(s); },
            [this](const BreakStatement& s) { return eval_break_stmt(s); },
            [this](const ContinueStatement& s) { return eval_continue_stmt(s); },
            [this](const LabeledStatement& s) { return eval_labeled_stmt(s); },
            [this](const ForStatement& s) { return eval_for_stmt(s); },
        },
        stmt.v);
}

StmtResult Interpreter::eval_expression_stmt(const ExpressionStatement& stmt) {
    auto result = eval_expr(stmt.expr);
    if (!result.is_ok()) {
        return StmtResult::err(result.error());
    }
    return StmtResult::ok(Completion::normal(result.value()));
}

StmtResult Interpreter::eval_var_decl(const VariableDeclaration& decl) {
    if (decl.kind == VarKind::Var) {
        // var: binding already hoisted; just assign if there is an initializer
        if (decl.init.has_value()) {
            const auto* fn_expr = std::get_if<FunctionExpression>(&decl.init->v);
            EvalResult init_result = fn_expr
                ? EvalResult::ok(make_function_value(
                    fn_expr->name,
                    fn_expr->params,
                    fn_expr->body,
                    current_env_,
                    fn_expr->name.has_value()))
                : eval_expr(decl.init.value());
            if (!init_result.is_ok()) {
                return StmtResult::err(init_result.error());
            }
            auto set_result = current_env_->set(decl.name, init_result.value());
            if (!set_result.is_ok()) {
                return StmtResult::err(set_result.error());
            }
        }
    } else {
        // let / const: create TDZ binding in current scope, then initialize
        current_env_->define(decl.name, decl.kind);
        if (decl.init.has_value()) {
            const auto* fn_expr = std::get_if<FunctionExpression>(&decl.init->v);
            EvalResult init_result = fn_expr
                ? EvalResult::ok(make_function_value(
                    fn_expr->name,
                    fn_expr->params,
                    fn_expr->body,
                    current_env_,
                    fn_expr->name.has_value()))
                : eval_expr(decl.init.value());
            if (!init_result.is_ok()) {
                return StmtResult::err(init_result.error());
            }
            auto init_env_result = current_env_->initialize(decl.name, init_result.value());
            if (!init_env_result.is_ok()) {
                return StmtResult::err(init_env_result.error());
            }
        } else {
            // No initializer: immediately initialize to undefined, TDZ ends (ECMAScript §14.3.1.1 step 3.b.i)
            current_env_->initialize(decl.name, Value::undefined());
        }
    }
    return StmtResult::ok(Completion::normal(Value::undefined()));
}

StmtResult Interpreter::eval_block_stmt(const BlockStatement& stmt) {
    auto block_env = RcPtr<Environment>::make(current_env_);
    gc_heap_.Register(block_env.get());
    ScopeGuard guard(*this, block_env, var_env_, current_this_);

    hoist_vars(stmt.body, *var_env_);

    Value last = Value::undefined();
    for (const auto& s : stmt.body) {
        auto result = eval_stmt(s);
        if (!result.is_ok()) {
            return result;
        }
        const Completion& c = result.completion();
        if (c.is_abrupt()) {
            return result;  // propagate any abrupt completion upward
        }
        last = c.value;
    }
    return StmtResult::ok(Completion::normal(last));
}

StmtResult Interpreter::eval_if_stmt(const IfStatement& stmt) {
    auto test_result = eval_expr(stmt.test);
    if (!test_result.is_ok()) {
        return StmtResult::err(test_result.error());
    }
    bool cond = to_boolean(test_result.value());
    if (cond) {
        return eval_stmt(*stmt.consequent);
    }
    if (stmt.alternate != nullptr) {
        return eval_stmt(*stmt.alternate);
    }
    return StmtResult::ok(Completion::normal(Value::undefined()));
}

StmtResult Interpreter::eval_while_stmt(const WhileStatement& stmt,
                                         std::optional<std::string> label) {
    while (true) {
        auto test_result = eval_expr(stmt.test);
        if (!test_result.is_ok()) {
            return StmtResult::err(test_result.error());
        }
        if (!to_boolean(test_result.value())) {
            break;
        }
        auto body_result = eval_stmt(*stmt.body);
        if (!body_result.is_ok()) {
            return body_result;
        }
        const Completion& c = body_result.completion();
        if (c.is_break()) {
            if (!c.target.has_value() || c.target == label) {
                // Unlabeled break or break targeting this loop's label
                return StmtResult::ok(Completion::normal(Value::undefined()));
            }
            return body_result;  // Labeled break for outer loop, propagate up
        }
        if (c.is_continue()) {
            if (!c.target.has_value() || c.target == label) {
                continue;  // Unlabeled continue or continue targeting this loop's label
            }
            return body_result;  // Labeled continue for outer loop, propagate up
        }
        if (c.is_return() || c.is_throw()) {
            return body_result;
        }
    }
    return StmtResult::ok(Completion::normal(Value::undefined()));
}

StmtResult Interpreter::eval_return_stmt(const ReturnStatement& stmt) {
    if (stmt.argument.has_value()) {
        auto result = eval_expr(stmt.argument.value());
        if (!result.is_ok()) {
            return StmtResult::err(result.error());
        }
        return StmtResult::ok(Completion::return_(result.value()));
    }
    return StmtResult::ok(Completion::return_(Value::undefined()));
}

// ---- Expression dispatch ----

EvalResult Interpreter::eval_expr(const ExprNode& expr) {
    return std::visit(
        overloaded{
            [](const NumberLiteral& e) { return EvalResult::ok(Value::number(e.value)); },
            [](const StringLiteral& e) { return EvalResult::ok(Value::string(e.value)); },
            [](const BooleanLiteral& e) { return EvalResult::ok(Value::boolean(e.value)); },
            [](const NullLiteral&) { return EvalResult::ok(Value::null()); },
            [this](const Identifier& e) { return eval_identifier(e); },
            [this](const UnaryExpression& e) { return eval_unary(e); },
            [this](const BinaryExpression& e) { return eval_binary(e); },
            [this](const LogicalExpression& e) { return eval_logical(e); },
            [this](const AssignmentExpression& e) { return eval_assignment(e); },
            [this](const ObjectExpression& e) { return eval_object_expr(e); },
            [this](const MemberExpression& e) { return eval_member_expr(e); },
            [this](const MemberAssignmentExpression& e) { return eval_member_assign(e); },
            [this](const FunctionExpression& e) { return eval_function_expr(e); },
            [this](const CallExpression& e) { return eval_call_expr(e); },
            [this](const NewExpression& e) { return eval_new_expr(e); },
            [this](const ArrayExpression& e) { return eval_array_expr(e); },
        },
        expr.v);
}

EvalResult Interpreter::eval_array_expr(const ArrayExpression& expr) {
    auto arr = RcPtr<JSObject>::make(ObjectKind::kArray);
    gc_heap_.Register(arr.get());
    arr->set_proto(array_prototype_);
    for (const auto& elem_expr : expr.elements) {
        auto v = eval_expr(*elem_expr);
        if (!v.is_ok()) return v;
        arr->elements_[arr->array_length_] = v.value();
        arr->array_length_++;
    }
    return EvalResult::ok(Value::object(ObjectPtr(arr)));
}

EvalResult Interpreter::eval_identifier(const Identifier& expr) {
    if (expr.name == "undefined") {
        return EvalResult::ok(Value::undefined());
    }
    if (expr.name == "this") {
        return EvalResult::ok(current_this_);
    }
    auto result = current_env_->get(expr.name);
    if (!result.is_ok()) {
        const std::string& msg = result.error().message();
        NativeErrorType err_type = NativeErrorType::kReferenceError;
        if (msg.rfind("TypeError:", 0) == 0) err_type = NativeErrorType::kTypeError;
        pending_throw_ = make_error_value(err_type, strip_error_prefix(msg));
        return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
    }
    return result;
}

EvalResult Interpreter::eval_unary(const UnaryExpression& expr) {
    // typeof special case: must not throw for undeclared identifiers
    if (expr.op == UnaryOp::Typeof) {
        if (std::holds_alternative<Identifier>(expr.operand->v)) {
            const auto& id = std::get<Identifier>(expr.operand->v);
            // "undefined" identifier
            if (id.name == "undefined") {
                return EvalResult::ok(Value::string("undefined"));
            }
            Binding* b = current_env_->lookup(id.name);
            if (b == nullptr) {
                return EvalResult::ok(Value::string("undefined"));
            }
            if (!b->initialized) {
                pending_throw_ = make_error_value(NativeErrorType::kReferenceError,
                    "Cannot access '" + id.name + "' before initialization");
                return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
            }
        }
        // Otherwise fall through to normal evaluation
        auto operand_result = eval_expr(*expr.operand);
        if (!operand_result.is_ok()) {
            return operand_result;
        }
        const Value& val = operand_result.value();
        switch (val.kind()) {
        case ValueKind::Undefined:
            return EvalResult::ok(Value::string("undefined"));
        case ValueKind::Null:
            return EvalResult::ok(Value::string("object"));
        case ValueKind::Bool:
            return EvalResult::ok(Value::string("boolean"));
        case ValueKind::Number:
            return EvalResult::ok(Value::string("number"));
        case ValueKind::String:
            return EvalResult::ok(Value::string("string"));
        case ValueKind::Object: {
            RcObject* obj = val.as_object_raw();
            if (obj && obj->object_kind() == ObjectKind::kFunction) {
                return EvalResult::ok(Value::string("function"));
            }
            return EvalResult::ok(Value::string("object"));
        }
        }
        return EvalResult::ok(Value::string("undefined"));
    }

    if (expr.op == UnaryOp::Void) {
        auto operand_result = eval_expr(*expr.operand);
        if (!operand_result.is_ok()) {
            return operand_result;
        }
        return EvalResult::ok(Value::undefined());
    }

    auto operand_result = eval_expr(*expr.operand);
    if (!operand_result.is_ok()) {
        return operand_result;
    }
    const Value& val = operand_result.value();

    switch (expr.op) {
    case UnaryOp::Minus: {
        auto num_result = to_number(val);
        if (!num_result.is_ok()) {
            return num_result;
        }
        return EvalResult::ok(Value::number(-num_result.value().as_number()));
    }
    case UnaryOp::Plus: {
        return to_number(val);
    }
    case UnaryOp::Bang:
        return EvalResult::ok(Value::boolean(!to_boolean(val)));
    default:
        break;
    }
    return EvalResult::ok(Value::undefined());
}

// Strict equality (===)
static bool strict_eq(const Value& a, const Value& b) {
    if (a.kind() != b.kind()) {
        return false;
    }
    switch (a.kind()) {
    case ValueKind::Undefined:
        return true;
    case ValueKind::Null:
        return true;
    case ValueKind::Bool:
        return a.as_bool() == b.as_bool();
    case ValueKind::Number: {
        double na = a.as_number();
        double nb = b.as_number();
        if (std::isnan(na) || std::isnan(nb)) {
            return false;
        }
        return na == nb;
    }
    case ValueKind::String:
        return a.as_string() == b.as_string();
    case ValueKind::Object:
        return a.as_object_raw() == b.as_object_raw();
    }
    return false;
}

// Abstract equality (==) — only primitive subset
static bool abstract_eq(const Value& a, const Value& b) {
    // Same type: use strict equality rules
    if (a.kind() == b.kind()) {
        return strict_eq(a, b);
    }
    // null == undefined  /  undefined == null
    bool a_nullish = a.is_null() || a.is_undefined();
    bool b_nullish = b.is_null() || b.is_undefined();
    if (a_nullish && b_nullish) {
        return true;
    }
    if (a_nullish || b_nullish) {
        return false;
    }
    // Boolean: convert to number, recurse
    if (a.is_bool()) {
        return abstract_eq(Value::number(a.as_bool() ? 1.0 : 0.0), b);
    }
    if (b.is_bool()) {
        return abstract_eq(a, Value::number(b.as_bool() ? 1.0 : 0.0));
    }
    // String == Number: convert string to number, recurse
    if (a.is_string() && b.is_number()) {
        char* end = nullptr;
        const std::string& s = a.as_string();
        double n = s.empty() ? 0.0 : std::strtod(s.c_str(), &end);
        if (!s.empty() && (end == s.c_str() || *end != '\0')) {
            n = std::numeric_limits<double>::quiet_NaN();
        }
        return abstract_eq(Value::number(n), b);
    }
    if (a.is_number() && b.is_string()) {
        char* end = nullptr;
        const std::string& s = b.as_string();
        double n = s.empty() ? 0.0 : std::strtod(s.c_str(), &end);
        if (!s.empty() && (end == s.c_str() || *end != '\0')) {
            n = std::numeric_limits<double>::quiet_NaN();
        }
        return abstract_eq(a, Value::number(n));
    }
    return false;
}

EvalResult Interpreter::eval_binary(const BinaryExpression& expr) {
    auto left_result = eval_expr(*expr.left);
    if (!left_result.is_ok()) {
        return left_result;
    }
    auto right_result = eval_expr(*expr.right);
    if (!right_result.is_ok()) {
        return right_result;
    }

    const Value& lv = left_result.value();
    const Value& rv = right_result.value();

    switch (expr.op) {
    case BinaryOp::Add: {
        // If either side is String, concatenate
        if (lv.is_string() || rv.is_string()) {
            return EvalResult::ok(Value::string(to_string_val(lv) + to_string_val(rv)));
        }
        auto ln = to_number(lv);
        if (!ln.is_ok()) {
            return ln;
        }
        auto rn = to_number(rv);
        if (!rn.is_ok()) {
            return rn;
        }
        return EvalResult::ok(Value::number(ln.value().as_number() + rn.value().as_number()));
    }
    case BinaryOp::Sub: {
        auto ln = to_number(lv);
        if (!ln.is_ok()) {
            return ln;
        }
        auto rn = to_number(rv);
        if (!rn.is_ok()) {
            return rn;
        }
        return EvalResult::ok(Value::number(ln.value().as_number() - rn.value().as_number()));
    }
    case BinaryOp::Mul: {
        auto ln = to_number(lv);
        if (!ln.is_ok()) {
            return ln;
        }
        auto rn = to_number(rv);
        if (!rn.is_ok()) {
            return rn;
        }
        return EvalResult::ok(Value::number(ln.value().as_number() * rn.value().as_number()));
    }
    case BinaryOp::Div: {
        auto ln = to_number(lv);
        if (!ln.is_ok()) {
            return ln;
        }
        auto rn = to_number(rv);
        if (!rn.is_ok()) {
            return rn;
        }
        return EvalResult::ok(Value::number(ln.value().as_number() / rn.value().as_number()));
    }
    case BinaryOp::Mod: {
        auto ln = to_number(lv);
        if (!ln.is_ok()) {
            return ln;
        }
        auto rn = to_number(rv);
        if (!rn.is_ok()) {
            return rn;
        }
        return EvalResult::ok(Value::number(std::fmod(ln.value().as_number(), rn.value().as_number())));
    }
    case BinaryOp::Lt: {
        // Both strings: lexicographic comparison (ECMAScript §13.11 AbstractRelationalComparison)
        if (lv.is_string() && rv.is_string()) {
            return EvalResult::ok(Value::boolean(lv.as_string() < rv.as_string()));
        }
        auto ln = to_number(lv);
        if (!ln.is_ok()) {
            return ln;
        }
        auto rn = to_number(rv);
        if (!rn.is_ok()) {
            return rn;
        }
        double lnum = ln.value().as_number();
        double rnum = rn.value().as_number();
        if (std::isnan(lnum) || std::isnan(rnum)) {
            return EvalResult::ok(Value::boolean(false));
        }
        return EvalResult::ok(Value::boolean(lnum < rnum));
    }
    case BinaryOp::Gt: {
        if (lv.is_string() && rv.is_string()) {
            return EvalResult::ok(Value::boolean(lv.as_string() > rv.as_string()));
        }
        auto ln = to_number(lv);
        if (!ln.is_ok()) {
            return ln;
        }
        auto rn = to_number(rv);
        if (!rn.is_ok()) {
            return rn;
        }
        double lnum = ln.value().as_number();
        double rnum = rn.value().as_number();
        if (std::isnan(lnum) || std::isnan(rnum)) {
            return EvalResult::ok(Value::boolean(false));
        }
        return EvalResult::ok(Value::boolean(lnum > rnum));
    }
    case BinaryOp::LtEq: {
        if (lv.is_string() && rv.is_string()) {
            return EvalResult::ok(Value::boolean(lv.as_string() <= rv.as_string()));
        }
        auto ln = to_number(lv);
        if (!ln.is_ok()) {
            return ln;
        }
        auto rn = to_number(rv);
        if (!rn.is_ok()) {
            return rn;
        }
        double lnum = ln.value().as_number();
        double rnum = rn.value().as_number();
        if (std::isnan(lnum) || std::isnan(rnum)) {
            return EvalResult::ok(Value::boolean(false));
        }
        return EvalResult::ok(Value::boolean(lnum <= rnum));
    }
    case BinaryOp::GtEq: {
        if (lv.is_string() && rv.is_string()) {
            return EvalResult::ok(Value::boolean(lv.as_string() >= rv.as_string()));
        }
        auto ln = to_number(lv);
        if (!ln.is_ok()) {
            return ln;
        }
        auto rn = to_number(rv);
        if (!rn.is_ok()) {
            return rn;
        }
        double lnum = ln.value().as_number();
        double rnum = rn.value().as_number();
        if (std::isnan(lnum) || std::isnan(rnum)) {
            return EvalResult::ok(Value::boolean(false));
        }
        return EvalResult::ok(Value::boolean(lnum >= rnum));
    }
    case BinaryOp::EqEqEq:
        return EvalResult::ok(Value::boolean(strict_eq(lv, rv)));
    case BinaryOp::NotEqEq:
        return EvalResult::ok(Value::boolean(!strict_eq(lv, rv)));
    case BinaryOp::EqEq:
        return EvalResult::ok(Value::boolean(abstract_eq(lv, rv)));
    case BinaryOp::NotEq:
        return EvalResult::ok(Value::boolean(!abstract_eq(lv, rv)));
    case BinaryOp::Instanceof: {
        // Non-object left side → false
        if (!lv.is_object()) {
            return EvalResult::ok(Value::boolean(false));
        }
        // Right side must be a Function
        if (!rv.is_object()) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "Right-hand side of instanceof is not callable");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        RcObject* ctor_raw = rv.as_object_raw();
        if (!ctor_raw || ctor_raw->object_kind() != ObjectKind::kFunction) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "Right-hand side of instanceof is not callable");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        auto* ctor_fn = static_cast<JSFunction*>(ctor_raw);
        const RcPtr<JSObject>& ctor_proto = ctor_fn->prototype_obj();
        if (!ctor_proto) {
            return EvalResult::ok(Value::boolean(false));
        }
        // Walk the prototype chain of lv
        RcObject* cur_raw = lv.as_object_raw();
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
        return EvalResult::ok(Value::boolean(found));
    }
    }
    return EvalResult::ok(Value::undefined());
}

EvalResult Interpreter::eval_logical(const LogicalExpression& expr) {
    auto left_result = eval_expr(*expr.left);
    if (!left_result.is_ok()) {
        return left_result;
    }
    const Value& lv = left_result.value();

    switch (expr.op) {
    case LogicalOp::And:
        if (!to_boolean(lv)) {
            return left_result;
        }
        return eval_expr(*expr.right);
    case LogicalOp::Or:
        if (to_boolean(lv)) {
            return left_result;
        }
        return eval_expr(*expr.right);
    }
    return EvalResult::ok(Value::undefined());
}

EvalResult Interpreter::eval_assignment(const AssignmentExpression& expr) {
    if (expr.op == AssignOp::Assign) {
        auto rhs = eval_expr(*expr.value);
        if (!rhs.is_ok()) {
            return rhs;
        }
        auto set_result = current_env_->set(expr.target, rhs.value());
        if (!set_result.is_ok()) {
            const std::string& msg = set_result.error().message();
            NativeErrorType err_type = NativeErrorType::kTypeError;
            if (msg.rfind("ReferenceError:", 0) == 0) err_type = NativeErrorType::kReferenceError;
            pending_throw_ = make_error_value(err_type, strip_error_prefix(msg));
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        return rhs;
    }

    // Compound assignment: read current value, compute, write back
    auto current_result = current_env_->get(expr.target);
    if (!current_result.is_ok()) {
        const std::string& msg = current_result.error().message();
        NativeErrorType err_type = NativeErrorType::kReferenceError;
        if (msg.rfind("TypeError:", 0) == 0) err_type = NativeErrorType::kTypeError;
        pending_throw_ = make_error_value(err_type, strip_error_prefix(msg));
        return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
    }
    auto rhs = eval_expr(*expr.value);
    if (!rhs.is_ok()) {
        return rhs;
    }

    Value new_val = Value::undefined();
    switch (expr.op) {
    case AssignOp::AddAssign: {
        const Value& lv = current_result.value();
        const Value& rv = rhs.value();
        if (lv.is_string() || rv.is_string()) {
            new_val = Value::string(to_string_val(lv) + to_string_val(rv));
        } else {
            auto ln = to_number(lv);
            auto rn = to_number(rv);
            if (!ln.is_ok()) {
                return ln;
            }
            if (!rn.is_ok()) {
                return rn;
            }
            new_val = Value::number(ln.value().as_number() + rn.value().as_number());
        }
        break;
    }
    case AssignOp::SubAssign: {
        auto ln = to_number(current_result.value());
        auto rn = to_number(rhs.value());
        if (!ln.is_ok()) {
            return ln;
        }
        if (!rn.is_ok()) {
            return rn;
        }
        new_val = Value::number(ln.value().as_number() - rn.value().as_number());
        break;
    }
    case AssignOp::MulAssign: {
        auto ln = to_number(current_result.value());
        auto rn = to_number(rhs.value());
        if (!ln.is_ok()) {
            return ln;
        }
        if (!rn.is_ok()) {
            return rn;
        }
        new_val = Value::number(ln.value().as_number() * rn.value().as_number());
        break;
    }
    case AssignOp::DivAssign: {
        auto ln = to_number(current_result.value());
        auto rn = to_number(rhs.value());
        if (!ln.is_ok()) {
            return ln;
        }
        if (!rn.is_ok()) {
            return rn;
        }
        new_val = Value::number(ln.value().as_number() / rn.value().as_number());
        break;
    }
    case AssignOp::ModAssign: {
        auto ln = to_number(current_result.value());
        auto rn = to_number(rhs.value());
        if (!ln.is_ok()) {
            return ln;
        }
        if (!rn.is_ok()) {
            return rn;
        }
        new_val = Value::number(std::fmod(ln.value().as_number(), rn.value().as_number()));
        break;
    }
    default:
        break;
    }

    auto set_result = current_env_->set(expr.target, new_val);
    if (!set_result.is_ok()) {
        const std::string& msg = set_result.error().message();
        NativeErrorType err_type = NativeErrorType::kTypeError;
        if (msg.rfind("ReferenceError:", 0) == 0) err_type = NativeErrorType::kReferenceError;
        pending_throw_ = make_error_value(err_type, strip_error_prefix(msg));
        return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
    }
    return EvalResult::ok(new_val);
}

EvalResult Interpreter::eval_object_expr(const ObjectExpression& expr) {
    auto obj = RcPtr<JSObject>::make();
    gc_heap_.Register(obj.get());
    obj->set_proto(object_prototype_);
    for (const auto& prop : expr.properties) {
        auto val = eval_expr(*prop.value);
        if (!val.is_ok()) {
            return val;
        }
        obj->set_property(prop.key, val.value());
    }
    return EvalResult::ok(Value::object(ObjectPtr(obj)));
}

EvalResult Interpreter::eval_member_expr(const MemberExpression& expr) {
    auto obj_result = eval_expr(*expr.object);
    if (!obj_result.is_ok()) {
        return obj_result;
    }
    const Value& obj_val = obj_result.value();

    if (obj_val.is_undefined() || obj_val.is_null()) {
        pending_throw_ = make_error_value(NativeErrorType::kTypeError,
            "Cannot read properties of " + to_string_val(obj_val));
        return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
    }

    // 非对象：Phase 3 返回 undefined（Phase 5 补原始值包装）
    if (!obj_val.is_object()) {
        return EvalResult::ok(Value::undefined());
    }

    auto key_result = eval_expr(*expr.property);
    if (!key_result.is_ok()) {
        return key_result;
    }
    std::string key = to_string_val(key_result.value());

    RcObject* raw_obj = obj_val.as_object_raw();
    if (raw_obj->object_kind() == ObjectKind::kFunction) {
        auto* fn = static_cast<JSFunction*>(raw_obj);
        if (key == "prototype") {
            const auto& proto = fn->prototype_obj();
            return EvalResult::ok(proto ? Value::object(ObjectPtr(proto)) : Value::undefined());
        }
        Value own = fn->get_property(key);
        if (!own.is_undefined()) return EvalResult::ok(own);
        // Fall back to Function.prototype
        if (function_prototype_) return EvalResult::ok(function_prototype_->get_property(key));
        return EvalResult::ok(Value::undefined());
    }
    if (raw_obj->object_kind() != ObjectKind::kOrdinary && raw_obj->object_kind() != ObjectKind::kArray) {
        return EvalResult::ok(Value::undefined());
    }
    auto* js_obj = static_cast<JSObject*>(raw_obj);
    return EvalResult::ok(js_obj->get_property(key));
}

EvalResult Interpreter::eval_member_assign(const MemberAssignmentExpression& expr) {
    auto obj_result = eval_expr(*expr.object);
    if (!obj_result.is_ok()) {
        return obj_result;
    }
    const Value& obj_val = obj_result.value();

    if (obj_val.is_undefined() || obj_val.is_null()) {
        pending_throw_ = make_error_value(NativeErrorType::kTypeError,
            "Cannot set properties of " + to_string_val(obj_val));
        return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
    }
    if (!obj_val.is_object()) {
        pending_throw_ = make_error_value(NativeErrorType::kTypeError,
            "Cannot set properties of non-object");
        return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
    }

    auto key_result = eval_expr(*expr.property);
    if (!key_result.is_ok()) {
        return key_result;
    }
    std::string key = to_string_val(key_result.value());

    auto val_result = eval_expr(*expr.value);
    if (!val_result.is_ok()) {
        return val_result;
    }

    RcObject* raw_obj2 = obj_val.as_object_raw();
    if (raw_obj2->object_kind() != ObjectKind::kOrdinary && raw_obj2->object_kind() != ObjectKind::kArray) {
        pending_throw_ = make_error_value(NativeErrorType::kTypeError,
            "Cannot set properties of non-ordinary object");
        return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
    }
    auto* js_obj = static_cast<JSObject*>(raw_obj2);
    auto set_result = js_obj->set_property_ex(key, val_result.value());
    if (!set_result.is_ok()) {
        const std::string& msg = set_result.error().message();
        NativeErrorType err_type = NativeErrorType::kRangeError;
        if (msg.rfind("TypeError:", 0) == 0) err_type = NativeErrorType::kTypeError;
        pending_throw_ = make_error_value(err_type, strip_error_prefix(msg));
        return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
    }
    return EvalResult::ok(val_result.value());
}

Value Interpreter::make_function_value(std::optional<std::string> name, std::vector<std::string> params,
                                        std::shared_ptr<std::vector<StmtNode>> body,
                                        RcPtr<Environment> closure_env,
                                        bool is_named_expr) {
    auto fn = RcPtr<JSFunction>::make();
    fn->set_name(name);
    fn->set_params(std::move(params));
    fn->set_body(std::move(body));
    fn->set_closure_env(std::move(closure_env));
    fn->set_is_named_expr(is_named_expr);

    // Eager prototype initialization: F.prototype = { constructor: F }
    Value fn_val = Value::object(ObjectPtr(fn));
    auto proto_obj = RcPtr<JSObject>::make();
    proto_obj->set_proto(object_prototype_);
    proto_obj->set_constructor_property(fn.get());
    fn->set_prototype_obj(proto_obj);

    gc_heap_.Register(fn.get());
    gc_heap_.Register(proto_obj.get());

    return fn_val;
}

StmtResult Interpreter::call_function(RcPtr<JSFunction> fn, Value this_val,
                                      std::vector<Value> args, bool is_new_call) {
    if (fn->is_native()) {
        auto r = fn->native_fn()(this_val, std::move(args), is_new_call);
        if (!r.is_ok()) {
            return StmtResult::err(r.error());
        }
        return StmtResult::ok(Completion::return_(r.value()));
    }

    RcPtr<Environment> outer = fn->closure_env() ? fn->closure_env() : global_env_;
    auto fn_env = RcPtr<Environment>::make(outer);
    gc_heap_.Register(fn_env.get());
    if (fn->is_named_expr() && fn->name().has_value()) {
        fn_env->define(fn->name().value(), VarKind::Const);
        auto init_result = fn_env->initialize(fn->name().value(), Value::object(ObjectPtr(fn)));
        if (!init_result.is_ok()) {
            return StmtResult::err(init_result.error());
        }
    }

    const auto& params = fn->params();
    for (size_t i = 0; i < params.size(); ++i) {
        Value arg_val = (i < args.size()) ? args[i] : Value::undefined();
        fn_env->define(params[i], VarKind::Var);
        fn_env->initialize(params[i], std::move(arg_val));
    }

    ScopeGuard guard(*this, fn_env, fn_env, std::move(this_val), /*is_call=*/true);
    hoist_vars(*fn->body(), *fn_env);

    Value result_val = Value::undefined();
    for (const auto& stmt : *fn->body()) {
        auto stmt_result = eval_stmt(stmt);
        if (!stmt_result.is_ok()) {
            return stmt_result;
        }
        const Completion& c = stmt_result.completion();
        if (c.is_return() || c.is_throw()) {
            return stmt_result;  // preserve kReturn/kThrow so callers can distinguish
        }
        result_val = c.value;
    }
    return StmtResult::ok(Completion::normal(result_val));
}

StmtResult Interpreter::eval_function_decl(const FunctionDeclaration& stmt) {
    Value fn_val = make_function_value(stmt.name, stmt.params, stmt.body, current_env_);
    auto set_result = var_env_->set(stmt.name, fn_val);
    if (!set_result.is_ok()) {
        return StmtResult::err(set_result.error());
    }
    return StmtResult::ok(Completion::normal(Value::undefined()));
}

// ---- Phase 7: throw / try / break / continue / labeled / for ----

// Extract a pending throw value from either:
//   (a) pending_throw_ sentinel (thrown Value from call boundary)
//   (b) or create a string Value from the error message
// Clears pending_throw_ after extraction.
static Value extract_throw_value(std::optional<Value>& pending, const std::string& msg,
                                  const char* sentinel) {
    if (msg == sentinel && pending.has_value()) {
        Value v = std::move(*pending);
        pending = std::nullopt;
        return v;
    }
    return Value::string(msg);
}

StmtResult Interpreter::eval_throw_stmt(const ThrowStatement& stmt) {
    auto r = eval_expr(stmt.argument);
    if (!r.is_ok()) {
        Value thrown = extract_throw_value(pending_throw_, r.error().message(), kPendingThrowSentinel);
        return StmtResult::ok(Completion::throw_(std::move(thrown)));
    }
    return StmtResult::ok(Completion::throw_(r.value()));
}

StmtResult Interpreter::exec_catch(const CatchClause& handler, Value thrown_val) {
    auto catch_env = RcPtr<Environment>::make(current_env_);
    gc_heap_.Register(catch_env.get());
    auto old_env = current_env_;
    current_env_ = catch_env;

    catch_env->define(handler.param, VarKind::Let);
    catch_env->initialize(handler.param, thrown_val);

    auto result = eval_block_stmt(handler.body);

    current_env_ = old_env;
    return result;
}

StmtResult Interpreter::eval_try_stmt(const TryStatement& stmt) {
    // 1. Execute try block
    StmtResult try_result = eval_block_stmt(stmt.block);

    // Internal C++ error from try block → convert to ThrowCompletion
    if (!try_result.is_ok()) {
        Value thrown = extract_throw_value(pending_throw_, try_result.error().message(),
                                           kPendingThrowSentinel);
        try_result = StmtResult::ok(Completion::throw_(std::move(thrown)));
    }

    // 2. If there is a catch handler and try produced a throw, execute catch
    if (stmt.handler.has_value()) {
        if (try_result.is_ok() && try_result.completion().is_throw()) {
            Value thrown_val = try_result.completion().value;
            try_result = exec_catch(*stmt.handler, std::move(thrown_val));
            // Internal error from catch → convert to ThrowCompletion
            if (!try_result.is_ok()) {
                Value thrown = extract_throw_value(pending_throw_, try_result.error().message(),
                                                   kPendingThrowSentinel);
                try_result = StmtResult::ok(Completion::throw_(std::move(thrown)));
            }
        }
        // If try was not a throw, catch body is skipped
    }

    // 3. Finally block: always execute, may override prior completion
    if (stmt.finalizer.has_value()) {
        StmtResult finally_result = eval_block_stmt(*stmt.finalizer);

        // Internal error from finally → replaces everything
        if (!finally_result.is_ok()) {
            return finally_result;
        }

        // Finally abrupt completion → replaces prior result
        if (finally_result.completion().is_abrupt()) {
            return finally_result;
        }

        // Finally normal completion → prior result wins
        return try_result;
    }

    return try_result;
}

StmtResult Interpreter::eval_break_stmt(const BreakStatement& stmt) {
    return StmtResult::ok(Completion::break_(stmt.label));
}

StmtResult Interpreter::eval_continue_stmt(const ContinueStatement& stmt) {
    return StmtResult::ok(Completion::continue_(stmt.label));
}

StmtResult Interpreter::eval_labeled_stmt(const LabeledStatement& stmt) {
    StmtResult result = StmtResult::ok(Completion::normal(Value::undefined()));

    // Pass label directly to loops so they can handle labeled continue internally
    if (std::holds_alternative<ForStatement>(stmt.body->v)) {
        result = eval_for_stmt(std::get<ForStatement>(stmt.body->v), stmt.label);
    } else if (std::holds_alternative<WhileStatement>(stmt.body->v)) {
        result = eval_while_stmt(std::get<WhileStatement>(stmt.body->v), stmt.label);
    } else {
        result = eval_stmt(*stmt.body);
    }

    if (result.is_ok() && result.completion().is_break() &&
        result.completion().target == stmt.label) {
        return StmtResult::ok(Completion::normal(Value::undefined()));
    }
    return result;
}

StmtResult Interpreter::eval_for_stmt(const ForStatement& stmt,
                                       std::optional<std::string> label) {
    // Create outer scope for for-init variables
    auto for_env = RcPtr<Environment>::make(current_env_);
    gc_heap_.Register(for_env.get());
    auto old_env = current_env_;
    current_env_ = for_env;

    // Execute init
    if (stmt.init.has_value()) {
        auto init_result = eval_stmt(*stmt.init.value());
        if (!init_result.is_ok()) {
            current_env_ = old_env;
            return init_result;
        }
        if (init_result.completion().is_abrupt()) {
            current_env_ = old_env;
            return init_result;
        }
    }

    StmtResult loop_result = StmtResult::ok(Completion::normal(Value::undefined()));

    while (true) {
        // Test condition
        if (stmt.test.has_value()) {
            auto test_r = eval_expr(*stmt.test);
            if (!test_r.is_ok()) {
                Value thrown = extract_throw_value(pending_throw_, test_r.error().message(),
                                                   kPendingThrowSentinel);
                current_env_ = old_env;
                return StmtResult::ok(Completion::throw_(std::move(thrown)));
            }
            if (!to_boolean(test_r.value())) {
                break;
            }
        }

        // Execute body
        auto body_result = eval_stmt(*stmt.body);
        if (!body_result.is_ok()) {
            current_env_ = old_env;
            return body_result;
        }
        const Completion& c = body_result.completion();
        if (c.is_break()) {
            if (!c.target.has_value() || c.target == label) {
                // Unlabeled break or break targeting this loop's label
                break;
            }
            current_env_ = old_env;
            return body_result;  // Labeled break for outer loop, propagate up
        }
        if (c.is_continue()) {
            if (!c.target.has_value() || c.target == label) {
                // Unlabeled continue or continue targeting this loop's label: fall through to update
            } else {
                current_env_ = old_env;
                return body_result;  // Labeled continue for outer loop, propagate up
            }
        } else if (c.is_return() || c.is_throw()) {
            current_env_ = old_env;
            return body_result;
        }

        // Execute update
        if (stmt.update.has_value()) {
            auto update_r = eval_expr(*stmt.update);
            if (!update_r.is_ok()) {
                Value thrown = extract_throw_value(pending_throw_, update_r.error().message(),
                                                   kPendingThrowSentinel);
                current_env_ = old_env;
                return StmtResult::ok(Completion::throw_(std::move(thrown)));
            }
        }
    }

    current_env_ = old_env;
    return loop_result;
}

EvalResult Interpreter::eval_function_expr(const FunctionExpression& expr) {
    return EvalResult::ok(make_function_value(expr.name, expr.params, expr.body, current_env_,
                                              expr.name.has_value()));
}

EvalResult Interpreter::eval_call_expr(const CallExpression& expr) {
    if (call_depth_ >= kMaxCallDepth) {
        pending_throw_ = make_error_value(NativeErrorType::kRangeError,
            "Maximum call stack size exceeded");
        return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
    }

    Value this_val = Value::undefined();
    Value callee_val = Value::undefined();

    // Detect method call: obj.method() — extract this from the object
    if (std::holds_alternative<MemberExpression>(expr.callee->v)) {
        const auto& member = std::get<MemberExpression>(expr.callee->v);
        auto obj_result = eval_expr(*member.object);
        if (!obj_result.is_ok()) {
            return obj_result;
        }
        this_val = obj_result.value();

        auto key_result = eval_expr(*member.property);
        if (!key_result.is_ok()) {
            return key_result;
        }
        std::string key = to_string_val(key_result.value());

        if (!this_val.is_object()) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "Cannot read properties of " + to_string_val(this_val));
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        RcObject* obj_ptr = this_val.as_object_raw();
        if (obj_ptr->object_kind() == ObjectKind::kOrdinary || obj_ptr->object_kind() == ObjectKind::kArray) {
            auto* js_obj = static_cast<JSObject*>(obj_ptr);
            callee_val = js_obj->get_property(key);
        } else if (obj_ptr->object_kind() == ObjectKind::kFunction) {
            auto* fn_obj = static_cast<JSFunction*>(obj_ptr);
            if (key == "prototype") {
                const auto& proto = fn_obj->prototype_obj();
                callee_val = proto ? Value::object(ObjectPtr(proto)) : Value::undefined();
            } else {
                callee_val = fn_obj->get_property(key);
                if (callee_val.is_undefined() && function_prototype_) {
                    callee_val = function_prototype_->get_property(key);
                }
            }
        } else {
            callee_val = Value::undefined();
        }
    } else {
        auto callee_result = eval_expr(*expr.callee);
        if (!callee_result.is_ok()) {
            return callee_result;
        }
        callee_val = std::move(callee_result.value());
    }

    if (!callee_val.is_object() || !callee_val.as_object_raw() ||
        callee_val.as_object_raw()->object_kind() != ObjectKind::kFunction) {
        pending_throw_ = make_error_value(NativeErrorType::kTypeError, "value is not a function");
        return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
    }
    auto* fn_raw = static_cast<JSFunction*>(callee_val.as_object_raw());
    auto fn = RcPtr<JSFunction>(fn_raw);

    std::vector<Value> args;
    args.reserve(expr.arguments.size());
    for (const auto& arg_expr : expr.arguments) {
        auto arg_result = eval_expr(*arg_expr);
        if (!arg_result.is_ok()) {
            return arg_result;
        }
        args.push_back(std::move(arg_result.value()));
    }

    auto call_result = call_function(fn, std::move(this_val), std::move(args));
    if (!call_result.is_ok()) {
        return EvalResult::err(call_result.error());
    }
    if (call_result.completion().is_throw()) {
        pending_throw_ = call_result.completion().value;
        return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
    }
    return EvalResult::ok(call_result.completion().value);
}

EvalResult Interpreter::call_function_val(Value fn_val, Value this_val, std::span<Value> args) {
    if (!fn_val.is_object() || !fn_val.as_object_raw() ||
        fn_val.as_object_raw()->object_kind() != ObjectKind::kFunction) {
        pending_throw_ = make_error_value(NativeErrorType::kTypeError, "value is not a function");
        return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
    }
    auto* fn_raw = static_cast<JSFunction*>(fn_val.as_object_raw());
    auto fn = RcPtr<JSFunction>(fn_raw);
    std::vector<Value> args_vec(args.begin(), args.end());
    auto call_result = call_function(fn, std::move(this_val), std::move(args_vec));
    if (!call_result.is_ok()) {
        return EvalResult::err(call_result.error());
    }
    if (call_result.completion().is_throw()) {
        pending_throw_ = call_result.completion().value;
        return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
    }
    return EvalResult::ok(call_result.completion().value);
}

EvalResult Interpreter::eval_new_expr(const NewExpression& expr) {
    if (call_depth_ >= kMaxCallDepth) {
        pending_throw_ = make_error_value(NativeErrorType::kRangeError,
            "Maximum call stack size exceeded");
        return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
    }

    auto callee_result = eval_expr(*expr.callee);
    if (!callee_result.is_ok()) {
        return callee_result;
    }
    const Value& callee_val = callee_result.value();
    if (!callee_val.is_object() || !callee_val.as_object_raw() ||
        callee_val.as_object_raw()->object_kind() != ObjectKind::kFunction) {
        pending_throw_ = make_error_value(NativeErrorType::kTypeError, "value is not a constructor");
        return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
    }
    auto* fn_raw2 = static_cast<JSFunction*>(callee_val.as_object_raw());
    auto fn = RcPtr<JSFunction>(fn_raw2);

    // Determine prototype for new object
    RcPtr<JSObject> proto = fn->prototype_obj() ? fn->prototype_obj() : object_prototype_;

    // Create new object with [[Prototype]] = F.prototype
    auto new_obj = RcPtr<JSObject>::make();
    gc_heap_.Register(new_obj.get());
    new_obj->set_proto(proto);

    std::vector<Value> args;
    args.reserve(expr.arguments.size());
    for (const auto& arg_expr : expr.arguments) {
        auto arg_result = eval_expr(*arg_expr);
        if (!arg_result.is_ok()) {
            return arg_result;
        }
        args.push_back(std::move(arg_result.value()));
    }

    Value this_val = Value::object(ObjectPtr(new_obj));
    auto call_result = call_function(fn, this_val, std::move(args), /*is_new_call=*/true);
    if (!call_result.is_ok()) {
        return EvalResult::err(call_result.error());
    }
    if (call_result.completion().is_throw()) {
        pending_throw_ = call_result.completion().value;
        return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
    }

    // Only an explicit return <Object> overrides this_val (ECMAScript §10.2.2 step 9)
    const Completion& c = call_result.completion();
    if (c.is_return() && c.value.is_object() && c.value.as_object_raw() != nullptr) {
        return EvalResult::ok(c.value);
    }
    return EvalResult::ok(this_val);
}

}  // namespace qppjs
