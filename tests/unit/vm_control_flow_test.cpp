#include "qppjs/frontend/parser.h"
#include "qppjs/vm/compiler.h"
#include "qppjs/vm/vm.h"

#include <gtest/gtest.h>

#include <string_view>

namespace {

// Helper: parse → compile → exec via VM, expect success, return Value
qppjs::Value vm_ok(std::string_view source) {
    auto parse_result = qppjs::parse_program(source);
    EXPECT_TRUE(parse_result.ok()) << "parse failed: " << parse_result.error().message();
    qppjs::Compiler compiler;
    auto bytecode = compiler.compile(parse_result.value());
    qppjs::VM vm;
    auto result = vm.exec(bytecode);
    EXPECT_TRUE(result.is_ok()) << "exec failed: " << result.error().message();
    return result.value();
}

// Helper: parse → compile → exec via VM, expect error, return error message
std::string vm_err(std::string_view source) {
    auto parse_result = qppjs::parse_program(source);
    EXPECT_TRUE(parse_result.ok()) << "parse failed: " << parse_result.error().message();
    qppjs::Compiler compiler;
    auto bytecode = compiler.compile(parse_result.value());
    qppjs::VM vm;
    auto result = vm.exec(bytecode);
    EXPECT_FALSE(result.is_ok()) << "expected error but got success";
    return result.error().message();
}

// ============================================================
// throw basics
// ============================================================

TEST(VMThrow, ThrowNumberCaughtByValue) {
    auto v = vm_ok("let e; try { throw 42; } catch(x) { e = x; } e");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 42.0);
}

TEST(VMThrow, ThrowStringCaughtByValue) {
    auto v = vm_ok("let e; try { throw \"msg\"; } catch(x) { e = x; } e");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "msg");
}

TEST(VMThrow, ThrowNewErrorCaughtMessage) {
    auto v = vm_ok(
        "let msg;"
        "try { throw new Error(\"oops\"); } catch(e) { msg = e.message; }"
        "msg");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "oops");
}

TEST(VMThrow, UncaughtThrowIsError) {
    auto msg = vm_err("throw 99");
    EXPECT_NE(msg.find("99"), std::string::npos);
}

// ============================================================
// try/catch/finally
// ============================================================

TEST(VMTryCatch, TryNormalCatchSkipped) {
    auto v = vm_ok(
        "let x = 0;"
        "try { x = 1; } catch(e) { x = 99; }"
        "x");
    EXPECT_EQ(v.as_number(), 1.0);
}

TEST(VMTryCatch, TryThrowCaughtByParam) {
    auto v = vm_ok(
        "let result;"
        "try { throw \"caught\"; } catch(e) { result = e; }"
        "result");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "caught");
}

TEST(VMTryCatch, FinallyAlwaysRuns) {
    auto v = vm_ok(
        "let x = 0;"
        "try { x = 1; } finally { x = x + 10; }"
        "x");
    EXPECT_EQ(v.as_number(), 11.0);
}

TEST(VMTryCatch, FinallyRunsOnThrow) {
    auto v = vm_ok(
        "let x = 0;"
        "try { try { throw 1; } catch(e) { x = 2; } finally { x = x + 10; } } catch(e) {}"
        "x");
    EXPECT_EQ(v.as_number(), 12.0);
}

TEST(VMTryCatch, CatchRethrowPropagates) {
    auto v = vm_ok(
        "let result;"
        "try {"
        "  try { throw \"inner\"; } catch(e) { throw \"rethrown\"; }"
        "} catch(e) { result = e; }"
        "result");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "rethrown");
}

TEST(VMTryCatch, NestedTryCatch) {
    auto v = vm_ok(
        "let msg;"
        "try {"
        "  try { throw new Error(\"inner\"); } catch(e) { throw new Error(\"rethrow\"); }"
        "} catch(e) { msg = e.message; }"
        "msg");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "rethrow");
}

TEST(VMTryCatch, CatchParamScopeIsolated) {
    auto v = vm_ok(
        "let outer = 0;"
        "try { throw 1; } catch(e) { outer = e; }"
        "outer");
    EXPECT_EQ(v.as_number(), 1.0);
}

TEST(VMTryCatch, FinallyThrowOverridesCatch) {
    auto v = vm_ok(
        "let result = \"none\";"
        "try {"
        "  try { throw \"t\"; } catch(e) { result = \"catch\"; } finally { throw \"finally\"; }"
        "} catch(e) { result = e; }"
        "result");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "finally");
}

// ============================================================
// break/continue in while
// ============================================================

TEST(VMBreakContinue, WhileBreak) {
    auto v = vm_ok(
        "let x = 0;"
        "while (x < 10) { if (x === 5) break; x = x + 1; }"
        "x");
    EXPECT_EQ(v.as_number(), 5.0);
}

TEST(VMBreakContinue, WhileContinue) {
    auto v = vm_ok(
        "let sum = 0; let i = 0;"
        "while (i < 10) {"
        "  i = i + 1;"
        "  if (i % 2 !== 0) continue;"
        "  sum = sum + i;"
        "}"
        "sum");
    EXPECT_EQ(v.as_number(), 30.0);
}

// ============================================================
// for loop
// ============================================================

TEST(VMFor, BasicFor) {
    auto v = vm_ok(
        "let sum = 0;"
        "for (let i = 0; i < 5; i = i + 1) { sum = sum + i; }"
        "sum");
    EXPECT_EQ(v.as_number(), 10.0);
}

TEST(VMFor, ForBreak) {
    auto v = vm_ok(
        "let sum = 0;"
        "for (let i = 0; i < 10; i = i + 1) {"
        "  if (i === 5) break;"
        "  sum = sum + i;"
        "}"
        "sum");
    EXPECT_EQ(v.as_number(), 10.0);
}

TEST(VMFor, ForContinue) {
    auto v = vm_ok(
        "let sum = 0;"
        "for (let i = 0; i < 10; i = i + 1) {"
        "  if (i % 2 !== 0) continue;"
        "  sum = sum + i;"
        "}"
        "sum");
    EXPECT_EQ(v.as_number(), 20.0);
}

TEST(VMFor, ForNoInit) {
    auto v = vm_ok(
        "let i = 0; let s = 0;"
        "for (; i < 3; i = i + 1) { s = s + i; }"
        "s");
    EXPECT_EQ(v.as_number(), 3.0);
}

TEST(VMFor, ForNoUpdate) {
    auto v = vm_ok(
        "let s = 0;"
        "for (let i = 0; i < 3;) { s = s + i; i = i + 1; }"
        "s");
    EXPECT_EQ(v.as_number(), 3.0);
}

// ============================================================
// labeled break/continue
// ============================================================

TEST(VMLabeled, LabeledBreakOuterWhile) {
    auto v = vm_ok(
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
    EXPECT_EQ(v.as_number(), 0.0);
}

TEST(VMLabeled, LabeledContinueOuterFor) {
    auto v = vm_ok(
        "let sum = 0;"
        "outer: for (let i = 0; i < 3; i = i + 1) {"
        "  for (let j = 0; j < 3; j = j + 1) {"
        "    if (j === 1) continue outer;"
        "    sum = sum + 1;"
        "  }"
        "}"
        "sum");
    EXPECT_EQ(v.as_number(), 3.0);
}

// ============================================================
// Error built-in
// ============================================================

TEST(VMError, NewErrorMessage) {
    auto v = vm_ok("new Error(\"hello\").message");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "hello");
}

TEST(VMError, NewErrorNoArgMessage) {
    auto v = vm_ok("new Error().message");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "");
}

TEST(VMError, NewErrorName) {
    auto v = vm_ok("new Error(\"x\").name");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "Error");
}

// ============================================================
// Engine internal errors can be caught
// ============================================================

TEST(VMTryCatch, InternalReferenceErrorCaught) {
    auto v = vm_ok(
        "let result = \"none\";"
        "try { undeclaredVar; } catch(e) { result = \"caught\"; }"
        "result");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "caught");
}

TEST(VMTryCatch, InternalTypeErrorCaught) {
    auto v = vm_ok(
        "let result = \"none\";"
        "try { null.x; } catch(e) { result = \"caught\"; }"
        "result");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "caught");
}

// ============================================================
// throw various types
// ============================================================

TEST(VMThrow, ThrowBoolCaught) {
    auto v = vm_ok("let r; try { throw true; } catch(e) { r = e; } r");
    EXPECT_TRUE(v.is_bool());
    EXPECT_EQ(v.as_bool(), true);
}

TEST(VMThrow, ThrowObjectCaught) {
    auto v = vm_ok(
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

// ============================================================
// break inside try/finally
// ============================================================

TEST(VMBreakFinally, BreakThroughFinally) {
    auto v = vm_ok(
        "let x = 0;"
        "while (true) {"
        "  try { break; } finally { x = 1; }"
        "}"
        "x");
    EXPECT_EQ(v.as_number(), 1.0);
}

}  // namespace
