#include "qppjs/frontend/parser.h"
#include "qppjs/runtime/interpreter.h"
#include "qppjs/vm/compiler.h"
#include "qppjs/vm/vm.h"

#include <gtest/gtest.h>

#include <cmath>
#include <string_view>

namespace {

// ============================================================
// Helpers
// ============================================================

qppjs::Value interp_ok(std::string_view source) {
    auto parse_result = qppjs::parse_program(source);
    EXPECT_TRUE(parse_result.ok()) << "parse failed: " << parse_result.error().message();
    if (!parse_result.ok()) return qppjs::Value::undefined();
    qppjs::Interpreter interp;
    auto result = interp.exec(parse_result.value());
    EXPECT_TRUE(result.is_ok()) << "exec failed: " << result.error().message();
    if (!result.is_ok()) return qppjs::Value::undefined();
    return result.value();
}

qppjs::Value vm_ok(std::string_view source) {
    auto parse_result = qppjs::parse_program(source);
    EXPECT_TRUE(parse_result.ok()) << "parse failed: " << parse_result.error().message();
    if (!parse_result.ok()) return qppjs::Value::undefined();
    qppjs::Compiler compiler;
    auto bytecode = compiler.compile(parse_result.value());
    qppjs::VM vm;
    auto result = vm.exec(bytecode);
    EXPECT_TRUE(result.is_ok()) << "exec failed: " << result.error().message();
    if (!result.is_ok()) return qppjs::Value::undefined();
    return result.value();
}

// ============================================================
// NM-01: 全局常量 NaN
// ============================================================

TEST(NumberMathInterpTest, NM01_NaN_not_equal_self) {
    auto v = interp_ok("NaN !== NaN");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(NumberMathVMTest, NM01_NaN_not_equal_self) {
    auto v = vm_ok("NaN !== NaN");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// NM-02: 全局常量 Infinity
// ============================================================

TEST(NumberMathInterpTest, NM02_Infinity_large) {
    auto v = interp_ok("Infinity > 1e308");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(NumberMathVMTest, NM02_Infinity_large) {
    auto v = vm_ok("Infinity > 1e308");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// NM-03: isNaN(NaN) === true
// ============================================================

TEST(NumberMathInterpTest, NM03_isNaN_NaN) {
    auto v = interp_ok("isNaN(NaN)");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(NumberMathVMTest, NM03_isNaN_NaN) {
    auto v = vm_ok("isNaN(NaN)");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// NM-04: isFinite(Infinity) === false
// ============================================================

TEST(NumberMathInterpTest, NM04_isFinite_Infinity) {
    auto v = interp_ok("isFinite(Infinity)");
    EXPECT_TRUE(v.is_bool());
    EXPECT_FALSE(v.as_bool());
}

TEST(NumberMathVMTest, NM04_isFinite_Infinity) {
    auto v = vm_ok("isFinite(Infinity)");
    EXPECT_TRUE(v.is_bool());
    EXPECT_FALSE(v.as_bool());
}

// ============================================================
// NM-05: 全局 isNaN 做 ToNumber 转换
// ============================================================

TEST(NumberMathInterpTest, NM05_global_isNaN_string) {
    auto v = interp_ok("isNaN(\"abc\")");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(NumberMathVMTest, NM05_global_isNaN_string) {
    auto v = vm_ok("isNaN(\"abc\")");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// NM-06: Number.isNaN 不做 ToNumber 转换
// ============================================================

TEST(NumberMathInterpTest, NM06_Number_isNaN_string) {
    auto v = interp_ok("Number.isNaN(\"abc\")");
    EXPECT_TRUE(v.is_bool());
    EXPECT_FALSE(v.as_bool());
}

TEST(NumberMathVMTest, NM06_Number_isNaN_string) {
    auto v = vm_ok("Number.isNaN(\"abc\")");
    EXPECT_TRUE(v.is_bool());
    EXPECT_FALSE(v.as_bool());
}

// ============================================================
// NM-07: 全局 isNaN(undefined) === true
// ============================================================

TEST(NumberMathInterpTest, NM07_global_isNaN_undefined) {
    auto v = interp_ok("isNaN(undefined)");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(NumberMathVMTest, NM07_global_isNaN_undefined) {
    auto v = vm_ok("isNaN(undefined)");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// NM-08: Number.isNaN(undefined) === false
// ============================================================

TEST(NumberMathInterpTest, NM08_Number_isNaN_undefined) {
    auto v = interp_ok("Number.isNaN(undefined)");
    EXPECT_TRUE(v.is_bool());
    EXPECT_FALSE(v.as_bool());
}

TEST(NumberMathVMTest, NM08_Number_isNaN_undefined) {
    auto v = vm_ok("Number.isNaN(undefined)");
    EXPECT_TRUE(v.is_bool());
    EXPECT_FALSE(v.as_bool());
}

// ============================================================
// NM-09: 全局 isFinite("1") === true
// ============================================================

TEST(NumberMathInterpTest, NM09_global_isFinite_string) {
    auto v = interp_ok("isFinite(\"1\")");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(NumberMathVMTest, NM09_global_isFinite_string) {
    auto v = vm_ok("isFinite(\"1\")");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// NM-10: Number.isFinite("1") === false
// ============================================================

TEST(NumberMathInterpTest, NM10_Number_isFinite_string) {
    auto v = interp_ok("Number.isFinite(\"1\")");
    EXPECT_TRUE(v.is_bool());
    EXPECT_FALSE(v.as_bool());
}

TEST(NumberMathVMTest, NM10_Number_isFinite_string) {
    auto v = vm_ok("Number.isFinite(\"1\")");
    EXPECT_TRUE(v.is_bool());
    EXPECT_FALSE(v.as_bool());
}

// ============================================================
// NM-11: Number.isFinite(Infinity) === false
// ============================================================

TEST(NumberMathInterpTest, NM11_Number_isFinite_Infinity) {
    auto v = interp_ok("Number.isFinite(Infinity)");
    EXPECT_TRUE(v.is_bool());
    EXPECT_FALSE(v.as_bool());
}

TEST(NumberMathVMTest, NM11_Number_isFinite_Infinity) {
    auto v = vm_ok("Number.isFinite(Infinity)");
    EXPECT_TRUE(v.is_bool());
    EXPECT_FALSE(v.as_bool());
}

// ============================================================
// NM-12: parseInt 十进制
// ============================================================

TEST(NumberMathInterpTest, NM12_parseInt_decimal) {
    auto v = interp_ok("parseInt(\"42\")");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 42.0);
}

TEST(NumberMathVMTest, NM12_parseInt_decimal) {
    auto v = vm_ok("parseInt(\"42\")");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 42.0);
}

// ============================================================
// NM-13: parseInt 十六进制
// ============================================================

TEST(NumberMathInterpTest, NM13_parseInt_hex) {
    auto v = interp_ok("parseInt(\"0xff\")");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 255.0);
}

TEST(NumberMathVMTest, NM13_parseInt_hex) {
    auto v = vm_ok("parseInt(\"0xff\")");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 255.0);
}

// ============================================================
// NM-14: parseInt 带 radix
// ============================================================

TEST(NumberMathInterpTest, NM14_parseInt_radix) {
    auto v = interp_ok("parseInt(\"10\", 2)");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 2.0);
}

TEST(NumberMathVMTest, NM14_parseInt_radix) {
    auto v = vm_ok("parseInt(\"10\", 2)");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 2.0);
}

// ============================================================
// NM-15: parseInt 空字符串 → NaN
// ============================================================

TEST(NumberMathInterpTest, NM15_parseInt_empty) {
    auto v = interp_ok("parseInt(\"\")");
    EXPECT_TRUE(v.is_number());
    EXPECT_TRUE(std::isnan(v.as_number()));
}

TEST(NumberMathVMTest, NM15_parseInt_empty) {
    auto v = vm_ok("parseInt(\"\")");
    EXPECT_TRUE(v.is_number());
    EXPECT_TRUE(std::isnan(v.as_number()));
}

// ============================================================
// NM-16: parseInt 前导空白
// ============================================================

TEST(NumberMathInterpTest, NM16_parseInt_leading_space) {
    auto v = interp_ok("parseInt(\"  42  \")");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 42.0);
}

TEST(NumberMathVMTest, NM16_parseInt_leading_space) {
    auto v = vm_ok("parseInt(\"  42  \")");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 42.0);
}

// ============================================================
// NM-17: parseInt 部分匹配
// ============================================================

TEST(NumberMathInterpTest, NM17_parseInt_partial) {
    auto v = interp_ok("parseInt(\"42abc\")");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 42.0);
}

TEST(NumberMathVMTest, NM17_parseInt_partial) {
    auto v = vm_ok("parseInt(\"42abc\")");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 42.0);
}

// ============================================================
// NM-18: parseFloat 基础
// ============================================================

TEST(NumberMathInterpTest, NM18_parseFloat_basic) {
    auto v = interp_ok("parseFloat(\"3.14\")");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 3.14);
}

TEST(NumberMathVMTest, NM18_parseFloat_basic) {
    auto v = vm_ok("parseFloat(\"3.14\")");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 3.14);
}

// ============================================================
// NM-19: parseFloat 部分匹配
// ============================================================

TEST(NumberMathInterpTest, NM19_parseFloat_partial) {
    auto v = interp_ok("parseFloat(\"3.14abc\")");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 3.14);
}

TEST(NumberMathVMTest, NM19_parseFloat_partial) {
    auto v = vm_ok("parseFloat(\"3.14abc\")");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 3.14);
}

// ============================================================
// NM-20: parseFloat 空字符串 → NaN
// ============================================================

TEST(NumberMathInterpTest, NM20_parseFloat_empty) {
    auto v = interp_ok("parseFloat(\"\")");
    EXPECT_TRUE(v.is_number());
    EXPECT_TRUE(std::isnan(v.as_number()));
}

TEST(NumberMathVMTest, NM20_parseFloat_empty) {
    auto v = vm_ok("parseFloat(\"\")");
    EXPECT_TRUE(v.is_number());
    EXPECT_TRUE(std::isnan(v.as_number()));
}

// ============================================================
// NM-21: parseFloat 前导空白
// ============================================================

TEST(NumberMathInterpTest, NM21_parseFloat_leading_space) {
    auto v = interp_ok("parseFloat(\"  1.5\")");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 1.5);
}

TEST(NumberMathVMTest, NM21_parseFloat_leading_space) {
    auto v = vm_ok("parseFloat(\"  1.5\")");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 1.5);
}

// ============================================================
// NM-22: Number.isInteger(1) === true
// ============================================================

TEST(NumberMathInterpTest, NM22_Number_isInteger_integer) {
    auto v = interp_ok("Number.isInteger(1)");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(NumberMathVMTest, NM22_Number_isInteger_integer) {
    auto v = vm_ok("Number.isInteger(1)");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// NM-23: Number.isInteger(1.5) === false
// ============================================================

TEST(NumberMathInterpTest, NM23_Number_isInteger_float) {
    auto v = interp_ok("Number.isInteger(1.5)");
    EXPECT_TRUE(v.is_bool());
    EXPECT_FALSE(v.as_bool());
}

TEST(NumberMathVMTest, NM23_Number_isInteger_float) {
    auto v = vm_ok("Number.isInteger(1.5)");
    EXPECT_TRUE(v.is_bool());
    EXPECT_FALSE(v.as_bool());
}

// ============================================================
// NM-24: Number.isInteger(NaN) === false
// ============================================================

TEST(NumberMathInterpTest, NM24_Number_isInteger_NaN) {
    auto v = interp_ok("Number.isInteger(NaN)");
    EXPECT_TRUE(v.is_bool());
    EXPECT_FALSE(v.as_bool());
}

TEST(NumberMathVMTest, NM24_Number_isInteger_NaN) {
    auto v = vm_ok("Number.isInteger(NaN)");
    EXPECT_TRUE(v.is_bool());
    EXPECT_FALSE(v.as_bool());
}

// ============================================================
// NM-25: Number.isInteger(Infinity) === false
// ============================================================

TEST(NumberMathInterpTest, NM25_Number_isInteger_Infinity) {
    auto v = interp_ok("Number.isInteger(Infinity)");
    EXPECT_TRUE(v.is_bool());
    EXPECT_FALSE(v.as_bool());
}

TEST(NumberMathVMTest, NM25_Number_isInteger_Infinity) {
    auto v = vm_ok("Number.isInteger(Infinity)");
    EXPECT_TRUE(v.is_bool());
    EXPECT_FALSE(v.as_bool());
}

// ============================================================
// NM-26: Number.isInteger(-0) === true
// ============================================================

TEST(NumberMathInterpTest, NM26_Number_isInteger_neg_zero) {
    auto v = interp_ok("Number.isInteger(-0)");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(NumberMathVMTest, NM26_Number_isInteger_neg_zero) {
    auto v = vm_ok("Number.isInteger(-0)");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// NM-27: Number.parseInt === parseInt (行为一致性)
// ============================================================

TEST(NumberMathInterpTest, NM27_Number_parseInt_same_as_global) {
    auto v = interp_ok("Number.parseInt(\"42\") === parseInt(\"42\")");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(NumberMathVMTest, NM27_Number_parseInt_same_as_global) {
    auto v = vm_ok("Number.parseInt(\"42\") === parseInt(\"42\")");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// NM-28: Math.floor
// ============================================================

TEST(NumberMathInterpTest, NM28_Math_floor) {
    auto v = interp_ok("Math.floor(4.7)");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 4.0);
}

TEST(NumberMathVMTest, NM28_Math_floor) {
    auto v = vm_ok("Math.floor(4.7)");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 4.0);
}

// ============================================================
// NM-29: Math.ceil
// ============================================================

TEST(NumberMathInterpTest, NM29_Math_ceil) {
    auto v = interp_ok("Math.ceil(4.2)");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 5.0);
}

TEST(NumberMathVMTest, NM29_Math_ceil) {
    auto v = vm_ok("Math.ceil(4.2)");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 5.0);
}

// ============================================================
// NM-30: Math.round(0.5) → 1
// ============================================================

TEST(NumberMathInterpTest, NM30_Math_round_half) {
    auto v = interp_ok("Math.round(0.5)");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 1.0);
}

TEST(NumberMathVMTest, NM30_Math_round_half) {
    auto v = vm_ok("Math.round(0.5)");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 1.0);
}

// ============================================================
// NM-31: Math.round(-0.5) → -0
// ============================================================

TEST(NumberMathInterpTest, NM31_Math_round_neg_half) {
    // -0.5 rounds to -0 (not -1): verify via 1/x === -Infinity
    auto v = interp_ok("1 / Math.round(-0.5) === -Infinity");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(NumberMathVMTest, NM31_Math_round_neg_half) {
    auto v = vm_ok("1 / Math.round(-0.5) === -Infinity");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// NM-32: Math.round(-1.5) → -1
// ============================================================

TEST(NumberMathInterpTest, NM32_Math_round_neg_one_half) {
    auto v = interp_ok("Math.round(-1.5)");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), -1.0);
}

TEST(NumberMathVMTest, NM32_Math_round_neg_one_half) {
    auto v = vm_ok("Math.round(-1.5)");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), -1.0);
}

// ============================================================
// NM-33: Math.max() → -Infinity
// ============================================================

TEST(NumberMathInterpTest, NM33_Math_max_no_args) {
    auto v = interp_ok("Math.max()");
    EXPECT_TRUE(v.is_number());
    EXPECT_TRUE(std::isinf(v.as_number()) && v.as_number() < 0);
}

TEST(NumberMathVMTest, NM33_Math_max_no_args) {
    auto v = vm_ok("Math.max()");
    EXPECT_TRUE(v.is_number());
    EXPECT_TRUE(std::isinf(v.as_number()) && v.as_number() < 0);
}

// ============================================================
// NM-34: Math.min() → +Infinity
// ============================================================

TEST(NumberMathInterpTest, NM34_Math_min_no_args) {
    auto v = interp_ok("Math.min()");
    EXPECT_TRUE(v.is_number());
    EXPECT_TRUE(std::isinf(v.as_number()) && v.as_number() > 0);
}

TEST(NumberMathVMTest, NM34_Math_min_no_args) {
    auto v = vm_ok("Math.min()");
    EXPECT_TRUE(v.is_number());
    EXPECT_TRUE(std::isinf(v.as_number()) && v.as_number() > 0);
}

// ============================================================
// NM-35: Math.max(1, NaN, 2) → NaN
// ============================================================

TEST(NumberMathInterpTest, NM35_Math_max_with_NaN) {
    auto v = interp_ok("Math.max(1, NaN, 2)");
    EXPECT_TRUE(v.is_number());
    EXPECT_TRUE(std::isnan(v.as_number()));
}

TEST(NumberMathVMTest, NM35_Math_max_with_NaN) {
    auto v = vm_ok("Math.max(1, NaN, 2)");
    EXPECT_TRUE(v.is_number());
    EXPECT_TRUE(std::isnan(v.as_number()));
}

// ============================================================
// NM-36: Math.sign(5) → 1
// ============================================================

TEST(NumberMathInterpTest, NM36_Math_sign_positive) {
    auto v = interp_ok("Math.sign(5)");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 1.0);
}

TEST(NumberMathVMTest, NM36_Math_sign_positive) {
    auto v = vm_ok("Math.sign(5)");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 1.0);
}

// ============================================================
// NM-37: Math.sign(-0) → -0
// ============================================================

TEST(NumberMathInterpTest, NM37_Math_sign_neg_zero) {
    auto v = interp_ok("1 / Math.sign(-0) === -Infinity");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(NumberMathVMTest, NM37_Math_sign_neg_zero) {
    auto v = vm_ok("1 / Math.sign(-0) === -Infinity");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// NM-38: Math.abs(-5) → 5
// ============================================================

TEST(NumberMathInterpTest, NM38_Math_abs_negative) {
    auto v = interp_ok("Math.abs(-5)");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 5.0);
}

TEST(NumberMathVMTest, NM38_Math_abs_negative) {
    auto v = vm_ok("Math.abs(-5)");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 5.0);
}

// ============================================================
// NM-39: Math.abs(-0) → +0
// ============================================================

TEST(NumberMathInterpTest, NM39_Math_abs_neg_zero) {
    auto v = interp_ok("1 / Math.abs(-0) === Infinity");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(NumberMathVMTest, NM39_Math_abs_neg_zero) {
    auto v = vm_ok("1 / Math.abs(-0) === Infinity");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// NM-40: Math.trunc(-4.7) → -4
// ============================================================

TEST(NumberMathInterpTest, NM40_Math_trunc) {
    auto v = interp_ok("Math.trunc(-4.7)");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), -4.0);
}

TEST(NumberMathVMTest, NM40_Math_trunc) {
    auto v = vm_ok("Math.trunc(-4.7)");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), -4.0);
}

// ============================================================
// NM-41: Math.floor(-4.7) → -5
// ============================================================

TEST(NumberMathInterpTest, NM41_Math_floor_negative) {
    auto v = interp_ok("Math.floor(-4.7)");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), -5.0);
}

TEST(NumberMathVMTest, NM41_Math_floor_negative) {
    auto v = vm_ok("Math.floor(-4.7)");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), -5.0);
}

// ============================================================
// NM-42: Math.pow(2, 10) → 1024
// ============================================================

TEST(NumberMathInterpTest, NM42_Math_pow) {
    auto v = interp_ok("Math.pow(2, 10)");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 1024.0);
}

TEST(NumberMathVMTest, NM42_Math_pow) {
    auto v = vm_ok("Math.pow(2, 10)");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 1024.0);
}

// ============================================================
// NM-43: Math.pow(NaN, 0) → 1
// ============================================================

TEST(NumberMathInterpTest, NM43_Math_pow_NaN_zero) {
    auto v = interp_ok("Math.pow(NaN, 0)");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 1.0);
}

TEST(NumberMathVMTest, NM43_Math_pow_NaN_zero) {
    auto v = vm_ok("Math.pow(NaN, 0)");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 1.0);
}

// ============================================================
// NM-44: Math.sqrt(4) → 2
// ============================================================

TEST(NumberMathInterpTest, NM44_Math_sqrt) {
    auto v = interp_ok("Math.sqrt(4)");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 2.0);
}

TEST(NumberMathVMTest, NM44_Math_sqrt) {
    auto v = vm_ok("Math.sqrt(4)");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 2.0);
}

// ============================================================
// NM-45: Math.log(1) → 0
// ============================================================

TEST(NumberMathInterpTest, NM45_Math_log) {
    auto v = interp_ok("Math.log(1)");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 0.0);
}

TEST(NumberMathVMTest, NM45_Math_log) {
    auto v = vm_ok("Math.log(1)");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 0.0);
}

// ============================================================
// NM-46: Math.random() in [0, 1)
// ============================================================

TEST(NumberMathInterpTest, NM46_Math_random_range) {
    // Run 10 times to verify range
    for (int i = 0; i < 10; ++i) {
        auto v = interp_ok("Math.random()");
        EXPECT_TRUE(v.is_number());
        EXPECT_GE(v.as_number(), 0.0);
        EXPECT_LT(v.as_number(), 1.0);
    }
}

TEST(NumberMathVMTest, NM46_Math_random_range) {
    for (int i = 0; i < 10; ++i) {
        auto v = vm_ok("Math.random()");
        EXPECT_TRUE(v.is_number());
        EXPECT_GE(v.as_number(), 0.0);
        EXPECT_LT(v.as_number(), 1.0);
    }
}

// ============================================================
// NM-47: Math.PI 常量
// ============================================================

TEST(NumberMathInterpTest, NM47_Math_PI) {
    auto v = interp_ok("Math.PI > 3.14 && Math.PI < 3.15");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(NumberMathVMTest, NM47_Math_PI) {
    auto v = vm_ok("Math.PI > 3.14 && Math.PI < 3.15");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// NM-48: Math.E 常量
// ============================================================

TEST(NumberMathInterpTest, NM48_Math_E) {
    auto v = interp_ok("Math.E > 2.71 && Math.E < 2.72");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(NumberMathVMTest, NM48_Math_E) {
    auto v = vm_ok("Math.E > 2.71 && Math.E < 2.72");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// NM-49: Math.min(-0, +0) → -0
// ============================================================

TEST(NumberMathInterpTest, NM49_Math_min_neg_zero) {
    auto v = interp_ok("1 / Math.min(-0, 0) === -Infinity");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(NumberMathVMTest, NM49_Math_min_neg_zero) {
    auto v = vm_ok("1 / Math.min(-0, 0) === -Infinity");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// NM-50: Math.max(-0, +0) → +0
// ============================================================

TEST(NumberMathInterpTest, NM50_Math_max_pos_zero) {
    auto v = interp_ok("1 / Math.max(-0, 0) === Infinity");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(NumberMathVMTest, NM50_Math_max_pos_zero) {
    auto v = vm_ok("1 / Math.max(-0, 0) === Infinity");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// NM-51: Number() 无参 → 0
// ============================================================

TEST(NumberMathInterpTest, NM51_Number_no_arg) {
    auto v = interp_ok("Number()");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 0.0);
}

TEST(NumberMathVMTest, NM51_Number_no_arg) {
    auto v = vm_ok("Number()");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 0.0);
}

// ============================================================
// NM-52: Number("42") → 42
// ============================================================

TEST(NumberMathInterpTest, NM52_Number_string) {
    auto v = interp_ok("Number(\"42\")");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 42.0);
}

TEST(NumberMathVMTest, NM52_Number_string) {
    auto v = vm_ok("Number(\"42\")");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 42.0);
}

// ============================================================
// NM-53: Number.isNaN(NaN) === true
// ============================================================

TEST(NumberMathInterpTest, NM53_Number_isNaN_NaN) {
    auto v = interp_ok("Number.isNaN(NaN)");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(NumberMathVMTest, NM53_Number_isNaN_NaN) {
    auto v = vm_ok("Number.isNaN(NaN)");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// NM-54: Math.sign(-3) → -1
// ============================================================

TEST(NumberMathInterpTest, NM54_Math_sign_negative) {
    auto v = interp_ok("Math.sign(-3)");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), -1.0);
}

TEST(NumberMathVMTest, NM54_Math_sign_negative) {
    auto v = vm_ok("Math.sign(-3)");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), -1.0);
}

// ============================================================
// NM-55: parseInt 前导空白+正号
// ============================================================

TEST(NumberMathInterpTest, NM55_parseInt_leading_space_plus_sign) {
    auto v = interp_ok("parseInt(\"  +42  \")");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 42.0);
}

TEST(NumberMathVMTest, NM55_parseInt_leading_space_plus_sign) {
    auto v = vm_ok("parseInt(\"  +42  \")");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 42.0);
}

// ============================================================
// NM-56: parseInt 负号
// ============================================================

TEST(NumberMathInterpTest, NM56_parseInt_negative_sign) {
    auto v = interp_ok("parseInt(\"-10\")");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), -10.0);
}

TEST(NumberMathVMTest, NM56_parseInt_negative_sign) {
    auto v = vm_ok("parseInt(\"-10\")");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), -10.0);
}

// ============================================================
// NM-57: parseInt("Infinity") → NaN
// ============================================================

TEST(NumberMathInterpTest, NM57_parseInt_Infinity_string) {
    auto v = interp_ok("parseInt(\"Infinity\")");
    EXPECT_TRUE(v.is_number());
    EXPECT_TRUE(std::isnan(v.as_number()));
}

TEST(NumberMathVMTest, NM57_parseInt_Infinity_string) {
    auto v = vm_ok("parseInt(\"Infinity\")");
    EXPECT_TRUE(v.is_number());
    EXPECT_TRUE(std::isnan(v.as_number()));
}

// ============================================================
// NM-58: parseInt("0o10") → 0（"0o" 不是十六进制，解析到 0 停止）
// ============================================================

TEST(NumberMathInterpTest, NM58_parseInt_octal_prefix) {
    auto v = interp_ok("parseInt(\"0o10\")");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 0.0);
}

TEST(NumberMathVMTest, NM58_parseInt_octal_prefix) {
    auto v = vm_ok("parseInt(\"0o10\")");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 0.0);
}

// ============================================================
// NM-59: parseInt("z", 36) → 35
// ============================================================

TEST(NumberMathInterpTest, NM59_parseInt_base36_max_letter) {
    auto v = interp_ok("parseInt(\"z\", 36)");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 35.0);
}

TEST(NumberMathVMTest, NM59_parseInt_base36_max_letter) {
    auto v = vm_ok("parseInt(\"z\", 36)");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 35.0);
}

// ============================================================
// NM-60: parseInt radix=1 → NaN
// ============================================================

TEST(NumberMathInterpTest, NM60_parseInt_radix_1) {
    auto v = interp_ok("parseInt(\"10\", 1)");
    EXPECT_TRUE(v.is_number());
    EXPECT_TRUE(std::isnan(v.as_number()));
}

TEST(NumberMathVMTest, NM60_parseInt_radix_1) {
    auto v = vm_ok("parseInt(\"10\", 1)");
    EXPECT_TRUE(v.is_number());
    EXPECT_TRUE(std::isnan(v.as_number()));
}

// ============================================================
// NM-61: parseInt radix=37 → NaN
// ============================================================

TEST(NumberMathInterpTest, NM61_parseInt_radix_37) {
    auto v = interp_ok("parseInt(\"10\", 37)");
    EXPECT_TRUE(v.is_number());
    EXPECT_TRUE(std::isnan(v.as_number()));
}

TEST(NumberMathVMTest, NM61_parseInt_radix_37) {
    auto v = vm_ok("parseInt(\"10\", 37)");
    EXPECT_TRUE(v.is_number());
    EXPECT_TRUE(std::isnan(v.as_number()));
}

// ============================================================
// NM-62: parseInt("10", 0) → 10（radix=0 默认十进制）
// ============================================================

TEST(NumberMathInterpTest, NM62_parseInt_radix_0) {
    auto v = interp_ok("parseInt(\"10\", 0)");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 10.0);
}

TEST(NumberMathVMTest, NM62_parseInt_radix_0) {
    auto v = vm_ok("parseInt(\"10\", 0)");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 10.0);
}

// ============================================================
// NM-63: parseFloat("Infinity") → Infinity
// ============================================================

TEST(NumberMathInterpTest, NM63_parseFloat_Infinity) {
    auto v = interp_ok("parseFloat(\"Infinity\")");
    EXPECT_TRUE(v.is_number());
    EXPECT_TRUE(std::isinf(v.as_number()) && v.as_number() > 0);
}

TEST(NumberMathVMTest, NM63_parseFloat_Infinity) {
    auto v = vm_ok("parseFloat(\"Infinity\")");
    EXPECT_TRUE(v.is_number());
    EXPECT_TRUE(std::isinf(v.as_number()) && v.as_number() > 0);
}

// ============================================================
// NM-64: parseFloat("-Infinity") → -Infinity
// ============================================================

TEST(NumberMathInterpTest, NM64_parseFloat_neg_Infinity) {
    auto v = interp_ok("parseFloat(\"-Infinity\")");
    EXPECT_TRUE(v.is_number());
    EXPECT_TRUE(std::isinf(v.as_number()) && v.as_number() < 0);
}

TEST(NumberMathVMTest, NM64_parseFloat_neg_Infinity) {
    auto v = vm_ok("parseFloat(\"-Infinity\")");
    EXPECT_TRUE(v.is_number());
    EXPECT_TRUE(std::isinf(v.as_number()) && v.as_number() < 0);
}

// ============================================================
// NM-65: parseFloat(".5") → 0.5（无整数部分）
// ============================================================

TEST(NumberMathInterpTest, NM65_parseFloat_leading_dot) {
    auto v = interp_ok("parseFloat(\".5\")");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 0.5);
}

TEST(NumberMathVMTest, NM65_parseFloat_leading_dot) {
    auto v = vm_ok("parseFloat(\".5\")");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 0.5);
}

// ============================================================
// NM-66: parseFloat("1e3") → 1000（科学计数法）
// ============================================================

TEST(NumberMathInterpTest, NM66_parseFloat_scientific) {
    auto v = interp_ok("parseFloat(\"1e3\")");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 1000.0);
}

TEST(NumberMathVMTest, NM66_parseFloat_scientific) {
    auto v = vm_ok("parseFloat(\"1e3\")");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 1000.0);
}

// ============================================================
// NM-67: parseFloat("1.5e-2") → 0.015
// ============================================================

TEST(NumberMathInterpTest, NM67_parseFloat_scientific_negative_exp) {
    auto v = interp_ok("parseFloat(\"1.5e-2\")");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 0.015);
}

TEST(NumberMathVMTest, NM67_parseFloat_scientific_negative_exp) {
    auto v = vm_ok("parseFloat(\"1.5e-2\")");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 0.015);
}

// ============================================================
// NM-68: Number.isFinite(0) → true
// ============================================================

TEST(NumberMathInterpTest, NM68_Number_isFinite_zero) {
    auto v = interp_ok("Number.isFinite(0)");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(NumberMathVMTest, NM68_Number_isFinite_zero) {
    auto v = vm_ok("Number.isFinite(0)");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// NM-69: Number.isFinite(-0) → true
// ============================================================

TEST(NumberMathInterpTest, NM69_Number_isFinite_neg_zero) {
    auto v = interp_ok("Number.isFinite(-0)");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(NumberMathVMTest, NM69_Number_isFinite_neg_zero) {
    auto v = vm_ok("Number.isFinite(-0)");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// NM-70: Number.isFinite(1/0) → false（Infinity）
// ============================================================

TEST(NumberMathInterpTest, NM70_Number_isFinite_div_zero) {
    auto v = interp_ok("Number.isFinite(1/0)");
    EXPECT_TRUE(v.is_bool());
    EXPECT_FALSE(v.as_bool());
}

TEST(NumberMathVMTest, NM70_Number_isFinite_div_zero) {
    auto v = vm_ok("Number.isFinite(1/0)");
    EXPECT_TRUE(v.is_bool());
    EXPECT_FALSE(v.as_bool());
}

// ============================================================
// NM-71: Math.round(-2.5) → -2（向+∞，不是 -3）
// ============================================================

TEST(NumberMathInterpTest, NM71_Math_round_neg_two_half) {
    auto v = interp_ok("Math.round(-2.5)");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), -2.0);
}

TEST(NumberMathVMTest, NM71_Math_round_neg_two_half) {
    auto v = vm_ok("Math.round(-2.5)");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), -2.0);
}

// ============================================================
// NM-72: Math.round(NaN) → NaN
// ============================================================

TEST(NumberMathInterpTest, NM72_Math_round_NaN) {
    auto v = interp_ok("Math.round(NaN)");
    EXPECT_TRUE(v.is_number());
    EXPECT_TRUE(std::isnan(v.as_number()));
}

TEST(NumberMathVMTest, NM72_Math_round_NaN) {
    auto v = vm_ok("Math.round(NaN)");
    EXPECT_TRUE(v.is_number());
    EXPECT_TRUE(std::isnan(v.as_number()));
}

// ============================================================
// NM-73: Math.round(Infinity) → Infinity
// ============================================================

TEST(NumberMathInterpTest, NM73_Math_round_Infinity) {
    auto v = interp_ok("Math.round(Infinity)");
    EXPECT_TRUE(v.is_number());
    EXPECT_TRUE(std::isinf(v.as_number()) && v.as_number() > 0);
}

TEST(NumberMathVMTest, NM73_Math_round_Infinity) {
    auto v = vm_ok("Math.round(Infinity)");
    EXPECT_TRUE(v.is_number());
    EXPECT_TRUE(std::isinf(v.as_number()) && v.as_number() > 0);
}

// ============================================================
// NM-74: Math.max(1, 2, 3) → 3（多参数）
// ============================================================

TEST(NumberMathInterpTest, NM74_Math_max_multi_args) {
    auto v = interp_ok("Math.max(1, 2, 3)");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 3.0);
}

TEST(NumberMathVMTest, NM74_Math_max_multi_args) {
    auto v = vm_ok("Math.max(1, 2, 3)");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 3.0);
}

// ============================================================
// NM-75: Math.min(1, 2, 3) → 1（多参数）
// ============================================================

TEST(NumberMathInterpTest, NM75_Math_min_multi_args) {
    auto v = interp_ok("Math.min(1, 2, 3)");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 1.0);
}

TEST(NumberMathVMTest, NM75_Math_min_multi_args) {
    auto v = vm_ok("Math.min(1, 2, 3)");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 1.0);
}

// ============================================================
// NM-76: Math.pow(1, NaN) → 1（C std::pow 规范）
// ============================================================

TEST(NumberMathInterpTest, NM76_Math_pow_one_NaN) {
    auto v = interp_ok("Math.pow(1, NaN)");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 1.0);
}

TEST(NumberMathVMTest, NM76_Math_pow_one_NaN) {
    auto v = vm_ok("Math.pow(1, NaN)");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 1.0);
}

// ============================================================
// NM-77: Math.pow(-7, 0.5) → NaN（负底数非整数指数）
// ============================================================

TEST(NumberMathInterpTest, NM77_Math_pow_neg_base_frac_exp) {
    auto v = interp_ok("Math.pow(-7, 0.5)");
    EXPECT_TRUE(v.is_number());
    EXPECT_TRUE(std::isnan(v.as_number()));
}

TEST(NumberMathVMTest, NM77_Math_pow_neg_base_frac_exp) {
    auto v = vm_ok("Math.pow(-7, 0.5)");
    EXPECT_TRUE(v.is_number());
    EXPECT_TRUE(std::isnan(v.as_number()));
}

// ============================================================
// NM-78: Math.pow(Infinity, 0) → 1
// ============================================================

TEST(NumberMathInterpTest, NM78_Math_pow_Infinity_zero) {
    auto v = interp_ok("Math.pow(Infinity, 0)");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 1.0);
}

TEST(NumberMathVMTest, NM78_Math_pow_Infinity_zero) {
    auto v = vm_ok("Math.pow(Infinity, 0)");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 1.0);
}

// ============================================================
// NM-79: Math.sign(Infinity) → 1
// ============================================================

TEST(NumberMathInterpTest, NM79_Math_sign_Infinity) {
    auto v = interp_ok("Math.sign(Infinity)");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 1.0);
}

TEST(NumberMathVMTest, NM79_Math_sign_Infinity) {
    auto v = vm_ok("Math.sign(Infinity)");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 1.0);
}

// ============================================================
// NM-80: Math.sign(-Infinity) → -1
// ============================================================

TEST(NumberMathInterpTest, NM80_Math_sign_neg_Infinity) {
    auto v = interp_ok("Math.sign(-Infinity)");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), -1.0);
}

TEST(NumberMathVMTest, NM80_Math_sign_neg_Infinity) {
    auto v = vm_ok("Math.sign(-Infinity)");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), -1.0);
}

// ============================================================
// NM-81: Math.sign(NaN) → NaN
// ============================================================

TEST(NumberMathInterpTest, NM81_Math_sign_NaN) {
    auto v = interp_ok("Math.sign(NaN)");
    EXPECT_TRUE(v.is_number());
    EXPECT_TRUE(std::isnan(v.as_number()));
}

TEST(NumberMathVMTest, NM81_Math_sign_NaN) {
    auto v = vm_ok("Math.sign(NaN)");
    EXPECT_TRUE(v.is_number());
    EXPECT_TRUE(std::isnan(v.as_number()));
}

// ============================================================
// NM-82: Math.sign(+0) → +0
// ============================================================

TEST(NumberMathInterpTest, NM82_Math_sign_pos_zero) {
    // +0: 1/Math.sign(+0) === Infinity
    auto v = interp_ok("1 / Math.sign(0) === Infinity");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(NumberMathVMTest, NM82_Math_sign_pos_zero) {
    auto v = vm_ok("1 / Math.sign(0) === Infinity");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// NM-83: Math.log(-1) → NaN
// ============================================================

TEST(NumberMathInterpTest, NM83_Math_log_negative) {
    auto v = interp_ok("Math.log(-1)");
    EXPECT_TRUE(v.is_number());
    EXPECT_TRUE(std::isnan(v.as_number()));
}

TEST(NumberMathVMTest, NM83_Math_log_negative) {
    auto v = vm_ok("Math.log(-1)");
    EXPECT_TRUE(v.is_number());
    EXPECT_TRUE(std::isnan(v.as_number()));
}

// ============================================================
// NM-84: Math.log(0) → -Infinity
// ============================================================

TEST(NumberMathInterpTest, NM84_Math_log_zero) {
    auto v = interp_ok("Math.log(0)");
    EXPECT_TRUE(v.is_number());
    EXPECT_TRUE(std::isinf(v.as_number()) && v.as_number() < 0);
}

TEST(NumberMathVMTest, NM84_Math_log_zero) {
    auto v = vm_ok("Math.log(0)");
    EXPECT_TRUE(v.is_number());
    EXPECT_TRUE(std::isinf(v.as_number()) && v.as_number() < 0);
}

// ============================================================
// NM-85: Math.log(Infinity) → Infinity
// ============================================================

TEST(NumberMathInterpTest, NM85_Math_log_Infinity) {
    auto v = interp_ok("Math.log(Infinity)");
    EXPECT_TRUE(v.is_number());
    EXPECT_TRUE(std::isinf(v.as_number()) && v.as_number() > 0);
}

TEST(NumberMathVMTest, NM85_Math_log_Infinity) {
    auto v = vm_ok("Math.log(Infinity)");
    EXPECT_TRUE(v.is_number());
    EXPECT_TRUE(std::isinf(v.as_number()) && v.as_number() > 0);
}

// ============================================================
// NM-86: isNaN(null) → false（ToNumber(null)=0）
// ============================================================

TEST(NumberMathInterpTest, NM86_global_isNaN_null) {
    auto v = interp_ok("isNaN(null)");
    EXPECT_TRUE(v.is_bool());
    EXPECT_FALSE(v.as_bool());
}

TEST(NumberMathVMTest, NM86_global_isNaN_null) {
    auto v = vm_ok("isNaN(null)");
    EXPECT_TRUE(v.is_bool());
    EXPECT_FALSE(v.as_bool());
}

// ============================================================
// NM-87: isFinite(null) → true（ToNumber(null)=0）
// ============================================================

TEST(NumberMathInterpTest, NM87_global_isFinite_null) {
    auto v = interp_ok("isFinite(null)");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(NumberMathVMTest, NM87_global_isFinite_null) {
    auto v = vm_ok("isFinite(null)");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// NM-88: isFinite("1.5") → true（ToNumber("1.5")=1.5）
// ============================================================

TEST(NumberMathInterpTest, NM88_global_isFinite_string_float) {
    auto v = interp_ok("isFinite(\"1.5\")");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(NumberMathVMTest, NM88_global_isFinite_string_float) {
    auto v = vm_ok("isFinite(\"1.5\")");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// NM-89: Number("3.14") → 3.14
// ============================================================

TEST(NumberMathInterpTest, NM89_Number_float_string) {
    auto v = interp_ok("Number(\"3.14\")");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 3.14);
}

TEST(NumberMathVMTest, NM89_Number_float_string) {
    auto v = vm_ok("Number(\"3.14\")");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 3.14);
}

// ============================================================
// NM-90: Number(true) → 1
// ============================================================

TEST(NumberMathInterpTest, NM90_Number_true) {
    auto v = interp_ok("Number(true)");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 1.0);
}

TEST(NumberMathVMTest, NM90_Number_true) {
    auto v = vm_ok("Number(true)");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 1.0);
}

// ============================================================
// NM-91: Number(false) → 0
// ============================================================

TEST(NumberMathInterpTest, NM91_Number_false) {
    auto v = interp_ok("Number(false)");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 0.0);
}

TEST(NumberMathVMTest, NM91_Number_false) {
    auto v = vm_ok("Number(false)");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 0.0);
}

// ============================================================
// NM-92: Number(null) → 0
// ============================================================

TEST(NumberMathInterpTest, NM92_Number_null) {
    auto v = interp_ok("Number(null)");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 0.0);
}

TEST(NumberMathVMTest, NM92_Number_null) {
    auto v = vm_ok("Number(null)");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 0.0);
}

// ============================================================
// NM-93: Number(undefined) → NaN
// ============================================================

TEST(NumberMathInterpTest, NM93_Number_undefined) {
    auto v = interp_ok("Number(undefined)");
    EXPECT_TRUE(v.is_number());
    EXPECT_TRUE(std::isnan(v.as_number()));
}

TEST(NumberMathVMTest, NM93_Number_undefined) {
    auto v = vm_ok("Number(undefined)");
    EXPECT_TRUE(v.is_number());
    EXPECT_TRUE(std::isnan(v.as_number()));
}

// ============================================================
// NM-94: Number("") → 0
// ============================================================

TEST(NumberMathInterpTest, NM94_Number_empty_string) {
    auto v = interp_ok("Number(\"\")");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 0.0);
}

TEST(NumberMathVMTest, NM94_Number_empty_string) {
    auto v = vm_ok("Number(\"\")");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 0.0);
}

// ============================================================
// NM-95: parseFloat("0x10") → 0（规范：只解析十进制，遇 0x 停在 "0"）
// ============================================================

TEST(NumberMathInterpTest, NM95_parseFloat_hex_prefix) {
    auto v = interp_ok("parseFloat(\"0x10\")");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 0.0);
}

TEST(NumberMathVMTest, NM95_parseFloat_hex_prefix) {
    auto v = vm_ok("parseFloat(\"0x10\")");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 0.0);
}

// ============================================================
// NM-96: parseInt("9007199254740993") 不崩溃（超过 MAX_SAFE_INTEGER）
// ============================================================

TEST(NumberMathInterpTest, NM96_parseInt_large_int) {
    auto v = interp_ok("parseInt(\"9007199254740993\")");
    EXPECT_TRUE(v.is_number());
    // 精度损失可接受，只要不崩溃且结果是有限数
    EXPECT_TRUE(std::isfinite(v.as_number()));
}

TEST(NumberMathVMTest, NM96_parseInt_large_int) {
    auto v = vm_ok("parseInt(\"9007199254740993\")");
    EXPECT_TRUE(v.is_number());
    EXPECT_TRUE(std::isfinite(v.as_number()));
}

// ============================================================
// NM-97: Number.prototype 存在
// ============================================================

TEST(NumberMathInterpTest, NM97_Number_prototype_exists) {
    auto v = interp_ok("typeof Number.prototype");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "object");
}

TEST(NumberMathVMTest, NM97_Number_prototype_exists) {
    auto v = vm_ok("typeof Number.prototype");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "object");
}

}  // namespace
