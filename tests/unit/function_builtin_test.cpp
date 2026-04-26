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
// Interpreter: Function.prototype.call/apply/bind tests
// ============================================================

TEST(InterpFunctionBuiltin, FB01_call_this_binding) {
    // FB-01: fn.call({x:42}) — this.x === 42
    auto v = interp_ok(R"(
        function getX() { return this.x; }
        getX.call({x: 42});
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 42.0);
}

TEST(InterpFunctionBuiltin, FB02_call_args_passing) {
    // FB-02: fn.call(null, 1, 2) — args passed
    auto v = interp_ok(R"(
        function add(a, b) { return a + b; }
        add.call(null, 1, 2);
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 3.0);
}

TEST(InterpFunctionBuiltin, FB03_call_undefined_this) {
    // FB-03: fn.call(undefined) — this is undefined, no crash
    auto v = interp_ok(R"(
        function retUndef() { return 99; }
        retUndef.call(undefined);
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 99.0);
}

TEST(InterpFunctionBuiltin, FB04_call_on_non_function_throws) {
    // FB-04: (123).call() — TypeError (caught via try/catch)
    auto v = interp_ok(R"(
        var caught = false;
        try { (123).call(); } catch(e) { caught = true; }
        caught;
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(InterpFunctionBuiltin, FB05_call_no_args) {
    // FB-05: fn.call() — equivalent to fn()
    auto v = interp_ok(R"(
        function ret7() { return 7; }
        ret7.call();
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 7.0);
}

TEST(InterpFunctionBuiltin, FB06_apply_array_spread) {
    // FB-06: fn.apply({x:1}, [2, 3]) — array spread
    auto v = interp_ok(R"(
        function sum(a, b) { return a + b; }
        sum.apply(null, [2, 3]);
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 5.0);
}

TEST(InterpFunctionBuiltin, FB07_apply_null_args) {
    // FB-07: fn.apply(null, null) — no args
    auto v = interp_ok(R"(
        function noArgs() { return 42; }
        noArgs.apply(null, null);
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 42.0);
}

TEST(InterpFunctionBuiltin, FB08_apply_undefined_args) {
    // FB-08: fn.apply(null, undefined) — no args
    auto v = interp_ok(R"(
        function noArgs() { return 42; }
        noArgs.apply(null, undefined);
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 42.0);
}

TEST(InterpFunctionBuiltin, FB09_apply_non_array_throws) {
    // FB-09: fn.apply(null, 42) — TypeError
    auto v = interp_ok(R"(
        var caught = false;
        function f() {}
        try { f.apply(null, 42); } catch(e) { caught = true; }
        caught;
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(InterpFunctionBuiltin, FB10_apply_array_like) {
    // FB-10: fn.apply(null, {length:2, 0:'a', 1:'b'}) — array-like
    auto v = interp_ok(R"(
        function join(a, b) { return a + b; }
        join.apply(null, {length: 2, 0: "hello", 1: "world"});
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "helloworld");
}

TEST(InterpFunctionBuiltin, FB11_bind_this_binding) {
    // FB-11: fn.bind({x:99})() — this bound
    auto v = interp_ok(R"(
        function getX() { return this.x; }
        var bound = getX.bind({x: 99});
        bound();
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 99.0);
}

TEST(InterpFunctionBuiltin, FB12_bind_prepend_args) {
    // FB-12: fn.bind(null, 10)(1, 2) — prepend args
    auto v = interp_ok(R"(
        function sum3(a, b, c) { return a + b + c; }
        var bound = sum3.bind(null, 10);
        bound(1, 2);
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 13.0);
}

TEST(InterpFunctionBuiltin, FB13_bind_length_no_args) {
    // FB-13: fn.bind(null).length === target.length
    auto v = interp_ok(R"(
        function f(a, b, c) {}
        f.bind(null).length;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 3.0);
}

TEST(InterpFunctionBuiltin, FB14_bind_length_with_args) {
    // FB-14: fn.bind(null, 1).length === max(target.length - 1, 0)
    auto v = interp_ok(R"(
        function f(a, b, c) {}
        f.bind(null, 1).length;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.0);
}

TEST(InterpFunctionBuiltin, FB15_bind_name) {
    // FB-15: fn.bind(null).name === "bound fn"
    auto v = interp_ok(R"(
        function fn() {}
        fn.bind(null).name;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "bound fn");
}

TEST(InterpFunctionBuiltin, FB16_double_bind) {
    // FB-16: fn.bind(a).bind(b)() — this fixed to a
    auto v = interp_ok(R"(
        function getX() { return this.x; }
        var a = {x: 1};
        var b = {x: 2};
        getX.bind(a).bind(b)();
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

// ============================================================
// VM: Function.prototype.call/apply/bind tests
// ============================================================

TEST(VMFunctionBuiltin, FB01_call_this_binding) {
    auto v = vm_ok(R"(
        function getX() { return this.x; }
        getX.call({x: 42});
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 42.0);
}

TEST(VMFunctionBuiltin, FB02_call_args_passing) {
    auto v = vm_ok(R"(
        function add(a, b) { return a + b; }
        add.call(null, 1, 2);
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 3.0);
}

TEST(VMFunctionBuiltin, FB03_call_undefined_this) {
    auto v = vm_ok(R"(
        function retUndef() { return 99; }
        retUndef.call(undefined);
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 99.0);
}

TEST(VMFunctionBuiltin, FB04_call_on_non_function_throws) {
    auto v = vm_ok(R"(
        var caught = false;
        try { (123).call(); } catch(e) { caught = true; }
        caught;
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(VMFunctionBuiltin, FB05_call_no_args) {
    auto v = vm_ok(R"(
        function ret7() { return 7; }
        ret7.call();
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 7.0);
}

TEST(VMFunctionBuiltin, FB06_apply_array_spread) {
    auto v = vm_ok(R"(
        function sum(a, b) { return a + b; }
        sum.apply(null, [2, 3]);
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 5.0);
}

TEST(VMFunctionBuiltin, FB07_apply_null_args) {
    auto v = vm_ok(R"(
        function noArgs() { return 42; }
        noArgs.apply(null, null);
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 42.0);
}

TEST(VMFunctionBuiltin, FB08_apply_undefined_args) {
    auto v = vm_ok(R"(
        function noArgs() { return 42; }
        noArgs.apply(null, undefined);
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 42.0);
}

TEST(VMFunctionBuiltin, FB09_apply_non_array_throws) {
    auto v = vm_ok(R"(
        var caught = false;
        function f() {}
        try { f.apply(null, 42); } catch(e) { caught = true; }
        caught;
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(VMFunctionBuiltin, FB10_apply_array_like) {
    auto v = vm_ok(R"(
        function join(a, b) { return a + b; }
        join.apply(null, {length: 2, 0: "hello", 1: "world"});
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "helloworld");
}

TEST(VMFunctionBuiltin, FB11_bind_this_binding) {
    auto v = vm_ok(R"(
        function getX() { return this.x; }
        var bound = getX.bind({x: 99});
        bound();
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 99.0);
}

TEST(VMFunctionBuiltin, FB12_bind_prepend_args) {
    auto v = vm_ok(R"(
        function sum3(a, b, c) { return a + b + c; }
        var bound = sum3.bind(null, 10);
        bound(1, 2);
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 13.0);
}

TEST(VMFunctionBuiltin, FB13_bind_length_no_args) {
    auto v = vm_ok(R"(
        function f(a, b, c) {}
        f.bind(null).length;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 3.0);
}

TEST(VMFunctionBuiltin, FB14_bind_length_with_args) {
    auto v = vm_ok(R"(
        function f(a, b, c) {}
        f.bind(null, 1).length;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.0);
}

TEST(VMFunctionBuiltin, FB15_bind_name) {
    auto v = vm_ok(R"(
        function fn() {}
        fn.bind(null).name;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "bound fn");
}

TEST(VMFunctionBuiltin, FB16_double_bind) {
    auto v = vm_ok(R"(
        function getX() { return this.x; }
        var a = {x: 1};
        var b = {x: 2};
        getX.bind(a).bind(b)();
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

// ============================================================
// Helpers for error-path tests (not present in original file)
// ============================================================

bool interp_err(std::string_view source) {
    auto parse_result = qppjs::parse_program(source);
    if (!parse_result.ok()) return true;
    qppjs::Interpreter interp;
    auto result = interp.exec(parse_result.value());
    return !result.is_ok();
}

bool vm_err(std::string_view source) {
    auto parse_result = qppjs::parse_program(source);
    if (!parse_result.ok()) return true;
    qppjs::Compiler compiler;
    auto bytecode = compiler.compile(parse_result.value());
    qppjs::VM vm;
    auto result = vm.exec(bytecode);
    return !result.is_ok();
}

// ============================================================
// Interpreter: edge / error-path / regression tests
// ============================================================

// FB-E01: call — thisArg is null (strict-mode semantics: passed as-is, no crash)
TEST(InterpFunctionBuiltin, FB_E01_call_null_this_no_crash) {
    auto v = interp_ok(R"(
        function f() { return 1; }
        f.call(null);
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

// FB-E02: call — thisArg is a number primitive (passed as-is, no coercion)
TEST(InterpFunctionBuiltin, FB_E02_call_primitive_this) {
    auto v = interp_ok(R"(
        function f() { return 2; }
        f.call(42);
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.0);
}

// FB-E03: apply — string as argsArray → TypeError (direct error path)
TEST(InterpFunctionBuiltin, FB_E03_apply_string_args_throws) {
    EXPECT_TRUE(interp_err(R"(
        function f() {}
        f.apply(null, "abc");
    )"));
}

// FB-E04: apply — boolean as argsArray → TypeError (direct error path)
TEST(InterpFunctionBuiltin, FB_E04_apply_bool_args_throws) {
    EXPECT_TRUE(interp_err(R"(
        function f() {}
        f.apply(null, true);
    )"));
}

// FB-E05: apply — array-like with length=0 → no args, no crash
TEST(InterpFunctionBuiltin, FB_E05_apply_array_like_length_zero) {
    auto v = interp_ok(R"(
        function f() { return 7; }
        f.apply(null, {length: 0});
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 7.0);
}

// FB-E06: apply — array-like missing numeric keys → undefined fill
TEST(InterpFunctionBuiltin, FB_E06_apply_array_like_sparse) {
    auto v = interp_ok(R"(
        var result = 0;
        function f(a, b) { if (b === undefined) { result = 99; } }
        f.apply(null, {length: 2, 0: 1});
        result;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 99.0);
}

// FB-E07: bind — more bound args than params → length clamped to 0
TEST(InterpFunctionBuiltin, FB_E07_bind_length_clamp_to_zero) {
    auto v = interp_ok(R"(
        function f(a) {}
        f.bind(null, 1, 2, 3).length;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}

// FB-E08: bind — zero-param function → length stays 0 after bind
TEST(InterpFunctionBuiltin, FB_E08_bind_zero_param_length) {
    auto v = interp_ok(R"(
        function f() {}
        f.bind(null).length;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}

// FB-E09: bind — anonymous function name is "bound "
TEST(InterpFunctionBuiltin, FB_E09_bind_anonymous_name) {
    auto v = interp_ok(R"(
        var f = function() {};
        f.bind(null).name;
    )");
    EXPECT_TRUE(v.is_string());
    // anonymous target name is empty → "bound "
    EXPECT_EQ(v.as_string(), "bound ");
}

// FB-E10: apply on non-function → TypeError (direct error path)
TEST(InterpFunctionBuiltin, FB_E10_apply_on_non_function_throws) {
    EXPECT_TRUE(interp_err("(123).apply(null, [])"));
}

// FB-E11: bind on non-function → TypeError (direct error path)
TEST(InterpFunctionBuiltin, FB_E11_bind_on_non_function_throws) {
    EXPECT_TRUE(interp_err("(123).bind(null)"));
}

// FB-E12: call — returned value is correctly propagated
TEST(InterpFunctionBuiltin, FB_E12_call_return_value_propagation) {
    auto v = interp_ok(R"(
        function f(x) { return x * 2; }
        f.call(null, 21);
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 42.0);
}

// FB-E13: bound function invoked via call — bound this wins over call's this
TEST(InterpFunctionBuiltin, FB_E13_bound_fn_call_this_ignored) {
    auto v = interp_ok(R"(
        function getX() { return this.x; }
        var bound = getX.bind({x: 10});
        bound.call({x: 99});
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 10.0);
}

// FB-E14: apply — this binding respected through apply
TEST(InterpFunctionBuiltin, FB_E14_apply_this_binding) {
    auto v = interp_ok(R"(
        function sum(a, b) { return this.base + a + b; }
        sum.apply({base: 100}, [1, 2]);
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 103.0);
}

// FB-E15: call — excess args beyond params are silently ignored
TEST(InterpFunctionBuiltin, FB_E15_call_excess_args_ignored) {
    auto v = interp_ok(R"(
        function f(a) { return a; }
        f.call(null, 5, 6, 7);
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 5.0);
}

// FB-E16: call — fewer args than params → missing args are undefined
TEST(InterpFunctionBuiltin, FB_E16_call_fewer_args_undefined) {
    auto v = interp_ok(R"(
        var result = 0;
        function f(a, b) { if (b === undefined) { result = 1; } }
        f.call(null, 42);
        result;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

// ============================================================
// VM: edge / error-path / regression tests
// ============================================================

TEST(VMFunctionBuiltin, FB_E01_call_null_this_no_crash) {
    auto v = vm_ok(R"(
        function f() { return 1; }
        f.call(null);
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

TEST(VMFunctionBuiltin, FB_E02_call_primitive_this) {
    auto v = vm_ok(R"(
        function f() { return 2; }
        f.call(42);
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.0);
}

TEST(VMFunctionBuiltin, FB_E03_apply_string_args_throws) {
    EXPECT_TRUE(vm_err(R"(
        function f() {}
        f.apply(null, "abc");
    )"));
}

TEST(VMFunctionBuiltin, FB_E04_apply_bool_args_throws) {
    EXPECT_TRUE(vm_err(R"(
        function f() {}
        f.apply(null, true);
    )"));
}

TEST(VMFunctionBuiltin, FB_E05_apply_array_like_length_zero) {
    auto v = vm_ok(R"(
        function f() { return 7; }
        f.apply(null, {length: 0});
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 7.0);
}

TEST(VMFunctionBuiltin, FB_E06_apply_array_like_sparse) {
    auto v = vm_ok(R"(
        var result = 0;
        function f(a, b) { if (b === undefined) { result = 99; } }
        f.apply(null, {length: 2, 0: 1});
        result;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 99.0);
}

TEST(VMFunctionBuiltin, FB_E07_bind_length_clamp_to_zero) {
    auto v = vm_ok(R"(
        function f(a) {}
        f.bind(null, 1, 2, 3).length;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}

TEST(VMFunctionBuiltin, FB_E08_bind_zero_param_length) {
    auto v = vm_ok(R"(
        function f() {}
        f.bind(null).length;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}

TEST(VMFunctionBuiltin, FB_E09_bind_anonymous_name) {
    auto v = vm_ok(R"(
        var f = function() {};
        f.bind(null).name;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "bound ");
}

TEST(VMFunctionBuiltin, FB_E10_apply_on_non_function_throws) {
    EXPECT_TRUE(vm_err("(123).apply(null, [])"));
}

TEST(VMFunctionBuiltin, FB_E11_bind_on_non_function_throws) {
    EXPECT_TRUE(vm_err("(123).bind(null)"));
}

TEST(VMFunctionBuiltin, FB_E12_call_return_value_propagation) {
    auto v = vm_ok(R"(
        function f(x) { return x * 2; }
        f.call(null, 21);
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 42.0);
}

TEST(VMFunctionBuiltin, FB_E13_bound_fn_call_this_ignored) {
    auto v = vm_ok(R"(
        function getX() { return this.x; }
        var bound = getX.bind({x: 10});
        bound.call({x: 99});
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 10.0);
}

TEST(VMFunctionBuiltin, FB_E14_apply_this_binding) {
    auto v = vm_ok(R"(
        function sum(a, b) { return this.base + a + b; }
        sum.apply({base: 100}, [1, 2]);
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 103.0);
}

TEST(VMFunctionBuiltin, FB_E15_call_excess_args_ignored) {
    auto v = vm_ok(R"(
        function f(a) { return a; }
        f.call(null, 5, 6, 7);
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 5.0);
}

TEST(VMFunctionBuiltin, FB_E16_call_fewer_args_undefined) {
    auto v = vm_ok(R"(
        var result = 0;
        function f(a, b) { if (b === undefined) { result = 1; } }
        f.call(null, 42);
        result;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

// ============================================================
// M1: bind + new — bound-this must be ignored, new instance used
// ============================================================

TEST(InterpFunctionBuiltin, FB_M1_bind_new_ignores_bound_this) {
    // new (fn.bind(boundObj))() must write to new instance, not boundObj
    auto v = interp_ok(R"(
        var bound = (function() { this.x = 1; }).bind({x: 99});
        var o = new bound();
        o.x;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

TEST(InterpFunctionBuiltin, FB_M1_bind_new_with_bound_args) {
    // new (Ctor.bind(null, 42))() — bound args forwarded, new instance returned
    auto v = interp_ok(R"(
        function Ctor(x) { this.x = x; }
        var b = Ctor.bind(null, 42);
        var o = new b();
        o.x;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 42.0);
}

// ============================================================
// M2: apply array-like — negative/NaN length must not crash
// ============================================================

TEST(InterpFunctionBuiltin, FB_M2_apply_negative_length) {
    // {length: -1} → treat as 0 args, no crash
    auto v = interp_ok(R"(
        var called = false;
        function f() { called = true; }
        f.apply(null, {length: -1});
        called;
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_EQ(v.as_bool(), true);
}

TEST(InterpFunctionBuiltin, FB_M2_apply_nan_length) {
    // {length: NaN} → treat as 0 args, no crash
    auto v = interp_ok(R"(
        var called = false;
        function f() { called = true; }
        f.apply(null, {length: 0/0});
        called;
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_EQ(v.as_bool(), true);
}

// ============================================================
// S1: chained bind — name must be "bound bound foo"
// ============================================================

TEST(InterpFunctionBuiltin, FB_S1_chained_bind_name) {
    auto v = interp_ok(R"(
        function foo() {}
        foo.bind(null).bind(null).name;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "bound bound foo");
}

// ============================================================
// VM: M1 / M2 / S1 symmetric tests
// ============================================================

TEST(VMFunctionBuiltin, FB_M1_bind_new_ignores_bound_this) {
    auto v = vm_ok(R"(
        var bound = (function() { this.x = 1; }).bind({x: 99});
        var o = new bound();
        o.x;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

TEST(VMFunctionBuiltin, FB_M1_bind_new_with_bound_args) {
    auto v = vm_ok(R"(
        function Ctor(x) { this.x = x; }
        var b = Ctor.bind(null, 42);
        var o = new b();
        o.x;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 42.0);
}

TEST(VMFunctionBuiltin, FB_M2_apply_negative_length) {
    auto v = vm_ok(R"(
        var called = false;
        function f() { called = true; }
        f.apply(null, {length: -1});
        called;
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_EQ(v.as_bool(), true);
}

TEST(VMFunctionBuiltin, FB_M2_apply_nan_length) {
    auto v = vm_ok(R"(
        var called = false;
        function f() { called = true; }
        f.apply(null, {length: 0/0});
        called;
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_EQ(v.as_bool(), true);
}

TEST(VMFunctionBuiltin, FB_S1_chained_bind_name) {
    auto v = vm_ok(R"(
        function foo() {}
        foo.bind(null).bind(null).name;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "bound bound foo");
}

}  // namespace
