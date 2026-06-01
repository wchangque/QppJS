#include "qppjs/frontend/parser.h"
#include "qppjs/frontend/ast.h"
#include "qppjs/runtime/interpreter.h"
#include "qppjs/vm/compiler.h"
#include "qppjs/vm/vm.h"

#include <gtest/gtest.h>
#include <string>

using namespace qppjs;

static Value interp_eval(const char* src) {
    auto parse_result = parse_program(std::string(src));
    if (!parse_result.ok()) return Value::undefined();
    Interpreter interp;
    auto result = interp.exec(parse_result.value());
    if (!result.is_ok()) return Value::undefined();
    return result.value();
}

static Value vm_eval(const char* src) {
    auto parse_result = parse_program(std::string(src));
    if (!parse_result.ok()) return Value::undefined();
    Compiler compiler;
    auto bytecode = compiler.compile(parse_result.value());
    if (!bytecode) return Value::undefined();
    VM vm;
    auto result = vm.exec(bytecode);
    if (!result.is_ok()) return Value::undefined();
    return result.value();
}

static bool parse_ok(const char* src) {
    return parse_program(std::string(src)).ok();
}

// ============================================================
// LA-01: x &&= y, x truthy → assigns
// ============================================================
TEST(LogicalAssignInterp, LA01_AndAssign_Truthy) {
    auto v = interp_eval("let x = 1; x &&= 42; x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 42.0);
}
TEST(LogicalAssignVM, LA01_AndAssign_Truthy) {
    auto v = vm_eval("let x = 1; x &&= 42; x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 42.0);
}

// ============================================================
// LA-02: x &&= y, x falsy → no assignment (short-circuit)
// ============================================================
TEST(LogicalAssignInterp, LA02_AndAssign_Falsy) {
    auto v = interp_eval("let x = 0; x &&= 42; x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}
TEST(LogicalAssignVM, LA02_AndAssign_Falsy) {
    auto v = vm_eval("let x = 0; x &&= 42; x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}

// ============================================================
// LA-03: x ||= y, x falsy → assigns
// ============================================================
TEST(LogicalAssignInterp, LA03_OrAssign_Falsy) {
    auto v = interp_eval("let x = 0; x ||= 42; x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 42.0);
}
TEST(LogicalAssignVM, LA03_OrAssign_Falsy) {
    auto v = vm_eval("let x = 0; x ||= 42; x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 42.0);
}

// ============================================================
// LA-04: x ||= y, x truthy → no assignment (short-circuit)
// ============================================================
TEST(LogicalAssignInterp, LA04_OrAssign_Truthy) {
    auto v = interp_eval("let x = 1; x ||= 42; x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}
TEST(LogicalAssignVM, LA04_OrAssign_Truthy) {
    auto v = vm_eval("let x = 1; x ||= 42; x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

// ============================================================
// LA-05: x ?\?= y, x null → assigns
// ============================================================
TEST(LogicalAssignInterp, LA05_NullishAssign_Null) {
    auto v = interp_eval("let x = null; x ?\?= 42; x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 42.0);
}
TEST(LogicalAssignVM, LA05_NullishAssign_Null) {
    auto v = vm_eval("let x = null; x ?\?= 42; x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 42.0);
}

// ============================================================
// LA-06: x ?\?= y, x undefined → assigns
// ============================================================
TEST(LogicalAssignInterp, LA06_NullishAssign_Undefined) {
    auto v = interp_eval("let x; x ?\?= 42; x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 42.0);
}
TEST(LogicalAssignVM, LA06_NullishAssign_Undefined) {
    auto v = vm_eval("let x; x ?\?= 42; x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 42.0);
}

// ============================================================
// LA-07: x ?\?= y, x = 0 (falsy but not nullish) → no assignment
// ============================================================
TEST(LogicalAssignInterp, LA07_NullishAssign_Zero) {
    auto v = interp_eval("let x = 0; x ?\?= 42; x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}
TEST(LogicalAssignVM, LA07_NullishAssign_Zero) {
    auto v = vm_eval("let x = 0; x ?\?= 42; x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}

// ============================================================
// LA-08: x ?\?= y, x = false (falsy but not nullish) → no assignment
// ============================================================
TEST(LogicalAssignInterp, LA08_NullishAssign_False) {
    auto v = interp_eval("let x = false; x ?\?= 42; x");
    EXPECT_TRUE(v.is_bool());
    EXPECT_EQ(v.as_bool(), false);
}
TEST(LogicalAssignVM, LA08_NullishAssign_False) {
    auto v = vm_eval("let x = false; x ?\?= 42; x");
    EXPECT_TRUE(v.is_bool());
    EXPECT_EQ(v.as_bool(), false);
}

// ============================================================
// LA-09: getter short-circuit - getter called once regardless
// ============================================================
TEST(LogicalAssignInterp, LA09_GetterCalledOnce) {
    auto v = interp_eval(
        "let count = 0;"
        "let obj = {};"
        "Object.defineProperty(obj, 'x', { get() { count++; return 1; }, set(v) {}, configurable: true });"
        "obj.x &&= 99;"  // x is 1 (truthy), so setter called; getter called once
        "count"
    );
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}
TEST(LogicalAssignVM, LA09_GetterCalledOnce) {
    auto v = vm_eval(
        "let count = 0;"
        "let obj = {};"
        "Object.defineProperty(obj, 'x', { get() { count++; return 1; }, set(v) {}, configurable: true });"
        "obj.x &&= 99;"
        "count"
    );
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

// ============================================================
// LA-10: setter not called on short-circuit
// ============================================================
TEST(LogicalAssignInterp, LA10_SetterNotCalledOnShortCircuit) {
    auto v = interp_eval(
        "let setCount = 0;"
        "let obj = {};"
        "Object.defineProperty(obj, 'x', { get() { return 0; }, set(v) { setCount++; }, configurable: true });"
        "obj.x &&= 99;"  // x is 0 (falsy), short-circuit, setter not called
        "setCount"
    );
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}
TEST(LogicalAssignVM, LA10_SetterNotCalledOnShortCircuit) {
    auto v = vm_eval(
        "let setCount = 0;"
        "let obj = {};"
        "Object.defineProperty(obj, 'x', { get() { return 0; }, set(v) { setCount++; }, configurable: true });"
        "obj.x &&= 99;"
        "setCount"
    );
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}

// ============================================================
// LA-11: member expression LHS: obj.x &&= y
// ============================================================
TEST(LogicalAssignInterp, LA11_MemberAndAssign) {
    auto v = interp_eval("let obj = { x: 1 }; obj.x &&= 42; obj.x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 42.0);
}
TEST(LogicalAssignVM, LA11_MemberAndAssign) {
    auto v = vm_eval("let obj = { x: 1 }; obj.x &&= 42; obj.x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 42.0);
}

// ============================================================
// LA-12: member expression LHS: obj.x ?\?= y
// ============================================================
TEST(LogicalAssignInterp, LA12_MemberNullishAssign) {
    auto v = interp_eval("let obj = { x: null }; obj.x ?\?= 42; obj.x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 42.0);
}
TEST(LogicalAssignVM, LA12_MemberNullishAssign) {
    auto v = vm_eval("let obj = { x: null }; obj.x ?\?= 42; obj.x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 42.0);
}

// ============================================================
// LA-13: chained: a &&= b &&= c
// ============================================================
TEST(LogicalAssignInterp, LA13_Chained) {
    auto v = interp_eval("let a = 1; let b = 2; a &&= b &&= 99; b");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 99.0);
}
TEST(LogicalAssignVM, LA13_Chained) {
    auto v = vm_eval("let a = 1; let b = 2; a &&= b &&= 99; b");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 99.0);
}

// ============================================================
// LA-14: return value is correct (original value when short-circuited)
// ============================================================
TEST(LogicalAssignInterp, LA14_ReturnValueShortCircuit) {
    auto v = interp_eval("let x = 5; let r = (x &&= 0); r");
    // x is truthy, so r = 0 (assigned)
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}
TEST(LogicalAssignVM, LA14_ReturnValueShortCircuit) {
    auto v = vm_eval("let x = 5; let r = (x &&= 0); r");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}

// LA-14b: short-circuit return value
TEST(LogicalAssignInterp, LA14b_ReturnValueOriginal) {
    auto v = interp_eval("let x = 0; let r = (x &&= 42); r");
    // x is falsy, short-circuit, r = original x = 0
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}
TEST(LogicalAssignVM, LA14b_ReturnValueOriginal) {
    auto v = vm_eval("let x = 0; let r = (x &&= 42); r");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}

// ============================================================
// LA-15: RHS side effect only when not short-circuiting
// ============================================================
TEST(LogicalAssignInterp, LA15_RHSSideEffectShortCircuit) {
    auto v = interp_eval(
        "let count = 0;"
        "function rhs() { count++; return 99; }"
        "let x = 0;"
        "x &&= rhs();"  // x is falsy → short-circuit → rhs() not called
        "count"
    );
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}
TEST(LogicalAssignVM, LA15_RHSSideEffectShortCircuit) {
    auto v = vm_eval(
        "let count = 0;"
        "function rhs() { count++; return 99; }"
        "let x = 0;"
        "x &&= rhs();"
        "count"
    );
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}

TEST(LogicalAssignInterp, LA15b_RHSSideEffectNotShortCircuit) {
    auto v = interp_eval(
        "let count = 0;"
        "function rhs() { count++; return 99; }"
        "let x = 1;"
        "x &&= rhs();"  // x is truthy → assign → rhs() called once
        "count"
    );
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}
TEST(LogicalAssignVM, LA15b_RHSSideEffectNotShortCircuit) {
    auto v = vm_eval(
        "let count = 0;"
        "function rhs() { count++; return 99; }"
        "let x = 1;"
        "x &&= rhs();"
        "count"
    );
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

// ============================================================
// Parser: ensure tokens are recognized
// ============================================================
TEST(LogicalAssignParser, AmpAmpEq_Recognized) {
    EXPECT_TRUE(parse_ok("let x = 1; x &&= 2;"));
}
TEST(LogicalAssignParser, PipePipeEq_Recognized) {
    EXPECT_TRUE(parse_ok("let x = 0; x ||= 2;"));
}
TEST(LogicalAssignParser, QuestionQuestionEq_Recognized) {
    EXPECT_TRUE(parse_ok("let x = null; x ?\?= 2;"));
}
