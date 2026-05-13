#include "qppjs/runtime/interpreter.h"

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

#include <optional>

#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
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

static double to_number_double(const Value& v) {
    switch (v.kind()) {
    case ValueKind::Undefined: return std::numeric_limits<double>::quiet_NaN();
    case ValueKind::Null:      return 0.0;
    case ValueKind::Bool:      return v.as_bool() ? 1.0 : 0.0;
    case ValueKind::Number:    return v.as_number();
    case ValueKind::String: {
        std::string_view sv = v.sv();
        if (sv.empty()) return 0.0;
        size_t first = sv.find_first_not_of(" \t\n\r\f\v");
        if (first == std::string_view::npos) return 0.0;
        size_t last = sv.find_last_not_of(" \t\n\r\f\v");
        // strtod requires null-terminated string; build one from the trimmed range.
        std::string s(sv.substr(first, last - first + 1));
        char* end = nullptr;
        double r = std::strtod(s.c_str(), &end);
        if (end == s.c_str() || *end != '\0') return std::numeric_limits<double>::quiet_NaN();
        return r;
    }
    case ValueKind::Object: return std::numeric_limits<double>::quiet_NaN();
    }
    return std::numeric_limits<double>::quiet_NaN();
}

static bool strict_eq_values(const Value& a, const Value& b) {
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
    case ValueKind::String:  return a.as_string() == b.as_string();
    case ValueKind::Object:  return a.as_object_raw() == b.as_object_raw();
    }
    return false;
}

static bool same_value_zero(const Value& a, const Value& b) {
    if (a.is_number() && b.is_number() && std::isnan(a.as_number()) && std::isnan(b.as_number())) {
        return true;
    }
    return strict_eq_values(a, b);
}

static std::optional<uint32_t> resolve_from_index(uint32_t len, const std::vector<Value>& args,
                                                   size_t arg_idx) {
    if (args.size() <= arg_idx || args[arg_idx].is_undefined()) return 0u;
    double n = to_number_double(args[arg_idx]);
    if (std::isnan(n)) n = 0.0;
    n = std::trunc(n);
    if (n >= static_cast<double>(len)) return std::nullopt;
    if (n >= 0.0) return static_cast<uint32_t>(n);
    double k = static_cast<double>(len) + n;
    return static_cast<uint32_t>(k < 0.0 ? 0.0 : k);
}

// ============================================================
// UTF-8 string utilities (used by string_prototype_ NativeFns)
// ============================================================

// Compute UTF-16 code unit count with caching via JSString::cp_count_.
// BMP characters (U+0000..U+FFFF) = 1 code unit; SMP characters (U+10000+) = 2 code units.
static int32_t utf8_cp_len(JSString* js_str) {
    if (js_str->cp_count_ >= 0) return js_str->cp_count_;
    std::string_view s = js_str->sv();
    int32_t count = 0;
    for (size_t i = 0; i < s.size(); ) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) { i += 1; count += 1; }
        else if (c < 0xE0) { i += 2; count += 1; }
        else if (c < 0xF0) { i += 3; count += 1; }
        else { i += 4; count += 2; }  // SMP: 2 UTF-16 code units
    }
    js_str->cp_count_ = count;
    return count;
}

// Convert UTF-16 code unit offset to byte offset.
static size_t utf8_cu_to_byte(std::string_view s, int32_t cu_offset) {
    size_t i = 0;
    int32_t cu = 0;
    while (i < s.size() && cu < cu_offset) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) { i += 1; cu += 1; }
        else if (c < 0xE0) { i += 2; cu += 1; }
        else if (c < 0xF0) { i += 3; cu += 1; }
        else { i += 4; cu += 2; }  // SMP: 2 code units
    }
    return i;
}

// Extract substring [cu_start, cu_end) by UTF-16 code unit indices.
static std::string utf8_substr(std::string_view s, int32_t cu_start, int32_t cu_end) {
    if (cu_start >= cu_end) return "";
    size_t byte_start = utf8_cu_to_byte(s, cu_start);
    size_t byte_end = utf8_cu_to_byte(s, cu_end);
    return std::string(s.substr(byte_start, byte_end - byte_start));
}

// Check if a Unicode codepoint is a JS whitespace character.
static bool is_js_whitespace_cp(uint32_t cp) {
    if (cp <= 0x20) {
        return cp == 0x09 || cp == 0x0A || cp == 0x0B || cp == 0x0C || cp == 0x0D || cp == 0x20;
    }
    switch (cp) {
    case 0x00A0: case 0x1680:
    case 0x2000: case 0x2001: case 0x2002: case 0x2003: case 0x2004:
    case 0x2005: case 0x2006: case 0x2007: case 0x2008: case 0x2009:
    case 0x200A: case 0x2028: case 0x2029: case 0x202F: case 0x205F:
    case 0x3000: case 0xFEFF:
        return true;
    default:
        return false;
    }
}

// Decode one UTF-8 codepoint from s[i], advance i.
static uint32_t utf8_decode_one(std::string_view s, size_t& i) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    uint32_t cp;
    if (c < 0x80) {
        cp = c; i += 1;
    } else if (c < 0xE0) {
        cp = (c & 0x1F);
        if (i + 1 < s.size()) cp = (cp << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3F);
        i += 2;
    } else if (c < 0xF0) {
        cp = (c & 0x0F);
        if (i + 1 < s.size()) cp = (cp << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3F);
        if (i + 2 < s.size()) cp = (cp << 6) | (static_cast<unsigned char>(s[i + 2]) & 0x3F);
        i += 3;
    } else {
        cp = (c & 0x07);
        if (i + 1 < s.size()) cp = (cp << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3F);
        if (i + 2 < s.size()) cp = (cp << 6) | (static_cast<unsigned char>(s[i + 2]) & 0x3F);
        if (i + 3 < s.size()) cp = (cp << 6) | (static_cast<unsigned char>(s[i + 3]) & 0x3F);
        i += 4;
    }
    return cp;
}

static std::string utf8_trim_impl(std::string_view s, bool trim_start, bool trim_end) {
    size_t start = 0;
    if (trim_start) {
        while (start < s.size()) {
            size_t tmp = start;
            uint32_t cp = utf8_decode_one(s, tmp);
            if (!is_js_whitespace_cp(cp)) break;
            start = tmp;
        }
    }
    size_t end = s.size();
    if (trim_end && end > start) {
        // Walk backwards without allocating a positions vector.
        // Find the start of the last UTF-8 codepoint by scanning back over continuation bytes.
        while (end > start) {
            size_t cp_start = end - 1;
            while (cp_start > start && (static_cast<unsigned char>(s[cp_start]) & 0xC0) == 0x80) {
                --cp_start;
            }
            size_t tmp = cp_start;
            uint32_t cp = utf8_decode_one(s, tmp);
            if (!is_js_whitespace_cp(cp)) break;
            end = cp_start;
        }
    }
    if (start >= end) return "";
    return std::string(s.substr(start, end - start));
}

// indexOf: returns UTF-16 code unit index of first occurrence of needle in haystack
// starting from code unit offset cu_from. Returns -1 if not found.
static int32_t str_index_of(std::string_view haystack, std::string_view needle,
                             int32_t cu_from, int32_t len) {
    if (needle.empty()) {
        return std::min(cu_from, len);
    }
    size_t byte_from = utf8_cu_to_byte(haystack, cu_from);
    size_t pos = haystack.find(needle, byte_from);
    if (pos == std::string_view::npos) return -1;
    // Convert byte pos back to UTF-16 code unit index
    int32_t cu_idx = 0;
    for (size_t i = 0; i < pos; ) {
        unsigned char c = static_cast<unsigned char>(haystack[i]);
        if (c < 0x80) { i += 1; cu_idx += 1; }
        else if (c < 0xE0) { i += 2; cu_idx += 1; }
        else if (c < 0xF0) { i += 3; cu_idx += 1; }
        else { i += 4; cu_idx += 2; }  // SMP: 2 code units
    }
    return cu_idx;
}

// lastIndexOf: returns UTF-16 code unit index of last occurrence of needle in haystack
// searching up to and including code unit offset cu_from. Returns -1 if not found.
static int32_t str_last_index_of(std::string_view haystack, std::string_view needle,
                                  int32_t cu_from, int32_t len) {
    if (needle.empty()) {
        return std::min(cu_from, len);
    }
    // byte_from is the byte offset of cu_from (the maximum allowed start position).
    size_t byte_from = utf8_cu_to_byte(haystack, cu_from);
    size_t pos = haystack.rfind(needle, byte_from);
    if (pos == std::string_view::npos) return -1;
    int32_t cu_idx = 0;
    for (size_t i = 0; i < pos; ) {
        unsigned char c = static_cast<unsigned char>(haystack[i]);
        if (c < 0x80) { i += 1; cu_idx += 1; }
        else if (c < 0xE0) { i += 2; cu_idx += 1; }
        else if (c < 0xF0) { i += 3; cu_idx += 1; }
        else { i += 4; cu_idx += 2; }  // SMP: 2 code units
    }
    return cu_idx;
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

    // Array.prototype.map
    auto map_fn = RcPtr<JSFunction>::make();
    map_fn->set_name(std::string("map"));
    map_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        RcObject* raw = this_val.as_object_raw();
        if (!raw || raw->object_kind() != ObjectKind::kArray) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError, "map called on non-array");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        if (args.empty() || !args[0].is_object() ||
            args[0].as_object_raw()->object_kind() != ObjectKind::kFunction) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError, "callback is not a function");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        auto* arr = static_cast<JSObject*>(raw);
        Value callback = args[0];
        Value this_arg = args.size() >= 2 ? args[1] : Value::undefined();
        uint32_t len = arr->array_length_;
        auto result = RcPtr<JSObject>::make(ObjectKind::kArray);
        gc_heap_.Register(result.get());
        result->set_proto(array_prototype_);
        result->array_length_ = len;
        result->elements_.reserve(arr->elements_.size());
        for (uint32_t i = 0; i < len; i++) {
            auto it = arr->elements_.find(i);
            if (it == arr->elements_.end()) continue;
            Value elem = it->second;
            Value call_args[3] = {elem, Value::number(static_cast<double>(i)), this_val};
            auto res = call_function_val(callback, this_arg, {call_args, 3});
            if (!res.is_ok()) return res;
            result->elements_[i] = std::move(res.value());
        }
        return EvalResult::ok(Value::object(ObjectPtr(result)));
    });
    array_prototype_->set_property("map", Value::object(ObjectPtr(map_fn)));

    // Array.prototype.filter
    auto filter_fn = RcPtr<JSFunction>::make();
    filter_fn->set_name(std::string("filter"));
    filter_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        RcObject* raw = this_val.as_object_raw();
        if (!raw || raw->object_kind() != ObjectKind::kArray) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError, "filter called on non-array");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        if (args.empty() || !args[0].is_object() ||
            args[0].as_object_raw()->object_kind() != ObjectKind::kFunction) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError, "callback is not a function");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        auto* arr = static_cast<JSObject*>(raw);
        Value callback = args[0];
        Value this_arg = args.size() >= 2 ? args[1] : Value::undefined();
        uint32_t len = arr->array_length_;
        auto result = RcPtr<JSObject>::make(ObjectKind::kArray);
        gc_heap_.Register(result.get());
        result->set_proto(array_prototype_);
        uint32_t to = 0;
        for (uint32_t i = 0; i < len; i++) {
            auto it = arr->elements_.find(i);
            if (it == arr->elements_.end()) continue;
            Value elem = it->second;
            Value call_args[3] = {elem, Value::number(static_cast<double>(i)), this_val};
            auto res = call_function_val(callback, this_arg, {call_args, 3});
            if (!res.is_ok()) return res;
            if (to_boolean(res.value())) {
                result->elements_[to++] = elem;
            }
        }
        result->array_length_ = to;
        return EvalResult::ok(Value::object(ObjectPtr(result)));
    });
    array_prototype_->set_property("filter", Value::object(ObjectPtr(filter_fn)));

    // Array.prototype.reduce
    auto reduce_fn = RcPtr<JSFunction>::make();
    reduce_fn->set_name(std::string("reduce"));
    reduce_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        RcObject* raw = this_val.as_object_raw();
        if (!raw || raw->object_kind() != ObjectKind::kArray) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError, "reduce called on non-array");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        if (args.empty() || !args[0].is_object() ||
            args[0].as_object_raw()->object_kind() != ObjectKind::kFunction) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError, "callback is not a function");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        auto* arr = static_cast<JSObject*>(raw);
        Value callback = args[0];
        bool has_initial = args.size() >= 2;
        uint32_t len = arr->array_length_;
        Value acc;
        uint32_t k = 0;
        if (has_initial) {
            acc = args[1];
        } else {
            bool found = false;
            for (; k < len; k++) {
                auto it = arr->elements_.find(k);
                if (it != arr->elements_.end()) {
                    acc = it->second;
                    k++;
                    found = true;
                    break;
                }
            }
            if (!found) {
                pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                                  "Reduce of empty array with no initial value");
                return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
            }
        }
        for (uint32_t i = k; i < len; i++) {
            auto it = arr->elements_.find(i);
            if (it == arr->elements_.end()) continue;
            Value call_args[4] = {acc, it->second, Value::number(static_cast<double>(i)), this_val};
            auto res = call_function_val(callback, Value::undefined(), {call_args, 4});
            if (!res.is_ok()) return res;
            acc = std::move(res.value());
        }
        return EvalResult::ok(acc);
    });
    array_prototype_->set_property("reduce", Value::object(ObjectPtr(reduce_fn)));

    // Array.prototype.reduceRight
    auto reduce_right_fn = RcPtr<JSFunction>::make();
    reduce_right_fn->set_name(std::string("reduceRight"));
    reduce_right_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        RcObject* raw = this_val.as_object_raw();
        if (!raw || raw->object_kind() != ObjectKind::kArray) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                              "reduceRight called on non-array");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        if (args.empty() || !args[0].is_object() ||
            args[0].as_object_raw()->object_kind() != ObjectKind::kFunction) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError, "callback is not a function");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        auto* arr = static_cast<JSObject*>(raw);
        Value callback = args[0];
        bool has_initial = args.size() >= 2;
        int64_t len = static_cast<int64_t>(arr->array_length_);
        Value acc;
        int64_t k = 0;
        if (has_initial) {
            acc = args[1];
            k = len - 1;
        } else {
            bool found = false;
            for (k = len - 1; k >= 0; k--) {
                auto it = arr->elements_.find(static_cast<uint32_t>(k));
                if (it != arr->elements_.end()) {
                    acc = it->second;
                    k--;
                    found = true;
                    break;
                }
            }
            if (!found) {
                pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                                  "Reduce of empty array with no initial value");
                return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
            }
        }
        for (; k >= 0; k--) {
            auto it = arr->elements_.find(static_cast<uint32_t>(k));
            if (it == arr->elements_.end()) continue;
            Value call_args[4] = {acc, it->second, Value::number(static_cast<double>(k)), this_val};
            auto res = call_function_val(callback, Value::undefined(), {call_args, 4});
            if (!res.is_ok()) return res;
            acc = std::move(res.value());
        }
        return EvalResult::ok(acc);
    });
    array_prototype_->set_property("reduceRight", Value::object(ObjectPtr(reduce_right_fn)));

    // Array.prototype.find
    auto find_fn = RcPtr<JSFunction>::make();
    find_fn->set_name(std::string("find"));
    find_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        RcObject* raw = this_val.as_object_raw();
        if (!raw || raw->object_kind() != ObjectKind::kArray) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError, "find called on non-array");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        if (args.empty() || !args[0].is_object() ||
            args[0].as_object_raw()->object_kind() != ObjectKind::kFunction) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError, "callback is not a function");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        auto* arr = static_cast<JSObject*>(raw);
        Value callback = args[0];
        Value this_arg = args.size() >= 2 ? args[1] : Value::undefined();
        uint32_t len = arr->array_length_;
        for (uint32_t i = 0; i < len; i++) {
            auto it = arr->elements_.find(i);
            Value call_args[3] = {
                it != arr->elements_.end() ? it->second : Value::undefined(),
                Value::number(static_cast<double>(i)),
                this_val
            };
            auto res = call_function_val(callback, this_arg, {call_args, 3});
            if (!res.is_ok()) return res;
            if (to_boolean(res.value())) return EvalResult::ok(call_args[0]);
        }
        return EvalResult::ok(Value::undefined());
    });
    array_prototype_->set_property("find", Value::object(ObjectPtr(find_fn)));

    // Array.prototype.findIndex
    auto find_index_fn = RcPtr<JSFunction>::make();
    find_index_fn->set_name(std::string("findIndex"));
    find_index_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        RcObject* raw = this_val.as_object_raw();
        if (!raw || raw->object_kind() != ObjectKind::kArray) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                              "findIndex called on non-array");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        if (args.empty() || !args[0].is_object() ||
            args[0].as_object_raw()->object_kind() != ObjectKind::kFunction) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError, "callback is not a function");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        auto* arr = static_cast<JSObject*>(raw);
        Value callback = args[0];
        Value this_arg = args.size() >= 2 ? args[1] : Value::undefined();
        uint32_t len = arr->array_length_;
        for (uint32_t i = 0; i < len; i++) {
            auto it = arr->elements_.find(i);
            Value call_args[3] = {
                it != arr->elements_.end() ? it->second : Value::undefined(),
                Value::number(static_cast<double>(i)),
                this_val
            };
            auto res = call_function_val(callback, this_arg, {call_args, 3});
            if (!res.is_ok()) return res;
            if (to_boolean(res.value())) return EvalResult::ok(Value::number(static_cast<double>(i)));
        }
        return EvalResult::ok(Value::number(-1.0));
    });
    array_prototype_->set_property("findIndex", Value::object(ObjectPtr(find_index_fn)));

    // Array.prototype.some
    auto some_fn = RcPtr<JSFunction>::make();
    some_fn->set_name(std::string("some"));
    some_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        RcObject* raw = this_val.as_object_raw();
        if (!raw || raw->object_kind() != ObjectKind::kArray) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError, "some called on non-array");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        if (args.empty() || !args[0].is_object() ||
            args[0].as_object_raw()->object_kind() != ObjectKind::kFunction) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError, "callback is not a function");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        auto* arr = static_cast<JSObject*>(raw);
        Value callback = args[0];
        Value this_arg = args.size() >= 2 ? args[1] : Value::undefined();
        uint32_t len = arr->array_length_;
        for (uint32_t i = 0; i < len; i++) {
            auto it = arr->elements_.find(i);
            if (it == arr->elements_.end()) continue;
            Value elem = it->second;
            Value call_args[3] = {elem, Value::number(static_cast<double>(i)), this_val};
            auto res = call_function_val(callback, this_arg, {call_args, 3});
            if (!res.is_ok()) return res;
            if (to_boolean(res.value())) return EvalResult::ok(Value::boolean(true));
        }
        return EvalResult::ok(Value::boolean(false));
    });
    array_prototype_->set_property("some", Value::object(ObjectPtr(some_fn)));

    // Array.prototype.every
    auto every_fn = RcPtr<JSFunction>::make();
    every_fn->set_name(std::string("every"));
    every_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        RcObject* raw = this_val.as_object_raw();
        if (!raw || raw->object_kind() != ObjectKind::kArray) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError, "every called on non-array");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        if (args.empty() || !args[0].is_object() ||
            args[0].as_object_raw()->object_kind() != ObjectKind::kFunction) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError, "callback is not a function");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        auto* arr = static_cast<JSObject*>(raw);
        Value callback = args[0];
        Value this_arg = args.size() >= 2 ? args[1] : Value::undefined();
        uint32_t len = arr->array_length_;
        for (uint32_t i = 0; i < len; i++) {
            auto it = arr->elements_.find(i);
            if (it == arr->elements_.end()) continue;
            Value elem = it->second;
            Value call_args[3] = {elem, Value::number(static_cast<double>(i)), this_val};
            auto res = call_function_val(callback, this_arg, {call_args, 3});
            if (!res.is_ok()) return res;
            if (!to_boolean(res.value())) return EvalResult::ok(Value::boolean(false));
        }
        return EvalResult::ok(Value::boolean(true));
    });
    array_prototype_->set_property("every", Value::object(ObjectPtr(every_fn)));

    // Array.prototype.indexOf
    auto index_of_fn = RcPtr<JSFunction>::make();
    index_of_fn->set_name(std::string("indexOf"));
    index_of_fn->set_native_fn([](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        RcObject* raw = this_val.as_object_raw();
        if (!raw || raw->object_kind() != ObjectKind::kArray) {
            return EvalResult::err(Error(ErrorKind::Runtime, "TypeError: indexOf called on non-array"));
        }
        auto* arr = static_cast<JSObject*>(raw);
        uint32_t len = arr->array_length_;
        auto k_opt = resolve_from_index(len, args, 1);
        if (!k_opt.has_value()) return EvalResult::ok(Value::number(-1.0));
        Value search_val = args.size() >= 1 ? args[0] : Value::undefined();
        for (uint32_t i = *k_opt; i < len; i++) {
            auto it = arr->elements_.find(i);
            if (it == arr->elements_.end()) continue;
            if (strict_eq_values(it->second, search_val)) {
                return EvalResult::ok(Value::number(static_cast<double>(i)));
            }
        }
        return EvalResult::ok(Value::number(-1.0));
    });
    array_prototype_->set_property("indexOf", Value::object(ObjectPtr(index_of_fn)));

    // Array.prototype.includes
    auto includes_fn = RcPtr<JSFunction>::make();
    includes_fn->set_name(std::string("includes"));
    includes_fn->set_native_fn([](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        RcObject* raw = this_val.as_object_raw();
        if (!raw || raw->object_kind() != ObjectKind::kArray) {
            return EvalResult::err(Error(ErrorKind::Runtime, "TypeError: includes called on non-array"));
        }
        auto* arr = static_cast<JSObject*>(raw);
        uint32_t len = arr->array_length_;
        auto k_opt = resolve_from_index(len, args, 1);
        if (!k_opt.has_value()) return EvalResult::ok(Value::boolean(false));
        Value search_val = args.size() >= 1 ? args[0] : Value::undefined();
        for (uint32_t i = *k_opt; i < len; i++) {
            auto it = arr->elements_.find(i);
            Value elem = it != arr->elements_.end() ? it->second : Value::undefined();
            if (same_value_zero(elem, search_val)) return EvalResult::ok(Value::boolean(true));
        }
        return EvalResult::ok(Value::boolean(false));
    });
    array_prototype_->set_property("includes", Value::object(ObjectPtr(includes_fn)));

    // Array.prototype.slice
    auto slice_fn = RcPtr<JSFunction>::make();
    slice_fn->set_name(std::string("slice"));
    slice_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        RcObject* raw = this_val.as_object_raw();
        if (!raw || raw->object_kind() != ObjectKind::kArray) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError, "slice called on non-array");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        auto* arr = static_cast<JSObject*>(raw);
        int64_t len = static_cast<int64_t>(arr->array_length_);
        double start_d = args.size() >= 1 && !args[0].is_undefined() ? to_number_double(args[0]) : 0.0;
        if (std::isnan(start_d)) start_d = 0.0;
        start_d = std::trunc(start_d);
        int64_t start = start_d < 0.0 ? std::max(len + static_cast<int64_t>(start_d), int64_t{0})
                                       : std::min(static_cast<int64_t>(start_d), len);
        double end_d = args.size() >= 2 && !args[1].is_undefined() ? to_number_double(args[1])
                                                                    : static_cast<double>(len);
        if (std::isnan(end_d)) end_d = 0.0;
        end_d = std::trunc(end_d);
        int64_t end = end_d < 0.0 ? std::max(len + static_cast<int64_t>(end_d), int64_t{0})
                                   : std::min(static_cast<int64_t>(end_d), len);
        int64_t count = std::max(end - start, int64_t{0});
        auto result = RcPtr<JSObject>::make(ObjectKind::kArray);
        gc_heap_.Register(result.get());
        result->set_proto(array_prototype_);
        uint32_t n = 0;
        for (int64_t k = start; k < start + count; k++) {
            auto it = arr->elements_.find(static_cast<uint32_t>(k));
            if (it != arr->elements_.end()) {
                result->elements_[n] = it->second;
            }
            n++;
        }
        result->array_length_ = static_cast<uint32_t>(count);
        return EvalResult::ok(Value::object(ObjectPtr(result)));
    });
    array_prototype_->set_property("slice", Value::object(ObjectPtr(slice_fn)));

    // Array.prototype.splice
    auto splice_fn = RcPtr<JSFunction>::make();
    splice_fn->set_name(std::string("splice"));
    splice_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        RcObject* raw = this_val.as_object_raw();
        if (!raw || raw->object_kind() != ObjectKind::kArray) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError, "splice called on non-array");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        auto* arr = static_cast<JSObject*>(raw);
        int64_t len = static_cast<int64_t>(arr->array_length_);
        int64_t start = 0;
        if (!args.empty()) {
            double s = to_number_double(args[0]);
            if (std::isnan(s)) s = 0.0;
            s = std::trunc(s);
            if (std::isinf(s) && s < 0.0) {
                start = 0;
            } else if (std::isinf(s)) {
                start = len;
            } else if (s < 0.0) {
                start = std::max(len + static_cast<int64_t>(s), int64_t{0});
            } else {
                start = std::min(static_cast<int64_t>(s), len);
            }
        }
        int64_t del_count = 0;
        uint32_t item_count = 0;
        if (args.empty()) {
            del_count = 0;
        } else if (args.size() == 1) {
            del_count = len - start;
        } else {
            item_count = static_cast<uint32_t>(args.size() - 2);
            double dc = to_number_double(args[1]);
            if (std::isnan(dc)) dc = 0.0;
            dc = std::trunc(dc);
            del_count = static_cast<int64_t>(
                std::max(0.0, std::min(dc, static_cast<double>(len - start))));
        }
        auto deleted = RcPtr<JSObject>::make(ObjectKind::kArray);
        gc_heap_.Register(deleted.get());
        deleted->set_proto(array_prototype_);
        for (int64_t k = 0; k < del_count; k++) {
            auto it = arr->elements_.find(static_cast<uint32_t>(start + k));
            if (it != arr->elements_.end()) {
                deleted->elements_[static_cast<uint32_t>(k)] = it->second;
            }
        }
        deleted->array_length_ = static_cast<uint32_t>(del_count);
        int64_t new_len = len - del_count + static_cast<int64_t>(item_count);
        if (item_count < static_cast<uint32_t>(del_count)) {
            int64_t shift = del_count - static_cast<int64_t>(item_count);
            for (int64_t k = start + static_cast<int64_t>(item_count); k < new_len; k++) {
                auto it = arr->elements_.find(static_cast<uint32_t>(k + shift));
                if (it != arr->elements_.end()) {
                    arr->elements_[static_cast<uint32_t>(k)] = std::move(it->second);
                    arr->elements_.erase(it);
                } else {
                    arr->elements_.erase(static_cast<uint32_t>(k));
                }
            }
            for (int64_t k = new_len; k < len; k++) {
                arr->elements_.erase(static_cast<uint32_t>(k));
            }
        } else if (item_count > static_cast<uint32_t>(del_count)) {
            int64_t shift = static_cast<int64_t>(item_count) - del_count;
            for (int64_t k = len - 1; k >= start + del_count; k--) {
                auto it = arr->elements_.find(static_cast<uint32_t>(k));
                if (it != arr->elements_.end()) {
                    arr->elements_[static_cast<uint32_t>(k + shift)] = std::move(it->second);
                    arr->elements_.erase(it);
                } else {
                    arr->elements_.erase(static_cast<uint32_t>(k + shift));
                }
            }
        }
        for (uint32_t i = 0; i < item_count; i++) {
            arr->elements_[static_cast<uint32_t>(start) + i] = args[2 + i];
        }
        arr->array_length_ = static_cast<uint32_t>(new_len);
        return EvalResult::ok(Value::object(ObjectPtr(deleted)));
    });
    array_prototype_->set_property("splice", Value::object(ObjectPtr(splice_fn)));

    // Array.prototype.sort
    auto sort_fn = RcPtr<JSFunction>::make();
    sort_fn->set_name(std::string("sort"));
    sort_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        RcObject* raw = this_val.as_object_raw();
        if (!raw || raw->object_kind() != ObjectKind::kArray) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError, "sort called on non-array");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        bool has_cmp = !args.empty() && !args[0].is_undefined();
        if (has_cmp) {
            if (!args[0].is_object() || args[0].as_object_raw()->object_kind() != ObjectKind::kFunction) {
                pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                                  "compareFn must be a function");
                return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
            }
        }
        auto* arr = static_cast<JSObject*>(raw);
        uint32_t len = arr->array_length_;
        struct Slot {
            Value val;
            uint32_t pos;
        };
        std::vector<Slot> slots;
        slots.reserve(arr->elements_.size());
        for (uint32_t i = 0; i < len; i++) {
            auto it = arr->elements_.find(i);
            if (it != arr->elements_.end()) {
                slots.push_back({it->second, i});
            }
        }
        Value cmp_fn = has_cmp ? args[0] : Value::undefined();
        EvalResult sort_err = EvalResult::ok(Value::undefined());
        bool had_error = false;
        std::stable_sort(slots.begin(), slots.end(), [&](const Slot& a, const Slot& b) -> bool {
            if (had_error) return false;
            if (has_cmp) {
                Value call_args[2] = {a.val, b.val};
                auto res = call_function_val(cmp_fn, Value::undefined(), {call_args, 2});
                if (!res.is_ok()) {
                    sort_err = res;
                    had_error = true;
                    return false;
                }
                double cmp = to_number_double(res.value());
                if (std::isnan(cmp)) cmp = 0.0;
                return cmp < 0.0;
            } else {
                std::string sa = Interpreter::to_string_val(a.val);
                std::string sb = Interpreter::to_string_val(b.val);
                return sa < sb;
            }
        });
        if (had_error) return sort_err;
        arr->elements_.clear();
        for (uint32_t i = 0; i < static_cast<uint32_t>(slots.size()); i++) {
            arr->elements_[i] = std::move(slots[i].val);
        }
        return EvalResult::ok(this_val);
    });
    array_prototype_->set_property("sort", Value::object(ObjectPtr(sort_fn)));

    // Array.prototype.join
    auto join_fn = RcPtr<JSFunction>::make();
    join_fn->set_name(std::string("join"));
    join_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        RcObject* raw = this_val.as_object_raw();
        if (!raw || raw->object_kind() != ObjectKind::kArray) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError, "join called on non-array");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        auto* arr = static_cast<JSObject*>(raw);
        uint32_t len = arr->array_length_;
        std::string sep = (args.empty() || args[0].is_undefined()) ? "," : Interpreter::to_string_val(args[0]);
        if (len == 0) return EvalResult::ok(Value::string(""));
        // First pass: compute total length for reserve
        size_t total = 0;
        for (uint32_t k = 0; k < len; k++) {
            auto it = arr->elements_.find(k);
            if (it != arr->elements_.end() && !it->second.is_null() && !it->second.is_undefined()) {
                if (it->second.is_string()) {
                    total += it->second.sv().size();
                } else {
                    total += Interpreter::to_string_val(it->second).size();
                }
            }
            if (k > 0) total += sep.size();
        }
        std::string result;
        result.reserve(total);
        for (uint32_t k = 0; k < len; k++) {
            if (k > 0) result += sep;
            auto it = arr->elements_.find(k);
            if (it != arr->elements_.end() && !it->second.is_null() && !it->second.is_undefined()) {
                if (it->second.is_string()) {
                    result += it->second.sv();
                } else {
                    result += Interpreter::to_string_val(it->second);
                }
            }
        }
        return EvalResult::ok(Value::string(result));
    });
    array_prototype_->set_property("join", Value::object(ObjectPtr(join_fn)));

    // Array.prototype.reverse
    auto reverse_fn = RcPtr<JSFunction>::make();
    reverse_fn->set_name(std::string("reverse"));
    reverse_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        (void)args;
        RcObject* raw = this_val.as_object_raw();
        if (!raw || raw->object_kind() != ObjectKind::kArray) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError, "reverse called on non-array");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        auto* arr = static_cast<JSObject*>(raw);
        uint32_t len = arr->array_length_;
        uint32_t middle = len / 2;
        for (uint32_t lower = 0; lower < middle; lower++) {
            uint32_t upper = len - 1 - lower;
            bool lower_exists = arr->elements_.count(lower) > 0;
            bool upper_exists = arr->elements_.count(upper) > 0;
            if (lower_exists && upper_exists) {
                std::swap(arr->elements_[lower], arr->elements_[upper]);
            } else if (upper_exists) {
                arr->elements_[lower] = arr->elements_[upper];
                arr->elements_.erase(upper);
            } else if (lower_exists) {
                arr->elements_[upper] = arr->elements_[lower];
                arr->elements_.erase(lower);
            }
        }
        return EvalResult::ok(this_val);
    });
    array_prototype_->set_property("reverse", Value::object(ObjectPtr(reverse_fn)));

    // Array.prototype.flat
    // Recursive helper captured by the native lambda
    auto flatten_into_array = [](auto& self, JSObject* result, JSObject* source,
                                 uint32_t source_len, uint32_t& target_idx,
                                 double depth, int recursion_depth) -> void {
        if (recursion_depth > 10000) return;
        for (uint32_t k = 0; k < source_len; k++) {
            auto it = source->elements_.find(k);
            if (it == source->elements_.end()) continue;
            const Value& elem = it->second;
            if (depth > 0.0 && elem.is_object() &&
                elem.as_object_raw()->object_kind() == ObjectKind::kArray) {
                auto* inner = static_cast<JSObject*>(elem.as_object_raw());
                self(self, result, inner, inner->array_length_, target_idx, depth - 1.0,
                     recursion_depth + 1);
            } else {
                result->elements_[target_idx++] = elem;
            }
        }
    };
    auto flat_fn = RcPtr<JSFunction>::make();
    flat_fn->set_name(std::string("flat"));
    flat_fn->set_native_fn([this, flatten_into_array](Value this_val, std::vector<Value> args,
                                                       bool) -> EvalResult {
        RcObject* raw = this_val.as_object_raw();
        if (!raw || raw->object_kind() != ObjectKind::kArray) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError, "flat called on non-array");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        auto* arr = static_cast<JSObject*>(raw);
        double depth_num = 1.0;
        if (!args.empty() && !args[0].is_undefined()) {
            depth_num = to_number_double(args[0]);
            if (std::isnan(depth_num)) depth_num = 0.0;
            depth_num = std::trunc(depth_num);
            if (std::isinf(depth_num) && depth_num > 0.0) {
                // positive infinity — keep as +Inf
            } else if (depth_num < 0.0) {
                depth_num = 0.0;
            }
        }
        auto result = RcPtr<JSObject>::make(ObjectKind::kArray);
        gc_heap_.Register(result.get());
        result->set_proto(array_prototype_);
        uint32_t target_idx = 0;
        flatten_into_array(flatten_into_array, result.get(), arr, arr->array_length_, target_idx,
                           depth_num, 0);
        result->array_length_ = target_idx;
        return EvalResult::ok(Value::object(ObjectPtr(result)));
    });
    array_prototype_->set_property("flat", Value::object(ObjectPtr(flat_fn)));

    // Array.prototype.flatMap
    auto flat_map_fn = RcPtr<JSFunction>::make();
    flat_map_fn->set_name(std::string("flatMap"));
    flat_map_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        RcObject* raw = this_val.as_object_raw();
        if (!raw || raw->object_kind() != ObjectKind::kArray) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                              "flatMap called on non-array");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        if (args.empty() || !args[0].is_object() ||
            args[0].as_object_raw()->object_kind() != ObjectKind::kFunction) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                              "flatMap callback must be a function");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        Value callback = args[0];
        Value this_arg = args.size() >= 2 ? args[1] : Value::undefined();
        auto* arr = static_cast<JSObject*>(raw);
        auto result = RcPtr<JSObject>::make(ObjectKind::kArray);
        gc_heap_.Register(result.get());
        result->set_proto(array_prototype_);
        uint32_t target_idx = 0;
        for (uint32_t k = 0; k < arr->array_length_; k++) {
            auto it = arr->elements_.find(k);
            if (it == arr->elements_.end()) continue;
            Value call_args[3] = {it->second, Value::number(static_cast<double>(k)), this_val};
            auto res = call_function_val(callback, this_arg, {call_args, 3});
            if (!res.is_ok()) return res;
            Value mapped = res.value();
            if (mapped.is_object() && mapped.as_object_raw()->object_kind() == ObjectKind::kArray) {
                auto* inner = static_cast<JSObject*>(mapped.as_object_raw());
                for (uint32_t j = 0; j < inner->array_length_; j++) {
                    auto jt = inner->elements_.find(j);
                    if (jt != inner->elements_.end()) {
                        result->elements_[target_idx++] = jt->second;
                    }
                }
            } else {
                result->elements_[target_idx++] = std::move(mapped);
            }
        }
        result->array_length_ = target_idx;
        return EvalResult::ok(Value::object(ObjectPtr(result)));
    });
    array_prototype_->set_property("flatMap", Value::object(ObjectPtr(flat_map_fn)));

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

    // ---- Promise ----

    // Build Promise.prototype
    promise_prototype_ = RcPtr<JSObject>::make();
    promise_prototype_->set_proto(object_prototype_);

    // Promise.prototype.then
    auto then_fn = RcPtr<JSFunction>::make();
    then_fn->set_name(std::string("then"));
    then_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        RcObject* raw = this_val.as_object_raw();
        if (!raw || raw->object_kind() != ObjectKind::kPromise) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "Promise.prototype.then called on non-Promise");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        auto* p = static_cast<JSPromise*>(raw);
        auto promise_rc = RcPtr<JSPromise>(p);
        Value on_fulfilled = args.size() > 0 ? args[0] : Value::undefined();
        Value on_rejected = args.size() > 1 ? args[1] : Value::undefined();
        auto result_promise = JSPromise::PerformThen(promise_rc, on_fulfilled, on_rejected, job_queue_);
        gc_heap_.Register(result_promise.get());
        return EvalResult::ok(Value::object(ObjectPtr(result_promise)));
    });
    promise_prototype_->set_property("then", Value::object(ObjectPtr(then_fn)));

    // Promise.prototype.catch
    auto catch_fn = RcPtr<JSFunction>::make();
    catch_fn->set_name(std::string("catch"));
    catch_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        RcObject* raw = this_val.as_object_raw();
        if (!raw || raw->object_kind() != ObjectKind::kPromise) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "Promise.prototype.catch called on non-Promise");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        auto* p = static_cast<JSPromise*>(raw);
        auto promise_rc = RcPtr<JSPromise>(p);
        Value on_rejected = args.size() > 0 ? args[0] : Value::undefined();
        auto result_promise = JSPromise::PerformThen(promise_rc, Value::undefined(), on_rejected, job_queue_);
        gc_heap_.Register(result_promise.get());
        return EvalResult::ok(Value::object(ObjectPtr(result_promise)));
    });
    promise_prototype_->set_property("catch", Value::object(ObjectPtr(catch_fn)));

    // Promise.prototype.finally
    auto finally_fn = RcPtr<JSFunction>::make();
    finally_fn->set_name(std::string("finally"));
    finally_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        RcObject* raw = this_val.as_object_raw();
        if (!raw || raw->object_kind() != ObjectKind::kPromise) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "Promise.prototype.finally called on non-Promise");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        auto* p = static_cast<JSPromise*>(raw);
        auto promise_rc = RcPtr<JSPromise>(p);
        Value on_finally = args.size() > 0 ? args[0] : Value::undefined();

        // finally 语义：无论 fulfill/reject 都调用 on_finally，然后传递原始值
        // 用 then(fulfill_wrapper, reject_wrapper) 实现
        Value captured_on_finally = on_finally;

        auto fulfill_wrapper = RcPtr<JSFunction>::make();
        fulfill_wrapper->set_native_fn([this, captured_on_finally](Value /*this_val*/,
                std::vector<Value> args2, bool) mutable -> EvalResult {
            Value val = args2.empty() ? Value::undefined() : args2[0];
            if (captured_on_finally.is_object() &&
                captured_on_finally.as_object_raw() &&
                captured_on_finally.as_object_raw()->object_kind() == ObjectKind::kFunction) {
                std::vector<Value> no_args;
                auto r = call_function_val(captured_on_finally, Value::undefined(),
                                           std::span<Value>(no_args.data(), no_args.size()));
                if (!r.is_ok()) return r;
                // C15: if finally fn returns a rejected Promise, propagate its reason
                if (r.is_ok() && r.value().is_object() && r.value().as_object_raw() &&
                    r.value().as_object_raw()->object_kind() == ObjectKind::kPromise) {
                    auto* rp = static_cast<JSPromise*>(r.value().as_object_raw());
                    if (rp->state() == PromiseState::kRejected) {
                        pending_throw_ = rp->result();
                        return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
                    }
                }
            }
            return EvalResult::ok(val);
        });
        gc_heap_.Register(fulfill_wrapper.get());

        auto reject_wrapper = RcPtr<JSFunction>::make();
        reject_wrapper->set_native_fn([this, captured_on_finally](Value /*this_val*/,
                std::vector<Value> args2, bool) mutable -> EvalResult {
            Value reason = args2.empty() ? Value::undefined() : args2[0];
            if (captured_on_finally.is_object() &&
                captured_on_finally.as_object_raw() &&
                captured_on_finally.as_object_raw()->object_kind() == ObjectKind::kFunction) {
                std::vector<Value> no_args;
                auto r = call_function_val(captured_on_finally, Value::undefined(),
                                           std::span<Value>(no_args.data(), no_args.size()));
                if (!r.is_ok()) return r;
                // C15: if finally fn returns a rejected Promise, propagate its reason
                if (r.is_ok() && r.value().is_object() && r.value().as_object_raw() &&
                    r.value().as_object_raw()->object_kind() == ObjectKind::kPromise) {
                    auto* rp = static_cast<JSPromise*>(r.value().as_object_raw());
                    if (rp->state() == PromiseState::kRejected) {
                        pending_throw_ = rp->result();
                        return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
                    }
                }
            }
            // re-throw original rejection reason
            pending_throw_ = reason;
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        });
        gc_heap_.Register(reject_wrapper.get());

        auto result_promise = JSPromise::PerformThen(promise_rc,
            Value::object(ObjectPtr(fulfill_wrapper)),
            Value::object(ObjectPtr(reject_wrapper)),
            job_queue_);
        gc_heap_.Register(result_promise.get());
        return EvalResult::ok(Value::object(ObjectPtr(result_promise)));
    });
    promise_prototype_->set_property("finally", Value::object(ObjectPtr(finally_fn)));

    // Promise constructor
    auto promise_ctor = RcPtr<JSFunction>::make();
    promise_ctor->set_name(std::string("Promise"));
    promise_ctor->set_native_fn([this](Value /*this_val*/, std::vector<Value> args,
                                        bool /*is_new_call*/) -> EvalResult {
        if (args.empty() || !args[0].is_object() || !args[0].as_object_raw() ||
            args[0].as_object_raw()->object_kind() != ObjectKind::kFunction) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "Promise constructor requires a function argument");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        auto promise = RcPtr<JSPromise>::make();
        gc_heap_.Register(promise.get());
        Value promise_val = Value::object(ObjectPtr(promise));

        // Create resolve function (capture promise by RcPtr to keep it alive)
        auto resolve_fn = RcPtr<JSFunction>::make();
        resolve_fn->set_native_fn([this, promise](Value /*this_val*/,
                std::vector<Value> resolve_args, bool) mutable -> EvalResult {
            Value val = resolve_args.empty() ? Value::undefined() : resolve_args[0];
            // If val is a Promise, adopt its state via PerformThen
            if (val.is_object() && val.as_object_raw() &&
                val.as_object_raw()->object_kind() == ObjectKind::kPromise) {
                auto* inner = static_cast<JSPromise*>(val.as_object_raw());
                auto inner_rc = RcPtr<JSPromise>(inner);
                auto fulfill_outer = RcPtr<JSFunction>::make();
                fulfill_outer->set_native_fn([this, promise](Value, std::vector<Value> a, bool) mutable -> EvalResult {
                    Value v = a.empty() ? Value::undefined() : a[0];
                    promise->Fulfill(v, job_queue_);
                    return EvalResult::ok(Value::undefined());
                });
                gc_heap_.Register(fulfill_outer.get());
                auto reject_outer = RcPtr<JSFunction>::make();
                reject_outer->set_native_fn([this, promise](Value, std::vector<Value> a, bool) mutable -> EvalResult {
                    Value r = a.empty() ? Value::undefined() : a[0];
                    promise->Reject(r, job_queue_);
                    return EvalResult::ok(Value::undefined());
                });
                gc_heap_.Register(reject_outer.get());
                JSPromise::PerformThen(inner_rc,
                    Value::object(ObjectPtr(fulfill_outer)),
                    Value::object(ObjectPtr(reject_outer)),
                    job_queue_);
            } else {
                promise->Fulfill(std::move(val), job_queue_);
            }
            return EvalResult::ok(Value::undefined());
        });
        gc_heap_.Register(resolve_fn.get());

        // Create reject function
        auto reject_fn = RcPtr<JSFunction>::make();
        reject_fn->set_native_fn([this, promise](Value /*this_val*/,
                std::vector<Value> reject_args, bool) mutable -> EvalResult {
            Value reason = reject_args.empty() ? Value::undefined() : reject_args[0];
            promise->Reject(std::move(reason), job_queue_);
            return EvalResult::ok(Value::undefined());
        });
        gc_heap_.Register(reject_fn.get());

        // Call executor(resolve, reject)
        std::vector<Value> executor_args = {
            Value::object(ObjectPtr(resolve_fn)),
            Value::object(ObjectPtr(reject_fn))
        };
        auto exec_result = call_function_val(args[0], Value::undefined(),
                                              std::span<Value>(executor_args.data(), executor_args.size()));
        if (!exec_result.is_ok()) {
            // executor threw: reject the promise
            Value thrown_val;
            if (exec_result.error().message() == kPendingThrowSentinel && pending_throw_.has_value()) {
                thrown_val = std::move(*pending_throw_);
                pending_throw_ = std::nullopt;
            } else {
                thrown_val = Value::string(exec_result.error().message());
            }
            promise->Reject(std::move(thrown_val), job_queue_);
        }

        return EvalResult::ok(promise_val);
    });

    // Promise.resolve static method
    auto promise_resolve_fn = RcPtr<JSFunction>::make();
    promise_resolve_fn->set_name(std::string("resolve"));
    promise_resolve_fn->set_native_fn([this](Value /*this_val*/, std::vector<Value> args, bool) -> EvalResult {
        Value val = args.empty() ? Value::undefined() : args[0];
        auto p = promise_resolve(val);
        return EvalResult::ok(Value::object(ObjectPtr(p)));
    });
    promise_ctor->set_property("resolve", Value::object(ObjectPtr(promise_resolve_fn)));

    // Promise.reject static method
    auto promise_reject_fn = RcPtr<JSFunction>::make();
    promise_reject_fn->set_name(std::string("reject"));
    promise_reject_fn->set_native_fn([this](Value /*this_val*/, std::vector<Value> args, bool) -> EvalResult {
        Value reason = args.empty() ? Value::undefined() : args[0];
        auto p = RcPtr<JSPromise>::make();
        gc_heap_.Register(p.get());
        p->Reject(reason, job_queue_);
        return EvalResult::ok(Value::object(ObjectPtr(p)));
    });
    promise_ctor->set_property("reject", Value::object(ObjectPtr(promise_reject_fn)));

    // P2-B: Promise.prototype must be accessible via Promise.prototype
    promise_ctor->set_property("prototype", Value::object(ObjectPtr(promise_prototype_)));

    gc_heap_.Register(promise_ctor.get());
    global_env_->define_initialized("Promise");
    global_env_->set("Promise", Value::object(ObjectPtr(promise_ctor)));

    // String.prototype
    string_prototype_ = RcPtr<JSObject>::make();
    string_prototype_->set_proto(object_prototype_);

    // indexOf(searchString, fromIndex)
    auto str_index_of_fn = RcPtr<JSFunction>::make();
    str_index_of_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        if (this_val.is_null() || this_val.is_undefined()) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "String.prototype.indexOf called on null or undefined");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        Value effective_this = this_val.is_string() ? this_val : Value::string(to_string_val(this_val));
        JSString* js_str = effective_this.js_string_raw();
        int32_t len = utf8_cp_len(js_str);
        std::string search = args.empty() ? "undefined" : to_string_val(args[0]);
        int32_t k = 0;
        if (args.size() >= 2) {
            double n = to_number_double(args[1]);
            if (std::isinf(n) && n > 0) {
                k = len;
            } else {
                if (std::isnan(n)) n = 0.0;
                n = std::trunc(n);
                k = n < 0.0 ? 0 : (n > len ? len : static_cast<int32_t>(n));
            }
        }
        return EvalResult::ok(Value::number(static_cast<double>(str_index_of(js_str->sv(), search, k, len))));
    });
    gc_heap_.Register(str_index_of_fn.get());
    string_prototype_->set_property("indexOf", Value::object(ObjectPtr(str_index_of_fn)));

    // lastIndexOf(searchString, fromIndex)
    auto str_last_index_of_fn = RcPtr<JSFunction>::make();
    str_last_index_of_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        if (this_val.is_null() || this_val.is_undefined()) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "String.prototype.lastIndexOf called on null or undefined");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        Value effective_this = this_val.is_string() ? this_val : Value::string(to_string_val(this_val));
        JSString* js_str = effective_this.js_string_raw();
        int32_t len = utf8_cp_len(js_str);
        std::string search = args.empty() ? "undefined" : to_string_val(args[0]);
        int32_t k = len;
        if (args.size() >= 2) {
            double n = to_number_double(args[1]);
            if (std::isnan(n)) {
                k = len;
            } else {
                n = std::trunc(n);
                if (n < 0.0) k = 0;
                else if (n > len) k = len;
                else k = static_cast<int32_t>(n);
            }
        }
        return EvalResult::ok(Value::number(static_cast<double>(str_last_index_of(js_str->sv(), search, k, len))));
    });
    gc_heap_.Register(str_last_index_of_fn.get());
    string_prototype_->set_property("lastIndexOf", Value::object(ObjectPtr(str_last_index_of_fn)));

    // slice(start, end)
    auto str_slice_fn = RcPtr<JSFunction>::make();
    str_slice_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        if (this_val.is_null() || this_val.is_undefined()) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "String.prototype.slice called on null or undefined");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        Value effective_this = this_val.is_string() ? this_val : Value::string(to_string_val(this_val));
        JSString* js_str = effective_this.js_string_raw();
        int32_t len = utf8_cp_len(js_str);
        auto resolve_slice_idx = [&](size_t arg_pos, int32_t default_val) -> int32_t {
            if (args.size() <= arg_pos || args[arg_pos].is_undefined()) return default_val;
            double n = to_number_double(args[arg_pos]);
            if (std::isnan(n)) return 0;
            if (std::isinf(n)) return n > 0 ? len : 0;
            n = std::trunc(n);
            if (n < 0.0) return static_cast<int32_t>(std::max(0.0, static_cast<double>(len) + n));
            return static_cast<int32_t>(std::min(static_cast<double>(len), n));
        };
        int32_t from = resolve_slice_idx(0, 0);
        int32_t to = resolve_slice_idx(1, len);
        return EvalResult::ok(Value::string(utf8_substr(js_str->sv(), from, to)));
    });
    gc_heap_.Register(str_slice_fn.get());
    string_prototype_->set_property("slice", Value::object(ObjectPtr(str_slice_fn)));

    // substring(start, end)
    auto str_substring_fn = RcPtr<JSFunction>::make();
    str_substring_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        if (this_val.is_null() || this_val.is_undefined()) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "String.prototype.substring called on null or undefined");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        Value effective_this = this_val.is_string() ? this_val : Value::string(to_string_val(this_val));
        JSString* js_str = effective_this.js_string_raw();
        int32_t len = utf8_cp_len(js_str);
        auto resolve_sub_idx = [&](size_t arg_pos, int32_t default_val) -> int32_t {
            if (args.size() <= arg_pos || args[arg_pos].is_undefined()) return default_val;
            double n = to_number_double(args[arg_pos]);
            if (std::isnan(n) || n < 0.0) return 0;
            if (n > static_cast<double>(len)) return len;
            return static_cast<int32_t>(std::trunc(n));
        };
        int32_t start = resolve_sub_idx(0, 0);
        int32_t end = resolve_sub_idx(1, len);
        if (start > end) std::swap(start, end);
        return EvalResult::ok(Value::string(utf8_substr(js_str->sv(), start, end)));
    });
    gc_heap_.Register(str_substring_fn.get());
    string_prototype_->set_property("substring", Value::object(ObjectPtr(str_substring_fn)));

    // split(separator, limit)
    auto str_split_fn = RcPtr<JSFunction>::make();
    str_split_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        if (this_val.is_null() || this_val.is_undefined()) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "String.prototype.split called on null or undefined");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        std::string str = to_string_val(this_val);
        auto result = RcPtr<JSObject>::make(ObjectKind::kArray);
        result->set_proto(array_prototype_);
        gc_heap_.Register(result.get());

        // M-2: parse limit before checking undefined separator
        uint32_t limit = std::numeric_limits<uint32_t>::max();
        if (args.size() >= 2 && !args[1].is_undefined()) {
            double n = to_number_double(args[1]);
            if (std::isnan(n) || std::isinf(n)) {
                limit = 0;
            } else {
                // ToUint32: modulo 2^32 of ToInteger(n)
                limit = static_cast<uint32_t>(static_cast<int64_t>(std::trunc(n)));
            }
        }

        if (limit == 0) {
            result->array_length_ = 0;
            return EvalResult::ok(Value::object(ObjectPtr(result)));
        }

        if (args.empty() || args[0].is_undefined()) {
            result->elements_[0] = Value::string(str);
            result->array_length_ = 1;
            return EvalResult::ok(Value::object(ObjectPtr(result)));
        }

        std::string sep = to_string_val(args[0]);
        uint32_t idx = 0;

        if (sep.empty()) {
            // Split by UTF-16 code unit (BMP = 1 element, SMP = 2 elements per spec)
            // Simplified: treat each UTF-8 sequence as one element (per-codepoint split).
            // SMP surrogate-pair splitting is not implemented (known limitation).
            size_t i = 0;
            while (i < str.size() && idx < limit) {
                size_t start = i;
                unsigned char c = static_cast<unsigned char>(str[i]);
                size_t cp_bytes;
                if (c < 0x80) cp_bytes = 1;
                else if (c < 0xE0) cp_bytes = 2;
                else if (c < 0xF0) cp_bytes = 3;
                else cp_bytes = 4;
                i += cp_bytes;
                result->elements_[idx] = Value::string(str.substr(start, cp_bytes));
                ++idx;
            }
        } else {
            size_t pos = 0;
            while (idx < limit) {
                size_t found = str.find(sep, pos);
                if (found == std::string::npos) {
                    result->elements_[idx] = Value::string(str.substr(pos));
                    ++idx;
                    break;
                }
                result->elements_[idx] = Value::string(str.substr(pos, found - pos));
                ++idx;
                pos = found + sep.size();
            }
        }
        result->array_length_ = idx;
        return EvalResult::ok(Value::object(ObjectPtr(result)));
    });
    gc_heap_.Register(str_split_fn.get());
    string_prototype_->set_property("split", Value::object(ObjectPtr(str_split_fn)));

    // trim()
    auto str_trim_fn = RcPtr<JSFunction>::make();
    str_trim_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        (void)args;
        if (this_val.is_null() || this_val.is_undefined()) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "String.prototype.trim called on null or undefined");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        return EvalResult::ok(Value::string(utf8_trim_impl(to_string_val(this_val), true, true)));
    });
    gc_heap_.Register(str_trim_fn.get());
    string_prototype_->set_property("trim", Value::object(ObjectPtr(str_trim_fn)));

    // trimStart()
    auto str_trim_start_fn = RcPtr<JSFunction>::make();
    str_trim_start_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        (void)args;
        if (this_val.is_null() || this_val.is_undefined()) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "String.prototype.trimStart called on null or undefined");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        return EvalResult::ok(Value::string(utf8_trim_impl(to_string_val(this_val), true, false)));
    });
    gc_heap_.Register(str_trim_start_fn.get());
    string_prototype_->set_property("trimStart", Value::object(ObjectPtr(str_trim_start_fn)));

    // trimEnd()
    auto str_trim_end_fn = RcPtr<JSFunction>::make();
    str_trim_end_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        (void)args;
        if (this_val.is_null() || this_val.is_undefined()) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "String.prototype.trimEnd called on null or undefined");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        return EvalResult::ok(Value::string(utf8_trim_impl(to_string_val(this_val), false, true)));
    });
    gc_heap_.Register(str_trim_end_fn.get());
    string_prototype_->set_property("trimEnd", Value::object(ObjectPtr(str_trim_end_fn)));

    // ---- Global constants: NaN, Infinity ----

    global_env_->define("NaN", VarKind::Const);
    global_env_->initialize("NaN", Value::number(std::numeric_limits<double>::quiet_NaN()));
    global_env_->define("Infinity", VarKind::Const);
    global_env_->initialize("Infinity", Value::number(std::numeric_limits<double>::infinity()));

    // ---- Global functions: isNaN, isFinite, parseInt, parseFloat ----

    // parseFloat helper (no substr copy)
    static auto parse_float_impl = [](const std::string& s) -> double {
        size_t start = 0;
        while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) start++;
        if (start == s.size()) return std::numeric_limits<double>::quiet_NaN();
        // JS spec: parseFloat only parses decimal float; "0x..." → parse only "0"
        if (s[start] == '0' && start + 1 < s.size() &&
            (s[start + 1] == 'x' || s[start + 1] == 'X')) {
            return 0.0;
        }
        char* end = nullptr;
        double result = std::strtod(s.c_str() + start, &end);
        if (end == s.c_str() + start) return std::numeric_limits<double>::quiet_NaN();
        return result;
    };

    // parseInt helper
    static auto parse_int_impl = [](const std::string& s, int radix) -> double {
        size_t i = 0;
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) i++;
        if (i == s.size()) return std::numeric_limits<double>::quiet_NaN();
        int sign = 1;
        if (s[i] == '+') { i++; }
        else if (s[i] == '-') { sign = -1; i++; }
        // Detect 0x/0X prefix
        if (radix == 0 || radix == 16) {
            if (i + 1 < s.size() && s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
                radix = 16;
                i += 2;
            }
        }
        if (radix == 0) radix = 10;
        if (radix < 2 || radix > 36) return std::numeric_limits<double>::quiet_NaN();
        if (i == s.size()) return std::numeric_limits<double>::quiet_NaN();
        // Parse digits manually to handle partial match; use double to avoid signed overflow UB
        double result = 0.0;
        bool found = false;
        while (i < s.size()) {
            char c = s[i];
            int digit = -1;
            if (c >= '0' && c <= '9') digit = c - '0';
            else if (c >= 'a' && c <= 'z') digit = c - 'a' + 10;
            else if (c >= 'A' && c <= 'Z') digit = c - 'A' + 10;
            if (digit < 0 || digit >= radix) break;
            result = result * radix + digit;
            found = true;
            i++;
        }
        if (!found) return std::numeric_limits<double>::quiet_NaN();
        return sign < 0 ? -result : result;
    };

    // Build parseInt function (shared with Number.parseInt)
    auto parse_int_fn = RcPtr<JSFunction>::make();
    parse_int_fn->set_name(std::string("parseInt"));
    parse_int_fn->set_native_fn([](Value /*this_val*/, std::vector<Value> args, bool) -> EvalResult {
        std::string s = args.empty() ? "undefined" : Interpreter::to_string_val(args[0]);
        int radix = 0;
        if (args.size() >= 2) {
            double r = to_number_double(args[1]);
            radix = std::isnan(r) ? 0 : static_cast<int>(std::trunc(r));
        }
        return EvalResult::ok(Value::number(parse_int_impl(s, radix)));
    });
    gc_heap_.Register(parse_int_fn.get());
    Value parse_int_val = Value::object(ObjectPtr(parse_int_fn));
    global_env_->define_initialized("parseInt");
    global_env_->set("parseInt", parse_int_val);

    // Build parseFloat function
    auto parse_float_fn = RcPtr<JSFunction>::make();
    parse_float_fn->set_name(std::string("parseFloat"));
    parse_float_fn->set_native_fn([](Value /*this_val*/, std::vector<Value> args, bool) -> EvalResult {
        std::string s = args.empty() ? "undefined" : Interpreter::to_string_val(args[0]);
        return EvalResult::ok(Value::number(parse_float_impl(s)));
    });
    gc_heap_.Register(parse_float_fn.get());
    global_env_->define_initialized("parseFloat");
    global_env_->set("parseFloat", Value::object(ObjectPtr(parse_float_fn)));

    // Build global isNaN (does ToNumber conversion)
    auto is_nan_fn = RcPtr<JSFunction>::make();
    is_nan_fn->set_name(std::string("isNaN"));
    is_nan_fn->set_native_fn([](Value /*this_val*/, std::vector<Value> args, bool) -> EvalResult {
        double n = to_number_double(args.empty() ? Value::undefined() : args[0]);
        return EvalResult::ok(Value::boolean(std::isnan(n)));
    });
    gc_heap_.Register(is_nan_fn.get());
    global_env_->define_initialized("isNaN");
    global_env_->set("isNaN", Value::object(ObjectPtr(is_nan_fn)));

    // Build global isFinite (does ToNumber conversion)
    auto is_finite_fn = RcPtr<JSFunction>::make();
    is_finite_fn->set_name(std::string("isFinite"));
    is_finite_fn->set_native_fn([](Value /*this_val*/, std::vector<Value> args, bool) -> EvalResult {
        double n = to_number_double(args.empty() ? Value::undefined() : args[0]);
        return EvalResult::ok(Value::boolean(std::isfinite(n)));
    });
    gc_heap_.Register(is_finite_fn.get());
    global_env_->define_initialized("isFinite");
    global_env_->set("isFinite", Value::object(ObjectPtr(is_finite_fn)));

    // ---- Number constructor ----

    number_constructor_ = RcPtr<JSFunction>::make();
    number_constructor_->set_name(std::string("Number"));
    number_constructor_->set_native_fn([](Value /*this_val*/, std::vector<Value> args,
                                          bool /*is_new*/) -> EvalResult {
        double n = args.empty() ? 0.0 : to_number_double(args[0]);
        return EvalResult::ok(Value::number(n));
    });

    // Number.isNaN (no ToNumber conversion)
    auto num_is_nan_fn = RcPtr<JSFunction>::make();
    num_is_nan_fn->set_name(std::string("isNaN"));
    num_is_nan_fn->set_native_fn([](Value /*this_val*/, std::vector<Value> args, bool) -> EvalResult {
        if (args.empty() || !args[0].is_number()) return EvalResult::ok(Value::boolean(false));
        return EvalResult::ok(Value::boolean(std::isnan(args[0].as_number())));
    });
    number_constructor_->set_property("isNaN", Value::object(ObjectPtr(num_is_nan_fn)));

    // Number.isFinite (no ToNumber conversion)
    auto num_is_finite_fn = RcPtr<JSFunction>::make();
    num_is_finite_fn->set_name(std::string("isFinite"));
    num_is_finite_fn->set_native_fn([](Value /*this_val*/, std::vector<Value> args, bool) -> EvalResult {
        if (args.empty() || !args[0].is_number()) return EvalResult::ok(Value::boolean(false));
        return EvalResult::ok(Value::boolean(std::isfinite(args[0].as_number())));
    });
    number_constructor_->set_property("isFinite", Value::object(ObjectPtr(num_is_finite_fn)));

    // Number.isInteger
    auto num_is_integer_fn = RcPtr<JSFunction>::make();
    num_is_integer_fn->set_name(std::string("isInteger"));
    num_is_integer_fn->set_native_fn([](Value /*this_val*/, std::vector<Value> args, bool) -> EvalResult {
        if (args.empty() || !args[0].is_number()) return EvalResult::ok(Value::boolean(false));
        double n = args[0].as_number();
        if (std::isnan(n) || std::isinf(n)) return EvalResult::ok(Value::boolean(false));
        return EvalResult::ok(Value::boolean(std::trunc(n) == n));
    });
    number_constructor_->set_property("isInteger", Value::object(ObjectPtr(num_is_integer_fn)));

    // Number.parseInt === global parseInt (same object)
    number_constructor_->set_property("parseInt", parse_int_val);

    // Number.prototype
    number_prototype_ = RcPtr<JSObject>::make();
    number_prototype_->set_proto(object_prototype_);
    gc_heap_.Register(number_prototype_.get());
    number_constructor_->set_prototype_obj(RcPtr<JSObject>(number_prototype_));
    number_constructor_->set_property("prototype", Value::object(ObjectPtr(number_prototype_)));

    gc_heap_.Register(number_constructor_.get());
    global_env_->define_initialized("Number");
    global_env_->set("Number", Value::object(ObjectPtr(number_constructor_)));

    // ---- Math object ----

    // Initialize PRNG state
    math_random_state_ = static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    if (math_random_state_ == 0) math_random_state_ = 1;

    math_obj_ = RcPtr<JSObject>::make();
    math_obj_->set_proto(object_prototype_);

    math_obj_->set_property("PI", Value::number(M_PI));
    math_obj_->set_property("E", Value::number(M_E));

    // Math.floor
    auto math_floor_fn = RcPtr<JSFunction>::make();
    math_floor_fn->set_native_fn([](Value, std::vector<Value> args, bool) -> EvalResult {
        double x = args.empty() ? std::numeric_limits<double>::quiet_NaN()
                                : to_number_double(args[0]);
        return EvalResult::ok(Value::number(std::floor(x)));
    });
    math_obj_->set_property("floor", Value::object(ObjectPtr(math_floor_fn)));

    // Math.ceil
    auto math_ceil_fn = RcPtr<JSFunction>::make();
    math_ceil_fn->set_native_fn([](Value, std::vector<Value> args, bool) -> EvalResult {
        double x = args.empty() ? std::numeric_limits<double>::quiet_NaN()
                                : to_number_double(args[0]);
        return EvalResult::ok(Value::number(std::ceil(x)));
    });
    math_obj_->set_property("ceil", Value::object(ObjectPtr(math_ceil_fn)));

    // Math.round: spec tie-breaking: x.5 rounds toward +Infinity; -0.5 → -0
    auto math_round_fn = RcPtr<JSFunction>::make();
    math_round_fn->set_native_fn([](Value, std::vector<Value> args, bool) -> EvalResult {
        double x = args.empty() ? std::numeric_limits<double>::quiet_NaN()
                                : to_number_double(args[0]);
        if (std::isnan(x) || std::isinf(x) || x == 0.0) return EvalResult::ok(Value::number(x));
        double r = std::floor(x + 0.5);
        // -0.5 case: x < 0 and result is 0 → return -0
        if (r == 0.0 && x < 0.0) return EvalResult::ok(Value::number(-0.0));
        return EvalResult::ok(Value::number(r));
    });
    math_obj_->set_property("round", Value::object(ObjectPtr(math_round_fn)));

    // Math.abs
    auto math_abs_fn = RcPtr<JSFunction>::make();
    math_abs_fn->set_native_fn([](Value, std::vector<Value> args, bool) -> EvalResult {
        double x = args.empty() ? std::numeric_limits<double>::quiet_NaN()
                                : to_number_double(args[0]);
        return EvalResult::ok(Value::number(std::abs(x)));
    });
    math_obj_->set_property("abs", Value::object(ObjectPtr(math_abs_fn)));

    // Math.max
    auto math_max_fn = RcPtr<JSFunction>::make();
    math_max_fn->set_native_fn([](Value, std::vector<Value> args, bool) -> EvalResult {
        double result = -std::numeric_limits<double>::infinity();
        for (auto& arg : args) {
            double v = to_number_double(arg);
            if (std::isnan(v)) return EvalResult::ok(Value::number(v));
            // ES: n > highest, or n is +0 and highest is -0
            if (v > result || (v == 0.0 && !std::signbit(v) && std::signbit(result))) result = v;
        }
        return EvalResult::ok(Value::number(result));
    });
    math_obj_->set_property("max", Value::object(ObjectPtr(math_max_fn)));

    // Math.min
    auto math_min_fn = RcPtr<JSFunction>::make();
    math_min_fn->set_native_fn([](Value, std::vector<Value> args, bool) -> EvalResult {
        double result = std::numeric_limits<double>::infinity();
        for (auto& arg : args) {
            double v = to_number_double(arg);
            if (std::isnan(v)) return EvalResult::ok(Value::number(v));
            // ES: n < lowest, or n is -0
            if (v < result || (v == 0.0 && std::signbit(v))) result = v;
        }
        return EvalResult::ok(Value::number(result));
    });
    math_obj_->set_property("min", Value::object(ObjectPtr(math_min_fn)));

    // Math.pow
    auto math_pow_fn = RcPtr<JSFunction>::make();
    math_pow_fn->set_native_fn([](Value, std::vector<Value> args, bool) -> EvalResult {
        double base = args.size() >= 1 ? to_number_double(args[0])
                                       : std::numeric_limits<double>::quiet_NaN();
        double exp = args.size() >= 2 ? to_number_double(args[1])
                                      : std::numeric_limits<double>::quiet_NaN();
        return EvalResult::ok(Value::number(std::pow(base, exp)));
    });
    math_obj_->set_property("pow", Value::object(ObjectPtr(math_pow_fn)));

    // Math.sqrt
    auto math_sqrt_fn = RcPtr<JSFunction>::make();
    math_sqrt_fn->set_native_fn([](Value, std::vector<Value> args, bool) -> EvalResult {
        double x = args.empty() ? std::numeric_limits<double>::quiet_NaN()
                                : to_number_double(args[0]);
        return EvalResult::ok(Value::number(std::sqrt(x)));
    });
    math_obj_->set_property("sqrt", Value::object(ObjectPtr(math_sqrt_fn)));

    // Math.log
    auto math_log_fn = RcPtr<JSFunction>::make();
    math_log_fn->set_native_fn([](Value, std::vector<Value> args, bool) -> EvalResult {
        double x = args.empty() ? std::numeric_limits<double>::quiet_NaN()
                                : to_number_double(args[0]);
        return EvalResult::ok(Value::number(std::log(x)));
    });
    math_obj_->set_property("log", Value::object(ObjectPtr(math_log_fn)));

    // Math.trunc
    auto math_trunc_fn = RcPtr<JSFunction>::make();
    math_trunc_fn->set_native_fn([](Value, std::vector<Value> args, bool) -> EvalResult {
        double x = args.empty() ? std::numeric_limits<double>::quiet_NaN()
                                : to_number_double(args[0]);
        return EvalResult::ok(Value::number(std::trunc(x)));
    });
    math_obj_->set_property("trunc", Value::object(ObjectPtr(math_trunc_fn)));

    // Math.sign
    auto math_sign_fn = RcPtr<JSFunction>::make();
    math_sign_fn->set_native_fn([](Value, std::vector<Value> args, bool) -> EvalResult {
        double x = args.empty() ? std::numeric_limits<double>::quiet_NaN()
                                : to_number_double(args[0]);
        if (std::isnan(x)) return EvalResult::ok(Value::number(x));
        if (x == 0.0) return EvalResult::ok(Value::number(x));  // preserves +0/-0
        return EvalResult::ok(Value::number(x > 0.0 ? 1.0 : -1.0));
    });
    math_obj_->set_property("sign", Value::object(ObjectPtr(math_sign_fn)));

    // Math.random (xorshift64*)
    auto math_random_fn = RcPtr<JSFunction>::make();
    math_random_fn->set_native_fn([this](Value, std::vector<Value> /*args*/, bool) -> EvalResult {
        math_random_state_ ^= math_random_state_ >> 12;
        math_random_state_ ^= math_random_state_ << 25;
        math_random_state_ ^= math_random_state_ >> 27;
        uint64_t r = math_random_state_ * 0x2545F4914F6CDD1DULL;
        double result = static_cast<double>(r >> 11) / static_cast<double>(1ULL << 53);
        return EvalResult::ok(Value::number(result));
    });
    math_obj_->set_property("random", Value::object(ObjectPtr(math_random_fn)));

    gc_heap_.Register(math_obj_.get());
    global_env_->define("Math", VarKind::Const);
    global_env_->initialize("Math", Value::object(ObjectPtr(math_obj_)));

    // Register the global environment with GcHeap so user-created closures reachable
    // from it are treated as roots and not swept.
    gc_heap_.Register(global_env_.get());
}

Interpreter::Interpreter() {
    init_runtime();
}

// ---- Promise helpers ----

RcPtr<JSPromise> Interpreter::promise_resolve(Value value) {
    // If value is already a Promise, return it directly
    if (value.is_object() && value.as_object_raw() &&
        value.as_object_raw()->object_kind() == ObjectKind::kPromise) {
        return RcPtr<JSPromise>(static_cast<JSPromise*>(value.as_object_raw()));
    }
    auto p = RcPtr<JSPromise>::make();
    gc_heap_.Register(p.get());
    p->Fulfill(std::move(value), job_queue_);
    return p;
}

void Interpreter::execute_reaction_job(ReactionJob job) {
    // Hold strong references to keep handler/capability/arg alive throughout execution.
    Value handler = std::move(job.handler);
    Value capability_val = std::move(job.capability);
    Value arg = std::move(job.arg);
    bool is_fulfill = job.is_fulfill;

    // Get capability promise (hold RcPtr to keep it alive)
    RcPtr<JSPromise> cap_rc;
    if (capability_val.is_object() && capability_val.as_object_raw() &&
        capability_val.as_object_raw()->object_kind() == ObjectKind::kPromise) {
        cap_rc = RcPtr<JSPromise>(static_cast<JSPromise*>(capability_val.as_object_raw()));
    }

    bool handler_is_fn = handler.is_object() && handler.as_object_raw() &&
                         handler.as_object_raw()->object_kind() == ObjectKind::kFunction;

    if (!handler_is_fn) {
        // Identity / thrower reaction
        if (cap_rc) {
            if (is_fulfill) {
                cap_rc->Fulfill(arg, job_queue_);
            } else {
                cap_rc->Reject(arg, job_queue_);
            }
        }
        return;
    }

    // Call handler(arg)
    std::vector<Value> handler_args = {arg};
    auto result = call_function_val(handler, Value::undefined(),
                                    std::span<Value>(handler_args.data(), handler_args.size()));

    if (!cap_rc) return;

    if (result.is_ok()) {
        Value ret = result.value();
        if (ret.is_object() && ret.as_object_raw() &&
            ret.as_object_raw()->object_kind() == ObjectKind::kPromise) {
            auto* inner = static_cast<JSPromise*>(ret.as_object_raw());
            // P2-F: self-referential resolution must be rejected with TypeError
            if (inner == cap_rc.get()) {
                cap_rc->Reject(make_error_value(NativeErrorType::kTypeError,
                    "Chaining cycle detected for promise"), job_queue_);
            } else {
                // Adopt the returned promise's state via PerformThen
                auto inner_rc = RcPtr<JSPromise>(inner);
                // Capture cap_rc by value to keep it alive in the lambdas
                auto fulfill_cap = RcPtr<JSFunction>::make();
                fulfill_cap->set_native_fn([this, cap_rc](Value, std::vector<Value> a, bool) mutable -> EvalResult {
                    Value v = a.empty() ? Value::undefined() : a[0];
                    cap_rc->Fulfill(v, job_queue_);
                    return EvalResult::ok(Value::undefined());
                });
                gc_heap_.Register(fulfill_cap.get());
                auto reject_cap = RcPtr<JSFunction>::make();
                reject_cap->set_native_fn([this, cap_rc](Value, std::vector<Value> a, bool) mutable -> EvalResult {
                    Value r = a.empty() ? Value::undefined() : a[0];
                    cap_rc->Reject(r, job_queue_);
                    return EvalResult::ok(Value::undefined());
                });
                gc_heap_.Register(reject_cap.get());
                JSPromise::PerformThen(inner_rc,
                    Value::object(ObjectPtr(fulfill_cap)),
                    Value::object(ObjectPtr(reject_cap)),
                    job_queue_);
            }
        } else {
            cap_rc->Fulfill(ret, job_queue_);
        }
    } else {
        // Handler threw: extract throw value and reject cap
        Value thrown_val;
        if (result.error().message() == kPendingThrowSentinel && pending_throw_.has_value()) {
            thrown_val = std::move(*pending_throw_);
            pending_throw_ = std::nullopt;
        } else {
            thrown_val = Value::string(result.error().message());
        }
        cap_rc->Reject(std::move(thrown_val), job_queue_);
    }
}

void Interpreter::drain_job_queue() {
    job_queue_.DrainAll([this](ReactionJob job) {
        execute_reaction_job(std::move(job));
    });
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
        std::string s = v.as_string();
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
    } else if (std::holds_alternative<AsyncFunctionDeclaration>(stmt.v)) {
        // P2-C: async function declarations are hoisted and immediately assigned,
        // mirroring the VM's behavior of emitting kMakeFunction+kSetVar at function entry.
        const auto& afdecl = std::get<AsyncFunctionDeclaration>(stmt.v);
        var_target.define_function(afdecl.name);
        Value async_fn_val = make_async_function_value(afdecl.name, afdecl.params, afdecl.body, current_env_);
        var_target.set(afdecl.name, async_fn_val);
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

void Interpreter::hoist_module_vars(const std::vector<StmtNode>& stmts, Environment& module_env) {
    for (const auto& stmt : stmts) {
        if (const auto* exp = std::get_if<ExportNamedDeclaration>(&stmt.v)) {
            // export let/const/var/function：Binding 已由 Link 阶段建立，跳过
            (void)exp;
        } else if (const auto* def = std::get_if<ExportDefaultDeclaration>(&stmt.v)) {
            // export default function foo() {}：在模块作用域建立 foo 的 var binding
            if (def->local_name.has_value() && module_env.find_local(*def->local_name) == nullptr) {
                module_env.define_function(*def->local_name);
            }
        } else if (std::holds_alternative<FunctionDeclaration>(stmt.v)) {
            const auto& fd = std::get<FunctionDeclaration>(stmt.v);
            Binding* b = module_env.find_local(fd.name);
            if (b == nullptr) {
                module_env.define_function(fd.name);
            } else if (!b->cell->initialized) {
                // Link 阶段为 export { fn } 建立的 Binding，函数声明提升后无 TDZ
                b->initialized = true;
                b->cell->initialized = true;
            }
        } else if (std::holds_alternative<AsyncFunctionDeclaration>(stmt.v)) {
            const auto& afd = std::get<AsyncFunctionDeclaration>(stmt.v);
            Binding* b = module_env.find_local(afd.name);
            if (b == nullptr) {
                module_env.define_function(afd.name);
            } else if (!b->cell->initialized) {
                b->initialized = true;
                b->cell->initialized = true;
            }
        } else if (std::holds_alternative<VariableDeclaration>(stmt.v)) {
            const auto& vd = std::get<VariableDeclaration>(stmt.v);
            if (vd.kind == VarKind::Var) {
                Binding* b = module_env.find_local(vd.name);
                if (b == nullptr) {
                    module_env.define_initialized(vd.name);
                } else if (!b->initialized && !b->cell->initialized) {
                    // Link 阶段建立的 Binding 是 TDZ，但 var 无 TDZ
                    b->initialized = true;
                    b->cell->initialized = true;
                }
            } else {
                // let/const 非导出：需要在模块环境中建立 TDZ binding
                if (module_env.find_local(vd.name) == nullptr) {
                    module_env.define(vd.name, vd.kind);
                }
            }
        }
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
    // Drain microtasks before GC: all synchronous code has completed.
    drain_job_queue();

    // Re-read the last expression variable after DrainAll to pick up microtask side effects.
    // Only re-reads if the last statement is a simple identifier expression (e.g., `result;`).
    if (!has_result && !program.body.empty()) {
        const auto& last_stmt = program.body.back();
        if (const auto* es = std::get_if<ExpressionStatement>(&last_stmt.v)) {
            if (const auto* id = std::get_if<Identifier>(&es->expr.v)) {
                if (id->name != "undefined") {
                    auto reeval = eval_identifier(*id);
                    if (reeval.is_ok()) {
                        final_result = EvalResult::ok(reeval.value());
                    }
                }
            }
        }
    }

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
        add_obj(promise_prototype_.get());
        add_obj(string_prototype_.get());
        add_obj(math_obj_.get());
        add_obj(number_prototype_.get());
        add_obj(object_constructor_.get());
        add_obj(number_constructor_.get());
        for (auto& ep : error_protos_) add_obj(ep.get());
        add_val(current_this_);
        if (pending_throw_.has_value()) add_val(*pending_throw_);
        // Include the result value so it is not swept
        if (final_result.is_ok()) add_val(final_result.value());
        // Include job queue roots
        std::vector<Value> jq_vals;
        job_queue_.CollectRoots(jq_vals);
        for (const auto& v : jq_vals) add_val(v);

        gc_heap_.Collect(roots);
    }

    // Cleanup: break remaining RC cycles from runtime objects.
    global_env_->clear_function_bindings();
    object_prototype_->clear_function_properties();
    if (array_prototype_) array_prototype_->clear_function_properties();
    if (function_prototype_) function_prototype_->clear_function_properties();
    if (promise_prototype_) promise_prototype_->clear_function_properties();
    if (string_prototype_) string_prototype_->clear_function_properties();
    if (math_obj_) math_obj_->clear_function_properties();
    if (number_prototype_) number_prototype_->clear_function_properties();
    if (object_constructor_) object_constructor_->clear_own_properties();
    if (number_constructor_) number_constructor_->clear_own_properties();

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
            [this](const AsyncFunctionDeclaration& s) { return eval_async_function_decl(s); },
            [this](const ThrowStatement& s) { return eval_throw_stmt(s); },
            [this](const TryStatement& s) { return eval_try_stmt(s); },
            [this](const BreakStatement& s) { return eval_break_stmt(s); },
            [this](const ContinueStatement& s) { return eval_continue_stmt(s); },
            [this](const LabeledStatement& s) { return eval_labeled_stmt(s); },
            [this](const ForStatement& s) { return eval_for_stmt(s); },
            [](const ImportDeclaration&) -> StmtResult {
                // Link 阶段已处理，执行时 no-op
                return StmtResult::ok(Completion::normal(Value::undefined()));
            },
            [this](const ExportNamedDeclaration& s) -> StmtResult {
                if (s.source.has_value()) {
                    // re-export：no-op（Link 阶段已将 Cell 共享）
                    return StmtResult::ok(Completion::normal(Value::undefined()));
                }
                if (s.declaration) {
                    if (const auto* vd = std::get_if<VariableDeclaration>(&s.declaration->v)) {
                        if (vd->kind != VarKind::Var) {
                            // export let/const：Binding 已由 Link 阶段建立（共享 Cell），
                            // 跳过 define，直接执行初始化
                            if (vd->init.has_value()) {
                                auto init_result = eval_expr(vd->init.value());
                                if (!init_result.is_ok()) return StmtResult::err(init_result.error());
                                auto set_result = current_env_->initialize(vd->name, init_result.value());
                                if (!set_result.is_ok()) return StmtResult::err(set_result.error());
                            } else {
                                // 无初始值：初始化为 undefined
                                auto set_result = current_env_->initialize(vd->name, Value::undefined());
                                if (!set_result.is_ok()) return StmtResult::err(set_result.error());
                            }
                            return StmtResult::ok(Completion::normal(Value::undefined()));
                        }
                    }
                    // export var/function：正常执行声明
                    return eval_stmt(*s.declaration);
                }
                // export { x, x as y }（无 source，无 declaration）
                // no-op：模块体执行完毕后在 exec_module_body 中统一写入 Cell
                return StmtResult::ok(Completion::normal(Value::undefined()));
            },
            [this](const ExportDefaultDeclaration& s) -> StmtResult {
                // 求值 expression，写入当前模块的 "default" Cell
                auto val = eval_expr(*s.expression);
                if (!val.is_ok()) return StmtResult::err(val.error());
                // 通过 current_module_ 找到 "default" Cell 并写入
                if (current_module_) {
                    Cell* cell = current_module_->find_export("default");
                    if (cell) {
                        cell->value = val.value();
                        cell->initialized = true;
                    }
                }
                // export default function foo() {}：同时在模块作用域绑定 foo
                if (s.local_name.has_value()) {
                    auto set_result = current_env_->set(*s.local_name, val.value());
                    if (!set_result.is_ok()) return StmtResult::err(set_result.error());
                }
                return StmtResult::ok(Completion::normal(Value::undefined()));
            },
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
        // 若 Link 阶段已建立共享 Cell Binding（export let/const 或 export { x }），跳过 define
        if (current_env_->find_local(decl.name) == nullptr) {
            current_env_->define(decl.name, decl.kind);
        }
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
            [this](const AwaitExpression& e) { return eval_await_expr(e); },
            [this](const AsyncFunctionExpression& e) { return eval_async_function_expr(e); },
            [this](const MetaProperty& /*e*/) {
                // import.meta 是词法绑定：优先使用当前函数的定义模块
                ModuleRecord* mod = nullptr;
                if (current_function_ && current_function_->defining_module()) {
                    mod = current_function_->defining_module();
                } else {
                    mod = current_module_;
                }
                if (mod && mod->meta_obj) {
                    return EvalResult::ok(Value::object(ObjectPtr(mod->meta_obj)));
                }
                return EvalResult::ok(Value::undefined());
            },
            [this](const ImportCallExpression& e) { return eval_import_call(e); },
        },
        expr.v);
}

EvalResult Interpreter::eval_array_expr(const ArrayExpression& expr) {
    auto arr = RcPtr<JSObject>::make(ObjectKind::kArray);
    gc_heap_.Register(arr.get());
    arr->set_proto(array_prototype_);
    for (const auto& elem_opt : expr.elements) {
        if (elem_opt.has_value()) {
            auto v = eval_expr(**elem_opt);
            if (!v.is_ok()) return v;
            arr->elements_[arr->array_length_] = v.value();
        }
        // hole: increment length but do not write to elements_ (true sparse hole)
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
        std::string sa = a.as_string();
        double n = sa.empty() ? 0.0 : std::strtod(sa.c_str(), &end);
        if (!sa.empty() && (end == sa.c_str() || *end != '\0')) {
            n = std::numeric_limits<double>::quiet_NaN();
        }
        return abstract_eq(Value::number(n), b);
    }
    if (a.is_number() && b.is_string()) {
        char* end = nullptr;
        std::string sb = b.as_string();
        double n = sb.empty() ? 0.0 : std::strtod(sb.c_str(), &end);
        if (!sb.empty() && (end == sb.c_str() || *end != '\0')) {
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

    // String primitive: handle length and string_prototype_ methods
    if (obj_val.is_string()) {
        auto key_result = eval_expr(*expr.property);
        if (!key_result.is_ok()) return key_result;
        std::string key = to_string_val(key_result.value());
        if (key == "length") {
            return EvalResult::ok(Value::number(static_cast<double>(utf8_cp_len(obj_val.js_string_raw()))));
        }
        if (string_prototype_) return EvalResult::ok(string_prototype_->get_property(key));
        return EvalResult::ok(Value::undefined());
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
        // Check own_properties_ first (covers explicitly set "prototype" like Promise.prototype)
        Value own = fn->get_property(key);
        if (!own.is_undefined()) return EvalResult::ok(own);
        if (key == "prototype") {
            // Fall back to the implicit F.prototype object
            const auto& proto = fn->prototype_obj();
            return EvalResult::ok(proto ? Value::object(ObjectPtr(proto)) : Value::undefined());
        }
        // Fall back to Function.prototype
        if (function_prototype_) return EvalResult::ok(function_prototype_->get_property(key));
        return EvalResult::ok(Value::undefined());
    }
    if (raw_obj->object_kind() == ObjectKind::kPromise) {
        // Promise property lookup: check promise_prototype_
        if (promise_prototype_) return EvalResult::ok(promise_prototype_->get_property(key));
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
    fn->set_defining_module(current_module_);

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

    JSFunction* saved_function = current_function_;
    current_function_ = fn.get();

    Value result_val = Value::undefined();
    for (const auto& stmt : *fn->body()) {
        auto stmt_result = eval_stmt(stmt);
        if (!stmt_result.is_ok()) {
            current_function_ = saved_function;
            return stmt_result;
        }
        const Completion& c = stmt_result.completion();
        if (c.is_return() || c.is_throw()) {
            current_function_ = saved_function;
            return stmt_result;  // preserve kReturn/kThrow so callers can distinguish
        }
        result_val = c.value;
    }
    current_function_ = saved_function;
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
        if (r.error().message() == kAsyncSuspendSentinel) {
            return StmtResult::err(r.error());
        }
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
    // Exception: kAsyncSuspendSentinel must be propagated as-is (async suspension).
    if (!try_result.is_ok()) {
        if (try_result.error().message() == kAsyncSuspendSentinel) {
            return try_result;
        }
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
            // Exception: kAsyncSuspendSentinel must be propagated as-is.
            if (!try_result.is_ok()) {
                if (try_result.error().message() == kAsyncSuspendSentinel) {
                    return try_result;
                }
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
                current_env_ = old_env;
                if (test_r.error().message() == kAsyncSuspendSentinel) {
                    return StmtResult::err(test_r.error());
                }
                Value thrown = extract_throw_value(pending_throw_, test_r.error().message(),
                                                   kPendingThrowSentinel);
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
                current_env_ = old_env;
                if (update_r.error().message() == kAsyncSuspendSentinel) {
                    return StmtResult::err(update_r.error());
                }
                Value thrown = extract_throw_value(pending_throw_, update_r.error().message(),
                                                   kPendingThrowSentinel);
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

        if (this_val.is_string()) {
            if (string_prototype_) {
                callee_val = string_prototype_->get_property(key);
            } else {
                callee_val = Value::undefined();
            }
        } else if (!this_val.is_object()) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "Cannot read properties of " + to_string_val(this_val));
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        } else {
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
        } else if (obj_ptr->object_kind() == ObjectKind::kPromise) {
            if (promise_prototype_) {
                callee_val = promise_prototype_->get_property(key);
            } else {
                callee_val = Value::undefined();
            }
        } else {
            callee_val = Value::undefined();
        }
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

// ============================================================
// ESM 模块执行
// ============================================================

EvalResult Interpreter::exec_module(const std::string& entry_path) {
    init_runtime();

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
    if (!eval_result.is_ok()) {
        return eval_result;
    }

    // 收集最终结果：入口模块的最后一个表达式值（通过 exec_module_body 的返回值）
    // 已在 evaluate_module 中执行，返回 eval_result（模块执行结果）
    EvalResult final_result = eval_result;

    // 执行剩余微任务（async function 调用可能产生 pending microtasks）
    drain_job_queue();

    // 微任务执行后刷新最后一条简单标识符表达式的值（与 exec() 的 drain 后刷新逻辑对称）
    if (final_result.is_ok() && entry_mod->module_env && !entry_mod->ast.body.empty()) {
        const auto& last_stmt = entry_mod->ast.body.back();
        if (const auto* es = std::get_if<ExpressionStatement>(&last_stmt.v)) {
            if (const auto* id = std::get_if<Identifier>(&es->expr.v)) {
                if (id->name != "undefined") {
                    auto reeval = entry_mod->module_env->get(id->name);
                    if (reeval.is_ok()) {
                        final_result = EvalResult::ok(reeval.value());
                    }
                }
            }
        }
    }

    // GC
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
        add_obj(promise_prototype_.get());
        add_obj(string_prototype_.get());
        add_obj(math_obj_.get());
        add_obj(number_prototype_.get());
        add_obj(object_constructor_.get());
        add_obj(number_constructor_.get());
        for (auto& ep : error_protos_) add_obj(ep.get());
        add_val(current_this_);
        if (pending_throw_.has_value()) add_val(*pending_throw_);
        if (final_result.is_ok()) add_val(final_result.value());
        // 将 job_queue_ 里的对象也加入 roots，避免 GC 误回收未执行的微任务引用的对象
        std::vector<Value> jq_vals;
        job_queue_.CollectRoots(jq_vals);
        for (const auto& v : jq_vals) add_val(v);
        module_loader_.TraceRoots(gc_heap_);

        gc_heap_.Collect(roots);
    }

    global_env_->clear_function_bindings();
    object_prototype_->clear_function_properties();
    if (array_prototype_) array_prototype_->clear_function_properties();
    if (function_prototype_) function_prototype_->clear_function_properties();
    if (promise_prototype_) promise_prototype_->clear_function_properties();
    if (string_prototype_) string_prototype_->clear_function_properties();
    if (math_obj_) math_obj_->clear_function_properties();
    if (number_prototype_) number_prototype_->clear_function_properties();
    if (object_constructor_) object_constructor_->clear_own_properties();
    if (number_constructor_) number_constructor_->clear_own_properties();
    // 清理所有模块环境中的函数引用（打破 module_env ↔ JSFunction 循环引用）
    module_loader_.ClearModuleEnvs();
    module_loader_.Clear();

    // 将 final_result 中的对象从 GcHeap 摘除，避免 Interpreter 析构后 gc_heap_ 失效
    // 导致调用者持有的 EvalResult 析构时触发 Unregister 崩溃。
    if (final_result.is_ok() && final_result.value().is_object()) {
        RcObject* raw = final_result.value().as_object_raw();
        if (raw && raw->gc_heap_) {
            gc_heap_.Unregister(raw);
            raw->gc_heap_ = nullptr;  // 防止析构时再次调用 Unregister
        }
    }

    return final_result;
}

EvalResult Interpreter::link_module(ModuleRecord& mod) {
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

    // 创建 import.meta 对象（[[Prototype]] = null）
    auto meta = RcPtr<JSObject>::make();
    gc_heap_.Register(meta.get());
    meta->set_property("url", Value::string(mod.specifier));
    mod.meta_obj = std::move(meta);

    // 建立导出变量 Binding（共享 Cell）
    for (const auto& stmt : mod.ast.body) {
        if (const auto* exp = std::get_if<ExportNamedDeclaration>(&stmt.v)) {
            if (exp->source.has_value()) continue;  // re-export，跳过
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
                } else if (const auto* afd = std::get_if<AsyncFunctionDeclaration>(&exp->declaration->v)) {
                    name = afd->name;
                    is_mutable = true;
                    initialized = true;  // async function 声明提升，无 TDZ
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
                // Load 阶段已为 local_name 预分配 Cell，并将 exports[export_name] 指向同一 Cell
                // Link 阶段将 local_name 的 Cell 注入 module_env，建立共享 Binding
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
            // 找到对应依赖
            RcPtr<ModuleRecord> dep_mod;
            for (const auto& dep : mod.dependencies) {
                // 通过 requested_modules 的顺序对应 dependencies
                // 这里需要按 specifier 匹配
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
                if (spec.is_namespace) continue;  // 不支持 namespace import
                const std::string& imported_name = spec.imported_name;
                const std::string& local_name = spec.local_name;

                // 查找导出 Cell（直接导出或 re-export）
                Cell* cell = dep_mod->find_export(imported_name);
                if (cell == nullptr) {
                    // 尝试 re-export 解析
                    for (const auto& re : dep_mod->re_exports) {
                        if (re.export_name == imported_name) {
                            // 找到 re-export 的来源模块
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

EvalResult Interpreter::evaluate_module(ModuleRecord& mod) {
    if (mod.status == ModuleStatus::kEvaluated) {
        return EvalResult::ok(Value::undefined());
    }
    if (mod.status == ModuleStatus::kErrored) {
        // 错误缓存：直接重抛
        if (mod.eval_exception.has_value()) {
            pending_throw_ = mod.eval_exception;
            return EvalResult::err(Error{ErrorKind::Runtime, kPendingThrowSentinel});
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
        // 缓存错误（目前只缓存 pending_throw_，不缓存 C++ Error）
        if (pending_throw_.has_value()) {
            mod.eval_exception = pending_throw_;
        }
        return body_result;
    }

    mod.status = ModuleStatus::kEvaluated;
    return body_result;
}

EvalResult Interpreter::exec_module_body(ModuleRecord& mod) {
    // 切换到模块环境，this = undefined
    ScopeGuard guard(*this, mod.module_env, mod.module_env, Value::undefined());

    // 保存并设置 current_module_
    ModuleRecord* saved_module = current_module_;
    current_module_ = &mod;

    // 变量提升：只提升非导出的 var 和 function（export let/const/var/function 的 Binding
    // 已由 Link 阶段通过 define_binding_with_cell 建立，不重复 define）
    hoist_module_vars(mod.ast.body, *mod.module_env);

    // function/async function 提升：在模块体执行前将函数值写入 Binding
    // （与 VM compiler 在函数体入口 emit kMakeFunction+kSetVar 的行为对齐）
    for (const auto& stmt : mod.ast.body) {
        if (const auto* exp = std::get_if<ExportNamedDeclaration>(&stmt.v)) {
            if (!exp->source.has_value() && exp->declaration) {
                if (const auto* fd = std::get_if<FunctionDeclaration>(&exp->declaration->v)) {
                    Value fn_val = make_function_value(
                        fd->name, fd->params, fd->body, current_env_, false);
                    current_env_->set(fd->name, fn_val);
                } else if (const auto* afd = std::get_if<AsyncFunctionDeclaration>(&exp->declaration->v)) {
                    Value fn_val = make_async_function_value(
                        afd->name, afd->params, afd->body, current_env_);
                    current_env_->set(afd->name, fn_val);
                }
            }
        } else if (const auto* afd = std::get_if<AsyncFunctionDeclaration>(&stmt.v)) {
            // 顶层非导出 async function 声明：P2-C 中 eval_async_function_decl 是 no-op，
            // 需在此处提升赋值（与 hoist_vars_stmt 对普通 exec() 的处理对齐）
            Value fn_val = make_async_function_value(
                afd->name, afd->params, afd->body, current_env_);
            current_env_->set(afd->name, fn_val);
        }
    }

    // 执行模块体语句
    Value last = Value::undefined();
    EvalResult final_result = EvalResult::ok(Value::undefined());
    bool has_error = false;
    bool tla_suspended = false;
    size_t tla_suspend_index = 0;

    const auto& stmts = mod.ast.body;
    for (size_t i = 0; i < stmts.size(); ++i) {
        auto result = eval_stmt(stmts[i]);
        if (!result.is_ok()) {
            const std::string& emsg = result.error().message();
            // TLA: 顶层 await 挂起
            if (emsg == kAsyncSuspendSentinel) {
                tla_suspended = true;
                tla_suspend_index = i;
                break;
            }
            // C++ 错误
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
                // 重新设置 pending_throw_ 以便错误缓存
                pending_throw_ = thrown;
                final_result = EvalResult::err(Error(ErrorKind::Runtime, name + ": " + message));
            } else {
                final_result = EvalResult::err(result.error());
            }
            has_error = true;
            break;
        }
        const Completion& c = result.completion();
        if (c.is_throw()) {
            const Value& thrown = c.value;
            pending_throw_ = thrown;
            if (thrown.is_object()) {
                RcObject* raw = thrown.as_object_raw();
                if (raw && raw->object_kind() == ObjectKind::kOrdinary) {
                    auto* obj = static_cast<JSObject*>(raw);
                    Value n = obj->get_property("name");
                    Value m = obj->get_property("message");
                    std::string name = n.is_string() ? n.as_string() : "Error";
                    std::string message = m.is_string() ? m.as_string() : "";
                    final_result = EvalResult::err(Error(ErrorKind::Runtime, name + ": " + message));
                } else {
                    final_result = EvalResult::err(Error(ErrorKind::Runtime, to_string_val(thrown)));
                }
            } else {
                final_result = EvalResult::err(Error(ErrorKind::Runtime, to_string_val(thrown)));
            }
            has_error = true;
            break;
        }
        if (c.is_normal()) {
            last = c.value;
        }
    }

    // TLA: 顶层 await 挂起，通过 run_async_body 机制异步执行剩余语句
    if (tla_suspended) {
        // 将 mod.ast.body 包装为 shared_ptr（no-op deleter，生命周期由 ModuleRecord 管理）
        auto body_ptr = std::shared_ptr<std::vector<StmtNode>>(
            const_cast<std::vector<StmtNode>*>(&stmts),
            [](std::vector<StmtNode>*) {});

        auto outer_promise = RcPtr<JSPromise>::make();
        gc_heap_.Register(outer_promise.get());

        // run_async_body 会切换到 mod.module_env，继续从 tla_suspend_index 执行
        run_async_body(body_ptr, tla_suspend_index, mod.module_env, Value::undefined(),
                       outer_promise);

        // 等待所有微任务完成
        drain_job_queue();

        // 从 outer_promise 读取最终结果
        current_module_ = saved_module;
        if (outer_promise->state() == PromiseState::kFulfilled) {
            return EvalResult::ok(outer_promise->result());
        } else if (outer_promise->state() == PromiseState::kRejected) {
            Value reason = outer_promise->result();
            pending_throw_ = reason;
            if (reason.is_object()) {
                RcObject* raw = reason.as_object_raw();
                if (raw && raw->object_kind() == ObjectKind::kOrdinary) {
                    auto* obj = static_cast<JSObject*>(raw);
                    Value n = obj->get_property("name");
                    Value m = obj->get_property("message");
                    std::string name = n.is_string() ? n.as_string() : "Error";
                    std::string message = m.is_string() ? m.as_string() : "";
                    return EvalResult::err(Error(ErrorKind::Runtime, name + ": " + message));
                }
            }
            return EvalResult::err(Error(ErrorKind::Runtime, to_string_val(reason)));
        } else {
            // Promise 仍 pending（不应发生，drain 后应已 settled）
            return EvalResult::err(Error(ErrorKind::Runtime,
                "Error: top-level await did not settle"));
        }
    }

    current_module_ = saved_module;

    if (has_error) return final_result;
    return EvalResult::ok(last);
}

// ---- dynamic import() ----

EvalResult Interpreter::eval_import_call(const ImportCallExpression& expr) {
    // Evaluate specifier expression
    auto spec_result = eval_expr(*expr.specifier);
    if (!spec_result.is_ok()) {
        // Return rejected Promise with the evaluation error
        auto p = RcPtr<JSPromise>::make();
        gc_heap_.Register(p.get());
        Value err_val = pending_throw_.has_value() ? *pending_throw_
                      : make_error_value(NativeErrorType::kTypeError, "import() specifier evaluation failed");
        pending_throw_ = std::nullopt;
        p->Reject(err_val, job_queue_);
        return EvalResult::ok(Value::object(ObjectPtr(p)));
    }

    std::string specifier = to_string_val(spec_result.value());

    // Resolve the specifier relative to the current module's directory (or cwd)
    std::string base_dir;
    if (current_module_) {
        base_dir = std::filesystem::path(current_module_->specifier).parent_path().string();
    } else {
        base_dir = std::filesystem::current_path().string();
    }

    // Create the result promise
    auto promise = RcPtr<JSPromise>::make();
    gc_heap_.Register(promise.get());

    // Load the module
    auto load_result = module_loader_.Load(specifier, base_dir);
    if (!load_result.ok()) {
        Value err_val = make_error_value(NativeErrorType::kError,
            "Cannot load module '" + specifier + "': " + load_result.error().message());
        promise->Reject(err_val, job_queue_);
        return EvalResult::ok(Value::object(ObjectPtr(promise)));
    }
    auto mod = load_result.value();

    // Link the module
    auto link_result = link_module(*mod);
    if (!link_result.is_ok()) {
        Value err_val;
        if (pending_throw_.has_value()) {
            err_val = std::move(*pending_throw_);
            pending_throw_ = std::nullopt;
        } else {
            err_val = make_error_value(NativeErrorType::kError,
                "Cannot link module '" + specifier + "'");
        }
        promise->Reject(err_val, job_queue_);
        return EvalResult::ok(Value::object(ObjectPtr(promise)));
    }

    // Evaluate the module
    auto eval_result = evaluate_module(*mod);
    if (!eval_result.is_ok()) {
        Value err_val;
        if (pending_throw_.has_value()) {
            err_val = std::move(*pending_throw_);
            pending_throw_ = std::nullopt;
        } else {
            err_val = make_error_value(NativeErrorType::kError,
                "Cannot evaluate module '" + specifier + "'");
        }
        promise->Reject(err_val, job_queue_);
        return EvalResult::ok(Value::object(ObjectPtr(promise)));
    }

    // Build namespace object from module exports
    auto ns_obj = RcPtr<JSObject>::make();
    gc_heap_.Register(ns_obj.get());
    for (const auto& entry : mod->exports) {
        if (entry.cell && entry.cell->initialized) {
            ns_obj->set_property(entry.name, entry.cell->value);
        } else if (entry.cell) {
            ns_obj->set_property(entry.name, Value::undefined());
        }
    }

    promise->Fulfill(Value::object(ObjectPtr(ns_obj)), job_queue_);
    return EvalResult::ok(Value::object(ObjectPtr(promise)));
}

// ---- async/await ----

Value Interpreter::make_async_function_value(std::optional<std::string> name,
                                              std::vector<std::string> params,
                                              std::shared_ptr<std::vector<StmtNode>> body,
                                              RcPtr<Environment> closure_env) {
    // Create a native JSFunction that wraps the async body execution
    auto fn = RcPtr<JSFunction>::make();
    fn->set_name(name);
    fn->set_params(params);
    fn->set_body(body);
    fn->set_closure_env(closure_env);
    fn->set_defining_module(current_module_);

    auto proto_obj = RcPtr<JSObject>::make();
    proto_obj->set_proto(object_prototype_);
    proto_obj->set_constructor_property(fn.get());
    fn->set_prototype_obj(proto_obj);

    // P2-D: capture fn as raw pointer for the self-reference binding inside the body.
    JSFunction* fn_self_raw = fn.get();

    // The async wrapper: creates outer_promise, executes body, fulfills/rejects
    fn->set_native_fn([this, body, params, closure_env, name, fn_self_raw](
            Value this_val_arg, std::vector<Value> call_args, bool) mutable -> EvalResult {
        // Create outer promise
        auto outer_promise = RcPtr<JSPromise>::make();
        gc_heap_.Register(outer_promise.get());
        Value outer_val = Value::object(ObjectPtr(outer_promise));

        // Set up function environment
        RcPtr<Environment> outer_env = closure_env ? closure_env : global_env_;
        auto fn_env = RcPtr<Environment>::make(outer_env);
        gc_heap_.Register(fn_env.get());

        // P2-D: bind the function name inside the body for named async function expressions.
        if (name.has_value()) {
            fn_env->define(name.value(), VarKind::Const);
            fn_env->initialize(name.value(), Value::object(ObjectPtr(RcPtr<JSFunction>(fn_self_raw))));
        }

        // Bind parameters
        for (size_t i = 0; i < params.size(); ++i) {
            Value arg_val = (i < call_args.size()) ? call_args[i] : Value::undefined();
            fn_env->define(params[i], VarKind::Var);
            fn_env->initialize(params[i], std::move(arg_val));
        }

        hoist_vars(*body, *fn_env);
        run_async_body(body, 0, fn_env, std::move(this_val_arg), outer_promise);
        return EvalResult::ok(outer_val);
    });

    gc_heap_.Register(fn.get());
    gc_heap_.Register(proto_obj.get());
    return Value::object(ObjectPtr(fn));
}

void Interpreter::run_async_body(std::shared_ptr<std::vector<StmtNode>> body, size_t stmt_index,
                                 RcPtr<Environment> fn_env, Value this_val,
                                 RcPtr<JSPromise> outer_promise) {
    JSPromise* saved_async_promise = current_async_promise_;
    bool saved_in_async = in_async_body_;
    current_async_promise_ = outer_promise.get();
    in_async_body_ = true;

    ScopeGuard guard(*this, fn_env, fn_env, this_val, /*is_call=*/true);

    Value result_val = Value::undefined();
    bool threw = false;
    bool suspended = false;
    Value throw_val;
    size_t suspend_stmt_index = 0;

    for (size_t i = stmt_index; i < body->size(); ++i) {
        auto stmt_result = eval_stmt((*body)[i]);
        if (!stmt_result.is_ok()) {
            const std::string& msg = stmt_result.error().message();
            if (msg == kAsyncSuspendSentinel) {
                suspended = true;
                suspend_stmt_index = i;
                break;
            }
            if (msg == kPendingThrowSentinel && pending_throw_.has_value()) {
                throw_val = std::move(*pending_throw_);
                pending_throw_ = std::nullopt;
            } else {
                throw_val = Value::string(msg);
            }
            threw = true;
            break;
        }
        const Completion& c = stmt_result.completion();
        if (c.is_return()) {
            result_val = c.value;
            break;
        }
        if (c.is_throw()) {
            throw_val = c.value;
            threw = true;
            break;
        }
        result_val = c.value;
    }

    current_async_promise_ = saved_async_promise;
    in_async_body_ = saved_in_async;

    if (suspended) {
        // eval_await_expr set pending_inner_promise_ before returning kAsyncSuspendSentinel.
        // We pick it up here and set up resume/reject callbacks.
        if (!pending_inner_promise_.has_value()) {
            // Should not happen, but defensively reject
            outer_promise->Reject(
                make_error_value(NativeErrorType::kTypeError, "internal: missing inner promise"),
                job_queue_);
            return;
        }
        auto inner_promise = std::move(*pending_inner_promise_);
        pending_inner_promise_ = std::nullopt;

        // resume_stmt_index is the statement that contained the await expression.
        // When resuming, pending_await_result_ will be set, so eval_await_expr
        // will return the fulfilled value without re-suspending.
        size_t resume_index = suspend_stmt_index;

        // Build resume_fn: called with fulfilled value
        auto resume_fn = RcPtr<JSFunction>::make();
        // Store fn_env and outer_promise in own_properties for GC safety
        resume_fn->set_property("__resume_env__", Value::object(ObjectPtr(fn_env)));
        resume_fn->set_property("__resume_promise__", Value::object(ObjectPtr(outer_promise)));
        resume_fn->set_native_fn([this, body, resume_index, fn_env, this_val,
                                  outer_promise](
                Value, std::vector<Value> args, bool) mutable -> EvalResult {
            Value fulfilled_val = args.empty() ? Value::undefined() : args[0];
            pending_await_result_ = std::move(fulfilled_val);
            run_async_body(body, resume_index, fn_env, this_val, outer_promise);
            return EvalResult::ok(Value::undefined());
        });
        gc_heap_.Register(resume_fn.get());

        // Build reject_fn: called with rejection reason
        auto reject_fn = RcPtr<JSFunction>::make();
        reject_fn->set_property("__resume_env__", Value::object(ObjectPtr(fn_env)));
        reject_fn->set_property("__resume_promise__", Value::object(ObjectPtr(outer_promise)));
        reject_fn->set_native_fn([this, body, resume_index, fn_env, this_val,
                                  outer_promise](
                Value, std::vector<Value> args, bool) mutable -> EvalResult {
            Value reason = args.empty() ? Value::undefined() : args[0];
            // Inject rejection as a pending throw so try/catch can intercept it
            pending_throw_ = std::move(reason);
            // Set pending_await_result_ to a dummy value so eval_await_expr
            // sees has_value() and checks pending_throw_ first
            pending_await_result_ = Value::undefined();
            run_async_body(body, resume_index, fn_env, this_val, outer_promise);
            return EvalResult::ok(Value::undefined());
        });
        gc_heap_.Register(reject_fn.get());

        JSPromise::PerformThen(inner_promise,
            Value::object(ObjectPtr(resume_fn)),
            Value::object(ObjectPtr(reject_fn)),
            job_queue_);
        return;
    }

    if (threw) {
        outer_promise->Reject(std::move(throw_val), job_queue_);
        return;
    }

    // If result_val is a Promise, adopt its state
    if (result_val.is_object() && result_val.as_object_raw() &&
        result_val.as_object_raw()->object_kind() == ObjectKind::kPromise) {
        auto* inner = static_cast<JSPromise*>(result_val.as_object_raw());
        auto inner_rc = RcPtr<JSPromise>(inner);
        auto fulfill_outer = RcPtr<JSFunction>::make();
        fulfill_outer->set_native_fn([this, outer_promise](Value, std::vector<Value> a, bool) mutable -> EvalResult {
            Value v = a.empty() ? Value::undefined() : a[0];
            outer_promise->Fulfill(v, job_queue_);
            return EvalResult::ok(Value::undefined());
        });
        gc_heap_.Register(fulfill_outer.get());
        auto reject_outer = RcPtr<JSFunction>::make();
        reject_outer->set_native_fn([this, outer_promise](Value, std::vector<Value> a, bool) mutable -> EvalResult {
            Value r = a.empty() ? Value::undefined() : a[0];
            outer_promise->Reject(r, job_queue_);
            return EvalResult::ok(Value::undefined());
        });
        gc_heap_.Register(reject_outer.get());
        JSPromise::PerformThen(inner_rc,
            Value::object(ObjectPtr(fulfill_outer)),
            Value::object(ObjectPtr(reject_outer)),
            job_queue_);
    } else {
        outer_promise->Fulfill(std::move(result_val), job_queue_);
    }
}

EvalResult Interpreter::eval_async_function_expr(const AsyncFunctionExpression& expr) {
    return EvalResult::ok(make_async_function_value(
        expr.name, expr.params, expr.body, current_env_));
}

StmtResult Interpreter::eval_async_function_decl(const AsyncFunctionDeclaration& /*stmt*/) {
    // P2-C: async function declarations are hoisted and assigned in hoist_vars_stmt; skip here.
    return StmtResult::ok(Completion::normal(Value::undefined()));
}

EvalResult Interpreter::eval_await_expr(const AwaitExpression& expr) {
    // Resume path: pending_await_result_ is set by resume_fn callback.
    // Check pending_throw_ first (reject_fn path).
    if (pending_await_result_.has_value()) {
        if (pending_throw_.has_value()) {
            // reject_fn path: propagate the rejection
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        Value result = std::move(*pending_await_result_);
        pending_await_result_ = std::nullopt;
        return EvalResult::ok(std::move(result));
    }

    // Suspend path: evaluate argument, wrap in inner_promise, signal suspension.
    auto arg_result = eval_expr(*expr.argument);
    if (!arg_result.is_ok()) return arg_result;

    auto inner_promise = promise_resolve(arg_result.value());

    // Store inner_promise for run_async_body to pick up and set up PerformThen.
    pending_inner_promise_ = inner_promise;

    // Signal suspension — run_async_body will detect this sentinel and set up callbacks.
    return EvalResult::err(Error(ErrorKind::Runtime, kAsyncSuspendSentinel));
}

}  // namespace qppjs
