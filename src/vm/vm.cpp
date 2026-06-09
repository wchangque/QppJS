#include "qppjs/vm/vm.h"

#include "qppjs/base/error.h"
#include "qppjs/frontend/ast.h"
#include "qppjs/frontend/parser.h"
#include "qppjs/runtime/completion.h"
#include "qppjs/runtime/for_of_iterator.h"
#include "qppjs/runtime/environment.h"
#include "qppjs/runtime/js_function.h"
#include "qppjs/runtime/for_in_iterator.h"
#include "qppjs/runtime/js_generator.h"
#include "qppjs/runtime/js_map.h"
#include "qppjs/runtime/js_object.h"
#include "qppjs/runtime/js_regexp.h"
#include "qppjs/runtime/module_loader.h"
#include "qppjs/runtime/module_record.h"
#include "qppjs/runtime/native_errors.h"
#include "qppjs/runtime/number_utils.h"
#include "qppjs/runtime/value.h"
#include "qppjs/vm/bytecode.h"
#include "qppjs/vm/compiler.h"
#include "qppjs/vm/opcode.h"

#include <cassert>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <regex>
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
    case ValueKind::String:  return !v.sv().empty();
    case ValueKind::Object:  return true;
    case ValueKind::Symbol:  return true;
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
    case ValueKind::Object: {
        // ToPrimitive(obj, "number"): 处理 wrapper 对象快路径
        RcObject* raw_obj = v.as_object_raw();
        if (raw_obj != nullptr) {
            // kStringObject: valueOf 返回包装的字符串
            if (raw_obj->object_kind() == ObjectKind::kStringObject) {
                auto* js_obj = static_cast<JSObject*>(raw_obj);
                Value wrapped = js_obj->wrapped_value();
                if (wrapped.is_string()) return to_number(wrapped);
            }
            // kBooleanObject: valueOf 返回包装的布尔值
            if (raw_obj->object_kind() == ObjectKind::kBooleanObject) {
                auto* js_obj = static_cast<JSObject*>(raw_obj);
                Value wrapped = js_obj->wrapped_value();
                if (wrapped.is_bool()) {
                    return EvalResult::ok(Value::number(wrapped.as_bool() ? 1.0 : 0.0));
                }
            }
        }
        return EvalResult::ok(Value::number(std::numeric_limits<double>::quiet_NaN()));
    }
    case ValueKind::Symbol:
        return EvalResult::err(Error(ErrorKind::Runtime, "TypeError: Cannot convert a Symbol value to a number"));
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
    // 整数且在 2^53 范围内：精确转换
    if (d == std::floor(d) && d >= -9007199254740992.0 && d <= 9007199254740992.0) {
        return std::to_string(static_cast<int64_t>(d));
    }
    // 大整数且 < 10^21：使用 fixed 格式（如 1e20 → "100000000000000000000"）
    if (d == std::floor(d) && std::abs(d) < 1e21) {
        // sprintf 方式，用足够大的缓冲区输出 fixed 整数格式
        char buf[64];
        int len = std::snprintf(buf, sizeof(buf), "%.0f", d);
        if (len > 0 && len < static_cast<int>(sizeof(buf))) {
            return std::string(buf, len);
        }
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
    case ValueKind::Symbol:
        // Implicit ToString of Symbol is forbidden; callers that need explicit conversion
        // (e.g. String(sym)) must handle separately.
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
    case ValueKind::Symbol: return a.as_symbol_id() == b.as_symbol_id();
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
    case ValueKind::Object:  return std::numeric_limits<double>::quiet_NaN();
    case ValueKind::Symbol:  return std::numeric_limits<double>::quiet_NaN();
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

// Coerce a primitive value to string for String.prototype method fallback.
static std::string vm_coerce_primitive_to_str(const Value& v) {
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
        std::ostringstream oss;
        oss << n;
        return oss.str();
    }
    return "[object Object]";
}

// Extract the effective string Value from a String.prototype method's this.
static Value string_this_value_vm(const Value& this_val) {
    if (this_val.is_string()) return this_val;
    if (this_val.is_object()) {
        RcObject* raw = this_val.as_object_raw();
        if (raw->object_kind() == ObjectKind::kStringObject) {
            return static_cast<JSObject*>(raw)->wrapped_value();
        }
        return Value::string("[object Object]");
    }
    return Value::string(vm_coerce_primitive_to_str(this_val));
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
        {NativeErrorType::kSyntaxError,    "SyntaxError"},
        {NativeErrorType::kEvalError,      "EvalError"},
        {NativeErrorType::kURIError,       "URIError"},
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
    array_prototype_ = RcPtr<JSObject>::make(ObjectKind::kArray);
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

    auto array_iterator_fn = RcPtr<JSFunction>::make();
    array_iterator_fn->set_name(std::string("[Symbol.iterator]"));
    array_iterator_fn->set_native_fn([this](Value this_val, std::vector<Value>, bool) -> EvalResult {
        auto iter_obj = RcPtr<JSObject>::make();
        gc_heap_.Register(iter_obj.get());
        iter_obj->set_property("__arr__", this_val);
        iter_obj->set_property("__idx__", Value::number(0.0));

        auto next_fn = RcPtr<JSFunction>::make();
        next_fn->set_name(std::string("next"));
        next_fn->set_native_fn([this](Value this_val, std::vector<Value>, bool) -> EvalResult {
            auto make_done = [&]() -> EvalResult {
                auto res = RcPtr<JSObject>::make();
                gc_heap_.Register(res.get());
                res->set_property("value", Value::undefined());
                res->set_property("done", Value::boolean(true));
                return EvalResult::ok(Value::object(ObjectPtr(res)));
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
            auto res = RcPtr<JSObject>::make();
            gc_heap_.Register(res.get());
            res->set_property("value", std::move(value));
            res->set_property("done", Value::boolean(false));
            return EvalResult::ok(Value::object(ObjectPtr(res)));
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
    gc_heap_.Register(array_iterator_fn.get());
    array_prototype_->set_property_by_symbol(symbol_table_.well_known_iterator,
                                             Value::object(ObjectPtr(array_iterator_fn)));

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
            // Skip holes (sparse array semantics)
            if (elem_it == arr->elements_.end()) continue;
            Value elem = elem_it->second;
            Value call_args[3] = {elem, Value::number(static_cast<double>(i)), this_val};
            std::span<Value> arg_span(call_args, 3);
            auto res = call_function_val(callback, Value::undefined(), arg_span);
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
    array_prototype_->define_builtin_property("map", Value::object(ObjectPtr(map_fn)));

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
    array_prototype_->define_builtin_property("filter", Value::object(ObjectPtr(filter_fn)));

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
    array_prototype_->define_builtin_property("reduce", Value::object(ObjectPtr(reduce_fn)));

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
    array_prototype_->define_builtin_property("reduceRight", Value::object(ObjectPtr(reduce_right_fn)));

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
    array_prototype_->define_builtin_property("find", Value::object(ObjectPtr(find_fn)));

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
    array_prototype_->define_builtin_property("findIndex", Value::object(ObjectPtr(find_index_fn)));

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
    array_prototype_->define_builtin_property("some", Value::object(ObjectPtr(some_fn)));

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
    array_prototype_->define_builtin_property("every", Value::object(ObjectPtr(every_fn)));

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
    array_prototype_->define_builtin_property("indexOf", Value::object(ObjectPtr(index_of_fn)));

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
    array_prototype_->define_builtin_property("includes", Value::object(ObjectPtr(includes_fn)));

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
    array_prototype_->define_builtin_property("slice", Value::object(ObjectPtr(slice_fn)));

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
    array_prototype_->define_builtin_property("splice", Value::object(ObjectPtr(splice_fn)));

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
    array_prototype_->define_builtin_property("sort", Value::object(ObjectPtr(sort_fn)));

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
    array_prototype_->define_builtin_property("join", Value::object(ObjectPtr(join_fn)));

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
    array_prototype_->define_builtin_property("reverse", Value::object(ObjectPtr(reverse_fn)));

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
    array_prototype_->define_builtin_property("flat", Value::object(ObjectPtr(flat_fn)));

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
    array_prototype_->define_builtin_property("flatMap", Value::object(ObjectPtr(flat_map_fn)));

    // Array.prototype.findLast
    auto find_last_fn = RcPtr<JSFunction>::make();
    find_last_fn->set_name(std::string("findLast"));
    find_last_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        RcObject* raw = this_val.as_object_raw();
        if (!raw || raw->object_kind() != ObjectKind::kArray) {
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                                     "findLast called on non-array");
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
        for (int64_t i = static_cast<int64_t>(len) - 1; i >= 0; i--) {
            auto it = arr->elements_.find(static_cast<uint32_t>(i));
            Value elem = (it != arr->elements_.end()) ? it->second : Value::undefined();
            Value call_args[3] = {elem, Value::number(static_cast<double>(i)), this_val};
            std::span<Value> arg_span(call_args, 3);
            auto res = call_function_val(callback, this_arg, arg_span);
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
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                                     "findLastIndex called on non-array");
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
        for (int64_t i = static_cast<int64_t>(len) - 1; i >= 0; i--) {
            auto it = arr->elements_.find(static_cast<uint32_t>(i));
            Value elem = (it != arr->elements_.end()) ? it->second : Value::undefined();
            Value call_args[3] = {elem, Value::number(static_cast<double>(i)), this_val};
            std::span<Value> arg_span(call_args, 3);
            auto res = call_function_val(callback, this_arg, arg_span);
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
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                                     "toSorted called on non-array");
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
            for (auto& s : slots) s.str_cache = VM::to_string_val(s.val);
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
                if (!res.is_ok()) { sort_err = res; had_error = true; return false; }
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
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                                     "toReversed called on non-array");
            return EvalResult::err(Error{ErrorKind::Runtime, "__qppjs_pending_throw__"});
        }
        auto* src = static_cast<JSObject*>(raw);
        uint32_t len = src->array_length_;
        auto result = RcPtr<JSObject>::make(ObjectKind::kArray);
        gc_heap_.Register(result.get());
        result->set_proto(array_prototype_);
        for (uint32_t i = 0; i < len; i++) {
            uint32_t from = len - 1 - i;
            auto it = src->elements_.find(from);
            if (it != src->elements_.end()) result->elements_[i] = it->second;
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
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                                     "toSpliced called on non-array");
            return EvalResult::err(Error{ErrorKind::Runtime, "__qppjs_pending_throw__"});
        }
        auto* src = static_cast<JSObject*>(raw);
        int64_t len = static_cast<int64_t>(src->array_length_);
        int64_t start = 0;
        if (!args.empty() && !args[0].is_undefined()) {
            double s = to_number_double_vm(args[0]);
            start = std::isnan(s) ? 0 : static_cast<int64_t>(std::trunc(s));
        }
        if (start < 0) start = std::max(int64_t(0), len + start);
        else start = std::min(start, len);
        int64_t del_count = len - start;
        if (args.size() >= 2 && !args[1].is_undefined()) {
            double d = to_number_double_vm(args[1]);
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
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                                     "with called on non-array");
            return EvalResult::err(Error{ErrorKind::Runtime, "__qppjs_pending_throw__"});
        }
        auto* src = static_cast<JSObject*>(raw);
        uint32_t len = src->array_length_;
        if (args.size() < 2) {
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                                     "Array.prototype.with requires 2 arguments");
            return EvalResult::err(Error{ErrorKind::Runtime, "__qppjs_pending_throw__"});
        }
        double idx_d = to_number_double_vm(args[0]);
        int64_t idx = std::isnan(idx_d) ? 0 : static_cast<int64_t>(std::trunc(idx_d));
        if (idx < 0) idx = static_cast<int64_t>(len) + idx;
        if (idx < 0 || idx >= static_cast<int64_t>(len)) {
            native_pending_throw_ = make_error_value(NativeErrorType::kRangeError, "Invalid index");
            return EvalResult::err(Error{ErrorKind::Runtime, "__qppjs_pending_throw__"});
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
        if (!this_raw) return EvalResult::err(Error{ErrorKind::Runtime,
            "TypeError: Array.prototype.concat called on null or undefined"});
        auto result = RcPtr<JSObject>::make(ObjectKind::kArray);
        gc_heap_.Register(result.get());
        result->set_proto(array_prototype_);
        uint32_t n = 0;
        if (this_raw->object_kind() == ObjectKind::kArray) {
            auto* arr = static_cast<JSObject*>(this_raw);
            for (uint32_t i = 0; i < arr->array_length_; ++i) {
                auto it = arr->elements_.find(i);
                if (it != arr->elements_.end()) result->elements_[n] = it->second;
                n++;
            }
        } else {
            result->elements_[n++] = this_val;
        }
        for (auto& arg : args) {
            if (arg.is_object() && arg.as_object_raw() &&
                arg.as_object_raw()->object_kind() == ObjectKind::kArray) {
                auto* arr = static_cast<JSObject*>(arg.as_object_raw());
                for (uint32_t i = 0; i < arr->array_length_; ++i) {
                    auto it = arr->elements_.find(i);
                    if (it != arr->elements_.end()) result->elements_[n] = it->second;
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
    fill_fn->set_native_fn([](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        auto* raw = this_val.as_object_raw();
        if (!raw) return EvalResult::err(Error{ErrorKind::Runtime,
            "TypeError: Array.prototype.fill called on null or undefined"});
        double len_d = 0;
        if (raw->object_kind() == ObjectKind::kArray)
            len_d = static_cast<double>(static_cast<JSObject*>(raw)->array_length_);
        auto len = static_cast<int64_t>(len_d);
        auto rel_start = args.size() > 1 ? to_number_double_vm(args[1]) : 0;
        auto k = rel_start < 0 ? std::max(len + static_cast<int64_t>(rel_start), INT64_C(0))
                               : std::min(static_cast<int64_t>(rel_start), len);
        auto rel_end = args.size() > 2 ? to_number_double_vm(args[2]) : len_d;
        auto final_end = rel_end < 0 ? std::max(len + static_cast<int64_t>(rel_end), INT64_C(0))
                                     : std::min(static_cast<int64_t>(rel_end), len);
        Value fill_val = args.empty() ? Value::undefined() : args[0];
        if (raw->object_kind() == ObjectKind::kArray) {
            auto* arr = static_cast<JSObject*>(raw);
            for (auto i = k; i < final_end; ++i)
                arr->elements_[static_cast<uint32_t>(i)] = fill_val;
        }
        return EvalResult::ok(this_val);
    });
    array_prototype_->define_builtin_property("fill", Value::object(ObjectPtr(fill_fn)));

    // Array.prototype.copyWithin
    auto copywithin_fn = RcPtr<JSFunction>::make();
    copywithin_fn->set_name(std::string("copyWithin"));
    copywithin_fn->set_native_fn([](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        auto* raw = this_val.as_object_raw();
        if (!raw) return EvalResult::err(Error{ErrorKind::Runtime,
            "TypeError: Array.prototype.copyWithin called on null or undefined"});
        if (raw->object_kind() != ObjectKind::kArray) return EvalResult::ok(this_val);
        auto* arr = static_cast<JSObject*>(raw);
        int64_t len = arr->array_length_;
        int64_t target = args.size() > 0 ? static_cast<int64_t>(to_number_double_vm(args[0])) : 0;
        if (target < 0) target = std::max(len + target, INT64_C(0));
        else target = std::min(target, len);
        int64_t start = args.size() > 1 ? static_cast<int64_t>(to_number_double_vm(args[1])) : 0;
        if (start < 0) start = std::max(len + start, INT64_C(0));
        else start = std::min(start, len);
        int64_t end = len;
        if (args.size() > 2 && !args[2].is_undefined()) {
            end = static_cast<int64_t>(to_number_double_vm(args[2]));
            if (end < 0) end = std::max(len + end, INT64_C(0));
            else end = std::min(end, len);
        }
        int64_t count = std::min(end - start, len - target);
        if (count > 0) {
            if (target < start || target >= start + count)
                for (int64_t i = 0; i < count; ++i) {
                    auto it = arr->elements_.find(static_cast<uint32_t>(start + i));
                    if (it != arr->elements_.end()) arr->elements_[static_cast<uint32_t>(target + i)] = it->second;
                    else arr->elements_.erase(static_cast<uint32_t>(target + i));
                }
            else
                for (int64_t i = count - 1; i >= 0; --i) {
                    auto it = arr->elements_.find(static_cast<uint32_t>(start + i));
                    if (it != arr->elements_.end()) arr->elements_[static_cast<uint32_t>(target + i)] = it->second;
                    else arr->elements_.erase(static_cast<uint32_t>(target + i));
                }
        }
        return EvalResult::ok(this_val);
    });
    array_prototype_->define_builtin_property("copyWithin", Value::object(ObjectPtr(copywithin_fn)));

    // Array.prototype.shift
    auto shift_fn = RcPtr<JSFunction>::make();
    shift_fn->set_name(std::string("shift"));
    shift_fn->set_native_fn([](Value this_val, std::vector<Value>, bool) -> EvalResult {
        auto* raw = this_val.as_object_raw();
        if (!raw || raw->object_kind() != ObjectKind::kArray)
            return EvalResult::err(Error{ErrorKind::Runtime,
                "TypeError: Array.prototype.shift called on null or undefined"});
        auto* arr = static_cast<JSObject*>(raw);
        if (arr->array_length_ == 0) return EvalResult::ok(Value::undefined());
        auto it = arr->elements_.find(0);
        Value first = (it != arr->elements_.end()) ? std::move(it->second) : Value::undefined();
        arr->elements_.erase(0);
        std::vector<std::pair<uint32_t, Value>> shifted;
        for (auto& [idx, val] : arr->elements_)
            shifted.emplace_back(idx - 1, std::move(val));
        arr->elements_.clear();
        for (auto& [idx, val] : shifted)
            arr->elements_[idx] = std::move(val);
        arr->array_length_--;
        return EvalResult::ok(first);
    });
    array_prototype_->define_builtin_property("shift", Value::object(ObjectPtr(shift_fn)));

    // Array.prototype.unshift
    auto unshift_fn = RcPtr<JSFunction>::make();
    unshift_fn->set_name(std::string("unshift"));
    unshift_fn->set_native_fn([](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        auto* raw = this_val.as_object_raw();
        if (!raw || raw->object_kind() != ObjectKind::kArray)
            return EvalResult::err(Error{ErrorKind::Runtime,
                "TypeError: Array.prototype.unshift called on null or undefined"});
        auto* arr = static_cast<JSObject*>(raw);
        uint32_t arg_count = static_cast<uint32_t>(args.size());
        if (arg_count > 0) {
            std::vector<std::pair<uint32_t, Value>> shifted;
            for (auto& [idx, val] : arr->elements_)
                shifted.emplace_back(idx + arg_count, std::move(val));
            arr->elements_.clear();
            for (auto& [idx, val] : shifted)
                arr->elements_[idx] = std::move(val);
            for (uint32_t i = 0; i < arg_count; ++i)
                arr->elements_[i] = std::move(args[i]);
        }
        arr->array_length_ += arg_count;
        return EvalResult::ok(Value::number(static_cast<double>(arr->array_length_)));
    });
    array_prototype_->define_builtin_property("unshift", Value::object(ObjectPtr(unshift_fn)));

    // Array.prototype.lastIndexOf
    auto lastindexof_fn = RcPtr<JSFunction>::make();
    lastindexof_fn->set_name(std::string("lastIndexOf"));
    lastindexof_fn->set_native_fn([](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        auto* raw = this_val.as_object_raw();
        if (!raw || raw->object_kind() != ObjectKind::kArray)
            return EvalResult::err(Error{ErrorKind::Runtime,
                "TypeError: Array.prototype.lastIndexOf called on null or undefined"});
        auto* arr = static_cast<JSObject*>(raw);
        if (args.empty()) return EvalResult::ok(Value::number(-1));
        Value search = args[0];
        int64_t len = arr->array_length_;
        if (len == 0) return EvalResult::ok(Value::number(-1));
        int64_t from = len - 1;
        if (args.size() > 1) {
            double d = to_number_double_vm(args[1]);
            from = std::isnan(d) ? 0 : static_cast<int64_t>(std::trunc(d));
            if (from < 0) from = std::max(len + from, INT64_C(0));
            else from = std::min(from, len - 1);
        }
        for (int64_t k = from; k >= 0; --k) {
            auto it = arr->elements_.find(static_cast<uint32_t>(k));
            if (it != arr->elements_.end() && strict_eq(it->second, search))
                return EvalResult::ok(Value::number(static_cast<double>(k)));
        }
        return EvalResult::ok(Value::number(-1));
    });
    array_prototype_->define_builtin_property("lastIndexOf", Value::object(ObjectPtr(lastindexof_fn)));

    // Array.prototype.at(index)
    {
        auto vm_at_fn = RcPtr<JSFunction>::make();
        vm_at_fn->set_name(std::string("at"));
        vm_at_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
            if (!this_val.is_object() || this_val.as_object_raw()->object_kind() != ObjectKind::kArray) {
                native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                                         "Array.prototype.at called on non-array");
                return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
            }
            auto* arr = static_cast<JSObject*>(this_val.as_object_raw());
            uint32_t len = arr->array_length_;
            double idx_d = args.empty() ? 0.0 : to_number_double_vm(args[0]);
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
        gc_heap_.Register(vm_at_fn.get());
        array_prototype_->define_builtin_property("at", Value::object(ObjectPtr(vm_at_fn)));
    }

    // Array.prototype.toString: equivalent to join(",")
    {
        auto vm_tostring_fn = RcPtr<JSFunction>::make();
        vm_tostring_fn->set_name(std::string("toString"));
        vm_tostring_fn->set_native_fn([](Value this_val, std::vector<Value>, bool) -> EvalResult {
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
        gc_heap_.Register(vm_tostring_fn.get());
        array_prototype_->define_builtin_property("toString", Value::object(ObjectPtr(vm_tostring_fn)));
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
    {
        // Array.from(arrayLike[, mapFn])
        auto from_fn = RcPtr<JSFunction>::make();
        from_fn->set_name(std::string("from"));
        from_fn->set_property("length", Value::number(1.0));
        from_fn->set_native_fn([this](Value, std::vector<Value> args, bool) -> EvalResult {
            if (args.empty()) {
                native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                    "undefined is not iterable");
                return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
            }
            Value iterable = args[0];
            Value map_fn = args.size() >= 2 ? args[1] : Value::undefined();
            bool has_map = map_fn.is_object() && map_fn.as_object_raw() &&
                           map_fn.as_object_raw()->object_kind() == ObjectKind::kFunction;
            auto arr = RcPtr<JSObject>::make(ObjectKind::kArray);
            gc_heap_.Register(arr.get());
            arr->set_proto(array_prototype_);
            // array-like path: object with .length but no Symbol.iterator
            if (iterable.is_object()) {
                RcObject* raw = iterable.as_object_raw();
                Value iter_factory = Value::undefined();
                if (raw->object_kind() == ObjectKind::kOrdinary || raw->object_kind() == ObjectKind::kArray ||
                    raw->object_kind() == ObjectKind::kGenerator ||
                    raw->object_kind() == ObjectKind::kMap || raw->object_kind() == ObjectKind::kSet) {
                    iter_factory = static_cast<JSObject*>(raw)->get_property_by_symbol(
                        symbol_table_.well_known_iterator);
                }
                if (iter_factory.is_undefined() && raw->object_kind() == ObjectKind::kOrdinary) {
                    auto* obj = static_cast<JSObject*>(raw);
                    Value len_val = obj->get_property("length");
                    double len_num = len_val.is_number() ? len_val.as_number() : 0.0;
                    uint32_t len = 0;
                    if (!std::isnan(len_num) && len_num > 0.0) {
                        len = static_cast<uint32_t>(std::min(len_num, static_cast<double>(UINT32_MAX)));
                    }
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
                    return EvalResult::ok(Value::object(ObjectPtr(arr)));
                }
                // iterable path: consume via Symbol.iterator
                if (!iter_factory.is_undefined()) {
                    auto iter_r = call_function_val(iter_factory, iterable, {});
                    if (!iter_r.is_ok()) return iter_r;
                    Value iterator = iter_r.value();
                    if (!iterator.is_object()) {
                        native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                            "iterator must be an object");
                        return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
                    }
                    Value next_method = Value::undefined();
                    ObjectKind ik = iterator.as_object_raw()->object_kind();
                    if (ik == ObjectKind::kOrdinary || ik == ObjectKind::kArray ||
                        ik == ObjectKind::kGenerator ||
                        ik == ObjectKind::kMap || ik == ObjectKind::kSet) {
                        next_method = static_cast<JSObject*>(iterator.as_object_raw())->get_property("next");
                    }
                    uint32_t idx = 0;
                    while (true) {
                        auto next_r = call_function_val(next_method, iterator, {});
                        if (!next_r.is_ok()) return next_r;
                        Value result = next_r.value();
                        if (!result.is_object()) break;
                        auto* res_obj = static_cast<JSObject*>(result.as_object_raw());
                        if (to_boolean(res_obj->get_property("done"))) break;
                        Value elem = res_obj->get_property("value");
                        if (has_map) {
                            std::vector<Value> map_args = {elem, Value::number(static_cast<double>(idx))};
                            auto res = call_function_val(map_fn, Value::undefined(),
                                std::span<Value>(map_args.data(), map_args.size()));
                            if (!res.is_ok()) return res;
                            elem = res.value();
                        }
                        arr->elements_[idx++] = std::move(elem);
                    }
                    arr->array_length_ = idx;
                    return EvalResult::ok(Value::object(ObjectPtr(arr)));
                }
                // kArray fast path (also supports mapFn)
                if (raw->object_kind() == ObjectKind::kArray) {
                    auto* src = static_cast<JSObject*>(raw);
                    for (uint32_t i = 0; i < src->array_length_; ++i) {
                        auto it = src->elements_.find(i);
                        Value elem = (it != src->elements_.end()) ? it->second : Value::undefined();
                        if (has_map) {
                            std::vector<Value> map_args = {elem, Value::number(static_cast<double>(i))};
                            auto res = call_function_val(map_fn, Value::undefined(),
                                std::span<Value>(map_args.data(), map_args.size()));
                            if (!res.is_ok()) return res;
                            elem = res.value();
                        }
                        arr->elements_[i] = std::move(elem);
                    }
                    arr->array_length_ = src->array_length_;
                    return EvalResult::ok(Value::object(ObjectPtr(arr)));
                }
            }
            // String path
            if (iterable.is_string()) {
                std::string_view sv = iterable.sv();
                size_t pos = 0;
                uint32_t idx = 0;
                while (pos < sv.size()) {
                    unsigned char c0 = static_cast<unsigned char>(sv[pos]);
                    size_t cp_bytes = (c0 < 0x80) ? 1 : (c0 < 0xE0) ? 2 : (c0 < 0xF0) ? 3 : 4;
                    if (pos + cp_bytes > sv.size()) cp_bytes = sv.size() - pos;
                    Value elem = Value::string(std::string(sv.data() + pos, cp_bytes));
                    if (has_map) {
                        std::vector<Value> map_args = {elem, Value::number(static_cast<double>(idx))};
                        auto res = call_function_val(map_fn, Value::undefined(),
                            std::span<Value>(map_args.data(), map_args.size()));
                        if (!res.is_ok()) return res;
                        elem = res.value();
                    }
                    arr->elements_[idx++] = std::move(elem);
                    pos += cp_bytes;
                }
                arr->array_length_ = idx;
                return EvalResult::ok(Value::object(ObjectPtr(arr)));
            }
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "value is not iterable");
            return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
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
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "Object.defineProperty called on non-object");
            return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
        }
        RcObject* raw = args[0].as_object_raw();
        // For kFunction, handle defineProperty via own_properties_
        if (raw->object_kind() == ObjectKind::kFunction) {
            auto* fn = static_cast<JSFunction*>(raw);
            if (args.size() >= 3 && args[2].is_object()) {
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
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "Object.defineProperty called on non-ordinary object");
            return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
        }
        auto* obj = static_cast<JSObject*>(raw);
        // Symbol key: store as symbol property (data value only)
        if (args.size() >= 2 && args[1].is_symbol()) {
            uint64_t sym_id = args[1].as_symbol_id();
            if (args.size() < 3 || !args[2].is_object()) {
                native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                    "Property description must be an object");
                return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
            }
            RcObject* desc_raw2 = args[2].as_object_raw();
            if (desc_raw2->object_kind() != ObjectKind::kOrdinary) {
                native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                    "Property description must be an object");
                return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
            }
            auto* desc_obj2 = static_cast<JSObject*>(desc_raw2);
            Value val = desc_obj2->has_own_property("value") ? desc_obj2->get_property("value") : Value::undefined();
            obj->set_property_by_symbol(sym_id, std::move(val));
            return EvalResult::ok(args[0]);
        }
        std::string key = args.size() >= 2 ? to_string_val(args[1]) : "undefined";
        if (args.size() < 3 || !args[2].is_object()) {
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "Property description must be an object");
            return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
        }
        RcObject* desc_raw = args[2].as_object_raw();
        if (desc_raw->object_kind() != ObjectKind::kOrdinary) {
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "Property description must be an object");
            return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
        }
        auto* desc_obj = static_cast<JSObject*>(desc_raw);
        PropDesc pd;
        bool has_value = desc_obj->has_own_property("value");
        bool has_writable = desc_obj->has_own_property("writable");
        bool has_get = desc_obj->has_own_property("get");
        bool has_set = desc_obj->has_own_property("set");
        bool has_enumerable = desc_obj->has_own_property("enumerable");
        bool has_configurable = desc_obj->has_own_property("configurable");
        if ((has_value || has_writable) && (has_get || has_set)) {
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "Invalid property descriptor. Cannot both specify accessors and a value or writable attribute");
            return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
        }
        if (has_value) pd.value = desc_obj->get_property("value");
        if (has_writable) pd.writable = to_boolean(desc_obj->get_property("writable"));
        if (has_get) {
            Value get_val = desc_obj->get_property("get");
            if (!get_val.is_undefined() && !get_val.is_null()) {
                if (!get_val.is_object() || get_val.as_object_raw()->object_kind() != ObjectKind::kFunction) {
                    native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                        "Getter must be a function");
                    return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
                }
            }
            pd.getter = std::move(get_val);
        }
        if (has_set) {
            Value set_val = desc_obj->get_property("set");
            if (!set_val.is_undefined() && !set_val.is_null()) {
                if (!set_val.is_object() || set_val.as_object_raw()->object_kind() != ObjectKind::kFunction) {
                    native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                        "Setter must be a function");
                    return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
                }
            }
            pd.setter = std::move(set_val);
        }
        if (has_enumerable) pd.enumerable = to_boolean(desc_obj->get_property("enumerable"));
        if (has_configurable) pd.configurable = to_boolean(desc_obj->get_property("configurable"));
        auto res = obj->define_property(key, pd);
        if (!res.is_ok()) {
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                strip_error_prefix(res.error().message()));
            return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
        }
        return EvalResult::ok(args[0]);
    });

    // Build Object.getOwnPropertyDescriptor
    auto get_own_prop_desc_fn = RcPtr<JSFunction>::make();
    get_own_prop_desc_fn->set_name(std::string("getOwnPropertyDescriptor"));
    get_own_prop_desc_fn->set_native_fn([this](Value /*this_val*/, std::vector<Value> args, bool) -> EvalResult {
        if (args.size() < 1) {
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "Object.getOwnPropertyDescriptor called on non-object");
            return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
        }
        // ES2015+: null/undefined 仍然 TypeError，其他原始值返回 undefined
        if (!args[0].is_object()) {
            if (args[0].is_null() || args[0].is_undefined()) {
                native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                    "Object.getOwnPropertyDescriptor called on null or undefined");
                return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
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
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "Object.preventExtensions called on non-object");
            return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
        }
        RcObject* raw = args[0].as_object_raw();
        if (raw->object_kind() == ObjectKind::kOrdinary || raw->object_kind() == ObjectKind::kArray) {
            static_cast<JSObject*>(raw)->set_extensible(false);
        }
        return EvalResult::ok(args[0]);
    });

    // Build Object.freeze
    auto vm_freeze_fn = RcPtr<JSFunction>::make();
    vm_freeze_fn->set_name(std::string("freeze"));
    vm_freeze_fn->set_native_fn([](Value /*this_val*/, std::vector<Value> args, bool) -> EvalResult {
        if (args.empty()) return EvalResult::ok(Value::undefined());
        if (!args[0].is_object()) return EvalResult::ok(args[0]);
        RcObject* raw = args[0].as_object_raw();
        if (raw->object_kind() == ObjectKind::kOrdinary || raw->object_kind() == ObjectKind::kArray) {
            static_cast<JSObject*>(raw)->freeze();
        }
        return EvalResult::ok(args[0]);
    });
    gc_heap_.Register(vm_freeze_fn.get());

    // Build Object.isFrozen
    auto vm_is_frozen_fn = RcPtr<JSFunction>::make();
    vm_is_frozen_fn->set_name(std::string("isFrozen"));
    vm_is_frozen_fn->set_native_fn([](Value /*this_val*/, std::vector<Value> args, bool) -> EvalResult {
        if (args.empty() || !args[0].is_object()) return EvalResult::ok(Value::boolean(true));
        RcObject* raw = args[0].as_object_raw();
        if (raw->object_kind() == ObjectKind::kOrdinary || raw->object_kind() == ObjectKind::kArray) {
            return EvalResult::ok(Value::boolean(static_cast<JSObject*>(raw)->is_frozen()));
        }
        return EvalResult::ok(Value::boolean(false));
    });
    gc_heap_.Register(vm_is_frozen_fn.get());

    // Build Object.seal
    auto vm_seal_fn = RcPtr<JSFunction>::make();
    vm_seal_fn->set_name(std::string("seal"));
    vm_seal_fn->set_native_fn([](Value /*this_val*/, std::vector<Value> args, bool) -> EvalResult {
        if (args.empty()) return EvalResult::ok(Value::undefined());
        if (!args[0].is_object()) return EvalResult::ok(args[0]);
        RcObject* raw = args[0].as_object_raw();
        if (raw->object_kind() == ObjectKind::kOrdinary || raw->object_kind() == ObjectKind::kArray) {
            static_cast<JSObject*>(raw)->seal();
        }
        return EvalResult::ok(args[0]);
    });
    gc_heap_.Register(vm_seal_fn.get());

    // Build Object.isSealed
    auto vm_is_sealed_fn = RcPtr<JSFunction>::make();
    vm_is_sealed_fn->set_name(std::string("isSealed"));
    vm_is_sealed_fn->set_native_fn([](Value /*this_val*/, std::vector<Value> args, bool) -> EvalResult {
        if (args.empty() || !args[0].is_object()) return EvalResult::ok(Value::boolean(true));
        RcObject* raw = args[0].as_object_raw();
        if (raw->object_kind() == ObjectKind::kOrdinary || raw->object_kind() == ObjectKind::kArray) {
            return EvalResult::ok(Value::boolean(static_cast<JSObject*>(raw)->is_sealed()));
        }
        return EvalResult::ok(Value::boolean(false));
    });
    gc_heap_.Register(vm_is_sealed_fn.get());

    // Build Object.getPrototypeOf
    auto get_proto_vm_fn = RcPtr<JSFunction>::make();
    get_proto_vm_fn->set_name(std::string("getPrototypeOf"));
    get_proto_vm_fn->set_native_fn([this](Value /*this_val*/, std::vector<Value> args, bool) -> EvalResult {
        if (args.empty()) {
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "Object.getPrototypeOf called on non-object");
            return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
        }
        // ES2015+: ToObject — 原始值自动装箱
        if (!args[0].is_object()) {
            if (args[0].is_null() || args[0].is_undefined()) {
                native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                    "Object.getPrototypeOf called on null or undefined");
                return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
            }
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
            if (function_prototype_) return EvalResult::ok(Value::object(ObjectPtr(function_prototype_)));
            return EvalResult::ok(Value::null());
        }
        return EvalResult::ok(Value::null());
    });
    gc_heap_.Register(get_proto_vm_fn.get());

    // Object.values(obj)
    auto vm_values_fn = RcPtr<JSFunction>::make();
    vm_values_fn->set_name(std::string("values"));
    vm_values_fn->set_native_fn([this](Value, std::vector<Value> args, bool) -> EvalResult {
        if (args.empty() || !args[0].is_object()) {
            return EvalResult::err(Error{ErrorKind::Runtime, "TypeError: Object.values called on non-object"});
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
    gc_heap_.Register(vm_values_fn.get());

    // Object.entries(obj)
    auto vm_entries_fn = RcPtr<JSFunction>::make();
    vm_entries_fn->set_name(std::string("entries"));
    vm_entries_fn->set_native_fn([this](Value, std::vector<Value> args, bool) -> EvalResult {
        if (args.empty() || !args[0].is_object()) {
            return EvalResult::err(Error{ErrorKind::Runtime, "TypeError: Object.entries called on non-object"});
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
    gc_heap_.Register(vm_entries_fn.get());

    // Object.fromEntries(iterable)
    auto vm_from_entries_fn = RcPtr<JSFunction>::make();
    vm_from_entries_fn->set_name(std::string("fromEntries"));
    vm_from_entries_fn->set_native_fn([this](Value, std::vector<Value> args, bool) -> EvalResult {
        if (args.empty()) {
            return EvalResult::err(Error{ErrorKind::Runtime,
                "TypeError: Object.fromEntries requires an iterable argument"});
        }
        Value iterable = args[0];
        auto new_obj = RcPtr<JSObject>::make();
        gc_heap_.Register(new_obj.get());
        new_obj->set_proto(object_prototype_);
        // Consume via Symbol.iterator if available, otherwise assume array
        auto process_item = [&](const Value& item) {
            if (!item.is_object()) return;
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
        };
        if (iterable.is_object()) {
            RcObject* raw = iterable.as_object_raw();
            if (raw->object_kind() == ObjectKind::kArray) {
                auto* arr = static_cast<JSObject*>(raw);
                for (uint32_t k = 0; k < arr->array_length_; ++k) {
                    auto it = arr->elements_.find(k);
                    if (it != arr->elements_.end()) process_item(it->second);
                }
            } else {
                Value iter_factory = Value::undefined();
                if (raw->object_kind() == ObjectKind::kOrdinary ||
                    raw->object_kind() == ObjectKind::kMap || raw->object_kind() == ObjectKind::kSet) {
                    iter_factory = static_cast<JSObject*>(raw)->get_property_by_symbol(
                        symbol_table_.well_known_iterator);
                }
                if (!iter_factory.is_undefined()) {
                    auto iter_r = call_function_val(iter_factory, iterable, {});
                    if (!iter_r.is_ok()) return iter_r;
                    Value iterator = iter_r.value();
                    if (!iterator.is_object()) {
                        return EvalResult::err(Error{ErrorKind::Runtime,
                            "TypeError: Object.fromEntries: iterator is not an object"});
                    }
                    Value next_method = Value::undefined();
                    ObjectKind ik = iterator.as_object_raw()->object_kind();
                    if (ik == ObjectKind::kOrdinary || ik == ObjectKind::kArray ||
                        ik == ObjectKind::kGenerator ||
                        ik == ObjectKind::kMap || ik == ObjectKind::kSet) {
                        next_method = static_cast<JSObject*>(iterator.as_object_raw())->get_property("next");
                    }
                    while (true) {
                        auto next_r = call_function_val(next_method, iterator, {});
                        if (!next_r.is_ok()) return next_r;
                        Value result = next_r.value();
                        if (!result.is_object()) break;
                        auto* res_obj = static_cast<JSObject*>(result.as_object_raw());
                        if (to_boolean(res_obj->get_property("done"))) break;
                        process_item(res_obj->get_property("value"));
                    }
                }
            }
        }
        return EvalResult::ok(Value::object(ObjectPtr(new_obj)));
    });
    gc_heap_.Register(vm_from_entries_fn.get());

    // Object.getOwnPropertyNames(obj)
    auto vm_get_own_prop_names_fn = RcPtr<JSFunction>::make();
    vm_get_own_prop_names_fn->set_name(std::string("getOwnPropertyNames"));
    vm_get_own_prop_names_fn->set_native_fn([this](Value, std::vector<Value> args, bool) -> EvalResult {
        if (args.empty() || !args[0].is_object()) {
            return EvalResult::err(Error{ErrorKind::Runtime,
                "TypeError: Object.getOwnPropertyNames called on non-object"});
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
    gc_heap_.Register(vm_get_own_prop_names_fn.get());

    // Object.is(a, b) — SameValue algorithm
    auto vm_object_is_fn = RcPtr<JSFunction>::make();
    vm_object_is_fn->set_name(std::string("is"));
    vm_object_is_fn->set_native_fn([](Value, std::vector<Value> args, bool) -> EvalResult {
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
    gc_heap_.Register(vm_object_is_fn.get());

    // Object.setPrototypeOf(obj, proto)
    auto vm_object_set_proto_fn = RcPtr<JSFunction>::make();
    vm_object_set_proto_fn->set_name(std::string("setPrototypeOf"));
    vm_object_set_proto_fn->set_native_fn([this](Value, std::vector<Value> args, bool) -> EvalResult {
        if (args.size() < 2 || !args[0].is_object()) {
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                                     "Object.setPrototypeOf: first argument must be an object");
            return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
        }
        Value& proto_val = args[1];
        if (!proto_val.is_null() && !proto_val.is_object()) {
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                                     "Object.setPrototypeOf: proto must be an object or null");
            return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
        }
        auto* obj = static_cast<JSObject*>(args[0].as_object_raw());
        if (proto_val.is_null()) {
            obj->set_proto(RcPtr<JSObject>{});
        } else {
            obj->set_proto(RcPtr<JSObject>(static_cast<JSObject*>(proto_val.as_object_raw())));
        }
        return EvalResult::ok(args[0]);
    });
    gc_heap_.Register(vm_object_set_proto_fn.get());

    // Object.hasOwn(obj, key)
    auto vm_object_has_own_fn = RcPtr<JSFunction>::make();
    vm_object_has_own_fn->set_name(std::string("hasOwn"));
    vm_object_has_own_fn->set_native_fn([this](Value, std::vector<Value> args, bool) -> EvalResult {
        if (args.size() < 2 || !args[0].is_object()) {
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                                     "Object.hasOwn: first argument must be an object");
            return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
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
    gc_heap_.Register(vm_object_has_own_fn.get());

    // Object.getOwnPropertySymbols(obj)
    auto vm_get_own_prop_symbols_fn = RcPtr<JSFunction>::make();
    vm_get_own_prop_symbols_fn->set_name(std::string("getOwnPropertySymbols"));
    vm_get_own_prop_symbols_fn->set_native_fn([this](Value, std::vector<Value> args, bool) -> EvalResult {
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
    gc_heap_.Register(vm_get_own_prop_symbols_fn.get());

    object_constructor_->set_property("keys", Value::object(ObjectPtr(keys_fn)));
    object_constructor_->set_property("assign", Value::object(ObjectPtr(assign_fn)));
    object_constructor_->set_property("create", Value::object(ObjectPtr(create_fn)));
    object_constructor_->set_property("defineProperty", Value::object(ObjectPtr(define_property_fn)));
    object_constructor_->set_property("getOwnPropertyDescriptor", Value::object(ObjectPtr(get_own_prop_desc_fn)));
    // Object.getOwnPropertyDescriptors
    {
        auto vm_gopds_fn = RcPtr<JSFunction>::make();
        vm_gopds_fn->set_name(std::string("getOwnPropertyDescriptors"));
        vm_gopds_fn->set_native_fn([this](Value, std::vector<Value> args, bool) -> EvalResult {
            auto result = RcPtr<JSObject>::make();
            gc_heap_.Register(result.get());
            result->set_proto(object_prototype_);
            if (args.empty() || !args[0].is_object()) return EvalResult::ok(Value::object(ObjectPtr(result)));
            auto* raw = args[0].as_object_raw();
            if (raw->object_kind() == ObjectKind::kOrdinary || raw->object_kind() == ObjectKind::kArray) {
                auto* obj = static_cast<JSObject*>(raw);
                for (const auto& key : obj->own_all_string_keys()) {
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
        gc_heap_.Register(vm_gopds_fn.get());
        object_constructor_->set_property("getOwnPropertyDescriptors", Value::object(ObjectPtr(vm_gopds_fn)));
    }
    object_constructor_->set_property("preventExtensions", Value::object(ObjectPtr(prevent_extensions_fn)));
    object_constructor_->set_property("freeze", Value::object(ObjectPtr(vm_freeze_fn)));
    object_constructor_->set_property("isFrozen", Value::object(ObjectPtr(vm_is_frozen_fn)));
    object_constructor_->set_property("seal", Value::object(ObjectPtr(vm_seal_fn)));
    object_constructor_->set_property("isSealed", Value::object(ObjectPtr(vm_is_sealed_fn)));
    object_constructor_->set_property("getPrototypeOf", Value::object(ObjectPtr(get_proto_vm_fn)));
    object_constructor_->set_property("values", Value::object(ObjectPtr(vm_values_fn)));
    object_constructor_->set_property("entries", Value::object(ObjectPtr(vm_entries_fn)));
    object_constructor_->set_property("fromEntries", Value::object(ObjectPtr(vm_from_entries_fn)));
    object_constructor_->set_property("getOwnPropertyNames", Value::object(ObjectPtr(vm_get_own_prop_names_fn)));
    object_constructor_->set_property("is", Value::object(ObjectPtr(vm_object_is_fn)));
    object_constructor_->set_property("setPrototypeOf", Value::object(ObjectPtr(vm_object_set_proto_fn)));
    object_constructor_->set_property("hasOwn", Value::object(ObjectPtr(vm_object_has_own_fn)));
    object_constructor_->set_property("getOwnPropertySymbols", Value::object(ObjectPtr(vm_get_own_prop_symbols_fn)));

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
    function_prototype_->define_builtin_property("call", Value::object(ObjectPtr(call_fn)));

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
    function_prototype_->define_builtin_property("apply", Value::object(ObjectPtr(apply_fn)));

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
    function_prototype_->define_builtin_property("bind", Value::object(ObjectPtr(bind_fn)));

    // Function.prototype.toString
    {
        auto fn = RcPtr<JSFunction>::make();
        fn->set_name(std::string("toString"));
        fn->set_native_fn([this](Value this_val, std::vector<Value>, bool) -> EvalResult {
            if (!this_val.is_object() || !this_val.as_object_raw() ||
                this_val.as_object_raw()->object_kind() != ObjectKind::kFunction) {
                native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                    "Function.prototype.toString requires a function");
                return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
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
    promise_prototype_->define_builtin_property("then", Value::object(ObjectPtr(vm_then_fn)));

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
    promise_prototype_->define_builtin_property("catch", Value::object(ObjectPtr(vm_catch_fn)));

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
    promise_prototype_->define_builtin_property("finally", Value::object(ObjectPtr(vm_finally_fn)));

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

    // Promise.all(iterable)
    {
        auto all_fn = RcPtr<JSFunction>::make();
        all_fn->set_name(std::string("all"));
        all_fn->set_native_fn([this](Value, std::vector<Value> args, bool) -> EvalResult {
            std::vector<Value> items;
            if (!args.empty() && args[0].is_object()) {
                Value iterable_arg = args[0];
                RcObject* iterable_raw = iterable_arg.as_object_raw();
                if (iterable_raw->object_kind() == ObjectKind::kArray) {
                    auto* arr_src = static_cast<JSObject*>(iterable_raw);
                    for (uint32_t k = 0; k < arr_src->array_length_; ++k) {
                        auto it = arr_src->elements_.find(k);
                        items.push_back(it != arr_src->elements_.end() ? it->second : Value::undefined());
                    }
                }
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
                auto p = vm_promise_resolve(items[i]);
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
        vm_promise_ctor->set_property("all", Value::object(ObjectPtr(all_fn)));
    }

    // Promise.race(iterable)
    {
        auto race_fn = RcPtr<JSFunction>::make();
        race_fn->set_name(std::string("race"));
        race_fn->set_native_fn([this](Value, std::vector<Value> args, bool) -> EvalResult {
            std::vector<Value> items;
            if (!args.empty() && args[0].is_object()) {
                Value iterable_arg = args[0];
                RcObject* iterable_raw = iterable_arg.as_object_raw();
                if (iterable_raw->object_kind() == ObjectKind::kArray) {
                    auto* arr_src = static_cast<JSObject*>(iterable_raw);
                    for (uint32_t k = 0; k < arr_src->array_length_; ++k) {
                        auto it = arr_src->elements_.find(k);
                        items.push_back(it != arr_src->elements_.end() ? it->second : Value::undefined());
                    }
                }
            }
            auto result_promise = RcPtr<JSPromise>::make();
            gc_heap_.Register(result_promise.get());
            for (auto& item : items) {
                auto p = vm_promise_resolve(item);
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
        vm_promise_ctor->set_property("race", Value::object(ObjectPtr(race_fn)));
    }

    // Promise.allSettled(iterable)
    {
        auto all_settled_fn = RcPtr<JSFunction>::make();
        all_settled_fn->set_name(std::string("allSettled"));
        all_settled_fn->set_native_fn([this](Value, std::vector<Value> args, bool) -> EvalResult {
            std::vector<Value> items;
            if (!args.empty() && args[0].is_object()) {
                Value iterable_arg = args[0];
                RcObject* iterable_raw = iterable_arg.as_object_raw();
                if (iterable_raw->object_kind() == ObjectKind::kArray) {
                    auto* arr_src = static_cast<JSObject*>(iterable_raw);
                    for (uint32_t k = 0; k < arr_src->array_length_; ++k) {
                        auto it = arr_src->elements_.find(k);
                        items.push_back(it != arr_src->elements_.end() ? it->second : Value::undefined());
                    }
                }
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
                auto p = vm_promise_resolve(items[i]);
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
        vm_promise_ctor->set_property("allSettled", Value::object(ObjectPtr(all_settled_fn)));
    }

    // Promise.any(iterable)
    {
        auto any_fn = RcPtr<JSFunction>::make();
        any_fn->set_name(std::string("any"));
        any_fn->set_native_fn([this](Value, std::vector<Value> args, bool) -> EvalResult {
            std::vector<Value> items;
            if (!args.empty() && args[0].is_object()) {
                Value iterable_arg = args[0];
                RcObject* iterable_raw = iterable_arg.as_object_raw();
                if (iterable_raw->object_kind() == ObjectKind::kArray) {
                    auto* arr_src = static_cast<JSObject*>(iterable_raw);
                    for (uint32_t k = 0; k < arr_src->array_length_; ++k) {
                        auto it = arr_src->elements_.find(k);
                        items.push_back(it != arr_src->elements_.end() ? it->second : Value::undefined());
                    }
                }
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
                auto p = vm_promise_resolve(items[i]);
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
        vm_promise_ctor->set_property("any", Value::object(ObjectPtr(any_fn)));
    }

    gc_heap_.Register(vm_promise_ctor.get());
    global_env_->define("Promise", VarKind::Const);
    global_env_->initialize("Promise", Value::object(ObjectPtr(vm_promise_ctor)));

    // String.prototype (kStringObject wrapper with empty string)
    string_prototype_ = RcPtr<JSObject>::make(ObjectKind::kStringObject);
    string_prototype_->set_wrapped_value(Value::string(""));
    string_prototype_->set_proto(object_prototype_);

    // indexOf(searchString, fromIndex)
    auto vm_str_index_of_fn = RcPtr<JSFunction>::make();
    vm_str_index_of_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        if (this_val.is_null() || this_val.is_undefined()) {
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "String.prototype.indexOf called on null or undefined");
            return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
        }
        Value effective_this = string_this_value_vm(this_val);
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
    string_prototype_->define_builtin_property("indexOf", Value::object(ObjectPtr(vm_str_index_of_fn)));

    // lastIndexOf(searchString, fromIndex)
    auto vm_str_last_index_of_fn = RcPtr<JSFunction>::make();
    vm_str_last_index_of_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        if (this_val.is_null() || this_val.is_undefined()) {
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "String.prototype.lastIndexOf called on null or undefined");
            return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
        }
        Value effective_this = string_this_value_vm(this_val);
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
    string_prototype_->define_builtin_property("lastIndexOf", Value::object(ObjectPtr(vm_str_last_index_of_fn)));

    // slice(start, end)
    auto vm_str_slice_fn = RcPtr<JSFunction>::make();
    vm_str_slice_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        if (this_val.is_null() || this_val.is_undefined()) {
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "String.prototype.slice called on null or undefined");
            return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
        }
        Value effective_this = string_this_value_vm(this_val);
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
    string_prototype_->define_builtin_property("slice", Value::object(ObjectPtr(vm_str_slice_fn)));

    // substring(start, end)
    auto vm_str_substring_fn = RcPtr<JSFunction>::make();
    vm_str_substring_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        if (this_val.is_null() || this_val.is_undefined()) {
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "String.prototype.substring called on null or undefined");
            return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
        }
        Value effective_this = string_this_value_vm(this_val);
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
    string_prototype_->define_builtin_property("substring", Value::object(ObjectPtr(vm_str_substring_fn)));

    // Annex B: substr(start, length) - negative start allowed
    {
        auto fn = RcPtr<JSFunction>::make();
        fn->set_name(std::string("substr"));
        fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
            if (this_val.is_null() || this_val.is_undefined()) {
                native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                    "String.prototype.substr called on null or undefined");
                return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
            }
            std::string str;
            if (this_val.is_string()) {
                str = this_val.sv();
            } else if (this_val.is_object() && this_val.as_object_raw() &&
                       this_val.as_object_raw()->object_kind() == ObjectKind::kStringObject) {
                str = static_cast<JSObject*>(this_val.as_object_raw())->wrapped_value().sv();
            } else {
                str = to_string_val(this_val);
            }
            JSString tmp_str(str);
            int32_t str_len = utf8_cp_len_vm(&tmp_str);
            int32_t start = 0;
            if (!args.empty() && !args[0].is_undefined()) {
                double n = to_number(args[0]).is_ok() ? to_number(args[0]).value().as_number()
                                                      : std::numeric_limits<double>::quiet_NaN();
                if (!std::isnan(n)) {
                    start = static_cast<int32_t>(std::trunc(n));
                    if (start < 0) start = std::max(0, str_len + start);
                    if (start > str_len) start = str_len;
                }
            }
            int32_t length = str_len - start;
            if (args.size() >= 2 && !args[1].is_undefined()) {
                auto ln = to_number(args[1]);
                double n = ln.is_ok() ? ln.value().as_number() : std::numeric_limits<double>::quiet_NaN();
                if (std::isnan(n) || n <= 0.0) return EvalResult::ok(Value::string(""));
                length = std::min(static_cast<int32_t>(std::trunc(n)), str_len - start);
            }
            if (length <= 0) return EvalResult::ok(Value::string(""));
            return EvalResult::ok(Value::string(utf8_substr_vm(tmp_str.sv(), start, start + length)));
        });
        gc_heap_.Register(fn.get());
        string_prototype_->define_builtin_property("substr", Value::object(ObjectPtr(fn)));
    }

    // split(separator, limit)
    auto vm_str_split_fn = RcPtr<JSFunction>::make();
    vm_str_split_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        if (this_val.is_null() || this_val.is_undefined()) {
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "String.prototype.split called on null or undefined");
            return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
        }
        std::string str(string_this_value_vm(this_val).sv());
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
    string_prototype_->define_builtin_property("split", Value::object(ObjectPtr(vm_str_split_fn)));

    // trim()
    auto vm_str_trim_fn = RcPtr<JSFunction>::make();
    vm_str_trim_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        (void)args;
        if (this_val.is_null() || this_val.is_undefined()) {
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "String.prototype.trim called on null or undefined");
            return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
        }
        return EvalResult::ok(Value::string(utf8_trim_impl_vm(string_this_value_vm(this_val).sv(), true, true)));
    });
    gc_heap_.Register(vm_str_trim_fn.get());
    string_prototype_->define_builtin_property("trim", Value::object(ObjectPtr(vm_str_trim_fn)));

    // trimStart()
    auto vm_str_trim_start_fn = RcPtr<JSFunction>::make();
    vm_str_trim_start_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        (void)args;
        if (this_val.is_null() || this_val.is_undefined()) {
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "String.prototype.trimStart called on null or undefined");
            return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
        }
        return EvalResult::ok(Value::string(utf8_trim_impl_vm(string_this_value_vm(this_val).sv(), true, false)));
    });
    gc_heap_.Register(vm_str_trim_start_fn.get());
    string_prototype_->define_builtin_property("trimStart", Value::object(ObjectPtr(vm_str_trim_start_fn)));

    // trimEnd()
    auto vm_str_trim_end_fn = RcPtr<JSFunction>::make();
    vm_str_trim_end_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        (void)args;
        if (this_val.is_null() || this_val.is_undefined()) {
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "String.prototype.trimEnd called on null or undefined");
            return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
        }
        return EvalResult::ok(Value::string(utf8_trim_impl_vm(string_this_value_vm(this_val).sv(), false, true)));
    });
    gc_heap_.Register(vm_str_trim_end_fn.get());
    string_prototype_->define_builtin_property("trimEnd", Value::object(ObjectPtr(vm_str_trim_end_fn)));

    // toLowerCase() / toUpperCase()
    {
        auto vm_lower_fn = RcPtr<JSFunction>::make();
        vm_lower_fn->set_name(std::string("toLowerCase"));
        vm_lower_fn->set_native_fn([this](Value this_val, std::vector<Value>, bool) -> EvalResult {
            if (this_val.is_null() || this_val.is_undefined()) {
                native_pending_throw_ = make_error_value(NativeErrorType::kTypeError, "null or undefined");
                return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
            }
            std::string s = std::string(string_this_value_vm(this_val).sv());
            for (auto& c : s) if (c >= 'A' && c <= 'Z') c += 32;
            return EvalResult::ok(Value::string(s));
        });
        gc_heap_.Register(vm_lower_fn.get());
        string_prototype_->define_builtin_property("toLowerCase", Value::object(ObjectPtr(vm_lower_fn)));
        string_prototype_->define_builtin_property("toLocaleLowerCase", Value::object(ObjectPtr(vm_lower_fn)));
    }
    {
        auto vm_upper_fn = RcPtr<JSFunction>::make();
        vm_upper_fn->set_name(std::string("toUpperCase"));
        vm_upper_fn->set_native_fn([this](Value this_val, std::vector<Value>, bool) -> EvalResult {
            if (this_val.is_null() || this_val.is_undefined()) {
                native_pending_throw_ = make_error_value(NativeErrorType::kTypeError, "null or undefined");
                return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
            }
            std::string s = std::string(string_this_value_vm(this_val).sv());
            for (auto& c : s) if (c >= 'a' && c <= 'z') c -= 32;
            return EvalResult::ok(Value::string(s));
        });
        gc_heap_.Register(vm_upper_fn.get());
        string_prototype_->define_builtin_property("toUpperCase", Value::object(ObjectPtr(vm_upper_fn)));
        string_prototype_->define_builtin_property("toLocaleUpperCase", Value::object(ObjectPtr(vm_upper_fn)));
    }

    // valueOf()
    auto vm_str_valueof_fn = RcPtr<JSFunction>::make();
    vm_str_valueof_fn->set_native_fn([this](Value this_val, std::vector<Value>, bool) -> EvalResult {
        if (this_val.is_string()) return EvalResult::ok(this_val);
        if (this_val.is_object()) {
            RcObject* raw = this_val.as_object_raw();
            if (raw->object_kind() == ObjectKind::kStringObject) {
                return EvalResult::ok(static_cast<JSObject*>(raw)->wrapped_value());
            }
        }
        native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
            "String.prototype.valueOf requires a string or String object");
        return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
    });
    gc_heap_.Register(vm_str_valueof_fn.get());
    string_prototype_->define_builtin_property("valueOf", Value::object(ObjectPtr(vm_str_valueof_fn)));

    // toString()
    auto vm_str_tostring_fn = RcPtr<JSFunction>::make();
    vm_str_tostring_fn->set_native_fn([this](Value this_val, std::vector<Value>, bool) -> EvalResult {
        if (this_val.is_string()) return EvalResult::ok(this_val);
        if (this_val.is_object()) {
            RcObject* raw = this_val.as_object_raw();
            if (raw->object_kind() == ObjectKind::kStringObject) {
                return EvalResult::ok(static_cast<JSObject*>(raw)->wrapped_value());
            }
        }
        native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
            "String.prototype.toString requires a string or String object");
        return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
    });
    gc_heap_.Register(vm_str_tostring_fn.get());
    string_prototype_->define_builtin_property("toString", Value::object(ObjectPtr(vm_str_tostring_fn)));

    auto string_iterator_fn = RcPtr<JSFunction>::make();
    string_iterator_fn->set_name(std::string("[Symbol.iterator]"));
    string_iterator_fn->set_native_fn([this](Value this_val, std::vector<Value>, bool) -> EvalResult {
        if (this_val.is_null() || this_val.is_undefined()) {
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                                     "String.prototype[Symbol.iterator] called on null or undefined");
            return EvalResult::err(Error{ErrorKind::Runtime, "__qppjs_pending_throw__"});
        }
        Value str_val = string_this_value_vm(this_val);
        auto iter_obj = RcPtr<JSObject>::make();
        gc_heap_.Register(iter_obj.get());
        iter_obj->set_property("__str__", std::move(str_val));
        iter_obj->set_property("__pos__", Value::number(0.0));

        auto next_fn = RcPtr<JSFunction>::make();
        next_fn->set_name(std::string("next"));
        next_fn->set_native_fn([this](Value this_val, std::vector<Value>, bool) -> EvalResult {
            auto make_done = [&]() -> EvalResult {
                auto res = RcPtr<JSObject>::make();
                gc_heap_.Register(res.get());
                res->set_property("value", Value::undefined());
                res->set_property("done", Value::boolean(true));
                return EvalResult::ok(Value::object(ObjectPtr(res)));
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
            auto res = RcPtr<JSObject>::make();
            gc_heap_.Register(res.get());
            res->set_property("value", Value::string(std::move(ch)));
            res->set_property("done", Value::boolean(false));
            return EvalResult::ok(Value::object(ObjectPtr(res)));
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
    gc_heap_.Register(string_iterator_fn.get());
    string_prototype_->set_property_by_symbol(symbol_table_.well_known_iterator,
                                              Value::object(ObjectPtr(string_iterator_fn)));

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
    // Number.parseFloat === global parseFloat
    number_constructor_->set_property("parseFloat", Value::object(ObjectPtr(vm_parse_float_fn)));
    // Number.isSafeInteger
    {
        auto vm_is_safe_int_fn = RcPtr<JSFunction>::make();
        vm_is_safe_int_fn->set_name(std::string("isSafeInteger"));
        vm_is_safe_int_fn->set_native_fn([](Value, std::vector<Value> args, bool) -> EvalResult {
            if (args.empty() || !args[0].is_number()) return EvalResult::ok(Value::boolean(false));
            double v = args[0].as_number();
            return EvalResult::ok(Value::boolean(std::isfinite(v) && v == std::trunc(v) && std::abs(v) <= 9007199254740991.0));
        });
        gc_heap_.Register(vm_is_safe_int_fn.get());
        number_constructor_->set_property("isSafeInteger", Value::object(ObjectPtr(vm_is_safe_int_fn)));
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
    auto vm_num_valueof_fn = RcPtr<JSFunction>::make();
    vm_num_valueof_fn->set_native_fn([this](Value this_val, std::vector<Value> /*args*/, bool) -> EvalResult {
        if (this_val.is_number()) return EvalResult::ok(this_val);
        native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
            "Number.prototype.valueOf requires a number");
        return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
    });
    gc_heap_.Register(vm_num_valueof_fn.get());
    number_prototype_->define_builtin_property("valueOf", Value::object(ObjectPtr(vm_num_valueof_fn)));

    // Number.prototype.toString([radix])
    auto vm_num_tostring_fn = RcPtr<JSFunction>::make();
    vm_num_tostring_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        if (!this_val.is_number()) {
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "Number.prototype.toString requires a number");
            return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
        }
        double val = this_val.as_number();
        int radix = 10;
        if (!args.empty() && !args[0].is_undefined()) {
            double r = to_number_double_vm(args[0]);
            radix = static_cast<int>(std::trunc(r));
        }
        if (radix < 2 || radix > 36) {
            native_pending_throw_ = make_error_value(NativeErrorType::kRangeError,
                "toString() radix must be between 2 and 36");
            return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
        }
        if (std::isnan(val)) return EvalResult::ok(Value::string("NaN"));
        if (std::isinf(val)) return EvalResult::ok(Value::string(val > 0 ? "Infinity" : "-Infinity"));
        if (radix == 10) {
            return EvalResult::ok(Value::string(to_string_val(this_val)));
        }
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
    gc_heap_.Register(vm_num_tostring_fn.get());
    number_prototype_->define_builtin_property("toString", Value::object(ObjectPtr(vm_num_tostring_fn)));

    // Number.prototype.toFixed([digits])
    auto vm_num_tofixed_fn = RcPtr<JSFunction>::make();
    vm_num_tofixed_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        if (!this_val.is_number()) {
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "Number.prototype.toFixed requires a number");
            return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
        }
        double val = this_val.as_number();
        int digits = 0;
        if (!args.empty() && !args[0].is_undefined()) {
            double d = to_number_double_vm(args[0]);
            digits = static_cast<int>(std::trunc(d));
        }
        if (digits < 0 || digits > 100) {
            native_pending_throw_ = make_error_value(NativeErrorType::kRangeError,
                "toFixed() digits must be between 0 and 100");
            return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
        }
        if (std::isnan(val)) return EvalResult::ok(Value::string("NaN"));
        if (std::isinf(val)) return EvalResult::ok(Value::string(val > 0 ? "Infinity" : "-Infinity"));
        if (std::fabs(val) >= 1e21) return EvalResult::ok(Value::string(to_string_val(this_val)));
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%.*f", digits, val);
        return EvalResult::ok(Value::string(buf));
    });
    gc_heap_.Register(vm_num_tofixed_fn.get());
    number_prototype_->define_builtin_property("toFixed", Value::object(ObjectPtr(vm_num_tofixed_fn)));

    // Number.prototype.toExponential([digits])
    auto vm_num_toexp_fn = RcPtr<JSFunction>::make();
    vm_num_toexp_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        if (!this_val.is_number()) {
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "Number.prototype.toExponential requires a number");
            return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
        }
        double val = this_val.as_number();
        if (std::isnan(val)) return EvalResult::ok(Value::string("NaN"));
        if (std::isinf(val)) return EvalResult::ok(Value::string(val > 0 ? "Infinity" : "-Infinity"));
        int digits = -1;
        if (!args.empty() && !args[0].is_undefined()) {
            double d = to_number_double_vm(args[0]);
            digits = static_cast<int>(std::trunc(d));
            if (digits < 0 || digits > 100) {
                native_pending_throw_ = make_error_value(NativeErrorType::kRangeError,
                    "toExponential() digits must be between 0 and 100");
                return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
            }
        }
        char buf[256];
        if (digits < 0) {
            std::snprintf(buf, sizeof(buf), "%e", val);
        } else {
            std::snprintf(buf, sizeof(buf), "%.*e", digits, val);
        }
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
    gc_heap_.Register(vm_num_toexp_fn.get());
    number_prototype_->define_builtin_property("toExponential", Value::object(ObjectPtr(vm_num_toexp_fn)));

    // Number.prototype.toPrecision([prec])
    auto vm_num_toprec_fn = RcPtr<JSFunction>::make();
    vm_num_toprec_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        if (!this_val.is_number()) {
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "Number.prototype.toPrecision requires a number");
            return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
        }
        double val = this_val.as_number();
        if (args.empty() || args[0].is_undefined()) {
            return EvalResult::ok(Value::string(to_string_val(this_val)));
        }
        int prec = static_cast<int>(std::trunc(to_number_double_vm(args[0])));
        if (prec < 1 || prec > 100) {
            native_pending_throw_ = make_error_value(NativeErrorType::kRangeError,
                "toPrecision() precision must be between 1 and 100");
            return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
        }
        if (std::isnan(val)) return EvalResult::ok(Value::string("NaN"));
        if (std::isinf(val)) return EvalResult::ok(Value::string(val > 0 ? "Infinity" : "-Infinity"));
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%.*g", prec, val);
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
    gc_heap_.Register(vm_num_toprec_fn.get());
    number_prototype_->define_builtin_property("toPrecision", Value::object(ObjectPtr(vm_num_toprec_fn)));

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
                    native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                        "Number.prototype.toLocaleString requires a number");
                    return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
                }
                val = static_cast<JSObject*>(raw)->wrapped_value().as_number();
            } else {
                native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                    "Number.prototype.toLocaleString requires a number");
                return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
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
    auto vm_bool_valueof_fn = RcPtr<JSFunction>::make();
    vm_bool_valueof_fn->set_native_fn([this](Value this_val, std::vector<Value> /*args*/, bool) -> EvalResult {
        if (this_val.is_bool()) return EvalResult::ok(this_val);
        if (this_val.is_object()) {
            RcObject* raw = this_val.as_object_raw();
            if (raw->object_kind() == ObjectKind::kBooleanObject) {
                return EvalResult::ok(static_cast<JSObject*>(raw)->wrapped_value());
            }
        }
        native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
            "Boolean.prototype.valueOf requires a boolean or Boolean object");
        return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
    });
    gc_heap_.Register(vm_bool_valueof_fn.get());
    boolean_prototype_->define_builtin_property("valueOf", Value::object(ObjectPtr(vm_bool_valueof_fn)));

    // Boolean.prototype.toString
    auto vm_bool_tostring_fn = RcPtr<JSFunction>::make();
    vm_bool_tostring_fn->set_native_fn([this](Value this_val, std::vector<Value> /*args*/, bool) -> EvalResult {
        bool b = false;
        if (this_val.is_bool()) {
            b = this_val.as_bool();
        } else if (this_val.is_object()) {
            RcObject* raw = this_val.as_object_raw();
            if (raw->object_kind() == ObjectKind::kBooleanObject) {
                b = static_cast<JSObject*>(raw)->wrapped_value().as_bool();
            } else {
                native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                    "Boolean.prototype.toString requires a boolean or Boolean object");
                return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
            }
        } else {
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "Boolean.prototype.toString requires a boolean or Boolean object");
            return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
        }
        return EvalResult::ok(Value::string(b ? "true" : "false"));
    });
    gc_heap_.Register(vm_bool_tostring_fn.get());
    boolean_prototype_->define_builtin_property("toString", Value::object(ObjectPtr(vm_bool_tostring_fn)));

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
                const std::string* desc = symbol_table_.GetDescription(args[0].as_symbol_id());
                std::string result = desc ? ("Symbol(" + *desc + ")") : "Symbol()";
                return EvalResult::ok(Value::string(result));
            }
            std::string s = args.empty() ? std::string("") : to_string_val(args[0]);
            return EvalResult::ok(Value::string(s));
        }
        // new String(...): Symbol throws TypeError
        if (!args.empty() && args[0].is_symbol()) {
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "Cannot convert a Symbol value to a string");
            return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
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
    auto vm_str_from_char_code_fn = RcPtr<JSFunction>::make();
    vm_str_from_char_code_fn->set_native_fn([](Value, std::vector<Value> args, bool) -> EvalResult {
        std::string result;
        for (auto& arg : args) {
            double n = to_number_double_vm(arg);
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
    gc_heap_.Register(vm_str_from_char_code_fn.get());
    string_constructor_->set_property("fromCharCode", Value::object(ObjectPtr(vm_str_from_char_code_fn)));

    // String.fromCodePoint(...codePoints) — encodes each code point as UTF-8
    {
        auto fn = RcPtr<JSFunction>::make();
        fn->set_name(std::string("fromCodePoint"));
        fn->set_native_fn([](Value, std::vector<Value> args, bool) -> EvalResult {
            std::string result;
            for (auto& arg : args) {
                double n = to_number_double_vm(arg);
                double trunc_n = std::trunc(n);
                if (n != trunc_n || trunc_n < 0.0 || trunc_n > 0x10FFFF) {
                    return EvalResult::err(Error{ErrorKind::Runtime,
                        "RangeError: Invalid code point"});
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

    // ---- RegExp prototype ----

    regexp_prototype_ = RcPtr<JSObject>::make();
    regexp_prototype_->set_proto(object_prototype_);
    gc_heap_.Register(regexp_prototype_.get());

    // RegExp.prototype.exec
    auto vm_regexp_exec_fn = RcPtr<JSFunction>::make();
    vm_regexp_exec_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        if (!this_val.is_object() || !this_val.as_object_raw() ||
            this_val.as_object_raw()->object_kind() != ObjectKind::kRegExp) {
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "RegExp.prototype.exec called on non-RegExp");
            return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
        }
        auto* rx = static_cast<JSRegExp*>(this_val.as_object_raw());
        std::string input = args.empty() ? "undefined" : to_string_val(args[0]);
        return vm_regexp_exec(rx, input);
    });
    regexp_prototype_->define_builtin_property("exec", Value::object(ObjectPtr(vm_regexp_exec_fn)));

    // RegExp.prototype.test
    auto vm_regexp_test_fn = RcPtr<JSFunction>::make();
    vm_regexp_test_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        if (!this_val.is_object() || !this_val.as_object_raw() ||
            this_val.as_object_raw()->object_kind() != ObjectKind::kRegExp) {
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "RegExp.prototype.test called on non-RegExp");
            return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
        }
        auto* rx = static_cast<JSRegExp*>(this_val.as_object_raw());
        std::string input = args.empty() ? "undefined" : to_string_val(args[0]);
        auto res = vm_regexp_exec(rx, input);
        if (!res.is_ok()) return res;
        return EvalResult::ok(Value::boolean(!res.value().is_null()));
    });
    regexp_prototype_->define_builtin_property("test", Value::object(ObjectPtr(vm_regexp_test_fn)));

    // RegExp.prototype.toString
    auto vm_regexp_tostring_fn = RcPtr<JSFunction>::make();
    vm_regexp_tostring_fn->set_native_fn([](Value this_val, std::vector<Value> /*args*/, bool) -> EvalResult {
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
    regexp_prototype_->define_builtin_property("toString", Value::object(ObjectPtr(vm_regexp_tostring_fn)));

    // ---- RegExp constructor ----

    regexp_constructor_ = RcPtr<JSFunction>::make();
    regexp_constructor_->set_name(std::string("RegExp"));
    regexp_constructor_->set_native_fn([this](Value /*this_val*/, std::vector<Value> args,
                                              bool is_new) -> EvalResult {
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
        return vm_make_regexp(pattern, flags);
    });
    regexp_constructor_->set_prototype_obj(RcPtr<JSObject>(regexp_prototype_));
    regexp_constructor_->set_property("prototype", Value::object(ObjectPtr(regexp_prototype_)));
    gc_heap_.Register(regexp_constructor_.get());
    global_env_->define_initialized("RegExp");
    global_env_->set("RegExp", Value::object(ObjectPtr(regexp_constructor_)));

    // ---- String.prototype.match ----

    auto vm_string_match_fn = RcPtr<JSFunction>::make();
    vm_string_match_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        if (this_val.is_undefined() || this_val.is_null()) {
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "String.prototype.match called on null or undefined");
            return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
        }
        std::string str(string_this_value_vm(this_val).sv());
        if (args.empty() || args[0].is_undefined()) {
            auto rx_res = vm_make_regexp("", "");
            if (!rx_res.is_ok()) return rx_res;
            auto* rx = static_cast<JSRegExp*>(rx_res.value().as_object_raw());
            return vm_regexp_exec(rx, str);
        }
        if (!args[0].is_object() || !args[0].as_object_raw() ||
            args[0].as_object_raw()->object_kind() != ObjectKind::kRegExp) {
            std::string pat = to_string_val(args[0]);
            auto rx_res = vm_make_regexp(pat, "");
            if (!rx_res.is_ok()) return rx_res;
            auto* rx = static_cast<JSRegExp*>(rx_res.value().as_object_raw());
            return vm_regexp_exec(rx, str);
        }
        auto* rx = static_cast<JSRegExp*>(args[0].as_object_raw());
        if (!rx->global_) {
            return vm_regexp_exec(rx, str);
        }
        rx->last_index_ = 0;
        auto result_arr = RcPtr<JSObject>::make(ObjectKind::kArray);
        gc_heap_.Register(result_arr.get());
        result_arr->set_proto(array_prototype_);
        while (true) {
            if (rx->last_index_ > static_cast<uint32_t>(str.size())) break;
            auto exec_res = vm_regexp_exec(rx, str);
            if (!exec_res.is_ok()) return exec_res;
            if (exec_res.value().is_null()) break;
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
        string_prototype_->define_builtin_property("match", Value::object(ObjectPtr(vm_string_match_fn)));
    }

    // ---- String.prototype.search ----

    auto vm_string_search_fn = RcPtr<JSFunction>::make();
    vm_string_search_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        if (this_val.is_undefined() || this_val.is_null()) {
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "String.prototype.search called on null or undefined");
            return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
        }
        std::string str(string_this_value_vm(this_val).sv());
        JSRegExp* rx = nullptr;
        RcPtr<JSObject> rx_holder;
        if (args.empty() || args[0].is_undefined()) {
            auto rx_res = vm_make_regexp("", "");
            if (!rx_res.is_ok()) return rx_res;
            rx_holder = RcPtr<JSObject>(static_cast<JSObject*>(rx_res.value().as_object_raw()));
            rx = static_cast<JSRegExp*>(rx_holder.get());
        } else if (args[0].is_object() && args[0].as_object_raw() &&
                   args[0].as_object_raw()->object_kind() == ObjectKind::kRegExp) {
            rx = static_cast<JSRegExp*>(args[0].as_object_raw());
        } else {
            std::string pat = to_string_val(args[0]);
            auto rx_res = vm_make_regexp(pat, "");
            if (!rx_res.is_ok()) return rx_res;
            rx_holder = RcPtr<JSObject>(static_cast<JSObject*>(rx_res.value().as_object_raw()));
            rx = static_cast<JSRegExp*>(rx_holder.get());
        }
        uint32_t saved_last_index = rx->last_index_;
        rx->last_index_ = 0;
        auto exec_res = vm_regexp_exec(rx, str);
        rx->last_index_ = saved_last_index;
        if (!exec_res.is_ok()) return exec_res;
        if (exec_res.value().is_null()) return EvalResult::ok(Value::number(-1.0));
        auto* match_arr = static_cast<JSObject*>(exec_res.value().as_object_raw());
        Value idx_val = match_arr->get_property("index");
        return EvalResult::ok(idx_val.is_number() ? idx_val : Value::number(-1.0));
    });
    gc_heap_.Register(vm_string_search_fn.get());
    if (string_prototype_) {
        string_prototype_->define_builtin_property("search", Value::object(ObjectPtr(vm_string_search_fn)));
    }

    // ---- String.prototype.replace ----

    auto vm_string_replace_fn = RcPtr<JSFunction>::make();
    vm_string_replace_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        if (this_val.is_undefined() || this_val.is_null()) {
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "String.prototype.replace called on null or undefined");
            return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
        }
        std::string str(string_this_value_vm(this_val).sv());
        Value search_val = args.size() >= 1 ? args[0] : Value::undefined();
        Value replace_val = args.size() >= 2 ? args[1] : Value::undefined();

        bool is_regexp = search_val.is_object() && search_val.as_object_raw() &&
                         search_val.as_object_raw()->object_kind() == ObjectKind::kRegExp;

        if (is_regexp) {
            auto* rx = static_cast<JSRegExp*>(search_val.as_object_raw());
            bool is_global = rx->global_;
            rx->last_index_ = 0;

            if (!is_global) {
                auto exec_res = vm_regexp_exec(rx, str);
                if (!exec_res.is_ok()) return exec_res;
                if (exec_res.value().is_null()) return EvalResult::ok(Value::string(str));
                auto* match_arr = static_cast<JSObject*>(exec_res.value().as_object_raw());
                std::string matched = match_arr->elements_.count(0) ? match_arr->elements_[0].as_string() : "";
                Value idx_val = match_arr->get_property("index");
                size_t match_start = idx_val.is_number() ? static_cast<size_t>(idx_val.as_number()) : 0;

                std::string repl;
                if (replace_val.is_object() && replace_val.as_object_raw() &&
                    replace_val.as_object_raw()->object_kind() == ObjectKind::kFunction) {
                    Value call_args[3] = {Value::string(matched),
                                          Value::number(static_cast<double>(match_start)),
                                          Value::string(str)};
                    auto r = call_function_val(replace_val, Value::undefined(), std::span<Value>(call_args, 3));
                    if (!r.is_ok()) return r;
                    repl = to_string_val(r.value());
                } else {
                    repl = to_string_val(replace_val);
                }
                std::string result = str.substr(0, match_start) + repl + str.substr(match_start + matched.size());
                return EvalResult::ok(Value::string(result));
            }

            // Global regexp replace
            const std::string orig_str = str;
            std::string result;
            size_t last_end = 0;
            rx->last_index_ = 0;
            while (true) {
                if (rx->last_index_ > static_cast<uint32_t>(orig_str.size())) break;
                auto exec_res = vm_regexp_exec(rx, orig_str);
                if (!exec_res.is_ok()) return exec_res;
                if (exec_res.value().is_null()) break;
                auto* match_arr = static_cast<JSObject*>(exec_res.value().as_object_raw());
                std::string matched = match_arr->elements_.count(0) ? match_arr->elements_[0].as_string() : "";
                Value idx_val = match_arr->get_property("index");
                size_t match_start = idx_val.is_number() ? static_cast<size_t>(idx_val.as_number()) : 0;

                result += orig_str.substr(last_end, match_start - last_end);
                if (replace_val.is_object() && replace_val.as_object_raw() &&
                    replace_val.as_object_raw()->object_kind() == ObjectKind::kFunction) {
                    Value call_args[3] = {Value::string(matched),
                                          Value::number(static_cast<double>(match_start)),
                                          Value::string(orig_str)};
                    auto r = call_function_val(replace_val, Value::undefined(), std::span<Value>(call_args, 3));
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

        std::string repl;
        if (replace_val.is_object() && replace_val.as_object_raw() &&
            replace_val.as_object_raw()->object_kind() == ObjectKind::kFunction) {
            Value call_args[3] = {Value::string(search_str),
                                  Value::number(static_cast<double>(pos)),
                                  Value::string(str)};
            auto r = call_function_val(replace_val, Value::undefined(), std::span<Value>(call_args, 3));
            if (!r.is_ok()) return r;
            repl = to_string_val(r.value());
        } else {
            repl = to_string_val(replace_val);
        }
        std::string result = str.substr(0, pos) + repl + str.substr(pos + search_str.size());
        return EvalResult::ok(Value::string(result));
    });
    gc_heap_.Register(vm_string_replace_fn.get());
    if (string_prototype_) {
        string_prototype_->define_builtin_property("replace", Value::object(ObjectPtr(vm_string_replace_fn)));
    }

    // ---- String.prototype.replaceAll ----

    auto vm_string_replace_all_fn = RcPtr<JSFunction>::make();
    vm_string_replace_all_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        if (this_val.is_undefined() || this_val.is_null()) {
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "String.prototype.replaceAll called on null or undefined");
            return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
        }
        std::string str(string_this_value_vm(this_val).sv());
        Value search_val = args.size() >= 1 ? args[0] : Value::undefined();
        Value replace_val = args.size() >= 2 ? args[1] : Value::undefined();

        if (search_val.is_object() && search_val.as_object_raw() &&
            search_val.as_object_raw()->object_kind() == ObjectKind::kRegExp) {
            auto* rx = static_cast<JSRegExp*>(search_val.as_object_raw());
            if (!rx->global_) {
                native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                    "String.prototype.replaceAll requires global flag for RegExp");
                return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
            }
        }

        std::string search_str = to_string_val(search_val);
        bool is_fn = replace_val.is_object() && replace_val.as_object_raw() &&
                     replace_val.as_object_raw()->object_kind() == ObjectKind::kFunction;
        std::string repl_str = is_fn ? "" : to_string_val(replace_val);

        std::string result;
        if (search_str.empty()) {
            for (size_t i = 0; i <= str.size(); ++i) {
                if (is_fn) {
                    Value call_args[3] = {Value::string(""),
                                          Value::number(static_cast<double>(i)),
                                          Value::string(str)};
                    auto r = call_function_val(replace_val, Value::undefined(), std::span<Value>(call_args, 3));
                    if (!r.is_ok()) return r;
                    result += to_string_val(r.value());
                } else {
                    result += repl_str;
                }
                if (i < str.size()) result += str[i];
            }
        } else {
            size_t pos = 0;
            while (true) {
                size_t found = str.find(search_str, pos);
                if (found == std::string::npos) {
                    result += str.substr(pos);
                    break;
                }
                result += str.substr(pos, found - pos);
                if (is_fn) {
                    Value call_args[3] = {Value::string(search_str),
                                          Value::number(static_cast<double>(found)),
                                          Value::string(str)};
                    auto r = call_function_val(replace_val, Value::undefined(), std::span<Value>(call_args, 3));
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
    gc_heap_.Register(vm_string_replace_all_fn.get());
    if (string_prototype_) {
        string_prototype_->define_builtin_property("replaceAll", Value::object(ObjectPtr(vm_string_replace_all_fn)));
    }

    // ---- String.prototype.at ----

    auto vm_string_at_fn = RcPtr<JSFunction>::make();
    vm_string_at_fn->set_native_fn([](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        if (this_val.is_undefined() || this_val.is_null()) {
            return EvalResult::ok(Value::undefined());
        }
        Value str_val = string_this_value_vm(this_val);
        auto* js_str = str_val.js_string_raw();
        int32_t len = js_str ? utf8_cp_len_vm(js_str) : static_cast<int32_t>(str_val.sv().size());
        double idx_d = args.empty() ? 0.0 : to_number_double_vm(args[0]);
        if (std::isnan(idx_d)) idx_d = 0.0;
        int32_t idx = static_cast<int32_t>(std::trunc(idx_d));
        if (idx < 0) idx = len + idx;
        if (idx < 0 || idx >= len) return EvalResult::ok(Value::undefined());
        return EvalResult::ok(Value::string(utf8_substr_vm(str_val.sv(), idx, idx + 1)));
    });
    gc_heap_.Register(vm_string_at_fn.get());
    if (string_prototype_) {
        string_prototype_->define_builtin_property("at", Value::object(ObjectPtr(vm_string_at_fn)));
    }

    // ---- String.prototype.padStart ----

    auto vm_string_pad_start_fn = RcPtr<JSFunction>::make();
    vm_string_pad_start_fn->set_native_fn([](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        if (this_val.is_undefined() || this_val.is_null()) {
            return EvalResult::ok(Value::undefined());
        }
        std::string str(string_this_value_vm(this_val).sv());
        int32_t target_len = args.empty() ? 0 : static_cast<int32_t>(to_number_double_vm(args[0]));
        std::string pad_str = args.size() >= 2 && !args[1].is_undefined() ? VM::to_string_val(args[1]) : " ";
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
    gc_heap_.Register(vm_string_pad_start_fn.get());
    if (string_prototype_) {
        string_prototype_->define_builtin_property("padStart", Value::object(ObjectPtr(vm_string_pad_start_fn)));
    }

    // ---- String.prototype.padEnd ----

    auto vm_string_pad_end_fn = RcPtr<JSFunction>::make();
    vm_string_pad_end_fn->set_native_fn([](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        if (this_val.is_undefined() || this_val.is_null()) {
            return EvalResult::ok(Value::undefined());
        }
        std::string str(string_this_value_vm(this_val).sv());
        int32_t target_len = args.empty() ? 0 : static_cast<int32_t>(to_number_double_vm(args[0]));
        std::string pad_str = args.size() >= 2 && !args[1].is_undefined() ? VM::to_string_val(args[1]) : " ";
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
    gc_heap_.Register(vm_string_pad_end_fn.get());
    if (string_prototype_) {
        string_prototype_->define_builtin_property("padEnd", Value::object(ObjectPtr(vm_string_pad_end_fn)));
    }

    // ---- String.prototype.repeat ----

    auto vm_string_repeat_fn = RcPtr<JSFunction>::make();
    vm_string_repeat_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        if (this_val.is_undefined() || this_val.is_null()) {
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "String.prototype.repeat called on null or undefined");
            return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
        }
        std::string str(string_this_value_vm(this_val).sv());
        double count_d = args.empty() ? 0.0 : to_number_double_vm(args[0]);
        if (std::isnan(count_d)) count_d = 0.0;
        if (count_d < 0 || std::isinf(count_d)) {
            native_pending_throw_ = make_error_value(NativeErrorType::kRangeError,
                "Invalid count value");
            return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
        }
        int32_t count = static_cast<int32_t>(std::trunc(count_d));
        if (count == 0 || str.empty()) return EvalResult::ok(Value::string(""));
        std::string result;
        result.reserve(str.size() * static_cast<size_t>(count));
        for (int32_t i = 0; i < count; ++i) result += str;
        return EvalResult::ok(Value::string(result));
    });
    gc_heap_.Register(vm_string_repeat_fn.get());
    if (string_prototype_) {
        string_prototype_->define_builtin_property("repeat", Value::object(ObjectPtr(vm_string_repeat_fn)));
    }

    // ---- String.prototype.startsWith ----

    auto vm_string_starts_with_fn = RcPtr<JSFunction>::make();
    vm_string_starts_with_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        if (this_val.is_undefined() || this_val.is_null()) {
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "String.prototype.startsWith called on null or undefined");
            return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
        }
        if (!args.empty() && args[0].is_object() && args[0].as_object_raw() &&
            args[0].as_object_raw()->object_kind() == ObjectKind::kRegExp) {
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "First argument to startsWith must not be a RegExp");
            return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
        }
        std::string_view str = string_this_value_vm(this_val).sv();
        std::string search = args.empty() ? "undefined" : to_string_val(args[0]);
        int32_t pos = 0;
        if (args.size() >= 2 && !args[1].is_undefined()) {
            double p = to_number_double_vm(args[1]);
            pos = std::isnan(p) ? 0 : static_cast<int32_t>(std::max(0.0, std::trunc(p)));
        }
        if (pos < 0) pos = 0;
        if (static_cast<size_t>(pos) > str.size()) return EvalResult::ok(Value::boolean(false));
        auto sub = str.substr(static_cast<size_t>(pos));
        bool result = sub.size() >= search.size() && sub.substr(0, search.size()) == search;
        return EvalResult::ok(Value::boolean(result));
    });
    gc_heap_.Register(vm_string_starts_with_fn.get());
    if (string_prototype_) {
        string_prototype_->define_builtin_property("startsWith", Value::object(ObjectPtr(vm_string_starts_with_fn)));
    }

    // ---- String.prototype.endsWith ----

    auto vm_string_ends_with_fn = RcPtr<JSFunction>::make();
    vm_string_ends_with_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        if (this_val.is_undefined() || this_val.is_null()) {
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "String.prototype.endsWith called on null or undefined");
            return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
        }
        if (!args.empty() && args[0].is_object() && args[0].as_object_raw() &&
            args[0].as_object_raw()->object_kind() == ObjectKind::kRegExp) {
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "First argument to endsWith must not be a RegExp");
            return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
        }
        std::string_view str = string_this_value_vm(this_val).sv();
        std::string search = args.empty() ? "undefined" : to_string_val(args[0]);
        int32_t end_pos = static_cast<int32_t>(str.size());
        if (args.size() >= 2 && !args[1].is_undefined()) {
            double p = to_number_double_vm(args[1]);
            if (!std::isnan(p)) end_pos = static_cast<int32_t>(std::min(std::max(0.0, std::trunc(p)),
                                                                          static_cast<double>(str.size())));
        }
        if (end_pos < 0) end_pos = 0;
        std::string_view sub = str.substr(0, static_cast<size_t>(end_pos));
        if (search.size() > sub.size()) return EvalResult::ok(Value::boolean(false));
        bool result = sub.substr(sub.size() - search.size()) == search;
        return EvalResult::ok(Value::boolean(result));
    });
    gc_heap_.Register(vm_string_ends_with_fn.get());
    if (string_prototype_) {
        string_prototype_->define_builtin_property("endsWith", Value::object(ObjectPtr(vm_string_ends_with_fn)));
    }

    // ---- String.prototype.includes ----

    auto vm_string_includes_fn = RcPtr<JSFunction>::make();
    vm_string_includes_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        if (this_val.is_undefined() || this_val.is_null()) {
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "String.prototype.includes called on null or undefined");
            return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
        }
        if (!args.empty() && args[0].is_object() && args[0].as_object_raw() &&
            args[0].as_object_raw()->object_kind() == ObjectKind::kRegExp) {
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "First argument to includes must not be a RegExp");
            return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
        }
        std::string_view str = string_this_value_vm(this_val).sv();
        std::string search = args.empty() ? "undefined" : to_string_val(args[0]);
        int32_t pos = 0;
        if (args.size() >= 2 && !args[1].is_undefined()) {
            double p = to_number_double_vm(args[1]);
            pos = std::isnan(p) ? 0 : static_cast<int32_t>(std::max(0.0, std::trunc(p)));
        }
        if (static_cast<size_t>(pos) > str.size()) return EvalResult::ok(Value::boolean(false));
        bool result = str.substr(static_cast<size_t>(pos)).find(search) != std::string_view::npos;
        return EvalResult::ok(Value::boolean(result));
    });
    gc_heap_.Register(vm_string_includes_fn.get());
    if (string_prototype_) {
        string_prototype_->define_builtin_property("includes", Value::object(ObjectPtr(vm_string_includes_fn)));
    }

    // ---- String.prototype.matchAll ----

    auto vm_string_match_all_fn = RcPtr<JSFunction>::make();
    vm_string_match_all_fn->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
        if (this_val.is_undefined() || this_val.is_null()) {
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "String.prototype.matchAll called on null or undefined");
            return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
        }
        std::string str(string_this_value_vm(this_val).sv());
        Value regexp_val = args.empty() ? Value::undefined() : args[0];

        if (regexp_val.is_object() && regexp_val.as_object_raw() &&
            regexp_val.as_object_raw()->object_kind() == ObjectKind::kRegExp) {
            auto* rx = static_cast<JSRegExp*>(regexp_val.as_object_raw());
            if (!rx->global_ && !rx->sticky_) {
                native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                    "String.prototype.matchAll requires global or sticky flag");
                return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
            }
        } else {
            std::string pat = regexp_val.is_undefined() ? "" : to_string_val(regexp_val);
            auto rx_res = vm_make_regexp(pat, "g");
            if (!rx_res.is_ok()) return rx_res;
            regexp_val = rx_res.value();
        }

        auto iter_obj = RcPtr<JSObject>::make();
        gc_heap_.Register(iter_obj.get());
        if (object_prototype_) iter_obj->set_proto(object_prototype_);
        iter_obj->set_property("__str__", Value::string(str));
        iter_obj->set_property("__rx__", regexp_val);

        auto vm_next_fn = RcPtr<JSFunction>::make();
        vm_next_fn->set_native_fn([this](Value iter_this, std::vector<Value> /*args*/, bool) -> EvalResult {
            if (!iter_this.is_object()) {
                return EvalResult::ok(Value::undefined());
            }
            auto* iter = static_cast<JSObject*>(iter_this.as_object_raw());
            Value str_val = iter->get_property("__str__");
            Value rx_val = iter->get_property("__rx__");
            if (!rx_val.is_object() || !rx_val.as_object_raw() ||
                rx_val.as_object_raw()->object_kind() != ObjectKind::kRegExp) {
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
            auto exec_res = vm_regexp_exec(rx, str);
            if (!exec_res.is_ok()) return exec_res;
            if (exec_res.value().is_null()) {
                iter->set_property("__rx__", Value::undefined());
                auto done_obj = RcPtr<JSObject>::make();
                gc_heap_.Register(done_obj.get());
                done_obj->set_property("value", Value::undefined());
                done_obj->set_property("done", Value::boolean(true));
                return EvalResult::ok(Value::object(ObjectPtr(done_obj)));
            }
            auto* match_arr = static_cast<JSObject*>(exec_res.value().as_object_raw());
            Value match0 = match_arr->elements_.count(0) ? match_arr->elements_[0] : Value::string("");
            if (match0.is_string() && match0.sv().empty()) rx->last_index_++;

            auto result_obj = RcPtr<JSObject>::make();
            gc_heap_.Register(result_obj.get());
            result_obj->set_property("value", exec_res.value());
            result_obj->set_property("done", Value::boolean(false));
            return EvalResult::ok(Value::object(ObjectPtr(result_obj)));
        });
        gc_heap_.Register(vm_next_fn.get());
        iter_obj->set_property("next", Value::object(ObjectPtr(vm_next_fn)));

        return EvalResult::ok(Value::object(ObjectPtr(iter_obj)));
    });
    gc_heap_.Register(vm_string_match_all_fn.get());
    if (string_prototype_) {
        string_prototype_->define_builtin_property("matchAll", Value::object(ObjectPtr(vm_string_match_all_fn)));
    }

    // ---- String.prototype.charCodeAt ----
    {
        auto fn = RcPtr<JSFunction>::make();
        fn->set_name(std::string("charCodeAt"));
        fn->set_native_fn([](Value this_val, std::vector<Value> args, bool) -> EvalResult {
            Value sv = string_this_value_vm(this_val);
            std::string_view s = sv.js_string_raw()->sv();
            double idx_d = args.empty() ? 0.0 : to_number_double_vm(args[0]);
            int32_t idx = static_cast<int32_t>(std::isfinite(idx_d) ? std::trunc(idx_d) : 0.0);
            int32_t len = utf8_cp_len_vm(sv.js_string_raw());
            if (idx < 0 || idx >= len) {
                return EvalResult::ok(Value::number(std::numeric_limits<double>::quiet_NaN()));
            }
            size_t byte_pos = utf8_cu_to_byte_vm(s, idx);
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
            Value sv = string_this_value_vm(this_val);
            std::string_view s = sv.js_string_raw()->sv();
            double idx_d = args.empty() ? 0.0 : to_number_double_vm(args[0]);
            int32_t idx = static_cast<int32_t>(std::isfinite(idx_d) ? std::trunc(idx_d) : 0.0);
            int32_t len = utf8_cp_len_vm(sv.js_string_raw());
            if (idx < 0 || idx >= len) {
                return EvalResult::ok(Value::string(""));
            }
            return EvalResult::ok(Value::string(utf8_substr_vm(s, idx, idx + 1)));
        });
        gc_heap_.Register(fn.get());
        if (string_prototype_) string_prototype_->define_builtin_property("charAt", Value::object(ObjectPtr(fn)));
    }

    // ---- String.prototype.codePointAt ----
    {
        auto fn = RcPtr<JSFunction>::make();
        fn->set_name(std::string("codePointAt"));
        fn->set_native_fn([](Value this_val, std::vector<Value> args, bool) -> EvalResult {
            Value sv = string_this_value_vm(this_val);
            std::string_view s = sv.js_string_raw()->sv();
            double idx_d = args.empty() ? 0.0 : to_number_double_vm(args[0]);
            int32_t idx = static_cast<int32_t>(std::isfinite(idx_d) ? std::trunc(idx_d) : 0.0);
            int32_t len = utf8_cp_len_vm(sv.js_string_raw());
            if (idx < 0 || idx >= len) return EvalResult::ok(Value::undefined());
            size_t byte_pos = utf8_cu_to_byte_vm(s, idx);
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
                native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                    "String.prototype.normalize requires a string");
                return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
            }
            Value sv = string_this_value_vm(this_val);
            std::string form = "NFC";
            if (!args.empty() && !args[0].is_undefined()) {
                form = to_string_val(args[0]);
            }
            if (form != "NFC" && form != "NFD" && form != "NFKC" && form != "NFKD") {
                native_pending_throw_ = make_error_value(NativeErrorType::kRangeError,
                    "The normalization form should be one of NFC, NFD, NFKC, NFKD");
                return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
            }
            return EvalResult::ok(sv);
        });
        gc_heap_.Register(fn.get());
        if (string_prototype_) string_prototype_->define_builtin_property("normalize", Value::object(ObjectPtr(fn)));
    }

    // concat(), trimLeft/trimRight aliases, localeCompare
    {
        auto vm_concat_fn = RcPtr<JSFunction>::make();
        vm_concat_fn->set_name(std::string("concat"));
        vm_concat_fn->set_native_fn([](Value this_val, std::vector<Value> args, bool) -> EvalResult {
            std::string result = std::string(string_this_value_vm(this_val).sv());
            for (auto& a : args) result += to_string_val(a);
            return EvalResult::ok(Value::string(result));
        });
        gc_heap_.Register(vm_concat_fn.get());
        if (string_prototype_) string_prototype_->define_builtin_property("concat", Value::object(ObjectPtr(vm_concat_fn)));
    }
    {
        Value ts = string_prototype_ ? string_prototype_->get_property("trimStart") : Value::undefined();
        Value te = string_prototype_ ? string_prototype_->get_property("trimEnd") : Value::undefined();
        if (string_prototype_) {
            string_prototype_->define_builtin_property("trimLeft", ts);
            string_prototype_->define_builtin_property("trimRight", te);
        }
    }
    {
        auto vm_lc_fn = RcPtr<JSFunction>::make();
        vm_lc_fn->set_name(std::string("localeCompare"));
        vm_lc_fn->set_native_fn([](Value this_val, std::vector<Value> args, bool) -> EvalResult {
            std::string a = std::string(string_this_value_vm(this_val).sv());
            std::string b = args.empty() ? "undefined" : to_string_val(args[0]);
            int cmp = a.compare(b);
            return EvalResult::ok(Value::number(cmp < 0 ? -1.0 : (cmp > 0 ? 1.0 : 0.0)));
        });
        gc_heap_.Register(vm_lc_fn.get());
        if (string_prototype_) string_prototype_->define_builtin_property("localeCompare", Value::object(ObjectPtr(vm_lc_fn)));
    }

    // ---- Annex B HTML string wrapping methods ----
    {
        auto vm_make_tag = [this](const char* open, const char* close, const char* name_str) {
            auto fn = RcPtr<JSFunction>::make();
            fn->set_name(std::string(name_str));
            std::string open_s = open;
            std::string close_s = close;
            fn->set_native_fn([this, open_s, close_s](Value this_val, std::vector<Value> /*args*/, bool) -> EvalResult {
                if (this_val.is_null() || this_val.is_undefined()) {
                    native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                        "Cannot call method on null or undefined");
                    return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
                }
                std::string str;
                if (this_val.is_string()) {
                    str = this_val.sv();
                } else if (this_val.is_object() && this_val.as_object_raw() &&
                           this_val.as_object_raw()->object_kind() == ObjectKind::kStringObject) {
                    str = static_cast<JSObject*>(this_val.as_object_raw())->wrapped_value().sv();
                } else {
                    str = to_string_val(this_val);
                }
                return EvalResult::ok(Value::string(open_s + str + close_s));
            });
            gc_heap_.Register(fn.get());
            return fn;
        };
        auto vm_make_attr = [this](const char* tag, const char* attr, const char* name_str) {
            auto fn = RcPtr<JSFunction>::make();
            fn->set_name(std::string(name_str));
            std::string tag_s = tag;
            std::string attr_s = attr;
            fn->set_native_fn([this, tag_s, attr_s](Value this_val, std::vector<Value> args, bool) -> EvalResult {
                if (this_val.is_null() || this_val.is_undefined()) {
                    native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                        "Cannot call method on null or undefined");
                    return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
                }
                std::string str = this_val.is_string() ? std::string(this_val.sv()) : to_string_val(this_val);
                std::string val = args.empty() ? "undefined" : to_string_val(args[0]);
                std::string escaped;
                for (char c : val) {
                    if (c == '"') escaped += "&quot;";
                    else escaped += c;
                }
                return EvalResult::ok(Value::string(
                    "<" + tag_s + " " + attr_s + "=\"" + escaped + "\">" + str + "</" + tag_s + ">"));
            });
            gc_heap_.Register(fn.get());
            return fn;
        };
        if (string_prototype_) {
            string_prototype_->define_builtin_property("big",       Value::object(ObjectPtr(vm_make_tag("<big>",    "</big>",    "big"))));
            string_prototype_->define_builtin_property("blink",     Value::object(ObjectPtr(vm_make_tag("<blink>",  "</blink>",  "blink"))));
            string_prototype_->define_builtin_property("bold",      Value::object(ObjectPtr(vm_make_tag("<b>",      "</b>",      "bold"))));
            string_prototype_->define_builtin_property("fixed",     Value::object(ObjectPtr(vm_make_tag("<tt>",     "</tt>",     "fixed"))));
            string_prototype_->define_builtin_property("italics",   Value::object(ObjectPtr(vm_make_tag("<i>",      "</i>",      "italics"))));
            string_prototype_->define_builtin_property("small",     Value::object(ObjectPtr(vm_make_tag("<small>",  "</small>",  "small"))));
            string_prototype_->define_builtin_property("strike",    Value::object(ObjectPtr(vm_make_tag("<strike>", "</strike>", "strike"))));
            string_prototype_->define_builtin_property("sub",       Value::object(ObjectPtr(vm_make_tag("<sub>",    "</sub>",    "sub"))));
            string_prototype_->define_builtin_property("sup",       Value::object(ObjectPtr(vm_make_tag("<sup>",    "</sup>",    "sup"))));
            string_prototype_->define_builtin_property("anchor",    Value::object(ObjectPtr(vm_make_attr("a",    "name",  "anchor"))));
            string_prototype_->define_builtin_property("link",      Value::object(ObjectPtr(vm_make_attr("a",    "href",  "link"))));
            string_prototype_->define_builtin_property("fontcolor", Value::object(ObjectPtr(vm_make_attr("font", "color", "fontcolor"))));
            string_prototype_->define_builtin_property("fontsize",  Value::object(ObjectPtr(vm_make_attr("font", "size",  "fontsize"))));
        }
    }

    // ---- Symbol ----

    symbol_prototype_ = RcPtr<JSObject>::make();
    symbol_prototype_->set_proto(object_prototype_);
    gc_heap_.Register(symbol_prototype_.get());

    // Symbol.prototype.toString
    auto vm_sym_tostring_fn = RcPtr<JSFunction>::make();
    vm_sym_tostring_fn->set_native_fn([this](Value this_val, std::vector<Value> /*args*/, bool) -> EvalResult {
        if (!this_val.is_symbol()) {
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "Symbol.prototype.toString requires a symbol");
            return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
        }
        const std::string* desc = symbol_table_.GetDescription(this_val.as_symbol_id());
        std::string result = desc ? ("Symbol(" + *desc + ")") : "Symbol()";
        return EvalResult::ok(Value::string(result));
    });
    symbol_prototype_->define_builtin_property("toString", Value::object(ObjectPtr(vm_sym_tostring_fn)));
    gc_heap_.Register(vm_sym_tostring_fn.get());

    // Symbol.prototype.valueOf
    auto vm_sym_valueof_fn = RcPtr<JSFunction>::make();
    vm_sym_valueof_fn->set_native_fn([](Value this_val, std::vector<Value> /*args*/, bool) -> EvalResult {
        return EvalResult::ok(this_val);
    });
    symbol_prototype_->define_builtin_property("valueOf", Value::object(ObjectPtr(vm_sym_valueof_fn)));
    gc_heap_.Register(vm_sym_valueof_fn.get());

    // Symbol constructor
    symbol_constructor_ = RcPtr<JSFunction>::make();
    symbol_constructor_->set_name(std::string("Symbol"));
    symbol_constructor_->set_native_fn([this](Value /*this_val*/, std::vector<Value> args, bool is_new_call) -> EvalResult {
        if (is_new_call) {
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "Symbol is not a constructor");
            return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
        }
        std::optional<std::string> description = std::nullopt;
        if (!args.empty() && !args[0].is_undefined()) {
            description = to_string_val(args[0]);
        }
        uint64_t id = symbol_table_.NewSymbol(std::move(description));
        return EvalResult::ok(Value::symbol(id));
    });

    // Symbol.for
    auto vm_sym_for_fn = RcPtr<JSFunction>::make();
    vm_sym_for_fn->set_native_fn([this](Value /*this_val*/, std::vector<Value> args, bool) -> EvalResult {
        std::string key = args.empty() ? "undefined" : to_string_val(args[0]);
        uint64_t id = symbol_table_.ForKey(key);
        return EvalResult::ok(Value::symbol(id));
    });
    symbol_constructor_->set_property("for", Value::object(ObjectPtr(vm_sym_for_fn)));
    gc_heap_.Register(vm_sym_for_fn.get());

    // Symbol.keyFor
    auto vm_sym_keyfor_fn = RcPtr<JSFunction>::make();
    vm_sym_keyfor_fn->set_native_fn([this](Value /*this_val*/, std::vector<Value> args, bool) -> EvalResult {
        if (args.empty() || !args[0].is_symbol()) {
            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                "Symbol.keyFor argument must be a symbol");
            return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
        }
        auto key = symbol_table_.KeyForId(args[0].as_symbol_id());
        if (!key.has_value()) return EvalResult::ok(Value::undefined());
        return EvalResult::ok(Value::string(*key));
    });
    symbol_constructor_->set_property("keyFor", Value::object(ObjectPtr(vm_sym_keyfor_fn)));
    gc_heap_.Register(vm_sym_keyfor_fn.get());

    // Well-Known Symbols
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
    global_env_->define("Symbol", VarKind::Const);
    global_env_->initialize("Symbol", Value::object(ObjectPtr(symbol_constructor_)));

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

        {
            auto size_fn = RcPtr<JSFunction>::make();
            size_fn->set_native_fn([](Value this_val, std::vector<Value>, bool) -> EvalResult {
                if (!this_val.is_object() || this_val.as_object_raw()->object_kind() != ObjectKind::kMap) {
                    return EvalResult::err(Error{ErrorKind::Runtime, "TypeError: Map.prototype.size requires a Map"});
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

        map_prototype_->define_builtin_property("set", Value::object(ObjectPtr(make_map_method(
            [this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
                if (!this_val.is_object() || this_val.as_object_raw()->object_kind() != ObjectKind::kMap) {
                    native_pending_throw_ = make_error_value(NativeErrorType::kTypeError, "Map.prototype.set called on non-Map");
                    return EvalResult::err(Error{ErrorKind::Runtime, "__qppjs_pending_throw__"});
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

        map_prototype_->define_builtin_property("get", Value::object(ObjectPtr(make_map_method(
            [this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
                if (!this_val.is_object() || this_val.as_object_raw()->object_kind() != ObjectKind::kMap) {
                    native_pending_throw_ = make_error_value(NativeErrorType::kTypeError, "Map.prototype.get called on non-Map");
                    return EvalResult::err(Error{ErrorKind::Runtime, "__qppjs_pending_throw__"});
                }
                auto* m = static_cast<JSMap*>(this_val.as_object_raw());
                Value key = args.size() > 0 ? args[0] : Value::undefined();
                size_t idx = m->find_key(key);
                if (idx == JSMap::kNotFound) return EvalResult::ok(Value::undefined());
                return EvalResult::ok(m->entries_[idx].second);
            }))));

        map_prototype_->define_builtin_property("has", Value::object(ObjectPtr(make_map_method(
            [this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
                if (!this_val.is_object() || this_val.as_object_raw()->object_kind() != ObjectKind::kMap) {
                    native_pending_throw_ = make_error_value(NativeErrorType::kTypeError, "Map.prototype.has called on non-Map");
                    return EvalResult::err(Error{ErrorKind::Runtime, "__qppjs_pending_throw__"});
                }
                auto* m = static_cast<JSMap*>(this_val.as_object_raw());
                Value key = args.size() > 0 ? args[0] : Value::undefined();
                return EvalResult::ok(Value::boolean(m->find_key(key) != JSMap::kNotFound));
            }))));

        map_prototype_->define_builtin_property("delete", Value::object(ObjectPtr(make_map_method(
            [this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
                if (!this_val.is_object() || this_val.as_object_raw()->object_kind() != ObjectKind::kMap) {
                    native_pending_throw_ = make_error_value(NativeErrorType::kTypeError, "Map.prototype.delete called on non-Map");
                    return EvalResult::err(Error{ErrorKind::Runtime, "__qppjs_pending_throw__"});
                }
                auto* m = static_cast<JSMap*>(this_val.as_object_raw());
                Value key = args.size() > 0 ? args[0] : Value::undefined();
                size_t idx = m->find_key(key);
                if (idx == JSMap::kNotFound) return EvalResult::ok(Value::boolean(false));
                m->entries_.erase(m->entries_.begin() + static_cast<ptrdiff_t>(idx));
                return EvalResult::ok(Value::boolean(true));
            }))));

        map_prototype_->define_builtin_property("clear", Value::object(ObjectPtr(make_map_method(
            [this](Value this_val, std::vector<Value>, bool) -> EvalResult {
                if (!this_val.is_object() || this_val.as_object_raw()->object_kind() != ObjectKind::kMap) {
                    native_pending_throw_ = make_error_value(NativeErrorType::kTypeError, "Map.prototype.clear called on non-Map");
                    return EvalResult::err(Error{ErrorKind::Runtime, "__qppjs_pending_throw__"});
                }
                auto* m = static_cast<JSMap*>(this_val.as_object_raw());
                m->entries_.clear();
                return EvalResult::ok(Value::undefined());
            }))));

        map_prototype_->define_builtin_property("forEach", Value::object(ObjectPtr(make_map_method(
            [this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
                if (!this_val.is_object() || this_val.as_object_raw()->object_kind() != ObjectKind::kMap) {
                    native_pending_throw_ = make_error_value(NativeErrorType::kTypeError, "Map.prototype.forEach called on non-Map");
                    return EvalResult::err(Error{ErrorKind::Runtime, "__qppjs_pending_throw__"});
                }
                if (args.empty() || !args[0].is_object() ||
                    args[0].as_object_raw()->object_kind() != ObjectKind::kFunction) {
                    native_pending_throw_ = make_error_value(NativeErrorType::kTypeError, "forEach callback must be a function");
                    return EvalResult::err(Error{ErrorKind::Runtime, "__qppjs_pending_throw__"});
                }
                auto* m = static_cast<JSMap*>(this_val.as_object_raw());
                Value this_arg = args.size() > 1 ? args[1] : Value::undefined();
                size_t sz = m->entries_.size();
                for (size_t i = 0; i < sz; ++i) {
                    if (i >= m->entries_.size()) break;
                    Value cb_args[3] = {m->entries_[i].second, m->entries_[i].first, this_val};
                    auto r = call_function_val(args[0], this_arg, std::span<Value>(cb_args, 3));
                    if (!r.is_ok()) return r;
                }
                return EvalResult::ok(Value::undefined());
            }))));

        auto make_map_iter_fn = [&](CollectionIterMode mode) {
            auto fn = RcPtr<JSFunction>::make();
            fn->set_native_fn([this, mode](Value this_val, std::vector<Value>, bool) -> EvalResult {
                if (!this_val.is_object() || this_val.as_object_raw()->object_kind() != ObjectKind::kMap) {
                    native_pending_throw_ = make_error_value(NativeErrorType::kTypeError, "Map iterator called on non-Map");
                    return EvalResult::err(Error{ErrorKind::Runtime, "__qppjs_pending_throw__"});
                }
                auto* m = static_cast<JSMap*>(this_val.as_object_raw());
                auto iter = RcPtr<JSMapIterator>::make();
                iter->map_ = RcPtr<JSMap>(m);
                iter->index_ = 0;
                iter->mode_ = mode;
                gc_heap_.Register(iter.get());

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

        {
            auto entries_val = map_prototype_->get_property("entries");
            map_prototype_->set_property_by_symbol(symbol_table_.well_known_iterator, entries_val);
        }

        auto map_ctor = RcPtr<JSFunction>::make();
        map_ctor->set_name(std::string("Map"));
        map_ctor->set_native_fn([this](Value, std::vector<Value> args, bool is_new) -> EvalResult {
            if (!is_new) {
                native_pending_throw_ = make_error_value(NativeErrorType::kTypeError, "Constructor Map requires 'new'");
                return EvalResult::err(Error{ErrorKind::Runtime, "__qppjs_pending_throw__"});
            }
            auto m = RcPtr<JSMap>::make();
            gc_heap_.Register(m.get());
            m->set_proto(map_prototype_);
            if (!args.empty() && !args[0].is_undefined() && !args[0].is_null()) {
                // Spread iterable into pairs
                Value iterable = args[0];
                if (!iterable.is_object()) {
                    native_pending_throw_ = make_error_value(NativeErrorType::kTypeError, "Map constructor: iterable is not an object");
                    return EvalResult::err(Error{ErrorKind::Runtime, "__qppjs_pending_throw__"});
                }
                // Use kArray fast path or generic iterator
                RcObject* raw = iterable.as_object_raw();
                if (raw->object_kind() == ObjectKind::kArray) {
                    auto* arr = static_cast<JSObject*>(raw);
                    for (uint32_t i = 0; i < arr->array_length_; ++i) {
                        auto it2 = arr->elements_.find(i);
                        if (it2 == arr->elements_.end()) continue;
                        const Value& item = it2->second;
                        if (!item.is_object()) {
                            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError, "Iterator value is not an object");
                            return EvalResult::err(Error{ErrorKind::Runtime, "__qppjs_pending_throw__"});
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
                } else {
                    // Generic iterator path via Symbol.iterator
                    Value iter_method = iterable.is_object() ?
                        static_cast<JSObject*>(raw)->get_property_by_symbol(symbol_table_.well_known_iterator) :
                        Value::undefined();
                    if (!iter_method.is_object() || iter_method.as_object_raw()->object_kind() != ObjectKind::kFunction) {
                        native_pending_throw_ = make_error_value(NativeErrorType::kTypeError, "Map constructor: not iterable");
                        return EvalResult::err(Error{ErrorKind::Runtime, "__qppjs_pending_throw__"});
                    }
                    auto iter_res = call_function_val(iter_method, iterable, std::span<Value>{});
                    if (!iter_res.is_ok()) return iter_res;
                    Value iter_obj = iter_res.value();
                    if (!iter_obj.is_object()) {
                        native_pending_throw_ = make_error_value(NativeErrorType::kTypeError, "Map constructor: iterator is not an object");
                        return EvalResult::err(Error{ErrorKind::Runtime, "__qppjs_pending_throw__"});
                    }
                    Value next_method = static_cast<JSObject*>(iter_obj.as_object_raw())->get_property("next");
                    while (true) {
                        auto next_res = call_function_val(next_method, iter_obj, std::span<Value>{});
                        if (!next_res.is_ok()) return next_res;
                        Value result = next_res.value();
                        if (!result.is_object()) break;
                        auto* res_obj = static_cast<JSObject*>(result.as_object_raw());
                        Value done = res_obj->get_property("done");
                        if (to_boolean(done)) break;
                        Value item = res_obj->get_property("value");
                        if (!item.is_object()) {
                            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError, "Iterator value is not an object");
                            return EvalResult::err(Error{ErrorKind::Runtime, "__qppjs_pending_throw__"});
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
            }
            return EvalResult::ok(Value::object(ObjectPtr(m)));
        });
        gc_heap_.Register(map_ctor.get());
        global_env_->define("Map", VarKind::Const);
        global_env_->initialize("Map", Value::object(ObjectPtr(map_ctor)));
    }

    // ---- Set ----
    {
        set_prototype_ = RcPtr<JSObject>::make();
        set_prototype_->set_proto(object_prototype_);
        gc_heap_.Register(set_prototype_.get());

        {
            auto size_fn = RcPtr<JSFunction>::make();
            size_fn->set_native_fn([](Value this_val, std::vector<Value>, bool) -> EvalResult {
                if (!this_val.is_object() || this_val.as_object_raw()->object_kind() != ObjectKind::kSet) {
                    return EvalResult::err(Error{ErrorKind::Runtime, "TypeError: Set.prototype.size requires a Set"});
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

        set_prototype_->define_builtin_property("add", Value::object(ObjectPtr(make_set_method(
            [this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
                if (!this_val.is_object() || this_val.as_object_raw()->object_kind() != ObjectKind::kSet) {
                    native_pending_throw_ = make_error_value(NativeErrorType::kTypeError, "Set.prototype.add called on non-Set");
                    return EvalResult::err(Error{ErrorKind::Runtime, "__qppjs_pending_throw__"});
                }
                auto* s = static_cast<JSSet*>(this_val.as_object_raw());
                Value val = args.size() > 0 ? args[0] : Value::undefined();
                if (s->find_value(val) == JSSet::kNotFound) {
                    s->values_.push_back(std::move(val));
                }
                return EvalResult::ok(this_val);
            }))));

        set_prototype_->define_builtin_property("has", Value::object(ObjectPtr(make_set_method(
            [this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
                if (!this_val.is_object() || this_val.as_object_raw()->object_kind() != ObjectKind::kSet) {
                    native_pending_throw_ = make_error_value(NativeErrorType::kTypeError, "Set.prototype.has called on non-Set");
                    return EvalResult::err(Error{ErrorKind::Runtime, "__qppjs_pending_throw__"});
                }
                auto* s = static_cast<JSSet*>(this_val.as_object_raw());
                Value val = args.size() > 0 ? args[0] : Value::undefined();
                return EvalResult::ok(Value::boolean(s->find_value(val) != JSSet::kNotFound));
            }))));

        set_prototype_->define_builtin_property("delete", Value::object(ObjectPtr(make_set_method(
            [this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
                if (!this_val.is_object() || this_val.as_object_raw()->object_kind() != ObjectKind::kSet) {
                    native_pending_throw_ = make_error_value(NativeErrorType::kTypeError, "Set.prototype.delete called on non-Set");
                    return EvalResult::err(Error{ErrorKind::Runtime, "__qppjs_pending_throw__"});
                }
                auto* s = static_cast<JSSet*>(this_val.as_object_raw());
                Value val = args.size() > 0 ? args[0] : Value::undefined();
                size_t idx = s->find_value(val);
                if (idx == JSSet::kNotFound) return EvalResult::ok(Value::boolean(false));
                s->values_.erase(s->values_.begin() + static_cast<ptrdiff_t>(idx));
                return EvalResult::ok(Value::boolean(true));
            }))));

        set_prototype_->define_builtin_property("clear", Value::object(ObjectPtr(make_set_method(
            [this](Value this_val, std::vector<Value>, bool) -> EvalResult {
                if (!this_val.is_object() || this_val.as_object_raw()->object_kind() != ObjectKind::kSet) {
                    native_pending_throw_ = make_error_value(NativeErrorType::kTypeError, "Set.prototype.clear called on non-Set");
                    return EvalResult::err(Error{ErrorKind::Runtime, "__qppjs_pending_throw__"});
                }
                auto* s = static_cast<JSSet*>(this_val.as_object_raw());
                s->values_.clear();
                return EvalResult::ok(Value::undefined());
            }))));

        set_prototype_->define_builtin_property("forEach", Value::object(ObjectPtr(make_set_method(
            [this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
                if (!this_val.is_object() || this_val.as_object_raw()->object_kind() != ObjectKind::kSet) {
                    native_pending_throw_ = make_error_value(NativeErrorType::kTypeError, "Set.prototype.forEach called on non-Set");
                    return EvalResult::err(Error{ErrorKind::Runtime, "__qppjs_pending_throw__"});
                }
                if (args.empty() || !args[0].is_object() ||
                    args[0].as_object_raw()->object_kind() != ObjectKind::kFunction) {
                    native_pending_throw_ = make_error_value(NativeErrorType::kTypeError, "forEach callback must be a function");
                    return EvalResult::err(Error{ErrorKind::Runtime, "__qppjs_pending_throw__"});
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

        auto make_set_iter_fn = [&](CollectionIterMode mode) {
            auto fn = RcPtr<JSFunction>::make();
            fn->set_native_fn([this, mode](Value this_val, std::vector<Value>, bool) -> EvalResult {
                if (!this_val.is_object() || this_val.as_object_raw()->object_kind() != ObjectKind::kSet) {
                    native_pending_throw_ = make_error_value(NativeErrorType::kTypeError, "Set iterator called on non-Set");
                    return EvalResult::err(Error{ErrorKind::Runtime, "__qppjs_pending_throw__"});
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
        set_prototype_->define_builtin_property("keys", Value::object(ObjectPtr(
            make_set_iter_fn(CollectionIterMode::kValues))));
        set_prototype_->define_builtin_property("entries", Value::object(ObjectPtr(
            make_set_iter_fn(CollectionIterMode::kEntries))));

        {
            auto values_val = set_prototype_->get_property("values");
            set_prototype_->set_property_by_symbol(symbol_table_.well_known_iterator, values_val);
        }

        auto set_ctor = RcPtr<JSFunction>::make();
        set_ctor->set_name(std::string("Set"));
        set_ctor->set_native_fn([this](Value, std::vector<Value> args, bool is_new) -> EvalResult {
            if (!is_new) {
                native_pending_throw_ = make_error_value(NativeErrorType::kTypeError, "Constructor Set requires 'new'");
                return EvalResult::err(Error{ErrorKind::Runtime, "__qppjs_pending_throw__"});
            }
            auto s = RcPtr<JSSet>::make();
            gc_heap_.Register(s.get());
            s->set_proto(set_prototype_);
            if (!args.empty() && !args[0].is_undefined() && !args[0].is_null()) {
                Value iterable = args[0];
                RcObject* raw = iterable.is_object() ? iterable.as_object_raw() : nullptr;
                if (raw && raw->object_kind() == ObjectKind::kArray) {
                    auto* arr = static_cast<JSObject*>(raw);
                    for (uint32_t i = 0; i < arr->array_length_; ++i) {
                        auto it2 = arr->elements_.find(i);
                        if (it2 == arr->elements_.end()) continue;
                        Value item = it2->second;
                        if (s->find_value(item) == JSSet::kNotFound) {
                            s->values_.push_back(std::move(item));
                        }
                    }
                } else if (raw) {
                    Value iter_method = static_cast<JSObject*>(raw)->get_property_by_symbol(symbol_table_.well_known_iterator);
                    if (!iter_method.is_object() || iter_method.as_object_raw()->object_kind() != ObjectKind::kFunction) {
                        native_pending_throw_ = make_error_value(NativeErrorType::kTypeError, "Set constructor: not iterable");
                        return EvalResult::err(Error{ErrorKind::Runtime, "__qppjs_pending_throw__"});
                    }
                    auto iter_res = call_function_val(iter_method, iterable, std::span<Value>{});
                    if (!iter_res.is_ok()) return iter_res;
                    Value iter_obj = iter_res.value();
                    if (!iter_obj.is_object()) {
                        native_pending_throw_ = make_error_value(NativeErrorType::kTypeError, "Set constructor: iterator is not an object");
                        return EvalResult::err(Error{ErrorKind::Runtime, "__qppjs_pending_throw__"});
                    }
                    Value next_method = static_cast<JSObject*>(iter_obj.as_object_raw())->get_property("next");
                    while (true) {
                        auto next_res = call_function_val(next_method, iter_obj, std::span<Value>{});
                        if (!next_res.is_ok()) return next_res;
                        Value result = next_res.value();
                        if (!result.is_object()) break;
                        auto* res_obj = static_cast<JSObject*>(result.as_object_raw());
                        Value done = res_obj->get_property("done");
                        if (to_boolean(done)) break;
                        Value item = res_obj->get_property("value");
                        if (s->find_value(item) == JSSet::kNotFound) {
                            s->values_.push_back(std::move(item));
                        }
                    }
                }
            }
            return EvalResult::ok(Value::object(ObjectPtr(s)));
        });
        gc_heap_.Register(set_ctor.get());
        global_env_->define("Set", VarKind::Const);
        global_env_->initialize("Set", Value::object(ObjectPtr(set_ctor)));
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
                    native_pending_throw_ = make_error_value(NativeErrorType::kTypeError, "WeakMap.prototype.set called on non-WeakMap");
                    return EvalResult::err(Error{ErrorKind::Runtime, "__qppjs_pending_throw__"});
                }
                Value key = args.size() > 0 ? args[0] : Value::undefined();
                if (!key.is_object()) {
                    native_pending_throw_ = make_error_value(NativeErrorType::kTypeError, "Invalid value used as weak map key");
                    return EvalResult::err(Error{ErrorKind::Runtime, "__qppjs_pending_throw__"});
                }
                auto* wm = static_cast<JSWeakMap*>(this_val.as_object_raw());
                Value val = args.size() > 1 ? args[1] : Value::undefined();
                wm->table_[key.as_object_raw()] = std::move(val);
                return EvalResult::ok(this_val);
            }))));

        weakmap_prototype_->define_builtin_property("get", Value::object(ObjectPtr(make_wm_method(
            [this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
                if (!this_val.is_object() || this_val.as_object_raw()->object_kind() != ObjectKind::kWeakMap) {
                    native_pending_throw_ = make_error_value(NativeErrorType::kTypeError, "WeakMap.prototype.get called on non-WeakMap");
                    return EvalResult::err(Error{ErrorKind::Runtime, "__qppjs_pending_throw__"});
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
                    native_pending_throw_ = make_error_value(NativeErrorType::kTypeError, "WeakMap.prototype.has called on non-WeakMap");
                    return EvalResult::err(Error{ErrorKind::Runtime, "__qppjs_pending_throw__"});
                }
                Value key = args.size() > 0 ? args[0] : Value::undefined();
                if (!key.is_object()) return EvalResult::ok(Value::boolean(false));
                auto* wm = static_cast<JSWeakMap*>(this_val.as_object_raw());
                return EvalResult::ok(Value::boolean(wm->table_.count(key.as_object_raw()) > 0));
            }))));

        weakmap_prototype_->define_builtin_property("delete", Value::object(ObjectPtr(make_wm_method(
            [this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
                if (!this_val.is_object() || this_val.as_object_raw()->object_kind() != ObjectKind::kWeakMap) {
                    native_pending_throw_ = make_error_value(NativeErrorType::kTypeError, "WeakMap.prototype.delete called on non-WeakMap");
                    return EvalResult::err(Error{ErrorKind::Runtime, "__qppjs_pending_throw__"});
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
                native_pending_throw_ = make_error_value(NativeErrorType::kTypeError, "Constructor WeakMap requires 'new'");
                return EvalResult::err(Error{ErrorKind::Runtime, "__qppjs_pending_throw__"});
            }
            auto wm = RcPtr<JSWeakMap>::make();
            gc_heap_.Register(wm.get());
            wm->set_proto(weakmap_prototype_);
            if (!args.empty() && !args[0].is_undefined() && !args[0].is_null()) {
                Value iterable = args[0];
                RcObject* raw = iterable.is_object() ? iterable.as_object_raw() : nullptr;
                auto process_pair = [&](const Value& item) -> EvalResult {
                    if (!item.is_object()) {
                        native_pending_throw_ = make_error_value(NativeErrorType::kTypeError, "Invalid value used as weak map key");
                        return EvalResult::err(Error{ErrorKind::Runtime, "__qppjs_pending_throw__"});
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
                        native_pending_throw_ = make_error_value(NativeErrorType::kTypeError, "Invalid value used as weak map key");
                        return EvalResult::err(Error{ErrorKind::Runtime, "__qppjs_pending_throw__"});
                    }
                    wm->table_[k.as_object_raw()] = std::move(v);
                    return EvalResult::ok(Value::undefined());
                };
                if (raw && raw->object_kind() == ObjectKind::kArray) {
                    auto* arr = static_cast<JSObject*>(raw);
                    for (uint32_t i = 0; i < arr->array_length_; ++i) {
                        auto it2 = arr->elements_.find(i);
                        if (it2 == arr->elements_.end()) continue;
                        auto r = process_pair(it2->second);
                        if (!r.is_ok()) return r;
                    }
                } else if (raw) {
                    Value iter_method = static_cast<JSObject*>(raw)->get_property_by_symbol(symbol_table_.well_known_iterator);
                    if (!iter_method.is_object() || iter_method.as_object_raw()->object_kind() != ObjectKind::kFunction) {
                        native_pending_throw_ = make_error_value(NativeErrorType::kTypeError, "WeakMap constructor: not iterable");
                        return EvalResult::err(Error{ErrorKind::Runtime, "__qppjs_pending_throw__"});
                    }
                    auto iter_res = call_function_val(iter_method, iterable, std::span<Value>{});
                    if (!iter_res.is_ok()) return iter_res;
                    Value iter_obj = iter_res.value();
                    Value next_method = iter_obj.is_object() ? static_cast<JSObject*>(iter_obj.as_object_raw())->get_property("next") : Value::undefined();
                    while (true) {
                        auto next_res = call_function_val(next_method, iter_obj, std::span<Value>{});
                        if (!next_res.is_ok()) return next_res;
                        Value result = next_res.value();
                        if (!result.is_object()) break;
                        auto* res_obj = static_cast<JSObject*>(result.as_object_raw());
                        if (to_boolean(res_obj->get_property("done"))) break;
                        auto r = process_pair(res_obj->get_property("value"));
                        if (!r.is_ok()) return r;
                    }
                }
            }
            return EvalResult::ok(Value::object(ObjectPtr(wm)));
        });
        gc_heap_.Register(wm_ctor.get());
        global_env_->define("WeakMap", VarKind::Const);
        global_env_->initialize("WeakMap", Value::object(ObjectPtr(wm_ctor)));
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
                    native_pending_throw_ = make_error_value(NativeErrorType::kTypeError, "WeakSet.prototype.add called on non-WeakSet");
                    return EvalResult::err(Error{ErrorKind::Runtime, "__qppjs_pending_throw__"});
                }
                Value val = args.size() > 0 ? args[0] : Value::undefined();
                if (!val.is_object()) {
                    native_pending_throw_ = make_error_value(NativeErrorType::kTypeError, "Invalid value used in weak set");
                    return EvalResult::err(Error{ErrorKind::Runtime, "__qppjs_pending_throw__"});
                }
                auto* ws = static_cast<JSWeakSet*>(this_val.as_object_raw());
                ws->table_.insert(val.as_object_raw());
                return EvalResult::ok(this_val);
            }))));

        weakset_prototype_->define_builtin_property("has", Value::object(ObjectPtr(make_ws_method(
            [this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
                if (!this_val.is_object() || this_val.as_object_raw()->object_kind() != ObjectKind::kWeakSet) {
                    native_pending_throw_ = make_error_value(NativeErrorType::kTypeError, "WeakSet.prototype.has called on non-WeakSet");
                    return EvalResult::err(Error{ErrorKind::Runtime, "__qppjs_pending_throw__"});
                }
                Value val = args.size() > 0 ? args[0] : Value::undefined();
                if (!val.is_object()) return EvalResult::ok(Value::boolean(false));
                auto* ws = static_cast<JSWeakSet*>(this_val.as_object_raw());
                return EvalResult::ok(Value::boolean(ws->table_.count(val.as_object_raw()) > 0));
            }))));

        weakset_prototype_->define_builtin_property("delete", Value::object(ObjectPtr(make_ws_method(
            [this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
                if (!this_val.is_object() || this_val.as_object_raw()->object_kind() != ObjectKind::kWeakSet) {
                    native_pending_throw_ = make_error_value(NativeErrorType::kTypeError, "WeakSet.prototype.delete called on non-WeakSet");
                    return EvalResult::err(Error{ErrorKind::Runtime, "__qppjs_pending_throw__"});
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
                native_pending_throw_ = make_error_value(NativeErrorType::kTypeError, "Constructor WeakSet requires 'new'");
                return EvalResult::err(Error{ErrorKind::Runtime, "__qppjs_pending_throw__"});
            }
            auto ws = RcPtr<JSWeakSet>::make();
            gc_heap_.Register(ws.get());
            ws->set_proto(weakset_prototype_);
            if (!args.empty() && !args[0].is_undefined() && !args[0].is_null()) {
                Value iterable = args[0];
                RcObject* raw = iterable.is_object() ? iterable.as_object_raw() : nullptr;
                if (raw && raw->object_kind() == ObjectKind::kArray) {
                    auto* arr = static_cast<JSObject*>(raw);
                    for (uint32_t i = 0; i < arr->array_length_; ++i) {
                        auto it2 = arr->elements_.find(i);
                        if (it2 == arr->elements_.end()) continue;
                        Value item = it2->second;
                        if (!item.is_object()) {
                            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError, "Invalid value used in weak set");
                            return EvalResult::err(Error{ErrorKind::Runtime, "__qppjs_pending_throw__"});
                        }
                        ws->table_.insert(item.as_object_raw());
                    }
                } else if (raw) {
                    Value iter_method = static_cast<JSObject*>(raw)->get_property_by_symbol(symbol_table_.well_known_iterator);
                    if (!iter_method.is_object() || iter_method.as_object_raw()->object_kind() != ObjectKind::kFunction) {
                        native_pending_throw_ = make_error_value(NativeErrorType::kTypeError, "WeakSet constructor: not iterable");
                        return EvalResult::err(Error{ErrorKind::Runtime, "__qppjs_pending_throw__"});
                    }
                    auto iter_res = call_function_val(iter_method, iterable, std::span<Value>{});
                    if (!iter_res.is_ok()) return iter_res;
                    Value iter_obj = iter_res.value();
                    Value next_method = iter_obj.is_object() ? static_cast<JSObject*>(iter_obj.as_object_raw())->get_property("next") : Value::undefined();
                    while (true) {
                        auto next_res = call_function_val(next_method, iter_obj, std::span<Value>{});
                        if (!next_res.is_ok()) return next_res;
                        Value result = next_res.value();
                        if (!result.is_object()) break;
                        auto* res_obj = static_cast<JSObject*>(result.as_object_raw());
                        if (to_boolean(res_obj->get_property("done"))) break;
                        Value item = res_obj->get_property("value");
                        if (!item.is_object()) {
                            native_pending_throw_ = make_error_value(NativeErrorType::kTypeError, "Invalid value used in weak set");
                            return EvalResult::err(Error{ErrorKind::Runtime, "__qppjs_pending_throw__"});
                        }
                        ws->table_.insert(item.as_object_raw());
                    }
                }
            }
            return EvalResult::ok(Value::object(ObjectPtr(ws)));
        });
        gc_heap_.Register(ws_ctor.get());
        global_env_->define("WeakSet", VarKind::Const);
        global_env_->initialize("WeakSet", Value::object(ObjectPtr(ws_ctor)));
    }

    // ---- WeakRef ----
    {
        auto wr_proto = RcPtr<JSObject>::make();
        wr_proto->set_proto(object_prototype_);
        gc_heap_.Register(wr_proto.get());

        auto vm_deref_fn = RcPtr<JSFunction>::make();
        vm_deref_fn->set_name(std::string("deref"));
        vm_deref_fn->set_native_fn([this](Value this_val, std::vector<Value> /*args*/, bool) -> EvalResult {
            if (!this_val.is_object() || !this_val.as_object_raw() ||
                this_val.as_object_raw()->object_kind() != ObjectKind::kOrdinary) {
                native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                    "WeakRef.prototype.deref called on non-WeakRef");
                return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
            }
            auto* obj = static_cast<JSObject*>(this_val.as_object_raw());
            return EvalResult::ok(obj->get_property("__weakref_target__"));
        });
        gc_heap_.Register(vm_deref_fn.get());
        wr_proto->define_builtin_property("deref", Value::object(ObjectPtr(vm_deref_fn)));
        wr_proto->set_property_by_symbol(symbol_table_.well_known_to_string_tag, Value::string("WeakRef"));

        auto vm_wr_ctor = RcPtr<JSFunction>::make();
        vm_wr_ctor->set_name(std::string("WeakRef"));
        vm_wr_ctor->set_property("length", Value::number(1.0));
        vm_wr_ctor->set_prototype_obj(wr_proto);
        wr_proto->set_constructor_property(vm_wr_ctor.get());
        vm_wr_ctor->set_native_fn([this, wr_proto](Value /*this_val*/, std::vector<Value> args, bool is_new) -> EvalResult {
            if (!is_new) {
                native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                    "WeakRef constructor requires 'new'");
                return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
            }
            if (args.empty() || (!args[0].is_object() && !args[0].is_symbol())) {
                native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                    "WeakRef target must be an object or symbol");
                return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
            }
            auto ref_obj = RcPtr<JSObject>::make();
            gc_heap_.Register(ref_obj.get());
            ref_obj->set_proto(wr_proto);
            ref_obj->set_property("__weakref_target__", args[0]);
            return EvalResult::ok(Value::object(ObjectPtr(ref_obj)));
        });
        gc_heap_.Register(vm_wr_ctor.get());
        wr_proto->define_builtin_property("constructor", Value::object(ObjectPtr(vm_wr_ctor)));
        global_env_->define_initialized("WeakRef");
        global_env_->set("WeakRef", Value::object(ObjectPtr(vm_wr_ctor)));
    }

    // ---- FinalizationRegistry ----
    {
        auto vm_fr_proto = RcPtr<JSObject>::make();
        vm_fr_proto->set_proto(object_prototype_);
        gc_heap_.Register(vm_fr_proto.get());
        auto vm_fr_register = RcPtr<JSFunction>::make();
        vm_fr_register->set_name(std::string("register"));
        vm_fr_register->set_native_fn([this](Value this_val, std::vector<Value> args, bool) -> EvalResult {
            if (!this_val.is_object() || !this_val.as_object_raw() ||
                this_val.as_object_raw()->object_kind() != ObjectKind::kOrdinary) {
                native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                    "FinalizationRegistry.prototype.register called on wrong type");
                return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
            }
            if (args.empty() || (!args[0].is_object() && !args[0].is_symbol())) {
                native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                    "register: target must be an object or symbol");
                return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
            }
            return EvalResult::ok(Value::undefined());
        });
        gc_heap_.Register(vm_fr_register.get());
        vm_fr_proto->define_builtin_property("register", Value::object(ObjectPtr(vm_fr_register)));
        auto vm_fr_unregister = RcPtr<JSFunction>::make();
        vm_fr_unregister->set_name(std::string("unregister"));
        vm_fr_unregister->set_native_fn([](Value, std::vector<Value>, bool) -> EvalResult {
            return EvalResult::ok(Value::boolean(false));
        });
        gc_heap_.Register(vm_fr_unregister.get());
        vm_fr_proto->define_builtin_property("unregister", Value::object(ObjectPtr(vm_fr_unregister)));
        vm_fr_proto->set_property_by_symbol(symbol_table_.well_known_to_string_tag,
            Value::string("FinalizationRegistry"));
        auto vm_fr_ctor = RcPtr<JSFunction>::make();
        vm_fr_ctor->set_name(std::string("FinalizationRegistry"));
        vm_fr_ctor->set_property("length", Value::number(1.0));
        vm_fr_ctor->set_prototype_obj(vm_fr_proto);
        vm_fr_proto->set_constructor_property(vm_fr_ctor.get());
        vm_fr_ctor->set_native_fn([this, vm_fr_proto](Value, std::vector<Value> args, bool is_new) -> EvalResult {
            if (!is_new) {
                native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                    "FinalizationRegistry constructor requires 'new'");
                return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
            }
            if (args.empty() || !args[0].is_object() ||
                args[0].as_object_raw()->object_kind() != ObjectKind::kFunction) {
                native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                    "FinalizationRegistry: callback must be callable");
                return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
            }
            auto reg_obj = RcPtr<JSObject>::make();
            gc_heap_.Register(reg_obj.get());
            reg_obj->set_proto(vm_fr_proto);
            reg_obj->set_property("__fr_callback__", args[0]);
            return EvalResult::ok(Value::object(ObjectPtr(reg_obj)));
        });
        gc_heap_.Register(vm_fr_ctor.get());
        vm_fr_proto->define_builtin_property("constructor", Value::object(ObjectPtr(vm_fr_ctor)));
        global_env_->define_initialized("FinalizationRegistry");
        global_env_->set("FinalizationRegistry", Value::object(ObjectPtr(vm_fr_ctor)));
    }

    // ---- globalThis ----
    {
        auto gt_obj = RcPtr<JSObject>::make();
        gc_heap_.Register(gt_obj.get());
        gt_obj->set_proto(object_prototype_);
        gt_obj->set_property("globalThis", Value::object(ObjectPtr(gt_obj)));
        global_env_->define_initialized("globalThis");
        global_env_->set("globalThis", Value::object(ObjectPtr(gt_obj)));
    }

    // Register the global environment with GcHeap.
    gc_heap_.Register(global_env_.get());

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

    // ---- JSON object ----
    {
        auto json_obj = RcPtr<JSObject>::make();
        json_obj->set_proto(object_prototype_);
        gc_heap_.Register(json_obj.get());

        auto vm_stringify_fn = RcPtr<JSFunction>::make();
        vm_stringify_fn->set_name(std::string("stringify"));
        vm_stringify_fn->set_native_fn([this](Value, std::vector<Value> args, bool) -> EvalResult {
            if (args.empty()) return EvalResult::ok(Value::undefined());
            std::set<RcObject*> seen;
            std::string result;
            if (!vm_json_stringify_value(args[0], result, seen)) {
                if (native_pending_throw_.has_value())
                    return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
                return EvalResult::ok(Value::undefined());
            }
            return EvalResult::ok(Value::string(result));
        });
        gc_heap_.Register(vm_stringify_fn.get());
        json_obj->set_property("stringify", Value::object(ObjectPtr(vm_stringify_fn)));

        auto vm_parse_fn = RcPtr<JSFunction>::make();
        vm_parse_fn->set_name(std::string("parse"));
        vm_parse_fn->set_native_fn([this](Value, std::vector<Value> args, bool) -> EvalResult {
            if (args.empty() || args[0].is_undefined()) {
                native_pending_throw_ = make_error_value(NativeErrorType::kSyntaxError,
                                                         "JSON.parse: unexpected end of JSON input");
                return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
            }
            std::string text = to_string_val(args[0]);
            size_t pos = 0;
            auto result = vm_json_parse_value(text, pos);
            if (!result.is_ok()) return result;
            while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t' ||
                                         text[pos] == '\n' || text[pos] == '\r')) ++pos;
            if (pos != text.size()) {
                native_pending_throw_ = make_error_value(NativeErrorType::kSyntaxError,
                    "JSON.parse: unexpected non-whitespace character after JSON data");
                return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
            }
            return result;
        });
        gc_heap_.Register(vm_parse_fn.get());
        json_obj->set_property("parse", Value::object(ObjectPtr(vm_parse_fn)));

        global_env_->define("JSON", VarKind::Const);
        global_env_->initialize("JSON", Value::object(ObjectPtr(json_obj)));
    }

    // ---- queueMicrotask ----
    {
        auto vm_qmt_fn = RcPtr<JSFunction>::make();
        vm_qmt_fn->set_name(std::string("queueMicrotask"));
        vm_qmt_fn->set_native_fn([this](Value, std::vector<Value> args, bool) -> EvalResult {
            if (args.empty() || !args[0].is_object() ||
                args[0].as_object_raw()->object_kind() != ObjectKind::kFunction) {
                native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
                                                         "queueMicrotask: argument must be a function");
                return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
            }
            Value fn_val = args[0];
            job_queue_.Enqueue(ReactionJob{fn_val, Value::undefined(), Value::undefined(), true});
            return EvalResult::ok(Value::undefined());
        });
        gc_heap_.Register(vm_qmt_fn.get());
        global_env_->define_initialized("queueMicrotask");
        global_env_->set("queueMicrotask", Value::object(ObjectPtr(vm_qmt_fn)));
    }

    // ---- Function constructor ----
    {
        auto fn_ctor = RcPtr<JSFunction>::make();
        fn_ctor->set_name(std::string("Function"));
        fn_ctor->set_native_fn([this](Value /*this_val*/, std::vector<Value> args, bool) -> EvalResult {
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
                native_pending_throw_ = make_error_value(NativeErrorType::kSyntaxError,
                    "Function constructor: " + parse_result.error().message());
                return EvalResult::err(Error{ErrorKind::Runtime, "__qppjs_pending_throw__"});
            }
            const auto& body = parse_result.value().body;
            if (body.empty()) {
                native_pending_throw_ = make_error_value(NativeErrorType::kSyntaxError,
                    "Function constructor: empty body");
                return EvalResult::err(Error{ErrorKind::Runtime, "__qppjs_pending_throw__"});
            }
            // Compile the program: the function declaration __anon__ will be hoisted
            Compiler fn_compiler;
            auto program_bc = fn_compiler.compile(parse_result.value());
            // Pre-run the program to hoist __anon__ into a temp env
            auto temp_env = RcPtr<Environment>::make(global_env_);
            for (uint16_t idx : program_bc->var_decls) {
                temp_env->define_initialized(program_bc->names[idx]);
            }
            for (uint16_t idx : program_bc->function_decls) {
                temp_env->define_function(program_bc->names[idx]);
            }
            CallFrame fn_frame;
            fn_frame.bytecode = program_bc.get();
            fn_frame.pc = 0;
            fn_frame.env = temp_env;
            fn_frame.this_val = Value::undefined();
            size_t fn_exit_depth = call_stack_.size();
            call_stack_.push_back(std::move(fn_frame));
            auto fn_run_result = run(fn_exit_depth);
            if (!fn_run_result.is_ok()) return fn_run_result;
            // Read the function from temp_env
            auto* binding = temp_env->lookup("__anon__");
            if (!binding || !binding->initialized) {
                native_pending_throw_ = make_error_value(NativeErrorType::kSyntaxError,
                    "Function constructor: could not create function");
                return EvalResult::err(Error{ErrorKind::Runtime, "__qppjs_pending_throw__"});
            }
            return EvalResult::ok(binding->cell->value);
        });
        gc_heap_.Register(fn_ctor.get());
        // Function.prototype = function_prototype_
        if (function_prototype_) {
            fn_ctor->set_property("prototype", Value::object(ObjectPtr(function_prototype_)));
            function_prototype_->define_builtin_property("constructor", Value::object(ObjectPtr(fn_ctor)));
        }
        global_env_->define("Function", VarKind::Const);
        global_env_->initialize("Function", Value::object(ObjectPtr(fn_ctor)));
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
                native_pending_throw_ = make_error_value(NativeErrorType::kSyntaxError,
                    parse_result.error().message());
                return EvalResult::err(Error{ErrorKind::Runtime, "__qppjs_pending_throw__"});
            }
            // Compile and execute the eval code using global_env_
            Compiler eval_compiler;
            auto bytecode = eval_compiler.compile(parse_result.value());
            // Pre-define top-level var_decls and function_decls in global_env_
            for (uint16_t idx : bytecode->var_decls) {
                if (!global_env_->lookup(bytecode->names[idx])) {
                    global_env_->define_initialized(bytecode->names[idx]);
                }
            }
            for (uint16_t idx : bytecode->function_decls) {
                if (!global_env_->lookup(bytecode->names[idx])) {
                    global_env_->define_function(bytecode->names[idx]);
                }
            }
            // Keep bytecode alive during run
            CallFrame eval_frame;
            eval_frame.bytecode = bytecode.get();
            eval_frame.pc = 0;
            eval_frame.env = global_env_;
            eval_frame.this_val = Value::undefined();
            size_t exit_depth = call_stack_.size();
            call_stack_.push_back(std::move(eval_frame));
            return run(exit_depth);
        });
        gc_heap_.Register(eval_fn.get());
        global_env_->define("eval", VarKind::Const);
        global_env_->initialize("eval", Value::object(ObjectPtr(eval_fn)));
    }
}

// ---- VM Promise helpers ----

// ---- JSON helpers ----

static std::string vm_json_escape_string(std::string_view sv) {
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

bool VM::vm_json_stringify_value(const Value& val, std::string& out, std::set<RcObject*>& seen) {
    if (val.is_null()) { out += "null"; return true; }
    if (val.is_bool()) { out += val.as_bool() ? "true" : "false"; return true; }
    if (val.is_number()) {
        double d = val.as_number();
        if (std::isnan(d) || std::isinf(d)) { out += "null"; return true; }
        std::ostringstream oss;
        if (d == std::trunc(d) && std::abs(d) < 1e15) {
            oss << static_cast<long long>(d);
        } else {
            oss << d;
        }
        out += oss.str();
        return true;
    }
    if (val.is_string()) { out += vm_json_escape_string(val.sv()); return true; }
    if (val.is_undefined() || val.is_symbol()) return false;
    if (!val.is_object()) return false;

    RcObject* raw = val.as_object_raw();
    if (raw->object_kind() == ObjectKind::kFunction) return false;

    if (seen.count(raw)) {
        native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
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
                if (!vm_json_stringify_value(it->second, out, seen)) {
                    if (native_pending_throw_.has_value()) { seen.erase(raw); return false; }
                    out += "null";
                }
            }
        }
        out += ']';
    } else {
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
            if (!vm_json_stringify_value(prop, prop_str, seen)) {
                if (native_pending_throw_.has_value()) { seen.erase(raw); return false; }
                continue;
            }
            if (!first) out += ',';
            first = false;
            out += vm_json_escape_string(key);
            out += ':';
            out += prop_str;
        }
        out += '}';
    }
    seen.erase(raw);
    return true;
}

EvalResult VM::vm_json_parse_value(const std::string& text, size_t& pos) {
    while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t' ||
                                  text[pos] == '\n' || text[pos] == '\r')) ++pos;
    if (pos >= text.size()) {
        native_pending_throw_ = make_error_value(NativeErrorType::kSyntaxError,
                                                  "JSON.parse: unexpected end of JSON input");
        return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
    }

    char c = text[pos];

    if (c == 'n') {
        if (text.substr(pos, 4) == "null") { pos += 4; return EvalResult::ok(Value::null()); }
        native_pending_throw_ = make_error_value(NativeErrorType::kSyntaxError, "JSON.parse: unexpected token");
        return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
    }
    if (c == 't') {
        if (text.substr(pos, 4) == "true") { pos += 4; return EvalResult::ok(Value::boolean(true)); }
        native_pending_throw_ = make_error_value(NativeErrorType::kSyntaxError, "JSON.parse: unexpected token");
        return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
    }
    if (c == 'f') {
        if (text.substr(pos, 5) == "false") { pos += 5; return EvalResult::ok(Value::boolean(false)); }
        native_pending_throw_ = make_error_value(NativeErrorType::kSyntaxError, "JSON.parse: unexpected token");
        return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
    }
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
                        native_pending_throw_ = make_error_value(NativeErrorType::kSyntaxError,
                                                                  "JSON.parse: invalid unicode escape");
                        return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
                    }
                    unsigned int code = 0;
                    for (int j = 0; j < 4; ++j) {
                        char h = text[pos + j];
                        int digit = 0;
                        if (h >= '0' && h <= '9') digit = h - '0';
                        else if (h >= 'a' && h <= 'f') digit = h - 'a' + 10;
                        else if (h >= 'A' && h <= 'F') digit = h - 'A' + 10;
                        else {
                            native_pending_throw_ = make_error_value(NativeErrorType::kSyntaxError,
                                                                      "JSON.parse: invalid unicode escape");
                            return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
                        }
                        code = code * 16 + static_cast<unsigned int>(digit);
                    }
                    pos += 4;
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
                    native_pending_throw_ = make_error_value(NativeErrorType::kSyntaxError,
                                                              "JSON.parse: invalid escape sequence");
                    return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
                }
            } else {
                if (static_cast<unsigned char>(text[pos]) < 0x20) {
                    native_pending_throw_ = make_error_value(NativeErrorType::kSyntaxError,
                                                              "JSON.parse: invalid control character in string");
                    return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
                }
                str += text[pos++];
            }
        }
        if (pos >= text.size()) {
            native_pending_throw_ = make_error_value(NativeErrorType::kSyntaxError,
                                                      "JSON.parse: unterminated string");
            return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
        }
        ++pos;
        return EvalResult::ok(Value::string(str));
    }
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
    if (c == '[') {
        ++pos;
        auto arr = RcPtr<JSObject>::make(ObjectKind::kArray);
        gc_heap_.Register(arr.get());
        arr->set_proto(array_prototype_);
        while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t' ||
                                      text[pos] == '\n' || text[pos] == '\r')) ++pos;
        if (pos < text.size() && text[pos] == ']') { ++pos; return EvalResult::ok(Value::object(ObjectPtr(arr))); }
        uint32_t idx = 0;
        while (true) {
            auto elem = vm_json_parse_value(text, pos);
            if (!elem.is_ok()) return elem;
            arr->elements_[idx] = elem.value();
            ++idx;
            arr->array_length_ = idx;
            while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t' ||
                                          text[pos] == '\n' || text[pos] == '\r')) ++pos;
            if (pos >= text.size()) break;
            if (text[pos] == ']') { ++pos; break; }
            if (text[pos] != ',') {
                native_pending_throw_ = make_error_value(NativeErrorType::kSyntaxError,
                                                          "JSON.parse: expected ',' or ']'");
                return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
            }
            ++pos;
        }
        return EvalResult::ok(Value::object(ObjectPtr(arr)));
    }
    if (c == '{') {
        ++pos;
        auto obj = RcPtr<JSObject>::make();
        gc_heap_.Register(obj.get());
        obj->set_proto(object_prototype_);
        while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t' ||
                                      text[pos] == '\n' || text[pos] == '\r')) ++pos;
        if (pos < text.size() && text[pos] == '}') { ++pos; return EvalResult::ok(Value::object(ObjectPtr(obj))); }
        while (true) {
            while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t' ||
                                          text[pos] == '\n' || text[pos] == '\r')) ++pos;
            if (pos >= text.size() || text[pos] != '"') {
                native_pending_throw_ = make_error_value(NativeErrorType::kSyntaxError,
                                                          "JSON.parse: expected string key");
                return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
            }
            auto key_result = vm_json_parse_value(text, pos);
            if (!key_result.is_ok()) return key_result;
            std::string key = key_result.value().is_string() ? std::string(key_result.value().sv()) : "";
            while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t' ||
                                          text[pos] == '\n' || text[pos] == '\r')) ++pos;
            if (pos >= text.size() || text[pos] != ':') {
                native_pending_throw_ = make_error_value(NativeErrorType::kSyntaxError,
                                                          "JSON.parse: expected ':'");
                return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
            }
            ++pos;
            auto val_result = vm_json_parse_value(text, pos);
            if (!val_result.is_ok()) return val_result;
            obj->set_property(key, val_result.value());
            while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t' ||
                                          text[pos] == '\n' || text[pos] == '\r')) ++pos;
            if (pos >= text.size()) break;
            if (text[pos] == '}') { ++pos; break; }
            if (text[pos] != ',') {
                native_pending_throw_ = make_error_value(NativeErrorType::kSyntaxError,
                                                          "JSON.parse: expected ',' or '}'");
                return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
            }
            ++pos;
        }
        return EvalResult::ok(Value::object(ObjectPtr(obj)));
    }

    native_pending_throw_ = make_error_value(NativeErrorType::kSyntaxError,
                                              "JSON.parse: unexpected token");
    return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
}

// ---- RegExp runtime ----

EvalResult VM::vm_make_regexp(const std::string& pattern, const std::string& flags) {
    bool seen[128] = {};
    for (char c : flags) {
        if (c != 'g' && c != 'i' && c != 'm' && c != 's' && c != 'u' && c != 'y') {
            native_pending_throw_ = make_error_value(NativeErrorType::kSyntaxError,
                std::string("Invalid regular expression flags: ") + c);
            return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
        }
        if (seen[static_cast<unsigned char>(c)]) {
            native_pending_throw_ = make_error_value(NativeErrorType::kSyntaxError,
                std::string("Duplicate regular expression flag: ") + c);
            return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
        }
        seen[static_cast<unsigned char>(c)] = true;
    }

    auto rx = RcPtr<JSRegExp>::make(pattern, flags);
    if (!rx->is_valid_) {
        native_pending_throw_ = make_error_value(NativeErrorType::kSyntaxError,
            "Invalid regular expression: " + pattern);
        return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
    }
    gc_heap_.Register(rx.get());
    if (regexp_prototype_) rx->set_proto(regexp_prototype_);
    return EvalResult::ok(Value::object(ObjectPtr(rx)));
}

EvalResult VM::vm_regexp_exec(JSRegExp* rx, const std::string& input) {
    if (!rx->is_valid_) {
        native_pending_throw_ = make_error_value(NativeErrorType::kSyntaxError,
            "Invalid regular expression");
        return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
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

    auto result = RcPtr<JSObject>::make(ObjectKind::kArray);
    gc_heap_.Register(result.get());
    if (array_prototype_) result->set_proto(array_prototype_);

    for (size_t i = 0; i < sm.size(); ++i) {
        if (i > 0 && !sm[i].matched) {
            // Unmatched capture group → undefined (hole)
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
// Async generator resolve helper
// ============================================================

void VM::vm_ag_resolve(EvalResult sync_result, RcPtr<JSPromise> outer_promise) {
    if (sync_result.is_ok()) {
        // Successful yield or completion: result is {value, done} object
        outer_promise->Fulfill(sync_result.value(), job_queue_);
    } else {
        // Error from generator (throw or pending_throw)
        Value err_val;
        const std::string& msg = sync_result.error().message();
        if ((msg == "__qppjs_pending_throw__" || msg.rfind("__qppjs_pending_throw__", 0) == 0) &&
            native_pending_throw_.has_value()) {
            err_val = std::move(*native_pending_throw_);
            native_pending_throw_ = std::nullopt;
        } else {
            err_val = Value::string(msg);
        }
        outer_promise->Reject(std::move(err_val), job_queue_);
    }
    vm_drain_job_queue();
}

// ============================================================
// Generator helpers
// ============================================================

EvalResult VM::vm_generator_resume(RcPtr<JSGeneratorObject> gen) {
    if (!gen->suspended_frame_) {
        native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
            "internal: generator has no suspended frame");
        return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
    }
    gen->state_ = GeneratorState::kExecuting;
    call_stack_.push_back(std::move(*gen->suspended_frame_));
    gen->suspended_frame_.reset();
    call_depth_++;
    // Re-link owning_generator pointer
    call_stack_.back().owning_generator = gen.get();

    size_t exit_depth = call_stack_.size() - 1;
    EvalResult run_result = run(exit_depth);

    if (vm_generator_yielded_) {
        // run() returned from kYield via suspend_exit; run_result is the {value, done: false} obj
        vm_generator_yielded_ = false;
        gen->state_ = GeneratorState::kSuspendedYield;
        return run_result;  // already wrapped as {value, done: false}
    }

    if (vm_async_suspended_) {
        // kAwait fired during an async generator body: the frame is in vm_suspended_frame_.
        // Save it back into the generator so we can resume later.
        vm_async_suspended_ = false;
        gen->state_ = GeneratorState::kSuspendedYield;
        gen->suspended_frame_ = std::make_unique<CallFrame>(std::move(*vm_suspended_frame_));
        gen->suspended_frame_->owning_generator = gen.get();
        vm_suspended_frame_.reset();
        // Return a sentinel value indicating await suspension — the caller (vm_ag_resolve) handles this.
        // Use a special negative-number sentinel to distinguish from normal yield.
        return EvalResult::ok(Value::undefined());  // special: outer promise stays pending after setup
    }

    // Completion or error
    gen->state_ = GeneratorState::kCompleted;

    if (run_result.is_ok()) {
        // Natural completion or explicit return: wrap in {value: ret, done: true}
        auto result_obj = RcPtr<JSObject>::make();
        gc_heap_.Register(result_obj.get());
        result_obj->set_proto(object_prototype_);
        result_obj->set_property("value", run_result.value());
        result_obj->set_property("done", Value::boolean(true));
        return EvalResult::ok(Value::object(ObjectPtr(result_obj)));
    }

    // Error: propagate
    return run_result;
}

EvalResult VM::vm_generator_next(RcPtr<JSGeneratorObject> gen, Value resume_val) {
    if (gen->state_ == GeneratorState::kCompleted) {
        auto result_obj = RcPtr<JSObject>::make();
        gc_heap_.Register(result_obj.get());
        result_obj->set_proto(object_prototype_);
        result_obj->set_property("value", Value::undefined());
        result_obj->set_property("done", Value::boolean(true));
        return EvalResult::ok(Value::object(ObjectPtr(result_obj)));
    }
    if (gen->state_ == GeneratorState::kExecuting) {
        native_pending_throw_ = make_error_value(NativeErrorType::kTypeError,
            "Generator is already running");
        return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
    }
    // Push resume value onto the suspended frame's stack (consumed by kYield resume path)
    if (gen->state_ == GeneratorState::kSuspendedYield && gen->suspended_frame_) {
        gen->suspended_frame_->stack.push_back(std::move(resume_val));
    }
    return vm_generator_resume(gen);
}

EvalResult VM::vm_generator_return(RcPtr<JSGeneratorObject> gen, Value return_val) {
    gen->state_ = GeneratorState::kCompleted;
    gen->suspended_frame_.reset();
    auto result_obj = RcPtr<JSObject>::make();
    gc_heap_.Register(result_obj.get());
    result_obj->set_proto(object_prototype_);
    result_obj->set_property("value", std::move(return_val));
    result_obj->set_property("done", Value::boolean(true));
    return EvalResult::ok(Value::object(ObjectPtr(result_obj)));
}

EvalResult VM::vm_generator_throw(RcPtr<JSGeneratorObject> gen, Value throw_val) {
    if (gen->state_ == GeneratorState::kCompleted ||
        gen->state_ == GeneratorState::kSuspendedStart) {
        gen->state_ = GeneratorState::kCompleted;
        native_pending_throw_ = std::move(throw_val);
        return EvalResult::err(Error(ErrorKind::Runtime, "__qppjs_pending_throw__"));
    }
    // kSuspendedYield: inject throw into the suspended frame and resume.
    // The exception dispatch at the top of run()'s main loop processes pending_throw
    // through the frame's handler_stack, allowing the generator's internal try-catch to fire.
    if (gen->suspended_frame_) {
        gen->suspended_frame_->pending_throw = std::move(throw_val);
    }
    return vm_generator_resume(gen);
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
        // Include call stack frames
        for (auto& cf : call_stack_) {
            add_obj(cf.env.get());
            add_val(cf.this_val);
            add_val(cf.new_instance);
            add_val(cf.new_target_val);
            if (cf.current_fn_holder) add_obj(cf.current_fn_holder.get());
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

    // 守卫 1：箭头函数不可 new
    if (fn->is_arrow() && is_new) {
        return EvalResult::err(Error(ErrorKind::Runtime, "TypeError: arrow function is not a constructor"));
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

    // Bind rest parameter
    if (fn->rest_param().has_value()) {
        const std::string& rest_name = fn->rest_param().value();
        auto rest_arr = RcPtr<JSObject>::make(ObjectKind::kArray);
        gc_heap_.Register(rest_arr.get());
        rest_arr->set_proto(array_prototype_);
        size_t rest_start = params.size();
        for (size_t i = rest_start; i < args.size(); ++i) {
            rest_arr->elements_[static_cast<uint32_t>(i - rest_start)] = args[i];
        }
        rest_arr->array_length_ = static_cast<uint32_t>(
            args.size() > rest_start ? args.size() - rest_start : 0);
        fn_env->define(rest_name, VarKind::Var);
        fn_env->initialize(rest_name, Value::object(ObjectPtr(rest_arr)));
    }

    // 守卫 2：箭头函数不创建 arguments（词法穿透外层）
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

    // Pre-define function declaration name bindings only（var_decls 由 prologue 之后的 kDefVar 字节码处理，
    // 确保 body var 在默认值求值期间不可见，符合规范要求）
    for (uint16_t idx : bc->function_decls) {
        fn_env->define_function(bc->names[idx]);
    }

    // 守卫 3：箭头函数使用词法 this
    Value actual_this = fn->is_arrow() ? fn->lexical_this() : std::move(this_val);

    CallFrame frame;
    frame.bytecode = bc.get();
    frame.pc = 0;
    frame.env = fn_env;
    frame.this_val = std::move(actual_this);
    frame.is_new_call = is_new;
    if (is_new) frame.new_instance = std::move(new_instance);
    frame.current_module = fn->defining_module();
    frame.current_fn_holder = fn;  // keep function alive via RcPtr
    frame.current_fn = fn.get();   // raw pointer for fast access

    // Generator function: create generator object without executing the body
    if (bc->is_generator) {
        if (is_new) {
            call_depth_--;
            return EvalResult::err(Error(ErrorKind::Runtime,
                "TypeError: generator function is not a constructor"));
        }
        auto gen_obj = RcPtr<JSGeneratorObject>::make();
        gc_heap_.Register(gen_obj.get());
        gen_obj->set_proto(generator_prototype_);
        gen_obj->state_ = GeneratorState::kSuspendedStart;
        gen_obj->suspended_frame_ = std::make_unique<CallFrame>(std::move(frame));
        gen_obj->suspended_frame_->owning_generator = gen_obj.get();
        call_depth_--;

        Value gen_val = Value::object(ObjectPtr(gen_obj));

        // Set up .next() native method
        {
            auto next_fn = RcPtr<JSFunction>::make();
            next_fn->set_name(std::string("next"));
            next_fn->set_property("length", Value::number(1));
            next_fn->set_native_fn([this, gen_obj](Value /*this_val*/, std::vector<Value> args, bool) mutable -> EvalResult {
                return vm_generator_next(gen_obj, args.empty() ? Value::undefined() : args[0]);
            });
            gc_heap_.Register(next_fn.get());
            gen_obj->set_property("next", Value::object(ObjectPtr(next_fn)));
        }
        // Set up .return() native method
        {
            auto ret_fn = RcPtr<JSFunction>::make();
            ret_fn->set_name(std::string("return"));
            ret_fn->set_property("length", Value::number(1));
            ret_fn->set_native_fn([this, gen_obj](Value /*this_val*/, std::vector<Value> args, bool) mutable -> EvalResult {
                return vm_generator_return(gen_obj, args.empty() ? Value::undefined() : args[0]);
            });
            gc_heap_.Register(ret_fn.get());
            gen_obj->set_property("return", Value::object(ObjectPtr(ret_fn)));
        }
        // Set up .throw() native method
        {
            auto throw_fn = RcPtr<JSFunction>::make();
            throw_fn->set_name(std::string("throw"));
            throw_fn->set_property("length", Value::number(1));
            throw_fn->set_native_fn([this, gen_obj](Value /*this_val*/, std::vector<Value> args, bool) mutable -> EvalResult {
                return vm_generator_throw(gen_obj, args.empty() ? Value::undefined() : args[0]);
            });
            gc_heap_.Register(throw_fn.get());
            gen_obj->set_property("throw", Value::object(ObjectPtr(throw_fn)));
        }

        return EvalResult::ok(gen_val);
    }

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

        // Base class constructor: run instance field initializer before executing body
        if (!frame.fields_initialized && frame.current_fn &&
            frame.current_fn->is_class_ctor() && !frame.current_fn->is_derived_ctor() &&
            frame.current_fn->field_initializer()) {
            frame.fields_initialized = true;
            Value fi_this = frame.this_val;  // copy before potential realloc
            auto fi_bc = frame.current_fn->field_initializer();
            RcPtr<Environment> fi_outer = frame.current_fn->closure_env()
                ? frame.current_fn->closure_env() : global_env_;
            auto fi_fn = RcPtr<JSFunction>::make();
            gc_heap_.Register(fi_fn.get());
            fi_fn->set_bytecode(fi_bc);
            fi_fn->set_closure_env(fi_outer);
            size_t fi_depth = call_stack_.size();
            auto fi_push = push_call_frame(fi_fn, fi_this, std::span<Value>{});
            if (!fi_push.is_ok()) {
                // push_call_frame failed; set pending_throw on ctor frame and continue
                call_stack_.back().pending_throw = make_error_value(NativeErrorType::kTypeError,
                    strip_error_prefix(fi_push.error().message()));
                continue;
            }
            EvalResult fi_res = run(fi_depth);
            if (!fi_res.is_ok() && call_stack_.size() > exit_depth) {
                call_stack_.back().pending_throw = make_error_value(NativeErrorType::kTypeError,
                    strip_error_prefix(fi_res.error().message()));
            }
            continue;  // re-fetch frame at top of while loop
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
            // Derived constructor: accessing 'this' before super() throws ReferenceError
            if (frame.current_fn && frame.current_fn->is_derived_ctor() &&
                !frame.derived_this_initialized) {
                frame.pending_throw = make_error_value(NativeErrorType::kReferenceError,
                    "Must call super constructor in derived class before accessing 'this'");
                continue;
            }
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

        case Opcode::kSwap: {
            // Swap TOS and TOS-1.
            if (stack.size() >= 2) {
                std::swap(stack[stack.size() - 1], stack[stack.size() - 2]);
            }
            break;
        }

        case Opcode::kArrayAppend: {
            // Stack layout (bottom to top): [..., arr(base), arr(dup), val]
            // Emitted as: kDup, compile_val, kArrayAppend
            // Pops val + arr(dup); appends val to arr; arr(base) remains on stack.
            Value val = std::move(stack.back());
            stack.pop_back();
            Value arr_dup = std::move(stack.back());  // pop the dup'd reference
            stack.pop_back();
            if (arr_dup.is_object()) {
                RcObject* raw = arr_dup.as_object_raw();
                if (raw && raw->object_kind() == ObjectKind::kArray) {
                    auto* arr = static_cast<JSObject*>(raw);
                    arr->elements_[arr->array_length_++] = std::move(val);
                }
            }
            break;
        }

        case Opcode::kSpreadAppend: {
            // Stack layout (bottom to top): [..., arr(base), arr(dup), iterable]
            // Emitted as: kDup, compile_iterable, kSpreadAppend
            // Pops iterable + arr(dup); spreads all elements into arr; arr(base) remains on stack.
            Value iterable = std::move(stack.back());
            stack.pop_back();
            Value arr_dup = std::move(stack.back());  // pop the dup'd reference
            stack.pop_back();
            if (!arr_dup.is_object() || !arr_dup.as_object_raw() ||
                arr_dup.as_object_raw()->object_kind() != ObjectKind::kArray) {
                frame.pending_throw = make_error_value(NativeErrorType::kTypeError,
                    "SpreadAppend: expected array");
                continue;
            }
            auto* target_arr = static_cast<JSObject*>(arr_dup.as_object_raw());

            // Fast path: array
            if (iterable.is_object() && iterable.as_object_raw() &&
                iterable.as_object_raw()->object_kind() == ObjectKind::kArray) {
                auto* src = static_cast<JSObject*>(iterable.as_object_raw());
                uint32_t len = src->array_length_;
                for (uint32_t i = 0; i < len; ++i) {
                    auto it = src->elements_.find(i);
                    target_arr->elements_[target_arr->array_length_++] =
                        (it != src->elements_.end()) ? it->second : Value::undefined();
                }
                break;
            }

            // Fast path: string
            if (iterable.is_string()) {
                std::string_view sv = iterable.sv();
                size_t pos = 0;
                while (pos < sv.size()) {
                    unsigned char c0 = static_cast<unsigned char>(sv[pos]);
                    size_t cp_bytes = (c0 < 0x80) ? 1 : (c0 < 0xE0) ? 2 : (c0 < 0xF0) ? 3 : 4;
                    if (pos + cp_bytes > sv.size()) cp_bytes = sv.size() - pos;
                    target_arr->elements_[target_arr->array_length_++] =
                        Value::string(std::string(sv.data() + pos, cp_bytes));
                    pos += cp_bytes;
                }
                break;
            }

            // Fast path: ArrayIterator (native)
            if (iterable.is_object() && iterable.as_object_raw() &&
                iterable.as_object_raw()->object_kind() == ObjectKind::kArrayIterator) {
                auto* arr_it = static_cast<ArrayIterator*>(iterable.as_object_raw());
                if (!arr_it->done_ && arr_it->array_ref_.is_object() &&
                    arr_it->array_ref_.as_object_raw() &&
                    arr_it->array_ref_.as_object_raw()->object_kind() == ObjectKind::kArray) {
                    auto* src = static_cast<JSObject*>(arr_it->array_ref_.as_object_raw());
                    for (; arr_it->index_ < src->array_length_; ++arr_it->index_) {
                        auto it = src->elements_.find(arr_it->index_);
                        target_arr->elements_[target_arr->array_length_++] =
                            (it != src->elements_.end()) ? it->second : Value::undefined();
                    }
                    arr_it->done_ = true;
                }
                break;
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
                        target_arr->elements_[target_arr->array_length_++] =
                            Value::string(std::string(sv.data() + pos, cp_bytes));
                        pos += cp_bytes;
                    }
                    str_it->byte_pos_ = static_cast<uint32_t>(sv.size());
                    str_it->done_ = true;
                }
                break;
            }

            // Fast path: ForOfIterator (native)
            if (iterable.is_object() && iterable.as_object_raw() &&
                iterable.as_object_raw()->object_kind() == ObjectKind::kForOfIterator) {
                auto* for_it = static_cast<ForOfIterator*>(iterable.as_object_raw());
                bool spread_error = false;
                while (!for_it->done_) {
                    auto next_r = call_function_val(for_it->next_method_, for_it->iterator_, {});
                    if (!next_r.is_ok()) {
                        const std::string& msg = next_r.error().message();
                        if (msg == "__qppjs_pending_throw__" && native_pending_throw_.has_value()) {
                            frame.pending_throw = std::move(*native_pending_throw_);
                            native_pending_throw_ = std::nullopt;
                        } else {
                            frame.pending_throw = make_error_value(NativeErrorType::kTypeError,
                                strip_error_prefix(msg));
                        }
                        spread_error = true;
                        break;
                    }
                    Value result = next_r.value();
                    if (!result.is_object()) {
                        frame.pending_throw = make_error_value(NativeErrorType::kTypeError,
                            "iterator result must be an object");
                        spread_error = true;
                        break;
                    }
                    Value done_val = Value::undefined();
                    Value value = Value::undefined();
                    RcObject* raw_res = result.as_object_raw();
                    ObjectKind rk = raw_res->object_kind();
                    if (rk == ObjectKind::kOrdinary || rk == ObjectKind::kArray) {
                        auto* res_obj = static_cast<JSObject*>(raw_res);
                        done_val = res_obj->get_property("done");
                        value = res_obj->get_property("value");
                    }
                    if (to_boolean(done_val)) {
                        for_it->done_ = true;
                        break;
                    }
                    target_arr->elements_[target_arr->array_length_++] = std::move(value);
                }
                if (spread_error) continue;
                break;
            }

            // Generic path: Symbol.iterator
            if (!iterable.is_object()) {
                frame.pending_throw = make_error_value(NativeErrorType::kTypeError, "value is not iterable");
                continue;
            }
            RcObject* raw_it = iterable.as_object_raw();
            Value iter_factory = Value::undefined();
            if (raw_it->object_kind() == ObjectKind::kOrdinary ||
                raw_it->object_kind() == ObjectKind::kRegExp ||
                raw_it->object_kind() == ObjectKind::kStringObject ||
                raw_it->object_kind() == ObjectKind::kBooleanObject ||
                raw_it->object_kind() == ObjectKind::kGenerator ||
                raw_it->object_kind() == ObjectKind::kMap ||
                raw_it->object_kind() == ObjectKind::kSet) {
                iter_factory = static_cast<JSObject*>(raw_it)->get_property_by_symbol(
                    symbol_table_.well_known_iterator);
            }
            if (iter_factory.is_undefined() || iter_factory.is_null()) {
                frame.pending_throw = make_error_value(NativeErrorType::kTypeError, "value is not iterable");
                continue;
            }
            auto iter_res = call_function_val(iter_factory, iterable, {});
            if (!iter_res.is_ok()) {
                const std::string& msg = iter_res.error().message();
                if (msg == "__qppjs_pending_throw__" && native_pending_throw_.has_value()) {
                    frame.pending_throw = std::move(*native_pending_throw_);
                    native_pending_throw_ = std::nullopt;
                } else {
                    frame.pending_throw = make_error_value(NativeErrorType::kTypeError,
                        strip_error_prefix(msg));
                }
                continue;
            }
            Value iterator_val = iter_res.value();
            if (!iterator_val.is_object()) {
                frame.pending_throw = make_error_value(NativeErrorType::kTypeError,
                    "iterator must be an object");
                continue;
            }
            Value next_method = Value::undefined();
            {
                RcObject* raw_iter2 = iterator_val.as_object_raw();
                ObjectKind ik = raw_iter2->object_kind();
                if (ik == ObjectKind::kOrdinary || ik == ObjectKind::kArray ||
                    ik == ObjectKind::kRegExp || ik == ObjectKind::kStringObject ||
                    ik == ObjectKind::kBooleanObject || ik == ObjectKind::kGenerator ||
                    ik == ObjectKind::kMap || ik == ObjectKind::kSet) {
                    next_method = static_cast<JSObject*>(raw_iter2)->get_property("next");
                }
            }
            bool spread_error = false;
            while (true) {
                auto next_r = call_function_val(next_method, iterator_val, {});
                if (!next_r.is_ok()) {
                    const std::string& msg = next_r.error().message();
                    if (msg == "__qppjs_pending_throw__" && native_pending_throw_.has_value()) {
                        frame.pending_throw = std::move(*native_pending_throw_);
                        native_pending_throw_ = std::nullopt;
                    } else {
                        frame.pending_throw = make_error_value(NativeErrorType::kTypeError,
                            strip_error_prefix(msg));
                    }
                    spread_error = true;
                    break;
                }
                Value result = next_r.value();
                if (!result.is_object()) {
                    frame.pending_throw = make_error_value(NativeErrorType::kTypeError,
                        "iterator result must be an object");
                    spread_error = true;
                    break;
                }
                Value done_val = Value::undefined();
                Value value = Value::undefined();
                RcObject* raw_res = result.as_object_raw();
                ObjectKind rk = raw_res->object_kind();
                if (rk == ObjectKind::kOrdinary || rk == ObjectKind::kArray) {
                    auto* res_obj = static_cast<JSObject*>(raw_res);
                    done_val = res_obj->get_property("done");
                    value = res_obj->get_property("value");
                }
                if (to_boolean(done_val)) break;
                target_arr->elements_[target_arr->array_length_++] = std::move(value);
            }
            if (spread_error) continue;
            break;
        }

        case Opcode::kApplyArgs: {
            // Stack: [..., func, this_val, args_array]
            // operand: 0=normal call, 1=new call
            uint8_t is_new_u8 = read_u8(bc, pc);
            bool is_new_apply = (is_new_u8 != 0);

            Value args_array = std::move(stack.back());
            stack.pop_back();
            Value this_arg = std::move(stack.back());
            stack.pop_back();
            Value func_val = std::move(stack.back());
            stack.pop_back();

            // Collect args from the array
            std::vector<Value> apply_args;
            if (args_array.is_object() && args_array.as_object_raw() &&
                args_array.as_object_raw()->object_kind() == ObjectKind::kArray) {
                auto* arr = static_cast<JSObject*>(args_array.as_object_raw());
                apply_args.resize(arr->array_length_);
                for (uint32_t i = 0; i < arr->array_length_; ++i) {
                    auto it = arr->elements_.find(i);
                    apply_args[i] = (it != arr->elements_.end()) ? it->second : Value::undefined();
                }
            }

            if (!func_val.is_object() || !func_val.as_object_raw() ||
                func_val.as_object_raw()->object_kind() != ObjectKind::kFunction) {
                frame.pending_throw = make_error_value(NativeErrorType::kTypeError,
                    "value is not a function");
                continue;
            }
            auto fn = RcPtr<JSFunction>(static_cast<JSFunction*>(func_val.as_object_raw()));

            if (is_new_apply) {
                // new call
                if (fn->is_arrow()) {
                    frame.pending_throw = make_error_value(NativeErrorType::kTypeError,
                        "arrow function is not a constructor");
                    continue;
                }
                if (fn->is_native()) {
                    auto res = fn->native_fn()(Value::undefined(),
                        std::vector<Value>(apply_args.begin(), apply_args.end()), true);
                    if (!res.is_ok()) {
                        frame.pending_throw = make_error_value(NativeErrorType::kTypeError,
                            strip_error_prefix(res.error().message()));
                        continue;
                    }
                    stack.push_back(res.value());
                    break;
                }
                auto instance = RcPtr<JSObject>::make();
                gc_heap_.Register(instance.get());
                const auto& proto_obj = fn->prototype_obj();
                if (proto_obj) {
                    instance->set_proto(proto_obj);
                } else {
                    instance->set_proto(object_prototype_);
                }
                Value instance_val = Value::object(ObjectPtr(instance));
                Value instance_copy = instance_val;
                auto push_res = push_call_frame(fn, instance_val,
                    std::span<Value>(apply_args.data(), apply_args.size()),
                    true, std::move(instance_copy));
                if (!push_res.is_ok()) {
                    frame.pending_throw = make_error_value(NativeErrorType::kTypeError,
                        strip_error_prefix(push_res.error().message()));
                    continue;
                }
            } else {
                // normal call
                if (fn->is_native()) {
                    auto res = fn->native_fn()(this_arg,
                        std::vector<Value>(apply_args.begin(), apply_args.end()), false);
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
                auto push_res = push_call_frame(fn, std::move(this_arg),
                    std::span<Value>(apply_args.data(), apply_args.size()));
                if (!push_res.is_ok()) {
                    frame.pending_throw = make_error_value(NativeErrorType::kTypeError,
                        strip_error_prefix(push_res.error().message()));
                    continue;
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
            if (obj_val.is_symbol()) {
                if (name == "description") {
                    const std::string* desc = symbol_table_.GetDescription(obj_val.as_symbol_id());
                    stack.push_back(desc ? Value::string(*desc) : Value::undefined());
                } else if (symbol_prototype_) {
                    stack.push_back(symbol_prototype_->get_property(name));
                } else {
                    stack.push_back(Value::undefined());
                }
                break;
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
            if (obj_val.is_number()) {
                if (number_prototype_) {
                    stack.push_back(number_prototype_->get_property(name));
                } else {
                    stack.push_back(Value::undefined());
                }
                break;
            }
            if (obj_val.is_bool()) {
                if (boolean_prototype_) {
                    stack.push_back(boolean_prototype_->get_property(name));
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
            if (raw_obj->object_kind() == ObjectKind::kRegExp) {
                auto* rx = static_cast<JSRegExp*>(raw_obj);
                if (name == "source") {
                    stack.push_back(Value::string(rx->pattern_.empty() ? "(?:)" : rx->pattern_));
                } else if (name == "flags") {
                    stack.push_back(Value::string(rx->flags_str_));
                } else if (name == "global") {
                    stack.push_back(Value::boolean(rx->global_));
                } else if (name == "ignoreCase") {
                    stack.push_back(Value::boolean(rx->ignore_case_));
                } else if (name == "multiline") {
                    stack.push_back(Value::boolean(rx->multiline_));
                } else if (name == "dotAll") {
                    stack.push_back(Value::boolean(rx->dot_all_));
                } else if (name == "sticky") {
                    stack.push_back(Value::boolean(rx->sticky_));
                } else if (name == "unicode") {
                    stack.push_back(Value::boolean(rx->unicode_));
                } else if (name == "lastIndex") {
                    stack.push_back(Value::number(static_cast<double>(rx->last_index_)));
                } else if (regexp_prototype_) {
                    stack.push_back(regexp_prototype_->get_property(name));
                } else {
                    stack.push_back(Value::undefined());
                }
                break;
            }
            if (raw_obj->object_kind() == ObjectKind::kStringObject) {
                auto* obj = static_cast<JSObject*>(raw_obj);
                if (name == "length") {
                    JSString* js_str = obj->wrapped_value().js_string_raw();
                    stack.push_back(Value::number(static_cast<double>(utf8_cp_len_vm(js_str))));
                } else {
                    stack.push_back(obj->get_property(name));
                }
                break;
            }
            if (raw_obj->object_kind() == ObjectKind::kBooleanObject) {
                auto* obj = static_cast<JSObject*>(raw_obj);
                stack.push_back(obj->get_property(name));
                break;
            }
            if (raw_obj->object_kind() != ObjectKind::kOrdinary && raw_obj->object_kind() != ObjectKind::kArray &&
                raw_obj->object_kind() != ObjectKind::kGenerator &&
                raw_obj->object_kind() != ObjectKind::kMap && raw_obj->object_kind() != ObjectKind::kSet &&
                raw_obj->object_kind() != ObjectKind::kWeakMap && raw_obj->object_kind() != ObjectKind::kWeakSet) {
                stack.push_back(Value::undefined());
                break;
            }
            {
                auto* obj = static_cast<JSObject*>(raw_obj);
                // Check prototype chain for accessor getter
                Value getter_to_call;
                bool found_accessor = false;
                const JSObject* cur = obj;
                while (cur != nullptr) {
                    const JSObject::PropertyEntry* entry = cur->get_own_entry(name);
                    if (entry != nullptr && (entry->flags & kPropIsAccessor)) {
                        found_accessor = true;
                        if (!entry->getter.is_undefined() && !entry->getter.is_null()) {
                            getter_to_call = entry->getter;
                        }
                        break;
                    }
                    if (entry != nullptr) break;
                    cur = cur->proto().get();
                }
                if (found_accessor) {
                    if (getter_to_call.is_undefined()) {
                        // call_stack_ may realloc after call_function_val; re-fetch frame
                        call_stack_.back().stack.push_back(Value::undefined());
                    } else {
                        auto getter_res = call_function_val(getter_to_call, obj_val, {});
                        // Re-fetch frame: call_stack_ may have reallocated during getter call
                        CallFrame& cur_frame = call_stack_.back();
                        if (!getter_res.is_ok()) {
                            const std::string& emsg = getter_res.error().message();
                            if (emsg == "__qppjs_pending_throw__" && native_pending_throw_.has_value()) {
                                cur_frame.pending_throw = std::move(*native_pending_throw_);
                                native_pending_throw_ = std::nullopt;
                            } else {
                                cur_frame.pending_throw = make_error_value(NativeErrorType::kTypeError,
                                    emsg);
                            }
                            continue;
                        }
                        cur_frame.stack.push_back(getter_res.value());
                    }
                    break;
                }
                // No accessor found — use normal property lookup
                stack.push_back(obj->get_property(name));
            }
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
            // Handle JSFunction specially (e.g., Fn.prototype = ..., static methods)
            RcObject* raw_set = obj_val.as_object_raw();
            if (raw_set->object_kind() == ObjectKind::kFunction) {
                auto* fn = static_cast<JSFunction*>(raw_set);
                if (name == "prototype" && val.is_object()) {
                    RcObject* proto_raw = val.as_object_raw();
                    if (proto_raw && proto_raw->object_kind() == ObjectKind::kOrdinary) {
                        fn->set_prototype_obj(RcPtr<JSObject>(static_cast<JSObject*>(proto_raw)));
                    }
                } else {
                    // Store as own property (e.g., static class methods)
                    fn->set_property(name, val);
                }
                stack.push_back(std::move(val));
                break;
            }
            // kRegExp: allow writing lastIndex
            if (raw_set->object_kind() == ObjectKind::kRegExp) {
                if (name == "lastIndex") {
                    double n = to_number_double_vm(val);
                    auto* rx = static_cast<JSRegExp*>(raw_set);
                    rx->last_index_ = std::isnan(n) || n < 0.0 ? 0u : static_cast<uint32_t>(n);
                }
                stack.push_back(std::move(val));
                break;
            }
            if (raw_set->object_kind() != ObjectKind::kOrdinary && raw_set->object_kind() != ObjectKind::kArray &&
                raw_set->object_kind() != ObjectKind::kGenerator &&
                raw_set->object_kind() != ObjectKind::kMap && raw_set->object_kind() != ObjectKind::kSet &&
                raw_set->object_kind() != ObjectKind::kWeakMap && raw_set->object_kind() != ObjectKind::kWeakSet &&
                raw_set->object_kind() != ObjectKind::kStringObject &&
                raw_set->object_kind() != ObjectKind::kBooleanObject) {
                frame.pending_throw = make_error_value(NativeErrorType::kTypeError,
                    "Cannot set property '" + name + "' on non-JSObject");
                continue;
            }
            {
                auto* obj = static_cast<JSObject*>(raw_set);
                // Check prototype chain for accessor setter
                Value setter_to_call;
                bool found_accessor = false;
                bool no_setter = false;
                {
                    const JSObject* cur = obj;
                    while (cur != nullptr) {
                        const JSObject::PropertyEntry* entry = cur->get_own_entry(name);
                        if (entry != nullptr) {
                            if (entry->flags & kPropIsAccessor) {
                                found_accessor = true;
                                if (entry->setter.is_undefined() || entry->setter.is_null()) {
                                    no_setter = true;
                                } else {
                                    setter_to_call = entry->setter;
                                }
                            }
                            break;
                        }
                        cur = cur->proto().get();
                    }
                }
                if (found_accessor) {
                    if (no_setter) {
                        // Sloppy mode: silently ignore write to get-only accessor
                        stack.push_back(std::move(val));
                    } else {
                        std::vector<Value> setter_args = {val};
                        auto setter_res = call_function_val(setter_to_call, obj_val, setter_args);
                        // Re-fetch frame: call_stack_ may have reallocated
                        CallFrame& cur_frame = call_stack_.back();
                        if (!setter_res.is_ok()) {
                            const std::string& emsg = setter_res.error().message();
                            if (emsg == "__qppjs_pending_throw__" && native_pending_throw_.has_value()) {
                                cur_frame.pending_throw = std::move(*native_pending_throw_);
                                native_pending_throw_ = std::nullopt;
                            } else {
                                cur_frame.pending_throw = make_error_value(NativeErrorType::kTypeError,
                                    emsg);
                            }
                            continue;
                        }
                        cur_frame.stack.push_back(std::move(val));
                    }
                } else {
                    auto set_ex_res = obj->set_property_ex(name, val);
                    if (!set_ex_res.is_ok()) {
                        const std::string& msg = set_ex_res.error().message();
                        NativeErrorType err_type = NativeErrorType::kRangeError;
                        if (msg.rfind("TypeError:", 0) == 0) err_type = NativeErrorType::kTypeError;
                        frame.pending_throw = make_error_value(err_type, strip_error_prefix(msg));
                        continue;
                    }
                    stack.push_back(std::move(val));
                }
            }
            break;
        }

        case Opcode::kDefineGetter: {
            uint16_t idx = read_u16(bc, pc);
            const std::string& name = bc->names[idx];
            Value fn_val = std::move(stack.back());
            stack.pop_back();
            Value obj_val = std::move(stack.back());
            stack.pop_back();
            if (obj_val.is_object()) {
                RcObject* raw = obj_val.as_object_raw();
                if (raw->object_kind() == ObjectKind::kOrdinary ||
                    raw->object_kind() == ObjectKind::kArray) {
                    auto* obj = static_cast<JSObject*>(raw);
                    // Write .name on getter function
                    if (fn_val.is_object() && fn_val.as_object_raw()->object_kind() == ObjectKind::kFunction) {
                        auto* fn = static_cast<JSFunction*>(fn_val.as_object_raw());
                        fn->set_is_method(true);
                        fn->set_property("name", Value::string("get " + name));
                    }
                    PropDesc desc;
                    desc.getter = fn_val;
                    desc.enumerable = true;
                    desc.configurable = true;
                    auto res = obj->define_property(name, desc);
                    if (!res.is_ok()) {
                        frame.pending_throw = make_error_value(NativeErrorType::kTypeError,
                            "Cannot define getter " + name);
                        continue;
                    }
                }
            }
            stack.push_back(std::move(fn_val));
            break;
        }

        case Opcode::kDefineSetter: {
            uint16_t idx = read_u16(bc, pc);
            const std::string& name = bc->names[idx];
            Value fn_val = std::move(stack.back());
            stack.pop_back();
            Value obj_val = std::move(stack.back());
            stack.pop_back();
            if (obj_val.is_object()) {
                RcObject* raw = obj_val.as_object_raw();
                if (raw->object_kind() == ObjectKind::kOrdinary ||
                    raw->object_kind() == ObjectKind::kArray) {
                    auto* obj = static_cast<JSObject*>(raw);
                    // Write .name on setter function
                    if (fn_val.is_object() && fn_val.as_object_raw()->object_kind() == ObjectKind::kFunction) {
                        auto* fn = static_cast<JSFunction*>(fn_val.as_object_raw());
                        fn->set_is_method(true);
                        fn->set_property("name", Value::string("set " + name));
                    }
                    PropDesc desc;
                    desc.setter = fn_val;
                    desc.enumerable = true;
                    desc.configurable = true;
                    auto res = obj->define_property(name, desc);
                    if (!res.is_ok()) {
                        frame.pending_throw = make_error_value(NativeErrorType::kTypeError,
                            "Cannot define setter " + name);
                        continue;
                    }
                }
            }
            stack.push_back(std::move(fn_val));
            break;
        }

        case Opcode::kSetComputedProp: {
            // Stack: sp[-3]=obj, sp[-2]=key, sp[-1]=val
            Value val = std::move(stack.back()); stack.pop_back();
            Value key = std::move(stack.back()); stack.pop_back();
            Value obj_copy = std::move(stack.back()); stack.pop_back();
            if (obj_copy.is_object()) {
                RcObject* raw = obj_copy.as_object_raw();
                if (raw->object_kind() == ObjectKind::kOrdinary ||
                    raw->object_kind() == ObjectKind::kArray) {
                    auto* obj = static_cast<JSObject*>(raw);
                    if (key.is_symbol()) {
                        uint64_t sym_id = key.as_symbol_id();
                        if (val.is_object() && val.as_object_raw()->object_kind() == ObjectKind::kFunction) {
                            auto* fn = static_cast<JSFunction*>(val.as_object_raw());
                            if (fn->is_method()) {
                                const std::string* desc = symbol_table_.GetDescription(sym_id);
                                std::string name = "[" + (desc ? *desc : "") + "]";
                                fn->set_property("name", Value::string(name));
                            }
                        }
                        obj->set_property_by_symbol(sym_id, val);
                    } else {
                        std::string str_key = to_string_val(key);
                        if (val.is_object() && val.as_object_raw()->object_kind() == ObjectKind::kFunction) {
                            auto* fn = static_cast<JSFunction*>(val.as_object_raw());
                            if (fn->is_method()) fn->set_property("name", Value::string(str_key));
                        }
                        obj->set_property(str_key, val);
                    }
                }
            }
            stack.push_back(std::move(val));
            break;
        }

        case Opcode::kDefineComputedGetter: {
            // Stack: sp[-3]=obj, sp[-2]=key, sp[-1]=fn
            Value fn_val = std::move(stack.back()); stack.pop_back();
            Value key = std::move(stack.back()); stack.pop_back();
            Value obj_copy = std::move(stack.back()); stack.pop_back();
            if (obj_copy.is_object()) {
                RcObject* raw = obj_copy.as_object_raw();
                if (raw->object_kind() == ObjectKind::kOrdinary ||
                    raw->object_kind() == ObjectKind::kArray) {
                    auto* obj = static_cast<JSObject*>(raw);
                    // Cache to_string_val once for non-symbol keys (used for name and define_property)
                    std::string str_key = key.is_symbol() ? std::string{} : to_string_val(key);
                    if (fn_val.is_object() && fn_val.as_object_raw()->object_kind() == ObjectKind::kFunction) {
                        auto* fn = static_cast<JSFunction*>(fn_val.as_object_raw());
                        fn->set_is_method(true);
                        if (key.is_symbol()) {
                            const std::string* desc = symbol_table_.GetDescription(key.as_symbol_id());
                            fn->set_property("name", Value::string("get [" + (desc ? *desc : "") + "]"));
                        } else {
                            fn->set_property("name", Value::string("get " + str_key));
                        }
                    }
                    PropDesc desc;
                    desc.getter = fn_val;
                    desc.enumerable = true;
                    desc.configurable = true;
                    if (key.is_symbol()) {
                        obj->define_property_by_symbol(key.as_symbol_id(), desc);
                    } else {
                        obj->define_property(str_key, desc);
                    }
                }
            }
            stack.push_back(std::move(fn_val));
            break;
        }

        case Opcode::kDefineComputedSetter: {
            // Stack: sp[-3]=obj, sp[-2]=key, sp[-1]=fn
            Value fn_val = std::move(stack.back()); stack.pop_back();
            Value key = std::move(stack.back()); stack.pop_back();
            Value obj_copy = std::move(stack.back()); stack.pop_back();
            if (obj_copy.is_object()) {
                RcObject* raw = obj_copy.as_object_raw();
                if (raw->object_kind() == ObjectKind::kOrdinary ||
                    raw->object_kind() == ObjectKind::kArray) {
                    auto* obj = static_cast<JSObject*>(raw);
                    // Cache to_string_val once for non-symbol keys (used for name and define_property)
                    std::string str_key = key.is_symbol() ? std::string{} : to_string_val(key);
                    if (fn_val.is_object() && fn_val.as_object_raw()->object_kind() == ObjectKind::kFunction) {
                        auto* fn = static_cast<JSFunction*>(fn_val.as_object_raw());
                        fn->set_is_method(true);
                        if (key.is_symbol()) {
                            const std::string* desc = symbol_table_.GetDescription(key.as_symbol_id());
                            fn->set_property("name", Value::string("set [" + (desc ? *desc : "") + "]"));
                        } else {
                            fn->set_property("name", Value::string("set " + str_key));
                        }
                    }
                    PropDesc desc;
                    desc.setter = fn_val;
                    desc.enumerable = true;
                    desc.configurable = true;
                    if (key.is_symbol()) {
                        obj->define_property_by_symbol(key.as_symbol_id(), desc);
                    } else {
                        obj->define_property(str_key, desc);
                    }
                }
            }
            stack.push_back(std::move(fn_val));
            break;
        }

        case Opcode::kGetElem: {
            Value key_val = std::move(stack.back());
            stack.pop_back();
            Value obj_val = std::move(stack.back());
            stack.pop_back();
            // Symbol key: look up symbol-keyed property on object or prototype
            if (key_val.is_symbol()) {
                uint64_t sym_id = key_val.as_symbol_id();
                if (obj_val.is_string()) {
                    // String primitive: look up symbol in string_prototype_
                    if (string_prototype_) {
                        stack.push_back(string_prototype_->get_property_by_symbol(sym_id));
                    } else {
                        stack.push_back(Value::undefined());
                    }
                } else if (obj_val.is_object()) {
                    RcObject* raw_sym = obj_val.as_object_raw();
                    if (raw_sym->object_kind() == ObjectKind::kOrdinary ||
                        raw_sym->object_kind() == ObjectKind::kArray ||
                        raw_sym->object_kind() == ObjectKind::kGenerator ||
                        raw_sym->object_kind() == ObjectKind::kMap ||
                        raw_sym->object_kind() == ObjectKind::kSet) {
                        auto* js_obj_sym = static_cast<JSObject*>(raw_sym);
                        const JSObject::SymbolPropertyEntry* sym_entry = js_obj_sym->find_symbol_entry(sym_id);
                        if (sym_entry != nullptr && sym_entry->is_accessor) {
                            if (sym_entry->getter.is_undefined() || sym_entry->getter.is_null()) {
                                call_stack_.back().stack.push_back(Value::undefined());
                            } else {
                                Value getter_copy = sym_entry->getter;
                                auto getter_res = call_function_val(getter_copy, obj_val, {});
                                CallFrame& cur_frame = call_stack_.back();
                                if (!getter_res.is_ok()) {
                                    const std::string& emsg = getter_res.error().message();
                                    if (emsg == "__qppjs_pending_throw__" && native_pending_throw_.has_value()) {
                                        cur_frame.pending_throw = std::move(*native_pending_throw_);
                                        native_pending_throw_ = std::nullopt;
                                    } else {
                                        cur_frame.pending_throw = make_error_value(NativeErrorType::kTypeError, emsg);
                                    }
                                    continue;
                                }
                                cur_frame.stack.push_back(getter_res.value());
                            }
                        } else if (sym_entry != nullptr) {
                            stack.push_back(sym_entry->value);
                        } else {
                            stack.push_back(Value::undefined());
                        }
                    } else if (raw_sym->object_kind() == ObjectKind::kFunction) {
                        // Static private field on JSFunction: use sym_id as string key prefix
                        auto* fn_sym = static_cast<JSFunction*>(raw_sym);
                        std::string sym_key = "__pfsym_" + std::to_string(sym_id) + "__";
                        Value v = fn_sym->get_property(sym_key);
                        stack.push_back(std::move(v));
                    } else {
                        stack.push_back(Value::undefined());
                    }
                } else {
                    stack.push_back(Value::undefined());
                }
                break;
            }
            if (obj_val.is_symbol()) {
                std::string key = to_string_val(key_val);
                if (key == "description") {
                    const std::string* desc = symbol_table_.GetDescription(obj_val.as_symbol_id());
                    stack.push_back(desc ? Value::string(*desc) : Value::undefined());
                } else if (symbol_prototype_) {
                    stack.push_back(symbol_prototype_->get_property(key));
                } else {
                    stack.push_back(Value::undefined());
                }
                break;
            }
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
            if (raw_elem->object_kind() == ObjectKind::kStringObject) {
                auto* obj = static_cast<JSObject*>(raw_elem);
                std::string key = to_string_val(key_val);
                if (key == "length") {
                    JSString* js_str = obj->wrapped_value().js_string_raw();
                    stack.push_back(Value::number(static_cast<double>(utf8_cp_len_vm(js_str))));
                } else {
                    stack.push_back(obj->get_property(key));
                }
                break;
            }
            if (raw_elem->object_kind() == ObjectKind::kBooleanObject) {
                auto* obj = static_cast<JSObject*>(raw_elem);
                stack.push_back(obj->get_property(to_string_val(key_val)));
                break;
            }
            if (raw_elem->object_kind() == ObjectKind::kFunction) {
                // Function with string key: check own properties (static accessor/method support)
                auto* fn_elem = static_cast<JSFunction*>(raw_elem);
                std::string fn_key = to_string_val(key_val);
                Value own_val = fn_elem->get_property(fn_key);
                if (!own_val.is_undefined()) {
                    stack.push_back(std::move(own_val));
                } else if (fn_key == "prototype") {
                    const auto& proto = fn_elem->prototype_obj();
                    stack.push_back(proto ? Value::object(ObjectPtr(proto)) : Value::undefined());
                } else if (function_prototype_) {
                    // Also check own_properties_ for accessor (static getter/setter)
                    // fn_elem->get_property already handles this; try accessor via own_properties_
                    stack.push_back(function_prototype_->get_property(fn_key));
                } else {
                    stack.push_back(Value::undefined());
                }
                break;
            }
            if (raw_elem->object_kind() != ObjectKind::kOrdinary &&
                raw_elem->object_kind() != ObjectKind::kGenerator &&
                raw_elem->object_kind() != ObjectKind::kMap && raw_elem->object_kind() != ObjectKind::kSet &&
                raw_elem->object_kind() != ObjectKind::kWeakMap && raw_elem->object_kind() != ObjectKind::kWeakSet) {
                frame.pending_throw = make_error_value(NativeErrorType::kTypeError,
                    "Cannot read element of non-JSObject");
                continue;
            }
            {
                auto* obj = static_cast<JSObject*>(raw_elem);
                std::string elem_key = to_string_val(key_val);
                // Check prototype chain for accessor getter (mirrors kGetProp behavior)
                Value getter_to_call;
                bool found_accessor = false;
                const JSObject* cur_elem = obj;
                while (cur_elem != nullptr) {
                    const JSObject::PropertyEntry* entry = cur_elem->get_own_entry(elem_key);
                    if (entry != nullptr && (entry->flags & kPropIsAccessor)) {
                        found_accessor = true;
                        if (!entry->getter.is_undefined() && !entry->getter.is_null()) {
                            getter_to_call = entry->getter;
                        }
                        break;
                    }
                    if (entry != nullptr) break;
                    cur_elem = cur_elem->proto().get();
                }
                if (found_accessor) {
                    if (getter_to_call.is_undefined()) {
                        call_stack_.back().stack.push_back(Value::undefined());
                    } else {
                        auto getter_res = call_function_val(getter_to_call, obj_val, {});
                        CallFrame& cur_frame = call_stack_.back();
                        if (!getter_res.is_ok()) {
                            const std::string& emsg = getter_res.error().message();
                            if (emsg == "__qppjs_pending_throw__" && native_pending_throw_.has_value()) {
                                cur_frame.pending_throw = std::move(*native_pending_throw_);
                                native_pending_throw_ = std::nullopt;
                            } else {
                                cur_frame.pending_throw = make_error_value(NativeErrorType::kTypeError, emsg);
                            }
                            continue;
                        }
                        cur_frame.stack.push_back(getter_res.value());
                    }
                } else {
                    stack.push_back(obj->get_property(elem_key));
                }
            }
            break;
        }

        case Opcode::kSetElem: {
            Value key_val = std::move(stack.back());
            stack.pop_back();
            Value val = std::move(stack.back());
            stack.pop_back();
            Value obj_val = std::move(stack.back());
            stack.pop_back();
            // Symbol key: store symbol-keyed property on object (or call setter if accessor)
            if (key_val.is_symbol()) {
                if (obj_val.is_object()) {
                    RcObject* raw_sym = obj_val.as_object_raw();
                    uint64_t sym_id = key_val.as_symbol_id();
                    if (raw_sym->object_kind() == ObjectKind::kOrdinary ||
                        raw_sym->object_kind() == ObjectKind::kArray) {
                        auto* js_obj_sym = static_cast<JSObject*>(raw_sym);
                        const JSObject::SymbolPropertyEntry* sym_entry = js_obj_sym->find_symbol_entry(sym_id);
                        if (sym_entry != nullptr && sym_entry->is_accessor) {
                            if (!sym_entry->setter.is_undefined() && !sym_entry->setter.is_null()) {
                                Value setter_copy = sym_entry->setter;
                                std::vector<Value> setter_arg_vec = {val};
                                auto sres = call_function_val(setter_copy, obj_val, setter_arg_vec);
                                CallFrame& cur_frame = call_stack_.back();
                                if (!sres.is_ok()) {
                                    const std::string& emsg = sres.error().message();
                                    if (emsg == "__qppjs_pending_throw__" && native_pending_throw_.has_value()) {
                                        cur_frame.pending_throw = std::move(*native_pending_throw_);
                                        native_pending_throw_ = std::nullopt;
                                    } else {
                                        cur_frame.pending_throw = make_error_value(NativeErrorType::kTypeError, emsg);
                                    }
                                    continue;
                                }
                                cur_frame.stack.push_back(std::move(val));
                            } else {
                                stack.push_back(std::move(val));
                            }
                            break;
                        }
                        js_obj_sym->set_property_by_symbol(sym_id, val);
                    } else if (raw_sym->object_kind() == ObjectKind::kFunction) {
                        // Static private field on JSFunction: use sym_id as string key prefix
                        auto* fn_sym = static_cast<JSFunction*>(raw_sym);
                        std::string sym_key = "__pfsym_" + std::to_string(sym_id) + "__";
                        fn_sym->set_property(sym_key, val);
                    }
                }
                stack.push_back(std::move(val));
                break;
            }
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
            if (raw_setelem->object_kind() != ObjectKind::kOrdinary &&
                raw_setelem->object_kind() != ObjectKind::kGenerator &&
                raw_setelem->object_kind() != ObjectKind::kMap && raw_setelem->object_kind() != ObjectKind::kSet &&
                raw_setelem->object_kind() != ObjectKind::kWeakMap && raw_setelem->object_kind() != ObjectKind::kWeakSet &&
                raw_setelem->object_kind() != ObjectKind::kStringObject &&
                raw_setelem->object_kind() != ObjectKind::kBooleanObject) {
                frame.pending_throw = make_error_value(NativeErrorType::kTypeError,
                    "Cannot set element on non-JSObject");
                continue;
            }
            {
                auto* obj = static_cast<JSObject*>(raw_setelem);
                std::string setelem_key = to_string_val(key_val);
                // Check prototype chain for accessor setter (mirrors kSetProp behavior)
                Value setter_to_call;
                bool found_accessor = false;
                bool no_setter = false;
                {
                    const JSObject* cur_setelem = obj;
                    while (cur_setelem != nullptr) {
                        const JSObject::PropertyEntry* entry = cur_setelem->get_own_entry(setelem_key);
                        if (entry != nullptr) {
                            if (entry->flags & kPropIsAccessor) {
                                found_accessor = true;
                                if (entry->setter.is_undefined() || entry->setter.is_null()) {
                                    no_setter = true;
                                } else {
                                    setter_to_call = entry->setter;
                                }
                            }
                            break;
                        }
                        cur_setelem = cur_setelem->proto().get();
                    }
                }
                if (found_accessor) {
                    if (no_setter) {
                        stack.push_back(std::move(val));  // sloppy: ignore
                    } else {
                        std::vector<Value> setter_args = {val};
                        auto setter_res = call_function_val(setter_to_call, obj_val, setter_args);
                        CallFrame& cur_frame = call_stack_.back();
                        if (!setter_res.is_ok()) {
                            const std::string& emsg = setter_res.error().message();
                            if (emsg == "__qppjs_pending_throw__" && native_pending_throw_.has_value()) {
                                cur_frame.pending_throw = std::move(*native_pending_throw_);
                                native_pending_throw_ = std::nullopt;
                            } else {
                                cur_frame.pending_throw = make_error_value(NativeErrorType::kTypeError, emsg);
                            }
                            continue;
                        }
                        cur_frame.stack.push_back(std::move(val));
                    }
                } else {
                    auto set_ex_res = obj->set_property_ex(setelem_key, val);
                    if (!set_ex_res.is_ok()) {
                        const std::string& msg = set_ex_res.error().message();
                        NativeErrorType err_type = NativeErrorType::kRangeError;
                        if (msg.rfind("TypeError:", 0) == 0) err_type = NativeErrorType::kTypeError;
                        frame.pending_throw = make_error_value(err_type, strip_error_prefix(msg));
                        continue;
                    }
                    stack.push_back(std::move(val));
                }
            }
            break;
        }

        // ---- Functions and calls ----

        case Opcode::kMakeFunction: {
            uint16_t fn_idx = read_u16(bc, pc);
            const auto& fn_bc = bc->functions[fn_idx];
            auto fn = RcPtr<JSFunction>::make();
            fn->set_name(fn_bc->name);
            fn->set_params(fn_bc->params);
            fn->set_rest_param(fn_bc->rest_param);
            fn->set_property("length", Value::number(static_cast<double>(fn_bc->length_count)));
            fn->set_property("name", Value::string(fn_bc->name.value_or("")));
            if (fn_bc->param_defs) fn->set_param_defs(fn_bc->param_defs);
            fn->set_bytecode(fn_bc);
            fn->set_closure_env(env);
            fn->set_is_named_expr(fn_bc->is_named_expr);
            fn->set_defining_module(frame.current_module);
            // 方法简写：标记 is_method，写入 .name（覆盖方法名称，保留已有逻辑）
            fn->set_is_method(fn_bc->is_method);
            if (fn_bc->is_method && fn_bc->name.has_value()) {
                fn->set_property("name", Value::string(*fn_bc->name));
            }
            // 箭头函数：捕获词法 this，不创建 prototype 对象
            if (fn_bc->is_arrow) {
                fn->set_arrow(true);
                fn->set_lexical_this(frame.this_val);
                gc_heap_.Register(fn.get());
                stack.push_back(Value::object(ObjectPtr(fn)));
                break;
            }
            auto proto_obj = RcPtr<JSObject>::make();
            proto_obj->set_proto(object_prototype_);
            proto_obj->set_constructor_property(fn.get());
            fn->set_prototype_obj(proto_obj);
            gc_heap_.Register(fn.get());
            gc_heap_.Register(proto_obj.get());
            // Wrap async generator functions (async function*)
            if (fn_bc->is_async_generator) {
                // fn_bc->is_generator=true, so inner_fn is already treated as generator
                auto inner_fn = fn;
                auto ag_wrapper = RcPtr<JSFunction>::make();
                ag_wrapper->set_name(fn_bc->name);
                ag_wrapper->set_property("__async_inner__", Value::object(ObjectPtr(inner_fn)));
                ag_wrapper->set_is_async_generator(true);
                ag_wrapper->set_native_fn([this, inner_fn](Value this_val,
                        std::vector<Value> call_args, bool) mutable -> EvalResult {
                    // push_call_frame with is_generator=true returns the generator object
                    auto push_res = push_call_frame(inner_fn, std::move(this_val),
                        std::span<Value>(call_args.data(), call_args.size()));
                    if (!push_res.is_ok()) {
                        return push_res;
                    }
                    // push_call_frame for generator returns the gen object via returned value
                    // (it's pushed to stack? No - push_call_frame returns ok(gen_val) for generators)
                    // Actually push_call_frame returns ok(gen_val) directly for generators
                    Value gen_val = push_res.value();
                    if (!gen_val.is_object()) {
                        return EvalResult::err(Error(ErrorKind::Runtime, "TypeError: async generator creation failed"));
                    }
                    RcObject* raw = gen_val.as_object_raw();
                    if (raw->object_kind() != ObjectKind::kGenerator) {
                        return EvalResult::err(Error(ErrorKind::Runtime, "TypeError: async generator creation failed"));
                    }
                    auto gen_obj = RcPtr<JSGeneratorObject>(static_cast<JSGeneratorObject*>(raw));

                    // Wrap .next() to return a Promise
                    {
                        auto next_fn = RcPtr<JSFunction>::make();
                        gc_heap_.Register(next_fn.get());
                        next_fn->set_name(std::string("next"));
                        next_fn->set_property("length", Value::number(1));
                        next_fn->set_native_fn([this, gen_obj](Value /*this_val*/, std::vector<Value> args, bool) mutable -> EvalResult {
                            auto outer_promise = RcPtr<JSPromise>::make();
                            gc_heap_.Register(outer_promise.get());
                            Value outer_val = Value::object(ObjectPtr(outer_promise));
                            Value resume_val = args.empty() ? Value::undefined() : args[0];
                            auto sync_result = vm_generator_next(gen_obj, std::move(resume_val));
                            // Handle await suspension: vm_pending_inner_promise_ is set by kAwait
                            if (vm_pending_inner_promise_.has_value()) {
                                auto inner_promise = std::move(*vm_pending_inner_promise_);
                                vm_pending_inner_promise_ = std::nullopt;
                                // Resume when inner promise resolves: call gen.next() with the value
                                auto resume_fn = RcPtr<JSFunction>::make();
                                gc_heap_.Register(resume_fn.get());
                                resume_fn->set_native_fn([this, gen_obj, outer_promise]
                                        (Value /*tv*/, std::vector<Value> rargs, bool) mutable -> EvalResult {
                                    Value resolved_val = rargs.empty() ? Value::undefined() : rargs[0];
                                    auto next2 = vm_generator_next(gen_obj, std::move(resolved_val));
                                    if (vm_pending_inner_promise_.has_value()) {
                                        // Another await: not fully supported in VM recursive resume
                                        vm_pending_inner_promise_ = std::nullopt;
                                        outer_promise->Reject(
                                            make_error_value(NativeErrorType::kTypeError,
                                                "multiple awaits in async generator not supported in VM"),
                                            job_queue_);
                                    } else {
                                        vm_ag_resolve(next2, outer_promise);
                                    }
                                    return EvalResult::ok(Value::undefined());
                                });
                                auto reject_fn = RcPtr<JSFunction>::make();
                                gc_heap_.Register(reject_fn.get());
                                reject_fn->set_native_fn([this, gen_obj, outer_promise]
                                        (Value /*tv*/, std::vector<Value> rargs, bool) mutable -> EvalResult {
                                    Value reason = rargs.empty() ? Value::undefined() : rargs[0];
                                    auto next3 = vm_generator_throw(gen_obj, std::move(reason));
                                    vm_ag_resolve(next3, outer_promise);
                                    return EvalResult::ok(Value::undefined());
                                });
                                JSPromise::PerformThen(inner_promise,
                                    Value::object(ObjectPtr(resume_fn)),
                                    Value::object(ObjectPtr(reject_fn)),
                                    job_queue_);
                                vm_drain_job_queue();
                            } else {
                                vm_ag_resolve(sync_result, outer_promise);
                            }
                            return EvalResult::ok(outer_val);
                        });
                        gen_obj->set_property("next", Value::object(ObjectPtr(next_fn)));
                    }
                    // Wrap .return() to return a Promise
                    {
                        auto ret_fn = RcPtr<JSFunction>::make();
                        gc_heap_.Register(ret_fn.get());
                        ret_fn->set_name(std::string("return"));
                        ret_fn->set_property("length", Value::number(1));
                        ret_fn->set_native_fn([this, gen_obj](Value /*this_val*/, std::vector<Value> args, bool) mutable -> EvalResult {
                            auto outer_promise = RcPtr<JSPromise>::make();
                            gc_heap_.Register(outer_promise.get());
                            Value outer_val = Value::object(ObjectPtr(outer_promise));
                            Value ret_val = args.empty() ? Value::undefined() : args[0];
                            auto sync_result = vm_generator_return(gen_obj, std::move(ret_val));
                            vm_ag_resolve(sync_result, outer_promise);
                            return EvalResult::ok(outer_val);
                        });
                        gen_obj->set_property("return", Value::object(ObjectPtr(ret_fn)));
                    }
                    // Wrap .throw() to return a Promise
                    {
                        auto throw_fn = RcPtr<JSFunction>::make();
                        gc_heap_.Register(throw_fn.get());
                        throw_fn->set_name(std::string("throw"));
                        throw_fn->set_property("length", Value::number(1));
                        throw_fn->set_native_fn([this, gen_obj](Value /*this_val*/, std::vector<Value> args, bool) mutable -> EvalResult {
                            auto outer_promise = RcPtr<JSPromise>::make();
                            gc_heap_.Register(outer_promise.get());
                            Value outer_val = Value::object(ObjectPtr(outer_promise));
                            Value throw_val = args.empty() ? Value::undefined() : args[0];
                            auto sync_result = vm_generator_throw(gen_obj, std::move(throw_val));
                            vm_ag_resolve(sync_result, outer_promise);
                            return EvalResult::ok(outer_val);
                        });
                        gen_obj->set_property("throw", Value::object(ObjectPtr(throw_fn)));
                    }

                    return EvalResult::ok(gen_val);
                });
                gc_heap_.Register(ag_wrapper.get());
                stack.push_back(Value::object(ObjectPtr(ag_wrapper)));
                // Skip the normal path
            // Wrap async functions: create a NativeFn that creates outer_promise and runs bytecode
            } else if (fn_bc->is_async) {
                auto inner_fn = fn;  // the bytecode function
                auto async_wrapper = RcPtr<JSFunction>::make();
                async_wrapper->set_name(fn_bc->name);
                // Store inner_fn in own_properties so GC can trace it
                async_wrapper->set_property("__async_inner__", Value::object(ObjectPtr(inner_fn)));
                // 方法简写：async_wrapper 也标记 is_method，写入 .name，不创建 proto
                if (fn_bc->is_method) {
                    async_wrapper->set_is_method(true);
                    if (fn_bc->name.has_value()) {
                        async_wrapper->set_property("name", Value::string(*fn_bc->name));
                    }
                }
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
                if (!fn_bc->is_method) {
                    // 非方法 async 函数才创建 prototype 对象
                    auto async_proto = RcPtr<JSObject>::make();
                    async_proto->set_proto(object_prototype_);
                    async_proto->set_constructor_property(async_wrapper.get());
                    async_wrapper->set_prototype_obj(async_proto);
                    gc_heap_.Register(async_proto.get());
                }
                gc_heap_.Register(async_wrapper.get());
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
            // Class constructor cannot be called without 'new'
            if (fn->is_class_ctor()) {
                std::string fn_name = fn->name().has_value() ? *fn->name() : "class";
                frame.pending_throw = make_error_value(NativeErrorType::kTypeError,
                    "Class constructor " + fn_name + " cannot be invoked without 'new'");
                continue;
            }
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
            // If push_call_frame returned a non-undefined value, it created a generator object
            if (!push_res.value().is_undefined()) {
                stack.push_back(push_res.value());
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
            // Class constructor cannot be called without 'new'
            if (fn->is_class_ctor()) {
                std::string fn_name = fn->name().has_value() ? *fn->name() : "class";
                frame.pending_throw = make_error_value(NativeErrorType::kTypeError,
                    "Class constructor " + fn_name + " cannot be invoked without 'new'");
                continue;
            }
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
            // If push_call_frame returned a non-undefined value, it created a generator object
            if (!push_res.value().is_undefined()) {
                stack.push_back(push_res.value());
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
            // 箭头函数不可 new
            if (fn->is_arrow()) {
                frame.pending_throw = make_error_value(NativeErrorType::kTypeError,
                    "arrow function is not a constructor");
                continue;
            }
            // 方法简写不可 new
            if (fn->is_method()) {
                std::string fn_name = fn->name().has_value() ? *fn->name() : "method";
                frame.pending_throw = make_error_value(NativeErrorType::kTypeError,
                    fn_name + " is not a constructor");
                continue;
            }
            // Generator function 不可 new
            if (fn->is_generator()) {
                std::string fn_name = fn->name().has_value() ? *fn->name() : "GeneratorFunction";
                frame.pending_throw = make_error_value(NativeErrorType::kTypeError,
                    fn_name + " is not a constructor");
                continue;
            }
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

            // Class constructor: check for direct call without new (already handled above)
            // Check for implicit derived ctor: need to call super with all args
            const auto& ctor_bc_ptr = fn->bytecode();
            bool is_implicit_derived = ctor_bc_ptr && ctor_bc_ptr->is_implicit_derived_ctor;

            auto push_res = push_call_frame(fn, instance_val, args,
                                            /*is_new=*/true, std::move(instance_copy));
            if (!push_res.is_ok()) {
                const std::string& msg = push_res.error().message();
                NativeErrorType err_type = NativeErrorType::kRangeError;
                if (msg.rfind("TypeError:", 0) == 0) err_type = NativeErrorType::kTypeError;
                frame.pending_throw = make_error_value(err_type, strip_error_prefix(msg));
                continue;
            }
            // Set new.target on the new frame to be the constructor function
            call_stack_.back().new_target_val = ctor_val;

            // Implicit derived ctor: auto-call super with all args before executing body
            if (is_implicit_derived && fn->fn_ctor_proto()) {
                // Get the rest args array (bound as $__class_impl_args__)
                // It's already bound in the frame's env. Pop all args from the rest param.
                std::vector<Value> super_args_vec(args.begin(), args.end());
                JSFunction* super_ctor_ic = fn->fn_ctor_proto();
                RcPtr<JSFunction> super_fn_ic(super_ctor_ic);
                // Create new_obj with new_target prototype
                RcPtr<JSObject> nt_proto_ic;
                if (ctor_val.is_object() && ctor_val.as_object_raw()->object_kind() == ObjectKind::kFunction) {
                    nt_proto_ic = static_cast<JSFunction*>(ctor_val.as_object_raw())->prototype_obj();
                }
                auto new_obj_ic = RcPtr<JSObject>::make();
                gc_heap_.Register(new_obj_ic.get());
                if (nt_proto_ic) new_obj_ic->set_proto(nt_proto_ic);
                else new_obj_ic->set_proto(object_prototype_);
                Value new_obj_ic_val = Value::object(ObjectPtr(new_obj_ic));

                auto push_super_ic = push_call_frame(super_fn_ic, new_obj_ic_val,
                    std::span<Value>(super_args_vec.data(), super_args_vec.size()), false);
                if (push_super_ic.is_ok()) {
                    call_stack_.back().new_target_val = ctor_val;
                    size_t super_exit_ic = call_stack_.size() - 1;
                    EvalResult super_res_ic = run(super_exit_ic);
                    if (super_res_ic.is_ok()) {
                        Value super_ret_ic = super_res_ic.value();
                        Value new_this_ic = (super_ret_ic.is_object() && !super_ret_ic.is_null())
                            ? std::move(super_ret_ic) : std::move(new_obj_ic_val);
                        // Update the implicit ctor frame's this_val and new_instance
                        auto& implicit_frame = call_stack_.back();
                        implicit_frame.this_val = new_this_ic;
                        implicit_frame.new_instance = new_this_ic;
                        implicit_frame.derived_this_initialized = true;
                        implicit_frame.fields_initialized = true;
                        // Run field initializer on the new this
                        if (fn->field_initializer()) {
                            auto fi_bc_ic = fn->field_initializer();
                            RcPtr<Environment> fi_outer_ic = fn->closure_env()
                                ? fn->closure_env() : global_env_;
                            auto fi_fn_ic = RcPtr<JSFunction>::make();
                            gc_heap_.Register(fi_fn_ic.get());
                            fi_fn_ic->set_bytecode(fi_bc_ic);
                            fi_fn_ic->set_closure_env(fi_outer_ic);
                            size_t fi_depth_ic = call_stack_.size();
                            auto fi_push_ic = push_call_frame(fi_fn_ic, new_this_ic,
                                std::span<Value>{});
                            if (fi_push_ic.is_ok()) {
                                EvalResult fi_res_ic = run(fi_depth_ic);
                                if (!fi_res_ic.is_ok() && call_stack_.size() > exit_depth) {
                                    call_stack_.back().pending_throw = make_error_value(
                                        NativeErrorType::kTypeError,
                                        strip_error_prefix(fi_res_ic.error().message()));
                                }
                            }
                        }
                    }
                    // If super failed, the frame's pending_throw will handle it
                }
            }
            break;
        }

        case Opcode::kReturn: {
            Value ret = std::move(stack.back());
            stack.pop_back();
            bool is_new = frame.is_new_call;
            // M4: derived ctor returning non-object without calling super() → ReferenceError
            if (is_new && frame.current_fn && frame.current_fn->is_derived_ctor() &&
                !frame.derived_this_initialized && !(ret.is_object() && !ret.is_null())) {
                frame.pending_throw = make_error_value(NativeErrorType::kReferenceError,
                    "Must call super constructor in derived class before returning from derived constructor");
                // put ret back and let pending_throw handler deal with it
                stack.push_back(std::move(ret));
                continue;
            }
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
            // M4: derived ctor returning undefined without calling super() → ReferenceError
            if (is_new && frame.current_fn && frame.current_fn->is_derived_ctor() &&
                !frame.derived_this_initialized) {
                frame.pending_throw = make_error_value(NativeErrorType::kReferenceError,
                    "Must call super constructor in derived class before returning from derived constructor");
                continue;
            }
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
            // Symbol on either side → TypeError
            if (lv.is_symbol() || rv.is_symbol()) {
                frame.pending_throw = make_error_value(NativeErrorType::kTypeError,
                    "Cannot convert a Symbol value to a string");
                continue;
            }
            if (lv.is_string() || rv.is_string()) {
                if (lv.is_string() && rv.is_string()) {
                    auto lsv = lv.sv();
                    auto rsv = rv.sv();
                    std::string r;
                    r.reserve(lsv.size() + rsv.size());
                    r += lsv;
                    r += rsv;
                    stack.push_back(Value::string(std::move(r)));
                } else {
                    stack.push_back(Value::string(to_string_val(lv) + to_string_val(rv)));
                }
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
            if (lv.is_symbol() || rv.is_symbol()) {
                frame.pending_throw = make_error_value(NativeErrorType::kTypeError,
                    "Cannot convert a Symbol value to a number");
                continue;
            }
            auto ln = to_number(lv); if (!ln.is_ok()) return ln;
            auto rn = to_number(rv); if (!rn.is_ok()) return rn;
            stack.push_back(Value::number(ln.value().as_number() - rn.value().as_number()));
            break;
        }

        case Opcode::kMul: {
            Value rv = std::move(stack.back()); stack.pop_back();
            Value lv = std::move(stack.back()); stack.pop_back();
            if (lv.is_symbol() || rv.is_symbol()) {
                frame.pending_throw = make_error_value(NativeErrorType::kTypeError,
                    "Cannot convert a Symbol value to a number");
                continue;
            }
            auto ln = to_number(lv); if (!ln.is_ok()) return ln;
            auto rn = to_number(rv); if (!rn.is_ok()) return rn;
            stack.push_back(Value::number(ln.value().as_number() * rn.value().as_number()));
            break;
        }

        case Opcode::kDiv: {
            Value rv = std::move(stack.back()); stack.pop_back();
            Value lv = std::move(stack.back()); stack.pop_back();
            if (lv.is_symbol() || rv.is_symbol()) {
                frame.pending_throw = make_error_value(NativeErrorType::kTypeError,
                    "Cannot convert a Symbol value to a number");
                continue;
            }
            auto ln = to_number(lv); if (!ln.is_ok()) return ln;
            auto rn = to_number(rv); if (!rn.is_ok()) return rn;
            stack.push_back(Value::number(ln.value().as_number() / rn.value().as_number()));
            break;
        }

        case Opcode::kMod: {
            Value rv = std::move(stack.back()); stack.pop_back();
            Value lv = std::move(stack.back()); stack.pop_back();
            if (lv.is_symbol() || rv.is_symbol()) {
                frame.pending_throw = make_error_value(NativeErrorType::kTypeError,
                    "Cannot convert a Symbol value to a number");
                continue;
            }
            auto ln = to_number(lv); if (!ln.is_ok()) return ln;
            auto rn = to_number(rv); if (!rn.is_ok()) return rn;
            stack.push_back(Value::number(std::fmod(ln.value().as_number(), rn.value().as_number())));
            break;
        }

        case Opcode::kPow: {
            Value rv = std::move(stack.back()); stack.pop_back();
            Value lv = std::move(stack.back()); stack.pop_back();
            if (lv.is_symbol() || rv.is_symbol()) {
                frame.pending_throw = make_error_value(NativeErrorType::kTypeError,
                    "Cannot convert a Symbol value to a number");
                continue;
            }
            auto ln = to_number(lv); if (!ln.is_ok()) return ln;
            auto rn = to_number(rv); if (!rn.is_ok()) return rn;
            stack.push_back(Value::number(std::pow(ln.value().as_number(), rn.value().as_number())));
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
            // Check [Symbol.toPrimitive] with "number" hint for objects
            if (v.is_object()) {
                RcObject* raw = v.as_object_raw();
                if (raw && raw->object_kind() != ObjectKind::kFunction) {
                    auto* obj = static_cast<JSObject*>(raw);
                    const JSObject::SymbolPropertyEntry* entry =
                        obj->find_symbol_entry(symbol_table_.well_known_to_primitive);
                    if (entry && !entry->value.is_undefined()) {
                        Value hint = Value::string("number");
                        auto prim_res = call_function_val(entry->value, v,
                                                          std::span<Value>(&hint, 1));
                        if (!prim_res.is_ok()) {
                            if (native_pending_throw_.has_value()) {
                                frame.pending_throw = std::move(*native_pending_throw_);
                                native_pending_throw_.reset();
                            } else {
                                frame.pending_throw = make_error_value(
                                    NativeErrorType::kTypeError, prim_res.error().message());
                            }
                            continue;
                        }
                        v = prim_res.value();
                    }
                }
            }
            auto n = to_number(v); if (!n.is_ok()) return n;
            stack.push_back(n.value());
            break;
        }

        case Opcode::kBitNot: {
            Value v = std::move(stack.back()); stack.pop_back();
            auto n = to_number(v); if (!n.is_ok()) return n;
            stack.push_back(Value::number(static_cast<double>(~to_int32_bits(n.value().as_number()))));
            break;
        }

        case Opcode::kNot: {
            Value v = std::move(stack.back()); stack.pop_back();
            stack.push_back(Value::boolean(!to_boolean(v)));
            break;
        }

        // ---- Bitwise ----

        case Opcode::kBitAnd: {
            Value rv = std::move(stack.back()); stack.pop_back();
            Value lv = std::move(stack.back()); stack.pop_back();
            auto ln = to_number(lv); if (!ln.is_ok()) return ln;
            auto rn = to_number(rv); if (!rn.is_ok()) return rn;
            int32_t result = to_int32_bits(ln.value().as_number()) & to_int32_bits(rn.value().as_number());
            stack.push_back(Value::number(static_cast<double>(result)));
            break;
        }

        case Opcode::kBitOr: {
            Value rv = std::move(stack.back()); stack.pop_back();
            Value lv = std::move(stack.back()); stack.pop_back();
            auto ln = to_number(lv); if (!ln.is_ok()) return ln;
            auto rn = to_number(rv); if (!rn.is_ok()) return rn;
            int32_t result = to_int32_bits(ln.value().as_number()) | to_int32_bits(rn.value().as_number());
            stack.push_back(Value::number(static_cast<double>(result)));
            break;
        }

        case Opcode::kBitXor: {
            Value rv = std::move(stack.back()); stack.pop_back();
            Value lv = std::move(stack.back()); stack.pop_back();
            auto ln = to_number(lv); if (!ln.is_ok()) return ln;
            auto rn = to_number(rv); if (!rn.is_ok()) return rn;
            int32_t result = to_int32_bits(ln.value().as_number()) ^ to_int32_bits(rn.value().as_number());
            stack.push_back(Value::number(static_cast<double>(result)));
            break;
        }

        case Opcode::kShl: {
            Value rv = std::move(stack.back()); stack.pop_back();
            Value lv = std::move(stack.back()); stack.pop_back();
            auto ln = to_number(lv); if (!ln.is_ok()) return ln;
            auto rn = to_number(rv); if (!rn.is_ok()) return rn;
            uint32_t shift = to_uint32_bits(rn.value().as_number()) & 0x1F;
            int32_t result = to_int32_bits(ln.value().as_number()) << shift;
            stack.push_back(Value::number(static_cast<double>(result)));
            break;
        }

        case Opcode::kSar: {
            Value rv = std::move(stack.back()); stack.pop_back();
            Value lv = std::move(stack.back()); stack.pop_back();
            auto ln = to_number(lv); if (!ln.is_ok()) return ln;
            auto rn = to_number(rv); if (!rn.is_ok()) return rn;
            uint32_t shift = to_uint32_bits(rn.value().as_number()) & 0x1F;
            int32_t result = to_int32_bits(ln.value().as_number()) >> shift;
            stack.push_back(Value::number(static_cast<double>(result)));
            break;
        }

        case Opcode::kShr: {
            Value rv = std::move(stack.back()); stack.pop_back();
            Value lv = std::move(stack.back()); stack.pop_back();
            auto ln = to_number(lv); if (!ln.is_ok()) return ln;
            auto rn = to_number(rv); if (!rn.is_ok()) return rn;
            uint32_t shift = to_uint32_bits(rn.value().as_number()) & 0x1F;
            uint32_t result = to_uint32_bits(ln.value().as_number()) >> shift;
            stack.push_back(Value::number(static_cast<double>(result)));
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
            case ValueKind::Symbol:    type_str = "symbol";    break;
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
            case ValueKind::Symbol:    type_str = "symbol";    break;
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
            // Check [Symbol.hasInstance] on the constructor
            {
                std::string sym_key = "__pfsym_" +
                    std::to_string(symbol_table_.well_known_has_instance) + "__";
                Value has_inst_val = ctor_fn->get_property(sym_key);
                if (!has_inst_val.is_undefined()) {
                    Value obj_copy = obj_val;
                    auto result = call_function_val(has_inst_val, ctor_val,
                                                   std::span<Value>(&obj_copy, 1));
                    if (!result.is_ok()) {
                        if (native_pending_throw_.has_value()) {
                            frame.pending_throw = std::move(*native_pending_throw_);
                            native_pending_throw_.reset();
                        } else {
                            frame.pending_throw = make_error_value(
                                NativeErrorType::kTypeError, result.error().message());
                        }
                        continue;
                    }
                    stack.push_back(Value::boolean(to_boolean(result.value())));
                    break;
                }
            }
            const RcPtr<JSObject>& ctor_proto = ctor_fn->prototype_obj();
            if (!ctor_proto) {
                stack.push_back(Value::boolean(false));
                break;
            }
            // Walk the prototype chain of obj_val
            RcObject* cur_raw = obj_val.as_object_raw();
            bool found = false;
            while (cur_raw) {
                ObjectKind k = cur_raw->object_kind();
                if (k != ObjectKind::kOrdinary && k != ObjectKind::kArray &&
                    k != ObjectKind::kRegExp && k != ObjectKind::kStringObject &&
                    k != ObjectKind::kBooleanObject && k != ObjectKind::kGenerator &&
                    k != ObjectKind::kMap && k != ObjectKind::kSet &&
                    k != ObjectKind::kWeakMap && k != ObjectKind::kWeakSet) break;
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

        case Opcode::kIn: {
            Value rhs = std::move(stack.back()); stack.pop_back();
            Value lhs = std::move(stack.back()); stack.pop_back();
            if (!rhs.is_object()) {
                frame.pending_throw = make_error_value(NativeErrorType::kTypeError,
                    "Right-hand side of 'in' must be an object");
                continue;
            }
            RcObject* raw = rhs.as_object_raw();
            if (!raw) {
                frame.pending_throw = make_error_value(NativeErrorType::kTypeError,
                    "Right-hand side of 'in' must be an object");
                continue;
            }
            bool found;
            if (raw->object_kind() == ObjectKind::kFunction) {
                auto* fn = static_cast<JSFunction*>(raw);
                found = !lhs.is_symbol() && fn->has_property(to_string_val(lhs));
            } else {
                auto* obj = static_cast<JSObject*>(raw);
                if (lhs.is_symbol()) {
                    found = obj->has_property_by_symbol(lhs.as_symbol_id());
                } else {
                    found = obj->has_property(to_string_val(lhs));
                }
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

        case Opcode::kJumpIfNotNullish: {
            int32_t offset = read_i32(bc, pc);
            Value v = std::move(stack.back()); stack.pop_back();
            if (!v.is_null() && !v.is_undefined()) {
                pc = static_cast<size_t>(static_cast<int64_t>(pc) + offset);
            }
            break;
        }

        // ---- Stack ----

        case Opcode::kPop:
            stack.pop_back();
            break;

        case Opcode::kDup: {
            Value copy = stack.back();
            stack.push_back(std::move(copy));
            break;
        }

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

        case Opcode::kGetNewTarget: {
            // new.target: push the new_target_val stored in the current CallFrame
            stack.push_back(frame.new_target_val);
            break;
        }

        case Opcode::kMakeClass: {
            // Stack: [super_or_undef]
            // fn_idx: constructor BytecodeFunction index
            uint16_t fn_idx = read_u16(bc, pc);
            const auto& fn_bc = bc->functions[fn_idx];

            Value super_val = std::move(stack.back());
            stack.pop_back();

            // Validate super class if provided
            JSFunction* super_fn = nullptr;
            RcPtr<JSObject> super_proto;
            bool vm_extends_null = false;  // M3: class C extends null {}
            if (!super_val.is_undefined()) {
                if (super_val.is_null()) {
                    // M3: extends null — proto gets null prototype, no super ctor
                    vm_extends_null = true;
                } else if (!super_val.is_object() ||
                    super_val.as_object_raw()->object_kind() != ObjectKind::kFunction) {
                    frame.pending_throw = make_error_value(NativeErrorType::kTypeError,
                        "Class extends value is not a constructor or null");
                    continue;
                } else {
                    super_fn = static_cast<JSFunction*>(super_val.as_object_raw());
                    super_proto = super_fn->prototype_obj();
                }
            }

            // Create ctor function
            auto fn = RcPtr<JSFunction>::make();
            fn->set_name(fn_bc->name);
            fn->set_params(fn_bc->params);
            fn->set_rest_param(fn_bc->rest_param);
            fn->set_property("length", Value::number(static_cast<double>(fn_bc->length_count)));
            fn->set_property("name", Value::string(fn_bc->name.value_or("")));
            if (fn_bc->param_defs) fn->set_param_defs(fn_bc->param_defs);
            fn->set_bytecode(fn_bc);
            fn->set_closure_env(env);
            fn->set_is_class_ctor(true);
            // M3: extends null → treat as base class (no super ctor to call)
            fn->set_is_derived_ctor(fn_bc->is_derived_ctor && !vm_extends_null);
            if (super_fn) {
                fn->set_fn_ctor_proto(super_fn);
            }
            if (fn_bc->field_initializer) {
                fn->set_field_initializer(fn_bc->field_initializer);
            }

            // Create prototype object
            auto proto_obj = RcPtr<JSObject>::make();
            if (vm_extends_null) {
                // M3: extends null → C.prototype.[[Prototype]] = null (empty RcPtr)
                proto_obj->set_proto(RcPtr<JSObject>{});
            } else if (super_proto) {
                proto_obj->set_proto(super_proto);
            } else {
                proto_obj->set_proto(object_prototype_);
            }
            proto_obj->set_constructor_property(fn.get());
            fn->set_prototype_obj(proto_obj);

            gc_heap_.Register(fn.get());
            gc_heap_.Register(proto_obj.get());
            stack.push_back(Value::object(ObjectPtr(fn)));
            break;
        }

        case Opcode::kSuperCall: {
            // super(...args) — call parent class constructor
            uint8_t argc = read_u8(bc, pc);
            constexpr int kSmallArgBuf2 = 8;
            Value small_buf2[kSmallArgBuf2];
            std::vector<Value> large_buf2;
            Value* arg_data2;
            if (argc <= kSmallArgBuf2) {
                for (int i = argc - 1; i >= 0; --i)
                    small_buf2[i] = std::move(stack[stack.size() - argc + i]);
                stack.resize(stack.size() - argc);
                arg_data2 = small_buf2;
            } else {
                large_buf2.resize(argc);
                for (int i = argc - 1; i >= 0; --i)
                    large_buf2[i] = std::move(stack[stack.size() - argc + i]);
                stack.resize(stack.size() - argc);
                arg_data2 = large_buf2.data();
            }
            std::span<Value> super_args(arg_data2, argc);

            // Get current function from frame
            JSFunction* active_fn = frame.current_fn;
            if (!active_fn) {
                frame.pending_throw = make_error_value(NativeErrorType::kTypeError,
                    "super() called outside a constructor");
                continue;
            }
            JSFunction* super_ctor = active_fn->fn_ctor_proto();
            if (!super_ctor) {
                frame.pending_throw = make_error_value(NativeErrorType::kTypeError,
                    "super() called in a non-derived constructor");
                continue;
            }

            // new.target prototype to create the instance
            Value new_target = frame.new_target_val;
            RcPtr<JSObject> new_proto;
            if (new_target.is_object() &&
                new_target.as_object_raw()->object_kind() == ObjectKind::kFunction) {
                auto* nt_fn = static_cast<JSFunction*>(new_target.as_object_raw());
                new_proto = nt_fn->prototype_obj();
            }

            auto new_obj = RcPtr<JSObject>::make();
            gc_heap_.Register(new_obj.get());
            if (new_proto) {
                new_obj->set_proto(new_proto);
            } else {
                new_obj->set_proto(object_prototype_);
            }
            Value new_obj_val = Value::object(ObjectPtr(new_obj));

            // Call super_ctor with new_obj as 'this'
            RcPtr<JSFunction> super_fn_rc(super_ctor);
            auto push_super = push_call_frame(super_fn_rc, new_obj_val, super_args, false);
            if (!push_super.is_ok()) {
                const std::string& msg = push_super.error().message();
                NativeErrorType err_type = NativeErrorType::kTypeError;
                frame.pending_throw = make_error_value(err_type, strip_error_prefix(msg));
                continue;
            }
            // Pass new_target down to super ctor
            call_stack_.back().new_target_val = new_target;

            size_t super_exit_depth = call_stack_.size() - 1;
            EvalResult super_result = run(super_exit_depth);

            // Refresh frame reference after run()
            auto& cur_frame = call_stack_.back();
            if (!super_result.is_ok()) {
                cur_frame.pending_throw = Value::string(super_result.error().message());
                continue;
            }
            Value super_ret = super_result.value();
            // If super returned an object, use it; otherwise use new_obj
            Value new_this = (super_ret.is_object() && !super_ret.is_null())
                ? std::move(super_ret)
                : std::move(new_obj_val);
            cur_frame.this_val = new_this;
            cur_frame.new_instance = new_this;  // update new_instance so kReturn uses the correct object
            cur_frame.derived_this_initialized = true;
            // Derived class constructor: run instance field initializer after super() returns
            if (!cur_frame.fields_initialized && cur_frame.current_fn &&
                cur_frame.current_fn->field_initializer()) {
                cur_frame.fields_initialized = true;
                Value fi_this2 = cur_frame.this_val;
                auto fi_bc2 = cur_frame.current_fn->field_initializer();
                RcPtr<Environment> fi_outer2 = cur_frame.current_fn->closure_env()
                    ? cur_frame.current_fn->closure_env() : global_env_;
                auto fi_fn2 = RcPtr<JSFunction>::make();
                gc_heap_.Register(fi_fn2.get());
                fi_fn2->set_bytecode(fi_bc2);
                fi_fn2->set_closure_env(fi_outer2);
                size_t fi_depth2 = call_stack_.size();
                auto fi_push2 = push_call_frame(fi_fn2, fi_this2, std::span<Value>{});
                if (!fi_push2.is_ok()) {
                    // Re-fetch cur_frame after potential realloc
                    call_stack_.back().pending_throw = make_error_value(NativeErrorType::kTypeError,
                        strip_error_prefix(fi_push2.error().message()));
                    continue;
                }
                EvalResult fi_res2 = run(fi_depth2);
                if (!fi_res2.is_ok() && call_stack_.size() > exit_depth) {
                    call_stack_.back().pending_throw = make_error_value(NativeErrorType::kTypeError,
                        strip_error_prefix(fi_res2.error().message()));
                    continue;
                }
            }
            // super() returns undefined
            call_stack_.back().stack.push_back(Value::undefined());
            break;
        }

        case Opcode::kSuperGetProp: {
            uint16_t name_idx = read_u16(bc, pc);
            const std::string& prop_name = bc->names[name_idx];
            JSFunction* cur_fn = frame.current_fn;
            if (!cur_fn || !cur_fn->home_object()) {
                frame.pending_throw = make_error_value(NativeErrorType::kTypeError,
                    "super property access outside method");
                continue;
            }
            JSObject* home = cur_fn->home_object();
            // M5: walk prototype chain starting from H.__proto__, call accessor getter if found
            RcObject* proto_raw = home->proto().get();
            Value result_val = Value::undefined();
            bool accessor_error = false;
            while (proto_raw) {
                if (proto_raw->object_kind() == ObjectKind::kOrdinary ||
                    proto_raw->object_kind() == ObjectKind::kArray) {
                    auto* proto_obj = static_cast<JSObject*>(proto_raw);
                    const auto* entry = proto_obj->get_own_entry(prop_name);
                    if (entry) {
                        if ((entry->flags & kPropIsAccessor) && !entry->getter.is_undefined()) {
                            // Call getter with current this (frame.this_val) as receiver
                            Value getter_copy = entry->getter;
                            if (!getter_copy.is_object() ||
                                getter_copy.as_object_raw()->object_kind() != ObjectKind::kFunction) {
                                break;
                            }
                            auto* getter_fn = static_cast<JSFunction*>(getter_copy.as_object_raw());
                            auto getter_fn_rc = RcPtr<JSFunction>(getter_fn);
                            size_t getter_exit = call_stack_.size();
                            auto push_res2 = push_call_frame(getter_fn_rc, frame.this_val, std::span<Value>{});
                            if (!push_res2.is_ok()) {
                                frame.pending_throw = make_error_value(NativeErrorType::kTypeError,
                                    "super getter call failed");
                                accessor_error = true;
                                break;
                            }
                            if (!push_res2.value().is_undefined()) {
                                result_val = push_res2.value();
                            } else {
                                auto getter_res = run(getter_exit);
                                if (!getter_res.is_ok()) {
                                    frame.pending_throw = make_error_value(NativeErrorType::kTypeError,
                                        "super getter threw");
                                    accessor_error = true;
                                    break;
                                }
                                result_val = getter_res.value();
                            }
                        } else {
                            result_val = entry->value;
                        }
                        break;
                    }
                    proto_raw = proto_obj->proto().get();
                } else {
                    break;
                }
            }
            if (accessor_error) continue;
            stack.push_back(std::move(result_val));
            break;
        }

        case Opcode::kSuperGetElem: {
            Value key = std::move(stack.back());
            stack.pop_back();
            std::string key_str = to_string_val(key);
            JSFunction* cur_fn2 = frame.current_fn;
            if (!cur_fn2 || !cur_fn2->home_object()) {
                frame.pending_throw = make_error_value(NativeErrorType::kTypeError,
                    "super property access outside method");
                continue;
            }
            JSObject* home2 = cur_fn2->home_object();
            // M5: walk prototype chain starting from H.__proto__, call accessor getter if found
            RcObject* proto_raw2 = home2->proto().get();
            Value result_val2 = Value::undefined();
            bool accessor_error2 = false;
            while (proto_raw2) {
                if (proto_raw2->object_kind() == ObjectKind::kOrdinary ||
                    proto_raw2->object_kind() == ObjectKind::kArray) {
                    auto* proto_obj2 = static_cast<JSObject*>(proto_raw2);
                    const auto* entry2 = proto_obj2->get_own_entry(key_str);
                    if (entry2) {
                        if ((entry2->flags & kPropIsAccessor) && !entry2->getter.is_undefined()) {
                            Value getter_copy2 = entry2->getter;
                            if (!getter_copy2.is_object() ||
                                getter_copy2.as_object_raw()->object_kind() != ObjectKind::kFunction) {
                                break;
                            }
                            auto* getter_fn2 = static_cast<JSFunction*>(getter_copy2.as_object_raw());
                            auto getter_fn_rc2 = RcPtr<JSFunction>(getter_fn2);
                            size_t getter_exit2 = call_stack_.size();
                            auto push_res3 = push_call_frame(getter_fn_rc2, frame.this_val, std::span<Value>{});
                            if (!push_res3.is_ok()) {
                                frame.pending_throw = make_error_value(NativeErrorType::kTypeError,
                                    "super getter call failed");
                                accessor_error2 = true;
                                break;
                            }
                            if (!push_res3.value().is_undefined()) {
                                result_val2 = push_res3.value();
                            } else {
                                auto getter_res2 = run(getter_exit2);
                                if (!getter_res2.is_ok()) {
                                    frame.pending_throw = make_error_value(NativeErrorType::kTypeError,
                                        "super getter threw");
                                    accessor_error2 = true;
                                    break;
                                }
                                result_val2 = getter_res2.value();
                            }
                        } else {
                            result_val2 = entry2->value;
                        }
                        break;
                    }
                    proto_raw2 = proto_obj2->proto().get();
                } else {
                    break;
                }
            }
            if (accessor_error2) continue;
            stack.push_back(std::move(result_val2));
            break;
        }

        case Opcode::kDefineClassMethod: {
            // Stack: [obj, fn] — define non-enumerable method on obj, push fn back
            uint16_t idx = read_u16(bc, pc);
            const std::string& name = bc->names[idx];
            Value fn_val = std::move(stack.back());
            stack.pop_back();
            Value obj_val = std::move(stack.back());
            stack.pop_back();
            if (obj_val.is_object()) {
                RcObject* raw = obj_val.as_object_raw();
                if (raw->object_kind() == ObjectKind::kOrdinary ||
                    raw->object_kind() == ObjectKind::kArray) {
                    auto* obj = static_cast<JSObject*>(raw);
                    PropDesc desc;
                    desc.value = fn_val;
                    desc.writable = true;
                    desc.enumerable = false;
                    desc.configurable = true;
                    obj->define_property(name, desc);
                }
            }
            stack.push_back(std::move(fn_val));
            break;
        }

        case Opcode::kDefineComputedClassMethod: {
            // Stack: [obj, key, fn] — define non-enumerable method on obj with computed key
            Value fn_val = std::move(stack.back());
            stack.pop_back();
            Value key_val = std::move(stack.back());
            stack.pop_back();
            Value obj_val = std::move(stack.back());
            stack.pop_back();
            std::string key_str = to_string_val(key_val);
            if (obj_val.is_object()) {
                RcObject* raw = obj_val.as_object_raw();
                if (raw->object_kind() == ObjectKind::kOrdinary ||
                    raw->object_kind() == ObjectKind::kArray) {
                    auto* obj = static_cast<JSObject*>(raw);
                    PropDesc desc;
                    desc.value = fn_val;
                    desc.writable = true;
                    desc.enumerable = false;
                    desc.configurable = true;
                    obj->define_property(key_str, desc);
                }
            }
            stack.push_back(std::move(fn_val));
            break;
        }

        case Opcode::kSetHomeObject: {
            // Stack: [..., obj, fn]  — sets fn->home_object = obj, stack unchanged
            if (stack.size() < 2) break;
            Value& fn_val2 = stack[stack.size() - 1];
            Value& obj_val2 = stack[stack.size() - 2];
            if (fn_val2.is_object() && fn_val2.as_object_raw()->object_kind() == ObjectKind::kFunction) {
                auto* fn2 = static_cast<JSFunction*>(fn_val2.as_object_raw());
                if (obj_val2.is_object()) {
                    RcObject* obj_raw2 = obj_val2.as_object_raw();
                    if (obj_raw2->object_kind() == ObjectKind::kOrdinary ||
                        obj_raw2->object_kind() == ObjectKind::kArray) {
                        fn2->set_home_object(static_cast<JSObject*>(obj_raw2));
                    }
                }
            }
            break;
        }

        case Opcode::kSetHomeObjectStatic: {
            // Stack: [..., ctor, fn]  — no-op for now.
            // static super requires home_object = ctor (JSFunction), but home_object_ is JSObject*.
            // Until the architecture is extended, static super will throw TypeError at runtime.
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

        case Opcode::kYield: {
            // Yield: pop TOS as yield value, save frame to owning generator, signal yield exit.
            Value yield_val = std::move(stack.back());
            stack.pop_back();

            // Find the owning generator
            JSGeneratorObject* gen_raw = frame.owning_generator;
            if (!gen_raw) {
                // Should not happen; yield outside generator context
                frame.pending_throw = make_error_value(NativeErrorType::kTypeError,
                    "yield outside generator");
                continue;
            }

            // Move current frame into the generator's suspended_frame_
            gen_raw->state_ = GeneratorState::kSuspendedYield;
            gen_raw->suspended_frame_ = std::make_unique<CallFrame>(std::move(call_stack_.back()));
            call_stack_.pop_back();
            call_depth_--;

            // Build {value: yield_val, done: false} result
            auto result_obj = RcPtr<JSObject>::make();
            gc_heap_.Register(result_obj.get());
            result_obj->set_proto(object_prototype_);
            result_obj->set_property("value", std::move(yield_val));
            result_obj->set_property("done", Value::boolean(false));

            vm_generator_yielded_ = true;
            vm_generator_yield_value_ = Value::object(ObjectPtr(result_obj));

            goto suspend_exit;
        }

        case Opcode::kToString: {
            Value top = std::move(stack.back());
            stack.pop_back();
            if (!top.is_string()) {
                // Check [Symbol.toPrimitive] with "string" hint for objects
                if (top.is_object()) {
                    RcObject* raw = top.as_object_raw();
                    if (raw && raw->object_kind() != ObjectKind::kFunction) {
                        auto* obj = static_cast<JSObject*>(raw);
                        const JSObject::SymbolPropertyEntry* entry =
                            obj->find_symbol_entry(symbol_table_.well_known_to_primitive);
                        if (entry && !entry->value.is_undefined()) {
                            Value hint = Value::string("string");
                            auto prim_res = call_function_val(entry->value, top,
                                                              std::span<Value>(&hint, 1));
                            if (!prim_res.is_ok()) {
                                if (native_pending_throw_.has_value()) {
                                    frame.pending_throw = std::move(*native_pending_throw_);
                                    native_pending_throw_.reset();
                                } else {
                                    frame.pending_throw = make_error_value(
                                        NativeErrorType::kTypeError, prim_res.error().message());
                                }
                                continue;
                            }
                            top = prim_res.value();
                        }
                    }
                }
                stack.push_back(Value::string(to_string_val(top)));
            } else {
                stack.push_back(std::move(top));
            }
            break;
        }

        case Opcode::kNewRegExp: {
            uint16_t pattern_idx = read_u16(bc, pc);
            uint16_t flags_idx = read_u16(bc, pc);
            const std::string& pattern = bc->constants[pattern_idx].as_string();
            const std::string& flags_str = bc->constants[flags_idx].as_string();
            auto res = vm_make_regexp(pattern, flags_str);
            if (!res.is_ok()) {
                if (native_pending_throw_.has_value()) {
                    frame.pending_throw = std::move(*native_pending_throw_);
                    native_pending_throw_.reset();
                } else {
                    frame.pending_throw = make_error_value(NativeErrorType::kSyntaxError,
                        res.error().message());
                }
                continue;
            }
            stack.push_back(res.value());
            break;
        }

        case Opcode::kDeleteProp: {
            uint16_t idx = read_u16(bc, pc);
            const std::string& name = bc->names[idx];
            Value obj_val = std::move(stack.back());
            stack.pop_back();
            if (!obj_val.is_object()) {
                stack.push_back(Value::boolean(true));
                break;
            }
            RcObject* raw_dp = obj_val.as_object_raw();
            if (!raw_dp || (raw_dp->object_kind() != ObjectKind::kOrdinary &&
                            raw_dp->object_kind() != ObjectKind::kArray &&
                            raw_dp->object_kind() != ObjectKind::kStringObject &&
                            raw_dp->object_kind() != ObjectKind::kBooleanObject)) {
                stack.push_back(Value::boolean(true));
                break;
            }
            auto* obj_dp = static_cast<JSObject*>(raw_dp);
            stack.push_back(Value::boolean(obj_dp->delete_property(name)));
            break;
        }

        case Opcode::kDeleteElem: {
            Value key_val = std::move(stack.back());
            stack.pop_back();
            Value obj_val = std::move(stack.back());
            stack.pop_back();
            if (!obj_val.is_object()) {
                stack.push_back(Value::boolean(true));
                break;
            }
            RcObject* raw_de = obj_val.as_object_raw();
            if (!raw_de || (raw_de->object_kind() != ObjectKind::kOrdinary &&
                            raw_de->object_kind() != ObjectKind::kArray &&
                            raw_de->object_kind() != ObjectKind::kStringObject &&
                            raw_de->object_kind() != ObjectKind::kBooleanObject)) {
                stack.push_back(Value::boolean(true));
                break;
            }
            auto* obj_de = static_cast<JSObject*>(raw_de);
            std::string key_str = to_string_val(key_val);
            stack.push_back(Value::boolean(obj_de->delete_property(key_str)));
            break;
        }

        case Opcode::kDeleteVar: {
            uint16_t idx = read_u16(bc, pc);
            const std::string& name = bc->names[idx];
            // TODO: strict mode Early Error (SyntaxError for delete of unqualified identifier)
            bool deleted = frame.env->delete_binding(name);
            stack.push_back(Value::boolean(deleted));
            break;
        }

        case Opcode::kForInStart: {
            Value obj_val = std::move(stack.back());
            stack.pop_back();
            auto iter = RcPtr<ForInIterator>::make();
            gc_heap_.Register(iter.get());
            if (!obj_val.is_null() && !obj_val.is_undefined() && obj_val.is_object()) {
                // Guard: only JSObject subclasses can be safely cast and enumerated.
                ObjectKind k = obj_val.as_object_raw()->object_kind();
                if (k == ObjectKind::kOrdinary || k == ObjectKind::kArray ||
                    k == ObjectKind::kRegExp || k == ObjectKind::kStringObject ||
                    k == ObjectKind::kBooleanObject) {
                    JSObject* obj = static_cast<JSObject*>(obj_val.as_object_raw());
                    for (const auto& key : obj->enumerate_properties()) {
                        iter->keys_.push_back(Value::string(key));
                    }
                }
            }
            stack.push_back(Value::object(ObjectPtr(iter)));
            break;
        }

        case Opcode::kForInNext: {
            // Peek the iterator (do not pop)
            ForInIterator* iter = static_cast<ForInIterator*>(stack.back().as_object_raw());
            if (iter->index_ >= iter->keys_.size()) {
                stack.push_back(Value::undefined());
                stack.push_back(Value::boolean(true));
            } else {
                stack.push_back(iter->keys_[iter->index_++]);
                stack.push_back(Value::boolean(false));
            }
            break;
        }

        case Opcode::kForOfStart: {
            Value iterable = std::move(stack.back());
            stack.pop_back();

            if (iterable.is_null() || iterable.is_undefined()) {
                frame.pending_throw = make_error_value(NativeErrorType::kTypeError, "not iterable");
                continue;
            }

            // Fast path: plain string → StringIterator
            if (iterable.is_string()) {
                auto iter = RcPtr<StringIterator>::make();
                iter->string_val_ = iterable;
                iter->byte_pos_ = 0;
                gc_heap_.Register(iter.get());
                stack.push_back(Value::object(ObjectPtr(iter)));
                break;
            }

            // Fast path: kArray → ArrayIterator
            if (iterable.is_object()) {
                RcObject* raw = iterable.as_object_raw();
                if (raw->object_kind() == ObjectKind::kArray) {
                    auto iter = RcPtr<ArrayIterator>::make();
                    iter->array_ref_ = iterable;
                    iter->index_ = 0;
                    gc_heap_.Register(iter.get());
                    stack.push_back(Value::object(ObjectPtr(iter)));
                    break;
                }
            }

            // Slow path: look up [Symbol.iterator] on the object's prototype chain
            Value iterator_factory = Value::undefined();
            if (iterable.is_object()) {
                RcObject* raw = iterable.as_object_raw();
                if (raw->object_kind() == ObjectKind::kOrdinary ||
                    raw->object_kind() == ObjectKind::kRegExp ||
                    raw->object_kind() == ObjectKind::kStringObject ||
                    raw->object_kind() == ObjectKind::kBooleanObject ||
                    raw->object_kind() == ObjectKind::kForOfIterator ||
                    raw->object_kind() == ObjectKind::kGenerator ||
                    raw->object_kind() == ObjectKind::kMap ||
                    raw->object_kind() == ObjectKind::kSet) {
                    iterator_factory =
                        static_cast<JSObject*>(raw)->get_property_by_symbol(symbol_table_.well_known_iterator);
                }
            }

            if (iterator_factory.is_undefined() || iterator_factory.is_null()) {
                frame.pending_throw = make_error_value(NativeErrorType::kTypeError, "not iterable");
                continue;
            }

            auto iterator_res = call_function_val(iterator_factory, iterable, {});
            if (!iterator_res.is_ok()) {
                const std::string& msg = iterator_res.error().message();
                if (msg == "__qppjs_pending_throw__" && native_pending_throw_.has_value()) {
                    frame.pending_throw = std::move(*native_pending_throw_);
                    native_pending_throw_ = std::nullopt;
                } else {
                    NativeErrorType err_type = NativeErrorType::kTypeError;
                    if (msg.rfind("RangeError:", 0) == 0) err_type = NativeErrorType::kRangeError;
                    if (msg.rfind("ReferenceError:", 0) == 0) err_type = NativeErrorType::kReferenceError;
                    if (msg.rfind("SyntaxError:", 0) == 0) err_type = NativeErrorType::kSyntaxError;
                    frame.pending_throw = make_error_value(err_type, strip_error_prefix(msg));
                }
                continue;
            }

            Value iterator_val = iterator_res.value();
            if (!iterator_val.is_object()) {
                frame.pending_throw = make_error_value(NativeErrorType::kTypeError,
                                                       "iterator must be an object");
                continue;
            }

            // Check if the factory returned a fast-path iterator
            RcObject* raw_iter = iterator_val.as_object_raw();
            ObjectKind iter_kind = raw_iter->object_kind();
            if (iter_kind == ObjectKind::kArrayIterator || iter_kind == ObjectKind::kStringIterator) {
                stack.push_back(std::move(iterator_val));
                break;
            }

            Value next_method = Value::undefined();
            if (iter_kind == ObjectKind::kOrdinary || iter_kind == ObjectKind::kArray ||
                iter_kind == ObjectKind::kRegExp || iter_kind == ObjectKind::kStringObject ||
                iter_kind == ObjectKind::kBooleanObject || iter_kind == ObjectKind::kGenerator ||
                iter_kind == ObjectKind::kMap || iter_kind == ObjectKind::kSet) {
                next_method = static_cast<JSObject*>(raw_iter)->get_property("next");
            }

            auto iter = RcPtr<ForOfIterator>::make();
            iter->iterator_ = iterator_val;
            iter->next_method_ = next_method;
            gc_heap_.Register(iter.get());
            stack.push_back(Value::object(ObjectPtr(iter)));
            break;
        }

        case Opcode::kForOfNext: {
            RcObject* raw_iter = stack.back().as_object_raw();
            ObjectKind kind = raw_iter->object_kind();

            if (kind == ObjectKind::kForOfIterator) {
                auto* iter = static_cast<ForOfIterator*>(raw_iter);
                if (iter->done_) {
                    stack.push_back(Value::undefined());
                    stack.push_back(Value::boolean(true));
                    break;
                }

                auto next_res = call_function_val(iter->next_method_, iter->iterator_, {});
                if (!next_res.is_ok()) {
                    const std::string& msg = next_res.error().message();
                    if (msg == "__qppjs_pending_throw__" && native_pending_throw_.has_value()) {
                        frame.pending_throw = std::move(*native_pending_throw_);
                        native_pending_throw_ = std::nullopt;
                    } else {
                        NativeErrorType err_type = NativeErrorType::kTypeError;
                        if (msg.rfind("RangeError:", 0) == 0) err_type = NativeErrorType::kRangeError;
                        if (msg.rfind("ReferenceError:", 0) == 0) err_type = NativeErrorType::kReferenceError;
                        if (msg.rfind("SyntaxError:", 0) == 0) err_type = NativeErrorType::kSyntaxError;
                        frame.pending_throw = make_error_value(err_type, strip_error_prefix(msg));
                    }
                    continue;
                }

                Value result_val = next_res.value();
                if (!result_val.is_object()) {
                    frame.pending_throw = make_error_value(NativeErrorType::kTypeError,
                                                           "iterator result must be an object");
                    continue;
                }

                Value done_val = Value::undefined();
                Value value = Value::undefined();
                RcObject* raw_result = result_val.as_object_raw();
                ObjectKind result_kind = raw_result->object_kind();
                if (result_kind == ObjectKind::kOrdinary || result_kind == ObjectKind::kArray ||
                    result_kind == ObjectKind::kRegExp || result_kind == ObjectKind::kStringObject ||
                    result_kind == ObjectKind::kBooleanObject || result_kind == ObjectKind::kGenerator ||
                    result_kind == ObjectKind::kMap || result_kind == ObjectKind::kSet) {
                    auto* obj = static_cast<JSObject*>(raw_result);
                    done_val = obj->get_property("done");
                    value = obj->get_property("value");
                }

                bool done = to_boolean(done_val);
                if (done) {
                    iter->done_ = true;
                    stack.push_back(Value::undefined());
                    stack.push_back(Value::boolean(true));
                } else {
                    stack.push_back(std::move(value));
                    stack.push_back(Value::boolean(false));
                }
                break;
            }

            if (kind == ObjectKind::kArrayIterator) {
                auto* iter = static_cast<ArrayIterator*>(raw_iter);
                if (iter->done_) {
                    stack.push_back(Value::undefined());
                    stack.push_back(Value::boolean(true));
                    break;
                }
                if (!iter->array_ref_.is_object() ||
                    iter->array_ref_.as_object_raw()->object_kind() != ObjectKind::kArray) {
                    iter->done_ = true;
                    stack.push_back(Value::undefined());
                    stack.push_back(Value::boolean(true));
                    break;
                }
                auto* arr = static_cast<JSObject*>(iter->array_ref_.as_object_raw());
                if (iter->index_ >= arr->array_length_) {
                    iter->done_ = true;
                    stack.push_back(Value::undefined());
                    stack.push_back(Value::boolean(true));
                } else {
                    auto elem_it = arr->elements_.find(iter->index_);
                    Value value = elem_it != arr->elements_.end() ? elem_it->second : Value::undefined();
                    ++iter->index_;
                    stack.push_back(std::move(value));
                    stack.push_back(Value::boolean(false));
                }
                break;
            }

            if (kind == ObjectKind::kStringIterator) {
                auto* iter = static_cast<StringIterator*>(raw_iter);
                if (iter->done_) {
                    stack.push_back(Value::undefined());
                    stack.push_back(Value::boolean(true));
                    break;
                }
                std::string_view sv = iter->string_val_.sv();
                if (iter->byte_pos_ >= sv.size()) {
                    iter->done_ = true;
                    stack.push_back(Value::undefined());
                    stack.push_back(Value::boolean(true));
                } else {
                    size_t start = iter->byte_pos_;
                    unsigned char c0 = static_cast<unsigned char>(sv[start]);
                    size_t cp_bytes = (c0 < 0x80) ? 1 : (c0 < 0xE0) ? 2 : (c0 < 0xF0) ? 3 : 4;
                    if (start + cp_bytes > sv.size()) cp_bytes = sv.size() - start;
                    iter->byte_pos_ += static_cast<uint32_t>(cp_bytes);
                    stack.push_back(Value::string(std::string(sv.substr(start, cp_bytes))));
                    stack.push_back(Value::boolean(false));
                }
                break;
            }

            frame.pending_throw = make_error_value(NativeErrorType::kTypeError, "invalid for-of iterator");
            continue;
        }

        case Opcode::kIteratorClose: {
            Value iter_val = std::move(stack.back());
            stack.pop_back();
            if (!iter_val.is_object()) {
                break;
            }
            RcObject* raw_iter = iter_val.as_object_raw();
            if (!raw_iter || raw_iter->object_kind() != ObjectKind::kForOfIterator) {
                break;
            }

            auto* iter = static_cast<ForOfIterator*>(raw_iter);
            if (iter->done_) {
                break;
            }
            iter->done_ = true;

            if (!iter->iterator_.is_object()) {
                break;
            }
            RcObject* raw_obj = iter->iterator_.as_object_raw();
            if (raw_obj->object_kind() != ObjectKind::kOrdinary &&
                raw_obj->object_kind() != ObjectKind::kArray &&
                raw_obj->object_kind() != ObjectKind::kRegExp &&
                raw_obj->object_kind() != ObjectKind::kStringObject &&
                raw_obj->object_kind() != ObjectKind::kBooleanObject) {
                break;
            }

            Value return_method = static_cast<JSObject*>(raw_obj)->get_property("return");
            if (!return_method.is_object() ||
                return_method.as_object_raw()->object_kind() != ObjectKind::kFunction) {
                break;
            }

            auto close_res = call_function_val(return_method, iter->iterator_, {});
            if (!close_res.is_ok()) {
                const std::string& msg = close_res.error().message();
                if (msg == "__qppjs_pending_throw__" && native_pending_throw_.has_value()) {
                    frame.pending_throw = std::move(*native_pending_throw_);
                    native_pending_throw_ = std::nullopt;
                } else {
                    NativeErrorType err_type = NativeErrorType::kTypeError;
                    if (msg.rfind("RangeError:", 0) == 0) err_type = NativeErrorType::kRangeError;
                    if (msg.rfind("ReferenceError:", 0) == 0) err_type = NativeErrorType::kReferenceError;
                    if (msg.rfind("SyntaxError:", 0) == 0) err_type = NativeErrorType::kSyntaxError;
                    frame.pending_throw = make_error_value(err_type, strip_error_prefix(msg));
                }
                continue;
            }

            if (!close_res.value().is_object()) {
                frame.pending_throw = make_error_value(NativeErrorType::kTypeError,
                                                       "iterator return must return an object");
                continue;
            }
            break;
        }

        case Opcode::kIteratorCloseAbnormal: {
            Value exception_value = std::move(stack.back());
            stack.pop_back();
            Value iter_val = std::move(stack.back());
            stack.pop_back();
            RcObject* raw_iter = iter_val.as_object_raw();

            if (raw_iter && raw_iter->object_kind() == ObjectKind::kForOfIterator) {
                auto* iter = static_cast<ForOfIterator*>(raw_iter);
                if (!iter->done_) {
                    iter->done_ = true;
                    if (iter->iterator_.is_object()) {
                        RcObject* raw_obj = iter->iterator_.as_object_raw();
                        if (raw_obj->object_kind() == ObjectKind::kOrdinary ||
                            raw_obj->object_kind() == ObjectKind::kArray ||
                            raw_obj->object_kind() == ObjectKind::kRegExp ||
                            raw_obj->object_kind() == ObjectKind::kStringObject ||
                            raw_obj->object_kind() == ObjectKind::kBooleanObject) {
                            Value return_method = static_cast<JSObject*>(raw_obj)->get_property("return");
                            if (return_method.is_object() &&
                                return_method.as_object_raw()->object_kind() == ObjectKind::kFunction) {
                                auto close_res = call_function_val(return_method, iter->iterator_, {});
                                if (!close_res.is_ok()) {
                                    if (native_pending_throw_.has_value()) {
                                        native_pending_throw_ = std::nullopt;
                                    }
                                    if (!call_stack_.empty()) {
                                        call_stack_.back().pending_throw = std::nullopt;
                                    }
                                }
                            }
                        }
                    }
                }
            }

            stack.push_back(std::move(exception_value));
            break;
        }

        case Opcode::kCopyDataProperties: {
            // Operand: n_excluded (u8) — number of excluded key strings on stack
            // Stack layout (bottom to top): [src_obj, excl_key_0, ..., excl_key_(n-1)]
            // Pops all n+1 values, pushes new object with all own enumerable string
            // properties of src_obj except those whose key is in the excluded set.
            uint8_t n_excluded = read_u8(bc, pc);

            // Collect excluded keys (top of stack = last key).
            // Symbol keys are skipped — they never appear in own_enumerable_string_keys.
            std::vector<std::string> excluded;
            excluded.reserve(n_excluded);
            for (int i = 0; i < static_cast<int>(n_excluded); ++i) {
                Value k = std::move(stack.back());
                stack.pop_back();
                if (!k.is_symbol()) excluded.push_back(to_string_val(k));
            }

            Value src_val = std::move(stack.back());
            stack.pop_back();

            auto rest_obj = RcPtr<JSObject>::make();
            gc_heap_.Register(rest_obj.get());
            rest_obj->set_proto(object_prototype_);

            if (src_val.is_object() && src_val.as_object_raw()) {
                RcObject* raw = src_val.as_object_raw();
                if (raw->object_kind() == ObjectKind::kOrdinary ||
                    raw->object_kind() == ObjectKind::kArray) {
                    auto* src_obj = static_cast<JSObject*>(raw);
                    for (const auto& key : src_obj->own_enumerable_string_keys()) {
                        bool skip = false;
                        for (const auto& ex : excluded) {
                            if (ex == key) { skip = true; break; }
                        }
                        if (!skip) {
                            rest_obj->set_property(key, src_obj->get_property(key));
                        }
                    }
                }
            }

            stack.push_back(Value::object(ObjectPtr(rest_obj)));
            break;
        }

        default:
            return EvalResult::err(Error(ErrorKind::Runtime, "Internal: unknown opcode"));
        }
    }

    return EvalResult::ok(Value::undefined());

suspend_exit:
    if (vm_generator_yielded_) {
        // Keep vm_generator_yielded_ = true so vm_generator_resume can detect it.
        // Return the yield result object (already built in kYield handler).
        Value yield_result = vm_generator_yield_value_.has_value()
            ? std::move(*vm_generator_yield_value_) : Value::undefined();
        vm_generator_yield_value_ = std::nullopt;
        return EvalResult::ok(std::move(yield_result));
    }
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
        for (auto& cf : call_stack_) {
            add_obj(cf.env.get());
            add_val(cf.this_val);
            add_val(cf.new_instance);
            add_val(cf.new_target_val);
            if (cf.current_fn_holder) add_obj(cf.current_fn_holder.get());
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
