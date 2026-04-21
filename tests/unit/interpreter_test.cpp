#include "qppjs/frontend/parser.h"
#include "qppjs/runtime/interpreter.h"

#include <gtest/gtest.h>

#include <cmath>
#include <string_view>

namespace {

// Helper: parse + exec, expect success, return Value
qppjs::Value exec_ok(std::string_view source) {
    auto parse_result = qppjs::parse_program(source);
    EXPECT_TRUE(parse_result.ok()) << "parse failed: " << parse_result.error().message();
    qppjs::Interpreter interp;
    auto result = interp.exec(parse_result.value());
    EXPECT_TRUE(result.is_ok()) << "exec failed: " << result.error().message();
    return result.value();
}

// Helper: parse + exec, expect error, return error message
std::string exec_err(std::string_view source) {
    auto parse_result = qppjs::parse_program(source);
    EXPECT_TRUE(parse_result.ok()) << "parse failed: " << parse_result.error().message();
    qppjs::Interpreter interp;
    auto result = interp.exec(parse_result.value());
    EXPECT_FALSE(result.is_ok()) << "expected error but got success";
    return result.error().message();
}

// ---- Basic literals ----

TEST(InterpreterLiterals, NumberLiteral) {
    auto v = exec_ok("42");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 42.0);
}

TEST(InterpreterLiterals, StringLiteral) {
    auto v = exec_ok("\"hello\"");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "hello");
}

TEST(InterpreterLiterals, BooleanTrue) {
    auto v = exec_ok("true");
    EXPECT_TRUE(v.is_bool());
    EXPECT_EQ(v.as_bool(), true);
}

TEST(InterpreterLiterals, BooleanFalse) {
    auto v = exec_ok("false");
    EXPECT_TRUE(v.is_bool());
    EXPECT_EQ(v.as_bool(), false);
}

TEST(InterpreterLiterals, NullLiteral) {
    auto v = exec_ok("null");
    EXPECT_TRUE(v.is_null());
}

// ---- Variable declaration and read ----

TEST(InterpreterVar, LetDecl) {
    auto v = exec_ok("let x = 1; x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

TEST(InterpreterVar, ConstDecl) {
    auto v = exec_ok("const x = 2; x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.0);
}

TEST(InterpreterVar, VarDecl) {
    auto v = exec_ok("var x = 3; x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 3.0);
}

TEST(InterpreterVar, UndeclaredRefError) {
    auto msg = exec_err("x");
    EXPECT_NE(msg.find("ReferenceError"), std::string::npos);
    EXPECT_NE(msg.find("x"), std::string::npos);
}

TEST(InterpreterVar, ConstAssignTypeError) {
    auto msg = exec_err("const x = 1; x = 2");
    EXPECT_NE(msg.find("TypeError"), std::string::npos);
}

TEST(InterpreterVar, VarHoisting) {
    // First read of x should be undefined (hoisted), not an error
    auto v = exec_ok("x; var x = 1; x");
    // Last expression x = 1 after declaration
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

TEST(InterpreterVar, VarHoistingFirstUndefined) {
    // The hoisted var is undefined before initialization
    // We verify by checking the if branch executes falsy path
    auto v = exec_ok("var y; if (y === undefined) { y = 99; } y");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 99.0);
}

// ---- TDZ ----

TEST(InterpreterTdz, InnerShadow) {
    auto v = exec_ok("let x = 1; { let x = 2; x }");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.0);
}

TEST(InterpreterTdz, LetNotVisibleOutside) {
    auto msg = exec_err("{ let x = 1; } x");
    EXPECT_NE(msg.find("ReferenceError"), std::string::npos);
}

// ---- ToBoolean ----

TEST(InterpreterToBoolean, ZeroFalsy) {
    auto v = exec_ok("if (0) 1; else 2");
    EXPECT_EQ(v.as_number(), 2.0);
}

TEST(InterpreterToBoolean, EmptyStringFalsy) {
    auto v = exec_ok("if (\"\") 1; else 2");
    EXPECT_EQ(v.as_number(), 2.0);
}

TEST(InterpreterToBoolean, NonEmptyStringTruthy) {
    auto v = exec_ok("if (\"0\") 1; else 2");
    EXPECT_EQ(v.as_number(), 1.0);
}

TEST(InterpreterToBoolean, NullFalsy) {
    auto v = exec_ok("if (null) 1; else 2");
    EXPECT_EQ(v.as_number(), 2.0);
}

TEST(InterpreterToBoolean, UndefinedFalsy) {
    auto v = exec_ok("if (undefined) 1; else 2");
    EXPECT_EQ(v.as_number(), 2.0);
}

// ---- typeof ----

TEST(InterpreterTypeof, TypeofUndefined) {
    auto v = exec_ok("typeof undefined");
    EXPECT_EQ(v.as_string(), "undefined");
}

TEST(InterpreterTypeof, TypeofNull) {
    auto v = exec_ok("typeof null");
    EXPECT_EQ(v.as_string(), "object");
}

TEST(InterpreterTypeof, TypeofBool) {
    auto v = exec_ok("typeof true");
    EXPECT_EQ(v.as_string(), "boolean");
}

TEST(InterpreterTypeof, TypeofNumber) {
    auto v = exec_ok("typeof 42");
    EXPECT_EQ(v.as_string(), "number");
}

TEST(InterpreterTypeof, TypeofString) {
    auto v = exec_ok("typeof \"str\"");
    EXPECT_EQ(v.as_string(), "string");
}

TEST(InterpreterTypeof, TypeofUndeclared) {
    // typeof undeclaredVar must NOT throw ReferenceError
    auto v = exec_ok("typeof undeclaredVar");
    EXPECT_EQ(v.as_string(), "undefined");
}

// ---- Unary operators ----

TEST(InterpreterUnary, Negate) {
    auto v = exec_ok("-3");
    EXPECT_EQ(v.as_number(), -3.0);
}

TEST(InterpreterUnary, BangTrue) {
    auto v = exec_ok("!true");
    EXPECT_EQ(v.as_bool(), false);
}

TEST(InterpreterUnary, BangFalse) {
    auto v = exec_ok("!false");
    EXPECT_EQ(v.as_bool(), true);
}

TEST(InterpreterUnary, BangZero) {
    auto v = exec_ok("!0");
    EXPECT_EQ(v.as_bool(), true);
}

TEST(InterpreterUnary, VoidZero) {
    auto v = exec_ok("void 0");
    EXPECT_TRUE(v.is_undefined());
}

TEST(InterpreterUnary, UnaryPlusString) {
    auto v = exec_ok("+\"3\"");
    EXPECT_EQ(v.as_number(), 3.0);
}

TEST(InterpreterUnary, UnaryMinusString) {
    auto v = exec_ok("-\"3\"");
    EXPECT_EQ(v.as_number(), -3.0);
}

// ---- Arithmetic ----

TEST(InterpreterArith, AddNumbers) {
    auto v = exec_ok("1 + 2");
    EXPECT_EQ(v.as_number(), 3.0);
}

TEST(InterpreterArith, AddStrings) {
    auto v = exec_ok("\"a\" + \"b\"");
    EXPECT_EQ(v.as_string(), "ab");
}

TEST(InterpreterArith, AddNumberString) {
    auto v = exec_ok("1 + \"2\"");
    EXPECT_EQ(v.as_string(), "12");
}

TEST(InterpreterArith, SubStringNumber) {
    auto v = exec_ok("\"3\" - 1");
    EXPECT_EQ(v.as_number(), 2.0);
}

TEST(InterpreterArith, Div) {
    auto v = exec_ok("10 / 2");
    EXPECT_EQ(v.as_number(), 5.0);
}

TEST(InterpreterArith, DivByZero) {
    auto v = exec_ok("1 / 0");
    EXPECT_TRUE(v.is_number());
    EXPECT_TRUE(std::isinf(v.as_number()));
    EXPECT_GT(v.as_number(), 0.0);
}

TEST(InterpreterArith, ZeroDivZero) {
    auto v = exec_ok("0 / 0");
    EXPECT_TRUE(v.is_number());
    EXPECT_TRUE(std::isnan(v.as_number()));
}

TEST(InterpreterArith, Mod) {
    auto v = exec_ok("10 % 3");
    EXPECT_EQ(v.as_number(), 1.0);
}

// ---- Comparison ----

TEST(InterpreterCmp, LessThan) {
    auto v = exec_ok("1 < 2");
    EXPECT_EQ(v.as_bool(), true);
}

TEST(InterpreterCmp, GreaterThan) {
    auto v = exec_ok("2 > 1");
    EXPECT_EQ(v.as_bool(), true);
}

TEST(InterpreterCmp, LessEq) {
    auto v = exec_ok("1 <= 1");
    EXPECT_EQ(v.as_bool(), true);
}

TEST(InterpreterCmp, NanLtNumber) {
    auto v = exec_ok("0 / 0 < 1");
    EXPECT_EQ(v.as_bool(), false);
}

// ---- Equality ----

TEST(InterpreterEq, StrictEqNumbers) {
    auto v = exec_ok("1 === 1");
    EXPECT_EQ(v.as_bool(), true);
}

TEST(InterpreterEq, StrictEqTypeMismatch) {
    auto v = exec_ok("1 === \"1\"");
    EXPECT_EQ(v.as_bool(), false);
}

TEST(InterpreterEq, NanNotEqNan) {
    auto v = exec_ok("0 / 0 === 0 / 0");
    EXPECT_EQ(v.as_bool(), false);
}

TEST(InterpreterEq, NullEqUndefined) {
    auto v = exec_ok("null == undefined");
    EXPECT_EQ(v.as_bool(), true);
}

TEST(InterpreterEq, NullNotEqZero) {
    auto v = exec_ok("null == 0");
    EXPECT_EQ(v.as_bool(), false);
}

TEST(InterpreterEq, ZeroEqFalse) {
    auto v = exec_ok("0 == false");
    EXPECT_EQ(v.as_bool(), true);
}

TEST(InterpreterEq, StringOneEqNumberOne) {
    auto v = exec_ok("\"1\" == 1");
    EXPECT_EQ(v.as_bool(), true);
}

// ---- Logical operators ----

TEST(InterpreterLogical, AndTruthy) {
    auto v = exec_ok("1 && 2");
    EXPECT_EQ(v.as_number(), 2.0);
}

TEST(InterpreterLogical, AndFalsy) {
    auto v = exec_ok("0 && 2");
    EXPECT_EQ(v.as_number(), 0.0);
}

TEST(InterpreterLogical, OrFalsy) {
    auto v = exec_ok("null || \"default\"");
    EXPECT_EQ(v.as_string(), "default");
}

TEST(InterpreterLogical, OrTruthy) {
    auto v = exec_ok("1 || 2");
    EXPECT_EQ(v.as_number(), 1.0);
}

// ---- Assignment ----

TEST(InterpreterAssign, AddAssign) {
    auto v = exec_ok("let x = 0; x += 5; x");
    EXPECT_EQ(v.as_number(), 5.0);
}

TEST(InterpreterAssign, SubAssign) {
    auto v = exec_ok("let x = 10; x -= 3; x");
    EXPECT_EQ(v.as_number(), 7.0);
}

// ---- Control flow ----

TEST(InterpreterControl, IfTrue) {
    auto v = exec_ok("let x = 0; if (true) { x = 1; } x");
    EXPECT_EQ(v.as_number(), 1.0);
}

TEST(InterpreterControl, IfFalseElse) {
    auto v = exec_ok("let x = 0; if (false) { x = 1; } else { x = 2; } x");
    EXPECT_EQ(v.as_number(), 2.0);
}

TEST(InterpreterControl, WhileLoop) {
    auto v = exec_ok("let i = 0; while (i < 3) { i = i + 1; } i");
    EXPECT_EQ(v.as_number(), 3.0);
}

TEST(InterpreterControl, WhileNotExecuted) {
    auto v = exec_ok("let x = 0; while (false) { x = 1; } x");
    EXPECT_EQ(v.as_number(), 0.0);
}

// ---- Scope ----

TEST(InterpreterScope, InnerLetNoLeak) {
    auto v = exec_ok("let x = 1; { let x = 2; } x");
    EXPECT_EQ(v.as_number(), 1.0);
}

TEST(InterpreterScope, VarSameBinding) {
    auto v = exec_ok("var x = 1; { var x = 2; } x");
    EXPECT_EQ(v.as_number(), 2.0);
}

TEST(InterpreterScope, InnerModifiesOuter) {
    auto v = exec_ok("let x = 1; { x = 2; } x");
    EXPECT_EQ(v.as_number(), 2.0);
}

// ---- Return ----

TEST(InterpreterReturn, TopLevelReturn) {
    auto v = exec_ok("return 42");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 42.0);
}

// ---- TDZ boundary ----

// typeof on a let binding that is in TDZ (declared but not yet initialized)
// should throw ReferenceError, unlike typeof on a completely undeclared variable.
// BUG: Current implementation returns "undefined" for TDZ typeof instead of throwing
// ReferenceError. See eval_unary: `!b->initialized` path returns "undefined" rather
// than delegating to get() which would throw. Disabled until the bug is fixed.
TEST(InterpreterTdz, TypeofInTdzThrows) {
    // "typeof x" before "let x = 1" in the same block must throw ReferenceError
    auto msg = exec_err("typeof x; let x = 1");
    EXPECT_NE(msg.find("ReferenceError"), std::string::npos);
}

// let without initializer should produce undefined (not TDZ error).
// BUG: Current implementation creates a Let binding with initialized=false (TDZ),
// but a bare `let x;` with no initializer should set the binding to undefined
// immediately (ES2025 §14.3.1.1 step 3.b.i: if no initializer, set to undefined).
// Disabled until the bug is fixed.
TEST(InterpreterTdz, LetNoInitIsUndefined) {
    auto v = exec_ok("let x; x");
    EXPECT_TRUE(v.is_undefined());
}

// Three-level scope: innermost block reads variable from middle block
TEST(InterpreterTdz, ThreeLevelScopeRead) {
    auto v = exec_ok("let a = 1; { let b = 2; { b } }");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.0);
}

// var hoisting: first read before declaration must be undefined
TEST(InterpreterVar, VarHoistingFirstReadIsUndefined) {
    // Verify the very first expression (before the declaration) evaluates to undefined.
    // Use if/else instead of ternary (not yet supported by the parser).
    auto v = exec_ok("var r; if (x === undefined) { r = 99; } else { r = 0; } var x = 1; r");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 99.0);
}

// var penetrates block boundaries
TEST(InterpreterVar, VarPenetratesBlock) {
    auto v = exec_ok("{ var x = 1; } x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

// Multiple var declarations of the same name must not error; last init wins
TEST(InterpreterVar, VarDuplicateDeclNoError) {
    auto v = exec_ok("var x = 1; var x = 2; x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.0);
}

// ---- ToBoolean boundary ----

TEST(InterpreterToBoolean, NegativeZeroFalsy) {
    // -0 is falsy per spec
    auto v = exec_ok("if (-0) 1; else 2");
    EXPECT_EQ(v.as_number(), 2.0);
}

TEST(InterpreterToBoolean, NanFalsy) {
    auto v = exec_ok("if (0 / 0) 1; else 2");
    EXPECT_EQ(v.as_number(), 2.0);
}

TEST(InterpreterToBoolean, StringFalseTruthy) {
    // Non-empty string "false" is truthy
    auto v = exec_ok("if (\"false\") 1; else 2");
    EXPECT_EQ(v.as_number(), 1.0);
}

// ---- Arithmetic boundary ----

TEST(InterpreterArith, NullPlusNumber) {
    // null ToNumber = 0, so null + 1 = 1
    auto v = exec_ok("null + 1");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

TEST(InterpreterArith, UndefinedPlusNumber) {
    // undefined ToNumber = NaN, so undefined + 1 = NaN
    auto v = exec_ok("undefined + 1");
    EXPECT_TRUE(v.is_number());
    EXPECT_TRUE(std::isnan(v.as_number()));
}

TEST(InterpreterArith, TruePlusTrue) {
    // true ToNumber = 1, so true + true = 2
    auto v = exec_ok("true + true");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.0);
}

TEST(InterpreterArith, EmptyStringPlusNull) {
    // "" is string, so string concatenation: "" + "null" = "null"
    auto v = exec_ok("\"\" + null");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "null");
}

TEST(InterpreterArith, EmptyStringPlusUndefined) {
    auto v = exec_ok("\"\" + undefined");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "undefined");
}

TEST(InterpreterArith, EmptyStringPlusFalse) {
    auto v = exec_ok("\"\" + false");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "false");
}

TEST(InterpreterArith, NumberPlusNumberPlusString) {
    // Left-associative: (1 + 2) + "3" = 3 + "3" = "33"
    auto v = exec_ok("1 + 2 + \"3\"");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "33");
}

TEST(InterpreterArith, StringPlusNumberPlusNumber) {
    // Left-associative: "1" + 2 = "12", "12" + 3 = "123"
    auto v = exec_ok("\"1\" + 2 + 3");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "123");
}

TEST(InterpreterArith, UnaryMinusNonNumericString) {
    // -"abc" => NaN
    auto v = exec_ok("-\"abc\"");
    EXPECT_TRUE(v.is_number());
    EXPECT_TRUE(std::isnan(v.as_number()));
}

// ---- Comparison boundary ----

// BUG: "10" < "9" relies on string lexicographic comparison (ES2025 §13.11).
// When both operands are strings, the relational operators must compare them
// lexicographically (code-unit order), not numerically.
// Current implementation in eval_binary always calls to_number() for Lt/Gt/LtEq/GtEq,
// so "10" < "9" evaluates as 10 < 9 = false instead of the spec-correct true.
// Disabled until the string comparison path is implemented.
TEST(InterpreterCmp, StringLexicographicOrder) {
    // "10" < "9" lexicographically: '1' < '9', so result is true
    auto v = exec_ok("\"10\" < \"9\"");
    EXPECT_EQ(v.as_bool(), true);
}

TEST(InterpreterCmp, NullLessThanOne) {
    // null ToNumber = 0, so null < 1 => 0 < 1 => true
    auto v = exec_ok("null < 1");
    EXPECT_EQ(v.as_bool(), true);
}

TEST(InterpreterCmp, UndefinedLessThanOne) {
    // undefined ToNumber = NaN, NaN < 1 => false
    auto v = exec_ok("undefined < 1");
    EXPECT_EQ(v.as_bool(), false);
}

TEST(InterpreterCmp, NanGreaterThan) {
    auto v = exec_ok("0 / 0 > 1");
    EXPECT_EQ(v.as_bool(), false);
}

TEST(InterpreterCmp, NanGreaterEq) {
    auto v = exec_ok("0 / 0 >= 1");
    EXPECT_EQ(v.as_bool(), false);
}

TEST(InterpreterCmp, NanLessEq) {
    auto v = exec_ok("0 / 0 <= 1");
    EXPECT_EQ(v.as_bool(), false);
}

// ---- Equality boundary ----

TEST(InterpreterEq, PositiveZeroStrictEqNegativeZero) {
    // +0 === -0 must be true per spec
    auto v = exec_ok("+0 === -0");
    EXPECT_EQ(v.as_bool(), true);
}

TEST(InterpreterEq, UndefinedNotEqFalse) {
    // undefined == false must be false (undefined is only == null/undefined)
    auto v = exec_ok("undefined == false");
    EXPECT_EQ(v.as_bool(), false);
}

TEST(InterpreterEq, UndefinedNotEqZero) {
    auto v = exec_ok("undefined == 0");
    EXPECT_EQ(v.as_bool(), false);
}

TEST(InterpreterEq, EmptyStringEqFalse) {
    // "" == false: false->0, ""->0, so 0 == 0 => true
    auto v = exec_ok("\"\" == false");
    EXPECT_EQ(v.as_bool(), true);
}

TEST(InterpreterEq, ZeroEqEmptyString) {
    // 0 == "": "" ToNumber = 0, so 0 == 0 => true
    auto v = exec_ok("0 == \"\"");
    EXPECT_EQ(v.as_bool(), true);
}

// ---- Logical operator boundary ----

TEST(InterpreterLogical, BothFalsyReturnsLast) {
    // 0 || false: 0 is falsy, so evaluate right; false is the result
    auto v = exec_ok("0 || false");
    EXPECT_TRUE(v.is_bool());
    EXPECT_EQ(v.as_bool(), false);
}

TEST(InterpreterLogical, AndShortCircuitDivByZero) {
    // false && (1/0): short-circuit, right side not evaluated
    // Result is false (not Infinity or error)
    auto v = exec_ok("false && (1/0)");
    EXPECT_TRUE(v.is_bool());
    EXPECT_EQ(v.as_bool(), false);
}

TEST(InterpreterLogical, OrShortCircuitDivByZero) {
    // true || (1/0): short-circuit, right side not evaluated
    auto v = exec_ok("true || (1/0)");
    EXPECT_TRUE(v.is_bool());
    EXPECT_EQ(v.as_bool(), true);
}

// ---- while + return interaction ----

TEST(InterpreterReturn, ReturnInsideWhile) {
    // return inside while must propagate out (not be swallowed by the loop)
    auto v = exec_ok("let i = 0; while (i < 10) { i = i + 1; if (i === 3) { return i; } } i");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 3.0);
}

TEST(InterpreterReturn, ReturnInsideWhileNestedIf) {
    // return nested inside while + if must propagate to top level
    auto v = exec_ok(
        "let x = 0;"
        "while (x < 5) {"
        "  x = x + 1;"
        "  if (x === 2) {"
        "    if (true) { return x; }"
        "  }"
        "}"
        "x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.0);
}

// ---- const boundary ----

TEST(InterpreterConst, CompoundAssignThrowsTypeError) {
    // const x = 1; x += 1 should throw TypeError (compound assign reads then sets)
    auto msg = exec_err("const x = 1; x += 1");
    EXPECT_NE(msg.find("TypeError"), std::string::npos);
}

// ---- Compound assignment boundary ----

TEST(InterpreterAssign, DivAssign) {
    auto v = exec_ok("let x = 10; x /= 4; x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.5);
}

TEST(InterpreterAssign, ModAssign) {
    auto v = exec_ok("let x = 10; x %= 3; x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

TEST(InterpreterAssign, MulAssign) {
    auto v = exec_ok("let x = 3; x *= 2; x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 6.0);
}

}  // namespace
