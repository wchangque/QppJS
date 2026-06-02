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

static bool interp_throws(std::string_view source) {
    auto parse_result = parse_program(source);
    if (!parse_result.ok()) return true;
    Interpreter interp;
    auto result = interp.exec(parse_result.value());
    return !result.is_ok();
}

static bool vm_throws(std::string_view source) {
    auto parse_result = parse_program(source);
    if (!parse_result.ok()) return true;
    Compiler compiler;
    auto bytecode = compiler.compile(parse_result.value());
    VM vm;
    auto result = vm.exec(bytecode);
    return !result.is_ok();
}

// ============================================================
// NS-01: toFixed 基础
// ============================================================

TEST(NumberStringMethods, NS01_ToFixedInterp) {
    auto v = interp_ok("(1.5).toFixed(1)");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "1.5");
}

TEST(NumberStringMethods, NS01_ToFixedVM) {
    auto v = vm_ok("(1.5).toFixed(1)");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "1.5");
}

// ============================================================
// NS-02: toFixed 三位小数
// ============================================================

TEST(NumberStringMethods, NS02_ToFixedThreeInterp) {
    auto v = interp_ok("(1).toFixed(3)");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "1.000");
}

TEST(NumberStringMethods, NS02_ToFixedThreeVM) {
    auto v = vm_ok("(1).toFixed(3)");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "1.000");
}

// ============================================================
// NS-03: toString(16) 十六进制
// ============================================================

TEST(NumberStringMethods, NS03_ToStringHexInterp) {
    auto v = interp_ok("(255).toString(16)");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "ff");
}

TEST(NumberStringMethods, NS03_ToStringHexVM) {
    auto v = vm_ok("(255).toString(16)");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "ff");
}

// ============================================================
// NS-04: toString(2) 二进制
// ============================================================

TEST(NumberStringMethods, NS04_ToStringBinaryInterp) {
    auto v = interp_ok("(8).toString(2)");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "1000");
}

TEST(NumberStringMethods, NS04_ToStringBinaryVM) {
    auto v = vm_ok("(8).toString(2)");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "1000");
}

// ============================================================
// NS-05: NaN.toFixed() → "NaN"
// ============================================================

TEST(NumberStringMethods, NS05_ToFixedNaNInterp) {
    auto v = interp_ok("(NaN).toFixed()");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "NaN");
}

TEST(NumberStringMethods, NS05_ToFixedNaNVM) {
    auto v = vm_ok("(NaN).toFixed()");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "NaN");
}

// ============================================================
// NS-06: Infinity.toString() → "Infinity"
// ============================================================

TEST(NumberStringMethods, NS06_ToStringInfinityInterp) {
    auto v = interp_ok("(Infinity).toString()");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "Infinity");
}

TEST(NumberStringMethods, NS06_ToStringInfinityVM) {
    auto v = vm_ok("(Infinity).toString()");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "Infinity");
}

// ============================================================
// NS-07: toExponential(2)
// ============================================================

TEST(NumberStringMethods, NS07_ToExponentialInterp) {
    auto v = interp_ok("(1.5e+20).toExponential(2)");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "1.50e+20");
}

TEST(NumberStringMethods, NS07_ToExponentialVM) {
    auto v = vm_ok("(1.5e+20).toExponential(2)");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "1.50e+20");
}

// ============================================================
// NS-08: toPrecision(5)
// ============================================================

TEST(NumberStringMethods, NS08_ToPrecisionInterp) {
    auto v = interp_ok("(123.456).toPrecision(5)");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "123.46");
}

TEST(NumberStringMethods, NS08_ToPrecisionVM) {
    auto v = vm_ok("(123.456).toPrecision(5)");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "123.46");
}

// ============================================================
// NS-09: toFixed(2) 零值
// ============================================================

TEST(NumberStringMethods, NS09_ToFixedZeroInterp) {
    auto v = interp_ok("(0).toFixed(2)");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "0.00");
}

TEST(NumberStringMethods, NS09_ToFixedZeroVM) {
    auto v = vm_ok("(0).toFixed(2)");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "0.00");
}

// ============================================================
// NS-10: toString 非法 radix → RangeError
// ============================================================

TEST(NumberStringMethods, NS10_ToStringBadRadixInterp) {
    EXPECT_TRUE(interp_throws("(10).toString(1)"));
}

TEST(NumberStringMethods, NS10_ToStringBadRadixVM) {
    EXPECT_TRUE(vm_throws("(10).toString(1)"));
}

// ============================================================
// NS-11: String.prototype.at(-1)
// ============================================================

TEST(NumberStringMethods, NS11_StringAtNegativeInterp) {
    auto v = interp_ok("'hello'.at(-1)");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "o");
}

TEST(NumberStringMethods, NS11_StringAtNegativeVM) {
    auto v = vm_ok("'hello'.at(-1)");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "o");
}

// ============================================================
// NS-12: String.prototype.at(0)
// ============================================================

TEST(NumberStringMethods, NS12_StringAtZeroInterp) {
    auto v = interp_ok("'hello'.at(0)");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "h");
}

TEST(NumberStringMethods, NS12_StringAtZeroVM) {
    auto v = vm_ok("'hello'.at(0)");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "h");
}

// ============================================================
// NS-13: replaceAll
// ============================================================

TEST(NumberStringMethods, NS13_ReplaceAllInterp) {
    auto v = interp_ok("'aabbcc'.replaceAll('b', 'x')");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "aaxxcc");
}

TEST(NumberStringMethods, NS13_ReplaceAllVM) {
    auto v = vm_ok("'aabbcc'.replaceAll('b', 'x')");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "aaxxcc");
}

// ============================================================
// NS-14: padStart
// ============================================================

TEST(NumberStringMethods, NS14_PadStartInterp) {
    auto v = interp_ok("'hi  '.padStart(8)");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "    hi  ");
}

TEST(NumberStringMethods, NS14_PadStartVM) {
    auto v = vm_ok("'hi  '.padStart(8)");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "    hi  ");
}

// ============================================================
// NS-15: padEnd
// ============================================================

TEST(NumberStringMethods, NS15_PadEndInterp) {
    auto v = interp_ok("'hi'.padEnd(5, '.')");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "hi...");
}

TEST(NumberStringMethods, NS15_PadEndVM) {
    auto v = vm_ok("'hi'.padEnd(5, '.')");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "hi...");
}

// ============================================================
// NS-16: repeat
// ============================================================

TEST(NumberStringMethods, NS16_RepeatInterp) {
    auto v = interp_ok("'abc'.repeat(3)");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "abcabcabc");
}

TEST(NumberStringMethods, NS16_RepeatVM) {
    auto v = vm_ok("'abc'.repeat(3)");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "abcabcabc");
}

// ============================================================
// NS-17: startsWith
// ============================================================

TEST(NumberStringMethods, NS17_StartsWithInterp) {
    auto v = interp_ok("'hello'.startsWith('hel')");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(NumberStringMethods, NS17_StartsWithVM) {
    auto v = vm_ok("'hello'.startsWith('hel')");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// NS-18: endsWith
// ============================================================

TEST(NumberStringMethods, NS18_EndsWithInterp) {
    auto v = interp_ok("'hello'.endsWith('llo')");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(NumberStringMethods, NS18_EndsWithVM) {
    auto v = vm_ok("'hello'.endsWith('llo')");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// NS-19: includes
// ============================================================

TEST(NumberStringMethods, NS19_IncludesInterp) {
    auto v = interp_ok("'hello world'.includes('world')");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(NumberStringMethods, NS19_IncludesVM) {
    auto v = vm_ok("'hello world'.includes('world')");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// NS-20: replace 仅第一个
// ============================================================

TEST(NumberStringMethods, NS20_ReplaceFirstInterp) {
    auto v = interp_ok("'hello'.replace('l', 'r')");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "herlo");
}

TEST(NumberStringMethods, NS20_ReplaceFirstVM) {
    auto v = vm_ok("'hello'.replace('l', 'r')");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "herlo");
}

// ============================================================
// NS-21: match global
// ============================================================

TEST(NumberStringMethods, NS21_MatchGlobalInterp) {
    auto v = interp_ok("'hello'.match(/l+/g)");
    EXPECT_TRUE(v.is_object());
    auto* arr = static_cast<JSObject*>(v.as_object_raw());
    EXPECT_EQ(arr->array_length_, 1u);
    EXPECT_EQ(arr->elements_[0].sv(), "ll");
}

TEST(NumberStringMethods, NS21_MatchGlobalVM) {
    auto v = vm_ok("'hello'.match(/l+/g)");
    EXPECT_TRUE(v.is_object());
    auto* arr = static_cast<JSObject*>(v.as_object_raw());
    EXPECT_EQ(arr->array_length_, 1u);
    EXPECT_EQ(arr->elements_[0].sv(), "ll");
}

// ============================================================
// NS-22: search
// ============================================================

TEST(NumberStringMethods, NS22_SearchInterp) {
    auto v = interp_ok("'hello'.search(/l/)");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.0);
}

TEST(NumberStringMethods, NS22_SearchVM) {
    auto v = vm_ok("'hello'.search(/l/)");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.0);
}

// ============================================================
// NS-23: repeat(0) → ""
// ============================================================

TEST(NumberStringMethods, NS23_RepeatZeroInterp) {
    auto v = interp_ok("'abc'.repeat(0)");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "");
}

TEST(NumberStringMethods, NS23_RepeatZeroVM) {
    auto v = vm_ok("'abc'.repeat(0)");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "");
}

// ============================================================
// NS-24: repeat(-1) → RangeError
// ============================================================

TEST(NumberStringMethods, NS24_RepeatNegativeInterp) {
    EXPECT_TRUE(interp_throws("'abc'.repeat(-1)"));
}

TEST(NumberStringMethods, NS24_RepeatNegativeVM) {
    EXPECT_TRUE(vm_throws("'abc'.repeat(-1)"));
}

// ============================================================
// NS-25: matchAll 基础
// ============================================================

TEST(NumberStringMethods, NS25_MatchAllInterp) {
    auto v = interp_ok(R"(
        var iter = 'aXbXc'.matchAll(/X/g);
        var r1 = iter.next();
        var r2 = iter.next();
        var r3 = iter.next();
        [r1.value[0], r2.value[0], r3.done]
    )");
    EXPECT_TRUE(v.is_object());
    auto* arr = static_cast<JSObject*>(v.as_object_raw());
    EXPECT_EQ(arr->elements_[0].sv(), "X");
    EXPECT_EQ(arr->elements_[1].sv(), "X");
    EXPECT_TRUE(arr->elements_[2].as_bool());
}

TEST(NumberStringMethods, NS25_MatchAllVM) {
    auto v = vm_ok(R"(
        var iter = 'aXbXc'.matchAll(/X/g);
        var r1 = iter.next();
        var r2 = iter.next();
        var r3 = iter.next();
        [r1.value[0], r2.value[0], r3.done]
    )");
    EXPECT_TRUE(v.is_object());
    auto* arr = static_cast<JSObject*>(v.as_object_raw());
    EXPECT_EQ(arr->elements_[0].sv(), "X");
    EXPECT_EQ(arr->elements_[1].sv(), "X");
    EXPECT_TRUE(arr->elements_[2].as_bool());
}

}  // namespace
