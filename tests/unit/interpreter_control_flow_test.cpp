#include "qppjs/frontend/parser.h"
#include "qppjs/runtime/interpreter.h"

#include <gtest/gtest.h>

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

// ---- throw basics ----

TEST(InterpreterThrow, ThrowNumberCaughtByValue) {
    auto v = exec_ok("let e; try { throw 42; } catch(x) { e = x; } e");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 42.0);
}

TEST(InterpreterThrow, ThrowStringCaughtByValue) {
    auto v = exec_ok("let e; try { throw \"msg\"; } catch(x) { e = x; } e");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "msg");
}

TEST(InterpreterThrow, ThrowNewErrorCaughtMessage) {
    auto v = exec_ok(
        "let msg;"
        "try { throw new Error(\"oops\"); } catch(e) { msg = e.message; }"
        "msg");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "oops");
}

TEST(InterpreterThrow, UncaughtThrowIsError) {
    auto msg = exec_err("throw 99");
    EXPECT_NE(msg.find("99"), std::string::npos);
}

// ---- try/catch/finally ----

TEST(InterpreterTryCatch, TryNormalCatchSkipped) {
    // try succeeds → catch body should not run
    auto v = exec_ok(
        "let x = 0;"
        "try { x = 1; } catch(e) { x = 99; }"
        "x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

TEST(InterpreterTryCatch, TryThrowCaughtByParam) {
    auto v = exec_ok(
        "let result;"
        "try { throw \"caught\"; } catch(e) { result = e; }"
        "result");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "caught");
}

TEST(InterpreterTryCatch, FinallyAlwaysRuns) {
    auto v = exec_ok(
        "let x = 0;"
        "try { x = 1; } finally { x = x + 10; }"
        "x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 11.0);
}

TEST(InterpreterTryCatch, FinallyRunsOnThrow) {
    auto v = exec_ok(
        "let x = 0;"
        "try { try { throw 1; } catch(e) { x = 2; } finally { x = x + 10; } } catch(e) {}"
        "x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 12.0);
}

TEST(InterpreterTryCatch, FinallyReturnOverridesTryThrow) {
    // A function that has try{throw}/finally{return} — finally return wins
    auto v = exec_ok(
        "function f() {"
        "  try { throw 1; } finally { return 42; }"
        "}"
        "f()");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 42.0);
}

TEST(InterpreterTryCatch, TryReturnFinallyNormal) {
    // try{return 1} finally{} → function returns 1
    auto v = exec_ok(
        "function f() {"
        "  try { return 1; } finally {}"
        "}"
        "f()");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

TEST(InterpreterTryCatch, CatchRethrowPropagates) {
    // catch re-throws → outer catch should catch it
    auto v = exec_ok(
        "let result;"
        "try {"
        "  try { throw \"inner\"; } catch(e) { throw \"rethrown\"; }"
        "} catch(e) { result = e; }"
        "result");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "rethrown");
}

TEST(InterpreterTryCatch, NestedTryCatch) {
    // Inner throw caught by outer
    auto v = exec_ok(
        "let msg;"
        "try {"
        "  try { throw new Error(\"inner\"); } catch(e) { throw new Error(\"rethrow\"); }"
        "} catch(e) { msg = e.message; }"
        "msg");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "rethrow");
}

TEST(InterpreterTryCatch, CatchParamScopeIsolated) {
    // Catch param 'e' should not be visible outside
    auto v = exec_ok(
        "let outer = 0;"
        "try { throw 1; } catch(e) { outer = e; }"
        "outer");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

TEST(InterpreterTryCatch, FinallyThrowOverridesCatch) {
    // finally throws → overwrites catch's normal completion
    auto v = exec_ok(
        "let result = \"none\";"
        "try {"
        "  try { throw \"t\"; } catch(e) { result = \"catch\"; } finally { throw \"finally\"; }"
        "} catch(e) { result = e; }"
        "result");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "finally");
}

// ---- break/continue in while ----

TEST(InterpreterBreakContinue, WhileBreak) {
    auto v = exec_ok(
        "let x = 0;"
        "while (x < 10) { if (x === 5) break; x = x + 1; }"
        "x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 5.0);
}

TEST(InterpreterBreakContinue, WhileContinue) {
    // Sum even numbers 0..8
    auto v = exec_ok(
        "let sum = 0; let i = 0;"
        "while (i < 10) {"
        "  i = i + 1;"
        "  if (i % 2 !== 0) continue;"
        "  sum = sum + i;"
        "}"
        "sum");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 30.0);  // 2+4+6+8+10
}

// ---- break/continue in for ----

TEST(InterpreterFor, BasicFor) {
    auto v = exec_ok(
        "let sum = 0;"
        "for (let i = 0; i < 5; i = i + 1) { sum = sum + i; }"
        "sum");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 10.0);
}

TEST(InterpreterFor, ForBreak) {
    auto v = exec_ok(
        "let sum = 0;"
        "for (let i = 0; i < 10; i = i + 1) {"
        "  if (i === 5) break;"
        "  sum = sum + i;"
        "}"
        "sum");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 10.0);  // 0+1+2+3+4
}

TEST(InterpreterFor, ForContinue) {
    // Sum only even indices 0..8
    auto v = exec_ok(
        "let sum = 0;"
        "for (let i = 0; i < 10; i = i + 1) {"
        "  if (i % 2 !== 0) continue;"
        "  sum = sum + i;"
        "}"
        "sum");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 20.0);  // 0+2+4+6+8
}

TEST(InterpreterFor, ForNoInit) {
    auto v = exec_ok(
        "let i = 0; let s = 0;"
        "for (; i < 3; i = i + 1) { s = s + i; }"
        "s");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 3.0);
}

TEST(InterpreterFor, ForNoUpdate) {
    auto v = exec_ok(
        "let s = 0;"
        "for (let i = 0; i < 3;) { s = s + i; i = i + 1; }"
        "s");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 3.0);
}

// ---- labeled break/continue ----

TEST(InterpreterLabeled, LabeledBreakOuterWhile) {
    auto v = exec_ok(
        "let count = 0;"
        "outer: while (count < 100) {"
        "  let inner = 0;"
        "  while (inner < 100) {"
        "    if (inner === 2) break outer;"
        "    inner = inner + 1;"
        "  }"
        "  count = count + 1;"
        "}"
        "count");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}

TEST(InterpreterLabeled, LabeledContinueOuterFor) {
    // Labeled continue to outer for: skip inner iterations when j === 1
    auto v = exec_ok(
        "let sum = 0;"
        "outer: for (let i = 0; i < 3; i = i + 1) {"
        "  for (let j = 0; j < 3; j = j + 1) {"
        "    if (j === 1) continue outer;"
        "    sum = sum + 1;"
        "  }"
        "}"
        "sum");
    EXPECT_TRUE(v.is_number());
    // Each outer iteration: j=0 adds 1, j=1 continues outer (skips j=2)
    // So 3 iterations * 1 = 3
    EXPECT_EQ(v.as_number(), 3.0);
}

// ---- break inside try/finally ----

TEST(InterpreterBreakFinally, BreakThroughFinally) {
    // break inside try should still run finally
    auto v = exec_ok(
        "let x = 0;"
        "while (true) {"
        "  try { break; } finally { x = 1; }"
        "}"
        "x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

// ---- Error built-in ----

TEST(InterpreterError, NewErrorMessage) {
    auto v = exec_ok("new Error(\"hello\").message");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "hello");
}

TEST(InterpreterError, NewErrorNoArgMessage) {
    auto v = exec_ok("new Error().message");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "");
}

TEST(InterpreterError, NewErrorName) {
    auto v = exec_ok("new Error(\"x\").name");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "Error");
}

// ---- Engine internal errors can be caught ----

TEST(InterpreterTryCatch, InternalReferenceErrorCaught) {
    auto v = exec_ok(
        "let result = \"none\";"
        "try { undeclaredVar; } catch(e) { result = \"caught\"; }"
        "result");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "caught");
}

TEST(InterpreterTryCatch, InternalTypeErrorCaught) {
    auto v = exec_ok(
        "let result = \"none\";"
        "try { null.x; } catch(e) { result = \"caught\"; }"
        "result");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "caught");
}

// ---- throw in expression catches as value ----

TEST(InterpreterThrow, ThrowBoolCaught) {
    auto v = exec_ok("let r; try { throw true; } catch(e) { r = e; } r");
    EXPECT_TRUE(v.is_bool());
    EXPECT_EQ(v.as_bool(), true);
}

TEST(InterpreterThrow, ThrowObjectCaught) {
    auto v = exec_ok(
        "let msg;"
        "function divide(a, b) {"
        "  if (b === 0) throw new Error(\"division by zero\");"
        "  return a / b;"
        "}"
        "try { divide(1, 0); } catch(e) { msg = e.message; }"
        "msg");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "division by zero");
}

}  // namespace
