#include "qppjs/runtime/interpreter.h"

#include "qppjs/base/error.h"
#include "qppjs/frontend/ast.h"
#include "qppjs/frontend/parser.h"
#include "qppjs/runtime/completion.h"
#include "qppjs/runtime/environment.h"
#include "qppjs/runtime/for_of_iterator.h"
#include "qppjs/runtime/js_function.h"
#include "qppjs/runtime/js_generator.h"
#include "qppjs/runtime/js_map.h"
#include "qppjs/runtime/js_object.h"
#include "qppjs/runtime/js_regexp.h"
#include "qppjs/runtime/module_loader.h"
#include "qppjs/runtime/module_record.h"
#include "qppjs/runtime/native_errors.h"
#include "qppjs/runtime/number_utils.h"
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
#include <regex>
#include <cstdio>
#include <set>
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
    case ValueKind::Object: {
        // ToPrimitive: kStringObject/kBooleanObject 快路径
        RcObject* raw = v.as_object_raw();
        if (raw != nullptr) {
            if (raw->object_kind() == ObjectKind::kStringObject) {
                auto* obj = static_cast<JSObject*>(raw);
                Value wrapped = obj->wrapped_value();
                if (wrapped.is_string()) return to_number_double(wrapped);
            }
            if (raw->object_kind() == ObjectKind::kBooleanObject) {
                auto* obj = static_cast<JSObject*>(raw);
                Value wrapped = obj->wrapped_value();
                if (wrapped.is_bool()) return wrapped.as_bool() ? 1.0 : 0.0;
            }
        }
        return std::numeric_limits<double>::quiet_NaN();
    }
    case ValueKind::Symbol:  return std::numeric_limits<double>::quiet_NaN();
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
    case ValueKind::Symbol:  return a.as_symbol_id() == b.as_symbol_id();
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

// Coerce a primitive value to string for String.prototype method fallback.
// Does not call Interpreter::to_string_val (private); only handles non-object primitives.
static std::string coerce_primitive_to_str(const Value& v) {
    if (v.is_undefined()) return "undefined";
    if (v.is_null()) return "null";
    if (v.is_bool()) return v.as_bool() ? "true" : "false";
    if (v.is_number()) {
        double n = v.as_number();
        if (std::isnan(n)) return "NaN";
        if (std::isinf(n)) return n > 0 ? "Infinity" : "-Infinity";
        if (n == static_cast<double>(static_cast<long long>(n)) && std::abs(n) < 1e15) {
            std::ostringstream oss;
            oss << static_cast<long long>(n);
            return oss.str();
        }
        // 大整数 < 10^21: 用 fixed 格式避免科学计数法
        if (n == std::floor(n) && std::abs(n) < 1e21) {
            char buf[64];
            int len = std::snprintf(buf, sizeof(buf), "%.0f", n);
            if (len > 0 && len < static_cast<int>(sizeof(buf))) {
                return std::string(buf, len);
            }
        }
        std::ostringstream oss;
        oss << n;
        return oss.str();
    }
    return "[object Object]";
}

// Extract the effective string Value from a String.prototype method's this.
// Handles string primitive, kStringObject wrapper, or falls back to coerce.
static Value string_this_value(const Value& this_val) {
    if (this_val.is_string()) return this_val;
    if (this_val.is_object()) {
        RcObject* raw = this_val.as_object_raw();
        if (raw->object_kind() == ObjectKind::kStringObject) {
            return static_cast<JSObject*>(raw)->wrapped_value();
        }
        return Value::string("[object Object]");
    }
    return Value::string(coerce_primitive_to_str(this_val));
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

    // Object.prototype built-in methods (non-enumerable via define_builtin_property)
    {
        auto fn = RcPtr<JSFunction>::make();
        fn->set_name(std::string("valueOf"));
        fn->set_native_fn([](Value this_val, std::vector<Value> /*args*/, bool) -> EvalResult {
            return EvalResult::ok(this_val);
        });
        gc_heap_.Register(fn.get());
        object_prototype_->define_builtin_property("valueOf", Value::object(ObjectPtr(fn)));
    }
    {
        auto fn = RcPtr<JSFunction>::make();
        fn->set_name(std::string("toString"));
        fn->set_native_fn([this](Value this_val, std::vector<Value> /*args*/, bool) -> EvalResult {
            if (this_val.is_null()) return EvalResult::ok(Value::string("[object Null]"));
            if (this_val.is_undefined()) return EvalResult::ok(Value::string("[object Undefined]"));
            std::string tag;
            if (this_val.is_object()) {
                RcObject* raw = this_val.as_object_raw();
                if (raw) {
                    ObjectKind kind = raw->object_kind();
                    if (kind == ObjectKind::kFunction) {
                        tag = "Function";
                    } else {
                        auto* obj = static_cast<JSObject*>(raw);
                        // Check [Symbol.toStringTag] first
                        const JSObject::SymbolPropertyEntry* tag_entry =
                            obj->find_symbol_entry(symbol_table_.well_known_to_string_tag);
                        if (tag_entry && tag_entry->value.is_string()) {
                            return EvalResult::ok(Value::string(
                                "[object " + tag_entry->value.as_string() + "]"));
                        }
                        if (kind == ObjectKind::kArray) tag = "Array";
                        else if (kind == ObjectKind::kRegExp) tag = "RegExp";
                        else if (kind == ObjectKind::kPromise) tag = "Promise";
                        else if (kind == ObjectKind::kMap) tag = "Map";
                        else if (kind == ObjectKind::kSet) tag = "Set";
                        else if (kind == ObjectKind::kWeakMap) tag = "WeakMap";
                        else if (kind == ObjectKind::kWeakSet) tag = "WeakSet";
                        else if (kind == ObjectKind::kStringObject) tag = "String";
                        else if (kind == ObjectKind::kBooleanObject) tag = "Boolean";
                        else tag = "Object";
                    }
                } else {
                    tag = "Object";
                }
            } else if (this_val.is_number()) {
                tag = "Number";
            } else if (this_val.is_string()) {
                tag = "String";
            } else if (this_val.is_bool()) {
                tag = "Boolean";
            } else {
                tag = "Object";
            }
            return EvalResult::ok(Value::string("[object " + tag + "]"));
        });
        gc_heap_.Register(fn.get());
        object_prototype_->define_builtin_property("toString", Value::object(ObjectPtr(fn)));
    }
    {
        auto fn = RcPtr<JSFunction>::make();
        fn->set_name(std::string("hasOwnProperty"));
        fn->set_native_fn([](Value this_val, std::vector<Value> args, bool) -> EvalResult {
            if (!this_val.is_object()) return EvalResult::ok(Value::boolean(false));
            auto* raw = this_val.as_object_raw();
            if (!raw) return EvalResult::ok(Value::boolean(false));
            std::string key = args.empty() ? "undefined" : to_string_val(args[0]);
            if (raw->object_kind() == ObjectKind::kFunction) {
                return EvalResult::ok(Value::boolean(static_cast<JSFunction*>(raw)->has_property(key)));
            }
            return EvalResult::ok(Value::boolean(static_cast<JSObject*>(raw)->has_own_property(key)));
        });
        gc_heap_.Register(fn.get());
        object_prototype_->define_builtin_property("hasOwnProperty", Value::object(ObjectPtr(fn)));
    }
    {
        auto fn = RcPtr<JSFunction>::make();
        fn->set_name(std::string("isPrototypeOf"));
        fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
            if (!this_val.is_object() || args.empty() || !args[0].is_object())
                return EvalResult::ok(Value::boolean(false));
            auto* needle = this_val.as_object_raw();
            auto* raw = args[0].as_object_raw();
            if (!raw) return EvalResult::ok(Value::boolean(false));
            if (raw->object_kind() == ObjectKind::kFunction) {
                // JSFunction prototype chain: function_prototype_ → object_prototype_ → null
                if (function_prototype_ && needle == function_prototype_.get())
                    return EvalResult::ok(Value::boolean(true));
                if (object_prototype_ && needle == object_prototype_.get())
                    return EvalResult::ok(Value::boolean(true));
                return EvalResult::ok(Value::boolean(false));
            }
            auto* cur = static_cast<JSObject*>(raw)->proto().get();
            while (cur) {
                if (cur == needle) return EvalResult::ok(Value::boolean(true));
                cur = cur->proto().get();
            }
            return EvalResult::ok(Value::boolean(false));
        });
        gc_heap_.Register(fn.get());
        object_prototype_->define_builtin_property("isPrototypeOf", Value::object(ObjectPtr(fn)));
    }
    {
        auto fn = RcPtr<JSFunction>::make();
        fn->set_name(std::string("propertyIsEnumerable"));
        fn->set_native_fn([](Value this_val, std::vector<Value> args, bool) -> EvalResult {
            if (!this_val.is_object()) return EvalResult::ok(Value::boolean(false));
            auto* raw = this_val.as_object_raw();
            if (!raw || raw->object_kind() == ObjectKind::kFunction)
                return EvalResult::ok(Value::boolean(false));
            std::string key = args.empty() ? "undefined" : to_string_val(args[0]);
            auto* obj = static_cast<JSObject*>(raw);
            const JSObject::PropertyEntry* entry = obj->get_own_entry(key);
            if (!entry) return EvalResult::ok(Value::boolean(false));
            return EvalResult::ok(Value::boolean((entry->flags & kPropEnumerable) != 0));
        });
        gc_heap_.Register(fn.get());
        object_prototype_->define_builtin_property("propertyIsEnumerable", Value::object(ObjectPtr(fn)));
    }

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
        {NativeErrorType::kSyntaxError,    "SyntaxError"},
        {NativeErrorType::kEvalError,      "EvalError"},
        {NativeErrorType::kURIError,       "URIError"},
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
    array_prototype_->define_builtin_property("push", Value::object(ObjectPtr(push_fn)));

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
    array_prototype_->define_builtin_property("pop", Value::object(ObjectPtr(pop_fn)));

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
            // Skip holes (sparse array semantics)
            if (elem_it == arr->elements_.end()) continue;
            Value elem = elem_it->second;
            Value call_args[3] = {elem, Value::number(static_cast<double>(i)), this_val};
            auto res = call_function_val(callback, Value::undefined(), {call_args, 3});
            if (!res.is_ok()) return res;
        }
        return EvalResult::ok(Value::undefined());
    });
    array_prototype_->define_builtin_property("forEach", Value::object(ObjectPtr(foreach_fn)));

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
    array_prototype_->define_builtin_property("map", Value::object(ObjectPtr(map_fn)));

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
    array_prototype_->define_builtin_property("filter", Value::object(ObjectPtr(filter_fn)));

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
    array_prototype_->define_builtin_property("reduce", Value::object(ObjectPtr(reduce_fn)));

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
    array_prototype_->define_builtin_property("reduceRight", Value::object(ObjectPtr(reduce_right_fn)));

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
    array_prototype_->define_builtin_property("find", Value::object(ObjectPtr(find_fn)));

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
    array_prototype_->define_builtin_property("findIndex", Value::object(ObjectPtr(find_index_fn)));

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
    array_prototype_->define_builtin_property("some", Value::object(ObjectPtr(some_fn)));

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
    array_prototype_->define_builtin_property("every", Value::object(ObjectPtr(every_fn)));

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
    array_prototype_->define_builtin_property("indexOf", Value::object(ObjectPtr(index_of_fn)));

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
    array_prototype_->define_builtin_property("includes", Value::object(ObjectPtr(includes_fn)));

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
    array_prototype_->define_builtin_property("slice", Value::object(ObjectPtr(slice_fn)));

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
        if (new_len > 9007199254740991LL) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                              "splice: new length exceeds 2^53-1");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
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
    array_prototype_->define_builtin_property("splice", Value::object(ObjectPtr(splice_fn)));

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
                s.str_cache = Interpreter::to_string_val(s.val);
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
    array_prototype_->define_builtin_property("sort", Value::object(ObjectPtr(sort_fn)));

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
    array_prototype_->define_builtin_property("join", Value::object(ObjectPtr(join_fn)));

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
    array_prototype_->define_builtin_property("reverse", Value::object(ObjectPtr(reverse_fn)));

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
    array_prototype_->define_builtin_property("flat", Value::object(ObjectPtr(flat_fn)));

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
    array_prototype_->define_builtin_property("flatMap", Value::object(ObjectPtr(flat_map_fn)));

    // Array.prototype.findLast
    auto find_last_fn = RcPtr<JSFunction>::make();
    find_last_fn->set_name(std::string("findLast"));
    find_last_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        RcObject* raw = this_val.as_object_raw();
        if (!raw || raw->object_kind() != ObjectKind::kArray) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                              "findLast called on non-array");
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
        for (int64_t i = static_cast<int64_t>(len) - 1; i >= 0; i--) {
            auto it = arr->elements_.find(static_cast<uint32_t>(i));
            Value elem = (it != arr->elements_.end()) ? it->second : Value::undefined();
            Value call_args[3] = {elem, Value::number(static_cast<double>(i)), this_val};
            auto res = call_function_val(callback, this_arg, {call_args, 3});
            if (!res.is_ok()) return res;
            if (to_boolean(res.value())) return EvalResult::ok(elem);
        }
        return EvalResult::ok(Value::undefined());
    });
    gc_heap_.Register(find_last_fn.get());
    array_prototype_->define_builtin_property("findLast", Value::object(ObjectPtr(find_last_fn)));

    // Array.prototype.findLastIndex
    auto find_last_index_fn = RcPtr<JSFunction>::make();
    find_last_index_fn->set_name(std::string("findLastIndex"));
    find_last_index_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        RcObject* raw = this_val.as_object_raw();
        if (!raw || raw->object_kind() != ObjectKind::kArray) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                              "findLastIndex called on non-array");
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
        for (int64_t i = static_cast<int64_t>(len) - 1; i >= 0; i--) {
            auto it = arr->elements_.find(static_cast<uint32_t>(i));
            Value elem = (it != arr->elements_.end()) ? it->second : Value::undefined();
            Value call_args[3] = {elem, Value::number(static_cast<double>(i)), this_val};
            auto res = call_function_val(callback, this_arg, {call_args, 3});
            if (!res.is_ok()) return res;
            if (to_boolean(res.value())) return EvalResult::ok(Value::number(static_cast<double>(i)));
        }
        return EvalResult::ok(Value::number(-1.0));
    });
    gc_heap_.Register(find_last_index_fn.get());
    array_prototype_->define_builtin_property("findLastIndex", Value::object(ObjectPtr(find_last_index_fn)));

    // Array.prototype.toSorted
    auto to_sorted_fn = RcPtr<JSFunction>::make();
    to_sorted_fn->set_name(std::string("toSorted"));
    to_sorted_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        RcObject* raw = this_val.as_object_raw();
        if (!raw || raw->object_kind() != ObjectKind::kArray) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                              "toSorted called on non-array");
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
        auto* src = static_cast<JSObject*>(raw);
        uint32_t len = src->array_length_;
        auto result = RcPtr<JSObject>::make(ObjectKind::kArray);
        gc_heap_.Register(result.get());
        result->set_proto(array_prototype_);
        struct Slot {
            Value val;
            uint32_t pos;
            std::string str_cache;
        };
        std::vector<Slot> slots;
        slots.reserve(len);
        uint32_t undef_count = 0;
        for (uint32_t i = 0; i < len; i++) {
            auto it = src->elements_.find(i);
            if (it == src->elements_.end()) {
            } else if (it->second.is_undefined()) {
                undef_count++;
            } else {
                slots.push_back({it->second, i, {}});
            }
        }
        if (!has_cmp) {
            for (auto& s : slots) s.str_cache = Interpreter::to_string_val(s.val);
        }
        Value cmp_fn = has_cmp ? args[0] : Value::undefined();
        EvalResult sort_err = EvalResult::ok(Value::undefined());
        bool had_error = false;
        std::stable_sort(slots.begin(), slots.end(), [&](const Slot& a, const Slot& b) -> bool {
            if (had_error) return false;
            if (has_cmp) {
                Value call_args[2] = {a.val, b.val};
                auto res = call_function_val(cmp_fn, Value::undefined(), {call_args, 2});
                if (!res.is_ok()) { sort_err = res; had_error = true; return false; }
                double cmp = to_number_double(res.value());
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
        uint32_t idx = 0;
        for (auto& s : slots) result->elements_[idx++] = std::move(s.val);
        for (uint32_t i = 0; i < undef_count; i++) result->elements_[idx++] = Value::undefined();
        result->array_length_ = idx + (len - slots.size() - undef_count);
        return EvalResult::ok(Value::object(ObjectPtr(result)));
    });
    gc_heap_.Register(to_sorted_fn.get());
    array_prototype_->define_builtin_property("toSorted", Value::object(ObjectPtr(to_sorted_fn)));

    // Array.prototype.toReversed
    auto to_reversed_fn = RcPtr<JSFunction>::make();
    to_reversed_fn->set_name(std::string("toReversed"));
    to_reversed_fn->set_native_fn([this](Value this_val, std::vector<Value>, bool) -> EvalResult {
        RcObject* raw = this_val.as_object_raw();
        if (!raw || raw->object_kind() != ObjectKind::kArray) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                              "toReversed called on non-array");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        auto* src = static_cast<JSObject*>(raw);
        uint32_t len = src->array_length_;
        auto result = RcPtr<JSObject>::make(ObjectKind::kArray);
        gc_heap_.Register(result.get());
        result->set_proto(array_prototype_);
        for (uint32_t i = 0; i < len; i++) {
            uint32_t from = len - 1 - i;
            auto it = src->elements_.find(from);
            if (it != src->elements_.end()) {
                result->elements_[i] = it->second;
            }
        }
        result->array_length_ = len;
        return EvalResult::ok(Value::object(ObjectPtr(result)));
    });
    gc_heap_.Register(to_reversed_fn.get());
    array_prototype_->define_builtin_property("toReversed", Value::object(ObjectPtr(to_reversed_fn)));

    // Array.prototype.toSpliced(start, deleteCount, ...items)
    auto to_spliced_fn = RcPtr<JSFunction>::make();
    to_spliced_fn->set_name(std::string("toSpliced"));
    to_spliced_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        RcObject* raw = this_val.as_object_raw();
        if (!raw || raw->object_kind() != ObjectKind::kArray) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                              "toSpliced called on non-array");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        auto* src = static_cast<JSObject*>(raw);
        int64_t len = static_cast<int64_t>(src->array_length_);
        int64_t start = 0;
        if (!args.empty() && !args[0].is_undefined()) {
            double s = to_number_double(args[0]);
            start = std::isnan(s) ? 0 : static_cast<int64_t>(std::trunc(s));
        }
        if (start < 0) start = std::max(int64_t(0), len + start);
        else start = std::min(start, len);
        int64_t del_count = len - start;
        if (args.size() >= 2 && !args[1].is_undefined()) {
            double d = to_number_double(args[1]);
            del_count = std::isnan(d) ? 0 : static_cast<int64_t>(std::trunc(d));
            del_count = std::max(int64_t(0), std::min(del_count, len - start));
        }
        uint32_t item_count = args.size() > 2 ? static_cast<uint32_t>(args.size() - 2) : 0;
        auto result = RcPtr<JSObject>::make(ObjectKind::kArray);
        gc_heap_.Register(result.get());
        result->set_proto(array_prototype_);
        uint32_t n = 0;
        for (int64_t i = 0; i < start; i++) {
            auto it = src->elements_.find(static_cast<uint32_t>(i));
            if (it != src->elements_.end()) result->elements_[n] = it->second;
            n++;
        }
        for (uint32_t k = 0; k < item_count; k++) result->elements_[n++] = args[2 + k];
        for (int64_t i = start + del_count; i < len; i++) {
            auto it = src->elements_.find(static_cast<uint32_t>(i));
            if (it != src->elements_.end()) result->elements_[n] = it->second;
            n++;
        }
        result->array_length_ = n;
        return EvalResult::ok(Value::object(ObjectPtr(result)));
    });
    gc_heap_.Register(to_spliced_fn.get());
    array_prototype_->define_builtin_property("toSpliced", Value::object(ObjectPtr(to_spliced_fn)));

    // Array.prototype.with(index, value)
    auto with_fn = RcPtr<JSFunction>::make();
    with_fn->set_name(std::string("with"));
    with_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        RcObject* raw = this_val.as_object_raw();
        if (!raw || raw->object_kind() != ObjectKind::kArray) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                              "with called on non-array");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        auto* src = static_cast<JSObject*>(raw);
        uint32_t len = src->array_length_;
        if (args.size() < 2) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                              "Array.prototype.with requires 2 arguments");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        double idx_d = to_number_double(args[0]);
        int64_t idx = std::isnan(idx_d) ? 0 : static_cast<int64_t>(std::trunc(idx_d));
        if (idx < 0) idx = static_cast<int64_t>(len) + idx;
        if (idx < 0 || idx >= static_cast<int64_t>(len)) {
            pending_throw_ = make_error_value(NativeErrorType::kRangeError,
                                              "Invalid index");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        auto result = RcPtr<JSObject>::make(ObjectKind::kArray);
        gc_heap_.Register(result.get());
        result->set_proto(array_prototype_);
        for (uint32_t i = 0; i < len; i++) {
            if (static_cast<int64_t>(i) == idx) {
                result->elements_[i] = args[1];
            } else {
                auto it = src->elements_.find(i);
                if (it != src->elements_.end()) result->elements_[i] = it->second;
            }
        }
        result->array_length_ = len;
        return EvalResult::ok(Value::object(ObjectPtr(result)));
    });
    gc_heap_.Register(with_fn.get());
    array_prototype_->define_builtin_property("with", Value::object(ObjectPtr(with_fn)));

    // Array.prototype.keys
    auto keys_iter_fn = RcPtr<JSFunction>::make();
    keys_iter_fn->set_name(std::string("keys"));
    keys_iter_fn->set_native_fn([this](Value this_val, std::vector<Value>, bool) -> EvalResult {
        auto iter_obj = RcPtr<JSObject>::make();
        gc_heap_.Register(iter_obj.get());
        iter_obj->set_property("__arr__", this_val);
        iter_obj->set_property("__idx__", Value::number(0.0));
        iter_obj->set_property("__mode__", Value::string("keys"));
        auto next_fn = RcPtr<JSFunction>::make();
        next_fn->set_name(std::string("next"));
        next_fn->set_native_fn([this](Value iter_this, std::vector<Value>, bool) -> EvalResult {
            auto make_done = [&]() -> EvalResult {
                auto r = RcPtr<JSObject>::make();
                gc_heap_.Register(r.get());
                r->set_property("value", Value::undefined());
                r->set_property("done", Value::boolean(true));
                return EvalResult::ok(Value::object(ObjectPtr(r)));
            };
            if (!iter_this.is_object()) return make_done();
            auto* iter = static_cast<JSObject*>(iter_this.as_object_raw());
            Value arr_val = iter->get_property("__arr__");
            Value idx_val = iter->get_property("__idx__");
            uint32_t idx = idx_val.is_number() ? static_cast<uint32_t>(idx_val.as_number()) : 0u;
            if (!arr_val.is_object() || arr_val.as_object_raw()->object_kind() != ObjectKind::kArray)
                return make_done();
            auto* arr = static_cast<JSObject*>(arr_val.as_object_raw());
            if (idx >= arr->array_length_) return make_done();
            iter->set_property("__idx__", Value::number(static_cast<double>(idx + 1)));
            auto r = RcPtr<JSObject>::make();
            gc_heap_.Register(r.get());
            r->set_property("value", Value::number(static_cast<double>(idx)));
            r->set_property("done", Value::boolean(false));
            return EvalResult::ok(Value::object(ObjectPtr(r)));
        });
        gc_heap_.Register(next_fn.get());
        iter_obj->set_property("next", Value::object(ObjectPtr(next_fn)));
        auto self_iter_fn = RcPtr<JSFunction>::make();
        self_iter_fn->set_native_fn([](Value v, std::vector<Value>, bool) -> EvalResult {
            return EvalResult::ok(v);
        });
        gc_heap_.Register(self_iter_fn.get());
        iter_obj->set_property_by_symbol(symbol_table_.well_known_iterator,
                                          Value::object(ObjectPtr(self_iter_fn)));
        return EvalResult::ok(Value::object(ObjectPtr(iter_obj)));
    });
    gc_heap_.Register(keys_iter_fn.get());
    array_prototype_->define_builtin_property("keys", Value::object(ObjectPtr(keys_iter_fn)));

    // Array.prototype.values
    auto values_iter_fn = RcPtr<JSFunction>::make();
    values_iter_fn->set_name(std::string("values"));
    values_iter_fn->set_native_fn([this](Value this_val, std::vector<Value>, bool) -> EvalResult {
        auto iter_obj = RcPtr<JSObject>::make();
        gc_heap_.Register(iter_obj.get());
        iter_obj->set_property("__arr__", this_val);
        iter_obj->set_property("__idx__", Value::number(0.0));
        auto next_fn = RcPtr<JSFunction>::make();
        next_fn->set_name(std::string("next"));
        next_fn->set_native_fn([this](Value iter_this, std::vector<Value>, bool) -> EvalResult {
            auto make_done = [&]() -> EvalResult {
                auto r = RcPtr<JSObject>::make();
                gc_heap_.Register(r.get());
                r->set_property("value", Value::undefined());
                r->set_property("done", Value::boolean(true));
                return EvalResult::ok(Value::object(ObjectPtr(r)));
            };
            if (!iter_this.is_object()) return make_done();
            auto* iter = static_cast<JSObject*>(iter_this.as_object_raw());
            Value arr_val = iter->get_property("__arr__");
            Value idx_val = iter->get_property("__idx__");
            uint32_t idx = idx_val.is_number() ? static_cast<uint32_t>(idx_val.as_number()) : 0u;
            if (!arr_val.is_object() || arr_val.as_object_raw()->object_kind() != ObjectKind::kArray)
                return make_done();
            auto* arr = static_cast<JSObject*>(arr_val.as_object_raw());
            if (idx >= arr->array_length_) return make_done();
            auto elem_it = arr->elements_.find(idx);
            Value value = (elem_it != arr->elements_.end()) ? elem_it->second : Value::undefined();
            iter->set_property("__idx__", Value::number(static_cast<double>(idx + 1)));
            auto r = RcPtr<JSObject>::make();
            gc_heap_.Register(r.get());
            r->set_property("value", std::move(value));
            r->set_property("done", Value::boolean(false));
            return EvalResult::ok(Value::object(ObjectPtr(r)));
        });
        gc_heap_.Register(next_fn.get());
        iter_obj->set_property("next", Value::object(ObjectPtr(next_fn)));
        auto self_iter_fn = RcPtr<JSFunction>::make();
        self_iter_fn->set_native_fn([](Value v, std::vector<Value>, bool) -> EvalResult {
            return EvalResult::ok(v);
        });
        gc_heap_.Register(self_iter_fn.get());
        iter_obj->set_property_by_symbol(symbol_table_.well_known_iterator,
                                          Value::object(ObjectPtr(self_iter_fn)));
        return EvalResult::ok(Value::object(ObjectPtr(iter_obj)));
    });
    gc_heap_.Register(values_iter_fn.get());
    array_prototype_->define_builtin_property("values", Value::object(ObjectPtr(values_iter_fn)));

    // Array.prototype.entries
    auto entries_iter_fn = RcPtr<JSFunction>::make();
    entries_iter_fn->set_name(std::string("entries"));
    entries_iter_fn->set_native_fn([this](Value this_val, std::vector<Value>, bool) -> EvalResult {
        auto iter_obj = RcPtr<JSObject>::make();
        gc_heap_.Register(iter_obj.get());
        iter_obj->set_property("__arr__", this_val);
        iter_obj->set_property("__idx__", Value::number(0.0));
        auto next_fn = RcPtr<JSFunction>::make();
        next_fn->set_name(std::string("next"));
        next_fn->set_native_fn([this](Value iter_this, std::vector<Value>, bool) -> EvalResult {
            auto make_done = [&]() -> EvalResult {
                auto r = RcPtr<JSObject>::make();
                gc_heap_.Register(r.get());
                r->set_property("value", Value::undefined());
                r->set_property("done", Value::boolean(true));
                return EvalResult::ok(Value::object(ObjectPtr(r)));
            };
            if (!iter_this.is_object()) return make_done();
            auto* iter = static_cast<JSObject*>(iter_this.as_object_raw());
            Value arr_val = iter->get_property("__arr__");
            Value idx_val = iter->get_property("__idx__");
            uint32_t idx = idx_val.is_number() ? static_cast<uint32_t>(idx_val.as_number()) : 0u;
            if (!arr_val.is_object() || arr_val.as_object_raw()->object_kind() != ObjectKind::kArray)
                return make_done();
            auto* arr = static_cast<JSObject*>(arr_val.as_object_raw());
            if (idx >= arr->array_length_) return make_done();
            auto elem_it = arr->elements_.find(idx);
            Value elem = (elem_it != arr->elements_.end()) ? elem_it->second : Value::undefined();
            iter->set_property("__idx__", Value::number(static_cast<double>(idx + 1)));
            // Build [idx, elem] pair array
            auto pair = RcPtr<JSObject>::make(ObjectKind::kArray);
            gc_heap_.Register(pair.get());
            pair->set_proto(array_prototype_);
            pair->elements_[0] = Value::number(static_cast<double>(idx));
            pair->elements_[1] = std::move(elem);
            pair->array_length_ = 2;
            auto r = RcPtr<JSObject>::make();
            gc_heap_.Register(r.get());
            r->set_property("value", Value::object(ObjectPtr(pair)));
            r->set_property("done", Value::boolean(false));
            return EvalResult::ok(Value::object(ObjectPtr(r)));
        });
        gc_heap_.Register(next_fn.get());
        iter_obj->set_property("next", Value::object(ObjectPtr(next_fn)));
        auto self_iter_fn = RcPtr<JSFunction>::make();
        self_iter_fn->set_native_fn([](Value v, std::vector<Value>, bool) -> EvalResult {
            return EvalResult::ok(v);
        });
        gc_heap_.Register(self_iter_fn.get());
        iter_obj->set_property_by_symbol(symbol_table_.well_known_iterator,
                                          Value::object(ObjectPtr(self_iter_fn)));
        return EvalResult::ok(Value::object(ObjectPtr(iter_obj)));
    });
    gc_heap_.Register(entries_iter_fn.get());
    array_prototype_->define_builtin_property("entries", Value::object(ObjectPtr(entries_iter_fn)));

    // Array.prototype.concat
    auto concat_fn = RcPtr<JSFunction>::make();
    concat_fn->set_name(std::string("concat"));
    concat_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        auto* this_raw = this_val.as_object_raw();
        if (!this_raw) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                              "Array.prototype.concat called on null or undefined");
            return EvalResult::err(Error{ErrorKind::Runtime, kPendingThrowSentinel});
        }
        auto result = RcPtr<JSObject>::make(ObjectKind::kArray);
        gc_heap_.Register(result.get());
        result->set_proto(array_prototype_);
        uint32_t n = 0;
        // Spread this array
        if (this_raw->object_kind() == ObjectKind::kArray) {
            auto* arr = static_cast<JSObject*>(this_raw);
            for (uint32_t i = 0; i < arr->array_length_; ++i) {
                auto it = arr->elements_.find(i);
                if (it != arr->elements_.end()) {
                    result->elements_[n] = it->second;
                }
                n++;
            }
        } else {
            result->elements_[n++] = this_val;
        }
        // Append args
        for (auto& arg : args) {
            if (arg.is_object() && arg.as_object_raw() &&
                arg.as_object_raw()->object_kind() == ObjectKind::kArray) {
                auto* arr = static_cast<JSObject*>(arg.as_object_raw());
                for (uint32_t i = 0; i < arr->array_length_; ++i) {
                    auto it = arr->elements_.find(i);
                    if (it != arr->elements_.end()) {
                        result->elements_[n] = it->second;
                    }
                    n++;
                }
            } else {
                result->elements_[n++] = arg;
            }
        }
        result->array_length_ = n;
        return EvalResult::ok(Value::object(ObjectPtr(result)));
    });
    array_prototype_->define_builtin_property("concat", Value::object(ObjectPtr(concat_fn)));

    // Array.prototype.fill
    auto fill_fn = RcPtr<JSFunction>::make();
    fill_fn->set_name(std::string("fill"));
    fill_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        auto* this_raw = this_val.as_object_raw();
        if (!this_raw) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                              "Array.prototype.fill called on null or undefined");
            return EvalResult::err(Error{ErrorKind::Runtime, kPendingThrowSentinel});
        }
        double len_d = 0;
        if (this_raw->object_kind() == ObjectKind::kArray) {
            len_d = static_cast<double>(static_cast<JSObject*>(this_raw)->array_length_);
        }
        auto len = static_cast<int64_t>(len_d);
        auto rel_start = args.size() > 1 ? to_number_double(args[1]) : 0;
        auto k = rel_start < 0 ? std::max(len + static_cast<int64_t>(rel_start), INT64_C(0))
                               : std::min(static_cast<int64_t>(rel_start), len);
        auto rel_end = args.size() > 2 ? to_number_double(args[2]) : len_d;
        auto final_end = rel_end < 0 ? std::max(len + static_cast<int64_t>(rel_end), INT64_C(0))
                                     : std::min(static_cast<int64_t>(rel_end), len);
        Value fill_val = args.empty() ? Value::undefined() : args[0];
        if (this_raw->object_kind() == ObjectKind::kArray) {
            auto* arr = static_cast<JSObject*>(this_raw);
            for (auto i = k; i < final_end; ++i) {
                arr->elements_[static_cast<uint32_t>(i)] = fill_val;
            }
        }
        return EvalResult::ok(this_val);
    });
    array_prototype_->define_builtin_property("fill", Value::object(ObjectPtr(fill_fn)));

    // Array.prototype.copyWithin
    auto copywithin_fn = RcPtr<JSFunction>::make();
    copywithin_fn->set_name(std::string("copyWithin"));
    copywithin_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        auto* raw = this_val.as_object_raw();
        if (!raw) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                              "Array.prototype.copyWithin called on null or undefined");
            return EvalResult::err(Error{ErrorKind::Runtime, kPendingThrowSentinel});
        }
        if (raw->object_kind() != ObjectKind::kArray) return EvalResult::ok(this_val);
        auto* arr = static_cast<JSObject*>(raw);
        int64_t len = arr->array_length_;
        int64_t target = args.size() > 0 ? static_cast<int64_t>(to_number_double(args[0])) : 0;
        if (target < 0) target = std::max(len + target, INT64_C(0));
        else target = std::min(target, len);
        int64_t start = args.size() > 1 ? static_cast<int64_t>(to_number_double(args[1])) : 0;
        if (start < 0) start = std::max(len + start, INT64_C(0));
        else start = std::min(start, len);
        int64_t end = len;
        if (args.size() > 2 && !args[2].is_undefined()) {
            end = static_cast<int64_t>(to_number_double(args[2]));
            if (end < 0) end = std::max(len + end, INT64_C(0));
            else end = std::min(end, len);
        }
        int64_t count = std::min(end - start, len - target);
        if (count > 0) {
            if (target < start || target >= start + count) {
                for (int64_t i = 0; i < count; ++i) {
                    auto it = arr->elements_.find(static_cast<uint32_t>(start + i));
                    if (it != arr->elements_.end()) {
                        arr->elements_[static_cast<uint32_t>(target + i)] = it->second;
                    } else {
                        arr->elements_.erase(static_cast<uint32_t>(target + i));
                    }
                }
            } else {
                for (int64_t i = count - 1; i >= 0; --i) {
                    auto it = arr->elements_.find(static_cast<uint32_t>(start + i));
                    if (it != arr->elements_.end()) {
                        arr->elements_[static_cast<uint32_t>(target + i)] = it->second;
                    } else {
                        arr->elements_.erase(static_cast<uint32_t>(target + i));
                    }
                }
            }
        }
        return EvalResult::ok(this_val);
    });
    array_prototype_->define_builtin_property("copyWithin", Value::object(ObjectPtr(copywithin_fn)));

    // Array.prototype.shift
    auto shift_fn = RcPtr<JSFunction>::make();
    shift_fn->set_name(std::string("shift"));
    shift_fn->set_native_fn([this](Value this_val, std::vector<Value> /*args*/, bool) -> EvalResult {
        auto* raw = this_val.as_object_raw();
        if (!raw || raw->object_kind() != ObjectKind::kArray) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                              "Array.prototype.shift called on null or undefined");
            return EvalResult::err(Error{ErrorKind::Runtime, kPendingThrowSentinel});
        }
        auto* arr = static_cast<JSObject*>(raw);
        if (arr->array_length_ == 0) return EvalResult::ok(Value::undefined());
        auto it = arr->elements_.find(0);
        Value first = (it != arr->elements_.end()) ? std::move(it->second) : Value::undefined();
        arr->elements_.erase(0);
        // Shift remaining elements down
        std::vector<std::pair<uint32_t, Value>> shifted;
        for (auto& [idx, val] : arr->elements_) {
            if (idx > 0) shifted.emplace_back(idx - 1, std::move(val));
        }
        arr->elements_.clear();
        for (auto& [idx, val] : shifted) {
            arr->elements_[idx] = std::move(val);
        }
        arr->array_length_--;
        return EvalResult::ok(first);
    });
    array_prototype_->define_builtin_property("shift", Value::object(ObjectPtr(shift_fn)));

    // Array.prototype.unshift
    auto unshift_fn = RcPtr<JSFunction>::make();
    unshift_fn->set_name(std::string("unshift"));
    unshift_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        auto* raw = this_val.as_object_raw();
        if (!raw || raw->object_kind() != ObjectKind::kArray) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                              "Array.prototype.unshift called on null or undefined");
            return EvalResult::err(Error{ErrorKind::Runtime, kPendingThrowSentinel});
        }
        auto* arr = static_cast<JSObject*>(raw);
        uint32_t arg_count = static_cast<uint32_t>(args.size());
        if (arg_count > 0) {
            // Shift existing elements up
            std::vector<std::pair<uint32_t, Value>> shifted;
            for (auto& [idx, val] : arr->elements_) {
                shifted.emplace_back(idx + arg_count, std::move(val));
            }
            arr->elements_.clear();
            for (auto& [idx, val] : shifted) {
                arr->elements_[idx] = std::move(val);
            }
            // Insert new elements at the front
            for (uint32_t i = 0; i < arg_count; ++i) {
                arr->elements_[i] = std::move(args[i]);
            }
        }
        arr->array_length_ += arg_count;
        return EvalResult::ok(Value::number(static_cast<double>(arr->array_length_)));
    });
    array_prototype_->define_builtin_property("unshift", Value::object(ObjectPtr(unshift_fn)));

    // Array.prototype.lastIndexOf
    auto lastindexof_fn = RcPtr<JSFunction>::make();
    lastindexof_fn->set_name(std::string("lastIndexOf"));
    lastindexof_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        auto* raw = this_val.as_object_raw();
        if (!raw || raw->object_kind() != ObjectKind::kArray) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                              "Array.prototype.lastIndexOf called on null or undefined");
            return EvalResult::err(Error{ErrorKind::Runtime, kPendingThrowSentinel});
        }
        auto* arr = static_cast<JSObject*>(raw);
        if (args.empty()) return EvalResult::ok(Value::number(-1));
        Value search = args[0];
        int64_t len = arr->array_length_;
        if (len == 0) return EvalResult::ok(Value::number(-1));
        int64_t from = len - 1;
        if (args.size() > 1) {
            double d = to_number_double(args[1]);
            from = std::isnan(d) ? 0 : static_cast<int64_t>(std::trunc(d));
            if (from < 0) from = std::max(len + from, INT64_C(0));
            else from = std::min(from, len - 1);
        }
        for (int64_t k = from; k >= 0; --k) {
            auto it = arr->elements_.find(static_cast<uint32_t>(k));
            if (it != arr->elements_.end() && strict_eq_values(it->second, search)) {
                return EvalResult::ok(Value::number(static_cast<double>(k)));
            }
        }
        return EvalResult::ok(Value::number(-1));
    });
    array_prototype_->define_builtin_property("lastIndexOf", Value::object(ObjectPtr(lastindexof_fn)));

    // Array.prototype.at(index)
    {
        auto at_fn = RcPtr<JSFunction>::make();
        at_fn->set_name(std::string("at"));
        at_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
            if (!this_val.is_object() || this_val.as_object_raw()->object_kind() != ObjectKind::kArray) {
                pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                                  "Array.prototype.at called on non-array");
                return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
            }
            auto* arr = static_cast<JSObject*>(this_val.as_object_raw());
            uint32_t len = arr->array_length_;
            double idx_d = args.empty() ? 0.0 : to_number_double(args[0]);
            if (std::isnan(idx_d) || std::isinf(idx_d)) idx_d = 0.0;
            double truncated = std::trunc(idx_d);
            if (truncated < 0.0) truncated = static_cast<double>(len) + truncated;
            if (truncated < 0.0 || truncated >= static_cast<double>(len))
                return EvalResult::ok(Value::undefined());
            uint32_t actual = static_cast<uint32_t>(truncated);
            auto it = arr->elements_.find(actual);
            if (it == arr->elements_.end()) return EvalResult::ok(Value::undefined());
            return EvalResult::ok(it->second);
        });
        gc_heap_.Register(at_fn.get());
        array_prototype_->define_builtin_property("at", Value::object(ObjectPtr(at_fn)));
    }

    // Array.prototype[Symbol.iterator]
    {
        auto array_iter_fn = RcPtr<JSFunction>::make();
        array_iter_fn->set_name(std::string("[Symbol.iterator]"));
        array_iter_fn->set_native_fn([this](Value this_val, std::vector<Value>, bool) -> EvalResult {
            auto iter_obj = RcPtr<JSObject>::make();
            gc_heap_.Register(iter_obj.get());
            iter_obj->set_property("__arr__", this_val);
            iter_obj->set_property("__idx__", Value::number(0.0));

            auto next_fn = RcPtr<JSFunction>::make();
            next_fn->set_name(std::string("next"));
            next_fn->set_native_fn([this](Value this_val, std::vector<Value>, bool) -> EvalResult {
                auto make_done = [&]() -> EvalResult {
                    auto r = RcPtr<JSObject>::make();
                    gc_heap_.Register(r.get());
                    r->set_property("value", Value::undefined());
                    r->set_property("done", Value::boolean(true));
                    return EvalResult::ok(Value::object(ObjectPtr(r)));
                };
                if (!this_val.is_object()) return make_done();
                auto* iter = static_cast<JSObject*>(this_val.as_object_raw());
                Value arr_val = iter->get_property("__arr__");
                Value idx_val = iter->get_property("__idx__");
                uint32_t idx = idx_val.is_number() ? static_cast<uint32_t>(idx_val.as_number()) : 0u;
                if (!arr_val.is_object() || arr_val.as_object_raw()->object_kind() != ObjectKind::kArray)
                    return make_done();
                auto* arr = static_cast<JSObject*>(arr_val.as_object_raw());
                if (idx >= arr->array_length_) return make_done();
                auto elem_it = arr->elements_.find(idx);
                Value value = (elem_it != arr->elements_.end()) ? elem_it->second : Value::undefined();
                iter->set_property("__idx__", Value::number(static_cast<double>(idx + 1)));
                auto r = RcPtr<JSObject>::make();
                gc_heap_.Register(r.get());
                r->set_property("value", std::move(value));
                r->set_property("done", Value::boolean(false));
                return EvalResult::ok(Value::object(ObjectPtr(r)));
            });
            gc_heap_.Register(next_fn.get());
            iter_obj->set_property("next", Value::object(ObjectPtr(next_fn)));
            // Iterator is also iterable: [Symbol.iterator]() { return this; }
            auto self_iter_fn = RcPtr<JSFunction>::make();
            self_iter_fn->set_name(std::string("[Symbol.iterator]"));
            self_iter_fn->set_native_fn([](Value this_val, std::vector<Value>, bool) -> EvalResult {
                return EvalResult::ok(this_val);
            });
            gc_heap_.Register(self_iter_fn.get());
            iter_obj->set_property_by_symbol(symbol_table_.well_known_iterator,
                                              Value::object(ObjectPtr(self_iter_fn)));
            return EvalResult::ok(Value::object(ObjectPtr(iter_obj)));
        });
        gc_heap_.Register(array_iter_fn.get());
        array_prototype_->set_property_by_symbol(symbol_table_.well_known_iterator,
                                                  Value::object(ObjectPtr(array_iter_fn)));
    }

    // Array.prototype.toString: equivalent to join(",")
    {
        auto tostring_fn = RcPtr<JSFunction>::make();
        tostring_fn->set_name(std::string("toString"));
        tostring_fn->set_native_fn([](Value this_val, std::vector<Value>, bool) -> EvalResult {
            if (!this_val.is_object() || this_val.as_object_raw()->object_kind() != ObjectKind::kArray)
                return EvalResult::ok(Value::string("[object Array]"));
            auto* arr = static_cast<JSObject*>(this_val.as_object_raw());
            std::string result;
            uint32_t len = arr->array_length_;
            for (uint32_t i = 0; i < len; i++) {
                if (i > 0) result += ",";
                auto it = arr->elements_.find(i);
                if (it != arr->elements_.end() && !it->second.is_null() && !it->second.is_undefined())
                    result += to_string_val(it->second);
            }
            return EvalResult::ok(Value::string(result));
        });
        array_prototype_->define_builtin_property("toString", Value::object(ObjectPtr(tostring_fn)));
    }

    // Build Array constructor
    auto array_constructor = RcPtr<JSFunction>::make();
    array_constructor->set_name(std::string("Array"));
    array_constructor->set_prototype_obj(array_prototype_);
    {
        auto isarray_fn = RcPtr<JSFunction>::make();
        isarray_fn->set_name(std::string("isArray"));
        isarray_fn->set_native_fn([](Value, std::vector<Value> args, bool) -> EvalResult {
            if (args.empty() || !args[0].is_object()) return EvalResult::ok(Value::boolean(false));
            RcObject* raw = args[0].as_object_raw();
            return EvalResult::ok(Value::boolean(raw && raw->object_kind() == ObjectKind::kArray));
        });
        array_constructor->set_property("isArray", Value::object(ObjectPtr(isarray_fn)));
    }
    // Array.from(arrayLike[, mapFn])
    {
        auto from_fn = RcPtr<JSFunction>::make();
        from_fn->set_name(std::string("from"));
        from_fn->set_property("length", Value::number(1.0));
        from_fn->set_native_fn([this](Value, std::vector<Value> args, bool) -> EvalResult {
            if (args.empty()) {
                pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                    "undefined is not iterable");
                return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
            }
            Value iterable = args[0];
            Value map_fn = args.size() >= 2 ? args[1] : Value::undefined();
            bool has_map = map_fn.is_object() && map_fn.as_object_raw() &&
                           map_fn.as_object_raw()->object_kind() == ObjectKind::kFunction;
            auto arr = RcPtr<JSObject>::make(ObjectKind::kArray);
            gc_heap_.Register(arr.get());
            arr->set_proto(array_prototype_);
            // array-like path: has .length but no Symbol.iterator
            if (iterable.is_object()) {
                RcObject* raw = iterable.as_object_raw();
                // Check for Symbol.iterator first
                Value iter_factory = Value::undefined();
                if (raw->object_kind() == ObjectKind::kOrdinary || raw->object_kind() == ObjectKind::kArray ||
                    raw->object_kind() == ObjectKind::kGenerator ||
                    raw->object_kind() == ObjectKind::kMap || raw->object_kind() == ObjectKind::kSet) {
                    iter_factory = static_cast<JSObject*>(raw)->get_property_by_symbol(
                        symbol_table_.well_known_iterator);
                }
                if (iter_factory.is_undefined() && raw->object_kind() == ObjectKind::kOrdinary) {
                    // array-like path
                    auto* obj = static_cast<JSObject*>(raw);
                    Value len_val = obj->get_property("length");
                    double len_num = len_val.is_number() ? len_val.as_number() : 0.0;
                    if (!std::isnan(len_num) && len_num > 0.0) {
                        uint32_t len = static_cast<uint32_t>(std::min(len_num, static_cast<double>(UINT32_MAX)));
                        for (uint32_t i = 0; i < len; ++i) {
                            Value elem = obj->get_property(std::to_string(i));
                            if (has_map) {
                                std::vector<Value> map_args = {elem, Value::number(static_cast<double>(i))};
                                auto res = call_function_val(map_fn, Value::undefined(),
                                    std::span<Value>(map_args.data(), map_args.size()));
                                if (!res.is_ok()) return res;
                                elem = res.value();
                            }
                            arr->elements_[i] = std::move(elem);
                        }
                        arr->array_length_ = len;
                    }
                    return EvalResult::ok(Value::object(ObjectPtr(arr)));
                }
            }
            // iterable path: use spread_into to collect, then apply mapFn
            std::vector<Value> items;
            if (iterable.is_string()) {
                spread_into(iterable, items);
            } else if (iterable.is_object()) {
                if (!spread_into(iterable, items)) {
                    pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                        "value is not iterable");
                    return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
                }
            } else {
                pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                    "value is not iterable");
                return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
            }
            for (size_t i = 0; i < items.size(); ++i) {
                Value elem = std::move(items[i]);
                if (has_map) {
                    std::vector<Value> map_args = {elem, Value::number(static_cast<double>(i))};
                    auto res = call_function_val(map_fn, Value::undefined(),
                        std::span<Value>(map_args.data(), map_args.size()));
                    if (!res.is_ok()) return res;
                    elem = res.value();
                }
                arr->elements_[static_cast<uint32_t>(i)] = std::move(elem);
            }
            arr->array_length_ = static_cast<uint32_t>(items.size());
            return EvalResult::ok(Value::object(ObjectPtr(arr)));
        });
        gc_heap_.Register(from_fn.get());
        array_constructor->set_property("from", Value::object(ObjectPtr(from_fn)));
    }
    // Array.of(...items)
    {
        auto of_fn = RcPtr<JSFunction>::make();
        of_fn->set_name(std::string("of"));
        of_fn->set_native_fn([this](Value, std::vector<Value> args, bool) -> EvalResult {
            auto arr = RcPtr<JSObject>::make(ObjectKind::kArray);
            gc_heap_.Register(arr.get());
            arr->set_proto(array_prototype_);
            for (size_t i = 0; i < args.size(); ++i) {
                arr->elements_[static_cast<uint32_t>(i)] = std::move(args[i]);
            }
            arr->array_length_ = static_cast<uint32_t>(args.size());
            return EvalResult::ok(Value::object(ObjectPtr(arr)));
        });
        gc_heap_.Register(of_fn.get());
        array_constructor->set_property("of", Value::object(ObjectPtr(of_fn)));
    }
    array_constructor->set_native_fn([this](Value, std::vector<Value> args, bool) -> EvalResult {
        auto arr = RcPtr<JSObject>::make(ObjectKind::kArray);
        gc_heap_.Register(arr.get());
        arr->set_proto(array_prototype_);
        if (args.size() == 1 && args[0].is_number()) {
            double v = args[0].as_number();
            uint32_t len = static_cast<uint32_t>(v);
            if (static_cast<double>(len) != v || len > UINT32_MAX) {
                return EvalResult::err(Error{ErrorKind::Runtime, "RangeError: Invalid array length"});
            }
            arr->array_length_ = len;
        } else {
            for (size_t i = 0; i < args.size(); ++i) {
                arr->elements_[static_cast<uint32_t>(i)] = std::move(args[i]);
            }
            arr->array_length_ = static_cast<uint32_t>(args.size());
        }
        return EvalResult::ok(Value::object(ObjectPtr(arr)));
    });

    array_prototype_->define_builtin_property("constructor", Value::object(ObjectPtr(array_constructor)));

    gc_heap_.Register(array_constructor.get());
    global_env_->define_initialized("Array");
    global_env_->set("Array", Value::object(ObjectPtr(array_constructor)));

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
    object_prototype_->define_builtin_property("constructor", Value::object(ObjectPtr(object_constructor_)));
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
    // Build Object.defineProperty
    auto define_property_fn = RcPtr<JSFunction>::make();
    define_property_fn->set_name(std::string("defineProperty"));
    define_property_fn->set_native_fn([this](Value /*this_val*/, std::vector<Value> args, bool) -> EvalResult {
        if (args.size() < 1 || !args[0].is_object()) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "Object.defineProperty called on non-object");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        RcObject* raw = args[0].as_object_raw();
        // For kFunction, handle defineProperty via own_properties_
        if (raw->object_kind() == ObjectKind::kFunction) {
            auto* fn = static_cast<JSFunction*>(raw);
            if (args.size() >= 2 && args.size() >= 3 && args[2].is_object()) {
                std::string key2 = to_string_val(args[1]);
                auto* desc_fn = static_cast<JSObject*>(args[2].as_object_raw());
                Value val2 = desc_fn->has_own_property("value") ? desc_fn->get_property("value") : Value::undefined();
                if (desc_fn->has_own_property("value") || desc_fn->has_own_property("get")) {
                    fn->set_property(key2, val2);
                }
            }
            return EvalResult::ok(args[0]);
        }
        if (raw->object_kind() != ObjectKind::kOrdinary && raw->object_kind() != ObjectKind::kArray &&
            raw->object_kind() != ObjectKind::kRegExp && raw->object_kind() != ObjectKind::kStringObject &&
            raw->object_kind() != ObjectKind::kBooleanObject) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "Object.defineProperty called on non-ordinary object");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        auto* obj = static_cast<JSObject*>(raw);
        // Symbol key: store as symbol property (data value only)
        if (args.size() >= 2 && args[1].is_symbol()) {
            uint64_t sym_id = args[1].as_symbol_id();
            if (args.size() < 3 || !args[2].is_object()) {
                pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                    "Property description must be an object");
                return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
            }
            RcObject* desc_raw2 = args[2].as_object_raw();
            if (desc_raw2->object_kind() != ObjectKind::kOrdinary) {
                pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                    "Property description must be an object");
                return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
            }
            auto* desc_obj2 = static_cast<JSObject*>(desc_raw2);
            Value val = desc_obj2->has_own_property("value") ? desc_obj2->get_property("value") : Value::undefined();
            obj->set_property_by_symbol(sym_id, std::move(val));
            return EvalResult::ok(args[0]);
        }
        std::string key = args.size() >= 2 ? to_string_val(args[1]) : "undefined";
        if (args.size() < 3 || !args[2].is_object()) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "Property description must be an object");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        RcObject* desc_raw = args[2].as_object_raw();
        if (desc_raw->object_kind() != ObjectKind::kOrdinary) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "Property description must be an object");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        auto* desc_obj = static_cast<JSObject*>(desc_raw);
        PropDesc pd;
        bool has_value = desc_obj->has_own_property("value");
        bool has_writable = desc_obj->has_own_property("writable");
        bool has_get = desc_obj->has_own_property("get");
        bool has_set = desc_obj->has_own_property("set");
        bool has_enumerable = desc_obj->has_own_property("enumerable");
        bool has_configurable = desc_obj->has_own_property("configurable");
        // data + accessor mixed is TypeError
        if ((has_value || has_writable) && (has_get || has_set)) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "Invalid property descriptor. Cannot both specify accessors and a value or writable attribute");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        if (has_value) pd.value = desc_obj->get_property("value");
        if (has_writable) pd.writable = to_boolean(desc_obj->get_property("writable"));
        if (has_get) {
            Value get_val = desc_obj->get_property("get");
            if (!get_val.is_undefined() && !get_val.is_null()) {
                if (!get_val.is_object() || get_val.as_object_raw()->object_kind() != ObjectKind::kFunction) {
                    pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                        "Getter must be a function");
                    return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
                }
            }
            pd.getter = std::move(get_val);
        }
        if (has_set) {
            Value set_val = desc_obj->get_property("set");
            if (!set_val.is_undefined() && !set_val.is_null()) {
                if (!set_val.is_object() || set_val.as_object_raw()->object_kind() != ObjectKind::kFunction) {
                    pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                        "Setter must be a function");
                    return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
                }
            }
            pd.setter = std::move(set_val);
        }
        if (has_enumerable) pd.enumerable = to_boolean(desc_obj->get_property("enumerable"));
        if (has_configurable) pd.configurable = to_boolean(desc_obj->get_property("configurable"));
        auto res = obj->define_property(key, pd);
        if (!res.is_ok()) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                strip_error_prefix(res.error().message()));
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        return EvalResult::ok(args[0]);
    });

    // Build Object.getOwnPropertyDescriptor
    auto get_own_prop_desc_fn = RcPtr<JSFunction>::make();
    get_own_prop_desc_fn->set_name(std::string("getOwnPropertyDescriptor"));
    get_own_prop_desc_fn->set_native_fn([this](Value /*this_val*/, std::vector<Value> args, bool) -> EvalResult {
        if (args.size() < 1) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "Object.getOwnPropertyDescriptor called on non-object");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        // ES2015+: null/undefined 仍然 TypeError，其他原始值返回 undefined
        if (!args[0].is_object()) {
            if (args[0].is_null() || args[0].is_undefined()) {
                pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                    "Object.getOwnPropertyDescriptor called on null or undefined");
                return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
            }
            return EvalResult::ok(Value::undefined());
        }
        RcObject* raw = args[0].as_object_raw();
        std::string key = args.size() >= 2 ? to_string_val(args[1]) : "undefined";
        auto desc_obj = RcPtr<JSObject>::make();
        gc_heap_.Register(desc_obj.get());
        desc_obj->set_proto(object_prototype_);
        if (raw->object_kind() == ObjectKind::kFunction) {
            auto* fn = static_cast<JSFunction*>(raw);
            auto it = fn->own_properties().find(key);
            if (it == fn->own_properties().end()) return EvalResult::ok(Value::undefined());
            desc_obj->set_property("value", it->second);
            desc_obj->set_property("writable", Value::boolean(true));
            desc_obj->set_property("enumerable", Value::boolean(false));
            desc_obj->set_property("configurable", Value::boolean(true));
            return EvalResult::ok(Value::object(ObjectPtr(desc_obj)));
        }
        if (raw->object_kind() != ObjectKind::kOrdinary && raw->object_kind() != ObjectKind::kArray &&
            raw->object_kind() != ObjectKind::kRegExp && raw->object_kind() != ObjectKind::kStringObject &&
            raw->object_kind() != ObjectKind::kBooleanObject &&
            raw->object_kind() != ObjectKind::kMap && raw->object_kind() != ObjectKind::kSet) {
            return EvalResult::ok(Value::undefined());
        }
        auto* obj = static_cast<JSObject*>(raw);
        const JSObject::PropertyEntry* entry = obj->get_own_entry(key);
        if (entry == nullptr) return EvalResult::ok(Value::undefined());
        bool is_accessor = (entry->flags & kPropIsAccessor) != 0;
        if (is_accessor) {
            desc_obj->set_property("get", entry->getter.is_undefined() ? Value::undefined() : entry->getter);
            desc_obj->set_property("set", entry->setter.is_undefined() ? Value::undefined() : entry->setter);
        } else {
            desc_obj->set_property("value", entry->value);
            desc_obj->set_property("writable", Value::boolean((entry->flags & kPropWritable) != 0));
        }
        desc_obj->set_property("enumerable", Value::boolean((entry->flags & kPropEnumerable) != 0));
        desc_obj->set_property("configurable", Value::boolean((entry->flags & kPropConfigurable) != 0));
        return EvalResult::ok(Value::object(ObjectPtr(desc_obj)));
    });

    // Build Object.preventExtensions
    auto prevent_extensions_fn = RcPtr<JSFunction>::make();
    prevent_extensions_fn->set_name(std::string("preventExtensions"));
    prevent_extensions_fn->set_native_fn([this](Value /*this_val*/, std::vector<Value> args, bool) -> EvalResult {
        if (args.empty() || !args[0].is_object()) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "Object.preventExtensions called on non-object");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        RcObject* raw = args[0].as_object_raw();
        if (raw->object_kind() == ObjectKind::kOrdinary || raw->object_kind() == ObjectKind::kArray) {
            static_cast<JSObject*>(raw)->set_extensible(false);
        }
        return EvalResult::ok(args[0]);
    });

    // Build Object.freeze
    auto freeze_fn = RcPtr<JSFunction>::make();
    freeze_fn->set_name(std::string("freeze"));
    freeze_fn->set_native_fn([](Value /*this_val*/, std::vector<Value> args, bool) -> EvalResult {
        if (args.empty()) return EvalResult::ok(Value::undefined());
        if (!args[0].is_object()) return EvalResult::ok(args[0]);
        RcObject* raw = args[0].as_object_raw();
        if (raw->object_kind() == ObjectKind::kOrdinary || raw->object_kind() == ObjectKind::kArray) {
            static_cast<JSObject*>(raw)->freeze();
        }
        return EvalResult::ok(args[0]);
    });
    gc_heap_.Register(freeze_fn.get());

    // Build Object.isFrozen
    auto is_frozen_fn = RcPtr<JSFunction>::make();
    is_frozen_fn->set_name(std::string("isFrozen"));
    is_frozen_fn->set_native_fn([](Value /*this_val*/, std::vector<Value> args, bool) -> EvalResult {
        if (args.empty() || !args[0].is_object()) return EvalResult::ok(Value::boolean(true));
        RcObject* raw = args[0].as_object_raw();
        if (raw->object_kind() == ObjectKind::kOrdinary || raw->object_kind() == ObjectKind::kArray) {
            return EvalResult::ok(Value::boolean(static_cast<JSObject*>(raw)->is_frozen()));
        }
        return EvalResult::ok(Value::boolean(false));
    });
    gc_heap_.Register(is_frozen_fn.get());

    // Build Object.seal
    auto seal_fn = RcPtr<JSFunction>::make();
    seal_fn->set_name(std::string("seal"));
    seal_fn->set_native_fn([](Value /*this_val*/, std::vector<Value> args, bool) -> EvalResult {
        if (args.empty()) return EvalResult::ok(Value::undefined());
        if (!args[0].is_object()) return EvalResult::ok(args[0]);
        RcObject* raw = args[0].as_object_raw();
        if (raw->object_kind() == ObjectKind::kOrdinary || raw->object_kind() == ObjectKind::kArray) {
            static_cast<JSObject*>(raw)->seal();
        }
        return EvalResult::ok(args[0]);
    });
    gc_heap_.Register(seal_fn.get());

    // Build Object.isSealed
    auto is_sealed_fn = RcPtr<JSFunction>::make();
    is_sealed_fn->set_name(std::string("isSealed"));
    is_sealed_fn->set_native_fn([](Value /*this_val*/, std::vector<Value> args, bool) -> EvalResult {
        if (args.empty() || !args[0].is_object()) return EvalResult::ok(Value::boolean(true));
        RcObject* raw = args[0].as_object_raw();
        if (raw->object_kind() == ObjectKind::kOrdinary || raw->object_kind() == ObjectKind::kArray) {
            return EvalResult::ok(Value::boolean(static_cast<JSObject*>(raw)->is_sealed()));
        }
        return EvalResult::ok(Value::boolean(false));
    });
    gc_heap_.Register(is_sealed_fn.get());

    // Build Object.getPrototypeOf
    auto get_proto_fn = RcPtr<JSFunction>::make();
    get_proto_fn->set_name(std::string("getPrototypeOf"));
    get_proto_fn->set_native_fn([this](Value /*this_val*/, std::vector<Value> args, bool) -> EvalResult {
        if (args.empty()) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "Object.getPrototypeOf called on non-object");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        // ES2015+: ToObject(O) — 原始值自动装箱
        if (!args[0].is_object()) {
            if (args[0].is_null() || args[0].is_undefined()) {
                pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                    "Object.getPrototypeOf called on null or undefined");
                return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
            }
            // 返回对应的 prototype
            if (args[0].is_string() && string_prototype_)
                return EvalResult::ok(Value::object(ObjectPtr(string_prototype_)));
            if (args[0].is_number() && number_prototype_)
                return EvalResult::ok(Value::object(ObjectPtr(number_prototype_)));
            if (args[0].is_bool() && boolean_prototype_)
                return EvalResult::ok(Value::object(ObjectPtr(boolean_prototype_)));
            return EvalResult::ok(Value::null());
        }
        RcObject* raw = args[0].as_object_raw();
        if (raw->object_kind() == ObjectKind::kOrdinary || raw->object_kind() == ObjectKind::kArray) {
            auto* obj = static_cast<JSObject*>(raw);
            if (obj->proto()) return EvalResult::ok(Value::object(ObjectPtr(obj->proto())));
            return EvalResult::ok(Value::null());
        }
        if (raw->object_kind() == ObjectKind::kFunction) {
            // Function.prototype is object_prototype_ (not the function's own .prototype)
            if (function_prototype_) return EvalResult::ok(Value::object(ObjectPtr(function_prototype_)));
            return EvalResult::ok(Value::null());
        }
        return EvalResult::ok(Value::null());
    });
    gc_heap_.Register(get_proto_fn.get());

    // Object.values(obj)
    auto values_fn = RcPtr<JSFunction>::make();
    values_fn->set_name(std::string("values"));
    values_fn->set_native_fn([this](Value, std::vector<Value> args, bool) -> EvalResult {
        if (args.empty() || !args[0].is_object()) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                              "Object.values called on non-object");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        RcObject* raw = args[0].as_object_raw();
        auto arr = RcPtr<JSObject>::make(ObjectKind::kArray);
        gc_heap_.Register(arr.get());
        arr->set_proto(array_prototype_);
        if (raw->object_kind() != ObjectKind::kFunction) {
            auto* obj = static_cast<JSObject*>(raw);
            auto keys = obj->own_enumerable_string_keys();
            for (size_t i = 0; i < keys.size(); ++i) {
                arr->elements_[static_cast<uint32_t>(i)] = obj->get_property(keys[i]);
            }
            arr->array_length_ = static_cast<uint32_t>(keys.size());
        }
        return EvalResult::ok(Value::object(ObjectPtr(arr)));
    });
    gc_heap_.Register(values_fn.get());

    // Object.entries(obj)
    auto entries_fn = RcPtr<JSFunction>::make();
    entries_fn->set_name(std::string("entries"));
    entries_fn->set_native_fn([this](Value, std::vector<Value> args, bool) -> EvalResult {
        if (args.empty() || !args[0].is_object()) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                              "Object.entries called on non-object");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        RcObject* raw = args[0].as_object_raw();
        auto arr = RcPtr<JSObject>::make(ObjectKind::kArray);
        gc_heap_.Register(arr.get());
        arr->set_proto(array_prototype_);
        if (raw->object_kind() != ObjectKind::kFunction) {
            auto* obj = static_cast<JSObject*>(raw);
            auto keys = obj->own_enumerable_string_keys();
            for (size_t i = 0; i < keys.size(); ++i) {
                auto pair = RcPtr<JSObject>::make(ObjectKind::kArray);
                gc_heap_.Register(pair.get());
                pair->set_proto(array_prototype_);
                pair->elements_[0] = Value::string(keys[i]);
                pair->elements_[1] = obj->get_property(keys[i]);
                pair->array_length_ = 2;
                arr->elements_[static_cast<uint32_t>(i)] = Value::object(ObjectPtr(pair));
            }
            arr->array_length_ = static_cast<uint32_t>(keys.size());
        }
        return EvalResult::ok(Value::object(ObjectPtr(arr)));
    });
    gc_heap_.Register(entries_fn.get());

    // Object.fromEntries(iterable)
    auto from_entries_fn = RcPtr<JSFunction>::make();
    from_entries_fn->set_name(std::string("fromEntries"));
    from_entries_fn->set_native_fn([this](Value, std::vector<Value> args, bool) -> EvalResult {
        if (args.empty()) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                              "Object.fromEntries requires an iterable argument");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        std::vector<Value> items;
        if (!spread_into(args[0], items)) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                              "Object.fromEntries argument is not iterable");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        auto new_obj = RcPtr<JSObject>::make();
        gc_heap_.Register(new_obj.get());
        new_obj->set_proto(object_prototype_);
        for (auto& item : items) {
            if (!item.is_object()) continue;
            RcObject* raw = item.as_object_raw();
            if (raw->object_kind() == ObjectKind::kArray) {
                auto* pair = static_cast<JSObject*>(raw);
                Value key_val = (pair->array_length_ > 0 && pair->elements_.count(0))
                    ? pair->elements_.at(0) : Value::undefined();
                Value val_val = (pair->array_length_ > 1 && pair->elements_.count(1))
                    ? pair->elements_.at(1) : Value::undefined();
                new_obj->set_property(to_string_val(key_val), val_val);
            } else if (raw->object_kind() == ObjectKind::kOrdinary) {
                auto* pair = static_cast<JSObject*>(raw);
                Value key_val = pair->get_property("0");
                Value val_val = pair->get_property("1");
                new_obj->set_property(to_string_val(key_val), val_val);
            }
        }
        return EvalResult::ok(Value::object(ObjectPtr(new_obj)));
    });
    gc_heap_.Register(from_entries_fn.get());

    // Object.getOwnPropertyNames(obj)
    auto get_own_prop_names_fn = RcPtr<JSFunction>::make();
    get_own_prop_names_fn->set_name(std::string("getOwnPropertyNames"));
    get_own_prop_names_fn->set_native_fn([this](Value, std::vector<Value> args, bool) -> EvalResult {
        if (args.empty() || !args[0].is_object()) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                              "Object.getOwnPropertyNames called on non-object");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        RcObject* raw = args[0].as_object_raw();
        auto arr = RcPtr<JSObject>::make(ObjectKind::kArray);
        gc_heap_.Register(arr.get());
        arr->set_proto(array_prototype_);
        if (raw->object_kind() != ObjectKind::kFunction) {
            auto* obj = static_cast<JSObject*>(raw);
            auto all_keys = obj->own_all_string_keys();
            for (size_t i = 0; i < all_keys.size(); ++i) {
                arr->elements_[static_cast<uint32_t>(i)] = Value::string(all_keys[i]);
            }
            arr->array_length_ = static_cast<uint32_t>(all_keys.size());
        }
        return EvalResult::ok(Value::object(ObjectPtr(arr)));
    });
    gc_heap_.Register(get_own_prop_names_fn.get());

    // Object.is(a, b) — SameValue algorithm
    auto object_is_fn = RcPtr<JSFunction>::make();
    object_is_fn->set_name(std::string("is"));
    object_is_fn->set_native_fn([](Value, std::vector<Value> args, bool) -> EvalResult {
        Value a = args.size() > 0 ? args[0] : Value::undefined();
        Value b = args.size() > 1 ? args[1] : Value::undefined();
        if (a.is_number() && b.is_number()) {
            double da = a.as_number();
            double db = b.as_number();
            if (std::isnan(da) && std::isnan(db)) return EvalResult::ok(Value::boolean(true));
            if (da == 0.0 && db == 0.0)
                return EvalResult::ok(Value::boolean(std::signbit(da) == std::signbit(db)));
            return EvalResult::ok(Value::boolean(da == db));
        }
        if (a.is_undefined() && b.is_undefined()) return EvalResult::ok(Value::boolean(true));
        if (a.is_null() && b.is_null()) return EvalResult::ok(Value::boolean(true));
        if (a.is_bool() && b.is_bool()) return EvalResult::ok(Value::boolean(a.as_bool() == b.as_bool()));
        if (a.is_string() && b.is_string()) return EvalResult::ok(Value::boolean(a.sv() == b.sv()));
        if (a.is_object() && b.is_object())
            return EvalResult::ok(Value::boolean(a.as_object_raw() == b.as_object_raw()));
        return EvalResult::ok(Value::boolean(false));
    });
    gc_heap_.Register(object_is_fn.get());

    // Object.setPrototypeOf(obj, proto)
    auto object_set_proto_fn = RcPtr<JSFunction>::make();
    object_set_proto_fn->set_name(std::string("setPrototypeOf"));
    object_set_proto_fn->set_native_fn([this](Value, std::vector<Value> args, bool) -> EvalResult {
        if (args.size() < 2 || !args[0].is_object()) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                              "Object.setPrototypeOf: first argument must be an object");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        Value& proto_val = args[1];
        if (!proto_val.is_null() && !proto_val.is_object()) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                              "Object.setPrototypeOf: proto must be an object or null");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        auto* obj = static_cast<JSObject*>(args[0].as_object_raw());
        if (proto_val.is_null()) {
            obj->set_proto(RcPtr<JSObject>{});
        } else {
            obj->set_proto(RcPtr<JSObject>(static_cast<JSObject*>(proto_val.as_object_raw())));
        }
        return EvalResult::ok(args[0]);
    });
    gc_heap_.Register(object_set_proto_fn.get());

    // Object.hasOwn(obj, key)
    auto object_has_own_fn = RcPtr<JSFunction>::make();
    object_has_own_fn->set_name(std::string("hasOwn"));
    object_has_own_fn->set_native_fn([this](Value, std::vector<Value> args, bool) -> EvalResult {
        if (args.size() < 2 || !args[0].is_object()) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                              "Object.hasOwn: first argument must be an object");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        std::string key = to_string_val(args[1]);
        auto* raw = args[0].as_object_raw();
        if (raw->object_kind() == ObjectKind::kFunction) {
            auto* fn = static_cast<JSFunction*>(raw);
            return EvalResult::ok(Value::boolean(fn->has_property(key)));
        }
        auto* obj = static_cast<JSObject*>(raw);
        return EvalResult::ok(Value::boolean(obj->get_own_entry(key) != nullptr));
    });
    gc_heap_.Register(object_has_own_fn.get());

    // Object.getOwnPropertySymbols(obj)
    auto get_own_prop_symbols_fn = RcPtr<JSFunction>::make();
    get_own_prop_symbols_fn->set_name(std::string("getOwnPropertySymbols"));
    get_own_prop_symbols_fn->set_native_fn([this](Value, std::vector<Value> args, bool) -> EvalResult {
        auto arr = RcPtr<JSObject>::make(ObjectKind::kArray);
        gc_heap_.Register(arr.get());
        arr->set_proto(array_prototype_);
        if (!args.empty() && args[0].is_object()) {
            RcObject* raw = args[0].as_object_raw();
            if (raw->object_kind() != ObjectKind::kFunction) {
                auto* obj = static_cast<JSObject*>(raw);
                auto sym_ids = obj->own_symbol_ids();
                for (uint32_t i = 0; i < static_cast<uint32_t>(sym_ids.size()); ++i) {
                    arr->elements_[i] = Value::symbol(sym_ids[i]);
                }
                arr->array_length_ = static_cast<uint32_t>(sym_ids.size());
            }
        }
        return EvalResult::ok(Value::object(ObjectPtr(arr)));
    });
    gc_heap_.Register(get_own_prop_symbols_fn.get());

    object_constructor_->set_property("keys", Value::object(ObjectPtr(keys_fn)));
    object_constructor_->set_property("assign", Value::object(ObjectPtr(assign_fn)));
    object_constructor_->set_property("create", Value::object(ObjectPtr(create_fn)));
    object_constructor_->set_property("defineProperty", Value::object(ObjectPtr(define_property_fn)));
    object_constructor_->set_property("getOwnPropertyDescriptor", Value::object(ObjectPtr(get_own_prop_desc_fn)));
    // Object.getOwnPropertyDescriptors(obj) - return all own property descriptors
    {
        auto fn = RcPtr<JSFunction>::make();
        fn->set_name(std::string("getOwnPropertyDescriptors"));
        fn->set_native_fn([this](Value /*this_val*/, std::vector<Value> args, bool) -> EvalResult {
            auto result = RcPtr<JSObject>::make();
            gc_heap_.Register(result.get());
            result->set_proto(object_prototype_);
            if (args.empty() || !args[0].is_object()) return EvalResult::ok(Value::object(ObjectPtr(result)));
            auto* raw = args[0].as_object_raw();
            if (raw->object_kind() == ObjectKind::kOrdinary || raw->object_kind() == ObjectKind::kArray) {
                auto* obj = static_cast<JSObject*>(raw);
                auto all_keys = obj->own_all_string_keys();
                for (const auto& key : all_keys) {
                    const JSObject::PropertyEntry* entry = obj->get_own_entry(key);
                    if (!entry) continue;
                    auto desc = RcPtr<JSObject>::make();
                    gc_heap_.Register(desc.get());
                    desc->set_proto(object_prototype_);
                    if (entry->flags & kPropIsAccessor) {
                        desc->set_property("get", entry->getter);
                        desc->set_property("set", entry->setter);
                    } else {
                        desc->set_property("value", entry->value);
                        desc->set_property("writable", Value::boolean((entry->flags & kPropWritable) != 0));
                    }
                    desc->set_property("enumerable", Value::boolean((entry->flags & kPropEnumerable) != 0));
                    desc->set_property("configurable", Value::boolean((entry->flags & kPropConfigurable) != 0));
                    result->set_property(key, Value::object(ObjectPtr(desc)));
                }
            }
            return EvalResult::ok(Value::object(ObjectPtr(result)));
        });
        object_constructor_->set_property("getOwnPropertyDescriptors", Value::object(ObjectPtr(fn)));
    }
    object_constructor_->set_property("preventExtensions", Value::object(ObjectPtr(prevent_extensions_fn)));
    object_constructor_->set_property("freeze", Value::object(ObjectPtr(freeze_fn)));
    object_constructor_->set_property("isFrozen", Value::object(ObjectPtr(is_frozen_fn)));
    object_constructor_->set_property("seal", Value::object(ObjectPtr(seal_fn)));
    object_constructor_->set_property("isSealed", Value::object(ObjectPtr(is_sealed_fn)));
    object_constructor_->set_property("getPrototypeOf", Value::object(ObjectPtr(get_proto_fn)));
    object_constructor_->set_property("values", Value::object(ObjectPtr(values_fn)));
    object_constructor_->set_property("entries", Value::object(ObjectPtr(entries_fn)));
    object_constructor_->set_property("fromEntries", Value::object(ObjectPtr(from_entries_fn)));
    object_constructor_->set_property("getOwnPropertyNames", Value::object(ObjectPtr(get_own_prop_names_fn)));
    object_constructor_->set_property("is", Value::object(ObjectPtr(object_is_fn)));
    object_constructor_->set_property("setPrototypeOf", Value::object(ObjectPtr(object_set_proto_fn)));
    object_constructor_->set_property("hasOwn", Value::object(ObjectPtr(object_has_own_fn)));
    object_constructor_->set_property("getOwnPropertySymbols", Value::object(ObjectPtr(get_own_prop_symbols_fn)));

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
    function_prototype_->define_builtin_property("call", Value::object(ObjectPtr(call_fn)));

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
    function_prototype_->define_builtin_property("apply", Value::object(ObjectPtr(apply_fn)));

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
    function_prototype_->define_builtin_property("bind", Value::object(ObjectPtr(bind_fn)));

    // Function.prototype.toString
    {
        auto fn = RcPtr<JSFunction>::make();
        fn->set_name(std::string("toString"));
        fn->set_native_fn([this](Value this_val, std::vector<Value>, bool) -> EvalResult {
            if (!this_val.is_object() || !this_val.as_object_raw() ||
                this_val.as_object_raw()->object_kind() != ObjectKind::kFunction) {
                pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                    "Function.prototype.toString requires a function");
                return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
            }
            auto* fn_raw = static_cast<JSFunction*>(this_val.as_object_raw());
            std::string name;
            Value name_prop = fn_raw->get_property("name");
            if (name_prop.is_string()) {
                name = std::string(name_prop.sv());
            } else if (fn_raw->name().has_value()) {
                name = fn_raw->name().value();
            }
            return EvalResult::ok(Value::string("function " + name + "() { [native code] }"));
        });
        gc_heap_.Register(fn.get());
        function_prototype_->define_builtin_property("toString", Value::object(ObjectPtr(fn)));
    }

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
    promise_prototype_->define_builtin_property("then", Value::object(ObjectPtr(then_fn)));

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
    promise_prototype_->define_builtin_property("catch", Value::object(ObjectPtr(catch_fn)));

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
    promise_prototype_->define_builtin_property("finally", Value::object(ObjectPtr(finally_fn)));

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

    // Promise.all(iterable)
    {
        auto all_fn = RcPtr<JSFunction>::make();
        all_fn->set_name(std::string("all"));
        all_fn->set_native_fn([this](Value, std::vector<Value> args, bool) -> EvalResult {
            std::vector<Value> items;
            if (!args.empty() && args[0].is_object()) {
                spread_into(args[0], items);
            }
            auto result_promise = RcPtr<JSPromise>::make();
            gc_heap_.Register(result_promise.get());
            if (items.empty()) {
                auto results_arr = RcPtr<JSObject>::make(ObjectKind::kArray);
                gc_heap_.Register(results_arr.get());
                results_arr->set_proto(array_prototype_);
                result_promise->Fulfill(Value::object(ObjectPtr(results_arr)), job_queue_);
                return EvalResult::ok(Value::object(ObjectPtr(result_promise)));
            }
            auto results = std::make_shared<std::vector<Value>>(items.size(), Value::undefined());
            auto remaining = std::make_shared<int>(static_cast<int>(items.size()));
            for (size_t i = 0; i < items.size(); ++i) {
                auto p = promise_resolve(items[i]);
                auto on_fulfill = RcPtr<JSFunction>::make();
                on_fulfill->set_native_fn([this, results, remaining, i, result_promise](
                        Value, std::vector<Value> a, bool) mutable -> EvalResult {
                    (*results)[i] = a.empty() ? Value::undefined() : a[0];
                    if (--(*remaining) == 0) {
                        auto arr = RcPtr<JSObject>::make(ObjectKind::kArray);
                        gc_heap_.Register(arr.get());
                        arr->set_proto(array_prototype_);
                        for (size_t j = 0; j < results->size(); ++j) {
                            arr->elements_[static_cast<uint32_t>(j)] = (*results)[j];
                        }
                        arr->array_length_ = static_cast<uint32_t>(results->size());
                        result_promise->Fulfill(Value::object(ObjectPtr(arr)), job_queue_);
                    }
                    return EvalResult::ok(Value::undefined());
                });
                gc_heap_.Register(on_fulfill.get());
                auto on_reject = RcPtr<JSFunction>::make();
                on_reject->set_native_fn([this, result_promise](Value, std::vector<Value> a, bool) mutable -> EvalResult {
                    Value r = a.empty() ? Value::undefined() : a[0];
                    result_promise->Reject(r, job_queue_);
                    return EvalResult::ok(Value::undefined());
                });
                gc_heap_.Register(on_reject.get());
                JSPromise::PerformThen(p,
                    Value::object(ObjectPtr(on_fulfill)),
                    Value::object(ObjectPtr(on_reject)),
                    job_queue_);
            }
            return EvalResult::ok(Value::object(ObjectPtr(result_promise)));
        });
        gc_heap_.Register(all_fn.get());
        promise_ctor->set_property("all", Value::object(ObjectPtr(all_fn)));
    }

    // Promise.race(iterable)
    {
        auto race_fn = RcPtr<JSFunction>::make();
        race_fn->set_name(std::string("race"));
        race_fn->set_native_fn([this](Value, std::vector<Value> args, bool) -> EvalResult {
            std::vector<Value> items;
            if (!args.empty() && args[0].is_object()) {
                spread_into(args[0], items);
            }
            auto result_promise = RcPtr<JSPromise>::make();
            gc_heap_.Register(result_promise.get());
            for (auto& item : items) {
                auto p = promise_resolve(item);
                auto on_fulfill = RcPtr<JSFunction>::make();
                on_fulfill->set_native_fn([this, result_promise](Value, std::vector<Value> a, bool) mutable -> EvalResult {
                    Value v = a.empty() ? Value::undefined() : a[0];
                    result_promise->Fulfill(v, job_queue_);
                    return EvalResult::ok(Value::undefined());
                });
                gc_heap_.Register(on_fulfill.get());
                auto on_reject = RcPtr<JSFunction>::make();
                on_reject->set_native_fn([this, result_promise](Value, std::vector<Value> a, bool) mutable -> EvalResult {
                    Value r = a.empty() ? Value::undefined() : a[0];
                    result_promise->Reject(r, job_queue_);
                    return EvalResult::ok(Value::undefined());
                });
                gc_heap_.Register(on_reject.get());
                JSPromise::PerformThen(p,
                    Value::object(ObjectPtr(on_fulfill)),
                    Value::object(ObjectPtr(on_reject)),
                    job_queue_);
            }
            return EvalResult::ok(Value::object(ObjectPtr(result_promise)));
        });
        gc_heap_.Register(race_fn.get());
        promise_ctor->set_property("race", Value::object(ObjectPtr(race_fn)));
    }

    // Promise.allSettled(iterable)
    {
        auto all_settled_fn = RcPtr<JSFunction>::make();
        all_settled_fn->set_name(std::string("allSettled"));
        all_settled_fn->set_native_fn([this](Value, std::vector<Value> args, bool) -> EvalResult {
            std::vector<Value> items;
            if (!args.empty() && args[0].is_object()) {
                spread_into(args[0], items);
            }
            auto result_promise = RcPtr<JSPromise>::make();
            gc_heap_.Register(result_promise.get());
            if (items.empty()) {
                auto arr = RcPtr<JSObject>::make(ObjectKind::kArray);
                gc_heap_.Register(arr.get());
                arr->set_proto(array_prototype_);
                result_promise->Fulfill(Value::object(ObjectPtr(arr)), job_queue_);
                return EvalResult::ok(Value::object(ObjectPtr(result_promise)));
            }
            auto results = std::make_shared<std::vector<Value>>(items.size(), Value::undefined());
            auto remaining = std::make_shared<int>(static_cast<int>(items.size()));
            for (size_t i = 0; i < items.size(); ++i) {
                auto p = promise_resolve(items[i]);
                auto on_fulfill = RcPtr<JSFunction>::make();
                on_fulfill->set_native_fn([this, results, remaining, i, result_promise](
                        Value, std::vector<Value> a, bool) mutable -> EvalResult {
                    Value v = a.empty() ? Value::undefined() : a[0];
                    auto entry = RcPtr<JSObject>::make();
                    gc_heap_.Register(entry.get());
                    entry->set_proto(object_prototype_);
                    entry->set_property("status", Value::string("fulfilled"));
                    entry->set_property("value", v);
                    (*results)[i] = Value::object(ObjectPtr(entry));
                    if (--(*remaining) == 0) {
                        auto arr = RcPtr<JSObject>::make(ObjectKind::kArray);
                        gc_heap_.Register(arr.get());
                        arr->set_proto(array_prototype_);
                        for (size_t j = 0; j < results->size(); ++j) {
                            arr->elements_[static_cast<uint32_t>(j)] = (*results)[j];
                        }
                        arr->array_length_ = static_cast<uint32_t>(results->size());
                        result_promise->Fulfill(Value::object(ObjectPtr(arr)), job_queue_);
                    }
                    return EvalResult::ok(Value::undefined());
                });
                gc_heap_.Register(on_fulfill.get());
                auto on_reject = RcPtr<JSFunction>::make();
                on_reject->set_native_fn([this, results, remaining, i, result_promise](
                        Value, std::vector<Value> a, bool) mutable -> EvalResult {
                    Value r = a.empty() ? Value::undefined() : a[0];
                    auto entry = RcPtr<JSObject>::make();
                    gc_heap_.Register(entry.get());
                    entry->set_proto(object_prototype_);
                    entry->set_property("status", Value::string("rejected"));
                    entry->set_property("reason", r);
                    (*results)[i] = Value::object(ObjectPtr(entry));
                    if (--(*remaining) == 0) {
                        auto arr = RcPtr<JSObject>::make(ObjectKind::kArray);
                        gc_heap_.Register(arr.get());
                        arr->set_proto(array_prototype_);
                        for (size_t j = 0; j < results->size(); ++j) {
                            arr->elements_[static_cast<uint32_t>(j)] = (*results)[j];
                        }
                        arr->array_length_ = static_cast<uint32_t>(results->size());
                        result_promise->Fulfill(Value::object(ObjectPtr(arr)), job_queue_);
                    }
                    return EvalResult::ok(Value::undefined());
                });
                gc_heap_.Register(on_reject.get());
                JSPromise::PerformThen(p,
                    Value::object(ObjectPtr(on_fulfill)),
                    Value::object(ObjectPtr(on_reject)),
                    job_queue_);
            }
            return EvalResult::ok(Value::object(ObjectPtr(result_promise)));
        });
        gc_heap_.Register(all_settled_fn.get());
        promise_ctor->set_property("allSettled", Value::object(ObjectPtr(all_settled_fn)));
    }

    // Promise.any(iterable)
    {
        auto any_fn = RcPtr<JSFunction>::make();
        any_fn->set_name(std::string("any"));
        any_fn->set_native_fn([this](Value, std::vector<Value> args, bool) -> EvalResult {
            std::vector<Value> items;
            if (!args.empty() && args[0].is_object()) {
                spread_into(args[0], items);
            }
            auto result_promise = RcPtr<JSPromise>::make();
            gc_heap_.Register(result_promise.get());
            if (items.empty()) {
                auto errors_arr = RcPtr<JSObject>::make(ObjectKind::kArray);
                gc_heap_.Register(errors_arr.get());
                errors_arr->set_proto(array_prototype_);
                auto agg_err = make_error_value(NativeErrorType::kTypeError,
                    "All promises were rejected");
                if (agg_err.is_object()) {
                    static_cast<JSObject*>(agg_err.as_object_raw())->set_property(
                        "errors", Value::object(ObjectPtr(errors_arr)));
                }
                result_promise->Reject(agg_err, job_queue_);
                return EvalResult::ok(Value::object(ObjectPtr(result_promise)));
            }
            auto errors = std::make_shared<std::vector<Value>>(items.size(), Value::undefined());
            auto remaining = std::make_shared<int>(static_cast<int>(items.size()));
            for (size_t i = 0; i < items.size(); ++i) {
                auto p = promise_resolve(items[i]);
                auto on_fulfill = RcPtr<JSFunction>::make();
                on_fulfill->set_native_fn([this, result_promise](Value, std::vector<Value> a, bool) mutable -> EvalResult {
                    Value v = a.empty() ? Value::undefined() : a[0];
                    result_promise->Fulfill(v, job_queue_);
                    return EvalResult::ok(Value::undefined());
                });
                gc_heap_.Register(on_fulfill.get());
                auto on_reject = RcPtr<JSFunction>::make();
                on_reject->set_native_fn([this, errors, remaining, i, result_promise](
                        Value, std::vector<Value> a, bool) mutable -> EvalResult {
                    (*errors)[i] = a.empty() ? Value::undefined() : a[0];
                    if (--(*remaining) == 0) {
                        auto errors_arr = RcPtr<JSObject>::make(ObjectKind::kArray);
                        gc_heap_.Register(errors_arr.get());
                        errors_arr->set_proto(array_prototype_);
                        for (size_t j = 0; j < errors->size(); ++j) {
                            errors_arr->elements_[static_cast<uint32_t>(j)] = (*errors)[j];
                        }
                        errors_arr->array_length_ = static_cast<uint32_t>(errors->size());
                        auto agg_err = make_error_value(NativeErrorType::kTypeError,
                            "All promises were rejected");
                        if (agg_err.is_object()) {
                            static_cast<JSObject*>(agg_err.as_object_raw())->set_property(
                                "errors", Value::object(ObjectPtr(errors_arr)));
                        }
                        result_promise->Reject(agg_err, job_queue_);
                    }
                    return EvalResult::ok(Value::undefined());
                });
                gc_heap_.Register(on_reject.get());
                JSPromise::PerformThen(p,
                    Value::object(ObjectPtr(on_fulfill)),
                    Value::object(ObjectPtr(on_reject)),
                    job_queue_);
            }
            return EvalResult::ok(Value::object(ObjectPtr(result_promise)));
        });
        gc_heap_.Register(any_fn.get());
        promise_ctor->set_property("any", Value::object(ObjectPtr(any_fn)));
    }

    gc_heap_.Register(promise_ctor.get());
    global_env_->define_initialized("Promise");
    global_env_->set("Promise", Value::object(ObjectPtr(promise_ctor)));

    // String.prototype (kStringObject wrapper with empty string)
    string_prototype_ = RcPtr<JSObject>::make(ObjectKind::kStringObject);
    string_prototype_->set_wrapped_value(Value::string(""));
    string_prototype_->set_proto(object_prototype_);

    // indexOf(searchString, fromIndex)
    auto str_index_of_fn = RcPtr<JSFunction>::make();
    str_index_of_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        if (this_val.is_null() || this_val.is_undefined()) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "String.prototype.indexOf called on null or undefined");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        Value effective_this = string_this_value(this_val);
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
    string_prototype_->define_builtin_property("indexOf", Value::object(ObjectPtr(str_index_of_fn)));

    // lastIndexOf(searchString, fromIndex)
    auto str_last_index_of_fn = RcPtr<JSFunction>::make();
    str_last_index_of_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        if (this_val.is_null() || this_val.is_undefined()) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "String.prototype.lastIndexOf called on null or undefined");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        Value effective_this = string_this_value(this_val);
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
    string_prototype_->define_builtin_property("lastIndexOf", Value::object(ObjectPtr(str_last_index_of_fn)));

    // slice(start, end)
    auto str_slice_fn = RcPtr<JSFunction>::make();
    str_slice_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        if (this_val.is_null() || this_val.is_undefined()) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "String.prototype.slice called on null or undefined");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        Value effective_this = string_this_value(this_val);
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
    string_prototype_->define_builtin_property("slice", Value::object(ObjectPtr(str_slice_fn)));

    // substring(start, end)
    auto str_substring_fn = RcPtr<JSFunction>::make();
    str_substring_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        if (this_val.is_null() || this_val.is_undefined()) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "String.prototype.substring called on null or undefined");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        Value effective_this = string_this_value(this_val);
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
    string_prototype_->define_builtin_property("substring", Value::object(ObjectPtr(str_substring_fn)));

    // Annex B: substr(start, length) - negative start allowed
    {
        auto fn = RcPtr<JSFunction>::make();
        fn->set_name(std::string("substr"));
        fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
            if (this_val.is_null() || this_val.is_undefined()) {
                pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                    "String.prototype.substr called on null or undefined");
                return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
            }
            Value effective_this = string_this_value(this_val);
            JSString* js_str = effective_this.js_string_raw();
            int32_t str_len = utf8_cp_len(js_str);
            int32_t start = 0;
            if (!args.empty() && !args[0].is_undefined()) {
                double n = to_number_double(args[0]);
                if (!std::isnan(n)) {
                    start = static_cast<int32_t>(std::trunc(n));
                    if (start < 0) start = std::max(0, str_len + start);
                    if (start > str_len) start = str_len;
                }
            }
            int32_t length = str_len - start;
            if (args.size() >= 2 && !args[1].is_undefined()) {
                double n = to_number_double(args[1]);
                if (std::isnan(n) || n <= 0.0) return EvalResult::ok(Value::string(""));
                length = std::min(static_cast<int32_t>(std::trunc(n)), str_len - start);
            }
            if (length <= 0) return EvalResult::ok(Value::string(""));
            return EvalResult::ok(Value::string(utf8_substr(js_str->sv(), start, start + length)));
        });
        gc_heap_.Register(fn.get());
        string_prototype_->define_builtin_property("substr", Value::object(ObjectPtr(fn)));
    }

    // split(separator, limit)
    auto str_split_fn = RcPtr<JSFunction>::make();
    str_split_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        if (this_val.is_null() || this_val.is_undefined()) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "String.prototype.split called on null or undefined");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        std::string str(string_this_value(this_val).sv());
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
    string_prototype_->define_builtin_property("split", Value::object(ObjectPtr(str_split_fn)));

    // trim()
    auto str_trim_fn = RcPtr<JSFunction>::make();
    str_trim_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        (void)args;
        if (this_val.is_null() || this_val.is_undefined()) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "String.prototype.trim called on null or undefined");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        return EvalResult::ok(Value::string(utf8_trim_impl(string_this_value(this_val).sv(), true, true)));
    });
    gc_heap_.Register(str_trim_fn.get());
    string_prototype_->define_builtin_property("trim", Value::object(ObjectPtr(str_trim_fn)));

    // trimStart()
    auto str_trim_start_fn = RcPtr<JSFunction>::make();
    str_trim_start_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        (void)args;
        if (this_val.is_null() || this_val.is_undefined()) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "String.prototype.trimStart called on null or undefined");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        return EvalResult::ok(Value::string(utf8_trim_impl(string_this_value(this_val).sv(), true, false)));
    });
    gc_heap_.Register(str_trim_start_fn.get());
    string_prototype_->define_builtin_property("trimStart", Value::object(ObjectPtr(str_trim_start_fn)));

    // trimEnd()
    auto str_trim_end_fn = RcPtr<JSFunction>::make();
    str_trim_end_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        (void)args;
        if (this_val.is_null() || this_val.is_undefined()) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "String.prototype.trimEnd called on null or undefined");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        return EvalResult::ok(Value::string(utf8_trim_impl(string_this_value(this_val).sv(), false, true)));
    });
    gc_heap_.Register(str_trim_end_fn.get());
    string_prototype_->define_builtin_property("trimEnd", Value::object(ObjectPtr(str_trim_end_fn)));

    // toLowerCase() / toUpperCase()
    {
        auto fn = RcPtr<JSFunction>::make();
        fn->set_name(std::string("toLowerCase"));
        fn->set_native_fn([this](Value this_val, std::vector<Value>, bool) -> EvalResult {
            if (this_val.is_null() || this_val.is_undefined()) {
                pending_throw_ = make_error_value(NativeErrorType::kTypeError, "null or undefined");
                return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
            }
            std::string s = std::string(string_this_value(this_val).sv());
            for (auto& c : s) if (c >= 'A' && c <= 'Z') c += 32;
            return EvalResult::ok(Value::string(s));
        });
        string_prototype_->define_builtin_property("toLowerCase", Value::object(ObjectPtr(fn)));
        string_prototype_->define_builtin_property("toLocaleLowerCase", Value::object(ObjectPtr(fn)));
    }
    {
        auto fn = RcPtr<JSFunction>::make();
        fn->set_name(std::string("toUpperCase"));
        fn->set_native_fn([this](Value this_val, std::vector<Value>, bool) -> EvalResult {
            if (this_val.is_null() || this_val.is_undefined()) {
                pending_throw_ = make_error_value(NativeErrorType::kTypeError, "null or undefined");
                return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
            }
            std::string s = std::string(string_this_value(this_val).sv());
            for (auto& c : s) if (c >= 'a' && c <= 'z') c -= 32;
            return EvalResult::ok(Value::string(s));
        });
        string_prototype_->define_builtin_property("toUpperCase", Value::object(ObjectPtr(fn)));
        string_prototype_->define_builtin_property("toLocaleUpperCase", Value::object(ObjectPtr(fn)));
    }

    // valueOf()
    auto str_valueof_fn = RcPtr<JSFunction>::make();
    str_valueof_fn->set_native_fn([this](Value this_val, std::vector<Value>, bool) -> EvalResult {
        if (this_val.is_string()) return EvalResult::ok(this_val);
        if (this_val.is_object()) {
            RcObject* raw = this_val.as_object_raw();
            if (raw->object_kind() == ObjectKind::kStringObject) {
                return EvalResult::ok(static_cast<JSObject*>(raw)->wrapped_value());
            }
        }
        pending_throw_ = make_error_value(NativeErrorType::kTypeError,
            "String.prototype.valueOf requires a string or String object");
        return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
    });
    gc_heap_.Register(str_valueof_fn.get());
    string_prototype_->define_builtin_property("valueOf", Value::object(ObjectPtr(str_valueof_fn)));

    // toString()
    auto str_tostring_fn = RcPtr<JSFunction>::make();
    str_tostring_fn->set_native_fn([this](Value this_val, std::vector<Value>, bool) -> EvalResult {
        if (this_val.is_string()) return EvalResult::ok(this_val);
        if (this_val.is_object()) {
            RcObject* raw = this_val.as_object_raw();
            if (raw->object_kind() == ObjectKind::kStringObject) {
                return EvalResult::ok(static_cast<JSObject*>(raw)->wrapped_value());
            }
        }
        pending_throw_ = make_error_value(NativeErrorType::kTypeError,
            "String.prototype.toString requires a string or String object");
        return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
    });
    gc_heap_.Register(str_tostring_fn.get());
    string_prototype_->define_builtin_property("toString", Value::object(ObjectPtr(str_tostring_fn)));

    // String.prototype[Symbol.iterator]
    {
        auto string_iter_fn = RcPtr<JSFunction>::make();
        string_iter_fn->set_name(std::string("[Symbol.iterator]"));
        string_iter_fn->set_native_fn([this](Value this_val, std::vector<Value>, bool) -> EvalResult {
            if (this_val.is_null() || this_val.is_undefined()) {
                pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                    "String.prototype[Symbol.iterator] called on null or undefined");
                return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
            }
            Value str_val = string_this_value(this_val);
            auto iter_obj = RcPtr<JSObject>::make();
            gc_heap_.Register(iter_obj.get());
            iter_obj->set_property("__str__", std::move(str_val));
            iter_obj->set_property("__pos__", Value::number(0.0));

            auto next_fn = RcPtr<JSFunction>::make();
            next_fn->set_name(std::string("next"));
            next_fn->set_native_fn([this](Value this_val, std::vector<Value>, bool) -> EvalResult {
                auto make_done = [&]() -> EvalResult {
                    auto r = RcPtr<JSObject>::make();
                    gc_heap_.Register(r.get());
                    r->set_property("value", Value::undefined());
                    r->set_property("done", Value::boolean(true));
                    return EvalResult::ok(Value::object(ObjectPtr(r)));
                };
                if (!this_val.is_object()) return make_done();
                auto* iter = static_cast<JSObject*>(this_val.as_object_raw());
                Value str_val = iter->get_property("__str__");
                Value pos_val = iter->get_property("__pos__");
                if (!str_val.is_string()) return make_done();
                std::string_view sv = str_val.sv();
                size_t pos = pos_val.is_number() ? static_cast<size_t>(pos_val.as_number()) : 0u;
                if (pos >= sv.size()) return make_done();
                unsigned char c0 = static_cast<unsigned char>(sv[pos]);
                size_t cp_bytes = (c0 < 0x80) ? 1 : (c0 < 0xE0) ? 2 : (c0 < 0xF0) ? 3 : 4;
                if (pos + cp_bytes > sv.size()) cp_bytes = sv.size() - pos;
                std::string ch(sv.data() + pos, cp_bytes);
                iter->set_property("__pos__", Value::number(static_cast<double>(pos + cp_bytes)));
                auto r = RcPtr<JSObject>::make();
                gc_heap_.Register(r.get());
                r->set_property("value", Value::string(std::move(ch)));
                r->set_property("done", Value::boolean(false));
                return EvalResult::ok(Value::object(ObjectPtr(r)));
            });
            gc_heap_.Register(next_fn.get());
            iter_obj->set_property("next", Value::object(ObjectPtr(next_fn)));
            // Iterator is also iterable: [Symbol.iterator]() { return this; }
            auto self_iter_fn = RcPtr<JSFunction>::make();
            self_iter_fn->set_name(std::string("[Symbol.iterator]"));
            self_iter_fn->set_native_fn([](Value this_val, std::vector<Value>, bool) -> EvalResult {
                return EvalResult::ok(this_val);
            });
            gc_heap_.Register(self_iter_fn.get());
            iter_obj->set_property_by_symbol(symbol_table_.well_known_iterator,
                                              Value::object(ObjectPtr(self_iter_fn)));
            return EvalResult::ok(Value::object(ObjectPtr(iter_obj)));
        });
        gc_heap_.Register(string_iter_fn.get());
        string_prototype_->set_property_by_symbol(symbol_table_.well_known_iterator,
                                                   Value::object(ObjectPtr(string_iter_fn)));
    }

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
    // Number.parseFloat === global parseFloat
    number_constructor_->set_property("parseFloat", Value::object(ObjectPtr(parse_float_fn)));
    // Number.isSafeInteger
    {
        auto fn = RcPtr<JSFunction>::make(); fn->set_name(std::string("isSafeInteger"));
        fn->set_native_fn([](Value, std::vector<Value> args, bool) -> EvalResult {
            if (args.empty() || !args[0].is_number()) return EvalResult::ok(Value::boolean(false));
            double v = args[0].as_number();
            return EvalResult::ok(Value::boolean(std::isfinite(v) && v == std::trunc(v) && std::abs(v) <= 9007199254740991.0));
        });
        number_constructor_->set_property("isSafeInteger", Value::object(ObjectPtr(fn)));
    }

    // Number static value properties
    number_constructor_->set_property("MAX_VALUE", Value::number(std::numeric_limits<double>::max()));
    number_constructor_->set_property("MIN_VALUE", Value::number(std::numeric_limits<double>::denorm_min()));
    number_constructor_->set_property("POSITIVE_INFINITY", Value::number(std::numeric_limits<double>::infinity()));
    number_constructor_->set_property("NEGATIVE_INFINITY", Value::number(-std::numeric_limits<double>::infinity()));
    number_constructor_->set_property("NaN", Value::number(std::numeric_limits<double>::quiet_NaN()));
    number_constructor_->set_property("EPSILON", Value::number(std::numeric_limits<double>::epsilon()));
    number_constructor_->set_property("MAX_SAFE_INTEGER", Value::number(9007199254740991.0));
    number_constructor_->set_property("MIN_SAFE_INTEGER", Value::number(-9007199254740991.0));

    // Number.prototype
    number_prototype_ = RcPtr<JSObject>::make();
    number_prototype_->set_proto(object_prototype_);
    gc_heap_.Register(number_prototype_.get());
    number_constructor_->set_prototype_obj(RcPtr<JSObject>(number_prototype_));
    number_constructor_->set_property("prototype", Value::object(ObjectPtr(number_prototype_)));
    number_prototype_->define_builtin_property("constructor", Value::object(ObjectPtr(number_constructor_)));

    // Number.prototype.valueOf
    auto num_valueof_fn = RcPtr<JSFunction>::make();
    num_valueof_fn->set_native_fn([this](Value this_val, std::vector<Value> /*args*/, bool) -> EvalResult {
        if (this_val.is_number()) return EvalResult::ok(this_val);
        if (this_val.is_object()) {
            RcObject* raw = this_val.as_object_raw();
            if (raw->object_kind() == ObjectKind::kOrdinary) {
                // Number wrapper objects not yet implemented; fall through
            }
        }
        if (!this_val.is_number()) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "Number.prototype.valueOf requires a number");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        return EvalResult::ok(this_val);
    });
    gc_heap_.Register(num_valueof_fn.get());
    number_prototype_->define_builtin_property("valueOf", Value::object(ObjectPtr(num_valueof_fn)));

    // Number.prototype.toString([radix])
    auto num_tostring_fn = RcPtr<JSFunction>::make();
    num_tostring_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        if (!this_val.is_number()) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "Number.prototype.toString requires a number");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        double val = this_val.as_number();
        int radix = 10;
        if (!args.empty() && !args[0].is_undefined()) {
            double r = to_number_double(args[0]);
            radix = static_cast<int>(std::trunc(r));
        }
        if (radix < 2 || radix > 36) {
            pending_throw_ = make_error_value(NativeErrorType::kRangeError,
                "toString() radix must be between 2 and 36");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        if (std::isnan(val)) return EvalResult::ok(Value::string("NaN"));
        if (std::isinf(val)) return EvalResult::ok(Value::string(val > 0 ? "Infinity" : "-Infinity"));
        if (radix == 10) {
            return EvalResult::ok(Value::string(to_string_val(this_val)));
        }
        // Non-decimal: convert integer part by repeated division, fractional by repeated multiplication
        bool negative = val < 0;
        double abs_val = std::fabs(val);
        double int_part = std::trunc(abs_val);
        double frac_part = abs_val - int_part;
        static const char digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
        std::string int_str;
        if (int_part == 0.0) {
            int_str = "0";
        } else {
            while (int_part >= 1.0) {
                int rem = static_cast<int>(std::fmod(int_part, static_cast<double>(radix)));
                int_str += digits[rem];
                int_part = std::trunc(int_part / static_cast<double>(radix));
            }
            std::reverse(int_str.begin(), int_str.end());
        }
        std::string result = (negative ? "-" : "") + int_str;
        if (frac_part > 0.0) {
            result += '.';
            int max_digits = 52;
            while (frac_part > 0.0 && max_digits-- > 0) {
                frac_part *= static_cast<double>(radix);
                int digit = static_cast<int>(std::trunc(frac_part));
                result += digits[digit];
                frac_part -= std::trunc(frac_part);
            }
        }
        return EvalResult::ok(Value::string(result));
    });
    gc_heap_.Register(num_tostring_fn.get());
    number_prototype_->define_builtin_property("toString", Value::object(ObjectPtr(num_tostring_fn)));

    // Number.prototype.toFixed([digits])
    auto num_tofixed_fn = RcPtr<JSFunction>::make();
    num_tofixed_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        if (!this_val.is_number()) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "Number.prototype.toFixed requires a number");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        double val = this_val.as_number();
        int digits = 0;
        if (!args.empty() && !args[0].is_undefined()) {
            double d = to_number_double(args[0]);
            digits = static_cast<int>(std::trunc(d));
        }
        if (digits < 0 || digits > 100) {
            pending_throw_ = make_error_value(NativeErrorType::kRangeError,
                "toFixed() digits must be between 0 and 100");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        if (std::isnan(val)) return EvalResult::ok(Value::string("NaN"));
        if (std::isinf(val)) return EvalResult::ok(Value::string(val > 0 ? "Infinity" : "-Infinity"));
        // Values >= 1e21 use exponential notation (JS spec)
        if (std::fabs(val) >= 1e21) return EvalResult::ok(Value::string(to_string_val(this_val)));
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%.*f", digits, val);
        return EvalResult::ok(Value::string(buf));
    });
    gc_heap_.Register(num_tofixed_fn.get());
    number_prototype_->define_builtin_property("toFixed", Value::object(ObjectPtr(num_tofixed_fn)));

    // Number.prototype.toExponential([digits])
    auto num_toexp_fn = RcPtr<JSFunction>::make();
    num_toexp_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        if (!this_val.is_number()) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "Number.prototype.toExponential requires a number");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        double val = this_val.as_number();
        if (std::isnan(val)) return EvalResult::ok(Value::string("NaN"));
        if (std::isinf(val)) return EvalResult::ok(Value::string(val > 0 ? "Infinity" : "-Infinity"));
        int digits = -1;
        if (!args.empty() && !args[0].is_undefined()) {
            double d = to_number_double(args[0]);
            digits = static_cast<int>(std::trunc(d));
            if (digits < 0 || digits > 100) {
                pending_throw_ = make_error_value(NativeErrorType::kRangeError,
                    "toExponential() digits must be between 0 and 100");
                return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
            }
        }
        char buf[256];
        if (digits < 0) {
            // No precision specified: use shortest representation
            std::snprintf(buf, sizeof(buf), "%e", val);
        } else {
            std::snprintf(buf, sizeof(buf), "%.*e", digits, val);
        }
        // Normalize exponent: remove leading zeros from exponent (e+003 → e+3)
        std::string result(buf);
        auto e_pos = result.rfind('e');
        if (e_pos != std::string::npos) {
            char sign = result[e_pos + 1];
            std::string exp_str = result.substr(e_pos + 2);
            // Remove leading zeros
            size_t first_nonzero = exp_str.find_first_not_of('0');
            if (first_nonzero == std::string::npos) exp_str = "0";
            else exp_str = exp_str.substr(first_nonzero);
            result = result.substr(0, e_pos + 1) + sign + exp_str;
        }
        return EvalResult::ok(Value::string(result));
    });
    gc_heap_.Register(num_toexp_fn.get());
    number_prototype_->define_builtin_property("toExponential", Value::object(ObjectPtr(num_toexp_fn)));

    // Number.prototype.toPrecision([prec])
    auto num_toprec_fn = RcPtr<JSFunction>::make();
    num_toprec_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        if (!this_val.is_number()) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "Number.prototype.toPrecision requires a number");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        double val = this_val.as_number();
        if (args.empty() || args[0].is_undefined()) {
            return EvalResult::ok(Value::string(to_string_val(this_val)));
        }
        int prec = static_cast<int>(std::trunc(to_number_double(args[0])));
        if (prec < 1 || prec > 100) {
            pending_throw_ = make_error_value(NativeErrorType::kRangeError,
                "toPrecision() precision must be between 1 and 100");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        if (std::isnan(val)) return EvalResult::ok(Value::string("NaN"));
        if (std::isinf(val)) return EvalResult::ok(Value::string(val > 0 ? "Infinity" : "-Infinity"));
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%.*g", prec, val);
        // Normalize exponential notation exponent (e+003 → e+3)
        std::string result(buf);
        auto e_pos = result.rfind('e');
        if (e_pos != std::string::npos) {
            char sign = result[e_pos + 1];
            std::string exp_str = result.substr(e_pos + 2);
            size_t first_nonzero = exp_str.find_first_not_of('0');
            if (first_nonzero == std::string::npos) exp_str = "0";
            else exp_str = exp_str.substr(first_nonzero);
            result = result.substr(0, e_pos + 1) + sign + exp_str;
        }
        return EvalResult::ok(Value::string(result));
    });
    gc_heap_.Register(num_toprec_fn.get());
    number_prototype_->define_builtin_property("toPrecision", Value::object(ObjectPtr(num_toprec_fn)));

    // Number.prototype.toLocaleString — simplified: delegate to toString
    {
        auto fn = RcPtr<JSFunction>::make();
        fn->set_name(std::string("toLocaleString"));
        fn->set_native_fn([this](Value this_val, std::vector<Value>, bool) -> EvalResult {
            double val = 0.0;
            if (this_val.is_number()) {
                val = this_val.as_number();
            } else if (this_val.is_object()) {
                RcObject* raw = this_val.as_object_raw();
                if (!raw || raw->object_kind() != ObjectKind::kOrdinary) {
                    pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                        "Number.prototype.toLocaleString requires a number");
                    return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
                }
                val = static_cast<JSObject*>(raw)->wrapped_value().as_number();
            } else {
                pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                    "Number.prototype.toLocaleString requires a number");
                return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
            }
            return EvalResult::ok(Value::string(to_string_val(Value::number(val))));
        });
        gc_heap_.Register(fn.get());
        number_prototype_->define_builtin_property("toLocaleString", Value::object(ObjectPtr(fn)));
    }

    gc_heap_.Register(number_constructor_.get());
    global_env_->define_initialized("Number");
    global_env_->set("Number", Value::object(ObjectPtr(number_constructor_)));

    // ---- Boolean.prototype ----

    boolean_prototype_ = RcPtr<JSObject>::make(ObjectKind::kBooleanObject);
    boolean_prototype_->set_wrapped_value(Value::boolean(false));
    boolean_prototype_->set_proto(object_prototype_);
    gc_heap_.Register(boolean_prototype_.get());

    // Boolean.prototype.valueOf
    auto bool_valueof_fn = RcPtr<JSFunction>::make();
    bool_valueof_fn->set_native_fn([this](Value this_val, std::vector<Value> /*args*/, bool) -> EvalResult {
        if (this_val.is_bool()) return EvalResult::ok(this_val);
        if (this_val.is_object()) {
            RcObject* raw = this_val.as_object_raw();
            if (raw->object_kind() == ObjectKind::kBooleanObject) {
                return EvalResult::ok(static_cast<JSObject*>(raw)->wrapped_value());
            }
        }
        pending_throw_ = make_error_value(NativeErrorType::kTypeError,
            "Boolean.prototype.valueOf requires a boolean or Boolean object");
        return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
    });
    gc_heap_.Register(bool_valueof_fn.get());
    boolean_prototype_->define_builtin_property("valueOf", Value::object(ObjectPtr(bool_valueof_fn)));

    // Boolean.prototype.toString
    auto bool_tostring_fn = RcPtr<JSFunction>::make();
    bool_tostring_fn->set_native_fn([this](Value this_val, std::vector<Value> /*args*/, bool) -> EvalResult {
        bool b = false;
        if (this_val.is_bool()) {
            b = this_val.as_bool();
        } else if (this_val.is_object()) {
            RcObject* raw = this_val.as_object_raw();
            if (raw->object_kind() == ObjectKind::kBooleanObject) {
                b = static_cast<JSObject*>(raw)->wrapped_value().as_bool();
            } else {
                pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                    "Boolean.prototype.toString requires a boolean or Boolean object");
                return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
            }
        } else {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "Boolean.prototype.toString requires a boolean or Boolean object");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        return EvalResult::ok(Value::string(b ? "true" : "false"));
    });
    gc_heap_.Register(bool_tostring_fn.get());
    boolean_prototype_->define_builtin_property("toString", Value::object(ObjectPtr(bool_tostring_fn)));

    // ---- Boolean constructor ----

    boolean_constructor_ = RcPtr<JSFunction>::make();
    boolean_constructor_->set_name(std::string("Boolean"));
    boolean_constructor_->set_native_fn([this](Value /*this_val*/, std::vector<Value> args,
                                               bool is_new) -> EvalResult {
        bool bool_val = args.empty() ? false : to_boolean(args[0]);
        if (!is_new) {
            return EvalResult::ok(Value::boolean(bool_val));
        }
        auto obj = RcPtr<JSObject>::make(ObjectKind::kBooleanObject);
        obj->set_wrapped_value(Value::boolean(bool_val));
        obj->set_proto(boolean_prototype_);
        gc_heap_.Register(obj.get());
        return EvalResult::ok(Value::object(ObjectPtr(obj)));
    });
    boolean_constructor_->set_prototype_obj(boolean_prototype_);
    boolean_constructor_->set_property("prototype", Value::object(ObjectPtr(boolean_prototype_)));
    boolean_prototype_->define_builtin_property("constructor", Value::object(ObjectPtr(boolean_constructor_)));

    gc_heap_.Register(boolean_constructor_.get());
    global_env_->define_initialized("Boolean");
    global_env_->set("Boolean", Value::object(ObjectPtr(boolean_constructor_)));

    // ---- String constructor ----

    string_constructor_ = RcPtr<JSFunction>::make();
    string_constructor_->set_name(std::string("String"));
    string_constructor_->set_native_fn([this](Value /*this_val*/, std::vector<Value> args,
                                              bool is_new) -> EvalResult {
        if (!is_new) {
            if (!args.empty() && args[0].is_symbol()) {
                return EvalResult::ok(Value::string(symbol_to_string(args[0].as_symbol_id(), symbol_table_)));
            }
            std::string s = args.empty() ? std::string("") : to_string_val(args[0]);
            return EvalResult::ok(Value::string(s));
        }
        // new String(...): Symbol throws TypeError
        if (!args.empty() && args[0].is_symbol()) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "Cannot convert a Symbol value to a string");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        std::string str = args.empty() ? std::string("") : to_string_val(args[0]);
        auto obj = RcPtr<JSObject>::make(ObjectKind::kStringObject);
        obj->set_wrapped_value(Value::string(str));
        obj->set_proto(string_prototype_);
        gc_heap_.Register(obj.get());
        return EvalResult::ok(Value::object(ObjectPtr(obj)));
    });
    string_constructor_->set_prototype_obj(string_prototype_);
    string_constructor_->set_property("prototype", Value::object(ObjectPtr(string_prototype_)));
    string_prototype_->define_builtin_property("constructor", Value::object(ObjectPtr(string_constructor_)));

    // String.fromCharCode
    auto str_from_char_code_fn = RcPtr<JSFunction>::make();
    str_from_char_code_fn->set_native_fn([](Value, std::vector<Value> args, bool) -> EvalResult {
        std::string result;
        for (auto& arg : args) {
            double n = to_number_double(arg);
            double trunc_n = std::isfinite(n) ? std::trunc(n) : 0.0;
            double mod = std::fmod(trunc_n, 65536.0);
            if (mod < 0.0) mod += 65536.0;
            uint16_t code = static_cast<uint16_t>(mod);
            if (code < 0x80) {
                result += static_cast<char>(code);
            } else if (code < 0x800) {
                result += static_cast<char>(0xC0 | (code >> 6));
                result += static_cast<char>(0x80 | (code & 0x3F));
            } else {
                result += static_cast<char>(0xE0 | (code >> 12));
                result += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                result += static_cast<char>(0x80 | (code & 0x3F));
            }
        }
        return EvalResult::ok(Value::string(result));
    });
    gc_heap_.Register(str_from_char_code_fn.get());
    string_constructor_->set_property("fromCharCode", Value::object(ObjectPtr(str_from_char_code_fn)));

    // String.fromCodePoint(...codePoints) — encodes each code point as UTF-8
    {
        auto fn = RcPtr<JSFunction>::make();
        fn->set_name(std::string("fromCodePoint"));
        fn->set_native_fn([this](Value, std::vector<Value> args, bool) -> EvalResult {
            std::string result;
            for (auto& arg : args) {
                double n = to_number_double(arg);
                double trunc_n = std::trunc(n);
                if (n != trunc_n || trunc_n < 0.0 || trunc_n > 0x10FFFF) {
                    pending_throw_ = make_error_value(NativeErrorType::kRangeError,
                        "Invalid code point");
                    return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
                }
                uint32_t cp = static_cast<uint32_t>(trunc_n);
                if (cp < 0x80) {
                    result += static_cast<char>(cp);
                } else if (cp < 0x800) {
                    result += static_cast<char>(0xC0 | (cp >> 6));
                    result += static_cast<char>(0x80 | (cp & 0x3F));
                } else if (cp < 0x10000) {
                    result += static_cast<char>(0xE0 | (cp >> 12));
                    result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                    result += static_cast<char>(0x80 | (cp & 0x3F));
                } else {
                    // Encode as UTF-8 4-byte sequence (SMP)
                    result += static_cast<char>(0xF0 | (cp >> 18));
                    result += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                    result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                    result += static_cast<char>(0x80 | (cp & 0x3F));
                }
            }
            return EvalResult::ok(Value::string(result));
        });
        gc_heap_.Register(fn.get());
        string_constructor_->set_property("fromCodePoint", Value::object(ObjectPtr(fn)));
    }

    gc_heap_.Register(string_prototype_.get());
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

    // ---- RegExp prototype ----

    regexp_prototype_ = RcPtr<JSObject>::make();
    regexp_prototype_->set_proto(object_prototype_);
    gc_heap_.Register(regexp_prototype_.get());

    // RegExp.prototype.exec
    auto regexp_exec_fn = RcPtr<JSFunction>::make();
    regexp_exec_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        if (!this_val.is_object() || !this_val.as_object_raw() ||
            this_val.as_object_raw()->object_kind() != ObjectKind::kRegExp) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "RegExp.prototype.exec called on non-RegExp");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        auto* rx = static_cast<JSRegExp*>(this_val.as_object_raw());
        std::string input = args.empty() ? "undefined" : to_string_val(args[0]);
        return regexp_exec(rx, input);
    });
    regexp_prototype_->define_builtin_property("exec", Value::object(ObjectPtr(regexp_exec_fn)));

    // RegExp.prototype.test
    auto regexp_test_fn = RcPtr<JSFunction>::make();
    regexp_test_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        if (!this_val.is_object() || !this_val.as_object_raw() ||
            this_val.as_object_raw()->object_kind() != ObjectKind::kRegExp) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "RegExp.prototype.test called on non-RegExp");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        auto* rx = static_cast<JSRegExp*>(this_val.as_object_raw());
        std::string input = args.empty() ? "undefined" : to_string_val(args[0]);
        auto res = regexp_exec(rx, input);
        if (!res.is_ok()) return res;
        return EvalResult::ok(Value::boolean(!res.value().is_null()));
    });
    regexp_prototype_->define_builtin_property("test", Value::object(ObjectPtr(regexp_test_fn)));

    // RegExp.prototype.toString
    auto regexp_tostring_fn = RcPtr<JSFunction>::make();
    regexp_tostring_fn->set_native_fn([](Value this_val, std::vector<Value> /*args*/, bool) -> EvalResult {
        if (!this_val.is_object() || !this_val.as_object_raw() ||
            this_val.as_object_raw()->object_kind() != ObjectKind::kRegExp) {
            return EvalResult::ok(Value::string("/(?:)/"));
        }
        auto* rx = static_cast<JSRegExp*>(this_val.as_object_raw());
        std::string src = rx->pattern_.empty() ? "(?:)" : rx->pattern_;
        // EscapeRegExpPattern: replace unescaped '/' with '\/'
        std::string escaped;
        escaped.reserve(src.size());
        for (size_t i = 0; i < src.size(); ++i) {
            if (src[i] == '\\' && i + 1 < src.size()) {
                escaped += src[i];
                escaped += src[++i];
            } else if (src[i] == '/') {
                escaped += "\\/";
            } else {
                escaped += src[i];
            }
        }
        return EvalResult::ok(Value::string("/" + escaped + "/" + rx->flags_str_));
    });
    regexp_prototype_->define_builtin_property("toString", Value::object(ObjectPtr(regexp_tostring_fn)));

    // ---- RegExp constructor ----

    regexp_constructor_ = RcPtr<JSFunction>::make();
    regexp_constructor_->set_name(std::string("RegExp"));
    regexp_constructor_->set_native_fn([this](Value /*this_val*/, std::vector<Value> args,
                                              bool is_new) -> EvalResult {
        // RegExp(rx) with no flags → return rx itself (only for non-new call)
        if (!is_new && args.size() >= 1 && args[0].is_object() && args[0].as_object_raw() &&
            args[0].as_object_raw()->object_kind() == ObjectKind::kRegExp &&
            (args.size() < 2 || args[1].is_undefined())) {
            return EvalResult::ok(args[0]);
        }
        std::string pattern;
        std::string flags;
        if (!args.empty() && !args[0].is_undefined()) {
            if (args[0].is_object() && args[0].as_object_raw() &&
                args[0].as_object_raw()->object_kind() == ObjectKind::kRegExp) {
                auto* src_rx = static_cast<JSRegExp*>(args[0].as_object_raw());
                pattern = src_rx->pattern_;
                flags = src_rx->flags_str_;
            } else {
                pattern = to_string_val(args[0]);
            }
        }
        if (args.size() >= 2 && !args[1].is_undefined()) {
            flags = to_string_val(args[1]);
        }
        return make_regexp(pattern, flags);
    });
    regexp_constructor_->set_prototype_obj(RcPtr<JSObject>(regexp_prototype_));
    regexp_constructor_->set_property("prototype", Value::object(ObjectPtr(regexp_prototype_)));
    gc_heap_.Register(regexp_constructor_.get());
    global_env_->define_initialized("RegExp");
    global_env_->set("RegExp", Value::object(ObjectPtr(regexp_constructor_)));

    // ---- String.prototype.match ----

    auto string_match_fn = RcPtr<JSFunction>::make();
    string_match_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        if (this_val.is_undefined() || this_val.is_null()) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "String.prototype.match called on null or undefined");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        std::string str(string_this_value(this_val).sv());
        if (args.empty() || args[0].is_undefined()) {
            // match(undefined) → match(/(?:)/)
            auto rx_res = make_regexp("", "");
            if (!rx_res.is_ok()) return rx_res;
            auto* rx = static_cast<JSRegExp*>(rx_res.value().as_object_raw());
            return regexp_exec(rx, str);
        }
        if (!args[0].is_object() || !args[0].as_object_raw() ||
            args[0].as_object_raw()->object_kind() != ObjectKind::kRegExp) {
            // Coerce to RegExp
            std::string pat = to_string_val(args[0]);
            auto rx_res = make_regexp(pat, "");
            if (!rx_res.is_ok()) return rx_res;
            auto* rx = static_cast<JSRegExp*>(rx_res.value().as_object_raw());
            return regexp_exec(rx, str);
        }
        auto* rx = static_cast<JSRegExp*>(args[0].as_object_raw());
        if (!rx->global_) {
            return regexp_exec(rx, str);
        }
        // Global match: collect all matches
        rx->last_index_ = 0;
        auto result_arr = RcPtr<JSObject>::make(ObjectKind::kArray);
        gc_heap_.Register(result_arr.get());
        result_arr->set_proto(array_prototype_);
        while (true) {
            if (rx->last_index_ > static_cast<uint32_t>(str.size())) break;
            auto exec_res = regexp_exec(rx, str);
            if (!exec_res.is_ok()) return exec_res;
            if (exec_res.value().is_null()) break;
            // Extract match[0]
            auto* match_arr = static_cast<JSObject*>(exec_res.value().as_object_raw());
            Value match0 = match_arr->get_property("0");
            result_arr->elements_[result_arr->array_length_] = match0;
            result_arr->array_length_++;
            // Advance lastIndex by 1 on empty match to prevent infinite loop.
            if (match0.is_string() && match0.sv().empty()) rx->last_index_++;
        }
        if (result_arr->array_length_ == 0) return EvalResult::ok(Value::null());
        return EvalResult::ok(Value::object(ObjectPtr(result_arr)));
    });
    if (string_prototype_) {
        string_prototype_->define_builtin_property("match", Value::object(ObjectPtr(string_match_fn)));
    }

    // ---- String.prototype.search ----

    auto string_search_fn = RcPtr<JSFunction>::make();
    string_search_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        if (this_val.is_undefined() || this_val.is_null()) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "String.prototype.search called on null or undefined");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        std::string str(string_this_value(this_val).sv());
        JSRegExp* rx = nullptr;
        RcPtr<JSObject> rx_holder;
        if (args.empty() || args[0].is_undefined()) {
            auto rx_res = make_regexp("", "");
            if (!rx_res.is_ok()) return rx_res;
            rx_holder = RcPtr<JSObject>(static_cast<JSObject*>(rx_res.value().as_object_raw()));
            rx = static_cast<JSRegExp*>(rx_holder.get());
        } else if (args[0].is_object() && args[0].as_object_raw() &&
                   args[0].as_object_raw()->object_kind() == ObjectKind::kRegExp) {
            rx = static_cast<JSRegExp*>(args[0].as_object_raw());
        } else {
            std::string pat = to_string_val(args[0]);
            auto rx_res = make_regexp(pat, "");
            if (!rx_res.is_ok()) return rx_res;
            rx_holder = RcPtr<JSObject>(static_cast<JSObject*>(rx_res.value().as_object_raw()));
            rx = static_cast<JSRegExp*>(rx_holder.get());
        }
        uint32_t saved_last_index = rx->last_index_;
        rx->last_index_ = 0;
        auto exec_res = regexp_exec(rx, str);
        rx->last_index_ = saved_last_index;
        if (!exec_res.is_ok()) return exec_res;
        if (exec_res.value().is_null()) return EvalResult::ok(Value::number(-1.0));
        auto* match_arr = static_cast<JSObject*>(exec_res.value().as_object_raw());
        Value idx_val = match_arr->get_property("index");
        return EvalResult::ok(idx_val.is_number() ? idx_val : Value::number(-1.0));
    });
    gc_heap_.Register(string_search_fn.get());
    if (string_prototype_) {
        string_prototype_->define_builtin_property("search", Value::object(ObjectPtr(string_search_fn)));
    }

    // ---- String.prototype.replace ----

    auto string_replace_fn = RcPtr<JSFunction>::make();
    string_replace_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        if (this_val.is_undefined() || this_val.is_null()) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "String.prototype.replace called on null or undefined");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        std::string str(string_this_value(this_val).sv());
        Value search_val = args.size() >= 1 ? args[0] : Value::undefined();
        Value replace_val = args.size() >= 2 ? args[1] : Value::undefined();

        bool is_regexp = search_val.is_object() && search_val.as_object_raw() &&
                         search_val.as_object_raw()->object_kind() == ObjectKind::kRegExp;

        auto do_replace_str = [&](size_t match_start, size_t match_len,
                                  const std::string& matched, const std::string& repl_str) -> std::string {
            std::string result;
            result.reserve(str.size());
            result += str.substr(0, match_start);
            // Process replacement pattern
            for (size_t i = 0; i < repl_str.size(); ++i) {
                if (repl_str[i] == '$' && i + 1 < repl_str.size()) {
                    char next = repl_str[i + 1];
                    if (next == '&') { result += matched; i++; }
                    else if (next == '`') { result += str.substr(0, match_start); i++; }
                    else if (next == '\'') { result += str.substr(match_start + match_len); i++; }
                    else { result += repl_str[i]; }
                } else {
                    result += repl_str[i];
                }
            }
            result += str.substr(match_start + match_len);
            return result;
        };

        if (is_regexp) {
            auto* rx = static_cast<JSRegExp*>(search_val.as_object_raw());
            bool is_global = rx->global_;
            rx->last_index_ = 0;

            if (!is_global) {
                auto exec_res = regexp_exec(rx, str);
                if (!exec_res.is_ok()) return exec_res;
                if (exec_res.value().is_null()) return EvalResult::ok(Value::string(str));
                auto* match_arr = static_cast<JSObject*>(exec_res.value().as_object_raw());
                std::string matched = match_arr->elements_.count(0) ? match_arr->elements_[0].as_string() : "";
                Value idx_val = match_arr->get_property("index");
                size_t match_start = idx_val.is_number() ? static_cast<size_t>(idx_val.as_number()) : 0;

                if (replace_val.is_object() && replace_val.as_object_raw() &&
                    replace_val.as_object_raw()->object_kind() == ObjectKind::kFunction) {
                    std::vector<Value> call_args = {Value::string(matched),
                                                    Value::number(static_cast<double>(match_start)),
                                                    Value::string(str)};
                    auto r = call_function_val(replace_val, Value::undefined(), std::span<Value>(call_args));
                    if (!r.is_ok()) return r;
                    std::string repl = to_string_val(r.value());
                    std::string result = str.substr(0, match_start) + repl +
                                        str.substr(match_start + matched.size());
                    return EvalResult::ok(Value::string(result));
                }
                std::string repl_str = to_string_val(replace_val);
                return EvalResult::ok(Value::string(do_replace_str(match_start, matched.size(), matched, repl_str)));
            }

            // Global regexp replace
            const std::string orig_str = str;
            std::string result;
            size_t last_end = 0;
            rx->last_index_ = 0;
            while (true) {
                if (rx->last_index_ > static_cast<uint32_t>(orig_str.size())) break;
                auto exec_res = regexp_exec(rx, orig_str);
                if (!exec_res.is_ok()) return exec_res;
                if (exec_res.value().is_null()) break;
                auto* match_arr = static_cast<JSObject*>(exec_res.value().as_object_raw());
                std::string matched = match_arr->elements_.count(0) ? match_arr->elements_[0].as_string() : "";
                Value idx_val = match_arr->get_property("index");
                size_t match_start = idx_val.is_number() ? static_cast<size_t>(idx_val.as_number()) : 0;

                result += orig_str.substr(last_end, match_start - last_end);
                if (replace_val.is_object() && replace_val.as_object_raw() &&
                    replace_val.as_object_raw()->object_kind() == ObjectKind::kFunction) {
                    std::vector<Value> call_args = {Value::string(matched),
                                                    Value::number(static_cast<double>(match_start)),
                                                    Value::string(orig_str)};
                    auto r = call_function_val(replace_val, Value::undefined(), std::span<Value>(call_args));
                    if (!r.is_ok()) return r;
                    result += to_string_val(r.value());
                } else {
                    std::string repl_str = to_string_val(replace_val);
                    for (size_t i = 0; i < repl_str.size(); ++i) {
                        if (repl_str[i] == '$' && i + 1 < repl_str.size()) {
                            char next = repl_str[i + 1];
                            if (next == '&') { result += matched; i++; }
                            else if (next == '`') { result += orig_str.substr(0, match_start); i++; }
                            else if (next == '\'') { result += orig_str.substr(match_start + matched.size()); i++; }
                            else { result += repl_str[i]; }
                        } else {
                            result += repl_str[i];
                        }
                    }
                }
                last_end = match_start + matched.size();
                if (matched.empty()) {
                    if (last_end < orig_str.size()) result += orig_str[last_end++];
                    else break;
                }
            }
            result += orig_str.substr(last_end);
            return EvalResult::ok(Value::string(result));
        }

        // String search: replace first occurrence
        std::string search_str = to_string_val(search_val);
        size_t pos = str.find(search_str);
        if (pos == std::string::npos) return EvalResult::ok(Value::string(str));

        if (replace_val.is_object() && replace_val.as_object_raw() &&
            replace_val.as_object_raw()->object_kind() == ObjectKind::kFunction) {
            std::vector<Value> call_args = {Value::string(search_str),
                                            Value::number(static_cast<double>(pos)),
                                            Value::string(str)};
            auto r = call_function_val(replace_val, Value::undefined(), std::span<Value>(call_args));
            if (!r.is_ok()) return r;
            std::string repl = to_string_val(r.value());
            std::string result = str.substr(0, pos) + repl + str.substr(pos + search_str.size());
            return EvalResult::ok(Value::string(result));
        }
        std::string repl_str = to_string_val(replace_val);
        return EvalResult::ok(Value::string(do_replace_str(pos, search_str.size(), search_str, repl_str)));
    });
    gc_heap_.Register(string_replace_fn.get());
    if (string_prototype_) {
        string_prototype_->define_builtin_property("replace", Value::object(ObjectPtr(string_replace_fn)));
    }

    // ---- String.prototype.replaceAll ----

    auto string_replace_all_fn = RcPtr<JSFunction>::make();
    string_replace_all_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        if (this_val.is_undefined() || this_val.is_null()) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "String.prototype.replaceAll called on null or undefined");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        std::string str(string_this_value(this_val).sv());
        Value search_val = args.size() >= 1 ? args[0] : Value::undefined();
        Value replace_val = args.size() >= 2 ? args[1] : Value::undefined();

        if (search_val.is_object() && search_val.as_object_raw() &&
            search_val.as_object_raw()->object_kind() == ObjectKind::kRegExp) {
            auto* rx = static_cast<JSRegExp*>(search_val.as_object_raw());
            if (!rx->global_) {
                pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                    "String.prototype.replaceAll requires global flag for RegExp");
                return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
            }
        }

        std::string search_str = to_string_val(search_val);
        bool is_fn = replace_val.is_object() && replace_val.as_object_raw() &&
                     replace_val.as_object_raw()->object_kind() == ObjectKind::kFunction;
        std::string repl_str = is_fn ? "" : to_string_val(replace_val);

        std::string result;
        size_t pos = 0;
        if (search_str.empty()) {
            // Empty search: insert replacement between every character
            for (size_t i = 0; i <= str.size(); ++i) {
                if (is_fn) {
                    std::vector<Value> call_args = {Value::string(""),
                                                    Value::number(static_cast<double>(i)),
                                                    Value::string(str)};
                    auto r = call_function_val(replace_val, Value::undefined(), std::span<Value>(call_args));
                    if (!r.is_ok()) return r;
                    result += to_string_val(r.value());
                } else {
                    result += repl_str;
                }
                if (i < str.size()) result += str[i];
            }
        } else {
            while (true) {
                size_t found = str.find(search_str, pos);
                if (found == std::string::npos) {
                    result += str.substr(pos);
                    break;
                }
                result += str.substr(pos, found - pos);
                if (is_fn) {
                    std::vector<Value> call_args = {Value::string(search_str),
                                                    Value::number(static_cast<double>(found)),
                                                    Value::string(str)};
                    auto r = call_function_val(replace_val, Value::undefined(), std::span<Value>(call_args));
                    if (!r.is_ok()) return r;
                    result += to_string_val(r.value());
                } else {
                    result += repl_str;
                }
                pos = found + search_str.size();
            }
        }
        return EvalResult::ok(Value::string(result));
    });
    gc_heap_.Register(string_replace_all_fn.get());
    if (string_prototype_) {
        string_prototype_->define_builtin_property("replaceAll", Value::object(ObjectPtr(string_replace_all_fn)));
    }

    // ---- String.prototype.at ----

    auto string_at_fn = RcPtr<JSFunction>::make();
    string_at_fn->set_native_fn([](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        if (this_val.is_undefined() || this_val.is_null()) {
            return EvalResult::ok(Value::undefined());
        }
        Value str_val = string_this_value(this_val);
        auto* js_str = str_val.js_string_raw();
        int32_t len = js_str ? utf8_cp_len(js_str) : static_cast<int32_t>(str_val.sv().size());
        double idx_d = args.empty() ? 0.0 : to_number_double(args[0]);
        if (std::isnan(idx_d)) idx_d = 0.0;
        int32_t idx = static_cast<int32_t>(std::trunc(idx_d));
        if (idx < 0) idx = len + idx;
        if (idx < 0 || idx >= len) return EvalResult::ok(Value::undefined());
        return EvalResult::ok(Value::string(utf8_substr(str_val.sv(), idx, idx + 1)));
    });
    gc_heap_.Register(string_at_fn.get());
    if (string_prototype_) {
        string_prototype_->define_builtin_property("at", Value::object(ObjectPtr(string_at_fn)));
    }

    // ---- String.prototype.padStart ----

    auto string_pad_start_fn = RcPtr<JSFunction>::make();
    string_pad_start_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        if (this_val.is_undefined() || this_val.is_null()) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "String.prototype.padStart called on null or undefined");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        std::string str(string_this_value(this_val).sv());
        int32_t target_len = args.empty() ? 0 : static_cast<int32_t>(to_number_double(args[0]));
        std::string pad_str = args.size() >= 2 && !args[1].is_undefined() ? to_string_val(args[1]) : " ";
        if (pad_str.empty()) return EvalResult::ok(Value::string(str));
        int32_t str_len = static_cast<int32_t>(str.size());
        if (target_len <= str_len) return EvalResult::ok(Value::string(str));
        int32_t pad_needed = target_len - str_len;
        std::string padding;
        padding.reserve(static_cast<size_t>(pad_needed));
        while (static_cast<int32_t>(padding.size()) < pad_needed) {
            padding += pad_str;
        }
        return EvalResult::ok(Value::string(padding.substr(0, static_cast<size_t>(pad_needed)) + str));
    });
    gc_heap_.Register(string_pad_start_fn.get());
    if (string_prototype_) {
        string_prototype_->define_builtin_property("padStart", Value::object(ObjectPtr(string_pad_start_fn)));
    }

    // ---- String.prototype.padEnd ----

    auto string_pad_end_fn = RcPtr<JSFunction>::make();
    string_pad_end_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        if (this_val.is_undefined() || this_val.is_null()) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "String.prototype.padEnd called on null or undefined");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        std::string str(string_this_value(this_val).sv());
        int32_t target_len = args.empty() ? 0 : static_cast<int32_t>(to_number_double(args[0]));
        std::string pad_str = args.size() >= 2 && !args[1].is_undefined() ? to_string_val(args[1]) : " ";
        if (pad_str.empty()) return EvalResult::ok(Value::string(str));
        int32_t str_len = static_cast<int32_t>(str.size());
        if (target_len <= str_len) return EvalResult::ok(Value::string(str));
        std::string result = str;
        result.reserve(static_cast<size_t>(target_len));
        while (static_cast<int32_t>(result.size()) < target_len) {
            result += pad_str;
        }
        return EvalResult::ok(Value::string(result.substr(0, static_cast<size_t>(target_len))));
    });
    gc_heap_.Register(string_pad_end_fn.get());
    if (string_prototype_) {
        string_prototype_->define_builtin_property("padEnd", Value::object(ObjectPtr(string_pad_end_fn)));
    }

    // ---- String.prototype.repeat ----

    auto string_repeat_fn = RcPtr<JSFunction>::make();
    string_repeat_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        if (this_val.is_undefined() || this_val.is_null()) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "String.prototype.repeat called on null or undefined");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        std::string str(string_this_value(this_val).sv());
        double count_d = args.empty() ? 0.0 : to_number_double(args[0]);
        if (std::isnan(count_d)) count_d = 0.0;
        if (count_d < 0 || std::isinf(count_d)) {
            pending_throw_ = make_error_value(NativeErrorType::kRangeError,
                "Invalid count value");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        int32_t count = static_cast<int32_t>(std::trunc(count_d));
        if (count == 0 || str.empty()) return EvalResult::ok(Value::string(""));
        std::string result;
        result.reserve(str.size() * static_cast<size_t>(count));
        for (int32_t i = 0; i < count; ++i) result += str;
        return EvalResult::ok(Value::string(result));
    });
    gc_heap_.Register(string_repeat_fn.get());
    if (string_prototype_) {
        string_prototype_->define_builtin_property("repeat", Value::object(ObjectPtr(string_repeat_fn)));
    }

    // ---- String.prototype.startsWith ----

    auto string_starts_with_fn = RcPtr<JSFunction>::make();
    string_starts_with_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        if (this_val.is_undefined() || this_val.is_null()) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "String.prototype.startsWith called on null or undefined");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        if (!args.empty() && args[0].is_object() && args[0].as_object_raw() &&
            args[0].as_object_raw()->object_kind() == ObjectKind::kRegExp) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "First argument to startsWith must not be a RegExp");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        std::string_view str = string_this_value(this_val).sv();
        std::string search = args.empty() ? "undefined" : to_string_val(args[0]);
        int32_t pos = 0;
        if (args.size() >= 2 && !args[1].is_undefined()) {
            double p = to_number_double(args[1]);
            pos = std::isnan(p) ? 0 : static_cast<int32_t>(std::max(0.0, std::trunc(p)));
        }
        if (pos < 0) pos = 0;
        if (static_cast<size_t>(pos) > str.size()) return EvalResult::ok(Value::boolean(false));
        auto sub = str.substr(static_cast<size_t>(pos));
        bool result = sub.size() >= search.size() && sub.substr(0, search.size()) == search;
        return EvalResult::ok(Value::boolean(result));
    });
    gc_heap_.Register(string_starts_with_fn.get());
    if (string_prototype_) {
        string_prototype_->define_builtin_property("startsWith", Value::object(ObjectPtr(string_starts_with_fn)));
    }

    // ---- String.prototype.endsWith ----

    auto string_ends_with_fn = RcPtr<JSFunction>::make();
    string_ends_with_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        if (this_val.is_undefined() || this_val.is_null()) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "String.prototype.endsWith called on null or undefined");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        if (!args.empty() && args[0].is_object() && args[0].as_object_raw() &&
            args[0].as_object_raw()->object_kind() == ObjectKind::kRegExp) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "First argument to endsWith must not be a RegExp");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        std::string_view str = string_this_value(this_val).sv();
        std::string search = args.empty() ? "undefined" : to_string_val(args[0]);
        int32_t end_pos = static_cast<int32_t>(str.size());
        if (args.size() >= 2 && !args[1].is_undefined()) {
            double p = to_number_double(args[1]);
            if (!std::isnan(p)) end_pos = static_cast<int32_t>(std::min(std::max(0.0, std::trunc(p)),
                                                                          static_cast<double>(str.size())));
        }
        if (end_pos < 0) end_pos = 0;
        std::string_view sub = str.substr(0, static_cast<size_t>(end_pos));
        if (search.size() > sub.size()) return EvalResult::ok(Value::boolean(false));
        bool result = sub.substr(sub.size() - search.size()) == search;
        return EvalResult::ok(Value::boolean(result));
    });
    gc_heap_.Register(string_ends_with_fn.get());
    if (string_prototype_) {
        string_prototype_->define_builtin_property("endsWith", Value::object(ObjectPtr(string_ends_with_fn)));
    }

    // ---- String.prototype.includes ----

    auto string_includes_fn = RcPtr<JSFunction>::make();
    string_includes_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        if (this_val.is_undefined() || this_val.is_null()) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "String.prototype.includes called on null or undefined");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        if (!args.empty() && args[0].is_object() && args[0].as_object_raw() &&
            args[0].as_object_raw()->object_kind() == ObjectKind::kRegExp) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "First argument to includes must not be a RegExp");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        std::string_view str = string_this_value(this_val).sv();
        std::string search = args.empty() ? "undefined" : to_string_val(args[0]);
        int32_t pos = 0;
        if (args.size() >= 2 && !args[1].is_undefined()) {
            double p = to_number_double(args[1]);
            pos = std::isnan(p) ? 0 : static_cast<int32_t>(std::max(0.0, std::trunc(p)));
        }
        if (static_cast<size_t>(pos) > str.size()) return EvalResult::ok(Value::boolean(false));
        bool result = str.substr(static_cast<size_t>(pos)).find(search) != std::string_view::npos;
        return EvalResult::ok(Value::boolean(result));
    });
    gc_heap_.Register(string_includes_fn.get());
    if (string_prototype_) {
        string_prototype_->define_builtin_property("includes", Value::object(ObjectPtr(string_includes_fn)));
    }

    // ---- String.prototype.matchAll ----

    auto string_match_all_fn = RcPtr<JSFunction>::make();
    string_match_all_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        if (this_val.is_undefined() || this_val.is_null()) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "String.prototype.matchAll called on null or undefined");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        std::string str(string_this_value(this_val).sv());
        Value regexp_val = args.empty() ? Value::undefined() : args[0];

        if (regexp_val.is_object() && regexp_val.as_object_raw() &&
            regexp_val.as_object_raw()->object_kind() == ObjectKind::kRegExp) {
            auto* rx = static_cast<JSRegExp*>(regexp_val.as_object_raw());
            if (!rx->global_ && !rx->sticky_) {
                pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                    "String.prototype.matchAll requires global or sticky flag");
                return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
            }
        } else {
            // Coerce to RegExp with global flag
            std::string pat = regexp_val.is_undefined() ? "" : to_string_val(regexp_val);
            auto rx_res = make_regexp(pat, "g");
            if (!rx_res.is_ok()) return rx_res;
            regexp_val = rx_res.value();
        }

        // Build a plain object iterator with .next() method
        auto iter_obj = RcPtr<JSObject>::make();
        gc_heap_.Register(iter_obj.get());
        if (object_prototype_) iter_obj->set_proto(object_prototype_);

        // Store string and regexp in iterator object
        iter_obj->set_property("__str__", Value::string(str));
        iter_obj->set_property("__rx__", regexp_val);

        auto next_fn = RcPtr<JSFunction>::make();
        next_fn->set_native_fn([this](Value iter_this, std::vector<Value> /*args*/, bool) -> EvalResult {
            if (!iter_this.is_object()) {
                return EvalResult::ok(Value::undefined());
            }
            auto* iter = static_cast<JSObject*>(iter_this.as_object_raw());
            Value str_val = iter->get_property("__str__");
            Value rx_val = iter->get_property("__rx__");
            if (!rx_val.is_object() || !rx_val.as_object_raw() ||
                rx_val.as_object_raw()->object_kind() != ObjectKind::kRegExp) {
                // Done
                auto done_obj = RcPtr<JSObject>::make();
                gc_heap_.Register(done_obj.get());
                done_obj->set_property("value", Value::undefined());
                done_obj->set_property("done", Value::boolean(true));
                return EvalResult::ok(Value::object(ObjectPtr(done_obj)));
            }
            std::string str(str_val.sv());
            auto* rx = static_cast<JSRegExp*>(rx_val.as_object_raw());
            if (rx->last_index_ > static_cast<uint32_t>(str.size())) {
                iter->set_property("__rx__", Value::undefined());
                auto done_obj = RcPtr<JSObject>::make();
                gc_heap_.Register(done_obj.get());
                done_obj->set_property("value", Value::undefined());
                done_obj->set_property("done", Value::boolean(true));
                return EvalResult::ok(Value::object(ObjectPtr(done_obj)));
            }
            auto exec_res = regexp_exec(rx, str);
            if (!exec_res.is_ok()) return exec_res;
            if (exec_res.value().is_null()) {
                iter->set_property("__rx__", Value::undefined());
                auto done_obj = RcPtr<JSObject>::make();
                gc_heap_.Register(done_obj.get());
                done_obj->set_property("value", Value::undefined());
                done_obj->set_property("done", Value::boolean(true));
                return EvalResult::ok(Value::object(ObjectPtr(done_obj)));
            }
            // Prevent infinite loop on empty match
            auto* match_arr = static_cast<JSObject*>(exec_res.value().as_object_raw());
            Value match0 = match_arr->elements_.count(0) ? match_arr->elements_[0] : Value::string("");
            if (match0.is_string() && match0.sv().empty()) rx->last_index_++;

            auto result_obj = RcPtr<JSObject>::make();
            gc_heap_.Register(result_obj.get());
            result_obj->set_property("value", exec_res.value());
            result_obj->set_property("done", Value::boolean(false));
            return EvalResult::ok(Value::object(ObjectPtr(result_obj)));
        });
        gc_heap_.Register(next_fn.get());
        iter_obj->set_property("next", Value::object(ObjectPtr(next_fn)));

        return EvalResult::ok(Value::object(ObjectPtr(iter_obj)));
    });
    gc_heap_.Register(string_match_all_fn.get());
    if (string_prototype_) {
        string_prototype_->define_builtin_property("matchAll", Value::object(ObjectPtr(string_match_all_fn)));
    }

    // ---- String.prototype.charCodeAt ----
    {
        auto fn = RcPtr<JSFunction>::make();
        fn->set_name(std::string("charCodeAt"));
        fn->set_native_fn([](Value this_val, std::vector<Value> args, bool) -> EvalResult {
            Value sv = string_this_value(this_val);
            if (sv.is_undefined()) return EvalResult::ok(Value::number(std::numeric_limits<double>::quiet_NaN()));
            std::string_view s = sv.js_string_raw()->sv();
            double idx_d = args.empty() ? 0.0 : to_number_double(args[0]);
            int32_t idx = static_cast<int32_t>(std::isfinite(idx_d) ? std::trunc(idx_d) : 0.0);
            // idx is a UTF-16 code unit index
            int32_t len = utf8_cp_len(sv.js_string_raw());
            if (idx < 0 || idx >= len) {
                return EvalResult::ok(Value::number(std::numeric_limits<double>::quiet_NaN()));
            }
            size_t byte_pos = utf8_cu_to_byte(s, idx);
            unsigned char c = static_cast<unsigned char>(s[byte_pos]);
            uint16_t code_unit;
            if (c < 0x80) {
                code_unit = c;
            } else if (c < 0xE0) {
                code_unit = static_cast<uint16_t>((c & 0x1F) << 6 | (static_cast<unsigned char>(s[byte_pos + 1]) & 0x3F));
            } else if (c < 0xF0) {
                code_unit = static_cast<uint16_t>((c & 0x0F) << 12 |
                    (static_cast<unsigned char>(s[byte_pos + 1]) & 0x3F) << 6 |
                    (static_cast<unsigned char>(s[byte_pos + 2]) & 0x3F));
            } else {
                // SMP: 4-byte UTF-8 → surrogate pair. Return high surrogate.
                uint32_t cp = ((c & 0x07u) << 18) |
                    ((static_cast<unsigned char>(s[byte_pos + 1]) & 0x3Fu) << 12) |
                    ((static_cast<unsigned char>(s[byte_pos + 2]) & 0x3Fu) << 6) |
                    (static_cast<unsigned char>(s[byte_pos + 3]) & 0x3Fu);
                cp -= 0x10000u;
                code_unit = static_cast<uint16_t>(0xD800u + (cp >> 10));
            }
            return EvalResult::ok(Value::number(static_cast<double>(code_unit)));
        });
        gc_heap_.Register(fn.get());
        if (string_prototype_) string_prototype_->define_builtin_property("charCodeAt", Value::object(ObjectPtr(fn)));
    }

    // ---- String.prototype.charAt ----
    {
        auto fn = RcPtr<JSFunction>::make();
        fn->set_name(std::string("charAt"));
        fn->set_native_fn([](Value this_val, std::vector<Value> args, bool) -> EvalResult {
            Value sv = string_this_value(this_val);
            if (sv.is_undefined()) return EvalResult::ok(Value::string(""));
            std::string_view s = sv.js_string_raw()->sv();
            double idx_d = args.empty() ? 0.0 : to_number_double(args[0]);
            int32_t idx = static_cast<int32_t>(std::isfinite(idx_d) ? std::trunc(idx_d) : 0.0);
            int32_t len = utf8_cp_len(sv.js_string_raw());
            if (idx < 0 || idx >= len) {
                return EvalResult::ok(Value::string(""));
            }
            return EvalResult::ok(Value::string(utf8_substr(s, idx, idx + 1)));
        });
        gc_heap_.Register(fn.get());
        if (string_prototype_) string_prototype_->define_builtin_property("charAt", Value::object(ObjectPtr(fn)));
    }

    // ---- String.prototype.codePointAt ----
    {
        auto fn = RcPtr<JSFunction>::make();
        fn->set_name(std::string("codePointAt"));
        fn->set_native_fn([](Value this_val, std::vector<Value> args, bool) -> EvalResult {
            Value sv = string_this_value(this_val);
            if (sv.is_undefined()) return EvalResult::ok(Value::undefined());
            std::string_view s = sv.js_string_raw()->sv();
            double idx_d = args.empty() ? 0.0 : to_number_double(args[0]);
            int32_t idx = static_cast<int32_t>(std::isfinite(idx_d) ? std::trunc(idx_d) : 0.0);
            int32_t len = utf8_cp_len(sv.js_string_raw());
            if (idx < 0 || idx >= len) return EvalResult::ok(Value::undefined());
            size_t byte_pos = utf8_cu_to_byte(s, idx);
            unsigned char c = static_cast<unsigned char>(s[byte_pos]);
            uint32_t cp;
            if (c < 0x80) {
                cp = c;
            } else if (c < 0xE0) {
                cp = static_cast<uint32_t>((c & 0x1F) << 6 | (static_cast<unsigned char>(s[byte_pos + 1]) & 0x3F));
            } else if (c < 0xF0) {
                cp = static_cast<uint32_t>((c & 0x0F) << 12 |
                    (static_cast<unsigned char>(s[byte_pos + 1]) & 0x3F) << 6 |
                    (static_cast<unsigned char>(s[byte_pos + 2]) & 0x3F));
            } else {
                cp = ((c & 0x07u) << 18) |
                    ((static_cast<unsigned char>(s[byte_pos + 1]) & 0x3Fu) << 12) |
                    ((static_cast<unsigned char>(s[byte_pos + 2]) & 0x3Fu) << 6) |
                    (static_cast<unsigned char>(s[byte_pos + 3]) & 0x3Fu);
            }
            return EvalResult::ok(Value::number(static_cast<double>(cp)));
        });
        gc_heap_.Register(fn.get());
        if (string_prototype_) string_prototype_->define_builtin_property("codePointAt", Value::object(ObjectPtr(fn)));
    }

    // ---- String.prototype.normalize ----
    {
        auto fn = RcPtr<JSFunction>::make();
        fn->set_name(std::string("normalize"));
        fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
            if (this_val.is_null() || this_val.is_undefined()) {
                pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                    "String.prototype.normalize requires a string");
                return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
            }
            Value sv = string_this_value(this_val);
            std::string form = "NFC";
            if (!args.empty() && !args[0].is_undefined()) {
                form = to_string_val(args[0]);
            }
            if (form != "NFC" && form != "NFD" && form != "NFKC" && form != "NFKD") {
                pending_throw_ = make_error_value(NativeErrorType::kRangeError,
                    "The normalization form should be one of NFC, NFD, NFKC, NFKD");
                return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
            }
            return EvalResult::ok(sv);
        });
        gc_heap_.Register(fn.get());
        if (string_prototype_) string_prototype_->define_builtin_property("normalize", Value::object(ObjectPtr(fn)));
    }

    // concat(), trimLeft/trimRight aliases, localeCompare
    {
        // String.prototype.concat(...args) → join args into this + args
        auto fn = RcPtr<JSFunction>::make();
        fn->set_name(std::string("concat"));
        fn->set_native_fn([](Value this_val, std::vector<Value> args, bool) -> EvalResult {
            std::string result = std::string(string_this_value(this_val).sv());
            for (auto& a : args) result += to_string_val(a);
            return EvalResult::ok(Value::string(result));
        });
        if (string_prototype_) string_prototype_->define_builtin_property("concat", Value::object(ObjectPtr(fn)));
    }
    {
        // trimLeft = trimStart, trimRight = trimEnd
        Value ts = string_prototype_ ? string_prototype_->get_property("trimStart") : Value::undefined();
        Value te = string_prototype_ ? string_prototype_->get_property("trimEnd") : Value::undefined();
        if (string_prototype_) {
            string_prototype_->define_builtin_property("trimLeft", ts);
            string_prototype_->define_builtin_property("trimRight", te);
        }
    }
    {
        // localeCompare(compareStr) → -1/0/1
        auto fn = RcPtr<JSFunction>::make();
        fn->set_name(std::string("localeCompare"));
        fn->set_native_fn([](Value this_val, std::vector<Value> args, bool) -> EvalResult {
            std::string a = std::string(string_this_value(this_val).sv());
            std::string b = args.empty() ? "undefined" : to_string_val(args[0]);
            int cmp = a.compare(b);
            return EvalResult::ok(Value::number(cmp < 0 ? -1.0 : (cmp > 0 ? 1.0 : 0.0)));
        });
        if (string_prototype_) string_prototype_->define_builtin_property("localeCompare", Value::object(ObjectPtr(fn)));
    }

    // ---- Annex B HTML string wrapping methods ----
    // Helper macro-like lambda builder for simple tag methods
    {
        // Build a helper: wraps string in a tag pair
        auto make_tag_fn = [this](const char* open, const char* close) {
            auto fn = RcPtr<JSFunction>::make();
            std::string open_s = open;
            std::string close_s = close;
            fn->set_native_fn([this, open_s, close_s](Value this_val, std::vector<Value> /*args*/, bool) -> EvalResult {
                std::string str;
                if (this_val.is_string()) {
                    str = this_val.as_string();
                } else if (this_val.is_object()) {
                    RcObject* raw = this_val.as_object_raw();
                    if (raw && raw->object_kind() == ObjectKind::kStringObject) {
                        str = static_cast<JSObject*>(raw)->wrapped_value().as_string();
                    } else {
                        str = to_string_val(this_val);
                    }
                } else if (this_val.is_null() || this_val.is_undefined()) {
                    pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                        "Cannot call method on null or undefined");
                    return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
                } else {
                    str = to_string_val(this_val);
                }
                return EvalResult::ok(Value::string(open_s + str + close_s));
            });
            return fn;
        };
        auto make_attr_fn = [this](const char* tag, const char* attr) {
            auto fn = RcPtr<JSFunction>::make();
            std::string tag_s = tag;
            std::string attr_s = attr;
            fn->set_native_fn([this, tag_s, attr_s](Value this_val, std::vector<Value> args, bool) -> EvalResult {
                std::string str;
                if (this_val.is_string()) {
                    str = this_val.as_string();
                } else if (this_val.is_null() || this_val.is_undefined()) {
                    pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                        "Cannot call method on null or undefined");
                    return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
                } else {
                    str = to_string_val(this_val);
                }
                std::string val = args.empty() ? "undefined" : to_string_val(args[0]);
                // Escape double quotes in attribute value
                std::string escaped;
                for (char c : val) {
                    if (c == '"') escaped += "&quot;";
                    else escaped += c;
                }
                return EvalResult::ok(Value::string(
                    "<" + tag_s + " " + attr_s + "=\"" + escaped + "\">" + str + "</" + tag_s + ">"));
            });
            return fn;
        };
        if (string_prototype_) {
            auto big_fn     = make_tag_fn("<big>",    "</big>");    big_fn->set_name("big");
            auto blink_fn   = make_tag_fn("<blink>",  "</blink>");  blink_fn->set_name("blink");
            auto bold_fn    = make_tag_fn("<b>",      "</b>");      bold_fn->set_name("bold");
            auto fixed_fn   = make_tag_fn("<tt>",     "</tt>");     fixed_fn->set_name("fixed");
            auto italics_fn = make_tag_fn("<i>",      "</i>");      italics_fn->set_name("italics");
            auto small_fn   = make_tag_fn("<small>",  "</small>");  small_fn->set_name("small");
            auto strike_fn  = make_tag_fn("<strike>", "</strike>"); strike_fn->set_name("strike");
            auto sub_fn     = make_tag_fn("<sub>",    "</sub>");    sub_fn->set_name("sub");
            auto sup_fn     = make_tag_fn("<sup>",    "</sup>");    sup_fn->set_name("sup");
            auto anchor_fn  = make_attr_fn("a",    "name");    anchor_fn->set_name("anchor");
            auto link_fn    = make_attr_fn("a",    "href");    link_fn->set_name("link");
            auto fontcolor_fn = make_attr_fn("font", "color"); fontcolor_fn->set_name("fontcolor");
            auto fontsize_fn  = make_attr_fn("font", "size");  fontsize_fn->set_name("fontsize");
            string_prototype_->define_builtin_property("big",      Value::object(ObjectPtr(big_fn)));
            string_prototype_->define_builtin_property("blink",    Value::object(ObjectPtr(blink_fn)));
            string_prototype_->define_builtin_property("bold",     Value::object(ObjectPtr(bold_fn)));
            string_prototype_->define_builtin_property("fixed",    Value::object(ObjectPtr(fixed_fn)));
            string_prototype_->define_builtin_property("italics",  Value::object(ObjectPtr(italics_fn)));
            string_prototype_->define_builtin_property("small",    Value::object(ObjectPtr(small_fn)));
            string_prototype_->define_builtin_property("strike",   Value::object(ObjectPtr(strike_fn)));
            string_prototype_->define_builtin_property("sub",      Value::object(ObjectPtr(sub_fn)));
            string_prototype_->define_builtin_property("sup",      Value::object(ObjectPtr(sup_fn)));
            string_prototype_->define_builtin_property("anchor",   Value::object(ObjectPtr(anchor_fn)));
            string_prototype_->define_builtin_property("link",     Value::object(ObjectPtr(link_fn)));
            string_prototype_->define_builtin_property("fontcolor", Value::object(ObjectPtr(fontcolor_fn)));
            string_prototype_->define_builtin_property("fontsize",  Value::object(ObjectPtr(fontsize_fn)));
        }
    }

    // ---- Symbol ----

    symbol_prototype_ = RcPtr<JSObject>::make();
    symbol_prototype_->set_proto(object_prototype_);
    gc_heap_.Register(symbol_prototype_.get());

    // Symbol.prototype.toString
    auto sym_tostring_fn = RcPtr<JSFunction>::make();
    sym_tostring_fn->set_native_fn([this](Value this_val, std::vector<Value> /*args*/, bool) -> EvalResult {
        if (!this_val.is_symbol()) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "Symbol.prototype.toString requires a symbol");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        return EvalResult::ok(Value::string(symbol_to_string(this_val.as_symbol_id(), symbol_table_)));
    });
    symbol_prototype_->define_builtin_property("toString", Value::object(ObjectPtr(sym_tostring_fn)));
    gc_heap_.Register(sym_tostring_fn.get());

    // Symbol.prototype.valueOf
    auto sym_valueof_fn = RcPtr<JSFunction>::make();
    sym_valueof_fn->set_native_fn([](Value this_val, std::vector<Value> /*args*/, bool) -> EvalResult {
        return EvalResult::ok(this_val);
    });
    symbol_prototype_->define_builtin_property("valueOf", Value::object(ObjectPtr(sym_valueof_fn)));
    gc_heap_.Register(sym_valueof_fn.get());

    // Symbol constructor
    symbol_constructor_ = RcPtr<JSFunction>::make();
    symbol_constructor_->set_name(std::string("Symbol"));
    symbol_constructor_->set_native_fn([this](Value /*this_val*/, std::vector<Value> args, bool is_new_call) -> EvalResult {
        if (is_new_call) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "Symbol is not a constructor");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        std::optional<std::string> description = std::nullopt;
        if (!args.empty() && !args[0].is_undefined()) {
            description = to_string_val(args[0]);
        }
        uint64_t id = symbol_table_.NewSymbol(std::move(description));
        return EvalResult::ok(Value::symbol(id));
    });

    // Symbol.for
    auto sym_for_fn = RcPtr<JSFunction>::make();
    sym_for_fn->set_native_fn([this](Value /*this_val*/, std::vector<Value> args, bool) -> EvalResult {
        std::string key = args.empty() ? "undefined" : to_string_val(args[0]);
        uint64_t id = symbol_table_.ForKey(key);
        return EvalResult::ok(Value::symbol(id));
    });
    symbol_constructor_->set_property("for", Value::object(ObjectPtr(sym_for_fn)));
    gc_heap_.Register(sym_for_fn.get());

    // Symbol.keyFor
    auto sym_keyfor_fn = RcPtr<JSFunction>::make();
    sym_keyfor_fn->set_native_fn([this](Value /*this_val*/, std::vector<Value> args, bool) -> EvalResult {
        if (args.empty() || !args[0].is_symbol()) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "Symbol.keyFor argument must be a symbol");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        auto key = symbol_table_.KeyForId(args[0].as_symbol_id());
        if (!key.has_value()) return EvalResult::ok(Value::undefined());
        return EvalResult::ok(Value::string(*key));
    });
    symbol_constructor_->set_property("keyFor", Value::object(ObjectPtr(sym_keyfor_fn)));
    gc_heap_.Register(sym_keyfor_fn.get());

    // Well-Known Symbols as properties on Symbol constructor
    symbol_constructor_->set_property("iterator",
        Value::symbol(symbol_table_.well_known_iterator));
    symbol_constructor_->set_property("toPrimitive",
        Value::symbol(symbol_table_.well_known_to_primitive));
    symbol_constructor_->set_property("hasInstance",
        Value::symbol(symbol_table_.well_known_has_instance));
    symbol_constructor_->set_property("toStringTag",
        Value::symbol(symbol_table_.well_known_to_string_tag));
    symbol_constructor_->set_property("asyncIterator",
        Value::symbol(symbol_table_.well_known_async_iterator));
    symbol_constructor_->set_property("species",
        Value::symbol(symbol_table_.well_known_species));

    gc_heap_.Register(symbol_constructor_.get());
    global_env_->define_initialized("Symbol");
    global_env_->set("Symbol", Value::object(ObjectPtr(symbol_constructor_)));

    // String constructor: explicit String(sym) → "Symbol(x)"
    // (Handled in the existing string_constructor_ NativeFn by checking is_symbol())
    // We patch here by wrapping the existing fn — but it's simpler to handle in the
    // string_constructor_ lambda itself. Since string_constructor_ is already set up,
    // we need to handle the Symbol case there. For now, we handle it in to_string_val
    // via a special path in the String() constructor.

    // Generator prototype: [Symbol.toStringTag] = "Generator", [Symbol.iterator] = return this
    generator_prototype_ = RcPtr<JSObject>::make();
    generator_prototype_->set_proto(object_prototype_);
    gc_heap_.Register(generator_prototype_.get());
    generator_prototype_->set_property_by_symbol(symbol_table_.well_known_to_string_tag,
        Value::string("Generator"));
    {
        auto gen_iter_fn = RcPtr<JSFunction>::make();
        gen_iter_fn->set_native_fn([](Value this_val, std::vector<Value> /*args*/, bool) -> EvalResult {
            return EvalResult::ok(this_val);
        });
        gc_heap_.Register(gen_iter_fn.get());
        generator_prototype_->set_property_by_symbol(symbol_table_.well_known_iterator,
            Value::object(ObjectPtr(gen_iter_fn)));
    }

    // ---- Map ----
    {
        map_prototype_ = RcPtr<JSObject>::make();
        map_prototype_->set_proto(object_prototype_);
        gc_heap_.Register(map_prototype_.get());

        // size getter via "size" property that dispatches based on ObjectKind
        {
            auto size_fn = RcPtr<JSFunction>::make();
            size_fn->set_native_fn([this](Value this_val, std::vector<Value>, bool) -> EvalResult {
                if (!this_val.is_object() || this_val.as_object_raw()->object_kind() != ObjectKind::kMap) {
                    pending_throw_ = make_error_value(NativeErrorType::kTypeError, "Map.prototype.size requires a Map");
                    return EvalResult::err(Error{ErrorKind::Runtime, kPendingThrowSentinel});
                }
                auto* m = static_cast<JSMap*>(this_val.as_object_raw());
                return EvalResult::ok(Value::number(static_cast<double>(m->entries_.size())));
            });
            gc_heap_.Register(size_fn.get());
            PropDesc desc;
            desc.getter = Value::object(ObjectPtr(size_fn));
            desc.enumerable = false;
            desc.configurable = true;
            map_prototype_->define_property("size", desc);
        }

        auto make_map_method = [&](auto fn_body) {
            auto fn = RcPtr<JSFunction>::make();
            fn->set_native_fn(fn_body);
            gc_heap_.Register(fn.get());
            return fn;
        };

        // Map.prototype.set
        map_prototype_->define_builtin_property("set", Value::object(ObjectPtr(make_map_method(
            [this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
                if (!this_val.is_object() || this_val.as_object_raw()->object_kind() != ObjectKind::kMap) {
                    pending_throw_ = make_error_value(NativeErrorType::kTypeError, "Map.prototype.set called on non-Map");
                    return EvalResult::err(Error{ErrorKind::Runtime, kPendingThrowSentinel});
                }
                auto* m = static_cast<JSMap*>(this_val.as_object_raw());
                Value key = args.size() > 0 ? args[0] : Value::undefined();
                Value val = args.size() > 1 ? args[1] : Value::undefined();
                size_t idx = m->find_key(key);
                if (idx == JSMap::kNotFound) {
                    m->entries_.emplace_back(std::move(key), std::move(val));
                } else {
                    m->entries_[idx].second = std::move(val);
                }
                return EvalResult::ok(this_val);
            }))));

        // Map.prototype.get
        map_prototype_->define_builtin_property("get", Value::object(ObjectPtr(make_map_method(
            [this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
                if (!this_val.is_object() || this_val.as_object_raw()->object_kind() != ObjectKind::kMap) {
                    pending_throw_ = make_error_value(NativeErrorType::kTypeError, "Map.prototype.get called on non-Map");
                    return EvalResult::err(Error{ErrorKind::Runtime, kPendingThrowSentinel});
                }
                auto* m = static_cast<JSMap*>(this_val.as_object_raw());
                Value key = args.size() > 0 ? args[0] : Value::undefined();
                size_t idx = m->find_key(key);
                if (idx == JSMap::kNotFound) return EvalResult::ok(Value::undefined());
                return EvalResult::ok(m->entries_[idx].second);
            }))));

        // Map.prototype.has
        map_prototype_->define_builtin_property("has", Value::object(ObjectPtr(make_map_method(
            [this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
                if (!this_val.is_object() || this_val.as_object_raw()->object_kind() != ObjectKind::kMap) {
                    pending_throw_ = make_error_value(NativeErrorType::kTypeError, "Map.prototype.has called on non-Map");
                    return EvalResult::err(Error{ErrorKind::Runtime, kPendingThrowSentinel});
                }
                auto* m = static_cast<JSMap*>(this_val.as_object_raw());
                Value key = args.size() > 0 ? args[0] : Value::undefined();
                return EvalResult::ok(Value::boolean(m->find_key(key) != JSMap::kNotFound));
            }))));

        // Map.prototype.delete
        map_prototype_->define_builtin_property("delete", Value::object(ObjectPtr(make_map_method(
            [this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
                if (!this_val.is_object() || this_val.as_object_raw()->object_kind() != ObjectKind::kMap) {
                    pending_throw_ = make_error_value(NativeErrorType::kTypeError, "Map.prototype.delete called on non-Map");
                    return EvalResult::err(Error{ErrorKind::Runtime, kPendingThrowSentinel});
                }
                auto* m = static_cast<JSMap*>(this_val.as_object_raw());
                Value key = args.size() > 0 ? args[0] : Value::undefined();
                size_t idx = m->find_key(key);
                if (idx == JSMap::kNotFound) return EvalResult::ok(Value::boolean(false));
                m->entries_.erase(m->entries_.begin() + static_cast<ptrdiff_t>(idx));
                return EvalResult::ok(Value::boolean(true));
            }))));

        // Map.prototype.clear
        map_prototype_->define_builtin_property("clear", Value::object(ObjectPtr(make_map_method(
            [this](Value this_val, std::vector<Value>, bool) -> EvalResult {
                if (!this_val.is_object() || this_val.as_object_raw()->object_kind() != ObjectKind::kMap) {
                    pending_throw_ = make_error_value(NativeErrorType::kTypeError, "Map.prototype.clear called on non-Map");
                    return EvalResult::err(Error{ErrorKind::Runtime, kPendingThrowSentinel});
                }
                auto* m = static_cast<JSMap*>(this_val.as_object_raw());
                m->entries_.clear();
                return EvalResult::ok(Value::undefined());
            }))));

        // Map.prototype.forEach
        map_prototype_->define_builtin_property("forEach", Value::object(ObjectPtr(make_map_method(
            [this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
                if (!this_val.is_object() || this_val.as_object_raw()->object_kind() != ObjectKind::kMap) {
                    pending_throw_ = make_error_value(NativeErrorType::kTypeError, "Map.prototype.forEach called on non-Map");
                    return EvalResult::err(Error{ErrorKind::Runtime, kPendingThrowSentinel});
                }
                if (args.empty() || !args[0].is_object() ||
                    args[0].as_object_raw()->object_kind() != ObjectKind::kFunction) {
                    pending_throw_ = make_error_value(NativeErrorType::kTypeError, "forEach callback must be a function");
                    return EvalResult::err(Error{ErrorKind::Runtime, kPendingThrowSentinel});
                }
                auto* m = static_cast<JSMap*>(this_val.as_object_raw());
                Value this_arg = args.size() > 1 ? args[1] : Value::undefined();
                size_t sz = m->entries_.size();
                for (size_t i = 0; i < sz; ++i) {
                    // Re-read size after each callback call in case entries changed
                    if (i >= m->entries_.size()) break;
                    Value cb_args[3] = {m->entries_[i].second, m->entries_[i].first, this_val};
                    auto r = call_function_val(args[0], this_arg, std::span<Value>(cb_args, 3));
                    if (!r.is_ok()) return r;
                }
                return EvalResult::ok(Value::undefined());
            }))));

        // Helper: create a MapIterator-based iterator
        auto make_map_iter_fn = [&](CollectionIterMode mode) {
            auto fn = RcPtr<JSFunction>::make();
            fn->set_native_fn([this, mode](Value this_val, std::vector<Value>, bool) -> EvalResult {
                if (!this_val.is_object() || this_val.as_object_raw()->object_kind() != ObjectKind::kMap) {
                    pending_throw_ = make_error_value(NativeErrorType::kTypeError, "Map iterator called on non-Map");
                    return EvalResult::err(Error{ErrorKind::Runtime, kPendingThrowSentinel});
                }
                auto* m = static_cast<JSMap*>(this_val.as_object_raw());
                auto iter = RcPtr<JSMapIterator>::make();
                iter->map_ = RcPtr<JSMap>(m);
                iter->index_ = 0;
                iter->mode_ = mode;
                gc_heap_.Register(iter.get());

                // Wrap in a JSObject so it's a proper JS value
                auto iter_obj = RcPtr<JSObject>::make();
                iter_obj->set_proto(object_prototype_);
                gc_heap_.Register(iter_obj.get());
                iter_obj->set_property("__map_iter__", Value::object(ObjectPtr(iter)));

                auto next_fn = RcPtr<JSFunction>::make();
                next_fn->set_native_fn([this, mode](Value self, std::vector<Value>, bool) -> EvalResult {
                    if (!self.is_object()) {
                        auto r = RcPtr<JSObject>::make();
                        gc_heap_.Register(r.get());
                        r->set_property("value", Value::undefined());
                        r->set_property("done", Value::boolean(true));
                        return EvalResult::ok(Value::object(ObjectPtr(r)));
                    }
                    auto* iter_wrap = static_cast<JSObject*>(self.as_object_raw());
                    Value iter_val = iter_wrap->get_property("__map_iter__");
                    if (!iter_val.is_object() || iter_val.as_object_raw()->object_kind() != ObjectKind::kMapIterator) {
                        auto r = RcPtr<JSObject>::make();
                        gc_heap_.Register(r.get());
                        r->set_property("value", Value::undefined());
                        r->set_property("done", Value::boolean(true));
                        return EvalResult::ok(Value::object(ObjectPtr(r)));
                    }
                    auto* iter = static_cast<JSMapIterator*>(iter_val.as_object_raw());
                    auto make_result = [&](Value value, bool done) -> EvalResult {
                        auto r = RcPtr<JSObject>::make();
                        gc_heap_.Register(r.get());
                        r->set_property("value", std::move(value));
                        r->set_property("done", Value::boolean(done));
                        return EvalResult::ok(Value::object(ObjectPtr(r)));
                    };
                    if (!iter->map_ || iter->index_ >= iter->map_->entries_.size()) {
                        return make_result(Value::undefined(), true);
                    }
                    auto& entry = iter->map_->entries_[iter->index_++];
                    if (mode == CollectionIterMode::kKeys) {
                        return make_result(entry.first, false);
                    } else if (mode == CollectionIterMode::kValues) {
                        return make_result(entry.second, false);
                    } else {
                        // kEntries: return [k, v] array
                        auto pair_arr = RcPtr<JSObject>::make(ObjectKind::kArray);
                        gc_heap_.Register(pair_arr.get());
                        pair_arr->set_proto(array_prototype_);
                        pair_arr->elements_[0] = entry.first;
                        pair_arr->elements_[1] = entry.second;
                        pair_arr->array_length_ = 2;
                        return make_result(Value::object(ObjectPtr(pair_arr)), false);
                    }
                });
                gc_heap_.Register(next_fn.get());
                iter_obj->set_property("next", Value::object(ObjectPtr(next_fn)));

                // [Symbol.iterator]() { return this; }
                auto self_iter_fn = RcPtr<JSFunction>::make();
                self_iter_fn->set_native_fn([](Value self2, std::vector<Value>, bool) -> EvalResult {
                    return EvalResult::ok(self2);
                });
                gc_heap_.Register(self_iter_fn.get());
                iter_obj->set_property_by_symbol(symbol_table_.well_known_iterator,
                                                  Value::object(ObjectPtr(self_iter_fn)));
                return EvalResult::ok(Value::object(ObjectPtr(iter_obj)));
            });
            gc_heap_.Register(fn.get());
            return fn;
        };

        map_prototype_->define_builtin_property("keys", Value::object(ObjectPtr(
            make_map_iter_fn(CollectionIterMode::kKeys))));
        map_prototype_->define_builtin_property("values", Value::object(ObjectPtr(
            make_map_iter_fn(CollectionIterMode::kValues))));
        map_prototype_->define_builtin_property("entries", Value::object(ObjectPtr(
            make_map_iter_fn(CollectionIterMode::kEntries))));

        // [Symbol.iterator] = entries
        {
            auto entries_val = map_prototype_->get_property("entries");
            map_prototype_->set_property_by_symbol(symbol_table_.well_known_iterator, entries_val);
        }

        // Map constructor
        auto map_ctor = RcPtr<JSFunction>::make();
        map_ctor->set_name(std::string("Map"));
        map_ctor->set_native_fn([this](Value, std::vector<Value> args, bool is_new) -> EvalResult {
            if (!is_new) {
                pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                    "Constructor Map requires 'new'");
                return EvalResult::err(Error{ErrorKind::Runtime, kPendingThrowSentinel});
            }
            auto m = RcPtr<JSMap>::make();
            gc_heap_.Register(m.get());
            m->set_proto(map_prototype_);
            if (!args.empty() && !args[0].is_undefined() && !args[0].is_null()) {
                // Iterate over iterable, each element is [k, v]
                std::vector<Value> items;
                if (!spread_into(args[0], items)) {
                    return EvalResult::err(Error{ErrorKind::Runtime, kPendingThrowSentinel});
                }
                for (auto& item : items) {
                    if (!item.is_object()) {
                        pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                            "Iterator value is not an object");
                        return EvalResult::err(Error{ErrorKind::Runtime, kPendingThrowSentinel});
                    }
                    auto* pair_obj = static_cast<JSObject*>(item.as_object_raw());
                    Value k, v;
                    if (pair_obj->object_kind() == ObjectKind::kArray) {
                        k = pair_obj->elements_.count(0) ? pair_obj->elements_.at(0) : Value::undefined();
                        v = pair_obj->elements_.count(1) ? pair_obj->elements_.at(1) : Value::undefined();
                    } else {
                        k = pair_obj->get_property("0");
                        v = pair_obj->get_property("1");
                    }
                    size_t idx = m->find_key(k);
                    if (idx == JSMap::kNotFound) {
                        m->entries_.emplace_back(std::move(k), std::move(v));
                    } else {
                        m->entries_[idx].second = std::move(v);
                    }
                }
            }
            return EvalResult::ok(Value::object(ObjectPtr(m)));
        });
        gc_heap_.Register(map_ctor.get());
        global_env_->define_initialized("Map");
        global_env_->set("Map", Value::object(ObjectPtr(map_ctor)));
    }

    // ---- Set ----
    {
        set_prototype_ = RcPtr<JSObject>::make();
        set_prototype_->set_proto(object_prototype_);
        gc_heap_.Register(set_prototype_.get());

        {
            auto size_fn = RcPtr<JSFunction>::make();
            size_fn->set_native_fn([this](Value this_val, std::vector<Value>, bool) -> EvalResult {
                if (!this_val.is_object() || this_val.as_object_raw()->object_kind() != ObjectKind::kSet) {
                    pending_throw_ = make_error_value(NativeErrorType::kTypeError, "Set.prototype.size requires a Set");
                    return EvalResult::err(Error{ErrorKind::Runtime, kPendingThrowSentinel});
                }
                auto* s = static_cast<JSSet*>(this_val.as_object_raw());
                return EvalResult::ok(Value::number(static_cast<double>(s->values_.size())));
            });
            gc_heap_.Register(size_fn.get());
            PropDesc desc;
            desc.getter = Value::object(ObjectPtr(size_fn));
            desc.enumerable = false;
            desc.configurable = true;
            set_prototype_->define_property("size", desc);
        }

        auto make_set_method = [&](auto fn_body) {
            auto fn = RcPtr<JSFunction>::make();
            fn->set_native_fn(fn_body);
            gc_heap_.Register(fn.get());
            return fn;
        };

        // Set.prototype.add
        set_prototype_->define_builtin_property("add", Value::object(ObjectPtr(make_set_method(
            [this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
                if (!this_val.is_object() || this_val.as_object_raw()->object_kind() != ObjectKind::kSet) {
                    pending_throw_ = make_error_value(NativeErrorType::kTypeError, "Set.prototype.add called on non-Set");
                    return EvalResult::err(Error{ErrorKind::Runtime, kPendingThrowSentinel});
                }
                auto* s = static_cast<JSSet*>(this_val.as_object_raw());
                Value val = args.size() > 0 ? args[0] : Value::undefined();
                if (s->find_value(val) == JSSet::kNotFound) {
                    s->values_.push_back(std::move(val));
                }
                return EvalResult::ok(this_val);
            }))));

        // Set.prototype.has
        set_prototype_->define_builtin_property("has", Value::object(ObjectPtr(make_set_method(
            [this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
                if (!this_val.is_object() || this_val.as_object_raw()->object_kind() != ObjectKind::kSet) {
                    pending_throw_ = make_error_value(NativeErrorType::kTypeError, "Set.prototype.has called on non-Set");
                    return EvalResult::err(Error{ErrorKind::Runtime, kPendingThrowSentinel});
                }
                auto* s = static_cast<JSSet*>(this_val.as_object_raw());
                Value val = args.size() > 0 ? args[0] : Value::undefined();
                return EvalResult::ok(Value::boolean(s->find_value(val) != JSSet::kNotFound));
            }))));

        // Set.prototype.delete
        set_prototype_->define_builtin_property("delete", Value::object(ObjectPtr(make_set_method(
            [this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
                if (!this_val.is_object() || this_val.as_object_raw()->object_kind() != ObjectKind::kSet) {
                    pending_throw_ = make_error_value(NativeErrorType::kTypeError, "Set.prototype.delete called on non-Set");
                    return EvalResult::err(Error{ErrorKind::Runtime, kPendingThrowSentinel});
                }
                auto* s = static_cast<JSSet*>(this_val.as_object_raw());
                Value val = args.size() > 0 ? args[0] : Value::undefined();
                size_t idx = s->find_value(val);
                if (idx == JSSet::kNotFound) return EvalResult::ok(Value::boolean(false));
                s->values_.erase(s->values_.begin() + static_cast<ptrdiff_t>(idx));
                return EvalResult::ok(Value::boolean(true));
            }))));

        // Set.prototype.clear
        set_prototype_->define_builtin_property("clear", Value::object(ObjectPtr(make_set_method(
            [this](Value this_val, std::vector<Value>, bool) -> EvalResult {
                if (!this_val.is_object() || this_val.as_object_raw()->object_kind() != ObjectKind::kSet) {
                    pending_throw_ = make_error_value(NativeErrorType::kTypeError, "Set.prototype.clear called on non-Set");
                    return EvalResult::err(Error{ErrorKind::Runtime, kPendingThrowSentinel});
                }
                auto* s = static_cast<JSSet*>(this_val.as_object_raw());
                s->values_.clear();
                return EvalResult::ok(Value::undefined());
            }))));

        // Set.prototype.forEach
        set_prototype_->define_builtin_property("forEach", Value::object(ObjectPtr(make_set_method(
            [this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
                if (!this_val.is_object() || this_val.as_object_raw()->object_kind() != ObjectKind::kSet) {
                    pending_throw_ = make_error_value(NativeErrorType::kTypeError, "Set.prototype.forEach called on non-Set");
                    return EvalResult::err(Error{ErrorKind::Runtime, kPendingThrowSentinel});
                }
                if (args.empty() || !args[0].is_object() ||
                    args[0].as_object_raw()->object_kind() != ObjectKind::kFunction) {
                    pending_throw_ = make_error_value(NativeErrorType::kTypeError, "forEach callback must be a function");
                    return EvalResult::err(Error{ErrorKind::Runtime, kPendingThrowSentinel});
                }
                auto* s = static_cast<JSSet*>(this_val.as_object_raw());
                Value this_arg = args.size() > 1 ? args[1] : Value::undefined();
                size_t sz = s->values_.size();
                for (size_t i = 0; i < sz; ++i) {
                    if (i >= s->values_.size()) break;
                    Value cb_args[3] = {s->values_[i], s->values_[i], this_val};
                    auto r = call_function_val(args[0], this_arg, std::span<Value>(cb_args, 3));
                    if (!r.is_ok()) return r;
                }
                return EvalResult::ok(Value::undefined());
            }))));

        // Helper: create a SetIterator-based iterator
        auto make_set_iter_fn = [&](CollectionIterMode mode) {
            auto fn = RcPtr<JSFunction>::make();
            fn->set_native_fn([this, mode](Value this_val, std::vector<Value>, bool) -> EvalResult {
                if (!this_val.is_object() || this_val.as_object_raw()->object_kind() != ObjectKind::kSet) {
                    pending_throw_ = make_error_value(NativeErrorType::kTypeError, "Set iterator called on non-Set");
                    return EvalResult::err(Error{ErrorKind::Runtime, kPendingThrowSentinel});
                }
                auto* s = static_cast<JSSet*>(this_val.as_object_raw());
                auto iter = RcPtr<JSSetIterator>::make();
                iter->set_ = RcPtr<JSSet>(s);
                iter->index_ = 0;
                iter->mode_ = mode;
                gc_heap_.Register(iter.get());

                auto iter_obj = RcPtr<JSObject>::make();
                iter_obj->set_proto(object_prototype_);
                gc_heap_.Register(iter_obj.get());
                iter_obj->set_property("__set_iter__", Value::object(ObjectPtr(iter)));

                auto next_fn = RcPtr<JSFunction>::make();
                next_fn->set_native_fn([this, mode](Value self, std::vector<Value>, bool) -> EvalResult {
                    if (!self.is_object()) {
                        auto r = RcPtr<JSObject>::make();
                        gc_heap_.Register(r.get());
                        r->set_property("value", Value::undefined());
                        r->set_property("done", Value::boolean(true));
                        return EvalResult::ok(Value::object(ObjectPtr(r)));
                    }
                    auto* iter_wrap = static_cast<JSObject*>(self.as_object_raw());
                    Value iter_val = iter_wrap->get_property("__set_iter__");
                    if (!iter_val.is_object() || iter_val.as_object_raw()->object_kind() != ObjectKind::kSetIterator) {
                        auto r = RcPtr<JSObject>::make();
                        gc_heap_.Register(r.get());
                        r->set_property("value", Value::undefined());
                        r->set_property("done", Value::boolean(true));
                        return EvalResult::ok(Value::object(ObjectPtr(r)));
                    }
                    auto* iter = static_cast<JSSetIterator*>(iter_val.as_object_raw());
                    auto make_result = [&](Value value, bool done) -> EvalResult {
                        auto r = RcPtr<JSObject>::make();
                        gc_heap_.Register(r.get());
                        r->set_property("value", std::move(value));
                        r->set_property("done", Value::boolean(done));
                        return EvalResult::ok(Value::object(ObjectPtr(r)));
                    };
                    if (!iter->set_ || iter->index_ >= iter->set_->values_.size()) {
                        return make_result(Value::undefined(), true);
                    }
                    Value elem = iter->set_->values_[iter->index_++];
                    if (mode == CollectionIterMode::kEntries) {
                        auto pair_arr = RcPtr<JSObject>::make(ObjectKind::kArray);
                        gc_heap_.Register(pair_arr.get());
                        pair_arr->set_proto(array_prototype_);
                        pair_arr->elements_[0] = elem;
                        pair_arr->elements_[1] = elem;
                        pair_arr->array_length_ = 2;
                        return make_result(Value::object(ObjectPtr(pair_arr)), false);
                    }
                    return make_result(std::move(elem), false);
                });
                gc_heap_.Register(next_fn.get());
                iter_obj->set_property("next", Value::object(ObjectPtr(next_fn)));

                auto self_iter_fn = RcPtr<JSFunction>::make();
                self_iter_fn->set_native_fn([](Value self2, std::vector<Value>, bool) -> EvalResult {
                    return EvalResult::ok(self2);
                });
                gc_heap_.Register(self_iter_fn.get());
                iter_obj->set_property_by_symbol(symbol_table_.well_known_iterator,
                                                  Value::object(ObjectPtr(self_iter_fn)));
                return EvalResult::ok(Value::object(ObjectPtr(iter_obj)));
            });
            gc_heap_.Register(fn.get());
            return fn;
        };

        set_prototype_->define_builtin_property("values", Value::object(ObjectPtr(
            make_set_iter_fn(CollectionIterMode::kValues))));
        // keys() === values() for Set
        set_prototype_->define_builtin_property("keys", Value::object(ObjectPtr(
            make_set_iter_fn(CollectionIterMode::kValues))));
        set_prototype_->define_builtin_property("entries", Value::object(ObjectPtr(
            make_set_iter_fn(CollectionIterMode::kEntries))));

        // [Symbol.iterator] = values
        {
            auto values_val = set_prototype_->get_property("values");
            set_prototype_->set_property_by_symbol(symbol_table_.well_known_iterator, values_val);
        }

        // Set constructor
        auto set_ctor = RcPtr<JSFunction>::make();
        set_ctor->set_name(std::string("Set"));
        set_ctor->set_native_fn([this](Value, std::vector<Value> args, bool is_new) -> EvalResult {
            if (!is_new) {
                pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                    "Constructor Set requires 'new'");
                return EvalResult::err(Error{ErrorKind::Runtime, kPendingThrowSentinel});
            }
            auto s = RcPtr<JSSet>::make();
            gc_heap_.Register(s.get());
            s->set_proto(set_prototype_);
            if (!args.empty() && !args[0].is_undefined() && !args[0].is_null()) {
                std::vector<Value> items;
                if (!spread_into(args[0], items)) {
                    return EvalResult::err(Error{ErrorKind::Runtime, kPendingThrowSentinel});
                }
                for (auto& item : items) {
                    if (s->find_value(item) == JSSet::kNotFound) {
                        s->values_.push_back(std::move(item));
                    }
                }
            }
            return EvalResult::ok(Value::object(ObjectPtr(s)));
        });
        gc_heap_.Register(set_ctor.get());
        global_env_->define_initialized("Set");
        global_env_->set("Set", Value::object(ObjectPtr(set_ctor)));
    }

    // ---- WeakMap ----
    {
        weakmap_prototype_ = RcPtr<JSObject>::make();
        weakmap_prototype_->set_proto(object_prototype_);
        gc_heap_.Register(weakmap_prototype_.get());

        auto make_wm_method = [&](auto fn_body) {
            auto fn = RcPtr<JSFunction>::make();
            fn->set_native_fn(fn_body);
            gc_heap_.Register(fn.get());
            return fn;
        };

        weakmap_prototype_->define_builtin_property("set", Value::object(ObjectPtr(make_wm_method(
            [this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
                if (!this_val.is_object() || this_val.as_object_raw()->object_kind() != ObjectKind::kWeakMap) {
                    pending_throw_ = make_error_value(NativeErrorType::kTypeError, "WeakMap.prototype.set called on non-WeakMap");
                    return EvalResult::err(Error{ErrorKind::Runtime, kPendingThrowSentinel});
                }
                Value key = args.size() > 0 ? args[0] : Value::undefined();
                if (!key.is_object()) {
                    pending_throw_ = make_error_value(NativeErrorType::kTypeError, "Invalid value used as weak map key");
                    return EvalResult::err(Error{ErrorKind::Runtime, kPendingThrowSentinel});
                }
                auto* wm = static_cast<JSWeakMap*>(this_val.as_object_raw());
                Value val = args.size() > 1 ? args[1] : Value::undefined();
                wm->table_[key.as_object_raw()] = std::move(val);
                return EvalResult::ok(this_val);
            }))));

        weakmap_prototype_->define_builtin_property("get", Value::object(ObjectPtr(make_wm_method(
            [this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
                if (!this_val.is_object() || this_val.as_object_raw()->object_kind() != ObjectKind::kWeakMap) {
                    pending_throw_ = make_error_value(NativeErrorType::kTypeError, "WeakMap.prototype.get called on non-WeakMap");
                    return EvalResult::err(Error{ErrorKind::Runtime, kPendingThrowSentinel});
                }
                Value key = args.size() > 0 ? args[0] : Value::undefined();
                if (!key.is_object()) return EvalResult::ok(Value::undefined());
                auto* wm = static_cast<JSWeakMap*>(this_val.as_object_raw());
                auto it = wm->table_.find(key.as_object_raw());
                if (it == wm->table_.end()) return EvalResult::ok(Value::undefined());
                return EvalResult::ok(it->second);
            }))));

        weakmap_prototype_->define_builtin_property("has", Value::object(ObjectPtr(make_wm_method(
            [this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
                if (!this_val.is_object() || this_val.as_object_raw()->object_kind() != ObjectKind::kWeakMap) {
                    pending_throw_ = make_error_value(NativeErrorType::kTypeError, "WeakMap.prototype.has called on non-WeakMap");
                    return EvalResult::err(Error{ErrorKind::Runtime, kPendingThrowSentinel});
                }
                Value key = args.size() > 0 ? args[0] : Value::undefined();
                if (!key.is_object()) return EvalResult::ok(Value::boolean(false));
                auto* wm = static_cast<JSWeakMap*>(this_val.as_object_raw());
                return EvalResult::ok(Value::boolean(wm->table_.count(key.as_object_raw()) > 0));
            }))));

        weakmap_prototype_->define_builtin_property("delete", Value::object(ObjectPtr(make_wm_method(
            [this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
                if (!this_val.is_object() || this_val.as_object_raw()->object_kind() != ObjectKind::kWeakMap) {
                    pending_throw_ = make_error_value(NativeErrorType::kTypeError, "WeakMap.prototype.delete called on non-WeakMap");
                    return EvalResult::err(Error{ErrorKind::Runtime, kPendingThrowSentinel});
                }
                Value key = args.size() > 0 ? args[0] : Value::undefined();
                if (!key.is_object()) return EvalResult::ok(Value::boolean(false));
                auto* wm = static_cast<JSWeakMap*>(this_val.as_object_raw());
                return EvalResult::ok(Value::boolean(wm->table_.erase(key.as_object_raw()) > 0));
            }))));

        auto wm_ctor = RcPtr<JSFunction>::make();
        wm_ctor->set_name(std::string("WeakMap"));
        wm_ctor->set_native_fn([this](Value, std::vector<Value> args, bool is_new) -> EvalResult {
            if (!is_new) {
                pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                    "Constructor WeakMap requires 'new'");
                return EvalResult::err(Error{ErrorKind::Runtime, kPendingThrowSentinel});
            }
            auto wm = RcPtr<JSWeakMap>::make();
            gc_heap_.Register(wm.get());
            wm->set_proto(weakmap_prototype_);
            if (!args.empty() && !args[0].is_undefined() && !args[0].is_null()) {
                std::vector<Value> items;
                if (!spread_into(args[0], items)) {
                    return EvalResult::err(Error{ErrorKind::Runtime, kPendingThrowSentinel});
                }
                for (auto& item : items) {
                    if (!item.is_object()) {
                        pending_throw_ = make_error_value(NativeErrorType::kTypeError, "Invalid value used as weak map key");
                        return EvalResult::err(Error{ErrorKind::Runtime, kPendingThrowSentinel});
                    }
                    auto* pair_obj = static_cast<JSObject*>(item.as_object_raw());
                    Value k, v;
                    if (pair_obj->object_kind() == ObjectKind::kArray) {
                        k = pair_obj->elements_.count(0) ? pair_obj->elements_.at(0) : Value::undefined();
                        v = pair_obj->elements_.count(1) ? pair_obj->elements_.at(1) : Value::undefined();
                    } else {
                        k = pair_obj->get_property("0");
                        v = pair_obj->get_property("1");
                    }
                    if (!k.is_object()) {
                        pending_throw_ = make_error_value(NativeErrorType::kTypeError, "Invalid value used as weak map key");
                        return EvalResult::err(Error{ErrorKind::Runtime, kPendingThrowSentinel});
                    }
                    wm->table_[k.as_object_raw()] = std::move(v);
                }
            }
            return EvalResult::ok(Value::object(ObjectPtr(wm)));
        });
        gc_heap_.Register(wm_ctor.get());
        global_env_->define_initialized("WeakMap");
        global_env_->set("WeakMap", Value::object(ObjectPtr(wm_ctor)));
    }

    // ---- WeakSet ----
    {
        weakset_prototype_ = RcPtr<JSObject>::make();
        weakset_prototype_->set_proto(object_prototype_);
        gc_heap_.Register(weakset_prototype_.get());

        auto make_ws_method = [&](auto fn_body) {
            auto fn = RcPtr<JSFunction>::make();
            fn->set_native_fn(fn_body);
            gc_heap_.Register(fn.get());
            return fn;
        };

        weakset_prototype_->define_builtin_property("add", Value::object(ObjectPtr(make_ws_method(
            [this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
                if (!this_val.is_object() || this_val.as_object_raw()->object_kind() != ObjectKind::kWeakSet) {
                    pending_throw_ = make_error_value(NativeErrorType::kTypeError, "WeakSet.prototype.add called on non-WeakSet");
                    return EvalResult::err(Error{ErrorKind::Runtime, kPendingThrowSentinel});
                }
                Value val = args.size() > 0 ? args[0] : Value::undefined();
                if (!val.is_object()) {
                    pending_throw_ = make_error_value(NativeErrorType::kTypeError, "Invalid value used in weak set");
                    return EvalResult::err(Error{ErrorKind::Runtime, kPendingThrowSentinel});
                }
                auto* ws = static_cast<JSWeakSet*>(this_val.as_object_raw());
                ws->table_.insert(val.as_object_raw());
                return EvalResult::ok(this_val);
            }))));

        weakset_prototype_->define_builtin_property("has", Value::object(ObjectPtr(make_ws_method(
            [this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
                if (!this_val.is_object() || this_val.as_object_raw()->object_kind() != ObjectKind::kWeakSet) {
                    pending_throw_ = make_error_value(NativeErrorType::kTypeError, "WeakSet.prototype.has called on non-WeakSet");
                    return EvalResult::err(Error{ErrorKind::Runtime, kPendingThrowSentinel});
                }
                Value val = args.size() > 0 ? args[0] : Value::undefined();
                if (!val.is_object()) return EvalResult::ok(Value::boolean(false));
                auto* ws = static_cast<JSWeakSet*>(this_val.as_object_raw());
                return EvalResult::ok(Value::boolean(ws->table_.count(val.as_object_raw()) > 0));
            }))));

        weakset_prototype_->define_builtin_property("delete", Value::object(ObjectPtr(make_ws_method(
            [this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
                if (!this_val.is_object() || this_val.as_object_raw()->object_kind() != ObjectKind::kWeakSet) {
                    pending_throw_ = make_error_value(NativeErrorType::kTypeError, "WeakSet.prototype.delete called on non-WeakSet");
                    return EvalResult::err(Error{ErrorKind::Runtime, kPendingThrowSentinel});
                }
                Value val = args.size() > 0 ? args[0] : Value::undefined();
                if (!val.is_object()) return EvalResult::ok(Value::boolean(false));
                auto* ws = static_cast<JSWeakSet*>(this_val.as_object_raw());
                return EvalResult::ok(Value::boolean(ws->table_.erase(val.as_object_raw()) > 0));
            }))));

        auto ws_ctor = RcPtr<JSFunction>::make();
        ws_ctor->set_name(std::string("WeakSet"));
        ws_ctor->set_native_fn([this](Value, std::vector<Value> args, bool is_new) -> EvalResult {
            if (!is_new) {
                pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                    "Constructor WeakSet requires 'new'");
                return EvalResult::err(Error{ErrorKind::Runtime, kPendingThrowSentinel});
            }
            auto ws = RcPtr<JSWeakSet>::make();
            gc_heap_.Register(ws.get());
            ws->set_proto(weakset_prototype_);
            if (!args.empty() && !args[0].is_undefined() && !args[0].is_null()) {
                std::vector<Value> items;
                if (!spread_into(args[0], items)) {
                    return EvalResult::err(Error{ErrorKind::Runtime, kPendingThrowSentinel});
                }
                for (auto& item : items) {
                    if (!item.is_object()) {
                        pending_throw_ = make_error_value(NativeErrorType::kTypeError, "Invalid value used in weak set");
                        return EvalResult::err(Error{ErrorKind::Runtime, kPendingThrowSentinel});
                    }
                    ws->table_.insert(item.as_object_raw());
                }
            }
            return EvalResult::ok(Value::object(ObjectPtr(ws)));
        });
        gc_heap_.Register(ws_ctor.get());
        global_env_->define_initialized("WeakSet");
        global_env_->set("WeakSet", Value::object(ObjectPtr(ws_ctor)));
    }

    // ---- WeakRef ----
    // Simplified implementation: holds target strongly (no actual weak semantics needed for test262)
    {
        auto wr_proto = RcPtr<JSObject>::make();
        wr_proto->set_proto(object_prototype_);
        gc_heap_.Register(wr_proto.get());
        wr_proto->define_builtin_property("constructor", Value::undefined()); // placeholder, set below

        // WeakRef.prototype.deref()
        auto deref_fn = RcPtr<JSFunction>::make();
        deref_fn->set_name(std::string("deref"));
        deref_fn->set_native_fn([this](Value this_val, std::vector<Value> /*args*/, bool) -> EvalResult {
            if (!this_val.is_object() || !this_val.as_object_raw() ||
                this_val.as_object_raw()->object_kind() != ObjectKind::kOrdinary) {
                pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                    "WeakRef.prototype.deref called on non-WeakRef");
                return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
            }
            auto* obj = static_cast<JSObject*>(this_val.as_object_raw());
            return EvalResult::ok(obj->get_property("__weakref_target__"));
        });
        gc_heap_.Register(deref_fn.get());
        wr_proto->define_builtin_property("deref", Value::object(ObjectPtr(deref_fn)));
        // Symbol.toStringTag = "WeakRef"
        wr_proto->set_property_by_symbol(symbol_table_.well_known_to_string_tag, Value::string("WeakRef"));

        auto wr_ctor = RcPtr<JSFunction>::make();
        wr_ctor->set_name(std::string("WeakRef"));
        wr_ctor->set_property("length", Value::number(1.0));
        wr_ctor->set_prototype_obj(wr_proto);
        wr_proto->set_constructor_property(wr_ctor.get());
        wr_ctor->set_native_fn([this, wr_proto](Value /*this_val*/, std::vector<Value> args, bool is_new) -> EvalResult {
            if (!is_new) {
                pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                    "WeakRef constructor requires 'new'");
                return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
            }
            if (args.empty() || (!args[0].is_object() && !args[0].is_symbol())) {
                pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                    "WeakRef target must be an object or symbol");
                return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
            }
            auto ref_obj = RcPtr<JSObject>::make();
            gc_heap_.Register(ref_obj.get());
            ref_obj->set_proto(wr_proto);
            ref_obj->set_property("__weakref_target__", args[0]);
            return EvalResult::ok(Value::object(ObjectPtr(ref_obj)));
        });
        gc_heap_.Register(wr_ctor.get());
        wr_proto->define_builtin_property("constructor", Value::object(ObjectPtr(wr_ctor)));
        global_env_->define_initialized("WeakRef");
        global_env_->set("WeakRef", Value::object(ObjectPtr(wr_ctor)));
    }

    // ---- FinalizationRegistry ----
    // Simplified: no actual cleanup callbacks (GC-dependent), but API-correct
    {
        auto fr_proto = RcPtr<JSObject>::make();
        fr_proto->set_proto(object_prototype_);
        gc_heap_.Register(fr_proto.get());
        // register(target, heldValue[, unregisterToken])
        auto fr_register = RcPtr<JSFunction>::make();
        fr_register->set_name(std::string("register"));
        fr_register->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
            if (!this_val.is_object() || !this_val.as_object_raw() ||
                this_val.as_object_raw()->object_kind() != ObjectKind::kOrdinary) {
                pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                    "FinalizationRegistry.prototype.register called on wrong type");
                return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
            }
            if (args.empty() || (!args[0].is_object() && !args[0].is_symbol())) {
                pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                    "register: target must be an object or symbol");
                return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
            }
            return EvalResult::ok(Value::undefined());  // no-op (no GC callbacks)
        });
        gc_heap_.Register(fr_register.get());
        fr_proto->define_builtin_property("register", Value::object(ObjectPtr(fr_register)));
        // unregister(token)
        auto fr_unregister = RcPtr<JSFunction>::make();
        fr_unregister->set_name(std::string("unregister"));
        fr_unregister->set_native_fn([](Value /*this_val*/, std::vector<Value> /*args*/, bool) -> EvalResult {
            return EvalResult::ok(Value::boolean(false));  // no-op
        });
        gc_heap_.Register(fr_unregister.get());
        fr_proto->define_builtin_property("unregister", Value::object(ObjectPtr(fr_unregister)));
        fr_proto->set_property_by_symbol(symbol_table_.well_known_to_string_tag,
            Value::string("FinalizationRegistry"));

        auto fr_ctor = RcPtr<JSFunction>::make();
        fr_ctor->set_name(std::string("FinalizationRegistry"));
        fr_ctor->set_property("length", Value::number(1.0));
        fr_ctor->set_prototype_obj(fr_proto);
        fr_proto->set_constructor_property(fr_ctor.get());
        fr_ctor->set_native_fn([this, fr_proto](Value /*this_val*/, std::vector<Value> args, bool is_new) -> EvalResult {
            if (!is_new) {
                pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                    "FinalizationRegistry constructor requires 'new'");
                return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
            }
            if (args.empty() || !args[0].is_object() ||
                args[0].as_object_raw()->object_kind() != ObjectKind::kFunction) {
                pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                    "FinalizationRegistry: callback must be callable");
                return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
            }
            auto reg_obj = RcPtr<JSObject>::make();
            gc_heap_.Register(reg_obj.get());
            reg_obj->set_proto(fr_proto);
            reg_obj->set_property("__fr_callback__", args[0]);
            return EvalResult::ok(Value::object(ObjectPtr(reg_obj)));
        });
        gc_heap_.Register(fr_ctor.get());
        fr_proto->define_builtin_property("constructor", Value::object(ObjectPtr(fr_ctor)));
        global_env_->define_initialized("FinalizationRegistry");
        global_env_->set("FinalizationRegistry", Value::object(ObjectPtr(fr_ctor)));
    }

    // ---- globalThis ----
    {
        auto gt_obj = RcPtr<JSObject>::make();
        gc_heap_.Register(gt_obj.get());
        gt_obj->set_proto(object_prototype_);
        // Self-referential: globalThis.globalThis === globalThis
        gt_obj->set_property("globalThis", Value::object(ObjectPtr(gt_obj)));
        global_env_->define_initialized("globalThis");
        global_env_->set("globalThis", Value::object(ObjectPtr(gt_obj)));
    }

    // ---- JSON object ----
    {
        auto json_obj = RcPtr<JSObject>::make();
        gc_heap_.Register(json_obj.get());
        json_obj->set_proto(object_prototype_);

        auto stringify_fn = RcPtr<JSFunction>::make();
        stringify_fn->set_name(std::string("stringify"));
        stringify_fn->set_native_fn([this](Value, std::vector<Value> args, bool) -> EvalResult {
            if (args.empty()) return EvalResult::ok(Value::undefined());
            std::set<RcObject*> seen;
            std::string result;
            if (!json_stringify_value(args[0], result, seen)) {
                if (pending_throw_.has_value())
                    return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
                return EvalResult::ok(Value::undefined());
            }
            return EvalResult::ok(Value::string(result));
        });
        gc_heap_.Register(stringify_fn.get());
        json_obj->set_property("stringify", Value::object(ObjectPtr(stringify_fn)));

        auto parse_fn = RcPtr<JSFunction>::make();
        parse_fn->set_name(std::string("parse"));
        parse_fn->set_native_fn([this](Value, std::vector<Value> args, bool) -> EvalResult {
            if (args.empty() || args[0].is_undefined()) {
                pending_throw_ = make_error_value(NativeErrorType::kSyntaxError,
                                                  "JSON.parse: unexpected end of JSON input");
                return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
            }
            std::string text = to_string_val(args[0]);
            size_t pos = 0;
            auto result = json_parse_value(text, pos);
            if (!result.is_ok()) return result;
            // skip trailing whitespace
            while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t' ||
                                         text[pos] == '\n' || text[pos] == '\r')) ++pos;
            if (pos != text.size()) {
                if (pending_throw_.has_value())
                    return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
                pending_throw_ = make_error_value(NativeErrorType::kSyntaxError,
                                                  "JSON.parse: unexpected non-whitespace character after JSON data");
                return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
            }
            return result;
        });
        gc_heap_.Register(parse_fn.get());
        json_obj->set_property("parse", Value::object(ObjectPtr(parse_fn)));

        global_env_->define_initialized("JSON");
        global_env_->set("JSON", Value::object(ObjectPtr(json_obj)));
    }

    // ---- queueMicrotask ----
    {
        auto qmt_fn = RcPtr<JSFunction>::make();
        qmt_fn->set_name(std::string("queueMicrotask"));
        qmt_fn->set_native_fn([this](Value, std::vector<Value> args, bool) -> EvalResult {
            if (args.empty() || !args[0].is_object() ||
                args[0].as_object_raw()->object_kind() != ObjectKind::kFunction) {
                pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                                  "queueMicrotask: argument must be a function");
                return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
            }
            Value fn_val = args[0];
            // Enqueue as a reaction job with no capability (fn called with undefined arg)
            job_queue_.Enqueue(ReactionJob{fn_val, Value::undefined(), Value::undefined(), true});
            return EvalResult::ok(Value::undefined());
        });
        gc_heap_.Register(qmt_fn.get());
        global_env_->define_initialized("queueMicrotask");
        global_env_->set("queueMicrotask", Value::object(ObjectPtr(qmt_fn)));
    }

    // ---- Function constructor ----
    {
        auto fn_ctor = RcPtr<JSFunction>::make();
        fn_ctor->set_name(std::string("Function"));
        fn_ctor->set_native_fn([this](Value /*this_val*/, std::vector<Value> args, bool) -> EvalResult {
            // new Function([p1, p2, ...], body)
            std::string params_str;
            std::string body_str;
            if (args.empty()) {
                body_str = "";
            } else {
                body_str = to_string_val(args.back());
                for (size_t i = 0; i + 1 < args.size(); ++i) {
                    if (i > 0) params_str += ",";
                    params_str += to_string_val(args[i]);
                }
            }
            std::string src = "function __anon__(" + params_str + ") { " + body_str + " }";
            auto parse_result = parse_program(src);
            if (!parse_result.ok()) {
                pending_throw_ = make_error_value(NativeErrorType::kSyntaxError,
                    "Function constructor: " + parse_result.error().message());
                return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
            }
            const auto& body = parse_result.value().body;
            if (body.empty()) {
                pending_throw_ = make_error_value(NativeErrorType::kSyntaxError,
                    "Function constructor: empty body");
                return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
            }
            const auto* fn_decl = std::get_if<FunctionDeclaration>(&body[0].v);
            if (!fn_decl) {
                pending_throw_ = make_error_value(NativeErrorType::kSyntaxError,
                    "Function constructor: parse error");
                return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
            }
            return EvalResult::ok(make_function_value(std::nullopt, fn_decl->params,
                fn_decl->body, global_env_, false, fn_decl->rest_param));
        });
        gc_heap_.Register(fn_ctor.get());
        // Function.prototype = function_prototype_
        if (function_prototype_) {
            fn_ctor->set_property("prototype", Value::object(ObjectPtr(function_prototype_)));
            function_prototype_->define_builtin_property("constructor", Value::object(ObjectPtr(fn_ctor)));
        }
        global_env_->define_initialized("Function");
        global_env_->set("Function", Value::object(ObjectPtr(fn_ctor)));
    }

    // ---- eval ----
    {
        auto eval_fn = RcPtr<JSFunction>::make();
        eval_fn->set_name(std::string("eval"));
        eval_fn->set_native_fn([this](Value /*this_val*/, std::vector<Value> args, bool) -> EvalResult {
            if (args.empty() || !args[0].is_string()) {
                return EvalResult::ok(args.empty() ? Value::undefined() : args[0]);
            }
            std::string code(args[0].sv());
            auto parse_result = parse_program(code);
            if (!parse_result.ok()) {
                pending_throw_ = make_error_value(NativeErrorType::kSyntaxError,
                    parse_result.error().message());
                return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
            }
            const auto& body = parse_result.value().body;
            // Hoist var declarations and function declarations into the global env
            hoist_vars(body, *global_env_);
            Value last = Value::undefined();
            for (const auto& stmt : body) {
                auto r = eval_stmt(stmt);
                if (!r.is_ok()) return EvalResult::err(r.error());
                if (r.completion().is_return()) return EvalResult::ok(r.completion().value);
                if (!r.completion().value.is_undefined()) last = r.completion().value;
            }
            return EvalResult::ok(last);
        });
        gc_heap_.Register(eval_fn.get());
        global_env_->define_initialized("eval");
        global_env_->set("eval", Value::object(ObjectPtr(eval_fn)));
    }

    // ---- Reflect ----
    {
        auto reflect_obj = RcPtr<JSObject>::make();
        gc_heap_.Register(reflect_obj.get());
        reflect_obj->set_proto(object_prototype_);

        // Reflect.apply(target, thisArg, argsList)
        {
            auto fn = RcPtr<JSFunction>::make(); fn->set_name(std::string("apply"));
            fn->set_native_fn([this](Value, std::vector<Value> args, bool) -> EvalResult {
                if (args.size() < 1) { pending_throw_ = make_error_value(NativeErrorType::kTypeError, "Reflect.apply: target required"); return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel)); }
                Value this_arg = args.size() > 1 ? args[1] : Value::undefined();
                std::vector<Value> call_args;
                if (args.size() > 2 && args[2].is_object()) {
                    auto* raw = args[2].as_object_raw();
                    if (raw->object_kind() == ObjectKind::kArray) {
                        auto* arr = static_cast<JSObject*>(raw);
                        for (uint32_t i = 0; i < arr->array_length_; i++) {
                            auto it = arr->elements_.find(i);
                            call_args.push_back(it != arr->elements_.end() ? it->second : Value::undefined());
                        }
                    }
                }
                return call_function_val(args[0], this_arg, call_args);
            });
            reflect_obj->set_property("apply", Value::object(ObjectPtr(fn)));
        }
        // Reflect.has(obj, key) - like in operator
        {
            auto fn = RcPtr<JSFunction>::make(); fn->set_name(std::string("has"));
            fn->set_native_fn([this](Value, std::vector<Value> args, bool) -> EvalResult {
                if (args.size() < 2 || !args[0].is_object()) { pending_throw_ = make_error_value(NativeErrorType::kTypeError, "Reflect.has: object required"); return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel)); }
                std::string key = to_string_val(args[1]);
                auto* raw = args[0].as_object_raw();
                if (raw->object_kind() == ObjectKind::kOrdinary || raw->object_kind() == ObjectKind::kArray) {
                    auto* obj = static_cast<JSObject*>(raw);
                    return EvalResult::ok(Value::boolean(obj->has_property(key)));
                }
                return EvalResult::ok(Value::boolean(false));
            });
            reflect_obj->set_property("has", Value::object(ObjectPtr(fn)));
        }
        // Reflect.ownKeys(obj) - getOwnPropertyNames + getOwnPropertySymbols
        {
            auto fn = RcPtr<JSFunction>::make(); fn->set_name(std::string("ownKeys"));
            fn->set_native_fn([this](Value, std::vector<Value> args, bool) -> EvalResult {
                auto arr = RcPtr<JSObject>::make(ObjectKind::kArray); gc_heap_.Register(arr.get());
                arr->set_proto(array_prototype_);
                if (!args.empty() && args[0].is_object()) {
                    auto* raw = args[0].as_object_raw();
                    if (raw->object_kind() == ObjectKind::kOrdinary || raw->object_kind() == ObjectKind::kArray) {
                        auto* obj = static_cast<JSObject*>(raw);
                        auto all = obj->own_all_string_keys();
                        for (uint32_t i = 0; i < all.size(); i++) {
                            arr->elements_[i] = Value::string(all[i]);
                            arr->array_length_ = i + 1;
                        }
                    }
                }
                return EvalResult::ok(Value::object(ObjectPtr(arr)));
            });
            reflect_obj->set_property("ownKeys", Value::object(ObjectPtr(fn)));
        }
        // Reflect.getPrototypeOf = Object.getPrototypeOf
        reflect_obj->set_property("getPrototypeOf", object_constructor_->get_property("getPrototypeOf"));
        // Reflect.setPrototypeOf = Object.setPrototypeOf
        reflect_obj->set_property("setPrototypeOf", object_constructor_->get_property("setPrototypeOf"));
        // Reflect.preventExtensions = Object.preventExtensions
        reflect_obj->set_property("preventExtensions", object_constructor_->get_property("preventExtensions"));
        // Reflect.getOwnPropertyDescriptor = Object.getOwnPropertyDescriptor
        reflect_obj->set_property("getOwnPropertyDescriptor", object_constructor_->get_property("getOwnPropertyDescriptor"));
        // Reflect.defineProperty = Object.defineProperty
        reflect_obj->set_property("defineProperty", object_constructor_->get_property("defineProperty"));
        // Reflect.deleteProperty(obj, key) - like delete obj[key]
        {
            auto fn = RcPtr<JSFunction>::make(); fn->set_name(std::string("deleteProperty"));
            fn->set_native_fn([](Value, std::vector<Value> args, bool) -> EvalResult {
                if (args.size() < 2 || !args[0].is_object()) return EvalResult::ok(Value::boolean(false));
                std::string key = to_string_val(args[1]);
                auto* raw = args[0].as_object_raw();
                if (raw->object_kind() == ObjectKind::kOrdinary || raw->object_kind() == ObjectKind::kArray) {
                    auto* obj = static_cast<JSObject*>(raw);
                    return EvalResult::ok(Value::boolean(obj->delete_property(key)));
                }
                return EvalResult::ok(Value::boolean(true));
            });
            reflect_obj->set_property("deleteProperty", Value::object(ObjectPtr(fn)));
        }
        // Reflect.isExtensible(obj)
        {
            auto fn = RcPtr<JSFunction>::make(); fn->set_name(std::string("isExtensible"));
            fn->set_native_fn([](Value, std::vector<Value> args, bool) -> EvalResult {
                if (args.empty() || !args[0].is_object()) return EvalResult::ok(Value::boolean(false));
                auto* raw = args[0].as_object_raw();
                if (raw->object_kind() == ObjectKind::kOrdinary || raw->object_kind() == ObjectKind::kArray) {
                    auto* obj_ext = static_cast<JSObject*>(raw);
                    return EvalResult::ok(Value::boolean(obj_ext->extensible()));
                }
                return EvalResult::ok(Value::boolean(true));
            });
            reflect_obj->set_property("isExtensible", Value::object(ObjectPtr(fn)));
        }
        global_env_->define_initialized("Reflect");
        global_env_->set("Reflect", Value::object(ObjectPtr(reflect_obj)));
    }

    // Register the global environment with GcHeap so user-created closures reachable
    // from it are treated as roots and not swept.
    gc_heap_.Register(global_env_.get());
}

Interpreter::Interpreter() {
    init_runtime();
}

// ---- JSON helpers ----

static std::string json_escape_string(std::string_view sv) {
    std::string out;
    out.reserve(sv.size() + 2);
    out += '"';
    for (unsigned char c : sv) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b";  break;
        case '\f': out += "\\f";  break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if (c < 0x20) {
                char buf[7];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    out += '"';
    return out;
}

bool Interpreter::json_stringify_value(const Value& val, std::string& out, std::set<RcObject*>& seen) {
    if (val.is_null()) { out += "null"; return true; }
    if (val.is_bool()) { out += val.as_bool() ? "true" : "false"; return true; }
    if (val.is_number()) {
        double d = val.as_number();
        if (std::isnan(d) || std::isinf(d)) { out += "null"; return true; }
        std::ostringstream oss;
        // Avoid trailing .0 for integer values
        if (d == std::trunc(d) && std::abs(d) < 1e15) {
            oss << static_cast<long long>(d);
        } else {
            oss << d;
        }
        out += oss.str();
        return true;
    }
    if (val.is_string()) { out += json_escape_string(val.sv()); return true; }
    if (val.is_undefined() || val.is_symbol()) return false;
    if (!val.is_object()) return false;

    RcObject* raw = val.as_object_raw();
    if (raw->object_kind() == ObjectKind::kFunction) return false;

    if (seen.count(raw)) {
        pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                          "JSON.stringify: circular reference");
        return false;
    }
    seen.insert(raw);

    if (raw->object_kind() == ObjectKind::kArray) {
        auto* arr = static_cast<JSObject*>(raw);
        out += '[';
        uint32_t len = arr->array_length_;
        for (uint32_t i = 0; i < len; ++i) {
            if (i > 0) out += ',';
            auto it = arr->elements_.find(i);
            if (it == arr->elements_.end() || it->second.is_undefined() ||
                it->second.is_symbol() ||
                (it->second.is_object() && it->second.as_object_raw()->object_kind() == ObjectKind::kFunction)) {
                out += "null";
            } else {
                if (!json_stringify_value(it->second, out, seen)) {
                    if (pending_throw_.has_value()) { seen.erase(raw); return false; }
                    out += "null";
                }
            }
        }
        out += ']';
    } else {
        // Plain object
        auto* obj = static_cast<JSObject*>(raw);
        auto keys = obj->own_enumerable_string_keys();
        out += '{';
        bool first = true;
        for (const auto& key : keys) {
            Value prop = obj->get_property(key);
            if (prop.is_undefined() || prop.is_symbol() ||
                (prop.is_object() && prop.as_object_raw()->object_kind() == ObjectKind::kFunction))
                continue;
            std::string prop_str;
            if (!json_stringify_value(prop, prop_str, seen)) {
                if (pending_throw_.has_value()) { seen.erase(raw); return false; }
                continue;
            }
            if (!first) out += ',';
            first = false;
            out += json_escape_string(key);
            out += ':';
            out += prop_str;
        }
        out += '}';
    }
    seen.erase(raw);
    return true;
}

EvalResult Interpreter::json_parse_value(const std::string& text, size_t& pos) {
    // skip whitespace
    while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t' ||
                                  text[pos] == '\n' || text[pos] == '\r')) ++pos;
    if (pos >= text.size()) {
        pending_throw_ = make_error_value(NativeErrorType::kSyntaxError,
                                          "JSON.parse: unexpected end of JSON input");
        return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
    }

    char c = text[pos];

    // null
    if (c == 'n') {
        if (text.substr(pos, 4) == "null") { pos += 4; return EvalResult::ok(Value::null()); }
        pending_throw_ = make_error_value(NativeErrorType::kSyntaxError, "JSON.parse: unexpected token");
        return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
    }
    // true
    if (c == 't') {
        if (text.substr(pos, 4) == "true") { pos += 4; return EvalResult::ok(Value::boolean(true)); }
        pending_throw_ = make_error_value(NativeErrorType::kSyntaxError, "JSON.parse: unexpected token");
        return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
    }
    // false
    if (c == 'f') {
        if (text.substr(pos, 5) == "false") { pos += 5; return EvalResult::ok(Value::boolean(false)); }
        pending_throw_ = make_error_value(NativeErrorType::kSyntaxError, "JSON.parse: unexpected token");
        return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
    }
    // string
    if (c == '"') {
        ++pos;
        std::string str;
        while (pos < text.size() && text[pos] != '"') {
            if (text[pos] == '\\') {
                ++pos;
                if (pos >= text.size()) break;
                switch (text[pos]) {
                case '"':  str += '"';  ++pos; break;
                case '\\': str += '\\'; ++pos; break;
                case '/':  str += '/';  ++pos; break;
                case 'b':  str += '\b'; ++pos; break;
                case 'f':  str += '\f'; ++pos; break;
                case 'n':  str += '\n'; ++pos; break;
                case 'r':  str += '\r'; ++pos; break;
                case 't':  str += '\t'; ++pos; break;
                case 'u': {
                    ++pos;
                    if (pos + 4 > text.size()) {
                        pending_throw_ = make_error_value(NativeErrorType::kSyntaxError,
                                                          "JSON.parse: invalid unicode escape");
                        return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
                    }
                    unsigned int code = 0;
                    for (int j = 0; j < 4; ++j) {
                        char h = text[pos + j];
                        int digit = 0;
                        if (h >= '0' && h <= '9') digit = h - '0';
                        else if (h >= 'a' && h <= 'f') digit = h - 'a' + 10;
                        else if (h >= 'A' && h <= 'F') digit = h - 'A' + 10;
                        else {
                            pending_throw_ = make_error_value(NativeErrorType::kSyntaxError,
                                                              "JSON.parse: invalid unicode escape");
                            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
                        }
                        code = code * 16 + static_cast<unsigned int>(digit);
                    }
                    pos += 4;
                    // Encode as UTF-8
                    if (code < 0x80) {
                        str += static_cast<char>(code);
                    } else if (code < 0x800) {
                        str += static_cast<char>(0xC0 | (code >> 6));
                        str += static_cast<char>(0x80 | (code & 0x3F));
                    } else {
                        str += static_cast<char>(0xE0 | (code >> 12));
                        str += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                        str += static_cast<char>(0x80 | (code & 0x3F));
                    }
                    break;
                }
                default:
                    pending_throw_ = make_error_value(NativeErrorType::kSyntaxError,
                                                      "JSON.parse: invalid escape sequence");
                    return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
                }
            } else {
                if (static_cast<unsigned char>(text[pos]) < 0x20) {
                    pending_throw_ = make_error_value(NativeErrorType::kSyntaxError,
                                                      "JSON.parse: invalid control character in string");
                    return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
                }
                str += text[pos++];
            }
        }
        if (pos >= text.size()) {
            pending_throw_ = make_error_value(NativeErrorType::kSyntaxError,
                                              "JSON.parse: unterminated string");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        ++pos; // consume closing '"'
        return EvalResult::ok(Value::string(str));
    }
    // number
    if (c == '-' || (c >= '0' && c <= '9')) {
        size_t start = pos;
        if (text[pos] == '-') ++pos;
        if (pos < text.size() && text[pos] == '0') {
            ++pos;
        } else {
            while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') ++pos;
        }
        if (pos < text.size() && text[pos] == '.') {
            ++pos;
            while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') ++pos;
        }
        if (pos < text.size() && (text[pos] == 'e' || text[pos] == 'E')) {
            ++pos;
            if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) ++pos;
            while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') ++pos;
        }
        std::string num_str = text.substr(start, pos - start);
        double d = std::strtod(num_str.c_str(), nullptr);
        return EvalResult::ok(Value::number(d));
    }
    // array
    if (c == '[') {
        ++pos;
        auto arr = RcPtr<JSObject>::make(ObjectKind::kArray);
        gc_heap_.Register(arr.get());
        arr->set_proto(array_prototype_);
        // skip whitespace
        while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t' ||
                                      text[pos] == '\n' || text[pos] == '\r')) ++pos;
        if (pos < text.size() && text[pos] == ']') { ++pos; return EvalResult::ok(Value::object(ObjectPtr(arr))); }
        uint32_t idx = 0;
        while (true) {
            auto elem = json_parse_value(text, pos);
            if (!elem.is_ok()) return elem;
            arr->elements_[idx] = elem.value();
            ++idx;
            arr->array_length_ = idx;
            // skip whitespace
            while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t' ||
                                          text[pos] == '\n' || text[pos] == '\r')) ++pos;
            if (pos >= text.size()) break;
            if (text[pos] == ']') { ++pos; break; }
            if (text[pos] != ',') {
                pending_throw_ = make_error_value(NativeErrorType::kSyntaxError,
                                                  "JSON.parse: expected ',' or ']'");
                return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
            }
            ++pos;
        }
        return EvalResult::ok(Value::object(ObjectPtr(arr)));
    }
    // object
    if (c == '{') {
        ++pos;
        auto obj = RcPtr<JSObject>::make();
        gc_heap_.Register(obj.get());
        obj->set_proto(object_prototype_);
        // skip whitespace
        while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t' ||
                                      text[pos] == '\n' || text[pos] == '\r')) ++pos;
        if (pos < text.size() && text[pos] == '}') { ++pos; return EvalResult::ok(Value::object(ObjectPtr(obj))); }
        while (true) {
            // parse key
            while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t' ||
                                          text[pos] == '\n' || text[pos] == '\r')) ++pos;
            if (pos >= text.size() || text[pos] != '"') {
                pending_throw_ = make_error_value(NativeErrorType::kSyntaxError,
                                                  "JSON.parse: expected string key");
                return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
            }
            auto key_result = json_parse_value(text, pos);
            if (!key_result.is_ok()) return key_result;
            std::string key = key_result.value().is_string() ? std::string(key_result.value().sv()) : "";
            // skip whitespace then ':'
            while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t' ||
                                          text[pos] == '\n' || text[pos] == '\r')) ++pos;
            if (pos >= text.size() || text[pos] != ':') {
                pending_throw_ = make_error_value(NativeErrorType::kSyntaxError,
                                                  "JSON.parse: expected ':'");
                return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
            }
            ++pos;
            auto val_result = json_parse_value(text, pos);
            if (!val_result.is_ok()) return val_result;
            obj->set_property(key, val_result.value());
            // skip whitespace
            while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t' ||
                                          text[pos] == '\n' || text[pos] == '\r')) ++pos;
            if (pos >= text.size()) break;
            if (text[pos] == '}') { ++pos; break; }
            if (text[pos] != ',') {
                pending_throw_ = make_error_value(NativeErrorType::kSyntaxError,
                                                  "JSON.parse: expected ',' or '}'");
                return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
            }
            ++pos;
        }
        return EvalResult::ok(Value::object(ObjectPtr(obj)));
    }

    pending_throw_ = make_error_value(NativeErrorType::kSyntaxError,
                                      "JSON.parse: unexpected token");
    return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
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
        return !v.sv().empty();
    case ValueKind::Object:
        return true;
    case ValueKind::Symbol:
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
    case ValueKind::Symbol:
        return EvalResult::err(Error(ErrorKind::Runtime, "TypeError: Cannot convert a Symbol value to a number"));
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
        // 大整数 < 10^21: 用 fixed 格式避免科学计数法
        if (n == std::floor(n) && std::abs(n) < 1e21) {
            char buf[64];
            int len = std::snprintf(buf, sizeof(buf), "%.0f", n);
            if (len > 0 && len < static_cast<int>(sizeof(buf))) {
                return std::string(buf, len);
            }
        }
        std::ostringstream oss;
        oss << n;
        return oss.str();
    }
    case ValueKind::String:
        return v.as_string();
    case ValueKind::Symbol:
        // Implicit ToString of Symbol is forbidden; callers that need explicit conversion
        // (e.g. String(sym)) must call symbol_to_string() directly.
        return "<symbol>";
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

std::string Interpreter::symbol_to_string(uint64_t id, const SymbolTable& table) {
    const std::string* desc = table.GetDescription(id);
    if (desc == nullptr) return "Symbol()";
    return "Symbol(" + *desc + ")";
}

// ---- Var hoisting ----

// 递归收集 PatternNode 中所有 IdentifierPattern 的名字
static void collect_pattern_names(const PatternNode& pat, std::vector<std::string>& out) {
    std::visit(overloaded{
        [&](const IdentifierPattern& ip) {
            out.push_back(ip.name);
        },
        [&](const ArrayPattern& ap) {
            for (const auto& elem_opt : ap.elements) {
                if (elem_opt.has_value()) {
                    collect_pattern_names(*elem_opt->pattern, out);
                }
            }
            if (ap.rest) collect_pattern_names(*ap.rest, out);
        },
        [&](const ObjectPattern& op) {
            for (const auto& prop : op.properties) {
                collect_pattern_names(*prop.value_pattern, out);
            }
            if (op.rest) collect_pattern_names(*op.rest, out);
        },
    }, pat.v);
}

void Interpreter::hoist_vars_stmt(const StmtNode& stmt, Environment& var_target) {
    if (std::holds_alternative<VariableDeclaration>(stmt.v)) {
        const auto& decl = std::get<VariableDeclaration>(stmt.v);
        if (decl.kind == VarKind::Var) {
            var_target.define_initialized(decl.name);
        } else {
            current_env_->define(decl.name, decl.kind);
        }
    } else if (std::holds_alternative<DestructuringDeclaration>(stmt.v)) {
        const auto& dd = std::get<DestructuringDeclaration>(stmt.v);
        if (dd.kind == VarKind::Var) {
            std::vector<std::string> names;
            collect_pattern_names(*dd.pattern, names);
            for (const auto& name : names) {
                var_target.define_initialized(name);
            }
        }
    } else if (std::holds_alternative<FunctionDeclaration>(stmt.v)) {
        const auto& fdecl = std::get<FunctionDeclaration>(stmt.v);
        var_target.define_function(fdecl.name);
    } else if (std::holds_alternative<AsyncFunctionDeclaration>(stmt.v)) {
        // P2-C: async function declarations are hoisted and immediately assigned,
        // mirroring the VM's behavior of emitting kMakeFunction+kSetVar at function entry.
        const auto& afdecl = std::get<AsyncFunctionDeclaration>(stmt.v);
        var_target.define_function(afdecl.name);
        Value async_fn_val;
        if (afdecl.is_generator) {
            async_fn_val = make_async_generator_value(afdecl.name, afdecl.params, afdecl.body, current_env_,
                                                       afdecl.rest_param);
        } else {
            async_fn_val = make_async_function_value(afdecl.name, afdecl.params, afdecl.body, current_env_,
                                                      afdecl.rest_param);
        }
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
    } else if (std::holds_alternative<ForInStatement>(stmt.v)) {
        const auto& for_in = std::get<ForInStatement>(stmt.v);
        if (for_in.has_decl && for_in.var_kind == VarKind::Var) {
            var_target.define_initialized(for_in.binding);
        }
        hoist_vars_stmt(*for_in.body, var_target);
    } else if (std::holds_alternative<ForOfStatement>(stmt.v)) {
        const auto& for_of = std::get<ForOfStatement>(stmt.v);
        if (for_of.has_decl && for_of.var_kind == VarKind::Var) {
            if (for_of.pattern_binding != nullptr) {
                std::vector<std::string> names;
                collect_pattern_names(*for_of.pattern_binding, names);
                for (const auto& name : names) {
                    var_target.define_initialized(name);
                }
            } else {
                var_target.define_initialized(for_of.binding);
            }
        }
        hoist_vars_stmt(*for_of.body, var_target);
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
    } else if (std::holds_alternative<SwitchStatement>(stmt.v)) {
        const auto& sw = std::get<SwitchStatement>(stmt.v);
        for (const auto& sc : sw.cases) {
            for (const auto& s : sc.consequent) {
                hoist_vars_stmt(*s, var_target);
            }
        }
    } else if (std::holds_alternative<WhileStatement>(stmt.v)) {
        hoist_vars_stmt(*std::get<WhileStatement>(stmt.v).body, var_target);
    } else if (std::holds_alternative<DoWhileStatement>(stmt.v)) {
        hoist_vars_stmt(*std::get<DoWhileStatement>(stmt.v).body, var_target);
    } else if (std::holds_alternative<BlockStatement>(stmt.v)) {
        hoist_vars(std::get<BlockStatement>(stmt.v).body, var_target);
    } else if (std::holds_alternative<IfStatement>(stmt.v)) {
        const auto& if_stmt = std::get<IfStatement>(stmt.v);
        hoist_vars_stmt(*if_stmt.consequent, var_target);
        if (if_stmt.alternate) hoist_vars_stmt(*if_stmt.alternate, var_target);
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
        add_obj(boolean_prototype_.get());
        add_obj(string_prototype_.get());
        add_obj(math_obj_.get());
        add_obj(number_prototype_.get());
        add_obj(object_constructor_.get());
        add_obj(number_constructor_.get());
        add_obj(boolean_constructor_.get());
        add_obj(string_constructor_.get());
        add_obj(regexp_prototype_.get());
        add_obj(regexp_constructor_.get());
        add_obj(symbol_prototype_.get());
        add_obj(symbol_constructor_.get());
        add_obj(generator_prototype_.get());
        add_obj(map_prototype_.get());
        add_obj(set_prototype_.get());
        add_obj(weakmap_prototype_.get());
        add_obj(weakset_prototype_.get());
        for (auto& ep : error_protos_) add_obj(ep.get());
        add_val(current_this_);
        if (pending_throw_.has_value()) add_val(*pending_throw_);
        // Include the result value so it is not swept
        if (final_result.is_ok()) add_val(final_result.value());
        // Include last expression value: may differ from final_result if re-eval updated final_result
        add_val(last);
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
    if (boolean_prototype_) boolean_prototype_->clear_function_properties();
    if (string_prototype_) string_prototype_->clear_function_properties();
    if (math_obj_) math_obj_->clear_function_properties();
    if (number_prototype_) number_prototype_->clear_function_properties();
    if (regexp_prototype_) regexp_prototype_->clear_function_properties();
    if (symbol_prototype_) symbol_prototype_->clear_function_properties();
    if (generator_prototype_) generator_prototype_->clear_function_properties();
    if (map_prototype_) map_prototype_->clear_function_properties();
    if (set_prototype_) set_prototype_->clear_function_properties();
    if (weakmap_prototype_) weakmap_prototype_->clear_function_properties();
    if (weakset_prototype_) weakset_prototype_->clear_function_properties();
    if (object_constructor_) object_constructor_->clear_own_properties();
    if (number_constructor_) number_constructor_->clear_own_properties();
    if (boolean_constructor_) boolean_constructor_->clear_own_properties();
    if (string_constructor_) string_constructor_->clear_own_properties();
    if (regexp_constructor_) regexp_constructor_->clear_own_properties();
    if (symbol_constructor_) symbol_constructor_->clear_own_properties();
    in_generator_resume_mode_ = false;
    pending_generator_resume_value_ = std::nullopt;
    pending_generator_yield_value_ = std::nullopt;

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
            [this](const DoWhileStatement& s) { return eval_do_while_stmt(s); },
            [this](const ReturnStatement& s) { return eval_return_stmt(s); },
            [this](const FunctionDeclaration& s) { return eval_function_decl(s); },
            [this](const AsyncFunctionDeclaration& s) { return eval_async_function_decl(s); },
            [this](const ThrowStatement& s) { return eval_throw_stmt(s); },
            [this](const TryStatement& s) { return eval_try_stmt(s); },
            [this](const BreakStatement& s) { return eval_break_stmt(s); },
            [this](const ContinueStatement& s) { return eval_continue_stmt(s); },
            [this](const LabeledStatement& s) { return eval_labeled_stmt(s); },
            [this](const ForStatement& s) { return eval_for_stmt(s); },
            [this](const ForInStatement& s) { return eval_for_in_stmt(s); },
            [this](const ForOfStatement& s) { return eval_for_of_stmt(s); },
            [this](const DestructuringDeclaration& s) { return eval_destructuring_decl(s); },
            [this](const ClassDeclaration& s) -> StmtResult {
                // 将 ClassDeclaration 转成 ClassExpression 求值，然后绑定名字
                ClassExpression ce;
                // 无法直接 move ClassDeclaration（const 引用），需要转换
                // 由于 ClassMethod 含 unique_ptr，只能手动构造等价 ClassExpression
                // 直接调用 eval_class_decl
                auto val = eval_class_decl(s);
                if (!val.is_ok()) return StmtResult::err(val.error());
                return StmtResult::ok(Completion::normal(Value::undefined()));
            },
            [this](const SwitchStatement& s) { return eval_switch_stmt(s); },
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

// Infer function/class name from variable assignment (ES2015+ name inference)
static void infer_function_name_if_anon(Value& val, const std::string& name) {
    if (!val.is_object()) return;
    auto* raw = val.as_object_raw();
    if (!raw || raw->object_kind() != ObjectKind::kFunction) return;
    auto* fn = static_cast<JSFunction*>(raw);
    // If function has no name or empty name, infer from variable
    Value existing = fn->get_property("name");
    if ((existing.is_string() && existing.sv().empty()) || existing.is_undefined()) {
        fn->set_name(name);
        // Also update own_properties_["name"] since it takes precedence in get_property
        auto it = fn->own_properties().find("name");
        if (it != fn->own_properties().end() && it->second.is_string() && it->second.sv().empty()) {
            fn->set_property("name", Value::string(name));
        }
    }
}

StmtResult Interpreter::eval_var_decl(const VariableDeclaration& decl) {
    if (decl.kind == VarKind::Var) {
        // var: binding already hoisted; just assign if there is an initializer
        if (decl.init.has_value()) {
            EvalResult init_result = eval_expr(decl.init.value());
            if (!init_result.is_ok()) {
                return StmtResult::err(init_result.error());
            }
            infer_function_name_if_anon(init_result.value(), decl.name);
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
            EvalResult init_result = eval_expr(decl.init.value());
            if (!init_result.is_ok()) {
                return StmtResult::err(init_result.error());
            }
            infer_function_name_if_anon(init_result.value(), decl.name);
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

// ============================================================
// Destructuring: bind_pattern + eval_destructuring_decl
// ============================================================

// bind a single identifier in the current environment
static StmtResult bind_identifier(const std::string& name, Value val, VarKind kind, bool is_assign,
                                   Environment* env, Environment* var_env) {
    if (is_assign || kind == VarKind::Var) {
        auto r = env->set(name, val);
        if (!r.is_ok()) return StmtResult::err(r.error());
    } else {
        // let / const: define then initialize
        if (env->find_local(name) == nullptr) {
            env->define(name, kind);
        }
        auto r = env->initialize(name, val);
        if (!r.is_ok()) return StmtResult::err(r.error());
    }
    return StmtResult::ok(Completion::normal(Value::undefined()));
}

StmtResult Interpreter::bind_pattern(const PatternNode& pattern, Value rhs,
                                      VarKind kind, bool is_assign) {
    return std::visit(overloaded{
        [&](const IdentifierPattern& ip) -> StmtResult {
            return bind_identifier(ip.name, rhs, kind, is_assign, current_env_.get(), var_env_.get());
        },
        [&](const ObjectPattern& op) -> StmtResult {
            // null/undefined → TypeError
            if (rhs.is_null() || rhs.is_undefined()) {
                pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                    "Cannot destructure property of null/undefined");
                return StmtResult::ok(Completion::throw_(*pending_throw_));
            }
            // Collect named keys for rest computation
            std::vector<std::string> named_keys;
            for (const auto& prop : op.properties) {
                // 计算键：求值 key_expr 获得实际键
                std::string resolved_key;
                bool key_is_symbol = false;
                uint64_t sym_id = 0;
                if (prop.computed && prop.key_expr != nullptr) {
                    auto key_r = eval_expr(*prop.key_expr);
                    if (!key_r.is_ok()) return StmtResult::err(key_r.error());
                    Value key_val = key_r.value();
                    if (key_val.is_symbol()) {
                        key_is_symbol = true;
                        sym_id = key_val.as_symbol_id();
                    } else {
                        resolved_key = to_string_val(key_val);
                    }
                } else {
                    resolved_key = prop.key;
                }
                if (!key_is_symbol) named_keys.push_back(resolved_key);

                // Get value from rhs
                Value val = Value::undefined();
                if (rhs.is_object()) {
                    RcObject* raw = rhs.as_object_raw();
                    ObjectKind k = raw->object_kind();
                    if (k == ObjectKind::kOrdinary || k == ObjectKind::kArray ||
                        k == ObjectKind::kRegExp || k == ObjectKind::kStringObject ||
                        k == ObjectKind::kBooleanObject) {
                        auto* obj = static_cast<JSObject*>(raw);
                        if (key_is_symbol) {
                            const JSObject::SymbolPropertyEntry* sym_entry = obj->find_symbol_entry(sym_id);
                            if (sym_entry != nullptr) {
                                if (sym_entry->is_accessor) {
                                    if (!sym_entry->getter.is_undefined() && !sym_entry->getter.is_null()) {
                                        Value getter_copy = sym_entry->getter;
                                        auto gres = call_function_val(getter_copy, rhs, {});
                                        if (!gres.is_ok()) return StmtResult::err(gres.error());
                                        val = gres.value();
                                    }
                                } else {
                                    val = sym_entry->value;
                                }
                            }
                        } else {
                            val = obj->get_property(resolved_key);
                        }
                    }
                }
                // Default value: if val === undefined
                if (val.is_undefined() && prop.default_value.has_value()) {
                    auto dv = eval_expr(**prop.default_value);
                    if (!dv.is_ok()) return StmtResult::err(dv.error());
                    val = dv.value();
                }
                // Recurse
                auto r = bind_pattern(*prop.value_pattern, std::move(val), kind, is_assign);
                if (!r.is_ok()) return r;
                if (r.completion().is_throw()) return r;
            }
            // rest
            if (op.rest != nullptr) {
                auto rest_obj = RcPtr<JSObject>::make(ObjectKind::kOrdinary);
                gc_heap_.Register(rest_obj.get());
                rest_obj->set_proto(object_prototype_);
                if (rhs.is_object()) {
                    RcObject* raw = rhs.as_object_raw();
                    ObjectKind k = raw->object_kind();
                    if (k == ObjectKind::kOrdinary || k == ObjectKind::kArray ||
                        k == ObjectKind::kRegExp || k == ObjectKind::kStringObject ||
                        k == ObjectKind::kBooleanObject) {
                        auto* obj = static_cast<JSObject*>(raw);
                        for (const auto& key : obj->own_enumerable_string_keys()) {
                            bool excluded = false;
                            for (const auto& nk : named_keys) {
                                if (nk == key) { excluded = true; break; }
                            }
                            if (!excluded) {
                                rest_obj->set_property(key, obj->get_property(key));
                            }
                        }
                    }
                }
                auto r = bind_pattern(*op.rest, Value::object(ObjectPtr(rest_obj)), kind, is_assign);
                if (!r.is_ok()) return r;
                if (r.completion().is_throw()) return r;
            }
            return StmtResult::ok(Completion::normal(Value::undefined()));
        },
        [&](const ArrayPattern& ap) -> StmtResult {
            // Get iterator from rhs
            if (rhs.is_null() || rhs.is_undefined()) {
                pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                    rhs.is_null() ? "null is not iterable" : "undefined is not iterable");
                return StmtResult::ok(Completion::throw_(*pending_throw_));
            }

            // Collect values from iterator
            std::vector<Value> values;
            if (!spread_into(rhs, values)) {
                Value thrown = pending_throw_.has_value() ? std::move(*pending_throw_)
                                                          : make_error_value(NativeErrorType::kTypeError,
                                                                             "not iterable");
                pending_throw_ = std::nullopt;
                return StmtResult::ok(Completion::throw_(std::move(thrown)));
            }
            size_t val_idx = 0;
            // Bind regular elements
            for (const auto& elem_opt : ap.elements) {
                if (!elem_opt.has_value()) {
                    // elision: skip
                    ++val_idx;
                    continue;
                }
                const auto& elem = *elem_opt;
                Value val = (val_idx < values.size()) ? values[val_idx] : Value::undefined();
                ++val_idx;
                // Default value
                if (val.is_undefined() && elem.default_value.has_value()) {
                    auto dv = eval_expr(**elem.default_value);
                    if (!dv.is_ok()) return StmtResult::err(dv.error());
                    val = dv.value();
                }
                auto r = bind_pattern(*elem.pattern, std::move(val), kind, is_assign);
                if (!r.is_ok()) return r;
                if (r.completion().is_throw()) return r;
            }
            // rest
            if (ap.rest != nullptr) {
                auto rest_arr = RcPtr<JSObject>::make(ObjectKind::kArray);
                gc_heap_.Register(rest_arr.get());
                rest_arr->set_proto(array_prototype_);
                for (size_t i = val_idx; i < values.size(); ++i) {
                    rest_arr->elements_[rest_arr->array_length_++] = values[i];
                }
                auto r = bind_pattern(*ap.rest, Value::object(ObjectPtr(rest_arr)), kind, is_assign);
                if (!r.is_ok()) return r;
                if (r.completion().is_throw()) return r;
            }
            return StmtResult::ok(Completion::normal(Value::undefined()));
        },
    }, pattern.v);
}

StmtResult Interpreter::eval_destructuring_decl(const DestructuringDeclaration& decl) {
    Value rhs;
    if (decl.init) {
        auto r = eval_expr(*decl.init);
        if (!r.is_ok()) return StmtResult::err(r.error());
        rhs = r.value();
    } else {
        rhs = Value::undefined();
    }
    // M2: for let/const, pre-declare all pattern names as TDZ before any binding,
    // so default value expressions can see sibling names already in scope.
    if (decl.kind == VarKind::Let || decl.kind == VarKind::Const) {
        std::vector<std::string> names;
        collect_pattern_names(*decl.pattern, names);
        for (const auto& name : names) {
            if (current_env_->find_local(name) == nullptr) {
                current_env_->define(name, decl.kind);
            }
        }
    }
    return bind_pattern(*decl.pattern, std::move(rhs), decl.kind, false);
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

StmtResult Interpreter::eval_do_while_stmt(const DoWhileStatement& stmt,
                                            std::optional<std::string> label) {
    do {
        auto body_result = eval_stmt(*stmt.body);
        if (!body_result.is_ok()) {
            return body_result;
        }
        const Completion& c = body_result.completion();
        if (c.is_break()) {
            if (!c.target.has_value() || c.target == label) {
                return StmtResult::ok(Completion::normal(Value::undefined()));
            }
            return body_result;
        }
        if (c.is_continue()) {
            if (!c.target.has_value() || c.target == label) {
                // continue goes to test evaluation
            } else {
                return body_result;
            }
        }
        if (c.is_return() || c.is_throw()) {
            return body_result;
        }
        auto test_result = eval_expr(stmt.test);
        if (!test_result.is_ok()) {
            return StmtResult::err(test_result.error());
        }
        if (!to_boolean(test_result.value())) {
            break;
        }
    } while (true);
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
            [this](const UpdateExpression& e) { return eval_update_expr(e); },
            [this](const AsyncFunctionExpression& e) { return eval_async_function_expr(e); },
            [this](const MetaProperty& e) {
                // new.target meta-property
                if (e.kind == MetaPropertyKind::kNewTarget) {
                    return EvalResult::ok(current_new_target_);
                }
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
            [this](const RegexLiteral& e) { return eval_regex_literal(e); },
            [this](const TemplateLiteral& e) { return eval_template_literal(e); },
            [this](const ArrowFunctionExpression& e) { return eval_arrow_function_expr(e); },
            [this](const ConditionalExpression& e) { return eval_conditional_expr(e); },
            [this](const SpreadElement& /*e*/) {
                pending_throw_ = make_error_value(NativeErrorType::kSyntaxError,
                    "invalid use of spread element");
                return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
            },
            [this](const DestructuringAssignmentExpression& e) -> EvalResult {
                auto rhs_r = eval_expr(*e.value);
                if (!rhs_r.is_ok()) return rhs_r;
                Value rhs = rhs_r.value();
                // Destructuring assignment: use is_assign=true
                auto r = bind_pattern(*e.pattern, rhs, VarKind::Var, true);
                if (!r.is_ok()) return EvalResult::err(r.error());
                if (r.completion().is_throw()) {
                    pending_throw_ = r.completion().value;
                    return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
                }
                // Expression result is the rhs value
                return EvalResult::ok(rhs);
            },
            [this](const OptionalChainExpression& e) { return eval_optional_chain(e); },
            [this](const YieldExpression& e) { return eval_yield_expr(e); },
            [this](const ClassExpression& e) { return eval_class_expr(e); },
            [this](const SuperCallExpression& e) { return eval_super_call(e); },
            [this](const SuperMemberExpression& e) { return eval_super_member(e); },
            [this](const TaggedTemplateExpression& e) { return eval_tagged_template_expr(e); },
            [this](const PrivateMemberExpression& pme) -> EvalResult {
                auto obj_r = eval_expr(*pme.object);
                if (!obj_r.is_ok()) return obj_r;
                Value obj_val = obj_r.value();
                if (!obj_val.is_object()) {
                    pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                        "Cannot read private member from non-object");
                    return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
                }
                // Look up symbol id in current function's private_fields_ mapping
                uint64_t sym_id = 0;
                if (current_function_) {
                    sym_id = current_function_->get_private_field_sym(pme.field_name);
                }
                if (sym_id == 0) {
                    pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                        "Cannot read private member '" + pme.field_name + "' of object whose class did not declare it");
                    return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
                }
                RcObject* raw = obj_val.as_object_raw();
                // JSFunction stores static private fields as string properties with #name key
                if (raw->object_kind() == ObjectKind::kFunction) {
                    auto* fn = static_cast<JSFunction*>(raw);
                    return EvalResult::ok(fn->get_property(pme.field_name));
                }
                auto* obj = static_cast<JSObject*>(raw);
                const JSObject::SymbolPropertyEntry* entry = obj->find_symbol_entry(sym_id);
                if (!entry) {
                    pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                        "Cannot read private member '" + pme.field_name + "' of object whose class did not declare it");
                    return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
                }
                if (entry->is_accessor) {
                    if (entry->getter.is_undefined() || entry->getter.is_null()) {
                        return EvalResult::ok(Value::undefined());
                    }
                    Value getter_copy = entry->getter;
                    return call_function_val(getter_copy, obj_val, {});
                }
                return EvalResult::ok(entry->value);
            },
            [this](const PrivateInExpression& pie) -> EvalResult {
                auto obj_r = eval_expr(*pie.object);
                if (!obj_r.is_ok()) return obj_r;
                Value obj_val = obj_r.value();
                if (!obj_val.is_object()) {
                    pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                        "Right side of 'in' must be an object");
                    return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
                }
                uint64_t sym_id = 0;
                if (current_function_) {
                    sym_id = current_function_->get_private_field_sym(pie.field_name);
                }
                if (sym_id == 0) return EvalResult::ok(Value::boolean(false));
                RcObject* raw = obj_val.as_object_raw();
                if (raw->object_kind() == ObjectKind::kFunction) {
                    return EvalResult::ok(Value::boolean(false));
                }
                auto* obj = static_cast<JSObject*>(raw);
                return EvalResult::ok(Value::boolean(obj->find_symbol_entry(sym_id) != nullptr));
            },
        },
        expr.v);
}

bool Interpreter::spread_into(const Value& iterable, std::vector<Value>& out) {
    // Fast path: array
    if (iterable.is_object() && iterable.as_object_raw()->object_kind() == ObjectKind::kArray) {
        JSObject* arr = static_cast<JSObject*>(iterable.as_object_raw());
        uint32_t len = arr->array_length_;
        for (uint32_t i = 0; i < len; ++i) {
            auto it = arr->elements_.find(i);
            out.push_back(it != arr->elements_.end() ? it->second : Value::undefined());
        }
        return true;
    }

    // Fast path: string — iterate by UTF-8 code points
    if (iterable.is_string()) {
        std::string_view sv = iterable.sv();
        size_t pos = 0;
        while (pos < sv.size()) {
            unsigned char c0 = static_cast<unsigned char>(sv[pos]);
            size_t cp_bytes = (c0 < 0x80) ? 1 : (c0 < 0xE0) ? 2 : (c0 < 0xF0) ? 3 : 4;
            if (pos + cp_bytes > sv.size()) cp_bytes = sv.size() - pos;
            out.push_back(Value::string(std::string(sv.data() + pos, cp_bytes)));
            pos += cp_bytes;
        }
        return true;
    }

    // Fast path: ArrayIterator (native)
    if (iterable.is_object() && iterable.as_object_raw() &&
        iterable.as_object_raw()->object_kind() == ObjectKind::kArrayIterator) {
        auto* arr_it = static_cast<ArrayIterator*>(iterable.as_object_raw());
        if (!arr_it->done_ && arr_it->array_ref_.is_object() &&
            arr_it->array_ref_.as_object_raw() &&
            arr_it->array_ref_.as_object_raw()->object_kind() == ObjectKind::kArray) {
            auto* arr = static_cast<JSObject*>(arr_it->array_ref_.as_object_raw());
            for (; arr_it->index_ < arr->array_length_; ++arr_it->index_) {
                auto it = arr->elements_.find(arr_it->index_);
                out.push_back(it != arr->elements_.end() ? it->second : Value::undefined());
            }
            arr_it->done_ = true;
        }
        return true;
    }

    // Fast path: StringIterator (native)
    if (iterable.is_object() && iterable.as_object_raw() &&
        iterable.as_object_raw()->object_kind() == ObjectKind::kStringIterator) {
        auto* str_it = static_cast<StringIterator*>(iterable.as_object_raw());
        if (!str_it->done_ && str_it->string_val_.is_string()) {
            std::string_view sv = str_it->string_val_.sv();
            size_t pos = str_it->byte_pos_;
            while (pos < sv.size()) {
                unsigned char c0 = static_cast<unsigned char>(sv[pos]);
                size_t cp_bytes = (c0 < 0x80) ? 1 : (c0 < 0xE0) ? 2 : (c0 < 0xF0) ? 3 : 4;
                if (pos + cp_bytes > sv.size()) cp_bytes = sv.size() - pos;
                out.push_back(Value::string(std::string(sv.data() + pos, cp_bytes)));
                pos += cp_bytes;
            }
            str_it->byte_pos_ = static_cast<uint32_t>(sv.size());
            str_it->done_ = true;
        }
        return true;
    }

    // Fast path: ForOfIterator (native)
    if (iterable.is_object() && iterable.as_object_raw() &&
        iterable.as_object_raw()->object_kind() == ObjectKind::kForOfIterator) {
        auto* for_it = static_cast<ForOfIterator*>(iterable.as_object_raw());
        while (!for_it->done_) {
            auto next_r = call_function_val(for_it->next_method_, for_it->iterator_, std::span<Value>());
            if (!next_r.is_ok()) return false;
            Value result = next_r.value();
            if (!result.is_object()) {
                pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                    "iterator result must be an object");
                return false;
            }
            ObjectKind rk = result.as_object_raw()->object_kind();
            Value done_val = Value::undefined();
            Value value = Value::undefined();
            if (rk == ObjectKind::kOrdinary || rk == ObjectKind::kArray) {
                auto* result_obj = static_cast<JSObject*>(result.as_object_raw());
                done_val = result_obj->get_property("done");
                value = result_obj->get_property("value");
            }
            if (to_boolean(done_val)) {
                for_it->done_ = true;
                break;
            }
            out.push_back(std::move(value));
        }
        return true;
    }

    // Generic path: Symbol.iterator
    if (!iterable.is_object()) {
        pending_throw_ = make_error_value(NativeErrorType::kTypeError, "value is not iterable");
        return false;
    }
    ObjectKind k = iterable.as_object_raw()->object_kind();
    JSObject* obj = nullptr;
    if (k == ObjectKind::kOrdinary || k == ObjectKind::kRegExp ||
        k == ObjectKind::kStringObject || k == ObjectKind::kBooleanObject ||
        k == ObjectKind::kGenerator ||
        k == ObjectKind::kMap || k == ObjectKind::kSet) {
        obj = static_cast<JSObject*>(iterable.as_object_raw());
    }
    if (!obj) {
        pending_throw_ = make_error_value(NativeErrorType::kTypeError, "value is not iterable");
        return false;
    }
    Value iter_method = obj->get_property_by_symbol(symbol_table_.well_known_iterator);
    if (iter_method.is_undefined() || iter_method.is_null()) {
        pending_throw_ = make_error_value(NativeErrorType::kTypeError, "value is not iterable");
        return false;
    }
    auto iter_r = call_function_val(iter_method, iterable, std::span<Value>());
    if (!iter_r.is_ok()) return false;
    Value iterator = iter_r.value();
    if (!iterator.is_object()) {
        pending_throw_ = make_error_value(NativeErrorType::kTypeError, "iterator must be an object");
        return false;
    }
    Value next_method = Value::undefined();
    ObjectKind ik = iterator.as_object_raw()->object_kind();
    if (ik == ObjectKind::kOrdinary || ik == ObjectKind::kArray ||
        ik == ObjectKind::kRegExp || ik == ObjectKind::kStringObject ||
        ik == ObjectKind::kBooleanObject || ik == ObjectKind::kGenerator ||
        ik == ObjectKind::kMap || ik == ObjectKind::kSet) {
        next_method = static_cast<JSObject*>(iterator.as_object_raw())->get_property("next");
    }
    while (true) {
        auto next_r = call_function_val(next_method, iterator, std::span<Value>());
        if (!next_r.is_ok()) return false;
        Value result = next_r.value();
        if (!result.is_object()) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "iterator result must be an object");
            return false;
        }
        Value done_val = Value::undefined();
        Value value = Value::undefined();
        ObjectKind rk = result.as_object_raw()->object_kind();
        if (rk == ObjectKind::kOrdinary || rk == ObjectKind::kArray) {
            auto* result_obj = static_cast<JSObject*>(result.as_object_raw());
            done_val = result_obj->get_property("done");
            value = result_obj->get_property("value");
        }
        if (to_boolean(done_val)) break;
        out.push_back(std::move(value));
    }
    return true;
}

EvalResult Interpreter::eval_array_expr(const ArrayExpression& expr) {
    auto arr = RcPtr<JSObject>::make(ObjectKind::kArray);
    gc_heap_.Register(arr.get());
    arr->set_proto(array_prototype_);
    for (const auto& elem_opt : expr.elements) {
        if (elem_opt.has_value()) {
            if (std::holds_alternative<SpreadElement>((*elem_opt)->v)) {
                const auto& sp = std::get<SpreadElement>((*elem_opt)->v);
                auto iterable_res = eval_expr(*sp.argument);
                if (!iterable_res.is_ok()) return iterable_res;
                std::vector<Value> spread_vals;
                if (!spread_into(iterable_res.value(), spread_vals)) {
                    return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
                }
                for (auto& sv : spread_vals) {
                    arr->elements_[arr->array_length_++] = std::move(sv);
                }
                continue;
            }
            auto v = eval_expr(**elem_opt);
            if (!v.is_ok()) return v;
            arr->elements_[arr->array_length_] = v.value();
        }
        // hole: increment length but do not write to elements_ (true sparse hole)
        arr->array_length_++;
    }
    return EvalResult::ok(Value::object(ObjectPtr(arr)));
}

// ---- RegExp runtime ----

EvalResult Interpreter::make_regexp(const std::string& pattern, const std::string& flags) {
    // Validate flags: only g/i/m/s/u/y, no duplicates
    bool seen[128] = {};
    for (char c : flags) {
        if (c != 'g' && c != 'i' && c != 'm' && c != 's' && c != 'u' && c != 'y') {
            pending_throw_ = make_error_value(NativeErrorType::kSyntaxError,
                std::string("Invalid regular expression flags: ") + c);
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        if (seen[static_cast<unsigned char>(c)]) {
            pending_throw_ = make_error_value(NativeErrorType::kSyntaxError,
                std::string("Duplicate regular expression flag: ") + c);
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        seen[static_cast<unsigned char>(c)] = true;
    }

    auto rx = RcPtr<JSRegExp>::make(pattern, flags);
    if (!rx->is_valid_) {
        pending_throw_ = make_error_value(NativeErrorType::kSyntaxError,
            "Invalid regular expression: " + pattern);
        return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
    }
    gc_heap_.Register(rx.get());
    if (regexp_prototype_) rx->set_proto(regexp_prototype_);
    return EvalResult::ok(Value::object(ObjectPtr(rx)));
}

EvalResult Interpreter::eval_regex_literal(const RegexLiteral& expr) {
    return make_regexp(expr.pattern, expr.flags);
}

EvalResult Interpreter::regexp_exec(JSRegExp* rx, const std::string& input) {
    if (!rx->is_valid_) {
        pending_throw_ = make_error_value(NativeErrorType::kSyntaxError,
            "Invalid regular expression");
        return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
    }

    size_t start_pos = (rx->global_ || rx->sticky_) ? rx->last_index_ : 0;
    if (start_pos > input.size()) {
        if (rx->global_ || rx->sticky_) rx->last_index_ = 0;
        return EvalResult::ok(Value::null());
    }

    std::smatch sm;
    auto search_begin = input.cbegin() + static_cast<std::ptrdiff_t>(start_pos);
    std::regex_constants::match_flag_type mflags = std::regex_constants::match_default;
    // match_not_bol: only when non-multiline and not at string start,
    // to prevent ^ from matching the substring start position.
    if (!rx->multiline_ && start_pos > 0) mflags |= std::regex_constants::match_not_bol;
    // For sticky: only match at exact position
    if (rx->sticky_) mflags |= std::regex_constants::match_continuous;

    bool found = std::regex_search(search_begin, input.cend(), sm, rx->compiled_, mflags);
    if (!found) {
        if (rx->global_ || rx->sticky_) rx->last_index_ = 0;
        return EvalResult::ok(Value::null());
    }

    size_t match_start = start_pos + static_cast<size_t>(sm.position(0));
    size_t match_end = match_start + static_cast<size_t>(sm.length(0));

    if (rx->global_ || rx->sticky_) {
        rx->last_index_ = static_cast<uint32_t>(match_end);
    }

    // Build result array
    auto result = RcPtr<JSObject>::make(ObjectKind::kArray);
    gc_heap_.Register(result.get());
    if (array_prototype_) result->set_proto(array_prototype_);

    // elements_[0] = full match, elements_[i] = capture group i
    for (size_t i = 0; i < sm.size(); ++i) {
        if (i > 0 && !sm[i].matched) {
            // Unmatched capture group → undefined (don't write, stays as hole/undefined)
        } else {
            result->elements_[static_cast<uint32_t>(i)] = Value::string(sm[i].str());
        }
        result->array_length_++;
    }
    result->set_property("index", Value::number(static_cast<double>(match_start)));
    result->set_property("input", Value::string(input));
    result->set_property("groups", Value::undefined());

    return EvalResult::ok(Value::object(ObjectPtr(result)));
}

EvalResult Interpreter::eval_identifier(const Identifier& expr) {
    if (expr.name == "undefined") {
        return EvalResult::ok(Value::undefined());
    }
    if (expr.name == "this") {
        // Derived constructor: 'this' before super() is ReferenceError
        if (current_function_ && current_function_->is_derived_ctor() &&
            !derived_this_initialized_) {
            pending_throw_ = make_error_value(NativeErrorType::kReferenceError,
                "Must call super constructor in derived class before accessing 'this'");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
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
            // "this" is stored in current_this_, not in env — fall through to eval_expr
            if (id.name != "this") {
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
        case ValueKind::Symbol:
            return EvalResult::ok(Value::string("symbol"));
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

    if (expr.op == UnaryOp::Delete) {
        // delete MemberExpression (dot access)
        if (std::holds_alternative<MemberExpression>(expr.operand->v)) {
            const auto& mem = std::get<MemberExpression>(expr.operand->v);
            auto obj_result = eval_expr(*mem.object);
            if (!obj_result.is_ok()) return obj_result;
            const Value& obj_val = obj_result.value();
            if (!obj_val.is_object()) {
                return EvalResult::ok(Value::boolean(true));
            }
            RcObject* raw = obj_val.as_object_raw();
            if (!raw || (raw->object_kind() != ObjectKind::kOrdinary &&
                         raw->object_kind() != ObjectKind::kArray)) {
                return EvalResult::ok(Value::boolean(true));
            }
            auto* obj = static_cast<JSObject*>(raw);
            std::string key;
            if (mem.computed) {
                auto key_result = eval_expr(*mem.property);
                if (!key_result.is_ok()) return key_result;
                key = to_string_val(key_result.value());
            } else {
                // non-computed: property is a StringLiteral with the prop name
                const auto& prop_lit = std::get<StringLiteral>(mem.property->v);
                key = prop_lit.value;
            }
            return EvalResult::ok(Value::boolean(obj->delete_property(key)));
        }
        // delete Identifier
        if (std::holds_alternative<Identifier>(expr.operand->v)) {
            const auto& id = std::get<Identifier>(expr.operand->v);
            // TODO: strict mode Early Error (SyntaxError for delete of unqualified identifier)
            bool deleted = current_env_->delete_binding(id.name);
            return EvalResult::ok(Value::boolean(deleted));
        }
        // delete OptionalChainExpression: short-circuit on null/undefined base, then delete last member
        if (std::holds_alternative<OptionalChainExpression>(expr.operand->v)) {
            const auto& oc = std::get<OptionalChainExpression>(expr.operand->v);
            auto base_r = eval_expr(*oc.base);
            if (!base_r.is_ok()) return base_r;
            if (base_r.value().is_null() || base_r.value().is_undefined()) {
                return EvalResult::ok(Value::boolean(true));
            }
            if (oc.links.empty()) {
                return EvalResult::ok(Value::boolean(true));
            }
            // Evaluate all links except the last to get the receiver
            Value current = base_r.value();
            Value receiver = Value::undefined();
            bool prev_was_member = false;
            for (size_t i = 0; i + 1 < oc.links.size(); ++i) {
                const auto& lnk = oc.links[i];
                bool optional = std::visit([](const auto& l) { return l.optional; }, lnk);
                if (optional && (current.is_null() || current.is_undefined())) {
                    return EvalResult::ok(Value::boolean(true));
                }
                if (const auto* p = std::get_if<OptionalChainExpression::PropLink>(&lnk)) {
                    receiver = current;
                    auto r = eval_get_property_of(current, Value::string(p->name));
                    if (!r.is_ok()) return r;
                    current = r.value();
                    prev_was_member = true;
                } else if (const auto* e2 = std::get_if<OptionalChainExpression::ElemLink>(&lnk)) {
                    receiver = current;
                    auto key_r = eval_expr(*e2->key);
                    if (!key_r.is_ok()) return key_r;
                    auto r = eval_get_property_of(current, key_r.value());
                    if (!r.is_ok()) return r;
                    current = r.value();
                    prev_was_member = true;
                } else if (const auto* c = std::get_if<OptionalChainExpression::CallLink>(&lnk)) {
                    std::vector<Value> call_args;
                    for (const auto& arg : c->args) {
                        auto arg_r = eval_expr(*arg);
                        if (!arg_r.is_ok()) return arg_r;
                        call_args.push_back(arg_r.value());
                    }
                    Value this_v = prev_was_member ? receiver : Value::undefined();
                    auto r = call_function_val(current, this_v, call_args);
                    if (!r.is_ok()) return r;
                    current = r.value();
                    receiver = Value::undefined();
                    prev_was_member = false;
                }
            }
            // Handle the last link
            const auto& last = oc.links.back();
            bool opt_last = std::visit([](const auto& l) { return l.optional; }, last);
            if (opt_last && (current.is_null() || current.is_undefined())) {
                return EvalResult::ok(Value::boolean(true));
            }
            if (const auto* p = std::get_if<OptionalChainExpression::PropLink>(&last)) {
                if (!current.is_object()) return EvalResult::ok(Value::boolean(true));
                RcObject* raw = current.as_object_raw();
                if (!raw || (raw->object_kind() != ObjectKind::kOrdinary &&
                             raw->object_kind() != ObjectKind::kArray)) {
                    return EvalResult::ok(Value::boolean(true));
                }
                return EvalResult::ok(Value::boolean(static_cast<JSObject*>(raw)->delete_property(p->name)));
            } else if (const auto* e2 = std::get_if<OptionalChainExpression::ElemLink>(&last)) {
                auto key_r = eval_expr(*e2->key);
                if (!key_r.is_ok()) return key_r;
                if (!current.is_object()) return EvalResult::ok(Value::boolean(true));
                RcObject* raw = current.as_object_raw();
                if (!raw || (raw->object_kind() != ObjectKind::kOrdinary &&
                             raw->object_kind() != ObjectKind::kArray)) {
                    return EvalResult::ok(Value::boolean(true));
                }
                std::string k = to_string_val(key_r.value());
                return EvalResult::ok(Value::boolean(static_cast<JSObject*>(raw)->delete_property(k)));
            } else {
                // CallLink as last: eval for side effects, return true
                const auto* c = std::get_if<OptionalChainExpression::CallLink>(&last);
                std::vector<Value> call_args;
                for (const auto& arg : c->args) {
                    auto arg_r = eval_expr(*arg);
                    if (!arg_r.is_ok()) return arg_r;
                    call_args.push_back(arg_r.value());
                }
                Value this_v = prev_was_member ? receiver : Value::undefined();
                auto r = call_function_val(current, this_v, call_args);
                if (!r.is_ok()) return r;
                return EvalResult::ok(Value::boolean(true));
            }
        }
        // delete <other expr> — eval for side effects, return true
        auto operand_result2 = eval_expr(*expr.operand);
        if (!operand_result2.is_ok()) return operand_result2;
        return EvalResult::ok(Value::boolean(true));
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
        // Check [Symbol.toPrimitive] with "number" hint for objects
        if (val.is_object()) {
            RcObject* raw = val.as_object_raw();
            if (raw && raw->object_kind() != ObjectKind::kFunction) {
                auto* obj = static_cast<JSObject*>(raw);
                const JSObject::SymbolPropertyEntry* entry =
                    obj->find_symbol_entry(symbol_table_.well_known_to_primitive);
                if (entry && !entry->value.is_undefined()) {
                    Value hint = Value::string("number");
                    auto result = call_function_val(entry->value, val,
                                                    std::span<Value>(&hint, 1));
                    if (!result.is_ok()) return result;
                    return to_number(result.value());
                }
            }
        }
        return to_number(val);
    }
    case UnaryOp::Bang:
        return EvalResult::ok(Value::boolean(!to_boolean(val)));
    case UnaryOp::BitNot: {
        auto num_result = to_number(val);
        if (!num_result.is_ok()) {
            return num_result;
        }
        return EvalResult::ok(Value::number(static_cast<double>(~to_int32_bits(num_result.value().as_number()))));
    }
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
    case ValueKind::Symbol:
        return a.as_symbol_id() == b.as_symbol_id();
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
        // Symbol on either side → TypeError (implicit ToString is forbidden)
        if (lv.is_symbol() || rv.is_symbol()) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "Cannot convert a Symbol value to a string");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
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
    case BinaryOp::Pow: {
        auto ln = to_number(lv);
        if (!ln.is_ok()) {
            return ln;
        }
        auto rn = to_number(rv);
        if (!rn.is_ok()) {
            return rn;
        }
        return EvalResult::ok(Value::number(std::pow(ln.value().as_number(), rn.value().as_number())));
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
        // Check [Symbol.hasInstance] on the constructor
        auto* ctor_fn = static_cast<JSFunction*>(ctor_raw);
        {
            std::string sym_key = "__pfsym_" +
                std::to_string(symbol_table_.well_known_has_instance) + "__";
            Value has_inst_val = ctor_fn->get_property(sym_key);
            if (!has_inst_val.is_undefined()) {
                Value lv_copy = lv;
                auto result = call_function_val(has_inst_val, rv,
                                               std::span<Value>(&lv_copy, 1));
                if (!result.is_ok()) return result;
                return EvalResult::ok(Value::boolean(to_boolean(result.value())));
            }
        }
        const RcPtr<JSObject>& ctor_proto = ctor_fn->prototype_obj();
        if (!ctor_proto) {
            return EvalResult::ok(Value::boolean(false));
        }
        // Walk the prototype chain of lv
        RcObject* cur_raw = lv.as_object_raw();
        bool found = false;
        while (cur_raw) {
            ObjectKind k = cur_raw->object_kind();
            if (k != ObjectKind::kOrdinary && k != ObjectKind::kArray &&
                k != ObjectKind::kRegExp && k != ObjectKind::kStringObject &&
                k != ObjectKind::kBooleanObject) break;
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
    case BinaryOp::In: {
        if (!rv.is_object()) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "Right-hand side of 'in' must be an object");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        RcObject* raw = rv.as_object_raw();
        if (!raw) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "Right-hand side of 'in' must be an object");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        bool found;
        if (raw->object_kind() == ObjectKind::kFunction) {
            auto* fn = static_cast<JSFunction*>(raw);
            found = !lv.is_symbol() && fn->has_property(to_string_val(lv));
        } else {
            auto* obj = static_cast<JSObject*>(raw);
            if (lv.is_symbol()) {
                found = obj->has_property_by_symbol(lv.as_symbol_id());
            } else {
                found = obj->has_property(to_string_val(lv));
            }
        }
        return EvalResult::ok(Value::boolean(found));
    }
    case BinaryOp::BitAnd: {
        auto ln = to_number(lv);
        if (!ln.is_ok()) return ln;
        auto rn = to_number(rv);
        if (!rn.is_ok()) return rn;
        int32_t result = to_int32_bits(ln.value().as_number()) & to_int32_bits(rn.value().as_number());
        return EvalResult::ok(Value::number(static_cast<double>(result)));
    }
    case BinaryOp::BitOr: {
        auto ln = to_number(lv);
        if (!ln.is_ok()) return ln;
        auto rn = to_number(rv);
        if (!rn.is_ok()) return rn;
        int32_t result = to_int32_bits(ln.value().as_number()) | to_int32_bits(rn.value().as_number());
        return EvalResult::ok(Value::number(static_cast<double>(result)));
    }
    case BinaryOp::BitXor: {
        auto ln = to_number(lv);
        if (!ln.is_ok()) return ln;
        auto rn = to_number(rv);
        if (!rn.is_ok()) return rn;
        int32_t result = to_int32_bits(ln.value().as_number()) ^ to_int32_bits(rn.value().as_number());
        return EvalResult::ok(Value::number(static_cast<double>(result)));
    }
    case BinaryOp::Shl: {
        auto ln = to_number(lv);
        if (!ln.is_ok()) return ln;
        auto rn = to_number(rv);
        if (!rn.is_ok()) return rn;
        uint32_t shift = to_uint32_bits(rn.value().as_number()) & 0x1F;
        int32_t result = to_int32_bits(ln.value().as_number()) << shift;
        return EvalResult::ok(Value::number(static_cast<double>(result)));
    }
    case BinaryOp::Sar: {
        auto ln = to_number(lv);
        if (!ln.is_ok()) return ln;
        auto rn = to_number(rv);
        if (!rn.is_ok()) return rn;
        uint32_t shift = to_uint32_bits(rn.value().as_number()) & 0x1F;
        int32_t result = to_int32_bits(ln.value().as_number()) >> shift;
        return EvalResult::ok(Value::number(static_cast<double>(result)));
    }
    case BinaryOp::Shr: {
        auto ln = to_number(lv);
        if (!ln.is_ok()) return ln;
        auto rn = to_number(rv);
        if (!rn.is_ok()) return rn;
        uint32_t shift = to_uint32_bits(rn.value().as_number()) & 0x1F;
        uint32_t result = to_uint32_bits(ln.value().as_number()) >> shift;
        return EvalResult::ok(Value::number(static_cast<double>(result)));
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
    case LogicalOp::Nullish:
        if (lv.is_null() || lv.is_undefined()) {
            return eval_expr(*expr.right);
        }
        return left_result;
    }
    return EvalResult::ok(Value::undefined());
}

EvalResult Interpreter::eval_conditional_expr(const ConditionalExpression& expr) {
    auto cond = eval_expr(*expr.condition);
    if (!cond.is_ok()) return cond;
    if (to_boolean(cond.value()))
        return eval_expr(*expr.consequent);
    else
        return eval_expr(*expr.alternate);
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

    // Logical assignment operators (short-circuit)
    if (expr.op == AssignOp::LogicalAndAssign || expr.op == AssignOp::LogicalOrAssign ||
        expr.op == AssignOp::NullishAssign) {
        auto lhs_result = current_env_->get(expr.target);
        if (!lhs_result.is_ok()) {
            const std::string& msg = lhs_result.error().message();
            NativeErrorType err_type = NativeErrorType::kReferenceError;
            if (msg.rfind("TypeError:", 0) == 0) err_type = NativeErrorType::kTypeError;
            pending_throw_ = make_error_value(err_type, strip_error_prefix(msg));
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        const Value& lv = lhs_result.value();
        bool should_assign = false;
        if (expr.op == AssignOp::LogicalAndAssign) {
            should_assign = to_boolean(lv);
        } else if (expr.op == AssignOp::LogicalOrAssign) {
            should_assign = !to_boolean(lv);
        } else {
            should_assign = lv.is_null() || lv.is_undefined();
        }
        if (!should_assign) {
            return lhs_result;
        }
        auto rhs = eval_expr(*expr.value);
        if (!rhs.is_ok()) return rhs;
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
        if (lv.is_symbol() || rv.is_symbol()) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "Cannot convert a Symbol value to a string");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
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
    case AssignOp::PowAssign: {
        auto ln = to_number(current_result.value());
        auto rn = to_number(rhs.value());
        if (!ln.is_ok()) {
            return ln;
        }
        if (!rn.is_ok()) {
            return rn;
        }
        new_val = Value::number(std::pow(ln.value().as_number(), rn.value().as_number()));
        break;
    }
    case AssignOp::BitAndAssign: {
        auto ln = to_number(current_result.value());
        auto rn = to_number(rhs.value());
        if (!ln.is_ok()) return ln;
        if (!rn.is_ok()) return rn;
        int32_t r = to_int32_bits(ln.value().as_number()) & to_int32_bits(rn.value().as_number());
        new_val = Value::number(static_cast<double>(r));
        break;
    }
    case AssignOp::BitOrAssign: {
        auto ln = to_number(current_result.value());
        auto rn = to_number(rhs.value());
        if (!ln.is_ok()) return ln;
        if (!rn.is_ok()) return rn;
        int32_t r = to_int32_bits(ln.value().as_number()) | to_int32_bits(rn.value().as_number());
        new_val = Value::number(static_cast<double>(r));
        break;
    }
    case AssignOp::BitXorAssign: {
        auto ln = to_number(current_result.value());
        auto rn = to_number(rhs.value());
        if (!ln.is_ok()) return ln;
        if (!rn.is_ok()) return rn;
        int32_t r = to_int32_bits(ln.value().as_number()) ^ to_int32_bits(rn.value().as_number());
        new_val = Value::number(static_cast<double>(r));
        break;
    }
    case AssignOp::ShlAssign: {
        auto ln = to_number(current_result.value());
        auto rn = to_number(rhs.value());
        if (!ln.is_ok()) return ln;
        if (!rn.is_ok()) return rn;
        uint32_t shift = to_uint32_bits(rn.value().as_number()) & 0x1F;
        int32_t r = to_int32_bits(ln.value().as_number()) << shift;
        new_val = Value::number(static_cast<double>(r));
        break;
    }
    case AssignOp::SarAssign: {
        auto ln = to_number(current_result.value());
        auto rn = to_number(rhs.value());
        if (!ln.is_ok()) return ln;
        if (!rn.is_ok()) return rn;
        uint32_t shift = to_uint32_bits(rn.value().as_number()) & 0x1F;
        int32_t r = to_int32_bits(ln.value().as_number()) >> shift;
        new_val = Value::number(static_cast<double>(r));
        break;
    }
    case AssignOp::ShrAssign: {
        auto ln = to_number(current_result.value());
        auto rn = to_number(rhs.value());
        if (!ln.is_ok()) return ln;
        if (!rn.is_ok()) return rn;
        uint32_t shift = to_uint32_bits(rn.value().as_number()) & 0x1F;
        uint32_t r = to_uint32_bits(ln.value().as_number()) >> shift;
        new_val = Value::number(static_cast<double>(r));
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
        if (std::holds_alternative<SpreadElement>(prop.value->v)) {
            pending_throw_ = make_error_value(NativeErrorType::kSyntaxError,
                "Object spread not supported");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        if (std::holds_alternative<AssignmentExpression>(prop.value->v)) {
            pending_throw_ = make_error_value(NativeErrorType::kSyntaxError,
                "Invalid shorthand property initializer");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }

        // 计算键：先求值 key_expr，ToPropertyKey，再处理属性
        if (prop.computed && prop.key_expr != nullptr) {
            auto key_r = eval_expr(*prop.key_expr);
            if (!key_r.is_ok()) return key_r;
            Value key_val = key_r.value();

            // Symbol 键透传，其他转 string
            bool key_is_symbol = key_val.is_symbol();
            uint64_t sym_id = 0;
            std::string str_key;
            if (key_is_symbol) {
                sym_id = key_val.as_symbol_id();
            } else {
                str_key = to_string_val(key_val);
            }

            if (prop.method_kind == MethodKind::kData) {
                auto val = eval_expr(*prop.value);
                if (!val.is_ok()) return val;
                if (key_is_symbol) obj->set_property_by_symbol(sym_id, val.value());
                else obj->set_property(str_key, val.value());
            } else if (prop.method_kind == MethodKind::kMethod ||
                       prop.method_kind == MethodKind::kGenerator ||
                       prop.method_kind == MethodKind::kAsyncMethod) {
                auto fn_res = eval_expr(*prop.value);
                if (!fn_res.is_ok()) return fn_res;
                Value fn_val = fn_res.value();
                if (fn_val.is_object() && fn_val.as_object_raw()->object_kind() == ObjectKind::kFunction) {
                    auto* fn = static_cast<JSFunction*>(fn_val.as_object_raw());
                    fn->set_is_method(true);
                    if (prop.method_kind == MethodKind::kGenerator) fn->set_is_generator(true);
                    if (key_is_symbol) {
                        const std::string* desc = symbol_table_.GetDescription(sym_id);
                        std::string name = "[" + (desc ? *desc : "") + "]";
                        fn->set_property("name", Value::string(name));
                    } else {
                        fn->set_property("name", Value::string(str_key));
                    }
                }
                if (key_is_symbol) obj->set_property_by_symbol(sym_id, fn_val);
                else obj->set_property(str_key, fn_val);
            } else if (prop.method_kind == MethodKind::kGetter) {
                auto fn_res = eval_expr(*prop.value);
                if (!fn_res.is_ok()) return fn_res;
                Value fn_val = fn_res.value();
                if (fn_val.is_object() && fn_val.as_object_raw()->object_kind() == ObjectKind::kFunction) {
                    auto* fn = static_cast<JSFunction*>(fn_val.as_object_raw());
                    fn->set_is_method(true);
                    fn->set_property("name", Value::string("get " + str_key));
                }
                PropDesc desc;
                desc.getter = fn_val;
                desc.enumerable = true;
                desc.configurable = true;
                EvalResult res = key_is_symbol
                    ? obj->define_property_by_symbol(sym_id, desc)
                    : obj->define_property(str_key, desc);
                if (!res.is_ok()) return res;
            } else if (prop.method_kind == MethodKind::kSetter) {
                auto fn_res = eval_expr(*prop.value);
                if (!fn_res.is_ok()) return fn_res;
                Value fn_val = fn_res.value();
                if (fn_val.is_object() && fn_val.as_object_raw()->object_kind() == ObjectKind::kFunction) {
                    auto* fn = static_cast<JSFunction*>(fn_val.as_object_raw());
                    fn->set_is_method(true);
                    fn->set_property("name", Value::string("set " + str_key));
                }
                PropDesc desc;
                desc.setter = fn_val;
                desc.enumerable = true;
                desc.configurable = true;
                EvalResult res = key_is_symbol
                    ? obj->define_property_by_symbol(sym_id, desc)
                    : obj->define_property(str_key, desc);
                if (!res.is_ok()) return res;
            }
            continue;
        }

        if (prop.method_kind == MethodKind::kData) {
            auto val = eval_expr(*prop.value);
            if (!val.is_ok()) return val;
            obj->set_property(prop.key, val.value());
        } else if (prop.method_kind == MethodKind::kMethod ||
                   prop.method_kind == MethodKind::kGenerator) {
            auto fn_res = eval_expr(*prop.value);
            if (!fn_res.is_ok()) return fn_res;
            Value fn_val = fn_res.value();
            if (fn_val.is_object() && fn_val.as_object_raw()->object_kind() == ObjectKind::kFunction) {
                auto* fn = static_cast<JSFunction*>(fn_val.as_object_raw());
                fn->set_is_method(true);
                if (prop.method_kind == MethodKind::kGenerator) fn->set_is_generator(true);
                fn->set_property("name", Value::string(prop.key));
            }
            obj->set_property(prop.key, fn_val);
        } else if (prop.method_kind == MethodKind::kAsyncMethod) {
            auto fn_res = eval_expr(*prop.value);
            if (!fn_res.is_ok()) return fn_res;
            Value fn_val = fn_res.value();
            if (fn_val.is_object() && fn_val.as_object_raw()->object_kind() == ObjectKind::kFunction) {
                auto* fn = static_cast<JSFunction*>(fn_val.as_object_raw());
                fn->set_is_method(true);
                fn->set_property("name", Value::string(prop.key));
            }
            obj->set_property(prop.key, fn_val);
        } else if (prop.method_kind == MethodKind::kGetter) {
            auto fn_res = eval_expr(*prop.value);
            if (!fn_res.is_ok()) return fn_res;
            Value fn_val = fn_res.value();
            if (fn_val.is_object() && fn_val.as_object_raw()->object_kind() == ObjectKind::kFunction) {
                auto* fn = static_cast<JSFunction*>(fn_val.as_object_raw());
                fn->set_is_method(true);
                fn->set_property("name", Value::string("get " + prop.key));
            }
            PropDesc desc;
            desc.getter = fn_val;
            desc.enumerable = true;
            desc.configurable = true;
            auto res = obj->define_property(prop.key, desc);
            if (!res.is_ok()) return res;
        } else if (prop.method_kind == MethodKind::kSetter) {
            auto fn_res = eval_expr(*prop.value);
            if (!fn_res.is_ok()) return fn_res;
            Value fn_val = fn_res.value();
            if (fn_val.is_object() && fn_val.as_object_raw()->object_kind() == ObjectKind::kFunction) {
                auto* fn = static_cast<JSFunction*>(fn_val.as_object_raw());
                fn->set_is_method(true);
                fn->set_property("name", Value::string("set " + prop.key));
            }
            PropDesc desc;
            desc.setter = fn_val;
            desc.enumerable = true;
            desc.configurable = true;
            auto res = obj->define_property(prop.key, desc);
            if (!res.is_ok()) return res;
        }
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

    // Symbol primitive: handle description + prototype methods
    if (obj_val.is_symbol()) {
        auto key_result = eval_expr(*expr.property);
        if (!key_result.is_ok()) return key_result;
        const Value& key_val = key_result.value();
        // Symbol key access on symbol — not supported; fall through to name-based lookup
        if (!key_val.is_symbol()) {
            std::string key = to_string_val(key_val);
            if (key == "description") {
                const std::string* desc = symbol_table_.GetDescription(obj_val.as_symbol_id());
                return EvalResult::ok(desc ? Value::string(*desc) : Value::undefined());
            }
            if (symbol_prototype_) return EvalResult::ok(symbol_prototype_->get_property(key));
        }
        return EvalResult::ok(Value::undefined());
    }

    // Number primitive: look up in number_prototype_
    if (obj_val.is_number()) {
        auto key_result = eval_expr(*expr.property);
        if (!key_result.is_ok()) return key_result;
        std::string key = to_string_val(key_result.value());
        if (number_prototype_) return EvalResult::ok(number_prototype_->get_property(key));
        return EvalResult::ok(Value::undefined());
    }

    // Boolean primitive: look up in boolean_prototype_
    if (obj_val.is_bool()) {
        auto key_result = eval_expr(*expr.property);
        if (!key_result.is_ok()) return key_result;
        std::string key = to_string_val(key_result.value());
        if (boolean_prototype_) return EvalResult::ok(boolean_prototype_->get_property(key));
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

    // Handle Symbol key on object
    if (key_result.value().is_symbol()) {
        if (!obj_val.is_object()) return EvalResult::ok(Value::undefined());
        RcObject* raw_for_sym = obj_val.as_object_raw();
        if (raw_for_sym->object_kind() == ObjectKind::kOrdinary ||
            raw_for_sym->object_kind() == ObjectKind::kArray ||
            raw_for_sym->object_kind() == ObjectKind::kGenerator ||
            raw_for_sym->object_kind() == ObjectKind::kMap ||
            raw_for_sym->object_kind() == ObjectKind::kSet ||
            raw_for_sym->object_kind() == ObjectKind::kWeakMap ||
            raw_for_sym->object_kind() == ObjectKind::kWeakSet) {
            auto* js_obj_sym = static_cast<JSObject*>(raw_for_sym);
            uint64_t sym_id = key_result.value().as_symbol_id();
            const JSObject::SymbolPropertyEntry* sym_entry = js_obj_sym->find_symbol_entry(sym_id);
            if (sym_entry != nullptr && sym_entry->is_accessor) {
                if (sym_entry->getter.is_undefined() || sym_entry->getter.is_null()) {
                    return EvalResult::ok(Value::undefined());
                }
                Value getter_copy = sym_entry->getter;
                return call_function_val(getter_copy, obj_val, {});
            }
            if (sym_entry != nullptr) return EvalResult::ok(sym_entry->value);
            return EvalResult::ok(Value::undefined());
        }
        return EvalResult::ok(Value::undefined());
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
    if (raw_obj->object_kind() == ObjectKind::kRegExp) {
        auto* rx = static_cast<JSRegExp*>(raw_obj);
        if (key == "source") {
            return EvalResult::ok(Value::string(rx->pattern_.empty() ? "(?:)" : rx->pattern_));
        }
        if (key == "flags") return EvalResult::ok(Value::string(rx->flags_str_));
        if (key == "global") return EvalResult::ok(Value::boolean(rx->global_));
        if (key == "ignoreCase") return EvalResult::ok(Value::boolean(rx->ignore_case_));
        if (key == "multiline") return EvalResult::ok(Value::boolean(rx->multiline_));
        if (key == "dotAll") return EvalResult::ok(Value::boolean(rx->dot_all_));
        if (key == "sticky") return EvalResult::ok(Value::boolean(rx->sticky_));
        if (key == "unicode") return EvalResult::ok(Value::boolean(rx->unicode_));
        if (key == "lastIndex") return EvalResult::ok(Value::number(static_cast<double>(rx->last_index_)));
        if (regexp_prototype_) return EvalResult::ok(regexp_prototype_->get_property(key));
        return EvalResult::ok(Value::undefined());
    }
    if (raw_obj->object_kind() == ObjectKind::kStringObject) {
        auto* js_obj = static_cast<JSObject*>(raw_obj);
        if (key == "length") {
            JSString* js_str = js_obj->wrapped_value().js_string_raw();
            return EvalResult::ok(Value::number(static_cast<double>(utf8_cp_len(js_str))));
        }
        return EvalResult::ok(js_obj->get_property(key));
    }
    if (raw_obj->object_kind() == ObjectKind::kBooleanObject) {
        auto* js_obj = static_cast<JSObject*>(raw_obj);
        return EvalResult::ok(js_obj->get_property(key));
    }
    if (raw_obj->object_kind() != ObjectKind::kOrdinary && raw_obj->object_kind() != ObjectKind::kArray &&
        raw_obj->object_kind() != ObjectKind::kGenerator &&
        raw_obj->object_kind() != ObjectKind::kMap && raw_obj->object_kind() != ObjectKind::kSet &&
        raw_obj->object_kind() != ObjectKind::kWeakMap && raw_obj->object_kind() != ObjectKind::kWeakSet) {
        return EvalResult::ok(Value::undefined());
    }
    auto* js_obj = static_cast<JSObject*>(raw_obj);
    // Check prototype chain for accessor getter; fall back to get_property for regular data.
    {
        const JSObject* cur = js_obj;
        while (cur != nullptr) {
            const JSObject::PropertyEntry* entry = cur->get_own_entry(key);
            if (entry != nullptr) {
                if (entry->flags & kPropIsAccessor) {
                    if (entry->getter.is_undefined() || entry->getter.is_null()) {
                        return EvalResult::ok(Value::undefined());
                    }
                    Value getter_copy = entry->getter;
                    return call_function_val(getter_copy, obj_val, {});
                }
                break;  // data property found: stop traversal
            }
            cur = cur->proto().get();
        }
    }
    // No accessor found — use normal property lookup (handles array length, index, etc.)
    return EvalResult::ok(js_obj->get_property(key));
}

// Property access on a pre-evaluated object value with a pre-evaluated key.
// Used by eval_optional_chain to avoid reconstructing AST nodes.
EvalResult Interpreter::eval_get_property_of(const Value& obj_val, const Value& key_val) {
    if (obj_val.is_undefined() || obj_val.is_null()) {
        pending_throw_ = make_error_value(NativeErrorType::kTypeError,
            "Cannot read properties of " + to_string_val(obj_val));
        return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
    }
    if (obj_val.is_string()) {
        std::string key = to_string_val(key_val);
        if (key == "length") {
            return EvalResult::ok(Value::number(static_cast<double>(utf8_cp_len(obj_val.js_string_raw()))));
        }
        if (string_prototype_) return EvalResult::ok(string_prototype_->get_property(key));
        return EvalResult::ok(Value::undefined());
    }
    if (obj_val.is_symbol()) {
        if (!key_val.is_symbol()) {
            std::string key = to_string_val(key_val);
            if (key == "description") {
                const std::string* desc = symbol_table_.GetDescription(obj_val.as_symbol_id());
                return EvalResult::ok(desc ? Value::string(*desc) : Value::undefined());
            }
            if (symbol_prototype_) return EvalResult::ok(symbol_prototype_->get_property(key));
        }
        return EvalResult::ok(Value::undefined());
    }
    if (!obj_val.is_object()) {
        return EvalResult::ok(Value::undefined());
    }
    if (key_val.is_symbol()) {
        RcObject* raw = obj_val.as_object_raw();
        if (raw->object_kind() == ObjectKind::kOrdinary || raw->object_kind() == ObjectKind::kArray ||
            raw->object_kind() == ObjectKind::kGenerator ||
            raw->object_kind() == ObjectKind::kMap || raw->object_kind() == ObjectKind::kSet ||
            raw->object_kind() == ObjectKind::kWeakMap || raw->object_kind() == ObjectKind::kWeakSet) {
            auto* js_obj_sym = static_cast<JSObject*>(raw);
            uint64_t sym_id = key_val.as_symbol_id();
            const JSObject::SymbolPropertyEntry* sym_entry = js_obj_sym->find_symbol_entry(sym_id);
            if (sym_entry != nullptr && sym_entry->is_accessor) {
                if (sym_entry->getter.is_undefined() || sym_entry->getter.is_null()) {
                    return EvalResult::ok(Value::undefined());
                }
                Value getter_copy = sym_entry->getter;
                return call_function_val(getter_copy, obj_val, {});
            }
            if (sym_entry != nullptr) return EvalResult::ok(sym_entry->value);
        }
        return EvalResult::ok(Value::undefined());
    }
    std::string key = to_string_val(key_val);
    RcObject* raw_obj = obj_val.as_object_raw();
    if (raw_obj->object_kind() == ObjectKind::kFunction) {
        auto* fn = static_cast<JSFunction*>(raw_obj);
        Value own = fn->get_property(key);
        if (!own.is_undefined()) return EvalResult::ok(own);
        if (key == "prototype") {
            const auto& proto = fn->prototype_obj();
            return EvalResult::ok(proto ? Value::object(ObjectPtr(proto)) : Value::undefined());
        }
        if (function_prototype_) return EvalResult::ok(function_prototype_->get_property(key));
        return EvalResult::ok(Value::undefined());
    }
    if (raw_obj->object_kind() == ObjectKind::kPromise) {
        if (promise_prototype_) return EvalResult::ok(promise_prototype_->get_property(key));
        return EvalResult::ok(Value::undefined());
    }
    if (raw_obj->object_kind() == ObjectKind::kRegExp) {
        auto* rx = static_cast<JSRegExp*>(raw_obj);
        if (key == "source") return EvalResult::ok(Value::string(rx->pattern_.empty() ? "(?:)" : rx->pattern_));
        if (key == "flags") return EvalResult::ok(Value::string(rx->flags_str_));
        if (key == "global") return EvalResult::ok(Value::boolean(rx->global_));
        if (key == "ignoreCase") return EvalResult::ok(Value::boolean(rx->ignore_case_));
        if (key == "multiline") return EvalResult::ok(Value::boolean(rx->multiline_));
        if (key == "dotAll") return EvalResult::ok(Value::boolean(rx->dot_all_));
        if (key == "sticky") return EvalResult::ok(Value::boolean(rx->sticky_));
        if (key == "unicode") return EvalResult::ok(Value::boolean(rx->unicode_));
        if (key == "lastIndex") return EvalResult::ok(Value::number(static_cast<double>(rx->last_index_)));
        if (regexp_prototype_) return EvalResult::ok(regexp_prototype_->get_property(key));
        return EvalResult::ok(Value::undefined());
    }
    if (raw_obj->object_kind() == ObjectKind::kStringObject) {
        auto* js_obj = static_cast<JSObject*>(raw_obj);
        if (key == "length") {
            JSString* js_str = js_obj->wrapped_value().js_string_raw();
            return EvalResult::ok(Value::number(static_cast<double>(utf8_cp_len(js_str))));
        }
        return EvalResult::ok(js_obj->get_property(key));
    }
    if (raw_obj->object_kind() == ObjectKind::kBooleanObject) {
        auto* js_obj = static_cast<JSObject*>(raw_obj);
        return EvalResult::ok(js_obj->get_property(key));
    }
    if (raw_obj->object_kind() != ObjectKind::kOrdinary && raw_obj->object_kind() != ObjectKind::kArray &&
        raw_obj->object_kind() != ObjectKind::kGenerator &&
        raw_obj->object_kind() != ObjectKind::kMap && raw_obj->object_kind() != ObjectKind::kSet &&
        raw_obj->object_kind() != ObjectKind::kWeakMap && raw_obj->object_kind() != ObjectKind::kWeakSet) {
        return EvalResult::ok(Value::undefined());
    }
    auto* js_obj = static_cast<JSObject*>(raw_obj);
    {
        const JSObject* cur = js_obj;
        while (cur != nullptr) {
            const JSObject::PropertyEntry* entry = cur->get_own_entry(key);
            if (entry != nullptr) {
                if (entry->flags & kPropIsAccessor) {
                    if (entry->getter.is_undefined() || entry->getter.is_null()) {
                        return EvalResult::ok(Value::undefined());
                    }
                    Value getter_copy = entry->getter;
                    return call_function_val(getter_copy, obj_val, {});
                }
                break;
            }
            cur = cur->proto().get();
        }
    }
    return EvalResult::ok(js_obj->get_property(key));
}

EvalResult Interpreter::eval_optional_chain(const OptionalChainExpression& expr) {
    auto base_r = eval_expr(*expr.base);
    if (!base_r.is_ok()) return base_r;

    Value current = base_r.value();
    Value receiver = Value::undefined();
    bool prev_was_member = false;

    for (size_t i = 0; i < expr.links.size(); ++i) {
        const auto& link = expr.links[i];

        // Check optional short-circuit: if base is null/undefined, return undefined
        bool optional = std::visit([](const auto& l) { return l.optional; }, link);
        if (optional && (current.is_null() || current.is_undefined())) {
            return EvalResult::ok(Value::undefined());
        }

        if (const auto* prop = std::get_if<OptionalChainExpression::PropLink>(&link)) {
            receiver = current;
            auto r = eval_get_property_of(current, Value::string(prop->name));
            if (!r.is_ok()) return r;
            current = r.value();
            prev_was_member = true;
        } else if (const auto* elem = std::get_if<OptionalChainExpression::ElemLink>(&link)) {
            receiver = current;
            auto key_r = eval_expr(*elem->key);
            if (!key_r.is_ok()) return key_r;
            auto r = eval_get_property_of(current, key_r.value());
            if (!r.is_ok()) return r;
            current = r.value();
            prev_was_member = true;
        } else if (const auto* call = std::get_if<OptionalChainExpression::CallLink>(&link)) {
            std::vector<Value> args;
            for (const auto& arg : call->args) {
                auto arg_r = eval_expr(*arg);
                if (!arg_r.is_ok()) return arg_r;
                args.push_back(arg_r.value());
            }
            Value this_val = prev_was_member ? receiver : Value::undefined();
            auto r = call_function_val(current, this_val, args);
            if (!r.is_ok()) return r;
            current = r.value();
            receiver = Value::undefined();
            prev_was_member = false;
        }
    }

    return EvalResult::ok(current);
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

    // Private field assignment: property is a synthetic Identifier with #name
    if (!expr.computed && std::holds_alternative<Identifier>(expr.property->v)) {
        const auto& id = std::get<Identifier>(expr.property->v);
        if (!id.name.empty() && id.name[0] == '#') {
            uint64_t sym_id = current_function_ ? current_function_->get_private_field_sym(id.name) : 0;
            if (sym_id == 0) {
                pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                    "Cannot set private member '" + id.name + "' of object whose class did not declare it");
                return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
            }
            RcObject* raw_pf = obj_val.as_object_raw();
            bool is_fn_obj = (raw_pf->object_kind() == ObjectKind::kFunction);
            JSObject* obj_pf = is_fn_obj ? nullptr : static_cast<JSObject*>(raw_pf);
            JSFunction* fn_pf = is_fn_obj ? static_cast<JSFunction*>(raw_pf) : nullptr;
            // Helper: read current private field value
            auto read_pf = [&]() -> Value {
                if (fn_pf) return fn_pf->get_property(id.name);  // static: string key
                if (!obj_pf) return Value::undefined();
                const JSObject::SymbolPropertyEntry* e = obj_pf->find_symbol_entry(sym_id);
                if (e && !e->is_accessor) return e->value;
                return Value::undefined();
            };
            // Helper: write private field value
            auto write_pf = [&](Value write_val) -> EvalResult {
                if (fn_pf) {
                    fn_pf->set_property(id.name, write_val);  // static: string key
                } else if (obj_pf) {
                    const JSObject::SymbolPropertyEntry* entry = obj_pf->find_symbol_entry(sym_id);
                    if (entry && entry->is_accessor) {
                        if (!entry->setter.is_undefined() && !entry->setter.is_null()) {
                            Value setter_copy = entry->setter;
                            std::vector<Value> setter_args = {write_val};
                            auto sres = call_function_val(setter_copy, obj_val, setter_args);
                            if (!sres.is_ok()) return sres;
                        }
                    } else {
                        obj_pf->set_property_by_symbol(sym_id, write_val);
                    }
                }
                return EvalResult::ok(write_val);
            };

            if (expr.op == AssignOp::Assign) {
                auto val_result = eval_expr(*expr.value);
                if (!val_result.is_ok()) return val_result;
                return write_pf(val_result.value());
            }
            // Compound assignment: read-modify-write
            Value cur = read_pf();
            auto rhs = eval_expr(*expr.value);
            if (!rhs.is_ok()) return rhs;
            Value new_v;
            auto apply_arith = [&](auto op_fn) -> EvalResult {
                auto ln = to_number(cur); if (!ln.is_ok()) return ln;
                auto rn = to_number(rhs.value()); if (!rn.is_ok()) return rn;
                new_v = Value::number(op_fn(ln.value().as_number(), rn.value().as_number()));
                return EvalResult::ok(new_v);
            };
            EvalResult op_res = EvalResult::ok(Value::undefined());
            switch (expr.op) {
                case AssignOp::AddAssign:
                    if (cur.is_string() || rhs.value().is_string()) {
                        new_v = Value::string(to_string_val(cur) + to_string_val(rhs.value()));
                        op_res = EvalResult::ok(new_v);
                    } else {
                        op_res = apply_arith([](double a, double b) { return a + b; });
                    }
                    break;
                case AssignOp::SubAssign:  op_res = apply_arith([](double a, double b) { return a - b; }); break;
                case AssignOp::MulAssign:  op_res = apply_arith([](double a, double b) { return a * b; }); break;
                case AssignOp::DivAssign:  op_res = apply_arith([](double a, double b) { return a / b; }); break;
                case AssignOp::ModAssign:  op_res = apply_arith([](double a, double b) { return std::fmod(a, b); }); break;
                case AssignOp::PowAssign:  op_res = apply_arith([](double a, double b) { return std::pow(a, b); }); break;
                case AssignOp::BitAndAssign: { auto ln = to_number(cur); if (!ln.is_ok()) return ln; auto rn = to_number(rhs.value()); if (!rn.is_ok()) return rn; new_v = Value::number(static_cast<double>(to_int32_bits(ln.value().as_number()) & to_int32_bits(rn.value().as_number()))); op_res = EvalResult::ok(new_v); break; }
                case AssignOp::BitOrAssign:  { auto ln = to_number(cur); if (!ln.is_ok()) return ln; auto rn = to_number(rhs.value()); if (!rn.is_ok()) return rn; new_v = Value::number(static_cast<double>(to_int32_bits(ln.value().as_number()) | to_int32_bits(rn.value().as_number()))); op_res = EvalResult::ok(new_v); break; }
                case AssignOp::BitXorAssign: { auto ln = to_number(cur); if (!ln.is_ok()) return ln; auto rn = to_number(rhs.value()); if (!rn.is_ok()) return rn; new_v = Value::number(static_cast<double>(to_int32_bits(ln.value().as_number()) ^ to_int32_bits(rn.value().as_number()))); op_res = EvalResult::ok(new_v); break; }
                case AssignOp::ShlAssign: { auto ln = to_number(cur); if (!ln.is_ok()) return ln; auto rn = to_number(rhs.value()); if (!rn.is_ok()) return rn; new_v = Value::number(static_cast<double>(to_int32_bits(ln.value().as_number()) << (to_uint32_bits(rn.value().as_number()) & 0x1F))); op_res = EvalResult::ok(new_v); break; }
                case AssignOp::SarAssign: { auto ln = to_number(cur); if (!ln.is_ok()) return ln; auto rn = to_number(rhs.value()); if (!rn.is_ok()) return rn; new_v = Value::number(static_cast<double>(to_int32_bits(ln.value().as_number()) >> (to_uint32_bits(rn.value().as_number()) & 0x1F))); op_res = EvalResult::ok(new_v); break; }
                case AssignOp::ShrAssign: { auto ln = to_number(cur); if (!ln.is_ok()) return ln; auto rn = to_number(rhs.value()); if (!rn.is_ok()) return rn; new_v = Value::number(static_cast<double>(to_uint32_bits(ln.value().as_number()) >> (to_uint32_bits(rn.value().as_number()) & 0x1F))); op_res = EvalResult::ok(new_v); break; }
                case AssignOp::LogicalAndAssign: {
                    if (!to_boolean(cur)) return EvalResult::ok(cur);
                    return write_pf(rhs.value());
                }
                case AssignOp::LogicalOrAssign: {
                    if (to_boolean(cur)) return EvalResult::ok(cur);
                    return write_pf(rhs.value());
                }
                case AssignOp::NullishAssign: {
                    if (!cur.is_null() && !cur.is_undefined()) return EvalResult::ok(cur);
                    return write_pf(rhs.value());
                }
                default: break;
            }
            if (!op_res.is_ok()) return op_res;
            return write_pf(new_v);
        }
    }

    auto key_result = eval_expr(*expr.property);
    if (!key_result.is_ok()) {
        return key_result;
    }

    // Logical assignment: read current value, short-circuit if needed
    if (expr.op == AssignOp::LogicalAndAssign || expr.op == AssignOp::LogicalOrAssign ||
        expr.op == AssignOp::NullishAssign) {
        auto cur_val = eval_get_property_of(obj_val, key_result.value());
        if (!cur_val.is_ok()) return cur_val;
        const Value& lv = cur_val.value();
        bool should_assign = false;
        if (expr.op == AssignOp::LogicalAndAssign) {
            should_assign = to_boolean(lv);
        } else if (expr.op == AssignOp::LogicalOrAssign) {
            should_assign = !to_boolean(lv);
        } else {
            should_assign = lv.is_null() || lv.is_undefined();
        }
        if (!should_assign) return cur_val;
        // Eval RHS and set property
        auto rhs = eval_expr(*expr.value);
        if (!rhs.is_ok()) return rhs;
        if (!key_result.value().is_symbol()) {
            std::string str_key = to_string_val(key_result.value());
            RcObject* raw_obj = obj_val.as_object_raw();
            if (raw_obj->object_kind() == ObjectKind::kOrdinary ||
                raw_obj->object_kind() == ObjectKind::kArray) {
                auto* js_obj = static_cast<JSObject*>(raw_obj);
                auto set_res = js_obj->set_property_ex(str_key, rhs.value());
                if (!set_res.is_ok()) {
                    const std::string& msg = set_res.error().message();
                    pending_throw_ = make_error_value(NativeErrorType::kTypeError, strip_error_prefix(msg));
                    return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
                }
            }
        } else {
            RcObject* raw_obj = obj_val.as_object_raw();
            if (raw_obj->object_kind() == ObjectKind::kOrdinary ||
                raw_obj->object_kind() == ObjectKind::kArray) {
                auto* js_obj = static_cast<JSObject*>(raw_obj);
                js_obj->set_property_by_symbol(key_result.value().as_symbol_id(), rhs.value());
            }
        }
        return rhs;
    }

    // Handle Symbol key assignment
    if (key_result.value().is_symbol()) {
        auto val_result2 = eval_expr(*expr.value);
        if (!val_result2.is_ok()) return val_result2;
        RcObject* raw_sym = obj_val.as_object_raw();
        if (raw_sym->object_kind() == ObjectKind::kOrdinary ||
            raw_sym->object_kind() == ObjectKind::kArray) {
            auto* js_obj_sym = static_cast<JSObject*>(raw_sym);
            uint64_t sym_id = key_result.value().as_symbol_id();
            const JSObject::SymbolPropertyEntry* sym_entry = js_obj_sym->find_symbol_entry(sym_id);
            if (sym_entry != nullptr && sym_entry->is_accessor) {
                if (!sym_entry->setter.is_undefined() && !sym_entry->setter.is_null()) {
                    Value setter_copy = sym_entry->setter;
                    std::vector<Value> setter_args = {val_result2.value()};
                    auto sres = call_function_val(setter_copy, obj_val, setter_args);
                    if (!sres.is_ok()) return sres;
                }
                return EvalResult::ok(val_result2.value());
            }
            js_obj_sym->set_property_by_symbol(sym_id, val_result2.value());
        }
        return EvalResult::ok(val_result2.value());
    }

    std::string key = to_string_val(key_result.value());

    // Arithmetic compound assignment: read-modify-write
    if (expr.op != AssignOp::Assign) {
        auto cur_val = eval_get_property_of(obj_val, Value::string(key));
        if (!cur_val.is_ok()) return cur_val;
        auto rhs = eval_expr(*expr.value);
        if (!rhs.is_ok()) return rhs;
        Value new_v;
        // Reuse arithmetic: call eval_binary_op equivalent inline
        auto apply_arith = [&](auto op_fn) -> EvalResult {
            auto ln = to_number(cur_val.value());
            if (!ln.is_ok()) return ln;
            auto rn = to_number(rhs.value());
            if (!rn.is_ok()) return rn;
            new_v = Value::number(op_fn(ln.value().as_number(), rn.value().as_number()));
            return EvalResult::ok(new_v);
        };
        EvalResult op_res = EvalResult::ok(Value::undefined());
        switch (expr.op) {
            case AssignOp::AddAssign: {
                // String concatenation or numeric
                if (cur_val.value().is_string() || rhs.value().is_string()) {
                    std::string s = to_string_val(cur_val.value()) + to_string_val(rhs.value());
                    new_v = Value::string(s);
                    op_res = EvalResult::ok(new_v);
                } else {
                    op_res = apply_arith([](double a, double b) { return a + b; });
                }
                break;
            }
            case AssignOp::SubAssign:
                op_res = apply_arith([](double a, double b) { return a - b; });
                break;
            case AssignOp::MulAssign:
                op_res = apply_arith([](double a, double b) { return a * b; });
                break;
            case AssignOp::DivAssign:
                op_res = apply_arith([](double a, double b) { return a / b; });
                break;
            case AssignOp::ModAssign:
                op_res = apply_arith([](double a, double b) { return std::fmod(a, b); });
                break;
            case AssignOp::PowAssign:
                op_res = apply_arith([](double a, double b) { return std::pow(a, b); });
                break;
            case AssignOp::BitAndAssign: {
                auto ln = to_number(cur_val.value()); if (!ln.is_ok()) return ln;
                auto rn = to_number(rhs.value()); if (!rn.is_ok()) return rn;
                new_v = Value::number(static_cast<double>(to_int32_bits(ln.value().as_number()) & to_int32_bits(rn.value().as_number())));
                op_res = EvalResult::ok(new_v);
                break;
            }
            case AssignOp::BitOrAssign: {
                auto ln = to_number(cur_val.value()); if (!ln.is_ok()) return ln;
                auto rn = to_number(rhs.value()); if (!rn.is_ok()) return rn;
                new_v = Value::number(static_cast<double>(to_int32_bits(ln.value().as_number()) | to_int32_bits(rn.value().as_number())));
                op_res = EvalResult::ok(new_v);
                break;
            }
            case AssignOp::BitXorAssign: {
                auto ln = to_number(cur_val.value()); if (!ln.is_ok()) return ln;
                auto rn = to_number(rhs.value()); if (!rn.is_ok()) return rn;
                new_v = Value::number(static_cast<double>(to_int32_bits(ln.value().as_number()) ^ to_int32_bits(rn.value().as_number())));
                op_res = EvalResult::ok(new_v);
                break;
            }
            case AssignOp::ShlAssign: {
                auto ln = to_number(cur_val.value()); if (!ln.is_ok()) return ln;
                auto rn = to_number(rhs.value()); if (!rn.is_ok()) return rn;
                uint32_t sh = to_uint32_bits(rn.value().as_number()) & 0x1F;
                new_v = Value::number(static_cast<double>(to_int32_bits(ln.value().as_number()) << sh));
                op_res = EvalResult::ok(new_v);
                break;
            }
            case AssignOp::SarAssign: {
                auto ln = to_number(cur_val.value()); if (!ln.is_ok()) return ln;
                auto rn = to_number(rhs.value()); if (!rn.is_ok()) return rn;
                uint32_t sh = to_uint32_bits(rn.value().as_number()) & 0x1F;
                new_v = Value::number(static_cast<double>(to_int32_bits(ln.value().as_number()) >> sh));
                op_res = EvalResult::ok(new_v);
                break;
            }
            case AssignOp::ShrAssign: {
                auto ln = to_number(cur_val.value()); if (!ln.is_ok()) return ln;
                auto rn = to_number(rhs.value()); if (!rn.is_ok()) return rn;
                uint32_t sh = to_uint32_bits(rn.value().as_number()) & 0x1F;
                new_v = Value::number(static_cast<double>(to_uint32_bits(ln.value().as_number()) >> sh));
                op_res = EvalResult::ok(new_v);
                break;
            }
            default:
                break;
        }
        if (!op_res.is_ok()) return op_res;
        // Write back
        RcObject* raw_wb = obj_val.as_object_raw();
        if (raw_wb->object_kind() == ObjectKind::kOrdinary ||
            raw_wb->object_kind() == ObjectKind::kArray) {
            auto* js_wb = static_cast<JSObject*>(raw_wb);
            auto set_res = js_wb->set_property_ex(key, new_v);
            if (!set_res.is_ok()) {
                const std::string& msg = set_res.error().message();
                NativeErrorType err_type = NativeErrorType::kTypeError;
                if (msg.rfind("RangeError:", 0) == 0) err_type = NativeErrorType::kRangeError;
                pending_throw_ = make_error_value(err_type, strip_error_prefix(msg));
                return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
            }
        }
        return EvalResult::ok(new_v);
    }

    auto val_result = eval_expr(*expr.value);
    if (!val_result.is_ok()) {
        return val_result;
    }

    RcObject* raw_obj2 = obj_val.as_object_raw();
    // kRegExp: allow writing lastIndex
    if (raw_obj2->object_kind() == ObjectKind::kRegExp) {
        if (key == "lastIndex") {
            double n = to_number_double(val_result.value());
            auto* rx = static_cast<JSRegExp*>(raw_obj2);
            rx->last_index_ = std::isnan(n) || n < 0.0 ? 0u : static_cast<uint32_t>(n);
        }
        return EvalResult::ok(val_result.value());
    }
    if (raw_obj2->object_kind() == ObjectKind::kFunction) {
        // Functions can have properties (e.g., assert._isSameValue = fn)
        auto* fn_obj2 = static_cast<JSFunction*>(raw_obj2);
        fn_obj2->set_property(key, val_result.value());
        return EvalResult::ok(val_result.value());
    }
    if (raw_obj2->object_kind() != ObjectKind::kOrdinary && raw_obj2->object_kind() != ObjectKind::kArray) {
        pending_throw_ = make_error_value(NativeErrorType::kTypeError,
            "Cannot set properties of non-ordinary object");
        return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
    }
    auto* js_obj = static_cast<JSObject*>(raw_obj2);
    // Check prototype chain for accessor setter
    {
        const JSObject* cur = js_obj;
        while (cur != nullptr) {
            const JSObject::PropertyEntry* entry = cur->get_own_entry(key);
            if (entry != nullptr) {
                if (entry->flags & kPropIsAccessor) {
                    if (entry->setter.is_undefined() || entry->setter.is_null()) {
                        // Sloppy mode: silently ignore write to get-only accessor
                        return EvalResult::ok(val_result.value());
                    }
                    Value setter_copy = entry->setter;
                    std::vector<Value> setter_args = {val_result.value()};
                    auto setter_res = call_function_val(setter_copy, obj_val, setter_args);
                    if (!setter_res.is_ok()) return setter_res;
                    return EvalResult::ok(val_result.value());
                }
                // data property — fall through to set_property_ex
                break;
            }
            cur = cur->proto().get();
        }
    }
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

Value Interpreter::make_function_value(std::optional<std::string> name, const std::vector<ParamDef>& params,
                                        std::shared_ptr<std::vector<StmtNode>> body,
                                        RcPtr<Environment> closure_env,
                                        bool is_named_expr,
                                        std::optional<std::string> rest_param) {
    // 计算 length_count（第一个有默认值或解构参数的索引）
    uint32_t length_count = static_cast<uint32_t>(params.size());
    for (uint32_t i = 0; i < static_cast<uint32_t>(params.size()); ++i) {
        if (params[i].default_init != nullptr || params[i].pattern_binding != nullptr) {
            length_count = i;
            break;
        }
    }

    auto fn = RcPtr<JSFunction>::make();
    fn->set_name(name);
    // 提取参数名列表（供原有绑定逻辑和 arguments 使用）
    std::vector<std::string> param_names;
    param_names.reserve(params.size());
    for (const auto& pd : params) param_names.push_back(pd.name);
    fn->set_params(std::move(param_names));
    // 设置 param_defs 以供 call_function 中默认值求值
    fn->set_param_defs(std::make_shared<std::vector<ParamDef>>(params));
    fn->set_body(std::move(body));
    fn->set_closure_env(std::move(closure_env));
    fn->set_is_named_expr(is_named_expr);
    fn->set_defining_module(current_module_);
    fn->set_rest_param(std::move(rest_param));
    fn->set_property("length", Value::number(static_cast<double>(length_count)));
    fn->set_property("name", Value::string(name.value_or("")));

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
    // 守卫：class constructor 不可直接调用（需要 new）
    if (fn->is_class_ctor() && !is_new_call) {
        std::string fn_name = fn->name().has_value() ? *fn->name() : "class";
        pending_throw_ = make_error_value(NativeErrorType::kTypeError,
            "Class constructor " + fn_name + " cannot be invoked without 'new'");
        return StmtResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
    }
    // 守卫 0：method 不可用 new 构造
    if (fn->is_method() && is_new_call) {
        std::string fn_name = fn->name().has_value() ? *fn->name() : "method";
        pending_throw_ = make_error_value(NativeErrorType::kTypeError,
            fn_name + " is not a constructor");
        return StmtResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
    }

    if (fn->is_native()) {
        auto r = fn->native_fn()(this_val, std::move(args), is_new_call);
        if (!r.is_ok()) {
            return StmtResult::err(r.error());
        }
        return StmtResult::ok(Completion::return_(r.value()));
    }

    // 守卫 1：箭头函数不可 new
    if (fn->is_arrow() && is_new_call) {
        pending_throw_ = make_error_value(NativeErrorType::kTypeError,
            "arrow function is not a constructor");
        return StmtResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
    }

    // M2: 非 new 调用时 new.target 应为 undefined；保存并恢复调用方的 new.target
    Value saved_call_new_target = current_new_target_;
    if (!is_new_call && !fn->is_arrow()) {
        current_new_target_ = Value::undefined();
    }
    struct NewTargetGuard {
        Interpreter& interp;
        Value saved;
        ~NewTargetGuard() { interp.current_new_target_ = std::move(saved); }
    } nt_guard{*this, saved_call_new_target};

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

    // 守卫 2：arguments 对象在参数绑定前建立（M2：默认值表达式可引用 arguments）
    // 箭头函数不创建 arguments（词法穿透外层）
    if (!fn->is_arrow()) {
        auto arg_obj = RcPtr<JSObject>::make();
        gc_heap_.Register(arg_obj.get());
        arg_obj->set_proto(object_prototype_);
        // arguments 是 kOrdinary 对象，用 set_property 存储数字索引属性
        // 这样 arguments[0] / arguments["0"] 才能通过 get_property("0") 正确读到
        for (size_t i = 0; i < args.size(); ++i) {
            arg_obj->set_property(std::to_string(i), args[i]);
        }
        arg_obj->set_property("length", Value::number(static_cast<double>(args.size())));
        fn_env->define("arguments", VarKind::Var);
        fn_env->initialize("arguments", Value::object(ObjectPtr(arg_obj)));
    }

    // 守卫 3：箭头函数使用词法 this（M2：actual_this 在参数绑定前确定）
    Value actual_this = fn->is_arrow() ? fn->lexical_this() : std::move(this_val);

    // 参数绑定：若有 param_defs 则支持默认值求值和解构参数
    if (fn->param_defs() != nullptr) {
        const auto& defs = *fn->param_defs();
        // 切换到 fn_env 并临时设置 this，以便默认值表达式能引用前面已绑定的参数、arguments 和 this
        RcPtr<Environment> old_env = current_env_;
        RcPtr<Environment> old_var_env = var_env_;
        Value old_this = current_this_;
        current_env_ = fn_env;
        var_env_ = fn_env;
        current_this_ = actual_this;  // 临时设置 this（M2）
        for (size_t i = 0; i < defs.size(); ++i) {
            Value arg_val = (i < args.size()) ? args[i] : Value::undefined();
            if (defs[i].pattern_binding != nullptr) {
                // 解构参数：先预声明所有解构出的变量名，再处理默认值，最后执行解构绑定
                std::vector<std::string> pat_names;
                collect_pattern_names(*defs[i].pattern_binding, pat_names);
                for (const auto& pn : pat_names) {
                    fn_env->define(pn, VarKind::Var);
                }
                if (arg_val.is_undefined() && defs[i].default_init != nullptr) {
                    auto default_r = eval_expr(*defs[i].default_init);
                    if (!default_r.is_ok()) {
                        current_env_ = old_env;
                        var_env_ = old_var_env;
                        current_this_ = std::move(old_this);
                        return StmtResult::err(default_r.error());
                    }
                    arg_val = default_r.value();
                }
                auto bind_r = bind_pattern(*defs[i].pattern_binding, std::move(arg_val),
                                           VarKind::Var, false);
                if (!bind_r.is_ok()) {
                    current_env_ = old_env;
                    var_env_ = old_var_env;
                    current_this_ = std::move(old_this);
                    return bind_r;
                }
            } else {
                fn_env->define(defs[i].name, VarKind::Var);   // 先声明（M1）
                fn_env->initialize(defs[i].name, arg_val);    // 初始化为实参或 undefined（M1）
                if (arg_val.is_undefined() && defs[i].default_init != nullptr) {
                    auto default_r = eval_expr(*defs[i].default_init);
                    if (!default_r.is_ok()) {
                        current_env_ = old_env;
                        var_env_ = old_var_env;
                        current_this_ = std::move(old_this);
                        return StmtResult::err(default_r.error());
                    }
                    fn_env->set(defs[i].name, default_r.value());  // 更新为默认值（M1）
                }
            }
        }
        current_env_ = old_env;
        var_env_ = old_var_env;
        current_this_ = std::move(old_this);  // 恢复 this（M2）
    } else {
        const auto& params = fn->params();
        for (size_t i = 0; i < params.size(); ++i) {
            Value arg_val = (i < args.size()) ? args[i] : Value::undefined();
            fn_env->define(params[i], VarKind::Var);
            fn_env->initialize(params[i], std::move(arg_val));
        }
    }

    // Bind rest parameter
    if (fn->rest_param().has_value()) {
        const std::string& rest_name = fn->rest_param().value();
        auto rest_arr = RcPtr<JSObject>::make(ObjectKind::kArray);
        gc_heap_.Register(rest_arr.get());
        rest_arr->set_proto(array_prototype_);
        size_t rest_start = fn->params().size();
        for (size_t i = rest_start; i < args.size(); ++i) {
            rest_arr->elements_[static_cast<uint32_t>(i - rest_start)] = args[i];
        }
        rest_arr->array_length_ = static_cast<uint32_t>(
            args.size() > rest_start ? args.size() - rest_start : 0);
        fn_env->define(rest_name, VarKind::Var);
        fn_env->initialize(rest_name, Value::object(ObjectPtr(rest_arr)));
    }

    // Generator function: create generator object without executing the body
    if (fn->is_generator()) {
        if (is_new_call) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                (fn->name().has_value() ? *fn->name() : "GeneratorFunction") +
                " is not a constructor");
            return StmtResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        auto gen_obj = RcPtr<JSGeneratorObject>::make();
        gc_heap_.Register(gen_obj.get());
        gen_obj->set_proto(generator_prototype_);
        gen_obj->state_ = GeneratorState::kSuspendedStart;
        gen_obj->gen_body_ = fn->body();
        gen_obj->gen_env_ = fn_env;
        gen_obj->gen_this_val_ = actual_this;
        gen_obj->suspended_stmt_index_ = 0;

        Value gen_val = Value::object(ObjectPtr(gen_obj));
        // Set up .next() method
        {
            auto next_fn = RcPtr<JSFunction>::make();
            next_fn->set_name(std::string("next"));
            next_fn->set_property("length", Value::number(1));
            next_fn->set_native_fn([this, gen_obj](Value /*this_val*/, std::vector<Value> args, bool) -> EvalResult {
                return generator_next(gen_obj, args.empty() ? Value::undefined() : args[0]);
            });
            gc_heap_.Register(next_fn.get());
            gen_obj->set_property("next", Value::object(ObjectPtr(next_fn)));
        }
        // Set up .return() method
        {
            auto ret_fn = RcPtr<JSFunction>::make();
            ret_fn->set_name(std::string("return"));
            ret_fn->set_property("length", Value::number(1));
            ret_fn->set_native_fn([this, gen_obj](Value /*this_val*/, std::vector<Value> args, bool) -> EvalResult {
                return generator_return(gen_obj, args.empty() ? Value::undefined() : args[0]);
            });
            gc_heap_.Register(ret_fn.get());
            gen_obj->set_property("return", Value::object(ObjectPtr(ret_fn)));
        }
        // Set up .throw() method
        {
            auto throw_fn = RcPtr<JSFunction>::make();
            throw_fn->set_name(std::string("throw"));
            throw_fn->set_property("length", Value::number(1));
            throw_fn->set_native_fn([this, gen_obj](Value /*this_val*/, std::vector<Value> args, bool) -> EvalResult {
                return generator_throw(gen_obj, args.empty() ? Value::undefined() : args[0]);
            });
            gc_heap_.Register(throw_fn.get());
            gen_obj->set_property("throw", Value::object(ObjectPtr(throw_fn)));
        }
        return StmtResult::ok(Completion::return_(gen_val));
    }

    ScopeGuard guard(*this, fn_env, fn_env, std::move(actual_this), /*is_call=*/true);
    hoist_vars(*fn->body(), *fn_env);

    JSFunction* saved_function = current_function_;
    current_function_ = fn.get();

    // Base class constructor: initialize instance fields before body execution
    if (fn->is_class_ctor() && !fn->is_derived_ctor() && fn->instance_fields() &&
        !fn->instance_fields()->empty()) {
        auto field_r = init_instance_fields(fn.get(), current_this_);
        if (!field_r.is_ok()) {
            current_function_ = saved_function;
            return StmtResult::err(field_r.error());
        }
    }

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
    // Functions always return undefined unless there is an explicit return statement.
    (void)result_val;
    return StmtResult::ok(Completion::normal(Value::undefined()));
}

StmtResult Interpreter::eval_function_decl(const FunctionDeclaration& stmt) {
    Value fn_val = make_function_value(stmt.name, stmt.params, stmt.body, current_env_,
                                       false, stmt.rest_param);
    if (stmt.is_generator) {
        auto* fn = static_cast<JSFunction*>(fn_val.as_object_raw());
        fn->set_is_generator(true);
    }
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
        const std::string& em = r.error().message();
        if (em == kAsyncSuspendSentinel || em == kGeneratorYieldSentinel) {
            return StmtResult::err(r.error());
        }
        Value thrown = extract_throw_value(pending_throw_, em, kPendingThrowSentinel);
        return StmtResult::ok(Completion::throw_(std::move(thrown)));
    }
    return StmtResult::ok(Completion::throw_(r.value()));
}

StmtResult Interpreter::exec_catch(const CatchClause& handler, Value thrown_val) {
    auto catch_env = RcPtr<Environment>::make(current_env_);
    gc_heap_.Register(catch_env.get());
    auto old_env = current_env_;
    current_env_ = catch_env;

    // catch 绑定：解构模式 > 简单标识符 > 无绑定（ES2019 可选 catch）
    if (handler.pattern_binding != nullptr) {
        // 解构 catch 参数（如 catch ([a, b]) 或 catch ({a, b})）
        // current_env_ 已指向 catch_env，bind_pattern 使用 current_env_ 定义变量
        auto bind_r = bind_pattern(*handler.pattern_binding, thrown_val, VarKind::Let, false);
        if (!bind_r.is_ok()) {
            current_env_ = old_env;
            return bind_r;
        }
    } else if (handler.param.has_value()) {
        catch_env->define(handler.param.value(), VarKind::Let);
        catch_env->initialize(handler.param.value(), thrown_val);
    }

    auto result = eval_block_stmt(handler.body);

    current_env_ = old_env;
    return result;
}

StmtResult Interpreter::eval_try_stmt(const TryStatement& stmt) {
    // 1. Execute try block
    StmtResult try_result = eval_block_stmt(stmt.block);

    // Internal C++ error from try block → convert to ThrowCompletion
    // Exception: sentinel values must be propagated as-is (async suspension / generator yield).
    if (!try_result.is_ok()) {
        const std::string& em = try_result.error().message();
        if (em == kAsyncSuspendSentinel || em == kGeneratorYieldSentinel) {
            return try_result;
        }
        Value thrown = extract_throw_value(pending_throw_, em, kPendingThrowSentinel);
        try_result = StmtResult::ok(Completion::throw_(std::move(thrown)));
    }

    // 2. If there is a catch handler and try produced a throw, execute catch
    if (stmt.handler.has_value()) {
        if (try_result.is_ok() && try_result.completion().is_throw()) {
            Value thrown_val = try_result.completion().value;
            try_result = exec_catch(*stmt.handler, std::move(thrown_val));
            // Internal error from catch → convert to ThrowCompletion
            // Exception: sentinel values must be propagated as-is.
            if (!try_result.is_ok()) {
                const std::string& em2 = try_result.error().message();
                if (em2 == kAsyncSuspendSentinel || em2 == kGeneratorYieldSentinel) {
                    return try_result;
                }
                Value thrown = extract_throw_value(pending_throw_, em2, kPendingThrowSentinel);
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

StmtResult Interpreter::eval_switch_stmt(const SwitchStatement& stmt) {
    auto disc_r = eval_expr(*stmt.discriminant);
    if (!disc_r.is_ok()) return StmtResult::err(disc_r.error());
    Value disc = disc_r.value();

    // 找 default 索引
    int default_idx = -1;
    for (int i = 0; i < static_cast<int>(stmt.cases.size()); ++i) {
        if (!stmt.cases[i].test.has_value()) {
            default_idx = i;
            break;
        }
    }

    // 找第一个严格匹配的 case
    int start_idx = -1;
    for (int i = 0; i < static_cast<int>(stmt.cases.size()); ++i) {
        if (!stmt.cases[i].test.has_value()) continue;
        auto test_r = eval_expr(**stmt.cases[i].test);
        if (!test_r.is_ok()) return StmtResult::err(test_r.error());
        if (strict_eq_values(disc, test_r.value())) {
            start_idx = i;
            break;
        }
    }
    if (start_idx < 0) start_idx = default_idx;
    if (start_idx < 0) return StmtResult::ok(Completion::normal(Value::undefined()));

    // 从 start_idx 开始执行，支持 fallthrough
    for (int i = start_idx; i < static_cast<int>(stmt.cases.size()); ++i) {
        for (const auto& s : stmt.cases[i].consequent) {
            auto r = eval_stmt(*s);
            if (!r.is_ok()) return r;
            const Completion& c = r.completion();
            if (c.is_break() && !c.target.has_value()) {
                // 无标签 break → 退出 switch
                return StmtResult::ok(Completion::normal(Value::undefined()));
            }
            if (c.is_return() || c.is_throw()) return r;
            if (c.is_break() && c.target.has_value()) return r;  // 有标签 break 向外传播
            if (c.is_continue()) return r;                       // continue 传递给外层循环
        }
    }
    return StmtResult::ok(Completion::normal(Value::undefined()));
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
    } else if (std::holds_alternative<ForInStatement>(stmt.body->v)) {
        result = eval_for_in_stmt(std::get<ForInStatement>(stmt.body->v), stmt.label);
    } else if (std::holds_alternative<ForOfStatement>(stmt.body->v)) {
        result = eval_for_of_stmt(std::get<ForOfStatement>(stmt.body->v), stmt.label);
    } else if (std::holds_alternative<WhileStatement>(stmt.body->v)) {
        result = eval_while_stmt(std::get<WhileStatement>(stmt.body->v), stmt.label);
    } else if (std::holds_alternative<DoWhileStatement>(stmt.body->v)) {
        result = eval_do_while_stmt(std::get<DoWhileStatement>(stmt.body->v), stmt.label);
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

StmtResult Interpreter::eval_for_in_stmt(const ForInStatement& stmt,
                                          std::optional<std::string> label) {
    // Evaluate the right-hand side object
    auto obj_r = eval_expr(*stmt.right);
    if (!obj_r.is_ok()) {
        if (obj_r.error().message() == kAsyncSuspendSentinel) {
            return StmtResult::err(obj_r.error());
        }
        Value thrown = extract_throw_value(pending_throw_, obj_r.error().message(), kPendingThrowSentinel);
        return StmtResult::ok(Completion::throw_(std::move(thrown)));
    }
    const Value& obj_val = obj_r.value();

    // null/undefined → skip the loop entirely
    if (obj_val.is_null() || obj_val.is_undefined()) {
        return StmtResult::ok(Completion::normal(Value::undefined()));
    }

    // Non-object → no enumerable string keys, skip loop
    if (!obj_val.is_object()) {
        return StmtResult::ok(Completion::normal(Value::undefined()));
    }

    // Guard: only JSObject subclasses can be safely cast and enumerated.
    // kFunction/kPromise/kEnvironment/kModule/kForInIterator are RcObject, not JSObject.
    {
        ObjectKind k = obj_val.as_object_raw()->object_kind();
        if (k != ObjectKind::kOrdinary && k != ObjectKind::kArray &&
            k != ObjectKind::kRegExp && k != ObjectKind::kStringObject &&
            k != ObjectKind::kBooleanObject) {
            return StmtResult::ok(Completion::normal(Value::undefined()));
        }
    }

    JSObject* obj = static_cast<JSObject*>(obj_val.as_object_raw());
    std::vector<std::string> keys = obj->enumerate_properties();

    RcPtr<Environment> old_env = current_env_;
    const bool is_lexical = stmt.has_decl && stmt.var_kind != VarKind::Var;

    StmtResult loop_result = StmtResult::ok(Completion::normal(Value::undefined()));

    for (const auto& key : keys) {
        Value key_val = Value::string(key);

        // Assign the key to the loop variable
        if (stmt.has_decl && stmt.var_kind == VarKind::Var) {
            var_env_->set(stmt.binding, key_val);
        } else if (is_lexical) {
            // let/const: create a fresh per-iteration scope so each iteration's closure
            // captures an independent binding (ES spec per-iteration environment semantics).
            auto iter_env = RcPtr<Environment>::make(old_env);
            gc_heap_.Register(iter_env.get());
            current_env_ = iter_env;
            current_env_->define(stmt.binding, stmt.var_kind);
            current_env_->initialize(stmt.binding, key_val);
        } else {
            // no_decl: assign to existing variable in scope chain
            auto set_r = current_env_->set(stmt.binding, key_val);
            if (!set_r.is_ok()) {
                current_env_ = old_env;
                Value thrown = extract_throw_value(pending_throw_, set_r.error().message(),
                                                   kPendingThrowSentinel);
                return StmtResult::ok(Completion::throw_(std::move(thrown)));
            }
        }

        // Execute loop body
        auto body_r = eval_stmt(*stmt.body);
        if (is_lexical) {
            current_env_ = old_env;
        }
        if (!body_r.is_ok()) {
            current_env_ = old_env;
            return body_r;
        }
        const Completion& c = body_r.completion();
        if (c.is_break()) {
            if (!c.target.has_value() || c.target == label) {
                break;
            }
            current_env_ = old_env;
            return body_r;
        }
        if (c.is_continue()) {
            if (!c.target.has_value() || c.target == label) {
                // continue to next iteration
            } else {
                current_env_ = old_env;
                return body_r;
            }
        } else if (c.is_return() || c.is_throw()) {
            current_env_ = old_env;
            return body_r;
        }
    }

    current_env_ = old_env;
    return loop_result;
}

StmtResult Interpreter::eval_for_of_stmt(const ForOfStatement& stmt,
                                          std::optional<std::string> label) {
    auto rhs_r = eval_expr(*stmt.right);
    if (!rhs_r.is_ok()) {
        if (rhs_r.error().message() == kAsyncSuspendSentinel) {
            return StmtResult::err(rhs_r.error());
        }
        Value thrown = extract_throw_value(pending_throw_, rhs_r.error().message(), kPendingThrowSentinel);
        return StmtResult::ok(Completion::throw_(std::move(thrown)));
    }
    Value iterable = rhs_r.value();

    // null/undefined → TypeError (for...of does not skip, unlike for...in)
    if (iterable.is_null() || iterable.is_undefined()) {
        pending_throw_ = make_error_value(NativeErrorType::kTypeError,
            iterable.is_null() ? "null is not iterable" : "undefined is not iterable");
        return StmtResult::ok(Completion::throw_(*pending_throw_));
    }

    const bool is_lexical = stmt.has_decl && stmt.var_kind != VarKind::Var;
    RcPtr<Environment> old_env = current_env_;

    // Helper lambda: execute one iteration body with iter_val as the loop variable.
    // Returns nullopt to continue looping, or a StmtResult to break/return/throw.
    auto run_body = [&](Value iter_val) -> std::optional<StmtResult> {
        if (stmt.pattern_binding) {
            // Destructuring for-of: create per-iteration scope for lexical bindings
            if (is_lexical) {
                auto iter_env = RcPtr<Environment>::make(old_env);
                gc_heap_.Register(iter_env.get());
                current_env_ = iter_env;
            }
            auto bind_r = bind_pattern(*stmt.pattern_binding, iter_val, stmt.var_kind, !stmt.has_decl);
            if (!bind_r.is_ok()) {
                current_env_ = old_env;
                return bind_r;
            }
            if (bind_r.completion().is_throw()) {
                current_env_ = old_env;
                return bind_r;
            }
        } else if (stmt.has_decl && stmt.var_kind == VarKind::Var) {
            var_env_->set(stmt.binding, iter_val);
        } else if (is_lexical) {
            auto iter_env = RcPtr<Environment>::make(old_env);
            gc_heap_.Register(iter_env.get());
            current_env_ = iter_env;
            current_env_->define(stmt.binding, stmt.var_kind);
            current_env_->initialize(stmt.binding, iter_val);
        } else {
            // no-decl: assign to existing variable
            auto set_r = current_env_->set(stmt.binding, iter_val);
            if (!set_r.is_ok()) {
                current_env_ = old_env;
                Value thrown = extract_throw_value(pending_throw_, set_r.error().message(),
                                                   kPendingThrowSentinel);
                return StmtResult::ok(Completion::throw_(std::move(thrown)));
            }
        }

        auto body_r = eval_stmt(*stmt.body);
        if (is_lexical) current_env_ = old_env;

        if (!body_r.is_ok()) {
            current_env_ = old_env;
            return body_r;
        }
        const Completion& c = body_r.completion();
        if (c.is_break()) {
            if (!c.target.has_value() || c.target == label) {
                return StmtResult::ok(Completion::normal(Value::undefined()));
            }
            current_env_ = old_env;
            return body_r;
        }
        if (c.is_continue()) {
            if (!c.target.has_value() || c.target == label) {
                return std::nullopt;  // continue to next iteration
            }
            current_env_ = old_env;
            return body_r;
        }
        if (c.is_return() || c.is_throw()) {
            current_env_ = old_env;
            return body_r;
        }
        return std::nullopt;  // normal: next iteration
    };

    // Fast path: arrays
    if (iterable.is_object() && iterable.as_object_raw()->object_kind() == ObjectKind::kArray) {
        JSObject* arr = static_cast<JSObject*>(iterable.as_object_raw());
        uint32_t len = arr->array_length_;
        for (uint32_t i = 0; i < len; i++) {
            Value val;
            auto it = arr->elements_.find(i);
            val = (it != arr->elements_.end()) ? it->second : Value::undefined();
            auto res = run_body(std::move(val));
            if (res.has_value()) return *res;
            // Re-read length in case body mutated the array
            len = arr->array_length_;
        }
        current_env_ = old_env;
        return StmtResult::ok(Completion::normal(Value::undefined()));
    }

    // Fast path: strings
    if (iterable.is_string()) {
        std::string_view sv = iterable.sv();
        size_t pos = 0;
        while (pos < sv.size()) {
            size_t start = pos;
            // Decode one UTF-8 code point
            unsigned char c0 = static_cast<unsigned char>(sv[pos]);
            size_t cp_bytes = (c0 < 0x80) ? 1 : (c0 < 0xE0) ? 2 : (c0 < 0xF0) ? 3 : 4;
            if (pos + cp_bytes > sv.size()) cp_bytes = sv.size() - pos;
            pos += cp_bytes;
            std::string char_str(sv.data() + start, cp_bytes);
            auto res = run_body(Value::string(char_str));
            if (res.has_value()) return *res;
        }
        current_env_ = old_env;
        return StmtResult::ok(Completion::normal(Value::undefined()));
    }

    // Generic path: object with Symbol.iterator
    if (iterable.is_object()) {
        ObjectKind k = iterable.as_object_raw()->object_kind();
        JSObject* obj = nullptr;
        if (k == ObjectKind::kOrdinary || k == ObjectKind::kRegExp ||
            k == ObjectKind::kStringObject || k == ObjectKind::kBooleanObject ||
            k == ObjectKind::kGenerator ||
            k == ObjectKind::kMap || k == ObjectKind::kSet) {
            obj = static_cast<JSObject*>(iterable.as_object_raw());
        }
        if (obj) {
            Value iter_method = obj->get_property_by_symbol(symbol_table_.well_known_iterator);
            if (!iter_method.is_undefined() && !iter_method.is_null()) {
                // Call Symbol.iterator()
                auto iter_r = call_function_val(iter_method, iterable, std::span<Value>());
                if (!iter_r.is_ok()) {
                    current_env_ = old_env;
                    Value thrown = extract_throw_value(pending_throw_, iter_r.error().message(),
                                                       kPendingThrowSentinel);
                    return StmtResult::ok(Completion::throw_(std::move(thrown)));
                }
                Value iterator = iter_r.value();
                if (!iterator.is_object()) {
                    current_env_ = old_env;
                    pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                        "iterator must be an object");
                    return StmtResult::ok(Completion::throw_(*pending_throw_));
                }

                // Get next() method from iterator
                Value next_method = Value::undefined();
                ObjectKind ik = iterator.as_object_raw()->object_kind();
                if (ik == ObjectKind::kOrdinary || ik == ObjectKind::kArray ||
                    ik == ObjectKind::kRegExp || ik == ObjectKind::kStringObject ||
                    ik == ObjectKind::kBooleanObject || ik == ObjectKind::kGenerator) {
                    auto* iter_obj = static_cast<JSObject*>(iterator.as_object_raw());
                    next_method = iter_obj->get_property("next");
                }

                // Iterate
                while (true) {
                    auto next_r = call_function_val(next_method, iterator, std::span<Value>());
                    if (!next_r.is_ok()) {
                        current_env_ = old_env;
                        Value thrown = extract_throw_value(pending_throw_, next_r.error().message(),
                                                           kPendingThrowSentinel);
                        return StmtResult::ok(Completion::throw_(std::move(thrown)));
                    }
                    Value result = next_r.value();
                    if (!result.is_object()) {
                        current_env_ = old_env;
                        pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                            "iterator result must be an object");
                        return StmtResult::ok(Completion::throw_(*pending_throw_));
                    }
                    // Read done/value
                    Value done_val = Value::undefined();
                    Value value = Value::undefined();
                    ObjectKind rk = result.as_object_raw()->object_kind();
                    if (rk == ObjectKind::kOrdinary || rk == ObjectKind::kArray) {
                        auto* result_obj = static_cast<JSObject*>(result.as_object_raw());
                        done_val = result_obj->get_property("done");
                        value = result_obj->get_property("value");
                    }
                    if (to_boolean(done_val)) break;

                    auto iter_res = run_body(std::move(value));
                    if (iter_res.has_value()) return *iter_res;
                }

                current_env_ = old_env;
                return StmtResult::ok(Completion::normal(Value::undefined()));
            }
        }
    }

    // Not iterable
    current_env_ = old_env;
    pending_throw_ = make_error_value(NativeErrorType::kTypeError, "value is not iterable");
    return StmtResult::ok(Completion::throw_(*pending_throw_));
}

EvalResult Interpreter::eval_function_expr(const FunctionExpression& expr) {
    Value fn_val = make_function_value(expr.name, expr.params, expr.body, current_env_,
                                       expr.name.has_value(), expr.rest_param);
    if (expr.is_generator) {
        auto* fn = static_cast<JSFunction*>(fn_val.as_object_raw());
        fn->set_is_generator(true);
    }
    return EvalResult::ok(std::move(fn_val));
}

EvalResult Interpreter::eval_arrow_function_expr(const ArrowFunctionExpression& expr) {
    uint32_t length_count = static_cast<uint32_t>(expr.params.size());
    for (uint32_t i = 0; i < static_cast<uint32_t>(expr.params.size()); ++i) {
        if (expr.params[i].default_init != nullptr) {
            length_count = i;
            break;
        }
    }
    auto fn = RcPtr<JSFunction>::make();
    std::vector<std::string> param_names;
    param_names.reserve(expr.params.size());
    for (const auto& pd : expr.params) param_names.push_back(pd.name);
    fn->set_params(std::move(param_names));
    fn->set_param_defs(std::make_shared<std::vector<ParamDef>>(expr.params));
    fn->set_rest_param(expr.rest_param);
    fn->set_property("length", Value::number(static_cast<double>(length_count)));
    fn->set_body(expr.body_stmts);
    fn->set_closure_env(current_env_);
    fn->set_defining_module(current_module_);
    fn->set_arrow(true);
    fn->set_lexical_this(current_this_);
    // 箭头函数不创建 prototype 对象（不可 new）
    gc_heap_.Register(fn.get());
    return EvalResult::ok(Value::object(ObjectPtr(fn)));
}

EvalResult Interpreter::eval_call_expr(const CallExpression& expr) {
    if (call_depth_ >= kMaxCallDepth) {
        pending_throw_ = make_error_value(NativeErrorType::kRangeError,
            "Maximum call stack size exceeded");
        return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
    }

    Value this_val = Value::undefined();
    Value callee_val = Value::undefined();

    bool super_method_call = false;
    // Detect super.method() call — method lookup on home_object.__proto__, receiver = current_this_
    if (std::holds_alternative<SuperMemberExpression>(expr.callee->v)) {
        const auto& smember = std::get<SuperMemberExpression>(expr.callee->v);
        auto method_r = eval_super_member(smember);
        if (!method_r.is_ok()) return method_r;
        callee_val = method_r.value();
        this_val = current_this_;  // receiver is the current 'this'
        super_method_call = true;
    }
    // Detect method call: obj.method() — extract this from the object
    if (!super_method_call && std::holds_alternative<MemberExpression>(expr.callee->v)) {
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

        // Symbol key: look up symbol property on object or prototype
        if (key_result.value().is_symbol()) {
            uint64_t sym_id = key_result.value().as_symbol_id();
            if (this_val.is_string()) {
                // String primitive: look up symbol in string_prototype_
                if (string_prototype_) {
                    callee_val = string_prototype_->get_property_by_symbol(sym_id);
                } else {
                    callee_val = Value::undefined();
                }
            } else if (!this_val.is_object()) {
                pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                    "Cannot read properties of " + to_string_val(this_val));
                return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
            } else {
                RcObject* sym_raw = this_val.as_object_raw();
                if (sym_raw->object_kind() == ObjectKind::kOrdinary ||
                    sym_raw->object_kind() == ObjectKind::kArray ||
                    sym_raw->object_kind() == ObjectKind::kGenerator ||
                    sym_raw->object_kind() == ObjectKind::kMap ||
                    sym_raw->object_kind() == ObjectKind::kSet) {
                    callee_val = static_cast<JSObject*>(sym_raw)->get_property_by_symbol(sym_id);
                } else {
                    callee_val = Value::undefined();
                }
            }
            // Fall through to function call below
        } else {

        std::string key = to_string_val(key_result.value());

        if (this_val.is_string()) {
            if (string_prototype_) {
                callee_val = string_prototype_->get_property(key);
            } else {
                callee_val = Value::undefined();
            }
        } else if (this_val.is_symbol()) {
            if (symbol_prototype_) {
                callee_val = symbol_prototype_->get_property(key);
            } else {
                callee_val = Value::undefined();
            }
        } else if (this_val.is_number()) {
            if (number_prototype_) {
                callee_val = number_prototype_->get_property(key);
            } else {
                callee_val = Value::undefined();
            }
        } else if (this_val.is_bool()) {
            if (boolean_prototype_) {
                callee_val = boolean_prototype_->get_property(key);
            } else {
                callee_val = Value::undefined();
            }
        } else if (!this_val.is_object()) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "Cannot read properties of " + to_string_val(this_val));
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        } else {
        RcObject* obj_ptr = this_val.as_object_raw();
        if (obj_ptr->object_kind() == ObjectKind::kOrdinary || obj_ptr->object_kind() == ObjectKind::kArray ||
            obj_ptr->object_kind() == ObjectKind::kGenerator ||
            obj_ptr->object_kind() == ObjectKind::kMap || obj_ptr->object_kind() == ObjectKind::kSet ||
            obj_ptr->object_kind() == ObjectKind::kWeakMap || obj_ptr->object_kind() == ObjectKind::kWeakSet) {
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
        } else if (obj_ptr->object_kind() == ObjectKind::kRegExp) {
            if (regexp_prototype_) {
                callee_val = regexp_prototype_->get_property(key);
            } else {
                callee_val = Value::undefined();
            }
        } else if (obj_ptr->object_kind() == ObjectKind::kStringObject ||
                   obj_ptr->object_kind() == ObjectKind::kBooleanObject) {
            auto* js_obj = static_cast<JSObject*>(obj_ptr);
            callee_val = js_obj->get_property(key);
        } else {
            callee_val = Value::undefined();
        }
        }
        } // end of string-key else branch
    } else if (!super_method_call) {
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
        if (std::holds_alternative<SpreadElement>(arg_expr->v)) {
            const auto& sp = std::get<SpreadElement>(arg_expr->v);
            auto iterable_res = eval_expr(*sp.argument);
            if (!iterable_res.is_ok()) return iterable_res;
            if (!spread_into(iterable_res.value(), args)) {
                return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
            }
        } else {
            auto arg_result = eval_expr(*arg_expr);
            if (!arg_result.is_ok()) {
                return arg_result;
            }
            args.push_back(std::move(arg_result.value()));
        }
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

    // 箭头函数不可 new
    if (fn->is_arrow()) {
        pending_throw_ = make_error_value(NativeErrorType::kTypeError,
            "arrow function is not a constructor");
        return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
    }

    // Determine prototype for new object
    RcPtr<JSObject> proto = fn->prototype_obj() ? fn->prototype_obj() : object_prototype_;

    // Create new object with [[Prototype]] = F.prototype
    auto new_obj = RcPtr<JSObject>::make();
    gc_heap_.Register(new_obj.get());
    new_obj->set_proto(proto);

    std::vector<Value> args;
    args.reserve(expr.arguments.size());
    for (const auto& arg_expr : expr.arguments) {
        if (std::holds_alternative<SpreadElement>(arg_expr->v)) {
            const auto& sp = std::get<SpreadElement>(arg_expr->v);
            auto iterable_res = eval_expr(*sp.argument);
            if (!iterable_res.is_ok()) return iterable_res;
            if (!spread_into(iterable_res.value(), args)) {
                return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
            }
        } else {
            auto arg_result = eval_expr(*arg_expr);
            if (!arg_result.is_ok()) {
                return arg_result;
            }
            args.push_back(std::move(arg_result.value()));
        }
    }

    // Set new.target for the constructor call
    Value saved_new_target = current_new_target_;
    bool saved_derived_init = derived_this_initialized_;
    current_new_target_ = callee_val;

    Value this_val = Value::object(ObjectPtr(new_obj));

    // Derived class: this is set by super(), not pre-created
    if (fn->is_derived_ctor()) {
        // The this_val will be updated by super() call
        this_val = Value::undefined();
    }

    // Handle implicit derived ctor: rest_param == "$__class_impl_args__" and empty body
    bool is_implicit_derived = fn->is_derived_ctor() &&
                               fn->rest_param().has_value() &&
                               fn->rest_param().value() == "$__class_impl_args__" &&
                               fn->body() && fn->body()->empty();
    if (is_implicit_derived) {
        // Implicit derived ctor: call super with all args
        JSFunction* super_ctor_id = fn->fn_ctor_proto();
        if (super_ctor_id) {
            // Create new_obj with new_target prototype
            auto new_proto_id = fn->prototype_obj();
            auto new_obj_id = RcPtr<JSObject>::make();
            gc_heap_.Register(new_obj_id.get());
            if (new_proto_id) new_obj_id->set_proto(new_proto_id);
            else new_obj_id->set_proto(object_prototype_);
            Value new_obj_id_val = Value::object(ObjectPtr(new_obj_id));

            RcPtr<JSFunction> super_rc_id(super_ctor_id);
            auto super_r_id = call_function(super_rc_id, new_obj_id_val, args, /*is_new_call=*/true);
            current_new_target_ = saved_new_target;
            derived_this_initialized_ = saved_derived_init;
            if (!super_r_id.is_ok()) return EvalResult::err(super_r_id.error());

            Value super_ret_id = super_r_id.completion().value;
            Value result_id = (super_ret_id.is_object() && !super_ret_id.is_null())
                ? std::move(super_ret_id) : std::move(new_obj_id_val);
            // Implicit derived ctor: initialize instance fields after super() returns
            if (fn->instance_fields() && !fn->instance_fields()->empty()) {
                auto field_r_id = init_instance_fields(fn.get(), result_id);
                if (!field_r_id.is_ok()) return EvalResult::err(field_r_id.error());
            }
            return EvalResult::ok(result_id);
        }
    }

    derived_this_initialized_ = !fn->is_derived_ctor();  // base ctor: initialized; derived: not yet

    auto call_result = call_function(fn, this_val, std::move(args), /*is_new_call=*/true);
    // M4: capture derived_this_initialized_ before restoring (to detect missing super())
    bool derived_super_called = derived_this_initialized_;
    current_new_target_ = saved_new_target;
    derived_this_initialized_ = saved_derived_init;
    Value derived_this = last_new_this_;
    last_new_this_ = Value::undefined();

    if (!call_result.is_ok()) {
        return EvalResult::err(call_result.error());
    }
    if (call_result.completion().is_throw()) {
        pending_throw_ = call_result.completion().value;
        return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
    }

    // For derived class, this_val was set by super() call inside the constructor body
    if (fn->is_derived_ctor()) {
        this_val = derived_this;
    }

    // Only an explicit return <Object> overrides this_val (ECMAScript §10.2.2 step 9)
    const Completion& c = call_result.completion();
    if (c.is_return() && c.value.is_object() && c.value.as_object_raw() != nullptr) {
        return EvalResult::ok(c.value);
    }

    // M4: derived ctor returned without calling super() → ReferenceError
    if (fn->is_derived_ctor() && !derived_super_called) {
        pending_throw_ = make_error_value(NativeErrorType::kReferenceError,
            "Must call super constructor in derived class before returning from derived constructor");
        return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
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
        add_obj(boolean_prototype_.get());
        add_obj(string_prototype_.get());
        add_obj(math_obj_.get());
        add_obj(number_prototype_.get());
        add_obj(object_constructor_.get());
        add_obj(number_constructor_.get());
        add_obj(boolean_constructor_.get());
        add_obj(string_constructor_.get());
        add_obj(regexp_prototype_.get());
        add_obj(regexp_constructor_.get());
        add_obj(symbol_prototype_.get());
        add_obj(symbol_constructor_.get());
        add_obj(generator_prototype_.get());
        add_obj(map_prototype_.get());
        add_obj(set_prototype_.get());
        add_obj(weakmap_prototype_.get());
        add_obj(weakset_prototype_.get());
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
    if (boolean_prototype_) boolean_prototype_->clear_function_properties();
    if (string_prototype_) string_prototype_->clear_function_properties();
    if (math_obj_) math_obj_->clear_function_properties();
    if (number_prototype_) number_prototype_->clear_function_properties();
    if (regexp_prototype_) regexp_prototype_->clear_function_properties();
    if (symbol_prototype_) symbol_prototype_->clear_function_properties();
    if (generator_prototype_) generator_prototype_->clear_function_properties();
    if (map_prototype_) map_prototype_->clear_function_properties();
    if (set_prototype_) set_prototype_->clear_function_properties();
    if (weakmap_prototype_) weakmap_prototype_->clear_function_properties();
    if (weakset_prototype_) weakset_prototype_->clear_function_properties();
    if (object_constructor_) object_constructor_->clear_own_properties();
    if (number_constructor_) number_constructor_->clear_own_properties();
    if (boolean_constructor_) boolean_constructor_->clear_own_properties();
    if (string_constructor_) string_constructor_->clear_own_properties();
    if (regexp_constructor_) regexp_constructor_->clear_own_properties();
    if (symbol_constructor_) symbol_constructor_->clear_own_properties();
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
                        fd->name, fd->params, fd->body, current_env_, false, fd->rest_param);
                    current_env_->set(fd->name, fn_val);
                } else if (const auto* afd = std::get_if<AsyncFunctionDeclaration>(&exp->declaration->v)) {
                    Value fn_val = make_async_function_value(
                        afd->name, afd->params, afd->body, current_env_, afd->rest_param);
                    current_env_->set(afd->name, fn_val);
                }
            }
        } else if (const auto* afd = std::get_if<AsyncFunctionDeclaration>(&stmt.v)) {
            // 顶层非导出 async function 声明：P2-C 中 eval_async_function_decl 是 no-op，
            // 需在此处提升赋值（与 hoist_vars_stmt 对普通 exec() 的处理对齐）
            Value fn_val = make_async_function_value(
                afd->name, afd->params, afd->body, current_env_, afd->rest_param);
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

// ---- 模板字符串 ----

EvalResult Interpreter::eval_template_literal(const TemplateLiteral& node) {
    std::string result;
    size_t min_len = 0;
    for (const auto& q : node.quasis) min_len += q.cooked.size();
    result.reserve(min_len);

    for (size_t i = 0; i < node.quasis.size(); ++i) {
        result += node.quasis[i].cooked;
        if (i < node.expressions.size()) {
            auto res = eval_expr(*node.expressions[i]);
            if (!res.is_ok()) return res;
            Value expr_val = res.value();
            // Check [Symbol.toPrimitive] with "string" hint for objects
            if (expr_val.is_object()) {
                RcObject* raw = expr_val.as_object_raw();
                if (raw && raw->object_kind() != ObjectKind::kFunction) {
                    auto* obj = static_cast<JSObject*>(raw);
                    const JSObject::SymbolPropertyEntry* entry =
                        obj->find_symbol_entry(symbol_table_.well_known_to_primitive);
                    if (entry && !entry->value.is_undefined()) {
                        Value hint = Value::string("string");
                        auto prim_res = call_function_val(entry->value, expr_val,
                                                          std::span<Value>(&hint, 1));
                        if (!prim_res.is_ok()) return prim_res;
                        expr_val = prim_res.value();
                    }
                }
            }
            result += to_string_val(expr_val);
        }
    }
    return EvalResult::ok(Value::string(result));
}

EvalResult Interpreter::eval_tagged_template_expr(const TaggedTemplateExpression& expr) {
    // Evaluate tag function (and receiver for method calls)
    Value tag_fn;
    Value receiver = Value::undefined();
    if (auto* mem = std::get_if<MemberExpression>(&expr.tag->v)) {
        auto obj_r = eval_expr(*mem->object);
        if (!obj_r.is_ok()) return obj_r;
        receiver = obj_r.value();
        Value prop_key;
        if (mem->computed) {
            auto kr = eval_expr(*mem->property);
            if (!kr.is_ok()) return kr;
            prop_key = kr.value();
        } else {
            prop_key = Value::string(std::get<StringLiteral>(mem->property->v).value);
        }
        auto prop_r = eval_get_property_of(receiver, prop_key);
        if (!prop_r.is_ok()) return prop_r;
        tag_fn = prop_r.value();
    } else {
        auto tag_r = eval_expr(*expr.tag);
        if (!tag_r.is_ok()) return tag_r;
        tag_fn = tag_r.value();
    }

    const TemplateLiteral& tmpl = expr.tmpl;

    // Build strings array
    auto strings = RcPtr<JSObject>::make(ObjectKind::kArray);
    gc_heap_.Register(strings.get());
    strings->array_length_ = static_cast<uint32_t>(tmpl.quasis.size());
    for (size_t i = 0; i < tmpl.quasis.size(); ++i) {
        strings->elements_[static_cast<uint32_t>(i)] = Value::string(tmpl.quasis[i].cooked);
    }

    // Build raw array
    auto raw_arr = RcPtr<JSObject>::make(ObjectKind::kArray);
    gc_heap_.Register(raw_arr.get());
    raw_arr->array_length_ = static_cast<uint32_t>(tmpl.quasis.size());
    for (size_t i = 0; i < tmpl.quasis.size(); ++i) {
        raw_arr->elements_[static_cast<uint32_t>(i)] = Value::string(tmpl.quasis[i].raw);
    }
    strings->set_property("raw", Value::object(ObjectPtr(raw_arr)));

    // Build args: [strings, ...expressions]
    std::vector<Value> call_args;
    call_args.push_back(Value::object(ObjectPtr(strings)));
    for (const auto& e : tmpl.expressions) {
        auto res = eval_expr(*e);
        if (!res.is_ok()) return res;
        call_args.push_back(res.value());
    }

    return call_function_val(tag_fn, receiver, call_args);
}

// ---- ++/-- update expressions ----

EvalResult Interpreter::eval_update_expr(const UpdateExpression& expr) {
    if (std::holds_alternative<MemberExpression>(expr.operand->v)) {
        const auto& member = std::get<MemberExpression>(expr.operand->v);

        auto obj_result = eval_expr(*member.object);
        if (!obj_result.is_ok()) return obj_result;
        if (!obj_result.value().is_object()) return EvalResult::ok(Value::undefined());
        ObjectPtr obj = obj_result.value().as_object();
        RcObject* raw = obj.get();

        std::string key;
        if (member.computed) {
            auto key_result = eval_expr(*member.property);
            if (!key_result.is_ok()) return key_result;
            key = to_string_val(key_result.value());
        } else {
            key = std::get<StringLiteral>(member.property->v).value;
        }

        Value old_val;
        if (raw->object_kind() == ObjectKind::kOrdinary || raw->object_kind() == ObjectKind::kArray) {
            old_val = static_cast<JSObject*>(raw)->get_property(key);
        } else {
            return EvalResult::ok(Value::undefined());
        }

        auto old_num = to_number(old_val);
        if (!old_num.is_ok()) return old_num;
        double old_d = old_num.value().as_number();
        double delta = (expr.op == UpdateOp::Inc) ? 1.0 : -1.0;
        double new_d = old_d + delta;

        if (raw->object_kind() == ObjectKind::kOrdinary || raw->object_kind() == ObjectKind::kArray) {
            auto set_ex_res = static_cast<JSObject*>(raw)->set_property_ex(key, Value::number(new_d));
            if (!set_ex_res.is_ok()) {
                const std::string& msg = set_ex_res.error().message();
                NativeErrorType err_type = NativeErrorType::kRangeError;
                if (msg.rfind("TypeError:", 0) == 0) err_type = NativeErrorType::kTypeError;
                pending_throw_ = make_error_value(err_type, strip_error_prefix(msg));
                return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
            }
        }

        return EvalResult::ok(Value::number(expr.prefix ? new_d : old_d));
    }

    // PrivateMemberExpression: e.g., ++this.#count or ++C.#staticCount
    if (std::holds_alternative<PrivateMemberExpression>(expr.operand->v)) {
        const auto& pme = std::get<PrivateMemberExpression>(expr.operand->v);
        auto obj_r = eval_expr(*pme.object);
        if (!obj_r.is_ok()) return obj_r;
        Value obj_val = obj_r.value();
        if (!obj_val.is_object()) {
            return EvalResult::ok(Value::number(std::numeric_limits<double>::quiet_NaN()));
        }
        uint64_t sym_id = current_function_ ? current_function_->get_private_field_sym(pme.field_name) : 0;
        if (sym_id == 0) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "Private field '" + pme.field_name + "' not found");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        RcObject* raw_upd = obj_val.as_object_raw();
        Value old_val;
        if (raw_upd->object_kind() == ObjectKind::kFunction) {
            // Static private field: stored as string property on JSFunction
            old_val = static_cast<JSFunction*>(raw_upd)->get_property(pme.field_name);
        } else {
            auto* obj_pf = static_cast<JSObject*>(raw_upd);
            const JSObject::SymbolPropertyEntry* entry = obj_pf->find_symbol_entry(sym_id);
            old_val = (entry && !entry->is_accessor) ? entry->value : Value::undefined();
        }
        double old_d;
        if (!old_val.is_number()) {
            auto n = to_number(old_val);
            if (!n.is_ok()) return n;
            old_d = n.value().as_number();
        } else {
            old_d = old_val.as_number();
        }
        double delta = (expr.op == UpdateOp::Inc) ? 1.0 : -1.0;
        double new_d = old_d + delta;
        if (raw_upd->object_kind() == ObjectKind::kFunction) {
            static_cast<JSFunction*>(raw_upd)->set_property(pme.field_name, Value::number(new_d));
        } else {
            static_cast<JSObject*>(raw_upd)->set_property_by_symbol(sym_id, Value::number(new_d));
        }
        return EvalResult::ok(Value::number(expr.prefix ? new_d : old_d));
    }

    const auto& ident = std::get<Identifier>(expr.operand->v);
    auto get_result = current_env_->get(ident.name);
    if (!get_result.is_ok()) {
        return get_result;
    }
    Value old_val = get_result.value();
    auto old_num = to_number(old_val);
    if (!old_num.is_ok()) return old_num;
    double old_d = old_num.value().as_number();
    double delta = (expr.op == UpdateOp::Inc) ? 1.0 : -1.0;
    double new_d = old_d + delta;

    auto set_result = current_env_->set(ident.name, Value::number(new_d));
    if (!set_result.is_ok()) {
        const std::string& msg = set_result.error().message();
        NativeErrorType err_type = NativeErrorType::kTypeError;
        if (msg.rfind("ReferenceError:", 0) == 0) err_type = NativeErrorType::kReferenceError;
        pending_throw_ = make_error_value(err_type, strip_error_prefix(msg));
        return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
    }
    return EvalResult::ok(Value::number(expr.prefix ? new_d : old_d));
}

// ---- async/await ----

Value Interpreter::make_async_function_value(std::optional<std::string> name,
                                              const std::vector<ParamDef>& params,
                                              std::shared_ptr<std::vector<StmtNode>> body,
                                              RcPtr<Environment> closure_env,
                                              std::optional<std::string> rest_param) {
    // 计算 length_count（第一个有默认值或解构参数的索引）
    uint32_t length_count = static_cast<uint32_t>(params.size());
    for (uint32_t i = 0; i < static_cast<uint32_t>(params.size()); ++i) {
        if (params[i].default_init != nullptr || params[i].pattern_binding != nullptr) {
            length_count = i; break;
        }
    }

    // Create a native JSFunction that wraps the async body execution
    auto fn = RcPtr<JSFunction>::make();
    fn->set_name(name);
    std::vector<std::string> param_names;
    param_names.reserve(params.size());
    for (const auto& pd : params) param_names.push_back(pd.name);
    fn->set_params(param_names);
    fn->set_rest_param(rest_param);
    fn->set_property("length", Value::number(static_cast<double>(length_count)));
    fn->set_body(body);
    fn->set_closure_env(closure_env);
    fn->set_defining_module(current_module_);

    auto proto_obj = RcPtr<JSObject>::make();
    proto_obj->set_proto(object_prototype_);
    proto_obj->set_constructor_property(fn.get());
    fn->set_prototype_obj(proto_obj);

    // P2-D: capture fn as raw pointer for the self-reference binding inside the body.
    JSFunction* fn_self_raw = fn.get();

    // 捕获 param_defs 供 lambda 内默认值求值
    auto param_defs_captured = std::make_shared<std::vector<ParamDef>>(params);

    // The async wrapper: creates outer_promise, executes body, fulfills/rejects
    fn->set_native_fn([this, body, param_defs_captured, closure_env, name, fn_self_raw,
                       rest_param](
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

        // Bind parameters（支持默认值和解构参数）
        {
            RcPtr<Environment> old_env = current_env_;
            RcPtr<Environment> old_var_env = var_env_;
            current_env_ = fn_env;
            var_env_ = fn_env;
            const auto& defs = *param_defs_captured;
            bool param_error = false;
            for (size_t i = 0; i < defs.size(); ++i) {
                Value arg_val = (i < call_args.size()) ? call_args[i] : Value::undefined();
                if (defs[i].pattern_binding != nullptr) {
                    // 解构参数：先预声明所有解构出的变量名
                    std::vector<std::string> pat_names;
                    collect_pattern_names(*defs[i].pattern_binding, pat_names);
                    for (const auto& pn : pat_names) {
                        fn_env->define(pn, VarKind::Var);
                    }
                    if (arg_val.is_undefined() && defs[i].default_init != nullptr) {
                        auto default_r = eval_expr(*defs[i].default_init);
                        if (!default_r.is_ok()) {
                            current_env_ = old_env;
                            var_env_ = old_var_env;
                            if (pending_throw_.has_value()) {
                                outer_promise->Reject(std::move(*pending_throw_), job_queue_);
                                pending_throw_ = std::nullopt;
                            } else {
                                outer_promise->Reject(
                                    make_error_value(NativeErrorType::kTypeError,
                                                     default_r.error().message()), job_queue_);
                            }
                            return EvalResult::ok(outer_val);
                        }
                        arg_val = default_r.value();
                    }
                    auto bind_r = bind_pattern(*defs[i].pattern_binding, std::move(arg_val),
                                               VarKind::Var, false);
                    if (!bind_r.is_ok()) {
                        current_env_ = old_env;
                        var_env_ = old_var_env;
                        if (pending_throw_.has_value()) {
                            outer_promise->Reject(std::move(*pending_throw_), job_queue_);
                            pending_throw_ = std::nullopt;
                        } else {
                            outer_promise->Reject(
                                make_error_value(NativeErrorType::kTypeError,
                                                 bind_r.error().message()), job_queue_);
                        }
                        param_error = true;
                        break;
                    }
                } else {
                    if (arg_val.is_undefined() && defs[i].default_init != nullptr) {
                        auto default_r = eval_expr(*defs[i].default_init);
                        if (!default_r.is_ok()) {
                            current_env_ = old_env;
                            var_env_ = old_var_env;
                            if (pending_throw_.has_value()) {
                                outer_promise->Reject(std::move(*pending_throw_), job_queue_);
                                pending_throw_ = std::nullopt;
                            } else {
                                outer_promise->Reject(
                                    make_error_value(NativeErrorType::kTypeError,
                                                     default_r.error().message()), job_queue_);
                            }
                            return EvalResult::ok(outer_val);
                        }
                        arg_val = default_r.value();
                    }
                    fn_env->define(defs[i].name, VarKind::Var);
                    fn_env->initialize(defs[i].name, std::move(arg_val));
                }
            }
            current_env_ = old_env;
            var_env_ = old_var_env;
            if (param_error) return EvalResult::ok(outer_val);
        }

        // Bind rest parameter
        if (rest_param.has_value()) {
            auto rest_arr = RcPtr<JSObject>::make(ObjectKind::kArray);
            gc_heap_.Register(rest_arr.get());
            rest_arr->set_proto(array_prototype_);
            size_t rest_start = param_defs_captured->size();
            for (size_t i = rest_start; i < call_args.size(); ++i) {
                rest_arr->elements_[static_cast<uint32_t>(i - rest_start)] = call_args[i];
            }
            rest_arr->array_length_ = static_cast<uint32_t>(
                call_args.size() > rest_start ? call_args.size() - rest_start : 0);
            fn_env->define(rest_param.value(), VarKind::Var);
            fn_env->initialize(rest_param.value(), Value::object(ObjectPtr(rest_arr)));
        }

        hoist_vars(*body, *fn_env);
        run_async_body(body, 0, fn_env, std::move(this_val_arg), outer_promise);
        return EvalResult::ok(outer_val);
    });

    gc_heap_.Register(fn.get());
    gc_heap_.Register(proto_obj.get());
    return Value::object(ObjectPtr(fn));
}

// ============================================================
// ag_resume: async generator body runner
// Resumes the generator body with async support (handles both yield and await).
// Resolves outer_promise when yield or completion; suspends when await is hit.
// ============================================================

void Interpreter::ag_resume(RcPtr<JSGeneratorObject> gen, RcPtr<JSPromise> outer_promise) {
    auto& body = *gen->gen_body_;

    // Save interpreter state
    RcPtr<Environment> saved_env = current_env_;
    RcPtr<Environment> saved_var_env = var_env_;
    Value saved_this = current_this_;
    JSPromise* saved_async_promise = current_async_promise_;
    bool saved_in_async = in_async_body_;

    // Switch to generator's environment and enable async context
    current_env_ = gen->gen_env_;
    var_env_ = gen->gen_env_;
    current_this_ = gen->gen_this_val_;
    current_async_promise_ = outer_promise.get();
    in_async_body_ = true;

    // Hoist vars on first execution
    if (!gen->vars_hoisted_) {
        hoist_vars(body, *gen->gen_env_);
        gen->vars_hoisted_ = true;
    }

    gen->state_ = GeneratorState::kExecuting;

    // Restore delegate iterator
    current_yield_delegate_iter_ = std::move(gen->yield_delegate_iter_);
    current_yield_delegate_next_ = std::move(gen->yield_delegate_next_);

    auto restore_state = [&]() {
        current_env_ = saved_env;
        var_env_ = saved_var_env;
        current_this_ = saved_this;
        current_async_promise_ = saved_async_promise;
        in_async_body_ = saved_in_async;
        in_generator_resume_mode_ = false;
        pending_generator_resume_value_ = std::nullopt;
        in_generator_throw_mode_ = false;
        pending_generator_throw_value_ = std::nullopt;
        gen->yield_delegate_iter_ = std::move(current_yield_delegate_iter_);
        gen->yield_delegate_next_ = std::move(current_yield_delegate_next_);
        current_yield_delegate_iter_ = Value::undefined();
        current_yield_delegate_next_ = Value::undefined();
    };

    for (size_t i = gen->suspended_stmt_index_; i < body.size(); ++i) {
        auto stmt_result = eval_stmt(body[i]);
        if (!stmt_result.is_ok()) {
            const std::string& em = stmt_result.error().message();

            if (em == kGeneratorYieldSentinel) {
                // yield expr: resolve outer_promise with {value, done:false}
                gen->state_ = GeneratorState::kSuspendedYield;
                gen->suspended_stmt_index_ = i;
                Value yield_val = pending_generator_yield_value_.has_value()
                    ? std::move(*pending_generator_yield_value_) : Value::undefined();
                pending_generator_yield_value_ = std::nullopt;
                restore_state();
                auto result_obj = RcPtr<JSObject>::make();
                gc_heap_.Register(result_obj.get());
                result_obj->set_proto(object_prototype_);
                result_obj->set_property("value", std::move(yield_val));
                result_obj->set_property("done", Value::boolean(false));
                outer_promise->Fulfill(Value::object(ObjectPtr(result_obj)), job_queue_);
                return;
            }

            if (em == kAsyncSuspendSentinel) {
                // await expr: save state, set up resume when inner promise resolves
                gen->state_ = GeneratorState::kSuspendedYield;  // reuse SuspendedYield state
                gen->suspended_stmt_index_ = i;
                restore_state();

                if (!pending_inner_promise_.has_value()) {
                    outer_promise->Reject(
                        make_error_value(NativeErrorType::kTypeError, "internal: missing inner promise"),
                        job_queue_);
                    return;
                }
                auto inner_promise = std::move(*pending_inner_promise_);
                pending_inner_promise_ = std::nullopt;

                // resume_fn: called when inner promise fulfills
                auto resume_fn = RcPtr<JSFunction>::make();
                gc_heap_.Register(resume_fn.get());
                resume_fn->set_native_fn([this, gen, outer_promise]
                        (Value /*this_val*/, std::vector<Value> args, bool) mutable -> EvalResult {
                    Value resolved_val = args.empty() ? Value::undefined() : args[0];
                    pending_await_result_ = std::move(resolved_val);
                    in_generator_resume_mode_ = false;
                    gen->state_ = GeneratorState::kSuspendedStart;  // allow resume
                    ag_resume(gen, outer_promise);
                    return EvalResult::ok(Value::undefined());
                });

                // reject_fn: called when inner promise rejects
                auto reject_fn = RcPtr<JSFunction>::make();
                gc_heap_.Register(reject_fn.get());
                reject_fn->set_native_fn([this, gen, outer_promise]
                        (Value /*this_val*/, std::vector<Value> args, bool) mutable -> EvalResult {
                    Value reason = args.empty() ? Value::undefined() : args[0];
                    pending_throw_ = std::move(reason);
                    in_generator_throw_mode_ = true;
                    gen->state_ = GeneratorState::kSuspendedStart;
                    ag_resume(gen, outer_promise);
                    return EvalResult::ok(Value::undefined());
                });

                JSPromise::PerformThen(
                    inner_promise,
                    Value::object(ObjectPtr(resume_fn)),
                    Value::object(ObjectPtr(reject_fn)),
                    job_queue_);
                drain_job_queue();
                return;
            }

            // Other error: mark completed, reject outer_promise
            gen->state_ = GeneratorState::kCompleted;
            Value err_val;
            if (em == kPendingThrowSentinel && pending_throw_.has_value()) {
                err_val = std::move(*pending_throw_);
                pending_throw_ = std::nullopt;
            } else {
                err_val = Value::string(em);
            }
            restore_state();
            outer_promise->Reject(std::move(err_val), job_queue_);
            return;
        }

        const Completion& c = stmt_result.completion();
        if (c.is_return()) {
            gen->state_ = GeneratorState::kCompleted;
            Value ret_val = c.value;
            restore_state();
            auto result_obj = RcPtr<JSObject>::make();
            gc_heap_.Register(result_obj.get());
            result_obj->set_proto(object_prototype_);
            result_obj->set_property("value", std::move(ret_val));
            result_obj->set_property("done", Value::boolean(true));
            outer_promise->Fulfill(Value::object(ObjectPtr(result_obj)), job_queue_);
            return;
        }
        if (c.is_throw()) {
            gen->state_ = GeneratorState::kCompleted;
            Value thrown = c.value;
            restore_state();
            outer_promise->Reject(std::move(thrown), job_queue_);
            return;
        }
    }

    // Body completed normally
    gen->state_ = GeneratorState::kCompleted;
    restore_state();
    auto result_obj = RcPtr<JSObject>::make();
    gc_heap_.Register(result_obj.get());
    result_obj->set_proto(object_prototype_);
    result_obj->set_property("value", Value::undefined());
    result_obj->set_property("done", Value::boolean(true));
    outer_promise->Fulfill(Value::object(ObjectPtr(result_obj)), job_queue_);
}

// ============================================================
// make_async_generator_value
// ============================================================

Value Interpreter::make_async_generator_value(std::optional<std::string> name,
                                               const std::vector<ParamDef>& params,
                                               std::shared_ptr<std::vector<StmtNode>> body,
                                               RcPtr<Environment> closure_env,
                                               std::optional<std::string> rest_param) {
    uint32_t length_count = static_cast<uint32_t>(params.size());
    for (uint32_t i = 0; i < static_cast<uint32_t>(params.size()); ++i) {
        if (params[i].default_init != nullptr || params[i].pattern_binding != nullptr) {
            length_count = i; break;
        }
    }

    auto fn = RcPtr<JSFunction>::make();
    fn->set_name(name);
    fn->set_property("length", Value::number(static_cast<double>(length_count)));
    fn->set_is_async_generator(true);

    auto param_defs_captured = std::make_shared<std::vector<ParamDef>>(params);

    fn->set_native_fn([this, body, param_defs_captured, closure_env, rest_param]
            (Value this_val_arg, std::vector<Value> call_args, bool) mutable -> EvalResult {
        // Set up function environment
        RcPtr<Environment> outer_env = closure_env ? closure_env : global_env_;
        auto fn_env = RcPtr<Environment>::make(outer_env);
        gc_heap_.Register(fn_env.get());

        // Bind parameters（支持默认值和解构参数）
        {
            RcPtr<Environment> old_env = current_env_;
            RcPtr<Environment> old_var_env = var_env_;
            current_env_ = fn_env;
            var_env_ = fn_env;
            const auto& defs = *param_defs_captured;
            for (size_t i = 0; i < defs.size(); ++i) {
                Value arg_val = (i < call_args.size()) ? call_args[i] : Value::undefined();
                if (defs[i].pattern_binding != nullptr) {
                    // 先预声明所有解构出的变量名
                    std::vector<std::string> pat_names;
                    collect_pattern_names(*defs[i].pattern_binding, pat_names);
                    for (const auto& pn : pat_names) {
                        fn_env->define(pn, VarKind::Var);
                    }
                    if (arg_val.is_undefined() && defs[i].default_init != nullptr) {
                        auto default_r = eval_expr(*defs[i].default_init);
                        if (!default_r.is_ok()) {
                            current_env_ = old_env;
                            var_env_ = old_var_env;
                            if (pending_throw_.has_value()) pending_throw_ = std::nullopt;
                            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                                              "Error in default param");
                            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
                        }
                        arg_val = default_r.value();
                    }
                    auto bind_r = bind_pattern(*defs[i].pattern_binding, std::move(arg_val),
                                               VarKind::Var, false);
                    if (!bind_r.is_ok()) {
                        current_env_ = old_env;
                        var_env_ = old_var_env;
                        return EvalResult::err(bind_r.error());
                    }
                } else {
                    if (arg_val.is_undefined() && defs[i].default_init != nullptr) {
                        auto default_r = eval_expr(*defs[i].default_init);
                        if (!default_r.is_ok()) {
                            current_env_ = old_env;
                            var_env_ = old_var_env;
                            if (pending_throw_.has_value()) pending_throw_ = std::nullopt;
                            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                                              "Error in default param");
                            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
                        }
                        arg_val = default_r.value();
                    }
                    fn_env->define(defs[i].name, VarKind::Var);
                    fn_env->initialize(defs[i].name, std::move(arg_val));
                }
            }
            current_env_ = old_env;
            var_env_ = old_var_env;
        }

        // Bind rest parameter
        if (rest_param.has_value()) {
            auto rest_arr = RcPtr<JSObject>::make(ObjectKind::kArray);
            gc_heap_.Register(rest_arr.get());
            rest_arr->set_proto(array_prototype_);
            size_t rest_start = param_defs_captured->size();
            for (size_t i = rest_start; i < call_args.size(); ++i) {
                rest_arr->elements_[static_cast<uint32_t>(i - rest_start)] = call_args[i];
            }
            rest_arr->array_length_ = static_cast<uint32_t>(
                call_args.size() > rest_start ? call_args.size() - rest_start : 0);
            fn_env->define(rest_param.value(), VarKind::Var);
            fn_env->initialize(rest_param.value(), Value::object(ObjectPtr(rest_arr)));
        }

        // Create the generator object (NOT executing the body yet)
        auto gen_obj = RcPtr<JSGeneratorObject>::make();
        gc_heap_.Register(gen_obj.get());
        gen_obj->set_proto(generator_prototype_);
        gen_obj->state_ = GeneratorState::kSuspendedStart;
        gen_obj->gen_body_ = body;
        gen_obj->gen_env_ = fn_env;
        gen_obj->gen_this_val_ = this_val_arg;
        gen_obj->suspended_stmt_index_ = 0;
        gen_obj->vars_hoisted_ = false;

        Value gen_val = Value::object(ObjectPtr(gen_obj));

        // Set up .next() — returns Promise
        {
            auto next_fn = RcPtr<JSFunction>::make();
            gc_heap_.Register(next_fn.get());
            next_fn->set_name(std::string("next"));
            next_fn->set_property("length", Value::number(1));
            next_fn->set_native_fn([this, gen_obj](Value /*this_val*/, std::vector<Value> args, bool) mutable -> EvalResult {
                if (gen_obj->state_ == GeneratorState::kCompleted) {
                    // Already done: return resolved Promise with {value:undefined, done:true}
                    auto result_obj = RcPtr<JSObject>::make();
                    gc_heap_.Register(result_obj.get());
                    result_obj->set_proto(object_prototype_);
                    result_obj->set_property("value", Value::undefined());
                    result_obj->set_property("done", Value::boolean(true));
                    auto done_promise = RcPtr<JSPromise>::make();
                    gc_heap_.Register(done_promise.get());
                    done_promise->Fulfill(Value::object(ObjectPtr(result_obj)), job_queue_);
                    drain_job_queue();
                    return EvalResult::ok(Value::object(ObjectPtr(done_promise)));
                }
                // Set up resume value if provided
                Value resume_val = args.empty() ? Value::undefined() : args[0];
                if (gen_obj->state_ == GeneratorState::kSuspendedYield) {
                    in_generator_resume_mode_ = true;
                    pending_generator_resume_value_ = std::move(resume_val);
                }
                // Create outer Promise
                auto outer_promise = RcPtr<JSPromise>::make();
                gc_heap_.Register(outer_promise.get());
                // Resume the async generator body
                ag_resume(gen_obj, outer_promise);
                drain_job_queue();
                return EvalResult::ok(Value::object(ObjectPtr(outer_promise)));
            });
            gen_obj->set_property("next", Value::object(ObjectPtr(next_fn)));
        }

        // Set up .return() — returns Promise
        {
            auto ret_fn = RcPtr<JSFunction>::make();
            gc_heap_.Register(ret_fn.get());
            ret_fn->set_name(std::string("return"));
            ret_fn->set_property("length", Value::number(1));
            ret_fn->set_native_fn([this, gen_obj](Value /*this_val*/, std::vector<Value> args, bool) mutable -> EvalResult {
                Value ret_val = args.empty() ? Value::undefined() : args[0];
                gen_obj->state_ = GeneratorState::kCompleted;
                auto result_obj = RcPtr<JSObject>::make();
                gc_heap_.Register(result_obj.get());
                result_obj->set_proto(object_prototype_);
                result_obj->set_property("value", std::move(ret_val));
                result_obj->set_property("done", Value::boolean(true));
                auto ret_promise = RcPtr<JSPromise>::make();
                gc_heap_.Register(ret_promise.get());
                ret_promise->Fulfill(Value::object(ObjectPtr(result_obj)), job_queue_);
                drain_job_queue();
                return EvalResult::ok(Value::object(ObjectPtr(ret_promise)));
            });
            gen_obj->set_property("return", Value::object(ObjectPtr(ret_fn)));
        }

        // Set up .throw() — returns Promise
        {
            auto throw_fn = RcPtr<JSFunction>::make();
            gc_heap_.Register(throw_fn.get());
            throw_fn->set_name(std::string("throw"));
            throw_fn->set_property("length", Value::number(1));
            throw_fn->set_native_fn([this, gen_obj](Value /*this_val*/, std::vector<Value> args, bool) mutable -> EvalResult {
                Value throw_val = args.empty() ? Value::undefined() : args[0];
                if (gen_obj->state_ == GeneratorState::kCompleted) {
                    auto throw_promise = RcPtr<JSPromise>::make();
                    gc_heap_.Register(throw_promise.get());
                    throw_promise->Reject(std::move(throw_val), job_queue_);
                    drain_job_queue();
                    return EvalResult::ok(Value::object(ObjectPtr(throw_promise)));
                }
                // Inject throw into generator
                in_generator_throw_mode_ = true;
                pending_generator_throw_value_ = std::move(throw_val);
                auto outer_promise = RcPtr<JSPromise>::make();
                gc_heap_.Register(outer_promise.get());
                ag_resume(gen_obj, outer_promise);
                drain_job_queue();
                return EvalResult::ok(Value::object(ObjectPtr(outer_promise)));
            });
            gen_obj->set_property("throw", Value::object(ObjectPtr(throw_fn)));
        }

        return EvalResult::ok(gen_val);
    });

    gc_heap_.Register(fn.get());
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
    if (expr.is_generator) {
        return EvalResult::ok(make_async_generator_value(
            expr.name, expr.params, expr.body, current_env_, expr.rest_param));
    }
    return EvalResult::ok(make_async_function_value(
        expr.name, expr.params, expr.body, current_env_, expr.rest_param));
}

StmtResult Interpreter::eval_async_function_decl(const AsyncFunctionDeclaration& /*stmt*/) {
    // P2-C: async function declarations are hoisted and assigned in hoist_vars_stmt; skip here.
    return StmtResult::ok(Completion::normal(Value::undefined()));
}

// ---- Generator helpers ----

EvalResult Interpreter::generator_resume(RcPtr<JSGeneratorObject> gen) {
    auto& body = *gen->gen_body_;

    // Save interpreter state
    RcPtr<Environment> saved_env = current_env_;
    RcPtr<Environment> saved_var_env = var_env_;
    Value saved_this = current_this_;

    // Switch to generator's environment
    current_env_ = gen->gen_env_;
    var_env_ = gen->gen_env_;
    current_this_ = gen->gen_this_val_;

    // Hoist vars on first execution
    if (!gen->vars_hoisted_) {
        hoist_vars(body, *gen->gen_env_);
        gen->vars_hoisted_ = true;
    }

    gen->state_ = GeneratorState::kExecuting;

    // Restore any in-progress yield* delegate iterator
    current_yield_delegate_iter_ = std::move(gen->yield_delegate_iter_);
    current_yield_delegate_next_ = std::move(gen->yield_delegate_next_);

    auto restore_state = [&]() {
        current_env_ = saved_env;
        var_env_ = saved_var_env;
        current_this_ = saved_this;
        in_generator_resume_mode_ = false;
        pending_generator_resume_value_ = std::nullopt;
        in_generator_throw_mode_ = false;
        pending_generator_throw_value_ = std::nullopt;
        // Save delegate iterator back to generator
        gen->yield_delegate_iter_ = std::move(current_yield_delegate_iter_);
        gen->yield_delegate_next_ = std::move(current_yield_delegate_next_);
        current_yield_delegate_iter_ = Value::undefined();
        current_yield_delegate_next_ = Value::undefined();
    };

    for (size_t i = gen->suspended_stmt_index_; i < body.size(); ++i) {
        auto stmt_result = eval_stmt(body[i]);
        if (!stmt_result.is_ok()) {
            const std::string& em = stmt_result.error().message();
            if (em == kGeneratorYieldSentinel) {
                // Yield: save state and return iter result
                gen->state_ = GeneratorState::kSuspendedYield;
                gen->suspended_stmt_index_ = i;
                Value yield_val = pending_generator_yield_value_.has_value()
                    ? std::move(*pending_generator_yield_value_) : Value::undefined();
                pending_generator_yield_value_ = std::nullopt;
                restore_state();
                // Build result object manually (gc_heap_ needed)
                auto result_obj = RcPtr<JSObject>::make();
                gc_heap_.Register(result_obj.get());
                result_obj->set_proto(object_prototype_);
                result_obj->set_property("value", std::move(yield_val));
                result_obj->set_property("done", Value::boolean(false));
                return EvalResult::ok(Value::object(ObjectPtr(result_obj)));
            }
            // Other error: mark completed and propagate
            gen->state_ = GeneratorState::kCompleted;
            restore_state();
            return EvalResult::err(stmt_result.error());
        }
        const Completion& c = stmt_result.completion();
        if (c.is_return()) {
            gen->state_ = GeneratorState::kCompleted;
            Value ret_val = c.value;
            restore_state();
            auto result_obj = RcPtr<JSObject>::make();
            gc_heap_.Register(result_obj.get());
            result_obj->set_proto(object_prototype_);
            result_obj->set_property("value", std::move(ret_val));
            result_obj->set_property("done", Value::boolean(true));
            return EvalResult::ok(Value::object(ObjectPtr(result_obj)));
        }
        if (c.is_throw()) {
            gen->state_ = GeneratorState::kCompleted;
            Value thrown = c.value;
            restore_state();
            pending_throw_ = std::move(thrown);
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
    }

    // Body completed normally
    gen->state_ = GeneratorState::kCompleted;
    restore_state();
    auto result_obj = RcPtr<JSObject>::make();
    gc_heap_.Register(result_obj.get());
    result_obj->set_proto(object_prototype_);
    result_obj->set_property("value", Value::undefined());
    result_obj->set_property("done", Value::boolean(true));
    return EvalResult::ok(Value::object(ObjectPtr(result_obj)));
}

EvalResult Interpreter::generator_next(RcPtr<JSGeneratorObject> gen, Value resume_val) {
    if (gen->state_ == GeneratorState::kCompleted) {
        auto result_obj = RcPtr<JSObject>::make();
        gc_heap_.Register(result_obj.get());
        result_obj->set_proto(object_prototype_);
        result_obj->set_property("value", Value::undefined());
        result_obj->set_property("done", Value::boolean(true));
        return EvalResult::ok(Value::object(ObjectPtr(result_obj)));
    }
    if (gen->state_ == GeneratorState::kExecuting) {
        pending_throw_ = make_error_value(NativeErrorType::kTypeError, "Generator is already running");
        return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
    }
    if (gen->state_ == GeneratorState::kSuspendedYield) {
        in_generator_resume_mode_ = true;
        pending_generator_resume_value_ = std::move(resume_val);
    }
    return generator_resume(gen);
}

EvalResult Interpreter::generator_return(RcPtr<JSGeneratorObject> gen, Value return_val) {
    if (gen->state_ == GeneratorState::kCompleted ||
        gen->state_ == GeneratorState::kSuspendedStart) {
        gen->state_ = GeneratorState::kCompleted;
        auto result_obj = RcPtr<JSObject>::make();
        gc_heap_.Register(result_obj.get());
        result_obj->set_proto(object_prototype_);
        result_obj->set_property("value", std::move(return_val));
        result_obj->set_property("done", Value::boolean(true));
        return EvalResult::ok(Value::object(ObjectPtr(result_obj)));
    }
    gen->state_ = GeneratorState::kCompleted;
    auto result_obj = RcPtr<JSObject>::make();
    gc_heap_.Register(result_obj.get());
    result_obj->set_proto(object_prototype_);
    result_obj->set_property("value", std::move(return_val));
    result_obj->set_property("done", Value::boolean(true));
    return EvalResult::ok(Value::object(ObjectPtr(result_obj)));
}

EvalResult Interpreter::generator_throw(RcPtr<JSGeneratorObject> gen, Value throw_val) {
    if (gen->state_ == GeneratorState::kCompleted ||
        gen->state_ == GeneratorState::kSuspendedStart) {
        gen->state_ = GeneratorState::kCompleted;
        pending_throw_ = std::move(throw_val);
        return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
    }
    // kSuspendedYield: resume the generator body and inject the throw at the yield point.
    // eval_yield_expr will detect in_generator_throw_mode_ and propagate kPendingThrowSentinel
    // into the generator body, allowing internal try-catch to handle it.
    in_generator_resume_mode_ = true;
    in_generator_throw_mode_ = true;
    pending_generator_throw_value_ = std::move(throw_val);
    return generator_resume(gen);
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

EvalResult Interpreter::eval_yield_expr(const YieldExpression& expr) {
    if (expr.is_delegate) {
        // yield* iterable: iterate and yield each value, preserving iterator between yields.
        Value iterator = Value::undefined();
        Value next_method = Value::undefined();

        if (in_generator_resume_mode_ && !current_yield_delegate_iter_.is_undefined()) {
            // Resuming an in-progress yield*: consume the resume flag and use the saved iterator.
            in_generator_resume_mode_ = false;
            iterator = std::move(current_yield_delegate_iter_);
            next_method = std::move(current_yield_delegate_next_);
            current_yield_delegate_iter_ = Value::undefined();
            current_yield_delegate_next_ = Value::undefined();
        } else {
            // First entry into yield*: evaluate argument and create iterator.
            in_generator_resume_mode_ = false;  // clear if set (no prior delegate state)
            auto iter_result = eval_expr(*expr.argument);
            if (!iter_result.is_ok()) return iter_result;
            Value iterable = iter_result.value();

            if (!iterable.is_object()) {
                pending_throw_ = make_error_value(NativeErrorType::kTypeError, "not iterable");
                return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
            }
            auto* raw = iterable.as_object_raw();
            Value iter_factory = static_cast<JSObject*>(raw)->get_property_by_symbol(
                symbol_table_.well_known_iterator);
            if (iter_factory.is_undefined()) {
                pending_throw_ = make_error_value(NativeErrorType::kTypeError, "not iterable");
                return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
            }
            auto iter_r = call_function_val(iter_factory, iterable, std::span<Value>());
            if (!iter_r.is_ok()) return EvalResult::err(iter_r.error());
            iterator = iter_r.value();
            if (!iterator.is_object()) {
                pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                    "iterator must be an object");
                return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
            }
            next_method = static_cast<JSObject*>(iterator.as_object_raw())->get_property("next");
        }

        // Call next once, yield the value, save iterator for the next resume.
        auto next_r = call_function_val(next_method, iterator, std::span<Value>());
        if (!next_r.is_ok()) return EvalResult::err(next_r.error());
        Value result_obj = next_r.value();
        if (!result_obj.is_object()) {
            pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "iterator result must be an object");
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        auto* res_raw = static_cast<JSObject*>(result_obj.as_object_raw());
        Value done_val = res_raw->get_property("done");
        Value value_val = res_raw->get_property("value");
        if (to_boolean(done_val)) {
            // Delegate is finished; clear saved state (already cleared above).
            return EvalResult::ok(value_val);
        }
        // Save iterator so that next resume continues from here.
        current_yield_delegate_iter_ = iterator;
        current_yield_delegate_next_ = next_method;
        pending_generator_yield_value_ = value_val;
        return EvalResult::err(Error(ErrorKind::Runtime, kGeneratorYieldSentinel));
    }

    // Simple yield expr
    if (in_generator_resume_mode_) {
        in_generator_resume_mode_ = false;
        // g.throw() path: inject the thrown value so the generator's try-catch can handle it
        if (in_generator_throw_mode_) {
            in_generator_throw_mode_ = false;
            Value throw_v = pending_generator_throw_value_.has_value()
                ? std::move(*pending_generator_throw_value_) : Value::undefined();
            pending_generator_throw_value_ = std::nullopt;
            pending_throw_ = std::move(throw_v);
            return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
        }
        // g.next() path: return the resume value
        Value resume = pending_generator_resume_value_.has_value()
            ? std::move(*pending_generator_resume_value_) : Value::undefined();
        pending_generator_resume_value_ = std::nullopt;
        return EvalResult::ok(std::move(resume));
    }

    // Evaluate the yield argument
    Value yield_val = Value::undefined();
    if (expr.argument) {
        auto arg_r = eval_expr(*expr.argument);
        if (!arg_r.is_ok()) return arg_r;
        yield_val = arg_r.value();
    }

    pending_generator_yield_value_ = std::move(yield_val);
    return EvalResult::err(Error(ErrorKind::Runtime, kGeneratorYieldSentinel));
}

// ============================================================
// class 支持（Interpreter 侧）
// ============================================================

EvalResult Interpreter::eval_class_common(
    const std::optional<std::unique_ptr<ExprNode>>& super_class_opt,
    const std::vector<ClassMethod>& methods,
    const std::vector<ClassField>& fields,
    const std::optional<std::string>& class_name) {

    bool has_super = super_class_opt.has_value();
    bool extends_null = false;  // M3: class C extends null {}
    JSFunction* super_fn = nullptr;
    RcPtr<JSObject> super_proto;

    // 0. 为私有字段分配 symbol（编译/执行期唯一性由 SymbolTable 保证）
    std::unordered_map<std::string, uint64_t> private_fields_map;
    for (const auto& f : fields) {
        if (f.is_private) {
            private_fields_map[f.key] = symbol_table_.NewSymbol(f.key);
        }
    }
    for (const auto& m : methods) {
        if (m.is_private) {
            private_fields_map[m.key] = symbol_table_.NewSymbol(m.key);
        }
    }

    // Helper: inject private_fields_map into a function
    auto inject_private_fields = [&](Value& fn_val) {
        if (!fn_val.is_object()) return;
        RcObject* raw = fn_val.as_object_raw();
        if (raw->object_kind() != ObjectKind::kFunction) return;
        auto* fn = static_cast<JSFunction*>(raw);
        for (const auto& [name, sym_id] : private_fields_map) {
            fn->set_private_field(name, sym_id);
        }
    };

    // 1. 求 super class
    if (has_super) {
        auto super_r = eval_expr(*super_class_opt->get());
        if (!super_r.is_ok()) return super_r;
        Value super_val = super_r.value();
        if (super_val.is_null()) {
            // M3: extends null — proto gets null prototype, no super ctor
            extends_null = true;
        } else if (!super_val.is_undefined()) {
            if (!super_val.is_object() ||
                super_val.as_object_raw()->object_kind() != ObjectKind::kFunction) {
                pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                    "Class extends value is not a constructor or null");
                return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
            }
            super_fn = static_cast<JSFunction*>(super_val.as_object_raw());
            super_proto = super_fn->prototype_obj();
        }
    }

    // 2. 找 constructor 方法
    const ClassMethod* ctor_method = nullptr;
    for (const auto& m : methods) {
        if (!m.computed && m.key == "constructor" && !m.is_static &&
            m.method_kind == MethodKind::kData) {
            ctor_method = &m;
            break;
        }
    }

    // 3. 创建 prototype 对象
    auto proto_obj = RcPtr<JSObject>::make();
    gc_heap_.Register(proto_obj.get());
    if (extends_null) {
        // M3: extends null → C.prototype.[[Prototype]] = null (empty RcPtr)
        proto_obj->set_proto(RcPtr<JSObject>{});
    } else if (super_proto) {
        proto_obj->set_proto(super_proto);
    } else {
        proto_obj->set_proto(object_prototype_);
    }

    // 4. 创建 constructor 函数
    Value ctor_fn_val;
    if (ctor_method != nullptr) {
        const auto& fe = std::get<FunctionExpression>(ctor_method->fn_expr->v);
        ctor_fn_val = make_function_value(class_name, fe.params, fe.body, current_env_,
                                           false, fe.rest_param);
    } else if (has_super && !extends_null) {
        // Implicit derived constructor: constructor(...args) { super(...args); }
        auto empty_body = std::make_shared<std::vector<StmtNode>>();
        ctor_fn_val = make_function_value(class_name, {}, empty_body, current_env_,
                                           false, std::optional<std::string>{"$__class_impl_args__"});
    } else {
        // Implicit base constructor (also used for extends null): constructor() {}
        auto empty_body = std::make_shared<std::vector<StmtNode>>();
        ctor_fn_val = make_function_value(class_name, {}, empty_body, current_env_);
    }

    auto* ctor_fn = static_cast<JSFunction*>(ctor_fn_val.as_object_raw());
    ctor_fn->set_is_class_ctor(true);
    // M3: extends null — treat as base class (no super ctor to call)
    ctor_fn->set_is_derived_ctor(has_super && !extends_null);
    if (super_fn) {
        ctor_fn->set_fn_ctor_proto(super_fn);
    }
    // Inject private fields mapping into ctor
    inject_private_fields(ctor_fn_val);

    // 5. 设置 prototype 和 constructor 属性
    ctor_fn->set_prototype_obj(proto_obj);
    proto_obj->set_constructor_property(ctor_fn);

    // 6. 挂载非 static 方法到 prototype
    for (const auto& m : methods) {
        if (m.is_static || m.method_kind == MethodKind::kData) continue;

        // 求方法 key
        std::string key;
        if (m.computed) {
            auto key_r = eval_expr(*m.key_expr);
            if (!key_r.is_ok()) return key_r;
            key = to_string_val(key_r.value());
        } else {
            key = m.key;
        }

        // 创建方法函数值
        Value method_val;
        if (std::holds_alternative<FunctionExpression>(m.fn_expr->v)) {
            const auto& fe = std::get<FunctionExpression>(m.fn_expr->v);
            method_val = make_function_value(
                m.computed ? std::nullopt : std::optional<std::string>{m.key},
                fe.params, fe.body, current_env_, false, fe.rest_param);
            // M6: class generator method — propagate is_generator flag
            if (fe.is_generator && method_val.is_object() &&
                method_val.as_object_raw()->object_kind() == ObjectKind::kFunction) {
                static_cast<JSFunction*>(method_val.as_object_raw())->set_is_generator(true);
            }
        } else if (std::holds_alternative<AsyncFunctionExpression>(m.fn_expr->v)) {
            const auto& afe = std::get<AsyncFunctionExpression>(m.fn_expr->v);
            method_val = make_async_function_value(
                m.computed ? std::nullopt : std::optional<std::string>{m.key},
                afe.params, afe.body, current_env_, afe.rest_param);
        } else {
            method_val = Value::undefined();
        }

        // Inject private fields mapping into method
        inject_private_fields(method_val);

        // 设置 home_object
        if (method_val.is_object() && method_val.as_object_raw()->object_kind() == ObjectKind::kFunction) {
            auto* mfn = static_cast<JSFunction*>(method_val.as_object_raw());
            mfn->set_home_object(proto_obj.get());
        }

        // 定义在 prototype 上（enumerable=false, writable=true, configurable=true）
        if (m.method_kind == MethodKind::kGetter) {
            PropDesc desc;
            desc.getter = method_val;
            desc.enumerable = false;
            desc.configurable = true;
            proto_obj->define_property(key, desc);
        } else if (m.method_kind == MethodKind::kSetter) {
            PropDesc desc;
            desc.setter = method_val;
            desc.enumerable = false;
            desc.configurable = true;
            proto_obj->define_property(key, desc);
        } else {
            PropDesc desc;
            desc.value = method_val;
            desc.writable = true;
            desc.enumerable = false;
            desc.configurable = true;
            proto_obj->define_property(key, desc);
        }
    }

    // 7. 挂载 static 方法到 ctor
    for (const auto& m : methods) {
        if (!m.is_static) continue;

        std::string key;
        if (m.computed) {
            auto key_r = eval_expr(*m.key_expr);
            if (!key_r.is_ok()) return key_r;
            key = to_string_val(key_r.value());
        } else {
            key = m.key;
        }

        Value method_val;
        if (std::holds_alternative<FunctionExpression>(m.fn_expr->v)) {
            const auto& fe = std::get<FunctionExpression>(m.fn_expr->v);
            method_val = make_function_value(
                m.computed ? std::nullopt : std::optional<std::string>{m.key},
                fe.params, fe.body, current_env_, false, fe.rest_param);
            // M6: static generator method — propagate is_generator flag
            if (fe.is_generator && method_val.is_object() &&
                method_val.as_object_raw()->object_kind() == ObjectKind::kFunction) {
                static_cast<JSFunction*>(method_val.as_object_raw())->set_is_generator(true);
            }
        } else if (std::holds_alternative<AsyncFunctionExpression>(m.fn_expr->v)) {
            const auto& afe = std::get<AsyncFunctionExpression>(m.fn_expr->v);
            method_val = make_async_function_value(
                m.computed ? std::nullopt : std::optional<std::string>{m.key},
                afe.params, afe.body, current_env_, afe.rest_param);
        } else {
            method_val = Value::undefined();
        }

        // Inject private fields mapping into static method
        inject_private_fields(method_val);

        ctor_fn->set_property(key, method_val);
    }

    // 8. 处理 fields
    // static fields：立即初始化到 ctor 本身
    // instance fields：存入 ctor_fn 供构造时使用（ClassField 使用 shared_ptr，可安全复制）
    auto instance_fields_vec = std::make_shared<std::vector<ClassField>>();
    for (const auto& f : fields) {
        if (f.is_static) {
            std::string key;
            if (f.computed) {
                auto key_r = eval_expr(*f.key_expr);
                if (!key_r.is_ok()) return key_r;
                key = to_string_val(key_r.value());
            } else {
                key = f.key;
            }
            Value init_val = Value::undefined();
            if (f.initializer != nullptr) {
                auto init_r = eval_expr(*f.initializer);
                if (!init_r.is_ok()) return init_r;
                init_val = init_r.value();
            }
            ctor_fn->set_property(key, init_val);
        } else {
            instance_fields_vec->push_back(f);
        }
    }
    if (!instance_fields_vec->empty()) {
        ctor_fn->set_instance_fields(instance_fields_vec);
    }

    return EvalResult::ok(ctor_fn_val);
}

EvalResult Interpreter::eval_class_expr(const ClassExpression& expr) {
    // 命名 class 表达式：在内部作用域绑定名字
    if (expr.name.has_value()) {
        auto class_env = RcPtr<Environment>::make(current_env_);
        gc_heap_.Register(class_env.get());
        class_env->define(*expr.name, VarKind::Const);
        ScopeGuard guard(*this, class_env, var_env_, current_this_);
        auto result = eval_class_common(expr.super_class, expr.methods, expr.fields, expr.name);
        if (!result.is_ok()) return result;
        // Bind name to the class
        auto init_r = class_env->initialize(*expr.name, result.value());
        (void)init_r;
        return result;
    }
    return eval_class_common(expr.super_class, expr.methods, expr.fields, expr.name);
}

EvalResult Interpreter::eval_class_decl(const ClassDeclaration& stmt) {
    std::optional<std::string> class_name{stmt.name};
    auto result = eval_class_common(stmt.super_class, stmt.methods, stmt.fields, class_name);
    if (!result.is_ok()) return result;
    // Bind to current scope (like let declaration at declaration point)
    current_env_->define(stmt.name, VarKind::Let);
    auto init_r = current_env_->initialize(stmt.name, result.value());
    if (!init_r.is_ok()) {
        // Already defined (e.g., re-declaration): try set
        current_env_->set(stmt.name, result.value());
    }
    return EvalResult::ok(Value::undefined());
}

EvalResult Interpreter::init_instance_fields(JSFunction* ctor_fn, Value& this_val) {
    const auto& fields = ctor_fn->instance_fields();
    if (!fields) return EvalResult::ok(Value::undefined());
    for (const auto& f : *fields) {
        Value init_val = Value::undefined();
        if (f.initializer != nullptr) {
            auto init_r = eval_expr(*f.initializer);
            if (!init_r.is_ok()) return init_r;
            init_val = init_r.value();
        }
        if (this_val.is_object() && this_val.as_object_raw() != nullptr) {
            if (f.is_private) {
                // Private field: use symbol key from ctor's private_fields_ mapping
                uint64_t sym_id = ctor_fn->get_private_field_sym(f.key);
                if (sym_id != 0) {
                    JSObject* obj = nullptr;
                    if (this_val.as_object_raw()->object_kind() == ObjectKind::kFunction) {
                        obj = static_cast<JSObject*>(this_val.as_object_raw());
                    } else {
                        obj = static_cast<JSObject*>(this_val.as_object_raw());
                    }
                    obj->set_property_by_symbol(sym_id, init_val);
                }
            } else {
                std::string key;
                if (f.computed) {
                    auto key_r = eval_expr(*f.key_expr);
                    if (!key_r.is_ok()) return key_r;
                    key = to_string_val(key_r.value());
                } else {
                    key = f.key;
                }
                if (this_val.as_object_raw()->object_kind() == ObjectKind::kFunction) {
                    static_cast<JSFunction*>(this_val.as_object_raw())->set_property(key, init_val);
                } else {
                    auto* obj = static_cast<JSObject*>(this_val.as_object_raw());
                    obj->set_property(key, init_val);
                }
            }
        }
    }
    return EvalResult::ok(Value::undefined());
}

EvalResult Interpreter::eval_super_call(const SuperCallExpression& expr) {
    // super(...args) — call parent class constructor
    JSFunction* active_fn = current_function_;
    if (!active_fn) {
        pending_throw_ = make_error_value(NativeErrorType::kTypeError,
            "super() called outside a constructor");
        return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
    }
    JSFunction* super_ctor = active_fn->fn_ctor_proto();
    if (!super_ctor) {
        pending_throw_ = make_error_value(NativeErrorType::kTypeError,
            "super() called in a non-derived constructor");
        return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
    }

    // Evaluate arguments
    std::vector<Value> args;
    for (const auto& arg : expr.arguments) {
        if (std::holds_alternative<SpreadElement>(arg->v)) {
            const auto& sp = std::get<SpreadElement>(arg->v);
            auto spread_r = eval_expr(*sp.argument);
            if (!spread_r.is_ok()) return spread_r;
            if (!spread_into(spread_r.value(), args)) {
                return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
            }
        } else {
            auto arg_r = eval_expr(*arg);
            if (!arg_r.is_ok()) return arg_r;
            args.push_back(arg_r.value());
        }
    }

    // Create new_obj using new.target prototype
    Value new_target = current_new_target_;
    RcPtr<JSObject> new_proto;
    if (new_target.is_object() &&
        new_target.as_object_raw()->object_kind() == ObjectKind::kFunction) {
        new_proto = static_cast<JSFunction*>(new_target.as_object_raw())->prototype_obj();
    }
    auto new_obj = RcPtr<JSObject>::make();
    gc_heap_.Register(new_obj.get());
    if (new_proto) {
        new_obj->set_proto(new_proto);
    } else {
        new_obj->set_proto(object_prototype_);
    }
    Value new_obj_val = Value::object(ObjectPtr(new_obj));

    // Call super constructor with new_obj as this (is_new_call=true to pass class ctor guard)
    RcPtr<JSFunction> super_fn_rc(super_ctor);

    // Save new_target and pass it down
    Value saved_new_target = current_new_target_;
    bool saved_derived_init = derived_this_initialized_;

    auto call_r = call_function(super_fn_rc, new_obj_val, std::move(args), /*is_new_call=*/true);

    current_new_target_ = saved_new_target;
    derived_this_initialized_ = saved_derived_init;

    if (!call_r.is_ok()) return EvalResult::err(call_r.error());

    // If super returned an object, use it
    Value super_ret = call_r.completion().value;
    Value new_this = (super_ret.is_object() && !super_ret.is_null())
        ? std::move(super_ret)
        : std::move(new_obj_val);

    // Update current_this_ (visible inside the derived ctor body)
    current_this_ = new_this;
    last_new_this_ = new_this;  // also save for eval_new_expr to read after call_function returns
    derived_this_initialized_ = true;

    // Derived class constructor: initialize instance fields after super() returns
    if (active_fn->instance_fields() && !active_fn->instance_fields()->empty()) {
        auto field_r = init_instance_fields(active_fn, current_this_);
        if (!field_r.is_ok()) return field_r;
    }

    return EvalResult::ok(Value::undefined());
}

EvalResult Interpreter::eval_super_member(const SuperMemberExpression& expr) {
    // super.prop or super[key]
    JSFunction* cur_fn = current_function_;
    if (!cur_fn || !cur_fn->home_object()) {
        pending_throw_ = make_error_value(NativeErrorType::kTypeError,
            "super property access outside method");
        return EvalResult::err(Error(ErrorKind::Runtime, kPendingThrowSentinel));
    }

    JSObject* home = cur_fn->home_object();
    // Get home.__proto__
    JSObject* home_proto = nullptr;
    if (home->proto()) {
        RcObject* proto_raw = home->proto().get();
        if (proto_raw->object_kind() == ObjectKind::kOrdinary ||
            proto_raw->object_kind() == ObjectKind::kArray) {
            home_proto = static_cast<JSObject*>(proto_raw);
        }
    }

    std::string key;
    if (expr.computed) {
        auto key_r = eval_expr(*expr.key_expr);
        if (!key_r.is_ok()) return key_r;
        key = to_string_val(key_r.value());
    } else {
        key = expr.property;
    }

    Value result_val = Value::undefined();
    if (home_proto) {
        // M5: walk prototype chain and call accessor getter with current this as receiver
        JSObject* search = home_proto;
        while (search) {
            const auto* entry = search->get_own_entry(key);
            if (entry) {
                if ((entry->flags & kPropIsAccessor) && !entry->getter.is_undefined()) {
                    auto getter_r = call_function_val(entry->getter, current_this_, {});
                    if (!getter_r.is_ok()) return getter_r;
                    return EvalResult::ok(getter_r.value());
                }
                result_val = entry->value;
                break;
            }
            RcObject* proto_raw = search->proto().get();
            if (!proto_raw) break;
            if (proto_raw->object_kind() == ObjectKind::kOrdinary ||
                proto_raw->object_kind() == ObjectKind::kArray) {
                search = static_cast<JSObject*>(proto_raw);
            } else {
                break;
            }
        }
    }
    return EvalResult::ok(result_val);
}

}  // namespace qppjs
