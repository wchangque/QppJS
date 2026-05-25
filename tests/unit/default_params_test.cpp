#include "qppjs/frontend/parser.h"
#include "qppjs/runtime/interpreter.h"
#include "qppjs/vm/compiler.h"
#include "qppjs/vm/vm.h"

#include <gtest/gtest.h>

#include <cmath>
#include <string_view>

using namespace qppjs;

namespace {

static bool parse_fails(std::string_view source) {
    return !parse_program(source).ok();
}

static Value interp_ok(std::string_view source) {
    auto parse_result = parse_program(source);
    EXPECT_TRUE(parse_result.ok()) << "parse failed: " << parse_result.error().message();
    Interpreter interp;
    auto result = interp.exec(parse_result.value());
    EXPECT_TRUE(result.is_ok()) << "exec failed: " << result.error().message();
    return result.value();
}

static bool interp_throws(std::string_view source) {
    auto parse_result = parse_program(source);
    if (!parse_result.ok()) return false;
    Interpreter interp;
    return !interp.exec(parse_result.value()).is_ok();
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

static bool vm_throws(std::string_view source) {
    auto parse_result = parse_program(source);
    if (!parse_result.ok()) return false;
    Compiler compiler;
    auto bytecode = compiler.compile(parse_result.value());
    VM vm;
    return !vm.exec(bytecode).is_ok();
}

// ============================================================
// Parser level — SyntaxError
// ============================================================

// DP-00: rest + default → SyntaxError at parse time
TEST(DefaultParams, ParserRestWithDefaultSyntaxError) {
    EXPECT_TRUE(parse_fails("function f(...x = []) {}"));
    EXPECT_TRUE(parse_fails("(function(...x = []) {})"));
    EXPECT_TRUE(parse_fails("(...x = []) => x"));
}

// ============================================================
// Interpreter tests
// ============================================================

// DP-01: f(a=1) called with no args → a === 1
TEST(DefaultParams, InterpNoArg) {
    EXPECT_EQ(interp_ok("function f(a = 1) { return a; } f()").as_number(), 1.0);
}

// DP-02: f(a=1) called with 2 → a === 2
TEST(DefaultParams, InterpArgProvided) {
    EXPECT_EQ(interp_ok("function f(a = 1) { return a; } f(2)").as_number(), 2.0);
}

// DP-03: f(a=1) called with undefined → a === 1 (undefined triggers default)
TEST(DefaultParams, InterpUndefinedTriggersDefault) {
    EXPECT_EQ(interp_ok("function f(a = 1) { return a; } f(undefined)").as_number(), 1.0);
}

// DP-04: f(a=1) called with null → a === null (null does NOT trigger default)
TEST(DefaultParams, InterpNullNoDefault) {
    EXPECT_TRUE(interp_ok("function f(a = 1) { return a; } f(null)").is_null());
}

// DP-05: f(a, b=a+1) called with f(5) → b === 6 (forward reference to earlier param)
TEST(DefaultParams, InterpForwardRef) {
    EXPECT_EQ(interp_ok("function f(a, b = a + 1) { return b; } f(5)").as_number(), 6.0);
}

// DP-06: function.length truncation — first param with default cuts the count
TEST(DefaultParams, InterpFunctionLength) {
    EXPECT_EQ(interp_ok("function f(a, b = 2, c) {} f.length").as_number(), 1.0);
    EXPECT_EQ(interp_ok("function f(a = 1) {} f.length").as_number(), 0.0);
    EXPECT_EQ(interp_ok("function f(a, b) {} f.length").as_number(), 2.0);
}

// DP-07: (function(a=1,...r){}).length === 0 (params before first default are counted; rest not counted)
TEST(DefaultParams, InterpLengthWithRest) {
    EXPECT_EQ(interp_ok("(function(a = 1, ...r) {}).length").as_number(), 0.0);
}

// DP-08: each call gets independent default [] object
TEST(DefaultParams, InterpDefaultIndependentEval) {
    auto result = interp_ok(R"(
        function f(a = []) { return a; }
        var r1 = f();
        var r2 = f();
        r1 === r2
    )");
    EXPECT_TRUE(result.is_bool());
    EXPECT_FALSE(result.as_bool());  // r1 !== r2 (different objects)
}

// DP-09: default expression throws → exception propagates
TEST(DefaultParams, InterpDefaultThrows) {
    EXPECT_TRUE(interp_throws(
        "function f(a = (() => { throw new Error('!'); })()) {} f()"));
}

// DP-10: arrow function with default
TEST(DefaultParams, InterpArrowDefault) {
    EXPECT_EQ(interp_ok("var f = (a = 42) => a; f()").as_number(), 42.0);
    EXPECT_EQ(interp_ok("var f = (a = 42) => a; f(7)").as_number(), 7.0);
}

// DP-11: async function with default
TEST(DefaultParams, InterpAsyncDefault) {
    EXPECT_EQ(interp_ok(R"(
        async function f(a = 99) { return a; }
        var result;
        f().then(function(v) { result = v; });
        result
    )").as_number(), 99.0);
}

// DP-12: multiple defaults, mixed
TEST(DefaultParams, InterpMultipleDefaults) {
    EXPECT_EQ(interp_ok("function f(a = 1, b = 2) { return a + b; } f()").as_number(), 3.0);
    EXPECT_EQ(interp_ok("function f(a = 1, b = 2) { return a + b; } f(10)").as_number(), 12.0);
    EXPECT_EQ(interp_ok("function f(a = 1, b = 2) { return a + b; } f(10, 20)").as_number(), 30.0);
}

// DP-13: function expression with default
TEST(DefaultParams, InterpFunctionExprDefault) {
    EXPECT_EQ(interp_ok("var f = function(a = 5) { return a; }; f()").as_number(), 5.0);
}

// ============================================================
// VM tests
// ============================================================

// DP-14: f(a=1) called with no args → a === 1
TEST(DefaultParams, VmNoArg) {
    EXPECT_EQ(vm_ok("function f(a = 1) { return a; } f()").as_number(), 1.0);
}

// DP-15: f(a=1) called with 2 → a === 2
TEST(DefaultParams, VmArgProvided) {
    EXPECT_EQ(vm_ok("function f(a = 1) { return a; } f(2)").as_number(), 2.0);
}

// DP-16: f(a=1) called with undefined → a === 1
TEST(DefaultParams, VmUndefinedTriggersDefault) {
    EXPECT_EQ(vm_ok("function f(a = 1) { return a; } f(undefined)").as_number(), 1.0);
}

// DP-17: f(a=1) called with null → a === null
TEST(DefaultParams, VmNullNoDefault) {
    EXPECT_TRUE(vm_ok("function f(a = 1) { return a; } f(null)").is_null());
}

// DP-18: f(a, b=a+1) called with f(5) → b === 6
TEST(DefaultParams, VmForwardRef) {
    EXPECT_EQ(vm_ok("function f(a, b = a + 1) { return b; } f(5)").as_number(), 6.0);
}

// DP-19: function.length truncation
TEST(DefaultParams, VmFunctionLength) {
    EXPECT_EQ(vm_ok("function f(a, b = 2, c) {} f.length").as_number(), 1.0);
    EXPECT_EQ(vm_ok("function f(a = 1) {} f.length").as_number(), 0.0);
    EXPECT_EQ(vm_ok("function f(a, b) {} f.length").as_number(), 2.0);
}

// DP-20: (function(a=1,...r){}).length === 0
TEST(DefaultParams, VmLengthWithRest) {
    EXPECT_EQ(vm_ok("(function(a = 1, ...r) {}).length").as_number(), 0.0);
}

// DP-21: each call gets independent default [] object
TEST(DefaultParams, VmDefaultIndependentEval) {
    auto result = vm_ok(R"(
        function f(a = []) { return a; }
        var r1 = f();
        var r2 = f();
        r1 === r2
    )");
    EXPECT_TRUE(result.is_bool());
    EXPECT_FALSE(result.as_bool());
}

// DP-22: default expression throws → exception propagates
TEST(DefaultParams, VmDefaultThrows) {
    EXPECT_TRUE(vm_throws(
        "function f(a = (() => { throw new Error('!'); })()) {} f()"));
}

// DP-23: arrow function with default
TEST(DefaultParams, VmArrowDefault) {
    EXPECT_EQ(vm_ok("var f = (a = 42) => a; f()").as_number(), 42.0);
    EXPECT_EQ(vm_ok("var f = (a = 42) => a; f(7)").as_number(), 7.0);
}

// DP-24: async function with default
TEST(DefaultParams, VmAsyncDefault) {
    EXPECT_EQ(vm_ok(R"(
        async function f(a = 99) { return a; }
        var result;
        f().then(function(v) { result = v; });
        result
    )").as_number(), 99.0);
}

// DP-25: multiple defaults, mixed
TEST(DefaultParams, VmMultipleDefaults) {
    EXPECT_EQ(vm_ok("function f(a = 1, b = 2) { return a + b; } f()").as_number(), 3.0);
    EXPECT_EQ(vm_ok("function f(a = 1, b = 2) { return a + b; } f(10)").as_number(), 12.0);
    EXPECT_EQ(vm_ok("function f(a = 1, b = 2) { return a + b; } f(10, 20)").as_number(), 30.0);
}

// DP-26: function expression with default
TEST(DefaultParams, VmFunctionExprDefault) {
    EXPECT_EQ(vm_ok("var f = function(a = 5) { return a; }; f()").as_number(), 5.0);
}

// ============================================================
// 追加边界测试
// ============================================================

// DP-27: 0 / false / "" / NaN 作为实参不触发默认值（仅 undefined 触发）
TEST(DefaultParams, InterpFalsyNonUndefinedNoDefault) {
    // 0 不触发
    EXPECT_EQ(interp_ok("function f(a = 99) { return a; } f(0)").as_number(), 0.0);
    // false 不触发
    EXPECT_FALSE(interp_ok("function f(a = true) { return a; } f(false)").as_bool());
    // "" 不触发
    EXPECT_EQ(interp_ok("function f(a = 'x') { return a; } f('')").as_string(), "");
    // NaN 不触发（NaN 是数字，非 undefined）
    auto nan_val = interp_ok("function f(a = 1) { return a; } f(NaN)");
    EXPECT_TRUE(nan_val.is_number());
    EXPECT_TRUE(std::isnan(nan_val.as_number()));
}

// DP-28: VM - 0 / false / "" / NaN 不触发默认值
TEST(DefaultParams, VmFalsyNonUndefinedNoDefault) {
    EXPECT_EQ(vm_ok("function f(a = 99) { return a; } f(0)").as_number(), 0.0);
    EXPECT_FALSE(vm_ok("function f(a = true) { return a; } f(false)").as_bool());
    EXPECT_EQ(vm_ok("function f(a = 'x') { return a; } f('')").as_string(), "");
    auto nan_val = vm_ok("function f(a = 1) { return a; } f(NaN)");
    EXPECT_TRUE(nan_val.is_number());
    EXPECT_TRUE(std::isnan(nan_val.as_number()));
}

// DP-29: 默认值后跟必选参数 f(a=1, b)，f(undefined, 2) → a=1, b=2
TEST(DefaultParams, InterpDefaultFollowedByRequired) {
    EXPECT_EQ(interp_ok("function f(a = 1, b) { return a + b; } f(undefined, 2)").as_number(), 3.0);
    // 显式传 a 时不使用默认值
    EXPECT_EQ(interp_ok("function f(a = 1, b) { return a + b; } f(10, 2)").as_number(), 12.0);
    // b 未传 → undefined，a 使用默认值 1
    EXPECT_EQ(interp_ok("function f(a = 1, b) { return a; } f(undefined)").as_number(), 1.0);
}

// DP-30: VM - 默认值后跟必选参数
TEST(DefaultParams, VmDefaultFollowedByRequired) {
    EXPECT_EQ(vm_ok("function f(a = 1, b) { return a + b; } f(undefined, 2)").as_number(), 3.0);
    EXPECT_EQ(vm_ok("function f(a = 1, b) { return a + b; } f(10, 2)").as_number(), 12.0);
    EXPECT_EQ(vm_ok("function f(a = 1, b) { return a; } f(undefined)").as_number(), 1.0);
}

// DP-31: 多次调用时默认值表达式独立求值（副作用计数验证）
TEST(DefaultParams, InterpSideEffectCountMultipleCalls) {
    // 每次调用无实参时执行一次默认值表达式，共 3 次
    EXPECT_EQ(interp_ok(R"(
        var n = 0;
        function f(a = ++n) { return a; }
        f(); f(); f();
        n
    )").as_number(), 3.0);
    // 提供实参时默认值表达式不执行，n 保持 0
    EXPECT_EQ(interp_ok(R"(
        var n = 0;
        function f(a = ++n) { return a; }
        f(99); f(99);
        n
    )").as_number(), 0.0);
    // 混合：2 次无参 + 1 次有参 → n == 2
    EXPECT_EQ(interp_ok(R"(
        var n = 0;
        function f(a = ++n) {}
        f(); f(99); f();
        n
    )").as_number(), 2.0);
}

// DP-32: VM - 多次调用副作用计数
TEST(DefaultParams, VmSideEffectCountMultipleCalls) {
    EXPECT_EQ(vm_ok(R"(
        var n = 0;
        function f(a = ++n) { return a; }
        f(); f(); f();
        n
    )").as_number(), 3.0);
    EXPECT_EQ(vm_ok(R"(
        var n = 0;
        function f(a = ++n) { return a; }
        f(99); f(99);
        n
    )").as_number(), 0.0);
    EXPECT_EQ(vm_ok(R"(
        var n = 0;
        function f(a = ++n) {}
        f(); f(99); f();
        n
    )").as_number(), 2.0);
}

// DP-33: rest 参数与默认值共存（f(a=1,...r)）
TEST(DefaultParams, InterpRestWithDefault) {
    // 无实参：a 使用默认值 1，r 为空
    EXPECT_EQ(interp_ok("function f(a = 1, ...r) { return r.length; } f()").as_number(), 0.0);
    EXPECT_EQ(interp_ok("function f(a = 1, ...r) { return a; } f()").as_number(), 1.0);
    // f(2,3,4)：a=2，r=[3,4]
    EXPECT_EQ(interp_ok("function f(a = 1, ...r) { return r.length; } f(2, 3, 4)").as_number(), 2.0);
    EXPECT_EQ(interp_ok("function f(a = 1, ...r) { return a; } f(2, 3, 4)").as_number(), 2.0);
    // f(undefined,3,4)：a 触发默认值=1，r=[3,4]
    EXPECT_EQ(interp_ok("function f(a = 1, ...r) { return a; } f(undefined, 3, 4)").as_number(), 1.0);
    EXPECT_EQ(interp_ok("function f(a = 1, ...r) { return r.length; } f(undefined, 3, 4)").as_number(), 2.0);
}

// DP-34: VM - rest 参数与默认值共存
TEST(DefaultParams, VmRestWithDefault) {
    EXPECT_EQ(vm_ok("function f(a = 1, ...r) { return r.length; } f()").as_number(), 0.0);
    EXPECT_EQ(vm_ok("function f(a = 1, ...r) { return a; } f()").as_number(), 1.0);
    EXPECT_EQ(vm_ok("function f(a = 1, ...r) { return r.length; } f(2, 3, 4)").as_number(), 2.0);
    EXPECT_EQ(vm_ok("function f(a = 1, ...r) { return a; } f(2, 3, 4)").as_number(), 2.0);
    EXPECT_EQ(vm_ok("function f(a = 1, ...r) { return a; } f(undefined, 3, 4)").as_number(), 1.0);
    EXPECT_EQ(vm_ok("function f(a = 1, ...r) { return r.length; } f(undefined, 3, 4)").as_number(), 2.0);
}

// DP-35: 默认值表达式中调用辅助函数 / 闭包
TEST(DefaultParams, InterpDefaultExprWithFunctionCall) {
    // 默认值调用外部辅助函数
    EXPECT_EQ(interp_ok(R"(
        function helper() { return 42; }
        function f(a = helper()) { return a; }
        f()
    )").as_number(), 42.0);
    // 默认值表达式引用外层闭包变量
    EXPECT_EQ(interp_ok(R"(
        var x = 10;
        function f(a = x * 2) { return a; }
        f()
    )").as_number(), 20.0);
    // 嵌套：默认值表达式中定义并立即调用函数（IIFE）
    EXPECT_EQ(interp_ok(R"(
        function f(a = (function() { return 7; })()) { return a; }
        f()
    )").as_number(), 7.0);
    // 箭头函数 IIFE 作为默认值
    EXPECT_EQ(interp_ok(R"(
        function f(a = (() => 8)()) { return a; }
        f()
    )").as_number(), 8.0);
}

// DP-36: VM - 默认值表达式中调用辅助函数 / 闭包
TEST(DefaultParams, VmDefaultExprWithFunctionCall) {
    EXPECT_EQ(vm_ok(R"(
        function helper() { return 42; }
        function f(a = helper()) { return a; }
        f()
    )").as_number(), 42.0);
    EXPECT_EQ(vm_ok(R"(
        var x = 10;
        function f(a = x * 2) { return a; }
        f()
    )").as_number(), 20.0);
    EXPECT_EQ(vm_ok(R"(
        function f(a = (function() { return 7; })()) { return a; }
        f()
    )").as_number(), 7.0);
    EXPECT_EQ(vm_ok(R"(
        function f(a = (() => 8)()) { return a; }
        f()
    )").as_number(), 8.0);
}

// DP-37: 函数声明与函数表达式默认值语义一致性
TEST(DefaultParams, InterpDeclVsExprConsistency) {
    // 函数声明：无参调用 → 默认值
    EXPECT_EQ(interp_ok("function f(a = 5) { return a; } f()").as_number(), 5.0);
    // 函数表达式：无参调用 → 默认值
    EXPECT_EQ(interp_ok("var f = function(a = 5) { return a; }; f()").as_number(), 5.0);
    // 两者 length 一致
    EXPECT_EQ(interp_ok("function f(a = 5) {} f.length").as_number(), 0.0);
    EXPECT_EQ(interp_ok("var f = function(a = 5) {}; f.length").as_number(), 0.0);
    // 多参数 length 截断一致
    EXPECT_EQ(interp_ok("function f(a, b = 2, c) {} f.length").as_number(), 1.0);
    EXPECT_EQ(interp_ok("var f = function(a, b = 2, c) {}; f.length").as_number(), 1.0);
}

// DP-38: VM - 函数声明与函数表达式默认值一致性
TEST(DefaultParams, VmDeclVsExprConsistency) {
    EXPECT_EQ(vm_ok("function f(a = 5) { return a; } f()").as_number(), 5.0);
    EXPECT_EQ(vm_ok("var f = function(a = 5) { return a; }; f()").as_number(), 5.0);
    EXPECT_EQ(vm_ok("function f(a = 5) {} f.length").as_number(), 0.0);
    EXPECT_EQ(vm_ok("var f = function(a = 5) {}; f.length").as_number(), 0.0);
    EXPECT_EQ(vm_ok("function f(a, b = 2, c) {} f.length").as_number(), 1.0);
    EXPECT_EQ(vm_ok("var f = function(a, b = 2, c) {}; f.length").as_number(), 1.0);
}

// DP-39: arguments 对象反映实际调用参数，而非默认值填充后的形参
// (non-simple 参数列表 → unmapped arguments 语义)
TEST(DefaultParams, InterpArgumentsUnmappedWithDefault) {
    // f() 传入 0 个实参，arguments.length === 0
    {
        auto pr = parse_program("function f(a = 1) { return arguments.length; } f()");
        ASSERT_TRUE(pr.ok());
        Interpreter interp;
        auto r = interp.exec(pr.value());
        ASSERT_TRUE(r.is_ok()) << "exec error: " << r.error().message();
        ASSERT_TRUE(r.value().is_number()) << "expected number for arguments.length";
        EXPECT_EQ(r.value().as_number(), 0.0);
    }
    // f(undefined) 传入 1 个实参，arguments.length === 1
    {
        auto pr = parse_program("function f(a = 1) { return arguments.length; } f(undefined)");
        ASSERT_TRUE(pr.ok());
        Interpreter interp;
        auto r = interp.exec(pr.value());
        ASSERT_TRUE(r.is_ok()) << "exec error: " << r.error().message();
        ASSERT_TRUE(r.value().is_number()) << "expected number for arguments.length (f(undefined))";
        EXPECT_EQ(r.value().as_number(), 1.0);
    }
    // f(undefined) 时 arguments[0] === undefined（原始调用值，非默认值 1）
    {
        auto pr = parse_program("function f(a = 1) { return arguments[0]; } f(undefined)");
        ASSERT_TRUE(pr.ok());
        Interpreter interp;
        auto r = interp.exec(pr.value());
        ASSERT_TRUE(r.is_ok()) << "exec error: " << r.error().message();
        EXPECT_TRUE(r.value().is_undefined());
    }
    // f(2) 时 arguments[0] === 2
    {
        auto pr = parse_program("function f(a = 1) { return arguments[0]; } f(2)");
        ASSERT_TRUE(pr.ok());
        Interpreter interp;
        auto r = interp.exec(pr.value());
        ASSERT_TRUE(r.is_ok()) << "exec error: " << r.error().message();
        ASSERT_TRUE(r.value().is_number());
        EXPECT_EQ(r.value().as_number(), 2.0);
    }
}

// DP-40: VM - arguments 对象 unmapped 语义
TEST(DefaultParams, VmArgumentsUnmappedWithDefault) {
    // f() 传入 0 个实参，arguments.length === 0
    {
        auto pr = parse_program("function f(a = 1) { return arguments.length; } f()");
        ASSERT_TRUE(pr.ok());
        Compiler compiler;
        auto bytecode = compiler.compile(pr.value());
        VM vm;
        auto r = vm.exec(bytecode);
        ASSERT_TRUE(r.is_ok()) << "exec error: " << r.error().message();
        ASSERT_TRUE(r.value().is_number()) << "expected number for arguments.length (f() VM)";
        EXPECT_EQ(r.value().as_number(), 0.0);
    }
    // f(undefined) 传入 1 个实参，arguments.length === 1
    {
        auto pr = parse_program("function f(a = 1) { return arguments.length; } f(undefined)");
        ASSERT_TRUE(pr.ok());
        Compiler compiler;
        auto bytecode = compiler.compile(pr.value());
        VM vm;
        auto r = vm.exec(bytecode);
        ASSERT_TRUE(r.is_ok()) << "exec error: " << r.error().message();
        ASSERT_TRUE(r.value().is_number()) << "expected number";
        EXPECT_EQ(r.value().as_number(), 1.0);
    }
    // f(undefined) 时 arguments[0] === undefined
    {
        auto pr = parse_program("function f(a = 1) { return arguments[0]; } f(undefined)");
        ASSERT_TRUE(pr.ok());
        Compiler compiler;
        auto bytecode = compiler.compile(pr.value());
        VM vm;
        auto r = vm.exec(bytecode);
        ASSERT_TRUE(r.is_ok()) << "exec error: " << r.error().message();
        EXPECT_TRUE(r.value().is_undefined());
    }
    // f(2) 时 arguments[0] === 2
    {
        auto pr = parse_program("function f(a = 1) { return arguments[0]; } f(2)");
        ASSERT_TRUE(pr.ok());
        Compiler compiler;
        auto bytecode = compiler.compile(pr.value());
        VM vm;
        auto r = vm.exec(bytecode);
        ASSERT_TRUE(r.is_ok()) << "exec error: " << r.error().message();
        ASSERT_TRUE(r.value().is_number());
        EXPECT_EQ(r.value().as_number(), 2.0);
    }
}

// DP-41: Parser — 简单参数列表的重复参数在宽松模式下合法
// （simple params：无默认值/无 rest/无解构，宽松模式可重名）
TEST(DefaultParams, ParserSimpleDuplicateParamsSloppy) {
    // 简单参数列表 + 重复名 → 宽松模式允许（parse OK）
    EXPECT_FALSE(parse_fails("function f(a, a) { return a; }"));
    EXPECT_FALSE(parse_fails("(function(a, a) {})"));
    // 注：function f(a = 1, a = 2) {} 按规范应为 SyntaxError（非 simple 参数列表不允许重复），
    // 但当前实现暂未在 Parse 阶段检测此场景，此处不添加强制失败的用例。
}

// DP-42: M1 — 默认值引用前序参数（双默认，前序参数也用默认值）
TEST(DefaultParams, InterpForwardRefBothDefault) {
    // f(a=1, b=a+1) 无实参 → b should use a=1 computed from default
    EXPECT_EQ(interp_ok("function f(a = 1, b = a + 1) { return b; } f()").as_number(), 2.0);
}

TEST(DefaultParams, VmForwardRefBothDefault) {
    EXPECT_EQ(vm_ok("function f(a = 1, b = a + 1) { return b; } f()").as_number(), 2.0);
}

// DP-43: M2 — 默认值表达式中可引用 arguments
TEST(DefaultParams, InterpArgumentsInDefault) {
    // f() → arguments.length === 0, default a = 0
    EXPECT_EQ(interp_ok("function f(a = arguments.length) { return a; } f()").as_number(), 0.0);
    // f(99) → arguments.length === 1, param is provided so default not used
    EXPECT_EQ(interp_ok("function f(a = arguments.length) { return a; } f(99)").as_number(), 99.0);
    // f(undefined, 'x') → arguments.length === 2, a uses default = 2
    EXPECT_EQ(
        interp_ok("function f(a = arguments.length) { return a; } f(undefined, 'x')").as_number(),
        2.0);
}

// DP-44: M3 — VM body var 声明在默认值求值时不可见（应透过到外层作用域）
TEST(DefaultParams, VmBodyVarNotVisibleInDefault) {
    // 外层有 x='outer'，body 有 var x，默认值表达式 `x` 应看到外层 'outer'
    EXPECT_EQ(
        vm_ok("var x = 'outer'; function f(a = x) { var x = 'inner'; return a; } f()").as_string(),
        "outer");
    // 解释器也应一致
    EXPECT_EQ(
        interp_ok(
            "var x = 'outer'; function f(a = x) { var x = 'inner'; return a; } f()").as_string(),
        "outer");
}

// DP-45: M4 — 默认值为赋值表达式（f(a = x = 1)）
TEST(DefaultParams, InterpDefaultIsAssignExpr) {
    // a 的默认值是赋值表达式 `x = 1`
    EXPECT_EQ(interp_ok("var x = 0; function f(a = (x = 1)) { return a; } f(); x").as_number(),
              1.0);
    // 调用时默认值被求值，x 被赋为 1
    EXPECT_EQ(
        interp_ok("var x = 0; function f(a = (x = 99)) { return x; } f(); x").as_number(), 99.0);
}

TEST(DefaultParams, VmDefaultIsAssignExpr) {
    EXPECT_EQ(vm_ok("var x = 0; function f(a = (x = 1)) { return a; } f(); x").as_number(), 1.0);
    EXPECT_EQ(
        vm_ok("var x = 0; function f(a = (x = 99)) { return x; } f(); x").as_number(), 99.0);
}

}  // namespace
