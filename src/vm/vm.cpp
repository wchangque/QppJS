#include "qppjs/vm/vm.h"

#include "qppjs/base/error.h"
#include "qppjs/runtime/completion.h"
#include "qppjs/runtime/environment.h"
#include "qppjs/runtime/js_function.h"
#include "qppjs/runtime/js_object.h"
#include "qppjs/runtime/native_errors.h"
#include "qppjs/runtime/value.h"
#include "qppjs/vm/bytecode.h"
#include "qppjs/vm/opcode.h"

#include <cassert>
#include <charconv>
#include <cmath>
#include <cstdlib>
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
    global_env_ = std::make_shared<Environment>(nullptr);
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
}

std::shared_ptr<Environment> VM::clone_closure_env(const std::shared_ptr<Environment>& env,
                                                   const std::optional<std::string>& excluded_name) const {
    if (!excluded_name.has_value() || env == nullptr) {
        return env;
    }
    return env->clone_for_closure(excluded_name);
}

// ============================================================
// exec (public entry)
// ============================================================

EvalResult VM::exec(std::shared_ptr<BytecodeFunction> bytecode) {
    global_env_ = std::make_shared<Environment>(nullptr);
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
    global_env_->clear_function_bindings();
    object_prototype_->clear_function_properties();
    if (array_prototype_) array_prototype_->clear_function_properties();
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

    auto outer = fn->closure_env() ? fn->closure_env() : global_env_;
    auto fn_env = std::make_shared<Environment>(outer);
    if (fn->name().has_value() && fn_env->lookup(fn->name().value()) == nullptr) {
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
                    frame.env = frame.env->outer();
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
        std::shared_ptr<Environment>& env = frame.env;

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
            env->define_initialized(bc->names[idx]);
            break;
        }

        case Opcode::kDefLet: {
            uint16_t idx = read_u16(bc, pc);
            env->define(bc->names[idx], VarKind::Let);
            break;
        }

        case Opcode::kDefConst: {
            uint16_t idx = read_u16(bc, pc);
            env->define(bc->names[idx], VarKind::Const);
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

        case Opcode::kPushScope:
            env = std::make_shared<Environment>(env);
            frame.scope_depth++;
            break;

        case Opcode::kPopScope:
            env = env->outer();
            frame.scope_depth--;
            break;

        // ---- Object properties ----

        case Opcode::kNewObject: {
            auto obj = RcPtr<JSObject>::make();
            obj->set_proto(object_prototype_);
            stack.push_back(Value::object(ObjectPtr(obj)));
            break;
        }

        case Opcode::kNewArray: {
            auto arr = RcPtr<JSObject>::make(ObjectKind::kArray);
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
            // Handle JSFunction specially (e.g., Fn.prototype)
            RcObject* raw_obj = obj_val.as_object_raw();
            if (raw_obj->object_kind() == ObjectKind::kFunction) {
                auto* fn = static_cast<JSFunction*>(raw_obj);
                if (name == "prototype") {
                    const auto& proto = fn->prototype_obj();
                    stack.push_back(proto ? Value::object(ObjectPtr(proto)) : Value::undefined());
                } else {
                    stack.push_back(Value::undefined());
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
            fn->set_closure_env(clone_closure_env(env, fn_bc->name));
            auto proto_obj = RcPtr<JSObject>::make();
            proto_obj->set_proto(object_prototype_);
            proto_obj->set_constructor_property(fn.get());
            fn->set_prototype_obj(proto_obj);
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

        default:
            return EvalResult::err(Error(ErrorKind::Runtime, "Internal: unknown opcode"));
        }
    }

    return EvalResult::ok(Value::undefined());
}

}  // namespace qppjs
