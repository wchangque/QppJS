#include "qppjs/frontend/ast.h"
#include "qppjs/frontend/ast_dump.h"
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

static bool interp_throws(std::string_view source) {
    auto parse_result = parse_program(source);
    EXPECT_TRUE(parse_result.ok()) << "parse failed: " << parse_result.error().message();
    if (!parse_result.ok()) return false;
    Interpreter interp;
    auto result = interp.exec(parse_result.value());
    return !result.is_ok();
}

static bool vm_throws(std::string_view source) {
    auto parse_result = parse_program(source);
    EXPECT_TRUE(parse_result.ok()) << "parse failed: " << parse_result.error().message();
    if (!parse_result.ok()) return false;
    Compiler compiler;
    auto bytecode = compiler.compile(parse_result.value());
    VM vm;
    auto result = vm.exec(bytecode);
    return !result.is_ok();
}

// ============================================================
// MS-01: Parser — 普通方法简写 {foo(){return 1}} 解析正确
// ============================================================

TEST(MethodShorthand, MS01_ParserMethodShorthand) {
    auto result = parse_program("var o = {foo(){return 1}};");
    ASSERT_TRUE(result.ok()) << result.error().message();
    const auto& body = result.value().body;
    ASSERT_FALSE(body.empty());
    const auto* vd = std::get_if<VariableDeclaration>(&body[0].v);
    ASSERT_NE(vd, nullptr);
    const auto* oe = vd->init.has_value() ? std::get_if<ObjectExpression>(&vd->init->v) : nullptr;
    ASSERT_NE(oe, nullptr);
    ASSERT_EQ(oe->properties.size(), 1u);
    EXPECT_EQ(oe->properties[0].key, "foo");
    EXPECT_EQ(oe->properties[0].method_kind, MethodKind::kMethod);
}

// ============================================================
// MS-02: Parser — getter {get foo(){return 1}} 解析正确
// ============================================================

TEST(MethodShorthand, MS02_ParserGetter) {
    auto result = parse_program("var o = {get foo(){return 1}};");
    ASSERT_TRUE(result.ok()) << result.error().message();
    const auto& body = result.value().body;
    const auto* vd = std::get_if<VariableDeclaration>(&body[0].v);
    ASSERT_NE(vd, nullptr);
    const auto* oe = vd->init.has_value() ? std::get_if<ObjectExpression>(&vd->init->v) : nullptr;
    ASSERT_NE(oe, nullptr);
    ASSERT_EQ(oe->properties.size(), 1u);
    EXPECT_EQ(oe->properties[0].key, "foo");
    EXPECT_EQ(oe->properties[0].method_kind, MethodKind::kGetter);
}

// ============================================================
// MS-03: Parser — setter {set foo(v){}} 解析正确
// ============================================================

TEST(MethodShorthand, MS03_ParserSetter) {
    auto result = parse_program("var o = {set foo(v){}};");
    ASSERT_TRUE(result.ok()) << result.error().message();
    const auto& body = result.value().body;
    const auto* vd = std::get_if<VariableDeclaration>(&body[0].v);
    ASSERT_NE(vd, nullptr);
    const auto* oe = vd->init.has_value() ? std::get_if<ObjectExpression>(&vd->init->v) : nullptr;
    ASSERT_NE(oe, nullptr);
    ASSERT_EQ(oe->properties.size(), 1u);
    EXPECT_EQ(oe->properties[0].key, "foo");
    EXPECT_EQ(oe->properties[0].method_kind, MethodKind::kSetter);
}

// ============================================================
// MS-04: Parser — {get: 1} 仍是普通数据属性
// ============================================================

TEST(MethodShorthand, MS04_ParserGetAsDataProp) {
    auto result = parse_program("var o = {get: 1};");
    ASSERT_TRUE(result.ok()) << result.error().message();
    const auto& body = result.value().body;
    const auto* vd = std::get_if<VariableDeclaration>(&body[0].v);
    ASSERT_NE(vd, nullptr);
    const auto* oe = vd->init.has_value() ? std::get_if<ObjectExpression>(&vd->init->v) : nullptr;
    ASSERT_NE(oe, nullptr);
    ASSERT_EQ(oe->properties.size(), 1u);
    EXPECT_EQ(oe->properties[0].key, "get");
    EXPECT_EQ(oe->properties[0].method_kind, MethodKind::kData);
}

// ============================================================
// MS-05: Parser — {set: 1} 仍是普通数据属性
// ============================================================

TEST(MethodShorthand, MS05_ParserSetAsDataProp) {
    auto result = parse_program("var o = {set: 1};");
    ASSERT_TRUE(result.ok()) << result.error().message();
    const auto& body = result.value().body;
    const auto* vd = std::get_if<VariableDeclaration>(&body[0].v);
    ASSERT_NE(vd, nullptr);
    const auto* oe = vd->init.has_value() ? std::get_if<ObjectExpression>(&vd->init->v) : nullptr;
    ASSERT_NE(oe, nullptr);
    ASSERT_EQ(oe->properties.size(), 1u);
    EXPECT_EQ(oe->properties[0].key, "set");
    EXPECT_EQ(oe->properties[0].method_kind, MethodKind::kData);
}

// ============================================================
// MS-06: Parser — {async foo(){}} 解析为 kAsyncMethod
// ============================================================

TEST(MethodShorthand, MS06_ParserAsyncMethod) {
    auto result = parse_program("var o = {async foo(){}};");
    ASSERT_TRUE(result.ok()) << result.error().message();
    const auto& body = result.value().body;
    const auto* vd = std::get_if<VariableDeclaration>(&body[0].v);
    ASSERT_NE(vd, nullptr);
    const auto* oe = vd->init.has_value() ? std::get_if<ObjectExpression>(&vd->init->v) : nullptr;
    ASSERT_NE(oe, nullptr);
    ASSERT_EQ(oe->properties.size(), 1u);
    EXPECT_EQ(oe->properties[0].key, "foo");
    EXPECT_EQ(oe->properties[0].method_kind, MethodKind::kAsyncMethod);
}

// ============================================================
// MS-07: Parser — {async: 1} 仍是普通数据属性
// ============================================================

TEST(MethodShorthand, MS07_ParserAsyncAsDataProp) {
    auto result = parse_program("var o = {async: 1};");
    ASSERT_TRUE(result.ok()) << result.error().message();
    const auto& body = result.value().body;
    const auto* vd = std::get_if<VariableDeclaration>(&body[0].v);
    ASSERT_NE(vd, nullptr);
    const auto* oe = vd->init.has_value() ? std::get_if<ObjectExpression>(&vd->init->v) : nullptr;
    ASSERT_NE(oe, nullptr);
    ASSERT_EQ(oe->properties.size(), 1u);
    EXPECT_EQ(oe->properties[0].key, "async");
    EXPECT_EQ(oe->properties[0].method_kind, MethodKind::kData);
}

// ============================================================
// MS-08: Parser — {*foo(){}} 解析为 kGenerator
// ============================================================

TEST(MethodShorthand, MS08_ParserGeneratorMethod) {
    auto result = parse_program("var o = {*foo(){}};");
    ASSERT_TRUE(result.ok()) << result.error().message();
    const auto& body = result.value().body;
    const auto* vd = std::get_if<VariableDeclaration>(&body[0].v);
    ASSERT_NE(vd, nullptr);
    const auto* oe = vd->init.has_value() ? std::get_if<ObjectExpression>(&vd->init->v) : nullptr;
    ASSERT_NE(oe, nullptr);
    ASSERT_EQ(oe->properties.size(), 1u);
    EXPECT_EQ(oe->properties[0].key, "foo");
    EXPECT_EQ(oe->properties[0].method_kind, MethodKind::kGenerator);
}

// ============================================================
// MS-09: Interp — 普通方法简写可调用
// ============================================================

TEST(MethodShorthand, MS09_InterpMethodCallable) {
    auto v = interp_ok("var o = {foo(){return 42}}; o.foo()");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 42.0);
}

// ============================================================
// MS-10: Interp — getter 在读时被调用
// ============================================================

TEST(MethodShorthand, MS10_InterpGetter) {
    auto v = interp_ok("var o = {get x(){return 99}}; o.x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 99.0);
}

// ============================================================
// MS-11: Interp — setter 在写时被调用
// ============================================================

TEST(MethodShorthand, MS11_InterpSetter) {
    auto v = interp_ok("var val = 0; var o = {set x(v){val=v}}; o.x = 7; val");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 7.0);
}

// ============================================================
// MS-12: Interp — method 用 new 抛 TypeError
// ============================================================

TEST(MethodShorthand, MS12_InterpMethodNotConstructor) {
    EXPECT_TRUE(interp_throws("var o = {foo(){}}; new o.foo()"));
}

// ============================================================
// MS-13: Interp — async 方法返回 Promise
// ============================================================

TEST(MethodShorthand, MS13_InterpAsyncMethod) {
    auto v = interp_ok("var o = {async bar(){return 5}}; typeof o.bar()");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "object");  // Promise is an object
}

// ============================================================
// MS-14: Interp — generator 方法（降级）可调用
// ============================================================

TEST(MethodShorthand, MS14_InterpGeneratorMethod) {
    auto v = interp_ok("var o = {*g(){return 1}}; typeof o.g");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "function");
}

// ============================================================
// MS-15: VM — 普通方法简写可调用
// ============================================================

TEST(MethodShorthand, MS15_VMMethodCallable) {
    auto v = vm_ok("var o = {foo(){return 42}}; o.foo()");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 42.0);
}

// ============================================================
// MS-16: VM — getter 在读时被调用
// ============================================================

TEST(MethodShorthand, MS16_VMGetter) {
    auto v = vm_ok("var o = {get x(){return 99}}; o.x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 99.0);
}

// ============================================================
// MS-17: VM — setter 在写时被调用
// ============================================================

TEST(MethodShorthand, MS17_VMSetter) {
    auto v = vm_ok("var val = 0; var o = {set x(v){val=v}}; o.x = 7; val");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 7.0);
}

// ============================================================
// MS-18: VM — method 用 new 抛 TypeError
// ============================================================

TEST(MethodShorthand, MS18_VMMethodNotConstructor) {
    EXPECT_TRUE(vm_throws("var o = {foo(){}}; new o.foo()"));
}

// ============================================================
// MS-19: VM — async 方法返回 Promise
// ============================================================

TEST(MethodShorthand, MS19_VMAsyncMethod) {
    auto v = vm_ok("var o = {async bar(){return 5}}; typeof o.bar()");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "object");
}

// ============================================================
// MS-20: VM — generator 方法（降级）可调用
// ============================================================

TEST(MethodShorthand, MS20_VMGeneratorMethod) {
    auto v = vm_ok("var o = {*g(){return 1}}; typeof o.g");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "function");
}

// ============================================================
// MS-21: Interp — method this 绑定正确
// ============================================================

TEST(MethodShorthand, MS21_InterpMethodThis) {
    auto v = interp_ok("var o = {val: 10, get(){return this.val}}; o.get()");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 10.0);
}

// ============================================================
// MS-22: VM — method this 绑定正确
// ============================================================

TEST(MethodShorthand, MS22_VMMethodThis) {
    auto v = vm_ok("var o = {val: 10, get(){return this.val}}; o.get()");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 10.0);
}

// ============================================================
// MS-23: Interp — method .name 属性
// ============================================================

TEST(MethodShorthand, MS23_InterpMethodName) {
    auto v = interp_ok("var o = {foo(){}}; o.foo.name");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "foo");
}

// ============================================================
// MS-24: VM — method .name 属性
// ============================================================

TEST(MethodShorthand, MS24_VMMethodName) {
    auto v = vm_ok("var o = {foo(){}}; o.foo.name");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "foo");
}

// ============================================================
// MS-25: Interp — 同对象多个方法
// ============================================================

TEST(MethodShorthand, MS25_InterpMultipleMethods) {
    auto v = interp_ok("var o = {a(){return 1}, b(){return 2}}; o.a() + o.b()");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 3.0);
}

// ============================================================
// MS-26: VM — 同对象多个方法
// ============================================================

TEST(MethodShorthand, MS26_VMMultipleMethods) {
    auto v = vm_ok("var o = {a(){return 1}, b(){return 2}}; o.a() + o.b()");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 3.0);
}

// ============================================================
// MS-27: Interp — getter/setter 配对（get+set 同名）
// ============================================================

TEST(MethodShorthand, MS27_InterpGetterSetterPair) {
    auto v = interp_ok(R"(
        var _v = 0;
        var o = {
            get x() { return _v; },
            set x(val) { _v = val; }
        };
        o.x = 42;
        o.x
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 42.0);
}

// ============================================================
// MS-28: VM — getter/setter 配对
// ============================================================

TEST(MethodShorthand, MS28_VMGetterSetterPair) {
    auto v = vm_ok(R"(
        var _v = 0;
        var o = {
            get x() { return _v; },
            set x(val) { _v = val; }
        };
        o.x = 42;
        o.x
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 42.0);
}

// ============================================================
// MS-29: Interp — 保留字作为方法名 {if(){return 1}}
// ============================================================

TEST(MethodShorthand, MS29_InterpReservedWordMethodName) {
    auto v = interp_ok("var o = {if(){return 1}}; o.if()");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

// ============================================================
// MS-30: VM — 保留字作为方法名
// ============================================================

TEST(MethodShorthand, MS30_VMReservedWordMethodName) {
    auto v = vm_ok("var o = {if(){return 1}}; o.if()");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

// ============================================================
// MS-31: Interp — getter .name 属性
// ============================================================

TEST(MethodShorthand, MS31_InterpGetterName) {
    auto v = interp_ok(R"(
        var o = {get foo(){return 1}};
        Object.getOwnPropertyDescriptor(o, 'foo').get.name
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "get foo");
}

// ============================================================
// MS-32: VM — getter .name 属性
// ============================================================

TEST(MethodShorthand, MS32_VMGetterName) {
    auto v = vm_ok(R"(
        var o = {get foo(){return 1}};
        Object.getOwnPropertyDescriptor(o, 'foo').get.name
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "get foo");
}

}  // namespace
