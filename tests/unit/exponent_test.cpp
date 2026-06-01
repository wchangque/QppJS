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

static bool parse_fails(std::string_view src) {
    return !parse_program(src).ok();
}

// ============================================================
// EXP-01: 2 ** 3 → 8
// ============================================================

TEST(Exponent, EXP01_BasicPowInterp) {
    auto v = interp_ok("2 ** 3");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 8.0);
}

TEST(Exponent, EXP01_BasicPowVM) {
    auto v = vm_ok("2 ** 3");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 8.0);
}

// ============================================================
// EXP-02: 2 ** 0 → 1
// ============================================================

TEST(Exponent, EXP02_ZeroExpInterp) {
    auto v = interp_ok("2 ** 0");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 1.0);
}

TEST(Exponent, EXP02_ZeroExpVM) {
    auto v = vm_ok("2 ** 0");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 1.0);
}

// ============================================================
// EXP-03: 2 ** -1 → 0.5
// ============================================================

TEST(Exponent, EXP03_NegExpInterp) {
    auto v = interp_ok("2 ** -1");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 0.5);
}

TEST(Exponent, EXP03_NegExpVM) {
    auto v = vm_ok("2 ** -1");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 0.5);
}

// ============================================================
// EXP-04: 2 ** 3 ** 2 → 512（右结合：2 ** (3 ** 2) = 2 ** 9）
// ============================================================

TEST(Exponent, EXP04_RightAssocInterp) {
    auto v = interp_ok("2 ** 3 ** 2");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 512.0);
}

TEST(Exponent, EXP04_RightAssocVM) {
    auto v = vm_ok("2 ** 3 ** 2");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 512.0);
}

// ============================================================
// EXP-05: (-2) ** 3 → -8
// ============================================================

TEST(Exponent, EXP05_NegBaseOddInterp) {
    auto v = interp_ok("(-2) ** 3");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), -8.0);
}

TEST(Exponent, EXP05_NegBaseOddVM) {
    auto v = vm_ok("(-2) ** 3");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), -8.0);
}

// ============================================================
// EXP-06: (-2) ** 2 → 4
// ============================================================

TEST(Exponent, EXP06_NegBaseEvenInterp) {
    auto v = interp_ok("(-2) ** 2");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 4.0);
}

TEST(Exponent, EXP06_NegBaseEvenVM) {
    auto v = vm_ok("(-2) ** 2");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 4.0);
}

// ============================================================
// EXP-07: 1 ** NaN → 1（Math.pow 语义：底数为 1，任意指数）
// ============================================================

TEST(Exponent, EXP07_OneNaNInterp) {
    auto v = interp_ok("1 ** NaN");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 1.0);
}

TEST(Exponent, EXP07_OneNaNVM) {
    auto v = vm_ok("1 ** NaN");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 1.0);
}

// ============================================================
// EXP-08: NaN ** 0 → 1（任意底数的 0 次幂）
// ============================================================

TEST(Exponent, EXP08_NaNZeroInterp) {
    auto v = interp_ok("NaN ** 0");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 1.0);
}

TEST(Exponent, EXP08_NaNZeroVM) {
    auto v = vm_ok("NaN ** 0");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 1.0);
}

// ============================================================
// EXP-09: x **= 3 → 复合赋值
// ============================================================

TEST(Exponent, EXP09_CompoundAssignInterp) {
    auto v = interp_ok("let x = 2; x **= 3; x");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 8.0);
}

TEST(Exponent, EXP09_CompoundAssignVM) {
    auto v = vm_ok("let x = 2; x **= 3; x");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 8.0);
}

// ============================================================
// EXP-10: -x ** y → SyntaxError（Parser 层级检查）
// ============================================================

TEST(Exponent, EXP10_UnaryLhsSyntaxError) {
    EXPECT_TRUE(parse_fails("-x ** y"));
}

TEST(Exponent, EXP10_UnaryPlusSyntaxError) {
    EXPECT_TRUE(parse_fails("+x ** y"));
}

TEST(Exponent, EXP10_UnaryBangSyntaxError) {
    EXPECT_TRUE(parse_fails("!x ** y"));
}

// ============================================================
// EXP-11: obj.x **= 2 → 成员表达式复合赋值
// ============================================================

TEST(Exponent, EXP11_MemberCompoundAssignInterp) {
    auto v = interp_ok("let obj = {x: 3}; obj.x **= 2; obj.x");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 9.0);
}

TEST(Exponent, EXP11_MemberCompoundAssignVM) {
    auto v = vm_ok("let obj = {x: 3}; obj.x **= 2; obj.x");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 9.0);
}

}  // namespace
