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

// ============================================================
// DP-01: 数组解构参数 function foo([a,b])
// ============================================================

TEST(DstrParams, DP01_ArrayDestructInterp) {
    auto v = interp_ok(R"(
        function foo([a, b]) { return a + b; }
        foo([10, 20])
    )");
    EXPECT_EQ(v.as_number(), 30.0);
}

TEST(DstrParams, DP01_ArrayDestructVM) {
    auto v = vm_ok(R"(
        function foo([a, b]) { return a + b; }
        foo([10, 20])
    )");
    EXPECT_EQ(v.as_number(), 30.0);
}

// ============================================================
// DP-02: 对象解构参数 function foo({a,b})
// ============================================================

TEST(DstrParams, DP02_ObjectDestructInterp) {
    auto v = interp_ok(R"(
        function foo({a, b}) { return a + b; }
        foo({a: 3, b: 7})
    )");
    EXPECT_EQ(v.as_number(), 10.0);
}

TEST(DstrParams, DP02_ObjectDestructVM) {
    auto v = vm_ok(R"(
        function foo({a, b}) { return a + b; }
        foo({a: 3, b: 7})
    )");
    EXPECT_EQ(v.as_number(), 10.0);
}

// ============================================================
// DP-03: 嵌套解构 function foo({a:[b,c]})
// ============================================================

TEST(DstrParams, DP03_NestedDestructInterp) {
    auto v = interp_ok(R"(
        function foo({a: [b, c]}) { return b + c; }
        foo({a: [4, 6]})
    )");
    EXPECT_EQ(v.as_number(), 10.0);
}

TEST(DstrParams, DP03_NestedDestructVM) {
    auto v = vm_ok(R"(
        function foo({a: [b, c]}) { return b + c; }
        foo({a: [4, 6]})
    )");
    EXPECT_EQ(v.as_number(), 10.0);
}

// ============================================================
// DP-04: 数组解构带默认值 function foo([a=1, b=2])
// ============================================================

TEST(DstrParams, DP04_ArrayDestructDefaultInterp) {
    auto v = interp_ok(R"(
        function foo([a=1, b=2]) { return a + b; }
        foo([10])
    )");
    EXPECT_EQ(v.as_number(), 12.0);
}

TEST(DstrParams, DP04_ArrayDestructDefaultVM) {
    auto v = vm_ok(R"(
        function foo([a=1, b=2]) { return a + b; }
        foo([10])
    )");
    EXPECT_EQ(v.as_number(), 12.0);
}

// ============================================================
// DP-05: 箭头函数解构参数 ([a,b]) => a+b
// ============================================================

TEST(DstrParams, DP05_ArrowArrayDestructInterp) {
    auto v = interp_ok(R"(
        const f = ([a, b]) => a + b;
        f([5, 6])
    )");
    EXPECT_EQ(v.as_number(), 11.0);
}

TEST(DstrParams, DP05_ArrowArrayDestructVM) {
    auto v = vm_ok(R"(
        const f = ([a, b]) => a + b;
        f([5, 6])
    )");
    EXPECT_EQ(v.as_number(), 11.0);
}

// ============================================================
// DP-06: class 方法参数解构
// ============================================================

TEST(DstrParams, DP06_ClassMethodDestructInterp) {
    auto v = interp_ok(R"(
        class Foo {
            sum({x, y}) { return x + y; }
        }
        new Foo().sum({x: 3, y: 4})
    )");
    EXPECT_EQ(v.as_number(), 7.0);
}

TEST(DstrParams, DP06_ClassMethodDestructVM) {
    auto v = vm_ok(R"(
        class Foo {
            sum({x, y}) { return x + y; }
        }
        new Foo().sum({x: 3, y: 4})
    )");
    EXPECT_EQ(v.as_number(), 7.0);
}

// ============================================================
// DP-07: rest + 解构混合 function foo([a], ...rest)
// ============================================================

TEST(DstrParams, DP07_RestAndDestructInterp) {
    auto v = interp_ok(R"(
        function foo([a], ...rest) { return a + rest[0]; }
        foo([10], 20)
    )");
    EXPECT_EQ(v.as_number(), 30.0);
}

TEST(DstrParams, DP07_RestAndDestructVM) {
    auto v = vm_ok(R"(
        function foo([a], ...rest) { return a + rest[0]; }
        foo([10], 20)
    )");
    EXPECT_EQ(v.as_number(), 30.0);
}

// ============================================================
// DP-08: 对象方法参数解构（对象字面量方法简写）
// ============================================================

TEST(DstrParams, DP08_MethodShorthandDestructInterp) {
    auto v = interp_ok(R"(
        const obj = {
            sum({a, b}) { return a + b; }
        };
        obj.sum({a: 8, b: 9})
    )");
    EXPECT_EQ(v.as_number(), 17.0);
}

TEST(DstrParams, DP08_MethodShorthandDestructVM) {
    auto v = vm_ok(R"(
        const obj = {
            sum({a, b}) { return a + b; }
        };
        obj.sum({a: 8, b: 9})
    )");
    EXPECT_EQ(v.as_number(), 17.0);
}

// ============================================================
// DP-09: 混合参数（普通 + 解构 + 普通）
// ============================================================

TEST(DstrParams, DP09_MixedParamsInterp) {
    auto v = interp_ok(R"(
        function foo(x, [a, b], y) { return x + a + b + y; }
        foo(1, [2, 3], 4)
    )");
    EXPECT_EQ(v.as_number(), 10.0);
}

TEST(DstrParams, DP09_MixedParamsVM) {
    auto v = vm_ok(R"(
        function foo(x, [a, b], y) { return x + a + b + y; }
        foo(1, [2, 3], 4)
    )");
    EXPECT_EQ(v.as_number(), 10.0);
}

// ============================================================
// DP-10: 对象解构带重命名 function foo({a: x, b: y})
// ============================================================

TEST(DstrParams, DP10_ObjectDestructRenameInterp) {
    auto v = interp_ok(R"(
        function foo({a: x, b: y}) { return x + y; }
        foo({a: 100, b: 200})
    )");
    EXPECT_EQ(v.as_number(), 300.0);
}

TEST(DstrParams, DP10_ObjectDestructRenameVM) {
    auto v = vm_ok(R"(
        function foo({a: x, b: y}) { return x + y; }
        foo({a: 100, b: 200})
    )");
    EXPECT_EQ(v.as_number(), 300.0);
}

// ============================================================
// DP-11: 对象解构带默认值 function foo({a=10, b=20})
// ============================================================

TEST(DstrParams, DP11_ObjectDestructDefaultInterp) {
    auto v = interp_ok(R"(
        function foo({a=10, b=20}) { return a + b; }
        foo({a: 5})
    )");
    EXPECT_EQ(v.as_number(), 25.0);
}

TEST(DstrParams, DP11_ObjectDestructDefaultVM) {
    auto v = vm_ok(R"(
        function foo({a=10, b=20}) { return a + b; }
        foo({a: 5})
    )");
    EXPECT_EQ(v.as_number(), 25.0);
}

// ============================================================
// DP-12: 解构参数 + 整体默认值 function foo([a,b]=[1,2])
// ============================================================

TEST(DstrParams, DP12_DestructWithWholDefaultInterp) {
    auto v = interp_ok(R"(
        function foo([a, b] = [1, 2]) { return a + b; }
        foo()
    )");
    EXPECT_EQ(v.as_number(), 3.0);
}

TEST(DstrParams, DP12_DestructWithWholDefaultVM) {
    auto v = vm_ok(R"(
        function foo([a, b] = [1, 2]) { return a + b; }
        foo()
    )");
    EXPECT_EQ(v.as_number(), 3.0);
}

// ============================================================
// DP-13: 多个解构参数
// ============================================================

TEST(DstrParams, DP13_MultipleDestructParamsInterp) {
    auto v = interp_ok(R"(
        function foo([a, b], {c, d}) { return a + b + c + d; }
        foo([1, 2], {c: 3, d: 4})
    )");
    EXPECT_EQ(v.as_number(), 10.0);
}

TEST(DstrParams, DP13_MultipleDestructParamsVM) {
    auto v = vm_ok(R"(
        function foo([a, b], {c, d}) { return a + b + c + d; }
        foo([1, 2], {c: 3, d: 4})
    )");
    EXPECT_EQ(v.as_number(), 10.0);
}

// ============================================================
// DP-14: 箭头函数对象解构参数
// ============================================================

TEST(DstrParams, DP14_ArrowObjectDestructInterp) {
    auto v = interp_ok(R"(
        const f = ({x, y}) => x * y;
        f({x: 3, y: 4})
    )");
    EXPECT_EQ(v.as_number(), 12.0);
}

TEST(DstrParams, DP14_ArrowObjectDestructVM) {
    auto v = vm_ok(R"(
        const f = ({x, y}) => x * y;
        f({x: 3, y: 4})
    )");
    EXPECT_EQ(v.as_number(), 12.0);
}

// ============================================================
// DP-15: 函数 .length 不计解构参数
// ============================================================

TEST(DstrParams, DP15_LengthIgnoresDestructInterp) {
    auto v = interp_ok(R"(
        function foo(a, [b, c], d) {}
        foo.length
    )");
    // 解构参数不计入 .length（按规范，length = 第一个有默认值/rest/解构参数前的参数数）
    // 这里 a 是第一个，[b,c] 是解构参数，length 应为 1
    EXPECT_EQ(v.as_number(), 1.0);
}

TEST(DstrParams, DP15_LengthIgnoresDestructVM) {
    auto v = vm_ok(R"(
        function foo(a, [b, c], d) {}
        foo.length
    )");
    EXPECT_EQ(v.as_number(), 1.0);
}

}  // namespace
