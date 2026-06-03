#include "qppjs/frontend/parser.h"
#include "qppjs/runtime/interpreter.h"
#include "qppjs/vm/compiler.h"
#include "qppjs/vm/vm.h"

#include <gtest/gtest.h>

#include <string_view>

using namespace qppjs;

namespace {

static Value interp_ok(std::string_view source) {
    auto parse_result = parse_program(source);
    EXPECT_TRUE(parse_result.ok()) << "parse failed: " << parse_result.error().message();
    if (!parse_result.ok()) return Value::undefined();
    Interpreter interp;
    auto result = interp.exec(parse_result.value());
    EXPECT_TRUE(result.is_ok()) << "exec failed: " << result.error().message();
    return result.value();
}

static Value vm_ok(std::string_view source) {
    auto parse_result = parse_program(source);
    EXPECT_TRUE(parse_result.ok()) << "parse failed: " << parse_result.error().message();
    if (!parse_result.ok()) return Value::undefined();
    Compiler compiler;
    auto bytecode = compiler.compile(parse_result.value());
    VM vm;
    auto result = vm.exec(bytecode);
    EXPECT_TRUE(result.is_ok()) << "exec failed: " << result.error().message();
    return result.value();
}

// ============================================================
// MF-01: Array.prototype.constructor === Array
// ============================================================

TEST(MiscFixes, MF01_ArrayConstructorInterp) {
    auto v = interp_ok("[1,2].constructor === Array");
    EXPECT_TRUE(v.is_bool() && v.as_bool());
}

TEST(MiscFixes, MF01_ArrayConstructorVM) {
    auto v = vm_ok("[1,2].constructor === Array");
    EXPECT_TRUE(v.is_bool() && v.as_bool());
}

// ============================================================
// MF-02: Object.prototype.toString.call([]) → "[object Array]"
// ============================================================

TEST(MiscFixes, MF02_ToStringArrayInterp) {
    auto v = interp_ok("Object.prototype.toString.call([])");
    EXPECT_EQ(v.as_string(), "[object Array]");
}

TEST(MiscFixes, MF02_ToStringArrayVM) {
    auto v = vm_ok("Object.prototype.toString.call([])");
    EXPECT_EQ(v.as_string(), "[object Array]");
}

// ============================================================
// MF-03: Object.prototype.toString.call(null) → "[object Null]"
// ============================================================

TEST(MiscFixes, MF03_ToStringNullInterp) {
    auto v = interp_ok("Object.prototype.toString.call(null)");
    EXPECT_EQ(v.as_string(), "[object Null]");
}

TEST(MiscFixes, MF03_ToStringNullVM) {
    auto v = vm_ok("Object.prototype.toString.call(null)");
    EXPECT_EQ(v.as_string(), "[object Null]");
}

// ============================================================
// MF-04: Object.prototype.toString.call(function(){}) → "[object Function]"
// ============================================================

TEST(MiscFixes, MF04_ToStringFunctionInterp) {
    auto v = interp_ok("Object.prototype.toString.call(function(){})");
    EXPECT_EQ(v.as_string(), "[object Function]");
}

TEST(MiscFixes, MF04_ToStringFunctionVM) {
    auto v = vm_ok("Object.prototype.toString.call(function(){})");
    EXPECT_EQ(v.as_string(), "[object Function]");
}

// ============================================================
// MF-05: String.fromCodePoint(65) → "A"
// ============================================================

TEST(MiscFixes, MF05_FromCodePointInterp) {
    auto v = interp_ok("String.fromCodePoint(65)");
    EXPECT_EQ(v.as_string(), "A");
}

TEST(MiscFixes, MF05_FromCodePointVM) {
    auto v = vm_ok("String.fromCodePoint(65)");
    EXPECT_EQ(v.as_string(), "A");
}

// MF-05b: multi-arg fromCodePoint
TEST(MiscFixes, MF05b_FromCodePointMultiInterp) {
    auto v = interp_ok("String.fromCodePoint(72, 101, 108, 108, 111)");
    EXPECT_EQ(v.as_string(), "Hello");
}

TEST(MiscFixes, MF05b_FromCodePointMultiVM) {
    auto v = vm_ok("String.fromCodePoint(72, 101, 108, 108, 111)");
    EXPECT_EQ(v.as_string(), "Hello");
}

// MF-05c: SMP codepoint (U+1F600 = emoji, requires surrogate pair in UTF-16)
TEST(MiscFixes, MF05c_FromCodePointSMPInterp) {
    // U+1F600 emoji, UTF-8 is F0 9F 98 80
    auto v = interp_ok("String.fromCodePoint(128512).length");
    // In UTF-16, length=2 (surrogate pair)
    EXPECT_EQ(v.as_number(), 2.0);
}

TEST(MiscFixes, MF05c_FromCodePointSMPVM) {
    auto v = vm_ok("String.fromCodePoint(128512).length");
    EXPECT_EQ(v.as_number(), 2.0);
}

// MF-05d: RangeError for out-of-range codepoint
TEST(MiscFixes, MF05d_FromCodePointRangeErrorInterp) {
    auto parse_result = parse_program("String.fromCodePoint(0x110000)");
    ASSERT_TRUE(parse_result.ok());
    Interpreter interp;
    auto result = interp.exec(parse_result.value());
    EXPECT_FALSE(result.is_ok());
    EXPECT_NE(result.error().message().find("RangeError"), std::string::npos);
}

TEST(MiscFixes, MF05d_FromCodePointRangeErrorVM) {
    auto parse_result = parse_program("String.fromCodePoint(0x110000)");
    ASSERT_TRUE(parse_result.ok());
    Compiler compiler;
    auto bytecode = compiler.compile(parse_result.value());
    VM vm;
    auto result = vm.exec(bytecode);
    EXPECT_FALSE(result.is_ok());
    EXPECT_NE(result.error().message().find("RangeError"), std::string::npos);
}

// ============================================================
// MF-06: "abc".codePointAt(0) → 97
// ============================================================

TEST(MiscFixes, MF06_CodePointAtInterp) {
    auto v = interp_ok("\"abc\".codePointAt(0)");
    EXPECT_EQ(v.as_number(), 97.0);
}

TEST(MiscFixes, MF06_CodePointAtVM) {
    auto v = vm_ok("\"abc\".codePointAt(0)");
    EXPECT_EQ(v.as_number(), 97.0);
}

// MF-06b: out of bounds returns undefined
TEST(MiscFixes, MF06b_CodePointAtOobInterp) {
    EXPECT_TRUE(interp_ok("\"abc\".codePointAt(10)").is_undefined());
}

TEST(MiscFixes, MF06b_CodePointAtOobVM) {
    EXPECT_TRUE(vm_ok("\"abc\".codePointAt(10)").is_undefined());
}

// ============================================================
// MF-07: Object.getOwnPropertySymbols
// ============================================================

TEST(MiscFixes, MF07_GetOwnPropertySymbolsInterp) {
    auto v = interp_ok(R"(
        var s = Symbol("key");
        var obj = {};
        obj[s] = 42;
        var syms = Object.getOwnPropertySymbols(obj);
        syms.length === 1 && syms[0] === s
    )");
    EXPECT_TRUE(v.is_bool() && v.as_bool());
}

TEST(MiscFixes, MF07_GetOwnPropertySymbolsVM) {
    auto v = vm_ok(R"(
        var s = Symbol("key");
        var obj = {};
        obj[s] = 42;
        var syms = Object.getOwnPropertySymbols(obj);
        syms.length === 1 && syms[0] === s
    )");
    EXPECT_TRUE(v.is_bool() && v.as_bool());
}

// MF-07b: empty object returns empty array
TEST(MiscFixes, MF07b_GetOwnPropertySymbolsEmptyInterp) {
    auto v = interp_ok("Object.getOwnPropertySymbols({}).length");
    EXPECT_EQ(v.as_number(), 0.0);
}

TEST(MiscFixes, MF07b_GetOwnPropertySymbolsEmptyVM) {
    auto v = vm_ok("Object.getOwnPropertySymbols({}).length");
    EXPECT_EQ(v.as_number(), 0.0);
}

// ============================================================
// MF-08: String.prototype.charCodeAt
// ============================================================

TEST(MiscFixes, MF08_CharCodeAtInterp) {
    auto v = interp_ok("\"abc\".charCodeAt(0)");
    EXPECT_EQ(v.as_number(), 97.0);
}

TEST(MiscFixes, MF08_CharCodeAtVM) {
    auto v = vm_ok("\"abc\".charCodeAt(0)");
    EXPECT_EQ(v.as_number(), 97.0);
}

TEST(MiscFixes, MF08b_CharCodeAtIndexInterp) {
    auto v = interp_ok("\"abc\".charCodeAt(1)");
    EXPECT_EQ(v.as_number(), 98.0);
}

TEST(MiscFixes, MF08b_CharCodeAtIndexVM) {
    auto v = vm_ok("\"abc\".charCodeAt(1)");
    EXPECT_EQ(v.as_number(), 98.0);
}

// out of bounds → NaN
TEST(MiscFixes, MF08c_CharCodeAtOobInterp) {
    auto v = interp_ok("\"abc\".charCodeAt(10)");
    EXPECT_TRUE(std::isnan(v.as_number()));
}

TEST(MiscFixes, MF08c_CharCodeAtOobVM) {
    auto v = vm_ok("\"abc\".charCodeAt(10)");
    EXPECT_TRUE(std::isnan(v.as_number()));
}

// ============================================================
// MF-09: String.prototype.charAt
// ============================================================

TEST(MiscFixes, MF09_CharAtInterp) {
    auto v = interp_ok("\"abc\".charAt(1)");
    EXPECT_EQ(v.as_string(), "b");
}

TEST(MiscFixes, MF09_CharAtVM) {
    auto v = vm_ok("\"abc\".charAt(1)");
    EXPECT_EQ(v.as_string(), "b");
}

// out of bounds → ""
TEST(MiscFixes, MF09b_CharAtOobInterp) {
    auto v = interp_ok("\"abc\".charAt(10)");
    EXPECT_EQ(v.as_string(), "");
}

TEST(MiscFixes, MF09b_CharAtOobVM) {
    auto v = vm_ok("\"abc\".charAt(10)");
    EXPECT_EQ(v.as_string(), "");
}

// ============================================================
// MF-10: Object.prototype.toString with undefined
// ============================================================

TEST(MiscFixes, MF10_ToStringUndefinedInterp) {
    auto v = interp_ok("Object.prototype.toString.call(undefined)");
    EXPECT_EQ(v.as_string(), "[object Undefined]");
}

TEST(MiscFixes, MF10_ToStringUndefinedVM) {
    auto v = vm_ok("Object.prototype.toString.call(undefined)");
    EXPECT_EQ(v.as_string(), "[object Undefined]");
}

// ============================================================
// MF-11: Array.isArray([]) → true
// ============================================================

TEST(MiscFixes, MF11_ArrayIsArrayInterp) {
    auto v = interp_ok("Array.isArray([])");
    EXPECT_TRUE(v.is_bool() && v.as_bool());
}

TEST(MiscFixes, MF11_ArrayIsArrayVM) {
    auto v = vm_ok("Array.isArray([])");
    EXPECT_TRUE(v.is_bool() && v.as_bool());
}

// ============================================================
// MF-12: Array.from("abc") → ["a","b","c"]
// ============================================================

TEST(MiscFixes, MF12_ArrayFromStringInterp) {
    auto v = interp_ok(R"(
        var a = Array.from("abc");
        a[0] === "a" && a[1] === "b" && a[2] === "c" && a.length === 3
    )");
    EXPECT_TRUE(v.is_bool() && v.as_bool());
}

TEST(MiscFixes, MF12_ArrayFromStringVM) {
    auto v = vm_ok(R"(
        var a = Array.from("abc");
        a[0] === "a" && a[1] === "b" && a[2] === "c" && a.length === 3
    )");
    EXPECT_TRUE(v.is_bool() && v.as_bool());
}

}  // namespace
