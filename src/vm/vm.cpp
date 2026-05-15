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
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
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
        std::string s = v.as_string();
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

static bool same_value_zero(const Value& a, const Value& b) {
    if (a.is_number() && b.is_number() && std::isnan(a.as_number()) && std::isnan(b.as_number())) {
        return true;
    }
    return strict_eq(a, b);
}

static double to_number_double_vm(const Value& v) {
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
        // strtod requires a null-terminated string; build one from the trimmed range.
        std::string s(sv.data() + first, last - first + 1);
        char* end = nullptr;
        double r = std::strtod(s.c_str(), &end);
        if (end == s.c_str() || *end != '\0') return std::numeric_limits<double>::quiet_NaN();
        return r;
    }
    case ValueKind::Object: return std::numeric_limits<double>::quiet_NaN();
    }
    return std::numeric_limits<double>::quiet_NaN();
}

static std::optional<uint32_t> resolve_from_index_vm(uint32_t len, const std::vector<Value>& args,
                                                      size_t arg_idx) {
    if (args.size() <= arg_idx || args[arg_idx].is_undefined()) return 0u;
    double n = to_number_double_vm(args[arg_idx]);
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

// UTF-16 code unit count: BMP = 1, SMP (U+10000+) = 2.
static int32_t utf8_cp_len_vm(JSString* js_str) {
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
static size_t utf8_cu_to_byte_vm(std::string_view s, int32_t cu_offset) {
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

static std::string utf8_substr_vm(std::string_view s, int32_t cu_start, int32_t cu_end) {
    if (cu_start >= cu_end) return "";
    size_t byte_start = utf8_cu_to_byte_vm(s, cu_start);
    size_t byte_end = utf8_cu_to_byte_vm(s, cu_end);
    return std::string(s.substr(byte_start, byte_end - byte_start));
}

static bool is_js_whitespace_cp_vm(uint32_t cp) {
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

static uint32_t utf8_decode_one_vm(std::string_view s, size_t& i) {
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

static std::string utf8_trim_impl_vm(std::string_view s, bool trim_start, bool trim_end) {
    size_t start = 0;
    if (trim_start) {
        while (start < s.size()) {
            size_t tmp = start;
            uint32_t cp = utf8_decode_one_vm(s, tmp);
            if (!is_js_whitespace_cp_vm(cp)) break;
            start = tmp;
        }
    }
    size_t end = s.size();
    if (trim_end && end > start) {
        // Walk backwards without allocating a positions vector.
        while (end > start) {
            size_t cp_start = end - 1;
            while (cp_start > start && (static_cast<unsigned char>(s[cp_start]) & 0xC0) == 0x80) {
                --cp_start;
            }
            size_t tmp = cp_start;
            uint32_t cp = utf8_decode_one_vm(s, tmp);
            if (!is_js_whitespace_cp_vm(cp)) break;
            end = cp_start;
        }
    }
    if (start >= end) return "";
    return std::string(s.substr(start, end - start));
}

static int32_t str_index_of_vm(std::string_view haystack, std::string_view needle,
                                int32_t cu_from, int32_t len) {
    if (needle.empty()) {
        return std::min(cu_from, len);
    }
    size_t byte_from = utf8_cu_to_byte_vm(haystack, cu_from);
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

static int32_t str_last_index_of_vm(std::string_view haystack, std::string_view needle,
                                     int32_t cu_from, int32_t len) {
    if (needle.empty()) {
        return std::min(cu_from, len);
    }
    // byte_from is the byte offset of cu_from (the maximum allowed start position).
    size_t byte_from = utf8_cu_to_byte_vm(haystack, cu_from);
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
        std::string sa = a.as_string();
        double n = sa.empty() ? 0.0 : std::strtod(sa.c_str(), &end);
        if (!sa.empty() && (end == sa.c_str() || *end != '\0'))
            n = std::numeric_limits<double>::quiet_NaN();
        return abstract_eq(Value::number(n), b);
    }
    if (a.is_number() && b.is_string()) {
        char* end = nullptr;
        std::string sb = b.as_string();
        double n = sb.empty() ? 0.0 : std::strtod(sb.c_str(), &end);
        if (!sb.empty() && (end == sb.c_str() || *end != '\0'))
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

    // Array.prototype.map
    auto map_fn = RcPtr<JSFunction>::make();
    map_fn->set_name(std::string("map"));
    map_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        RcObject* raw = this_val.as_object_raw();
        if (!raw || raw->object_kind() != ObjectKind::kArray) {
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                                     "map called on non-array");
            return EvalResult::err(Error{ErrorKind::Runtime, "__qppjs_pending_throw__"});
        }
        if (args.empty() || !args[0].is_object() ||
            args[0].as_object_raw()->object_kind() != ObjectKind::kFunction) {
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                                     "callback is not a function");
            return EvalResult::err(Error{ErrorKind::Runtime, "__qppjs_pending_throw__"});
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
            std::span<Value> arg_span(call_args, 3);
            auto res = call_function_val(callback, this_arg, arg_span);
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
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                                     "filter called on non-array");
            return EvalResult::err(Error{ErrorKind::Runtime, "__qppjs_pending_throw__"});
        }
        if (args.empty() || !args[0].is_object() ||
            args[0].as_object_raw()->object_kind() != ObjectKind::kFunction) {
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                                     "callback is not a function");
            return EvalResult::err(Error{ErrorKind::Runtime, "__qppjs_pending_throw__"});
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
            std::span<Value> arg_span(call_args, 3);
            auto res = call_function_val(callback, this_arg, arg_span);
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
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                                     "reduce called on non-array");
            return EvalResult::err(Error{ErrorKind::Runtime, "__qppjs_pending_throw__"});
        }
        if (args.empty() || !args[0].is_object() ||
            args[0].as_object_raw()->object_kind() != ObjectKind::kFunction) {
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                                     "callback is not a function");
            return EvalResult::err(Error{ErrorKind::Runtime, "__qppjs_pending_throw__"});
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
                native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                                         "Reduce of empty array with no initial value");
                return EvalResult::err(Error{ErrorKind::Runtime, "__qppjs_pending_throw__"});
            }
        }
        for (uint32_t i = k; i < len; i++) {
            auto it = arr->elements_.find(i);
            if (it == arr->elements_.end()) continue;
            Value call_args[4] = {acc, it->second, Value::number(static_cast<double>(i)), this_val};
            std::span<Value> arg_span(call_args, 4);
            auto res = call_function_val(callback, Value::undefined(), arg_span);
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
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                                     "reduceRight called on non-array");
            return EvalResult::err(Error{ErrorKind::Runtime, "__qppjs_pending_throw__"});
        }
        if (args.empty() || !args[0].is_object() ||
            args[0].as_object_raw()->object_kind() != ObjectKind::kFunction) {
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                                     "callback is not a function");
            return EvalResult::err(Error{ErrorKind::Runtime, "__qppjs_pending_throw__"});
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
                native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                                         "Reduce of empty array with no initial value");
                return EvalResult::err(Error{ErrorKind::Runtime, "__qppjs_pending_throw__"});
            }
        }
        for (; k >= 0; k--) {
            auto it = arr->elements_.find(static_cast<uint32_t>(k));
            if (it == arr->elements_.end()) continue;
            Value call_args[4] = {acc, it->second, Value::number(static_cast<double>(k)), this_val};
            std::span<Value> arg_span(call_args, 4);
            auto res = call_function_val(callback, Value::undefined(), arg_span);
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
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                                     "find called on non-array");
            return EvalResult::err(Error{ErrorKind::Runtime, "__qppjs_pending_throw__"});
        }
        if (args.empty() || !args[0].is_object() ||
            args[0].as_object_raw()->object_kind() != ObjectKind::kFunction) {
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                                     "callback is not a function");
            return EvalResult::err(Error{ErrorKind::Runtime, "__qppjs_pending_throw__"});
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
            std::span<Value> arg_span(call_args, 3);
            auto res = call_function_val(callback, this_arg, arg_span);
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
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                                     "findIndex called on non-array");
            return EvalResult::err(Error{ErrorKind::Runtime, "__qppjs_pending_throw__"});
        }
        if (args.empty() || !args[0].is_object() ||
            args[0].as_object_raw()->object_kind() != ObjectKind::kFunction) {
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                                     "callback is not a function");
            return EvalResult::err(Error{ErrorKind::Runtime, "__qppjs_pending_throw__"});
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
            std::span<Value> arg_span(call_args, 3);
            auto res = call_function_val(callback, this_arg, arg_span);
            if (!res.is_ok()) return res;
            if (to_boolean(res.value())) {
                return EvalResult::ok(Value::number(static_cast<double>(i)));
            }
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
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                                     "some called on non-array");
            return EvalResult::err(Error{ErrorKind::Runtime, "__qppjs_pending_throw__"});
        }
        if (args.empty() || !args[0].is_object() ||
            args[0].as_object_raw()->object_kind() != ObjectKind::kFunction) {
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                                     "callback is not a function");
            return EvalResult::err(Error{ErrorKind::Runtime, "__qppjs_pending_throw__"});
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
            std::span<Value> arg_span(call_args, 3);
            auto res = call_function_val(callback, this_arg, arg_span);
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
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                                     "every called on non-array");
            return EvalResult::err(Error{ErrorKind::Runtime, "__qppjs_pending_throw__"});
        }
        if (args.empty() || !args[0].is_object() ||
            args[0].as_object_raw()->object_kind() != ObjectKind::kFunction) {
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                                     "callback is not a function");
            return EvalResult::err(Error{ErrorKind::Runtime, "__qppjs_pending_throw__"});
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
            std::span<Value> arg_span(call_args, 3);
            auto res = call_function_val(callback, this_arg, arg_span);
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
            return EvalResult::err(Error{ErrorKind::Runtime, "TypeError: indexOf called on non-array"});
        }
        auto* arr = static_cast<JSObject*>(raw);
        uint32_t len = arr->array_length_;
        auto k_opt = resolve_from_index_vm(len, args, 1);
        if (!k_opt.has_value()) return EvalResult::ok(Value::number(-1.0));
        Value search_val = args.size() >= 1 ? args[0] : Value::undefined();
        for (uint32_t i = *k_opt; i < len; i++) {
            auto it = arr->elements_.find(i);
            if (it == arr->elements_.end()) continue;
            if (strict_eq(it->second, search_val)) {
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
            return EvalResult::err(Error{ErrorKind::Runtime, "TypeError: includes called on non-array"});
        }
        auto* arr = static_cast<JSObject*>(raw);
        uint32_t len = arr->array_length_;
        auto k_opt = resolve_from_index_vm(len, args, 1);
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
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                                     "slice called on non-array");
            return EvalResult::err(Error{ErrorKind::Runtime, "__qppjs_pending_throw__"});
        }
        auto* arr = static_cast<JSObject*>(raw);
        int64_t len = static_cast<int64_t>(arr->array_length_);
        double start_d = args.size() >= 1 && !args[0].is_undefined() ? to_number_double_vm(args[0]) : 0.0;
        if (std::isnan(start_d)) start_d = 0.0;
        start_d = std::trunc(start_d);
        int64_t start = start_d < 0.0 ? std::max(len + static_cast<int64_t>(start_d), int64_t{0})
                                       : std::min(static_cast<int64_t>(start_d), len);
        double end_d = args.size() >= 2 && !args[1].is_undefined() ? to_number_double_vm(args[1])
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
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                                     "splice called on non-array");
            return EvalResult::err(Error{ErrorKind::Runtime, "__qppjs_pending_throw__"});
        }
        auto* arr = static_cast<JSObject*>(raw);
        int64_t len = static_cast<int64_t>(arr->array_length_);
        int64_t start = 0;
        if (!args.empty()) {
            double s = to_number_double_vm(args[0]);
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
            double dc = to_number_double_vm(args[1]);
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
        if (new_len > 9007199254740991LL) {
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                                     "splice: new length exceeds 2^53-1");
            return EvalResult::err(Error{ErrorKind::Runtime, "__qppjs_pending_throw__"});
        }
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
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                                     "sort called on non-array");
            return EvalResult::err(Error{ErrorKind::Runtime, "__qppjs_pending_throw__"});
        }
        bool has_cmp = !args.empty() && !args[0].is_undefined();
        if (has_cmp) {
            if (!args[0].is_object() || args[0].as_object_raw()->object_kind() != ObjectKind::kFunction) {
                native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                                         "compareFn must be a function");
                return EvalResult::err(Error{ErrorKind::Runtime, "__qppjs_pending_throw__"});
            }
        }
        auto* arr = static_cast<JSObject*>(raw);
        uint32_t len = arr->array_length_;
        struct Slot {
            Value val;
            uint32_t pos;
            std::string str_cache;
        };
        std::vector<Slot> slots;
        slots.reserve(len);
        uint32_t undef_count = 0;
        for (uint32_t i = 0; i < len; i++) {
            auto it = arr->elements_.find(i);
            if (it == arr->elements_.end()) {
            } else if (it->second.is_undefined()) {
                undef_count++;
            } else {
                slots.push_back({it->second, i, {}});
            }
        }
        if (!has_cmp) {
            for (auto& s : slots) {
                s.str_cache = VM::to_string_val(s.val);
            }
        }
        Value cmp_fn = has_cmp ? args[0] : Value::undefined();
        EvalResult sort_err = EvalResult::ok(Value::undefined());
        bool had_error = false;
        std::stable_sort(slots.begin(), slots.end(), [&](const Slot& a, const Slot& b) -> bool {
            if (had_error) return false;
            if (has_cmp) {
                Value call_args[2] = {a.val, b.val};
                std::span<Value> arg_span(call_args, 2);
                auto res = call_function_val(cmp_fn, Value::undefined(), arg_span);
                if (!res.is_ok()) {
                    sort_err = res;
                    had_error = true;
                    return false;
                }
                double cmp = to_number_double_vm(res.value());
                if (std::isnan(cmp)) cmp = 0.0;
                if (cmp != 0.0) return cmp < 0.0;
                return a.pos < b.pos;
            } else {
                int cmp = a.str_cache.compare(b.str_cache);
                if (cmp != 0) return cmp < 0;
                return a.pos < b.pos;
            }
        });
        if (had_error) return sort_err;
        arr->elements_.clear();
        uint32_t idx = 0;
        for (auto& s : slots) {
            arr->elements_[idx++] = std::move(s.val);
        }
        for (uint32_t i = 0; i < undef_count; i++) {
            arr->elements_[idx++] = Value::undefined();
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
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                                     "join called on non-array");
            return EvalResult::err(Error{ErrorKind::Runtime, "__qppjs_pending_throw__"});
        }
        auto* arr = static_cast<JSObject*>(raw);
        uint32_t len = arr->array_length_;
        std::string sep = (args.empty() || args[0].is_undefined()) ? "," : VM::to_string_val(args[0]);
        if (len == 0) return EvalResult::ok(Value::string(""));
        // First pass: compute total length for reserve
        size_t total = 0;
        for (uint32_t k = 0; k < len; k++) {
            auto it = arr->elements_.find(k);
            if (it != arr->elements_.end() && !it->second.is_null() && !it->second.is_undefined()) {
                if (it->second.is_string()) {
                    total += it->second.sv().size();
                } else {
                    total += VM::to_string_val(it->second).size();
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
                    result += VM::to_string_val(it->second);
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
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                                     "reverse called on non-array");
            return EvalResult::err(Error{ErrorKind::Runtime, "__qppjs_pending_throw__"});
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
    // Recursive helper (vm suffix to avoid ODR conflict with interpreter translation unit)
    auto flatten_into_array_vm = [](auto& self, JSObject* result, JSObject* source,
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
    flat_fn->set_native_fn([this, flatten_into_array_vm](Value this_val, std::vector<Value> args,
                                                          bool) -> EvalResult {
        RcObject* raw = this_val.as_object_raw();
        if (!raw || raw->object_kind() != ObjectKind::kArray) {
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                                     "flat called on non-array");
            return EvalResult::err(Error{ErrorKind::Runtime, "__qppjs_pending_throw__"});
        }
        auto* arr = static_cast<JSObject*>(raw);
        double depth_num = 1.0;
        if (!args.empty() && !args[0].is_undefined()) {
            depth_num = to_number_double_vm(args[0]);
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
        flatten_into_array_vm(flatten_into_array_vm, result.get(), arr, arr->array_length_,
                              target_idx, depth_num, 0);
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
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                                     "flatMap called on non-array");
            return EvalResult::err(Error{ErrorKind::Runtime, "__qppjs_pending_throw__"});
        }
        if (args.empty() || !args[0].is_object() ||
            args[0].as_object_raw()->object_kind() != ObjectKind::kFunction) {
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                                     "flatMap callback must be a function");
            return EvalResult::err(Error{ErrorKind::Runtime, "__qppjs_pending_throw__"});
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
            std::span<Value> arg_span(call_args, 3);
            auto res = call_function_val(callback, this_arg, arg_span);
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

    // ---- Promise ----

    promise_prototype_ = RcPtr<JSObject>::make();
    promise_prototype_->set_proto(object_prototype_);

    // Promise.prototype.then
    auto vm_then_fn = RcPtr<JSFunction>::make();
    vm_then_fn->set_name(std::string("then"));
    vm_then_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        RcObject* raw = this_val.as_object_raw();
        if (!raw || raw->object_kind() != ObjectKind::kPromise) {
            return EvalResult::err(Error(ErrorKind::Runtime,
                "TypeError: Promise.prototype.then called on non-Promise"));
        }
        auto* p = static_cast<JSPromise*>(raw);
        auto promise_rc = RcPtr<JSPromise>(p);
        Value on_fulfilled = args.size() > 0 ? args[0] : Value::undefined();
        Value on_rejected = args.size() > 1 ? args[1] : Value::undefined();
        auto result_promise = JSPromise::PerformThen(promise_rc, on_fulfilled, on_rejected, job_queue_);
        gc_heap_.Register(result_promise.get());
        return EvalResult::ok(Value::object(ObjectPtr(result_promise)));
    });
    promise_prototype_->set_property("then", Value::object(ObjectPtr(vm_then_fn)));

    // Promise.prototype.catch
    auto vm_catch_fn = RcPtr<JSFunction>::make();
    vm_catch_fn->set_name(std::string("catch"));
    vm_catch_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        RcObject* raw = this_val.as_object_raw();
        if (!raw || raw->object_kind() != ObjectKind::kPromise) {
            return EvalResult::err(Error(ErrorKind::Runtime,
                "TypeError: Promise.prototype.catch called on non-Promise"));
        }
        auto* p = static_cast<JSPromise*>(raw);
        auto promise_rc = RcPtr<JSPromise>(p);
        Value on_rejected = args.size() > 0 ? args[0] : Value::undefined();
        auto result_promise = JSPromise::PerformThen(promise_rc, Value::undefined(), on_rejected, job_queue_);
        gc_heap_.Register(result_promise.get());
        return EvalResult::ok(Value::object(ObjectPtr(result_promise)));
    });
    promise_prototype_->set_property("catch", Value::object(ObjectPtr(vm_catch_fn)));

    // Promise.prototype.finally
    auto vm_finally_fn = RcPtr<JSFunction>::make();
    vm_finally_fn->set_name(std::string("finally"));
    vm_finally_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        RcObject* raw = this_val.as_object_raw();
        if (!raw || raw->object_kind() != ObjectKind::kPromise) {
            return EvalResult::err(Error(ErrorKind::Runtime,
                "TypeError: Promise.prototype.finally called on non-Promise"));
        }
        auto* p = static_cast<JSPromise*>(raw);
        auto promise_rc = RcPtr<JSPromise>(p);
        Value on_finally = args.size() > 0 ? args[0] : Value::undefined();
        Value captured_on_finally = on_finally;

        auto fulfill_wrapper = RcPtr<JSFunction>::make();
        fulfill_wrapper->set_native_fn([this, captured_on_finally](Value /*this_val*/,
                std::vector<Value> args2, bool) mutable -> EvalResult {
            Value val = args2.empty() ? Value::undefined() : args2[0];
            if (captured_on_finally.is_object() &&
                captured_on_finally.as_object_raw() &&
                captured_on_finally.as_object_raw()->object_kind() == ObjectKind::kFunction) {
                auto res = call_function_val(captured_on_finally, Value::undefined(),
                                             std::span<Value>());
                if (!res.is_ok()) return res;
                // C15: if finally fn returns a rejected Promise, propagate its reason
                if (res.value().is_object() && res.value().as_object_raw() &&
                    res.value().as_object_raw()->object_kind() == ObjectKind::kPromise) {
                    auto* rp = static_cast<JSPromise*>(res.value().as_object_raw());
                    if (rp->state() == PromiseState::kRejected) {
                        native_pending_throw_ = rp->result();
                        return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
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
                auto res = call_function_val(captured_on_finally, Value::undefined(),
                                             std::span<Value>());
                if (!res.is_ok()) return res;
                // C15: if finally fn returns a rejected Promise, propagate its reason
                if (res.value().is_object() && res.value().as_object_raw() &&
                    res.value().as_object_raw()->object_kind() == ObjectKind::kPromise) {
                    auto* rp = static_cast<JSPromise*>(res.value().as_object_raw());
                    if (rp->state() == PromiseState::kRejected) {
                        native_pending_throw_ = rp->result();
                        return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
                    }
                }
            }
            // Re-throw the original rejection reason.
            native_pending_throw_ = reason;
            return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
        });
        gc_heap_.Register(reject_wrapper.get());

        auto result_promise = JSPromise::PerformThen(promise_rc,
            Value::object(ObjectPtr(fulfill_wrapper)),
            Value::object(ObjectPtr(reject_wrapper)),
            job_queue_);
        gc_heap_.Register(result_promise.get());
        return EvalResult::ok(Value::object(ObjectPtr(result_promise)));
    });
    promise_prototype_->set_property("finally", Value::object(ObjectPtr(vm_finally_fn)));

    // Promise constructor
    auto vm_promise_ctor = RcPtr<JSFunction>::make();
    vm_promise_ctor->set_name(std::string("Promise"));
    vm_promise_ctor->set_native_fn([this](Value /*this_val*/, std::vector<Value> args,
                                           bool /*is_new_call*/) -> EvalResult {
        if (args.empty() || !args[0].is_object() || !args[0].as_object_raw() ||
            args[0].as_object_raw()->object_kind() != ObjectKind::kFunction) {
            return EvalResult::err(Error(ErrorKind::Runtime,
                "TypeError: Promise constructor requires a function argument"));
        }
        auto promise = RcPtr<JSPromise>::make();
        gc_heap_.Register(promise.get());
        Value promise_val = Value::object(ObjectPtr(promise));

        auto resolve_fn = RcPtr<JSFunction>::make();
        resolve_fn->set_native_fn([this, promise](Value, std::vector<Value> resolve_args, bool) mutable -> EvalResult {
            Value val = resolve_args.empty() ? Value::undefined() : resolve_args[0];
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

        auto reject_fn = RcPtr<JSFunction>::make();
        reject_fn->set_native_fn([this, promise](Value, std::vector<Value> reject_args, bool) mutable -> EvalResult {
            Value reason = reject_args.empty() ? Value::undefined() : reject_args[0];
            promise->Reject(std::move(reason), job_queue_);
            return EvalResult::ok(Value::undefined());
        });
        gc_heap_.Register(reject_fn.get());

        std::vector<Value> executor_args = {
            Value::object(ObjectPtr(resolve_fn)),
            Value::object(ObjectPtr(reject_fn))
        };
        auto exec_result = call_function_val(args[0], Value::undefined(),
                                              std::span<Value>(executor_args.data(), executor_args.size()));
        if (!exec_result.is_ok()) {
            // executor threw: reject the promise
            Value thrown_val = Value::string(exec_result.error().message());
            // Check if the error is a pending throw from a kThrow
            if (!call_stack_.empty() && call_stack_.back().pending_throw.has_value()) {
                thrown_val = std::move(*call_stack_.back().pending_throw);
                call_stack_.back().pending_throw = std::nullopt;
            }
            promise->Reject(std::move(thrown_val), job_queue_);
        }

        return EvalResult::ok(promise_val);
    });

    // Promise.resolve
    auto vm_promise_resolve_fn = RcPtr<JSFunction>::make();
    vm_promise_resolve_fn->set_name(std::string("resolve"));
    vm_promise_resolve_fn->set_native_fn([this](Value, std::vector<Value> args, bool) -> EvalResult {
        Value val = args.empty() ? Value::undefined() : args[0];
        auto p = vm_promise_resolve(val);
        return EvalResult::ok(Value::object(ObjectPtr(p)));
    });
    vm_promise_ctor->set_property("resolve", Value::object(ObjectPtr(vm_promise_resolve_fn)));

    // Promise.reject
    auto vm_promise_reject_fn = RcPtr<JSFunction>::make();
    vm_promise_reject_fn->set_name(std::string("reject"));
    vm_promise_reject_fn->set_native_fn([this](Value, std::vector<Value> args, bool) -> EvalResult {
        Value reason = args.empty() ? Value::undefined() : args[0];
        auto p = RcPtr<JSPromise>::make();
        gc_heap_.Register(p.get());
        p->Reject(reason, job_queue_);
        return EvalResult::ok(Value::object(ObjectPtr(p)));
    });
    vm_promise_ctor->set_property("reject", Value::object(ObjectPtr(vm_promise_reject_fn)));

    // P2-B: Promise.prototype must be accessible via Promise.prototype
    vm_promise_ctor->set_property("prototype", Value::object(ObjectPtr(promise_prototype_)));

    gc_heap_.Register(vm_promise_ctor.get());
    global_env_->define("Promise", VarKind::Const);
    global_env_->initialize("Promise", Value::object(ObjectPtr(vm_promise_ctor)));

    // String.prototype
    string_prototype_ = RcPtr<JSObject>::make();
    string_prototype_->set_proto(object_prototype_);

    // indexOf(searchString, fromIndex)
    auto vm_str_index_of_fn = RcPtr<JSFunction>::make();
    vm_str_index_of_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        if (this_val.is_null() || this_val.is_undefined()) {
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "String.prototype.indexOf called on null or undefined");
            return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
        }
        Value effective_this = this_val.is_string() ? this_val : Value::string(to_string_val(this_val));
        JSString* js_str = effective_this.js_string_raw();
        int32_t len = utf8_cp_len_vm(js_str);
        std::string search = args.empty() ? "undefined" : to_string_val(args[0]);
        int32_t k = 0;
        if (args.size() >= 2) {
            double n = to_number_double_vm(args[1]);
            if (std::isinf(n) && n > 0) {
                k = len;
            } else {
                if (std::isnan(n)) n = 0.0;
                n = std::trunc(n);
                k = n < 0.0 ? 0 : (n > len ? len : static_cast<int32_t>(n));
            }
        }
        return EvalResult::ok(Value::number(static_cast<double>(str_index_of_vm(js_str->sv(), search, k, len))));
    });
    gc_heap_.Register(vm_str_index_of_fn.get());
    string_prototype_->set_property("indexOf", Value::object(ObjectPtr(vm_str_index_of_fn)));

    // lastIndexOf(searchString, fromIndex)
    auto vm_str_last_index_of_fn = RcPtr<JSFunction>::make();
    vm_str_last_index_of_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        if (this_val.is_null() || this_val.is_undefined()) {
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "String.prototype.lastIndexOf called on null or undefined");
            return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
        }
        Value effective_this = this_val.is_string() ? this_val : Value::string(to_string_val(this_val));
        JSString* js_str = effective_this.js_string_raw();
        int32_t len = utf8_cp_len_vm(js_str);
        std::string search = args.empty() ? "undefined" : to_string_val(args[0]);
        int32_t k = len;
        if (args.size() >= 2) {
            double n = to_number_double_vm(args[1]);
            if (std::isnan(n)) {
                k = len;
            } else {
                n = std::trunc(n);
                if (n < 0.0) k = 0;
                else if (n > len) k = len;
                else k = static_cast<int32_t>(n);
            }
        }
        return EvalResult::ok(Value::number(static_cast<double>(str_last_index_of_vm(js_str->sv(), search, k, len))));
    });
    gc_heap_.Register(vm_str_last_index_of_fn.get());
    string_prototype_->set_property("lastIndexOf", Value::object(ObjectPtr(vm_str_last_index_of_fn)));

    // slice(start, end)
    auto vm_str_slice_fn = RcPtr<JSFunction>::make();
    vm_str_slice_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        if (this_val.is_null() || this_val.is_undefined()) {
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "String.prototype.slice called on null or undefined");
            return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
        }
        Value effective_this = this_val.is_string() ? this_val : Value::string(to_string_val(this_val));
        JSString* js_str = effective_this.js_string_raw();
        int32_t len = utf8_cp_len_vm(js_str);
        auto resolve_slice_idx = [&](size_t arg_pos, int32_t default_val) -> int32_t {
            if (args.size() <= arg_pos || args[arg_pos].is_undefined()) return default_val;
            double n = to_number_double_vm(args[arg_pos]);
            if (std::isnan(n)) return 0;
            if (std::isinf(n)) return n > 0 ? len : 0;
            n = std::trunc(n);
            if (n < 0.0) return static_cast<int32_t>(std::max(0.0, static_cast<double>(len) + n));
            return static_cast<int32_t>(std::min(static_cast<double>(len), n));
        };
        int32_t from = resolve_slice_idx(0, 0);
        int32_t to = resolve_slice_idx(1, len);
        return EvalResult::ok(Value::string(utf8_substr_vm(js_str->sv(), from, to)));
    });
    gc_heap_.Register(vm_str_slice_fn.get());
    string_prototype_->set_property("slice", Value::object(ObjectPtr(vm_str_slice_fn)));

    // substring(start, end)
    auto vm_str_substring_fn = RcPtr<JSFunction>::make();
    vm_str_substring_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        if (this_val.is_null() || this_val.is_undefined()) {
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "String.prototype.substring called on null or undefined");
            return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
        }
        Value effective_this = this_val.is_string() ? this_val : Value::string(to_string_val(this_val));
        JSString* js_str = effective_this.js_string_raw();
        int32_t len = utf8_cp_len_vm(js_str);
        auto resolve_sub_idx = [&](size_t arg_pos, int32_t default_val) -> int32_t {
            if (args.size() <= arg_pos || args[arg_pos].is_undefined()) return default_val;
            double n = to_number_double_vm(args[arg_pos]);
            if (std::isnan(n) || n < 0.0) return 0;
            if (n > static_cast<double>(len)) return len;
            return static_cast<int32_t>(std::trunc(n));
        };
        int32_t start = resolve_sub_idx(0, 0);
        int32_t end = resolve_sub_idx(1, len);
        if (start > end) std::swap(start, end);
        return EvalResult::ok(Value::string(utf8_substr_vm(js_str->sv(), start, end)));
    });
    gc_heap_.Register(vm_str_substring_fn.get());
    string_prototype_->set_property("substring", Value::object(ObjectPtr(vm_str_substring_fn)));

    // split(separator, limit)
    auto vm_str_split_fn = RcPtr<JSFunction>::make();
    vm_str_split_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        if (this_val.is_null() || this_val.is_undefined()) {
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "String.prototype.split called on null or undefined");
            return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
        }
        std::string str = to_string_val(this_val);
        auto result = RcPtr<JSObject>::make(ObjectKind::kArray);
        result->set_proto(array_prototype_);
        gc_heap_.Register(result.get());

        // M-2: parse limit before checking undefined separator
        uint32_t limit = std::numeric_limits<uint32_t>::max();
        if (args.size() >= 2 && !args[1].is_undefined()) {
            double n = to_number_double_vm(args[1]);
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
            // Split by codepoint (SMP surrogate-pair splitting not implemented).
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
    gc_heap_.Register(vm_str_split_fn.get());
    string_prototype_->set_property("split", Value::object(ObjectPtr(vm_str_split_fn)));

    // trim()
    auto vm_str_trim_fn = RcPtr<JSFunction>::make();
    vm_str_trim_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        (void)args;
        if (this_val.is_null() || this_val.is_undefined()) {
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "String.prototype.trim called on null or undefined");
            return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
        }
        return EvalResult::ok(Value::string(utf8_trim_impl_vm(to_string_val(this_val), true, true)));
    });
    gc_heap_.Register(vm_str_trim_fn.get());
    string_prototype_->set_property("trim", Value::object(ObjectPtr(vm_str_trim_fn)));

    // trimStart()
    auto vm_str_trim_start_fn = RcPtr<JSFunction>::make();
    vm_str_trim_start_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        (void)args;
        if (this_val.is_null() || this_val.is_undefined()) {
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "String.prototype.trimStart called on null or undefined");
            return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
        }
        return EvalResult::ok(Value::string(utf8_trim_impl_vm(to_string_val(this_val), true, false)));
    });
    gc_heap_.Register(vm_str_trim_start_fn.get());
    string_prototype_->set_property("trimStart", Value::object(ObjectPtr(vm_str_trim_start_fn)));

    // trimEnd()
    auto vm_str_trim_end_fn = RcPtr<JSFunction>::make();
    vm_str_trim_end_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        (void)args;
        if (this_val.is_null() || this_val.is_undefined()) {
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "String.prototype.trimEnd called on null or undefined");
            return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
        }
        return EvalResult::ok(Value::string(utf8_trim_impl_vm(to_string_val(this_val), false, true)));
    });
    gc_heap_.Register(vm_str_trim_end_fn.get());
    string_prototype_->set_property("trimEnd", Value::object(ObjectPtr(vm_str_trim_end_fn)));

    // ---- Global constants: NaN, Infinity ----

    global_env_->define("NaN", VarKind::Const);
    global_env_->initialize("NaN", Value::number(std::numeric_limits<double>::quiet_NaN()));
    global_env_->define("Infinity", VarKind::Const);
    global_env_->initialize("Infinity", Value::number(std::numeric_limits<double>::infinity()));

    // ---- Global functions: isNaN, isFinite, parseInt, parseFloat ----

    // parseFloat helper (no substr copy)
    static auto vm_parse_float_impl = [](const std::string& s) -> double {
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
    static auto vm_parse_int_impl = [](const std::string& s, int radix) -> double {
        size_t i = 0;
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) i++;
        if (i == s.size()) return std::numeric_limits<double>::quiet_NaN();
        int sign = 1;
        if (s[i] == '+') { i++; }
        else if (s[i] == '-') { sign = -1; i++; }
        if (radix == 0 || radix == 16) {
            if (i + 1 < s.size() && s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
                radix = 16;
                i += 2;
            }
        }
        if (radix == 0) radix = 10;
        if (radix < 2 || radix > 36) return std::numeric_limits<double>::quiet_NaN();
        if (i == s.size()) return std::numeric_limits<double>::quiet_NaN();
        // Use double to avoid signed overflow UB for large integers
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
    auto vm_parse_int_fn = RcPtr<JSFunction>::make();
    vm_parse_int_fn->set_name(std::string("parseInt"));
    vm_parse_int_fn->set_native_fn([](Value /*this_val*/, std::vector<Value> args, bool) -> EvalResult {
        std::string s = args.empty() ? "undefined" : VM::to_string_val(args[0]);
        int radix = 0;
        if (args.size() >= 2) {
            double r = to_number_double_vm(args[1]);
            radix = std::isnan(r) ? 0 : static_cast<int>(std::trunc(r));
        }
        return EvalResult::ok(Value::number(vm_parse_int_impl(s, radix)));
    });
    gc_heap_.Register(vm_parse_int_fn.get());
    Value vm_parse_int_val = Value::object(ObjectPtr(vm_parse_int_fn));
    global_env_->define_initialized("parseInt");
    global_env_->set("parseInt", vm_parse_int_val);

    // Build parseFloat function
    auto vm_parse_float_fn = RcPtr<JSFunction>::make();
    vm_parse_float_fn->set_name(std::string("parseFloat"));
    vm_parse_float_fn->set_native_fn([](Value /*this_val*/, std::vector<Value> args, bool) -> EvalResult {
        std::string s = args.empty() ? "undefined" : VM::to_string_val(args[0]);
        return EvalResult::ok(Value::number(vm_parse_float_impl(s)));
    });
    gc_heap_.Register(vm_parse_float_fn.get());
    global_env_->define_initialized("parseFloat");
    global_env_->set("parseFloat", Value::object(ObjectPtr(vm_parse_float_fn)));

    // Build global isNaN (does ToNumber conversion)
    auto vm_is_nan_fn = RcPtr<JSFunction>::make();
    vm_is_nan_fn->set_name(std::string("isNaN"));
    vm_is_nan_fn->set_native_fn([](Value /*this_val*/, std::vector<Value> args, bool) -> EvalResult {
        double n = to_number_double_vm(args.empty() ? Value::undefined() : args[0]);
        return EvalResult::ok(Value::boolean(std::isnan(n)));
    });
    gc_heap_.Register(vm_is_nan_fn.get());
    global_env_->define_initialized("isNaN");
    global_env_->set("isNaN", Value::object(ObjectPtr(vm_is_nan_fn)));

    // Build global isFinite (does ToNumber conversion)
    auto vm_is_finite_fn = RcPtr<JSFunction>::make();
    vm_is_finite_fn->set_name(std::string("isFinite"));
    vm_is_finite_fn->set_native_fn([](Value /*this_val*/, std::vector<Value> args, bool) -> EvalResult {
        double n = to_number_double_vm(args.empty() ? Value::undefined() : args[0]);
        return EvalResult::ok(Value::boolean(std::isfinite(n)));
    });
    gc_heap_.Register(vm_is_finite_fn.get());
    global_env_->define_initialized("isFinite");
    global_env_->set("isFinite", Value::object(ObjectPtr(vm_is_finite_fn)));

    // ---- Number constructor ----

    number_constructor_ = RcPtr<JSFunction>::make();
    number_constructor_->set_name(std::string("Number"));
    number_constructor_->set_native_fn([](Value /*this_val*/, std::vector<Value> args,
                                          bool /*is_new*/) -> EvalResult {
        double n = args.empty() ? 0.0 : to_number_double_vm(args[0]);
        return EvalResult::ok(Value::number(n));
    });

    // Number.isNaN (no ToNumber conversion)
    auto vm_num_is_nan_fn = RcPtr<JSFunction>::make();
    vm_num_is_nan_fn->set_name(std::string("isNaN"));
    vm_num_is_nan_fn->set_native_fn([](Value /*this_val*/, std::vector<Value> args, bool) -> EvalResult {
        if (args.empty() || !args[0].is_number()) return EvalResult::ok(Value::boolean(false));
        return EvalResult::ok(Value::boolean(std::isnan(args[0].as_number())));
    });
    number_constructor_->set_property("isNaN", Value::object(ObjectPtr(vm_num_is_nan_fn)));

    // Number.isFinite (no ToNumber conversion)
    auto vm_num_is_finite_fn = RcPtr<JSFunction>::make();
    vm_num_is_finite_fn->set_name(std::string("isFinite"));
    vm_num_is_finite_fn->set_native_fn([](Value /*this_val*/, std::vector<Value> args, bool) -> EvalResult {
        if (args.empty() || !args[0].is_number()) return EvalResult::ok(Value::boolean(false));
        return EvalResult::ok(Value::boolean(std::isfinite(args[0].as_number())));
    });
    number_constructor_->set_property("isFinite", Value::object(ObjectPtr(vm_num_is_finite_fn)));

    // Number.isInteger
    auto vm_num_is_integer_fn = RcPtr<JSFunction>::make();
    vm_num_is_integer_fn->set_name(std::string("isInteger"));
    vm_num_is_integer_fn->set_native_fn([](Value /*this_val*/, std::vector<Value> args, bool) -> EvalResult {
        if (args.empty() || !args[0].is_number()) return EvalResult::ok(Value::boolean(false));
        double n = args[0].as_number();
        if (std::isnan(n) || std::isinf(n)) return EvalResult::ok(Value::boolean(false));
        return EvalResult::ok(Value::boolean(std::trunc(n) == n));
    });
    number_constructor_->set_property("isInteger", Value::object(ObjectPtr(vm_num_is_integer_fn)));

    // Number.parseInt === global parseInt (same object)
    number_constructor_->set_property("parseInt", vm_parse_int_val);

    // Number.prototype
    number_prototype_ = RcPtr<JSObject>::make();
    number_prototype_->set_proto(object_prototype_);
    gc_heap_.Register(number_prototype_.get());
    number_constructor_->set_prototype_obj(RcPtr<JSObject>(number_prototype_));
    number_constructor_->set_property("prototype", Value::object(ObjectPtr(number_prototype_)));

    gc_heap_.Register(number_constructor_.get());
    global_env_->define_initialized("Number");
    global_env_->set("Number", Value::object(ObjectPtr(number_constructor_)));

    // ---- Boolean constructor ----

    boolean_constructor_ = RcPtr<JSFunction>::make();
    boolean_constructor_->set_name(std::string("Boolean"));
    boolean_constructor_->set_native_fn([](Value /*this_val*/, std::vector<Value> args,
                                           bool /*is_new*/) -> EvalResult {
        bool b = args.empty() ? false : to_boolean(args[0]);
        return EvalResult::ok(Value::boolean(b));
    });

    gc_heap_.Register(boolean_constructor_.get());
    global_env_->define_initialized("Boolean");
    global_env_->set("Boolean", Value::object(ObjectPtr(boolean_constructor_)));

    // ---- String constructor ----

    string_constructor_ = RcPtr<JSFunction>::make();
    string_constructor_->set_name(std::string("String"));
    string_constructor_->set_native_fn([](Value /*this_val*/, std::vector<Value> args,
                                          bool /*is_new*/) -> EvalResult {
        std::string s = args.empty() ? std::string("") : to_string_val(args[0]);
        return EvalResult::ok(Value::string(s));
    });

    gc_heap_.Register(string_constructor_.get());
    global_env_->define_initialized("String");
    global_env_->set("String", Value::object(ObjectPtr(string_constructor_)));

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
    auto vm_math_floor_fn = RcPtr<JSFunction>::make();
    vm_math_floor_fn->set_native_fn([](Value, std::vector<Value> args, bool) -> EvalResult {
        double x = args.empty() ? std::numeric_limits<double>::quiet_NaN()
                                : to_number_double_vm(args[0]);
        return EvalResult::ok(Value::number(std::floor(x)));
    });
    math_obj_->set_property("floor", Value::object(ObjectPtr(vm_math_floor_fn)));

    // Math.ceil
    auto vm_math_ceil_fn = RcPtr<JSFunction>::make();
    vm_math_ceil_fn->set_native_fn([](Value, std::vector<Value> args, bool) -> EvalResult {
        double x = args.empty() ? std::numeric_limits<double>::quiet_NaN()
                                : to_number_double_vm(args[0]);
        return EvalResult::ok(Value::number(std::ceil(x)));
    });
    math_obj_->set_property("ceil", Value::object(ObjectPtr(vm_math_ceil_fn)));

    // Math.round: spec tie-breaking: x.5 rounds toward +Infinity; -0.5 → -0
    auto vm_math_round_fn = RcPtr<JSFunction>::make();
    vm_math_round_fn->set_native_fn([](Value, std::vector<Value> args, bool) -> EvalResult {
        double x = args.empty() ? std::numeric_limits<double>::quiet_NaN()
                                : to_number_double_vm(args[0]);
        if (std::isnan(x) || std::isinf(x) || x == 0.0) return EvalResult::ok(Value::number(x));
        double r = std::floor(x + 0.5);
        if (r == 0.0 && x < 0.0) return EvalResult::ok(Value::number(-0.0));
        return EvalResult::ok(Value::number(r));
    });
    math_obj_->set_property("round", Value::object(ObjectPtr(vm_math_round_fn)));

    // Math.abs
    auto vm_math_abs_fn = RcPtr<JSFunction>::make();
    vm_math_abs_fn->set_native_fn([](Value, std::vector<Value> args, bool) -> EvalResult {
        double x = args.empty() ? std::numeric_limits<double>::quiet_NaN()
                                : to_number_double_vm(args[0]);
        return EvalResult::ok(Value::number(std::abs(x)));
    });
    math_obj_->set_property("abs", Value::object(ObjectPtr(vm_math_abs_fn)));

    // Math.max
    auto vm_math_max_fn = RcPtr<JSFunction>::make();
    vm_math_max_fn->set_native_fn([](Value, std::vector<Value> args, bool) -> EvalResult {
        double result = -std::numeric_limits<double>::infinity();
        for (auto& arg : args) {
            double v = to_number_double_vm(arg);
            if (std::isnan(v)) return EvalResult::ok(Value::number(v));
            // ES: n > highest, or n is +0 and highest is -0
            if (v > result || (v == 0.0 && !std::signbit(v) && std::signbit(result))) result = v;
        }
        return EvalResult::ok(Value::number(result));
    });
    math_obj_->set_property("max", Value::object(ObjectPtr(vm_math_max_fn)));

    // Math.min
    auto vm_math_min_fn = RcPtr<JSFunction>::make();
    vm_math_min_fn->set_native_fn([](Value, std::vector<Value> args, bool) -> EvalResult {
        double result = std::numeric_limits<double>::infinity();
        for (auto& arg : args) {
            double v = to_number_double_vm(arg);
            if (std::isnan(v)) return EvalResult::ok(Value::number(v));
            // ES: n < lowest, or n is -0
            if (v < result || (v == 0.0 && std::signbit(v))) result = v;
        }
        return EvalResult::ok(Value::number(result));
    });
    math_obj_->set_property("min", Value::object(ObjectPtr(vm_math_min_fn)));

    // Math.pow
    auto vm_math_pow_fn = RcPtr<JSFunction>::make();
    vm_math_pow_fn->set_native_fn([](Value, std::vector<Value> args, bool) -> EvalResult {
        double base = args.size() >= 1 ? to_number_double_vm(args[0])
                                       : std::numeric_limits<double>::quiet_NaN();
        double exp = args.size() >= 2 ? to_number_double_vm(args[1])
                                      : std::numeric_limits<double>::quiet_NaN();
        return EvalResult::ok(Value::number(std::pow(base, exp)));
    });
    math_obj_->set_property("pow", Value::object(ObjectPtr(vm_math_pow_fn)));

    // Math.sqrt
    auto vm_math_sqrt_fn = RcPtr<JSFunction>::make();
    vm_math_sqrt_fn->set_native_fn([](Value, std::vector<Value> args, bool) -> EvalResult {
        double x = args.empty() ? std::numeric_limits<double>::quiet_NaN()
                                : to_number_double_vm(args[0]);
        return EvalResult::ok(Value::number(std::sqrt(x)));
    });
    math_obj_->set_property("sqrt", Value::object(ObjectPtr(vm_math_sqrt_fn)));

    // Math.log
    auto vm_math_log_fn = RcPtr<JSFunction>::make();
    vm_math_log_fn->set_native_fn([](Value, std::vector<Value> args, bool) -> EvalResult {
        double x = args.empty() ? std::numeric_limits<double>::quiet_NaN()
                                : to_number_double_vm(args[0]);
        return EvalResult::ok(Value::number(std::log(x)));
    });
    math_obj_->set_property("log", Value::object(ObjectPtr(vm_math_log_fn)));

    // Math.trunc
    auto vm_math_trunc_fn = RcPtr<JSFunction>::make();
    vm_math_trunc_fn->set_native_fn([](Value, std::vector<Value> args, bool) -> EvalResult {
        double x = args.empty() ? std::numeric_limits<double>::quiet_NaN()
                                : to_number_double_vm(args[0]);
        return EvalResult::ok(Value::number(std::trunc(x)));
    });
    math_obj_->set_property("trunc", Value::object(ObjectPtr(vm_math_trunc_fn)));

    // Math.sign
    auto vm_math_sign_fn = RcPtr<JSFunction>::make();
    vm_math_sign_fn->set_native_fn([](Value, std::vector<Value> args, bool) -> EvalResult {
        double x = args.empty() ? std::numeric_limits<double>::quiet_NaN()
                                : to_number_double_vm(args[0]);
        if (std::isnan(x)) return EvalResult::ok(Value::number(x));
        if (x == 0.0) return EvalResult::ok(Value::number(x));  // preserves +0/-0
        return EvalResult::ok(Value::number(x > 0.0 ? 1.0 : -1.0));
    });
    math_obj_->set_property("sign", Value::object(ObjectPtr(vm_math_sign_fn)));

    // Math.random (xorshift64*)
    auto vm_math_random_fn = RcPtr<JSFunction>::make();
    vm_math_random_fn->set_native_fn([this](Value, std::vector<Value> /*args*/, bool) -> EvalResult {
        math_random_state_ ^= math_random_state_ >> 12;
        math_random_state_ ^= math_random_state_ << 25;
        math_random_state_ ^= math_random_state_ >> 27;
        uint64_t r = math_random_state_ * 0x2545F4914F6CDD1DULL;
        double result = static_cast<double>(r >> 11) / static_cast<double>(1ULL << 53);
        return EvalResult::ok(Value::number(result));
    });
    math_obj_->set_property("random", Value::object(ObjectPtr(vm_math_random_fn)));

    gc_heap_.Register(math_obj_.get());
    global_env_->define("Math", VarKind::Const);
    global_env_->initialize("Math", Value::object(ObjectPtr(math_obj_)));

    // Register the global environment with GcHeap.
    gc_heap_.Register(global_env_.get());

    // ---- RegExp constructor stub ----
    {
        auto regexp_fn = RcPtr<JSFunction>::make();
        regexp_fn->set_name(std::string("RegExp"));
        regexp_fn->set_native_fn([this](Value /*this_val*/, std::vector<Value> /*args*/, bool /*is_new_call*/) -> EvalResult {
            auto obj = RcPtr<JSObject>::make();
            obj->set_proto(object_prototype_);
            gc_heap_.Register(obj.get());
            return EvalResult::ok(Value::object(ObjectPtr(obj)));
        });
        gc_heap_.Register(regexp_fn.get());
        global_env_->define("RegExp", VarKind::Const);
        global_env_->initialize("RegExp", Value::object(ObjectPtr(regexp_fn)));
    }

    // ---- Date constructor stub ----
    {
        auto date_fn = RcPtr<JSFunction>::make();
        date_fn->set_name(std::string("Date"));
        date_fn->set_native_fn([this](Value /*this_val*/, std::vector<Value> /*args*/, bool is_new_call) -> EvalResult {
            if (is_new_call) {
                auto obj = RcPtr<JSObject>::make();
                obj->set_proto(object_prototype_);
                gc_heap_.Register(obj.get());
                return EvalResult::ok(Value::object(ObjectPtr(obj)));
            }
            auto now = std::chrono::system_clock::now();
            auto time_t_now = std::chrono::system_clock::to_time_t(now);
            std::string date_str = std::ctime(&time_t_now);
            if (!date_str.empty() && date_str.back() == '\n') date_str.pop_back();
            return EvalResult::ok(Value::string(date_str));
        });
        gc_heap_.Register(date_fn.get());
        global_env_->define("Date", VarKind::Const);
        global_env_->initialize("Date", Value::object(ObjectPtr(date_fn)));
    }

    // ---- JSON object stub ----
    {
        auto json_obj = RcPtr<JSObject>::make();
        json_obj->set_proto(object_prototype_);

        auto stringify_fn = RcPtr<JSFunction>::make();
        stringify_fn->set_name(std::string("stringify"));
        stringify_fn->set_native_fn([](Value /*this_val*/, std::vector<Value> args, bool) -> EvalResult {
            if (args.empty()) return EvalResult::ok(Value::undefined());
            const auto& v = args[0];
            if (v.is_string()) {
                std::string s = v.as_string();
                return EvalResult::ok(Value::string("\"" + s + "\""));
            }
            if (v.is_number()) {
                return EvalResult::ok(Value::string(VM::to_string_val(v)));
            }
            return EvalResult::ok(Value::string("{}"));
        });
        json_obj->set_property("stringify", Value::object(ObjectPtr(stringify_fn)));

        auto parse_fn = RcPtr<JSFunction>::make();
        parse_fn->set_name(std::string("parse"));
        parse_fn->set_native_fn([](Value /*this_val*/, std::vector<Value> /*args*/, bool) -> EvalResult {
            return EvalResult::err(Error{ErrorKind::Runtime, "JSON.parse not implemented"});
        });
        json_obj->set_property("parse", Value::object(ObjectPtr(parse_fn)));

        gc_heap_.Register(json_obj.get());
        global_env_->define("JSON", VarKind::Const);
        global_env_->initialize("JSON", Value::object(ObjectPtr(json_obj)));
    }
}

// ---- VM Promise helpers ----

RcPtr<JSPromise> VM::vm_promise_resolve(Value value) {
    if (value.is_object() && value.as_object_raw() &&
        value.as_object_raw()->object_kind() == ObjectKind::kPromise) {
        return RcPtr<JSPromise>(static_cast<JSPromise*>(value.as_object_raw()));
    }
    auto p = RcPtr<JSPromise>::make();
    gc_heap_.Register(p.get());
    p->Fulfill(std::move(value), job_queue_);
    return p;
}

void VM::vm_execute_reaction_job(ReactionJob job) {
    Value handler = std::move(job.handler);
    Value capability_val = std::move(job.capability);
    Value arg = std::move(job.arg);
    bool is_fulfill = job.is_fulfill;

    RcPtr<JSPromise> cap_rc;
    if (capability_val.is_object() && capability_val.as_object_raw() &&
        capability_val.as_object_raw()->object_kind() == ObjectKind::kPromise) {
        cap_rc = RcPtr<JSPromise>(static_cast<JSPromise*>(capability_val.as_object_raw()));
    }

    bool handler_is_fn = handler.is_object() && handler.as_object_raw() &&
                         handler.as_object_raw()->object_kind() == ObjectKind::kFunction;

    if (!handler_is_fn) {
        if (cap_rc) {
            if (is_fulfill) {
                cap_rc->Fulfill(arg, job_queue_);
            } else {
                cap_rc->Reject(arg, job_queue_);
            }
        }
        return;
    }

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
                auto inner_rc = RcPtr<JSPromise>(inner);
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
        Value thrown_val;
        if (native_pending_throw_.has_value()) {
            // Native fn (e.g., finally reject_wrapper) stored throw value here.
            thrown_val = std::move(*native_pending_throw_);
            native_pending_throw_ = std::nullopt;
        } else if (!call_stack_.empty() && call_stack_.back().pending_throw.has_value()) {
            thrown_val = std::move(*call_stack_.back().pending_throw);
            call_stack_.back().pending_throw = std::nullopt;
        } else {
            thrown_val = Value::string(result.error().message());
        }
        cap_rc->Reject(std::move(thrown_val), job_queue_);
    }
}

void VM::vm_drain_job_queue() {
    job_queue_.DrainAll([this](ReactionJob job) {
        vm_execute_reaction_job(std::move(job));
    });
}

void VM::vm_handle_async_result(EvalResult body_result, RcPtr<JSPromise> outer_promise) {
    // Handle nested suspension (multiple awaits in sequence)
    while (!body_result.is_ok() &&
           body_result.error().message() == kAsyncSuspendSentinel) {
        vm_async_suspended_ = false;
        if (!vm_pending_inner_promise_.has_value() || !vm_suspended_frame_.has_value()) {
            outer_promise->Reject(
                make_error_value(NativeErrorType::kTypeError,
                    "internal: missing suspend state"),
                job_queue_);
            return;
        }
        auto inner_promise = std::move(*vm_pending_inner_promise_);
        vm_pending_inner_promise_ = std::nullopt;

        auto shared_frame = std::make_shared<CallFrame>(std::move(*vm_suspended_frame_));
        vm_suspended_frame_ = std::nullopt;

        // Build resume_fn
        auto resume_fn = RcPtr<JSFunction>::make();
        resume_fn->set_property("__resume_env__",
            Value::object(ObjectPtr(shared_frame->env)));
        resume_fn->set_property("__resume_promise__",
            Value::object(ObjectPtr(outer_promise)));
        resume_fn->set_native_fn([this, outer_promise, shared_frame](
                Value, std::vector<Value> args, bool) mutable -> EvalResult {
            Value fulfilled_val = args.empty() ? Value::undefined() : args[0];
            shared_frame->stack.push_back(std::move(fulfilled_val));
            call_stack_.push_back(std::move(*shared_frame));
            call_depth_++;
            size_t ed = call_stack_.size() - 1;
            EvalResult res = run(ed);
            vm_handle_async_result(res, outer_promise);
            return EvalResult::ok(Value::undefined());
        });
        gc_heap_.Register(resume_fn.get());

        // Build reject_fn
        auto reject_fn = RcPtr<JSFunction>::make();
        reject_fn->set_property("__resume_env__",
            Value::object(ObjectPtr(shared_frame->env)));
        reject_fn->set_property("__resume_promise__",
            Value::object(ObjectPtr(outer_promise)));
        reject_fn->set_native_fn([this, outer_promise, shared_frame](
                Value, std::vector<Value> args, bool) mutable -> EvalResult {
            Value reason = args.empty() ? Value::undefined() : args[0];
            shared_frame->pending_throw = std::move(reason);
            shared_frame->stack.push_back(Value::undefined());
            call_stack_.push_back(std::move(*shared_frame));
            call_depth_++;
            size_t ed = call_stack_.size() - 1;
            EvalResult res = run(ed);
            vm_handle_async_result(res, outer_promise);
            return EvalResult::ok(Value::undefined());
        });
        gc_heap_.Register(reject_fn.get());

        JSPromise::PerformThen(inner_promise,
            Value::object(ObjectPtr(resume_fn)),
            Value::object(ObjectPtr(reject_fn)),
            job_queue_);
        return;
    }

    // Normal completion or error
    if (body_result.is_ok()) {
        Value ret = body_result.value();
        if (ret.is_object() && ret.as_object_raw() &&
            ret.as_object_raw()->object_kind() == ObjectKind::kPromise) {
            auto* inner_p = static_cast<JSPromise*>(ret.as_object_raw());
            auto inner_rc = RcPtr<JSPromise>(inner_p);
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
            outer_promise->Fulfill(std::move(ret), job_queue_);
        }
    } else {
        // Error from the body: extract throw value
        Value thrown_val;
        if (native_pending_throw_.has_value()) {
            thrown_val = std::move(*native_pending_throw_);
            native_pending_throw_ = std::nullopt;
        } else if (!call_stack_.empty() && call_stack_.back().pending_throw.has_value()) {
            thrown_val = std::move(*call_stack_.back().pending_throw);
            call_stack_.back().pending_throw = std::nullopt;
        } else {
            thrown_val = Value::string(body_result.error().message());
        }
        outer_promise->Reject(std::move(thrown_val), job_queue_);
    }
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

    // Drain microtasks before GC
    vm_drain_job_queue();

    // Re-read the last expression variable after DrainAll to pick up microtask side effects.
    // If the last statement is a simple identifier expression (e.g., `result;`),
    // re-read its value from the global environment after DrainAll.
    if (result.is_ok() && bytecode->last_expr_name.has_value()) {
        const auto& var_name = *bytecode->last_expr_name;
        if (var_name != "undefined") {
            auto lookup = global_env_->lookup(var_name);
            if (lookup && lookup->initialized) {
                result = EvalResult::ok(lookup->cell->value);
            }
        }
    }

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
        add_obj(promise_prototype_.get());
        add_obj(string_prototype_.get());
        add_obj(math_obj_.get());
        add_obj(number_prototype_.get());
        add_obj(object_constructor_.get());
        add_obj(number_constructor_.get());
        add_obj(boolean_constructor_.get());
        add_obj(string_constructor_.get());
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
        // Include job queue roots
        std::vector<Value> jq_vals;
        job_queue_.CollectRoots(jq_vals);
        for (const auto& v : jq_vals) add_val(v);

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
    if (boolean_constructor_) boolean_constructor_->clear_own_properties();
    if (string_constructor_) string_constructor_->clear_own_properties();

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
    frame.current_module = fn->defining_module();

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

        case Opcode::kArrayHole: {
            // Elision hole: increment array_length_ without writing an element.
            // Stack top must be the array object (placed there by kDup before this opcode).
            Value arr_val = std::move(stack.back());
            stack.pop_back();
            if (arr_val.is_object()) {
                RcObject* raw_hole = arr_val.as_object_raw();
                if (raw_hole && raw_hole->object_kind() == ObjectKind::kArray) {
                    auto* arr = static_cast<JSObject*>(raw_hole);
                    arr->array_length_++;
                }
            }
            break;
        }

        // ---- Variable update (++/--) ----

        case Opcode::kVarPreInc: {
            uint16_t idx = read_u16(bc, pc);
            const std::string& name = bc->names[idx];
            auto get_result = env->get(name);
            if (!get_result.is_ok()) {
                const std::string& msg = get_result.error().message();
                NativeErrorType err_type = NativeErrorType::kReferenceError;
                if (msg.rfind("TypeError:", 0) == 0) err_type = NativeErrorType::kTypeError;
                frame.pending_throw = make_error_value(err_type, strip_error_prefix(msg));
                continue;
            }
            double old_d = to_number_double_vm(get_result.value());
            double new_d = old_d + 1.0;
            auto set_result = env->set(name, Value::number(new_d));
            if (!set_result.is_ok()) {
                const std::string& msg = set_result.error().message();
                NativeErrorType err_type = NativeErrorType::kTypeError;
                if (msg.rfind("RangeError:", 0) == 0) err_type = NativeErrorType::kRangeError;
                frame.pending_throw = make_error_value(err_type, strip_error_prefix(msg));
                continue;
            }
            stack.push_back(Value::number(new_d));
            break;
        }
        case Opcode::kVarPreDec: {
            uint16_t idx = read_u16(bc, pc);
            const std::string& name = bc->names[idx];
            auto get_result = env->get(name);
            if (!get_result.is_ok()) {
                const std::string& msg = get_result.error().message();
                NativeErrorType err_type = NativeErrorType::kReferenceError;
                if (msg.rfind("TypeError:", 0) == 0) err_type = NativeErrorType::kTypeError;
                frame.pending_throw = make_error_value(err_type, strip_error_prefix(msg));
                continue;
            }
            double old_d = to_number_double_vm(get_result.value());
            double new_d = old_d - 1.0;
            auto set_result = env->set(name, Value::number(new_d));
            if (!set_result.is_ok()) {
                const std::string& msg = set_result.error().message();
                NativeErrorType err_type = NativeErrorType::kTypeError;
                if (msg.rfind("RangeError:", 0) == 0) err_type = NativeErrorType::kRangeError;
                frame.pending_throw = make_error_value(err_type, strip_error_prefix(msg));
                continue;
            }
            stack.push_back(Value::number(new_d));
            break;
        }
        case Opcode::kVarPostInc: {
            uint16_t idx = read_u16(bc, pc);
            const std::string& name = bc->names[idx];
            auto get_result = env->get(name);
            if (!get_result.is_ok()) {
                const std::string& msg = get_result.error().message();
                NativeErrorType err_type = NativeErrorType::kReferenceError;
                if (msg.rfind("TypeError:", 0) == 0) err_type = NativeErrorType::kTypeError;
                frame.pending_throw = make_error_value(err_type, strip_error_prefix(msg));
                continue;
            }
            double old_d = to_number_double_vm(get_result.value());
            double new_d = old_d + 1.0;
            auto set_result = env->set(name, Value::number(new_d));
            if (!set_result.is_ok()) {
                const std::string& msg = set_result.error().message();
                NativeErrorType err_type = NativeErrorType::kTypeError;
                if (msg.rfind("RangeError:", 0) == 0) err_type = NativeErrorType::kRangeError;
                frame.pending_throw = make_error_value(err_type, strip_error_prefix(msg));
                continue;
            }
            stack.push_back(Value::number(old_d));
            break;
        }
        case Opcode::kVarPostDec: {
            uint16_t idx = read_u16(bc, pc);
            const std::string& name = bc->names[idx];
            auto get_result = env->get(name);
            if (!get_result.is_ok()) {
                const std::string& msg = get_result.error().message();
                NativeErrorType err_type = NativeErrorType::kReferenceError;
                if (msg.rfind("TypeError:", 0) == 0) err_type = NativeErrorType::kTypeError;
                frame.pending_throw = make_error_value(err_type, strip_error_prefix(msg));
                continue;
            }
            double old_d = to_number_double_vm(get_result.value());
            double new_d = old_d - 1.0;
            auto set_result = env->set(name, Value::number(new_d));
            if (!set_result.is_ok()) {
                const std::string& msg = set_result.error().message();
                NativeErrorType err_type = NativeErrorType::kTypeError;
                if (msg.rfind("RangeError:", 0) == 0) err_type = NativeErrorType::kRangeError;
                frame.pending_throw = make_error_value(err_type, strip_error_prefix(msg));
                continue;
            }
            stack.push_back(Value::number(old_d));
            break;
        }

        // ---- Property update (++/--) ----

        case Opcode::kPropPreInc: {
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
                stack.push_back(Value::number(std::numeric_limits<double>::quiet_NaN()));
                continue;
            }
            RcObject* raw = obj_val.as_object_raw();
            Value old_val = Value::undefined();
            if (raw->object_kind() == ObjectKind::kFunction) {
                auto* fn = static_cast<JSFunction*>(raw);
                old_val = fn->get_property(name);
                if (old_val.is_undefined() && function_prototype_) {
                    old_val = function_prototype_->get_property(name);
                }
            } else if (raw->object_kind() == ObjectKind::kOrdinary || raw->object_kind() == ObjectKind::kArray) {
                old_val = static_cast<JSObject*>(raw)->get_property(name);
            } else {
                stack.push_back(Value::number(std::numeric_limits<double>::quiet_NaN()));
                continue;
            }
            double old_d = to_number_double_vm(old_val);
            double new_d = old_d + 1.0;
            if (raw->object_kind() == ObjectKind::kFunction) {
                static_cast<JSFunction*>(raw)->set_property(name, Value::number(new_d));
            } else {
                auto set_ex_res = static_cast<JSObject*>(raw)->set_property_ex(name, Value::number(new_d));
                if (!set_ex_res.is_ok()) {
                    const std::string& msg = set_ex_res.error().message();
                    NativeErrorType err_type = NativeErrorType::kRangeError;
                    if (msg.rfind("TypeError:", 0) == 0) err_type = NativeErrorType::kTypeError;
                    frame.pending_throw = make_error_value(err_type, strip_error_prefix(msg));
                    continue;
                }
            }
            stack.push_back(Value::number(new_d));
            break;
        }
        case Opcode::kPropPreDec: {
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
                stack.push_back(Value::number(std::numeric_limits<double>::quiet_NaN()));
                continue;
            }
            RcObject* raw = obj_val.as_object_raw();
            Value old_val = Value::undefined();
            if (raw->object_kind() == ObjectKind::kFunction) {
                auto* fn = static_cast<JSFunction*>(raw);
                old_val = fn->get_property(name);
                if (old_val.is_undefined() && function_prototype_) {
                    old_val = function_prototype_->get_property(name);
                }
            } else if (raw->object_kind() == ObjectKind::kOrdinary || raw->object_kind() == ObjectKind::kArray) {
                old_val = static_cast<JSObject*>(raw)->get_property(name);
            } else {
                stack.push_back(Value::number(std::numeric_limits<double>::quiet_NaN()));
                continue;
            }
            double old_d = to_number_double_vm(old_val);
            double new_d = old_d - 1.0;
            if (raw->object_kind() == ObjectKind::kFunction) {
                static_cast<JSFunction*>(raw)->set_property(name, Value::number(new_d));
            } else {
                auto set_ex_res = static_cast<JSObject*>(raw)->set_property_ex(name, Value::number(new_d));
                if (!set_ex_res.is_ok()) {
                    const std::string& msg = set_ex_res.error().message();
                    NativeErrorType err_type = NativeErrorType::kRangeError;
                    if (msg.rfind("TypeError:", 0) == 0) err_type = NativeErrorType::kTypeError;
                    frame.pending_throw = make_error_value(err_type, strip_error_prefix(msg));
                    continue;
                }
            }
            stack.push_back(Value::number(new_d));
            break;
        }
        case Opcode::kPropPostInc: {
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
                stack.push_back(Value::number(std::numeric_limits<double>::quiet_NaN()));
                continue;
            }
            RcObject* raw = obj_val.as_object_raw();
            Value old_val = Value::undefined();
            if (raw->object_kind() == ObjectKind::kFunction) {
                auto* fn = static_cast<JSFunction*>(raw);
                old_val = fn->get_property(name);
                if (old_val.is_undefined() && function_prototype_) {
                    old_val = function_prototype_->get_property(name);
                }
            } else if (raw->object_kind() == ObjectKind::kOrdinary || raw->object_kind() == ObjectKind::kArray) {
                old_val = static_cast<JSObject*>(raw)->get_property(name);
            } else {
                stack.push_back(Value::number(std::numeric_limits<double>::quiet_NaN()));
                continue;
            }
            double old_d = to_number_double_vm(old_val);
            double new_d = old_d + 1.0;
            if (raw->object_kind() == ObjectKind::kFunction) {
                static_cast<JSFunction*>(raw)->set_property(name, Value::number(new_d));
            } else {
                auto set_ex_res = static_cast<JSObject*>(raw)->set_property_ex(name, Value::number(new_d));
                if (!set_ex_res.is_ok()) {
                    const std::string& msg = set_ex_res.error().message();
                    NativeErrorType err_type = NativeErrorType::kRangeError;
                    if (msg.rfind("TypeError:", 0) == 0) err_type = NativeErrorType::kTypeError;
                    frame.pending_throw = make_error_value(err_type, strip_error_prefix(msg));
                    continue;
                }
            }
            stack.push_back(Value::number(old_d));
            break;
        }
        case Opcode::kPropPostDec: {
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
                stack.push_back(Value::number(std::numeric_limits<double>::quiet_NaN()));
                continue;
            }
            RcObject* raw = obj_val.as_object_raw();
            Value old_val = Value::undefined();
            if (raw->object_kind() == ObjectKind::kFunction) {
                auto* fn = static_cast<JSFunction*>(raw);
                old_val = fn->get_property(name);
                if (old_val.is_undefined() && function_prototype_) {
                    old_val = function_prototype_->get_property(name);
                }
            } else if (raw->object_kind() == ObjectKind::kOrdinary || raw->object_kind() == ObjectKind::kArray) {
                old_val = static_cast<JSObject*>(raw)->get_property(name);
            } else {
                stack.push_back(Value::number(std::numeric_limits<double>::quiet_NaN()));
                continue;
            }
            double old_d = to_number_double_vm(old_val);
            double new_d = old_d - 1.0;
            if (raw->object_kind() == ObjectKind::kFunction) {
                static_cast<JSFunction*>(raw)->set_property(name, Value::number(new_d));
            } else {
                auto set_ex_res = static_cast<JSObject*>(raw)->set_property_ex(name, Value::number(new_d));
                if (!set_ex_res.is_ok()) {
                    const std::string& msg = set_ex_res.error().message();
                    NativeErrorType err_type = NativeErrorType::kRangeError;
                    if (msg.rfind("TypeError:", 0) == 0) err_type = NativeErrorType::kTypeError;
                    frame.pending_throw = make_error_value(err_type, strip_error_prefix(msg));
                    continue;
                }
            }
            stack.push_back(Value::number(old_d));
            break;
        }

        // ---- Element update (++/--) ----

        case Opcode::kElemPreInc: {
            Value key_val = std::move(stack.back());
            stack.pop_back();
            Value obj_val = std::move(stack.back());
            stack.pop_back();
            if (obj_val.is_undefined() || obj_val.is_null()) {
                frame.pending_throw = make_error_value(NativeErrorType::kTypeError,
                    "Cannot read property '" + to_string_val(key_val) + "' of " + to_string_val(obj_val));
                continue;
            }
            if (!obj_val.is_object()) {
                stack.push_back(Value::number(std::numeric_limits<double>::quiet_NaN()));
                continue;
            }
            std::string key = to_string_val(key_val);
            RcObject* raw = obj_val.as_object_raw();
            Value old_val = Value::undefined();
            if (raw->object_kind() == ObjectKind::kOrdinary || raw->object_kind() == ObjectKind::kArray) {
                old_val = static_cast<JSObject*>(raw)->get_property(key);
            } else {
                stack.push_back(Value::number(std::numeric_limits<double>::quiet_NaN()));
                continue;
            }
            double old_d = to_number_double_vm(old_val);
            double new_d = old_d + 1.0;
            auto set_ex_res = static_cast<JSObject*>(raw)->set_property_ex(key, Value::number(new_d));
            if (!set_ex_res.is_ok()) {
                const std::string& msg = set_ex_res.error().message();
                NativeErrorType err_type = NativeErrorType::kRangeError;
                if (msg.rfind("TypeError:", 0) == 0) err_type = NativeErrorType::kTypeError;
                frame.pending_throw = make_error_value(err_type, strip_error_prefix(msg));
                continue;
            }
            stack.push_back(Value::number(new_d));
            break;
        }
        case Opcode::kElemPreDec: {
            Value key_val = std::move(stack.back());
            stack.pop_back();
            Value obj_val = std::move(stack.back());
            stack.pop_back();
            if (obj_val.is_undefined() || obj_val.is_null()) {
                frame.pending_throw = make_error_value(NativeErrorType::kTypeError,
                    "Cannot read property '" + to_string_val(key_val) + "' of " + to_string_val(obj_val));
                continue;
            }
            if (!obj_val.is_object()) {
                stack.push_back(Value::number(std::numeric_limits<double>::quiet_NaN()));
                continue;
            }
            std::string key = to_string_val(key_val);
            RcObject* raw = obj_val.as_object_raw();
            Value old_val = Value::undefined();
            if (raw->object_kind() == ObjectKind::kOrdinary || raw->object_kind() == ObjectKind::kArray) {
                old_val = static_cast<JSObject*>(raw)->get_property(key);
            } else {
                stack.push_back(Value::number(std::numeric_limits<double>::quiet_NaN()));
                continue;
            }
            double old_d = to_number_double_vm(old_val);
            double new_d = old_d - 1.0;
            auto set_ex_res = static_cast<JSObject*>(raw)->set_property_ex(key, Value::number(new_d));
            if (!set_ex_res.is_ok()) {
                const std::string& msg = set_ex_res.error().message();
                NativeErrorType err_type = NativeErrorType::kRangeError;
                if (msg.rfind("TypeError:", 0) == 0) err_type = NativeErrorType::kTypeError;
                frame.pending_throw = make_error_value(err_type, strip_error_prefix(msg));
                continue;
            }
            stack.push_back(Value::number(new_d));
            break;
        }
        case Opcode::kElemPostInc: {
            Value key_val = std::move(stack.back());
            stack.pop_back();
            Value obj_val = std::move(stack.back());
            stack.pop_back();
            if (obj_val.is_undefined() || obj_val.is_null()) {
                frame.pending_throw = make_error_value(NativeErrorType::kTypeError,
                    "Cannot read property '" + to_string_val(key_val) + "' of " + to_string_val(obj_val));
                continue;
            }
            if (!obj_val.is_object()) {
                stack.push_back(Value::number(std::numeric_limits<double>::quiet_NaN()));
                continue;
            }
            std::string key = to_string_val(key_val);
            RcObject* raw = obj_val.as_object_raw();
            Value old_val = Value::undefined();
            if (raw->object_kind() == ObjectKind::kOrdinary || raw->object_kind() == ObjectKind::kArray) {
                old_val = static_cast<JSObject*>(raw)->get_property(key);
            } else {
                stack.push_back(Value::number(std::numeric_limits<double>::quiet_NaN()));
                continue;
            }
            double old_d = to_number_double_vm(old_val);
            double new_d = old_d + 1.0;
            auto set_ex_res = static_cast<JSObject*>(raw)->set_property_ex(key, Value::number(new_d));
            if (!set_ex_res.is_ok()) {
                const std::string& msg = set_ex_res.error().message();
                NativeErrorType err_type = NativeErrorType::kRangeError;
                if (msg.rfind("TypeError:", 0) == 0) err_type = NativeErrorType::kTypeError;
                frame.pending_throw = make_error_value(err_type, strip_error_prefix(msg));
                continue;
            }
            stack.push_back(Value::number(old_d));
            break;
        }
        case Opcode::kElemPostDec: {
            Value key_val = std::move(stack.back());
            stack.pop_back();
            Value obj_val = std::move(stack.back());
            stack.pop_back();
            if (obj_val.is_undefined() || obj_val.is_null()) {
                frame.pending_throw = make_error_value(NativeErrorType::kTypeError,
                    "Cannot read property '" + to_string_val(key_val) + "' of " + to_string_val(obj_val));
                continue;
            }
            if (!obj_val.is_object()) {
                stack.push_back(Value::number(std::numeric_limits<double>::quiet_NaN()));
                continue;
            }
            std::string key = to_string_val(key_val);
            RcObject* raw = obj_val.as_object_raw();
            Value old_val = Value::undefined();
            if (raw->object_kind() == ObjectKind::kOrdinary || raw->object_kind() == ObjectKind::kArray) {
                old_val = static_cast<JSObject*>(raw)->get_property(key);
            } else {
                stack.push_back(Value::number(std::numeric_limits<double>::quiet_NaN()));
                continue;
            }
            double old_d = to_number_double_vm(old_val);
            double new_d = old_d - 1.0;
            auto set_ex_res = static_cast<JSObject*>(raw)->set_property_ex(key, Value::number(new_d));
            if (!set_ex_res.is_ok()) {
                const std::string& msg = set_ex_res.error().message();
                NativeErrorType err_type = NativeErrorType::kRangeError;
                if (msg.rfind("TypeError:", 0) == 0) err_type = NativeErrorType::kTypeError;
                frame.pending_throw = make_error_value(err_type, strip_error_prefix(msg));
                continue;
            }
            stack.push_back(Value::number(old_d));
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
            if (obj_val.is_string()) {
                if (name == "length") {
                    stack.push_back(Value::number(static_cast<double>(utf8_cp_len_vm(obj_val.js_string_raw()))));
                } else if (string_prototype_) {
                    stack.push_back(string_prototype_->get_property(name));
                } else {
                    stack.push_back(Value::undefined());
                }
                break;
            }
            if (!obj_val.is_object()) {
                stack.push_back(Value::undefined());
                break;
            }
            // Handle JSFunction specially (e.g., Fn.prototype, Object.keys)
            RcObject* raw_obj = obj_val.as_object_raw();
            if (raw_obj->object_kind() == ObjectKind::kFunction) {
                auto* fn = static_cast<JSFunction*>(raw_obj);
                // P2-B: check own_properties_ first (covers explicitly set "prototype")
                Value own = fn->get_property(name);
                if (!own.is_undefined()) {
                    stack.push_back(std::move(own));
                } else if (name == "prototype") {
                    // Fall back to implicit F.prototype object
                    const auto& proto = fn->prototype_obj();
                    stack.push_back(proto ? Value::object(ObjectPtr(proto)) : Value::undefined());
                } else if (function_prototype_) {
                    stack.push_back(function_prototype_->get_property(name));
                } else {
                    stack.push_back(Value::undefined());
                }
                break;
            }
            if (raw_obj->object_kind() == ObjectKind::kPromise) {
                if (promise_prototype_) {
                    stack.push_back(promise_prototype_->get_property(name));
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
            if (obj_val.is_string()) {
                std::string key = to_string_val(key_val);
                if (key == "length") {
                    stack.push_back(Value::number(static_cast<double>(utf8_cp_len_vm(obj_val.js_string_raw()))));
                } else if (string_prototype_) {
                    stack.push_back(string_prototype_->get_property(key));
                } else {
                    stack.push_back(Value::undefined());
                }
                break;
            }
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
            fn->set_defining_module(frame.current_module);
            auto proto_obj = RcPtr<JSObject>::make();
            proto_obj->set_proto(object_prototype_);
            proto_obj->set_constructor_property(fn.get());
            fn->set_prototype_obj(proto_obj);
            gc_heap_.Register(fn.get());
            gc_heap_.Register(proto_obj.get());
            // Wrap async functions: create a NativeFn that creates outer_promise and runs bytecode
            if (fn_bc->is_async) {
                auto inner_fn = fn;  // the bytecode function
                auto async_wrapper = RcPtr<JSFunction>::make();
                async_wrapper->set_name(fn_bc->name);
                // Store inner_fn in own_properties so GC can trace it
                async_wrapper->set_property("__async_inner__", Value::object(ObjectPtr(inner_fn)));
                async_wrapper->set_native_fn([this, inner_fn](Value this_val,
                        std::vector<Value> call_args, bool) mutable -> EvalResult {
                    // Create outer promise
                    auto outer_promise = RcPtr<JSPromise>::make();
                    gc_heap_.Register(outer_promise.get());
                    Value outer_val = Value::object(ObjectPtr(outer_promise));

                    // Push async call frame and run it
                    auto push_res = push_call_frame(inner_fn, std::move(this_val),
                        std::span<Value>(call_args.data(), call_args.size()));
                    if (!push_res.is_ok()) {
                        outer_promise->Reject(Value::string(push_res.error().message()), job_queue_);
                        return EvalResult::ok(outer_val);
                    }
                    // Run until the async frame returns, throws, or suspends
                    size_t exit_depth = call_stack_.size() - 1;
                    EvalResult body_result = run(exit_depth);

                    vm_handle_async_result(body_result, outer_promise);
                    return EvalResult::ok(outer_val);
                });
                auto async_proto = RcPtr<JSObject>::make();
                async_proto->set_proto(object_prototype_);
                async_proto->set_constructor_property(async_wrapper.get());
                async_wrapper->set_prototype_obj(async_proto);
                gc_heap_.Register(async_wrapper.get());
                gc_heap_.Register(async_proto.get());
                stack.push_back(Value::object(ObjectPtr(async_wrapper)));
            } else {
                stack.push_back(Value::object(ObjectPtr(fn)));
            }
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
                    if (msg == "__qppjs_pending_throw__" && native_pending_throw_.has_value()) {
                        frame.pending_throw = std::move(*native_pending_throw_);
                        native_pending_throw_ = std::nullopt;
                    } else {
                        NativeErrorType err_type = NativeErrorType::kTypeError;
                        if (msg.rfind("ReferenceError:", 0) == 0) err_type = NativeErrorType::kReferenceError;
                        else if (msg.rfind("RangeError:", 0) == 0) err_type = NativeErrorType::kRangeError;
                        frame.pending_throw = make_error_value(err_type, strip_error_prefix(msg));
                    }
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

        case Opcode::kMetaProperty: {
            // import.meta 是词法绑定：直接使用当前帧的 current_module
            // （push_call_frame 已从 JSFunction::defining_module_ 设置）
            if (frame.current_module && frame.current_module->meta_obj) {
                stack.push_back(Value::object(ObjectPtr(frame.current_module->meta_obj)));
            } else {
                stack.push_back(Value::undefined());
            }
            break;
        }

        case Opcode::kImportCall: {
            // Dynamic import(specifier): pop specifier, push Promise.
            // Synchronously loads the module (Load/Link/Evaluate) and returns a fulfilled
            // Promise wrapping the namespace object, or a rejected Promise on error.
            Value spec_val = std::move(stack.back());
            stack.pop_back();
            std::string specifier = to_string_val(spec_val);

            // Resolve base_dir from the nearest module frame in the call stack
            std::string base_dir;
            ModuleRecord* mod_ctx = frame.current_module;
            if (!mod_ctx) {
                for (int i = static_cast<int>(call_stack_.size()) - 1; i >= 0; --i) {
                    if (call_stack_[i].current_module) {
                        mod_ctx = call_stack_[i].current_module;
                        break;
                    }
                }
            }
            if (mod_ctx) {
                base_dir = std::filesystem::path(mod_ctx->specifier).parent_path().string();
            } else {
                base_dir = std::filesystem::current_path().string();
            }

            auto promise = RcPtr<JSPromise>::make();
            gc_heap_.Register(promise.get());

            auto load_result = module_loader_.Load(specifier, base_dir);
            if (!load_result.ok()) {
                Value err_val = make_error_value(NativeErrorType::kError,
                    "Cannot load module '" + specifier + "': " + load_result.error().message());
                promise->Reject(err_val, job_queue_);
                stack.push_back(Value::object(ObjectPtr(promise)));
                break;
            }
            auto mod = load_result.value();

            auto link_result = link_module(*mod);
            if (!link_result.is_ok()) {
                Value err_val = make_error_value(NativeErrorType::kError,
                    link_result.error().message());
                promise->Reject(err_val, job_queue_);
                stack.push_back(Value::object(ObjectPtr(promise)));
                break;
            }

            auto eval_result_mod = evaluate_module(*mod);
            if (!eval_result_mod.is_ok()) {
                // evaluate_module may set frame.pending_throw for cached errors
                Value err_val;
                if (frame.pending_throw.has_value()) {
                    err_val = std::move(*frame.pending_throw);
                    frame.pending_throw = std::nullopt;
                } else {
                    err_val = make_error_value(NativeErrorType::kError,
                        eval_result_mod.error().message());
                }
                promise->Reject(err_val, job_queue_);
                stack.push_back(Value::object(ObjectPtr(promise)));
                break;
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
            stack.push_back(Value::object(ObjectPtr(promise)));
            break;
        }

        case Opcode::kAwait: {
            // Suspend the async function: move the current frame out, store inner_promise,
            // and signal suspension to the async wrapper via vm_async_suspended_.
            Value arg_val = std::move(stack.back());
            stack.pop_back();
            auto inner_promise = vm_promise_resolve(std::move(arg_val));

            // Move current frame out of call_stack_
            vm_suspended_frame_ = std::move(call_stack_.back());
            call_stack_.pop_back();
            call_depth_--;

            // Store inner_promise for the async wrapper to pick up
            vm_pending_inner_promise_ = inner_promise;
            vm_async_suspended_ = true;

            // Signal suspension — run() will detect this and return kAsyncSuspendSentinel
            goto suspend_exit;
        }

        default:
            return EvalResult::err(Error(ErrorKind::Runtime, "Internal: unknown opcode"));
        }
    }

    return EvalResult::ok(Value::undefined());

suspend_exit:
    return EvalResult::err(Error(ErrorKind::Runtime, kAsyncSuspendSentinel));
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

    // 执行剩余微任务（async function 调用可能产生 pending microtasks）
    vm_drain_job_queue();

    // 微任务执行后刷新最后一条简单标识符表达式的值（与 exec() 的 drain 后刷新逻辑对称）
    if (eval_result.is_ok() && entry_mod->module_env && !entry_mod->ast.body.empty()) {
        const auto& last_stmt = entry_mod->ast.body.back();
        if (const auto* es = std::get_if<ExpressionStatement>(&last_stmt.v)) {
            if (const auto* id = std::get_if<Identifier>(&es->expr.v)) {
                if (id->name != "undefined") {
                    auto reeval = entry_mod->module_env->get(id->name);
                    if (reeval.is_ok()) {
                        eval_result = EvalResult::ok(reeval.value());
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
        add_obj(object_prototype_.get());
        add_obj(array_prototype_.get());
        add_obj(function_prototype_.get());
        add_obj(promise_prototype_.get());
        add_obj(string_prototype_.get());
        add_obj(math_obj_.get());
        add_obj(number_prototype_.get());
        add_obj(object_constructor_.get());
        add_obj(number_constructor_.get());
        add_obj(boolean_constructor_.get());
        add_obj(string_constructor_.get());
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
    if (boolean_constructor_) boolean_constructor_->clear_own_properties();
    if (string_constructor_) string_constructor_->clear_own_properties();
    // 清理所有模块环境中的函数引用（打破 module_env ↔ JSFunction 循环引用）
    module_loader_.ClearModuleEnvs();
    module_loader_.Clear();

    // 将 eval_result 中的对象从 GcHeap 摘除，避免 VM 析构后 gc_heap_ 失效
    // 导致调用者持有的 EvalResult 析构时触发 Unregister 崩溃。
    if (eval_result.is_ok() && eval_result.value().is_object()) {
        RcObject* raw = eval_result.value().as_object_raw();
        if (raw && raw->gc_heap_) {
            gc_heap_.Unregister(raw);
            raw->gc_heap_ = nullptr;  // 防止析构时再次调用 Unregister
        }
    }

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

    // 创建 import.meta 对象（[[Prototype]] = null）
    auto meta = RcPtr<JSObject>::make();
    gc_heap_.Register(meta.get());
    meta->set_property("url", Value::string(mod.specifier));
    mod.meta_obj = std::move(meta);

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

    // TLA: 顶层 await 挂起，通过 vm_handle_async_result 异步执行剩余字节码
    if (!result.is_ok() && result.error().message() == kAsyncSuspendSentinel) {
        auto outer_promise = RcPtr<JSPromise>::make();
        gc_heap_.Register(outer_promise.get());

        vm_handle_async_result(result, outer_promise);

        // 等待所有微任务完成
        vm_drain_job_queue();

        // 从 outer_promise 读取最终结果
        if (outer_promise->state() == PromiseState::kFulfilled) {
            return EvalResult::ok(outer_promise->result());
        } else if (outer_promise->state() == PromiseState::kRejected) {
            Value reason = outer_promise->result();
            if (!call_stack_.empty()) {
                call_stack_.back().pending_throw = reason;
            } else {
                native_pending_throw_ = reason;
            }
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
            return EvalResult::err(Error(ErrorKind::Runtime,
                "Error: top-level await did not settle"));
        }
    }

    return result;
}

}  // namespace qppjs
