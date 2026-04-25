#include "qppjs/frontend/parser.h"
#include "qppjs/runtime/interpreter.h"
#include "qppjs/vm/compiler.h"
#include "qppjs/vm/vm.h"

#include <gtest/gtest.h>

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
// Interpreter: console object tests
// ============================================================

// C-01: typeof console === "object"
TEST(InterpConsole, TypeofConsole) {
    auto v = interp_ok("typeof console");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "object");
}

// C-02: typeof console.log === "function"
TEST(InterpConsole, TypeofConsoleLog) {
    auto v = interp_ok("typeof console.log");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "function");
}

// C-03: console.log() — zero args, outputs "\n"
TEST(InterpConsole, LogZeroArgs) {
    testing::internal::CaptureStdout();
    interp_ok("console.log()");
    std::string output = testing::internal::GetCapturedStdout();
    EXPECT_EQ(output, "\n");
}

// C-04: console.log("hello") — outputs "hello\n"
TEST(InterpConsole, LogSingleString) {
    testing::internal::CaptureStdout();
    interp_ok("console.log('hello')");
    std::string output = testing::internal::GetCapturedStdout();
    EXPECT_EQ(output, "hello\n");
}

// C-05: console.log("hello", 42) — outputs "hello 42\n"
TEST(InterpConsole, LogStringAndNumber) {
    testing::internal::CaptureStdout();
    interp_ok("console.log('hello', 42)");
    std::string output = testing::internal::GetCapturedStdout();
    EXPECT_EQ(output, "hello 42\n");
}

// C-06: console.log(undefined) — outputs "undefined\n"
TEST(InterpConsole, LogUndefined) {
    testing::internal::CaptureStdout();
    interp_ok("console.log(undefined)");
    std::string output = testing::internal::GetCapturedStdout();
    EXPECT_EQ(output, "undefined\n");
}

// C-07: console.log(null) — outputs "null\n"
TEST(InterpConsole, LogNull) {
    testing::internal::CaptureStdout();
    interp_ok("console.log(null)");
    std::string output = testing::internal::GetCapturedStdout();
    EXPECT_EQ(output, "null\n");
}

// C-08: console.log(true, false) — outputs "true false\n"
TEST(InterpConsole, LogBooleans) {
    testing::internal::CaptureStdout();
    interp_ok("console.log(true, false)");
    std::string output = testing::internal::GetCapturedStdout();
    EXPECT_EQ(output, "true false\n");
}

// C-09: console.log(3.14) — stdout contains "3.14"
TEST(InterpConsole, LogFloat) {
    testing::internal::CaptureStdout();
    interp_ok("console.log(3.14)");
    std::string output = testing::internal::GetCapturedStdout();
    EXPECT_NE(output.find("3.14"), std::string::npos);
}

// C-10: console.log({}) — outputs "[object Object]\n"
TEST(InterpConsole, LogObject) {
    testing::internal::CaptureStdout();
    interp_ok("console.log({})");
    std::string output = testing::internal::GetCapturedStdout();
    EXPECT_EQ(output, "[object Object]\n");
}

// ============================================================
// VM: console object tests
// ============================================================

// C-01: typeof console === "object"
TEST(VMConsole, TypeofConsole) {
    auto v = vm_ok("typeof console");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "object");
}

// C-02: typeof console.log === "function"
TEST(VMConsole, TypeofConsoleLog) {
    auto v = vm_ok("typeof console.log");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "function");
}

// C-03: console.log() — zero args, outputs "\n"
TEST(VMConsole, LogZeroArgs) {
    testing::internal::CaptureStdout();
    vm_ok("console.log()");
    std::string output = testing::internal::GetCapturedStdout();
    EXPECT_EQ(output, "\n");
}

// C-04: console.log("hello") — outputs "hello\n"
TEST(VMConsole, LogSingleString) {
    testing::internal::CaptureStdout();
    vm_ok("console.log('hello')");
    std::string output = testing::internal::GetCapturedStdout();
    EXPECT_EQ(output, "hello\n");
}

// C-05: console.log("hello", 42) — outputs "hello 42\n"
TEST(VMConsole, LogStringAndNumber) {
    testing::internal::CaptureStdout();
    vm_ok("console.log('hello', 42)");
    std::string output = testing::internal::GetCapturedStdout();
    EXPECT_EQ(output, "hello 42\n");
}

// C-06: console.log(undefined) — outputs "undefined\n"
TEST(VMConsole, LogUndefined) {
    testing::internal::CaptureStdout();
    vm_ok("console.log(undefined)");
    std::string output = testing::internal::GetCapturedStdout();
    EXPECT_EQ(output, "undefined\n");
}

// C-07: console.log(null) — outputs "null\n"
TEST(VMConsole, LogNull) {
    testing::internal::CaptureStdout();
    vm_ok("console.log(null)");
    std::string output = testing::internal::GetCapturedStdout();
    EXPECT_EQ(output, "null\n");
}

// C-08: console.log(true, false) — outputs "true false\n"
TEST(VMConsole, LogBooleans) {
    testing::internal::CaptureStdout();
    vm_ok("console.log(true, false)");
    std::string output = testing::internal::GetCapturedStdout();
    EXPECT_EQ(output, "true false\n");
}

// C-09: console.log(3.14) — stdout contains "3.14"
TEST(VMConsole, LogFloat) {
    testing::internal::CaptureStdout();
    vm_ok("console.log(3.14)");
    std::string output = testing::internal::GetCapturedStdout();
    EXPECT_NE(output.find("3.14"), std::string::npos);
}

// C-10: console.log({}) — outputs "[object Object]\n"
TEST(VMConsole, LogObject) {
    testing::internal::CaptureStdout();
    vm_ok("console.log({})");
    std::string output = testing::internal::GetCapturedStdout();
    EXPECT_EQ(output, "[object Object]\n");
}

// ============================================================
// Interpreter: boundary / edge-case tests
// ============================================================

// C-11: 5 个参数，空格分隔正确
TEST(InterpConsole, LogFiveArgs) {
    testing::internal::CaptureStdout();
    interp_ok("console.log(1, 2, 3, 4, 5)");
    std::string output = testing::internal::GetCapturedStdout();
    EXPECT_EQ(output, "1 2 3 4 5\n");
}

// C-12: NaN 输出 "NaN"（用 0/0 产生 NaN，NaN 全局标识符尚未注册）
TEST(InterpConsole, LogNaN) {
    testing::internal::CaptureStdout();
    interp_ok("console.log(0/0)");
    std::string output = testing::internal::GetCapturedStdout();
    EXPECT_EQ(output, "NaN\n");
}

// C-13: Infinity 输出 "Infinity"（用 1/0 产生 Infinity）
TEST(InterpConsole, LogInfinity) {
    testing::internal::CaptureStdout();
    interp_ok("console.log(1/0)");
    std::string output = testing::internal::GetCapturedStdout();
    EXPECT_EQ(output, "Infinity\n");
}

// C-14: -Infinity 输出 "-Infinity"（用 -1/0 产生 -Infinity）
TEST(InterpConsole, LogNegInfinity) {
    testing::internal::CaptureStdout();
    interp_ok("console.log(-1/0)");
    std::string output = testing::internal::GetCapturedStdout();
    EXPECT_EQ(output, "-Infinity\n");
}

// C-15: 0 输出 "0"
TEST(InterpConsole, LogZero) {
    testing::internal::CaptureStdout();
    interp_ok("console.log(0)");
    std::string output = testing::internal::GetCapturedStdout();
    EXPECT_EQ(output, "0\n");
}

// C-16: -0 输出 "0"（规范 ToString(-0) === "0"）
TEST(InterpConsole, LogNegativeZero) {
    testing::internal::CaptureStdout();
    interp_ok("console.log(-0)");
    std::string output = testing::internal::GetCapturedStdout();
    EXPECT_EQ(output, "0\n");
}

// C-17: 负整数
TEST(InterpConsole, LogNegativeInt) {
    testing::internal::CaptureStdout();
    interp_ok("console.log(-42)");
    std::string output = testing::internal::GetCapturedStdout();
    EXPECT_EQ(output, "-42\n");
}

// C-18: 空字符串参数 — 输出一个空行（仅 "\n"，无空格）
TEST(InterpConsole, LogEmptyString) {
    testing::internal::CaptureStdout();
    interp_ok("console.log('')");
    std::string output = testing::internal::GetCapturedStdout();
    EXPECT_EQ(output, "\n");
}

// C-19: 含空格的字符串 — 原样输出，不额外分隔
TEST(InterpConsole, LogStringWithSpaces) {
    testing::internal::CaptureStdout();
    interp_ok("console.log('hello world')");
    std::string output = testing::internal::GetCapturedStdout();
    EXPECT_EQ(output, "hello world\n");
}

// C-20: 连续两次调用 — 各自独立输出一行
TEST(InterpConsole, LogTwoCalls) {
    testing::internal::CaptureStdout();
    interp_ok("console.log('a'); console.log('b')");
    std::string output = testing::internal::GetCapturedStdout();
    EXPECT_EQ(output, "a\nb\n");
}

// C-21: 将 console.log 赋值给变量后调用
TEST(InterpConsole, LogViaVariable) {
    testing::internal::CaptureStdout();
    interp_ok("var fn = console.log; fn('via var')");
    std::string output = testing::internal::GetCapturedStdout();
    EXPECT_EQ(output, "via var\n");
}

// C-22: console 注册不影响其他全局变量查找（回归）
TEST(InterpConsole, ConsoleDoesNotBreakGlobalLookup) {
    auto v = interp_ok("var x = 99; x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 99.0);
}

// C-23: console.log 返回值为 undefined
TEST(InterpConsole, LogReturnsUndefined) {
    testing::internal::CaptureStdout();
    auto v = interp_ok("console.log('x')");
    testing::internal::GetCapturedStdout();
    EXPECT_TRUE(v.is_undefined());
}

// C-24: 混合类型 5 参数
TEST(InterpConsole, LogMixedFiveArgs) {
    testing::internal::CaptureStdout();
    interp_ok("console.log(undefined, null, true, 0, 'end')");
    std::string output = testing::internal::GetCapturedStdout();
    EXPECT_EQ(output, "undefined null true 0 end\n");
}

// ============================================================
// VM: boundary / edge-case tests
// ============================================================

// C-11: 5 个参数，空格分隔正确
TEST(VMConsole, LogFiveArgs) {
    testing::internal::CaptureStdout();
    vm_ok("console.log(1, 2, 3, 4, 5)");
    std::string output = testing::internal::GetCapturedStdout();
    EXPECT_EQ(output, "1 2 3 4 5\n");
}

// C-12: NaN 输出 "NaN"（用 0/0 产生 NaN，NaN 全局标识符尚未注册）
TEST(VMConsole, LogNaN) {
    testing::internal::CaptureStdout();
    vm_ok("console.log(0/0)");
    std::string output = testing::internal::GetCapturedStdout();
    EXPECT_EQ(output, "NaN\n");
}

// C-13: Infinity 输出 "Infinity"（用 1/0 产生 Infinity）
TEST(VMConsole, LogInfinity) {
    testing::internal::CaptureStdout();
    vm_ok("console.log(1/0)");
    std::string output = testing::internal::GetCapturedStdout();
    EXPECT_EQ(output, "Infinity\n");
}

// C-14: -Infinity 输出 "-Infinity"（用 -1/0 产生 -Infinity）
TEST(VMConsole, LogNegInfinity) {
    testing::internal::CaptureStdout();
    vm_ok("console.log(-1/0)");
    std::string output = testing::internal::GetCapturedStdout();
    EXPECT_EQ(output, "-Infinity\n");
}

// C-15: 0 输出 "0"
TEST(VMConsole, LogZero) {
    testing::internal::CaptureStdout();
    vm_ok("console.log(0)");
    std::string output = testing::internal::GetCapturedStdout();
    EXPECT_EQ(output, "0\n");
}

// C-16: -0 输出 "0"（规范 ToString(-0) === "0"）
TEST(VMConsole, LogNegativeZero) {
    testing::internal::CaptureStdout();
    vm_ok("console.log(-0)");
    std::string output = testing::internal::GetCapturedStdout();
    EXPECT_EQ(output, "0\n");
}

// C-17: 负整数
TEST(VMConsole, LogNegativeInt) {
    testing::internal::CaptureStdout();
    vm_ok("console.log(-42)");
    std::string output = testing::internal::GetCapturedStdout();
    EXPECT_EQ(output, "-42\n");
}

// C-18: 空字符串参数 — 输出一个空行（仅 "\n"，无空格）
TEST(VMConsole, LogEmptyString) {
    testing::internal::CaptureStdout();
    vm_ok("console.log('')");
    std::string output = testing::internal::GetCapturedStdout();
    EXPECT_EQ(output, "\n");
}

// C-19: 含空格的字符串 — 原样输出，不额外分隔
TEST(VMConsole, LogStringWithSpaces) {
    testing::internal::CaptureStdout();
    vm_ok("console.log('hello world')");
    std::string output = testing::internal::GetCapturedStdout();
    EXPECT_EQ(output, "hello world\n");
}

// C-20: 连续两次调用 — 各自独立输出一行
TEST(VMConsole, LogTwoCalls) {
    testing::internal::CaptureStdout();
    vm_ok("console.log('a'); console.log('b')");
    std::string output = testing::internal::GetCapturedStdout();
    EXPECT_EQ(output, "a\nb\n");
}

// C-21: 将 console.log 赋值给变量后调用
TEST(VMConsole, LogViaVariable) {
    testing::internal::CaptureStdout();
    vm_ok("var fn = console.log; fn('via var')");
    std::string output = testing::internal::GetCapturedStdout();
    EXPECT_EQ(output, "via var\n");
}

// C-22: console 注册不影响其他全局变量查找（回归）
TEST(VMConsole, ConsoleDoesNotBreakGlobalLookup) {
    auto v = vm_ok("var x = 99; x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 99.0);
}

// C-23: console.log 返回值为 undefined
TEST(VMConsole, LogReturnsUndefined) {
    testing::internal::CaptureStdout();
    auto v = vm_ok("console.log('x')");
    testing::internal::GetCapturedStdout();
    EXPECT_TRUE(v.is_undefined());
}

// C-24: 混合类型 5 参数
TEST(VMConsole, LogMixedFiveArgs) {
    testing::internal::CaptureStdout();
    vm_ok("console.log(undefined, null, true, 0, 'end')");
    std::string output = testing::internal::GetCapturedStdout();
    EXPECT_EQ(output, "undefined null true 0 end\n");
}

}  // namespace
