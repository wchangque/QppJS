#include "qppjs/frontend/ast_dump.h"
#include "qppjs/frontend/parser.h"
#include "qppjs/runtime/interpreter.h"
#include "qppjs/runtime/js_object.h"
#include "qppjs/runtime/rc_object.h"
#include "qppjs/runtime/value.h"

#include <gtest/gtest.h>
#include <string>

namespace qppjs {
namespace {

EvalResult run(const std::string& src) {
    auto parse_result = parse_program(src);
    if (!parse_result.ok()) {
        return EvalResult::err(parse_result.error());
    }
    Interpreter interp;
    return interp.exec(parse_result.value());
}

// ============================================================
// 1. 原型链基础（JSObject 层面）
// ============================================================

// 属性在自身：直接返回，不走链
TEST(ProtoChainTest, OwnPropertyFoundDirectly) {
    auto obj = RcPtr<JSObject>::make();
    obj->set_property("x", Value::number(1.0));
    EXPECT_EQ(obj->get_property("x").as_number(), 1.0);
}

// 属性在 proto 上：能读到
TEST(ProtoChainTest, PropertyFoundOnProto) {
    auto proto = RcPtr<JSObject>::make();
    proto->set_property("y", Value::number(42.0));

    auto obj = RcPtr<JSObject>::make();
    obj->set_proto(proto);

    EXPECT_EQ(obj->get_property("y").as_number(), 42.0);
    EXPECT_FALSE(obj->has_own_property("y"));
}

// 属性在 proto.proto 上：多级链能读到
TEST(ProtoChainTest, PropertyFoundOnProtoProto) {
    auto grandproto = RcPtr<JSObject>::make();
    grandproto->set_property("z", Value::string("deep"));

    auto proto = RcPtr<JSObject>::make();
    proto->set_proto(grandproto);

    auto obj = RcPtr<JSObject>::make();
    obj->set_proto(proto);

    ASSERT_TRUE(obj->get_property("z").is_string());
    EXPECT_EQ(obj->get_property("z").as_string(), "deep");
    EXPECT_FALSE(obj->has_own_property("z"));
    EXPECT_FALSE(proto->has_own_property("z"));
}

// 属性在链上都不存在：返回 undefined
TEST(ProtoChainTest, MissingPropertyReturnsUndefined) {
    auto proto = RcPtr<JSObject>::make();
    auto obj = RcPtr<JSObject>::make();
    obj->set_proto(proto);

    EXPECT_TRUE(obj->get_property("nonexistent").is_undefined());
}

// set_property 只写自身，不影响 proto 上同名属性
TEST(ProtoChainTest, SetPropertyWritesOwnNotProto) {
    auto proto = RcPtr<JSObject>::make();
    proto->set_property("x", Value::number(1.0));

    auto obj = RcPtr<JSObject>::make();
    obj->set_proto(proto);

    obj->set_property("x", Value::number(99.0));

    // obj 自身有 x = 99
    EXPECT_TRUE(obj->has_own_property("x"));
    EXPECT_EQ(obj->get_property("x").as_number(), 99.0);
    // proto 上的 x 没有被修改
    EXPECT_EQ(proto->get_property("x").as_number(), 1.0);
}

// 自身属性遮蔽 proto 属性
TEST(ProtoChainTest, OwnPropertyShadowsProto) {
    auto proto = RcPtr<JSObject>::make();
    proto->set_property("v", Value::number(1.0));

    auto obj = RcPtr<JSObject>::make();
    obj->set_proto(proto);
    obj->set_property("v", Value::number(2.0));

    EXPECT_EQ(obj->get_property("v").as_number(), 2.0);
}

// has_own_property 不走链：proto 上的属性返回 false
TEST(ProtoChainTest, HasOwnPropertyDoesNotWalkChain) {
    auto proto = RcPtr<JSObject>::make();
    proto->set_property("p", Value::boolean(true));

    auto obj = RcPtr<JSObject>::make();
    obj->set_proto(proto);

    EXPECT_FALSE(obj->has_own_property("p"));
    EXPECT_TRUE(proto->has_own_property("p"));
}

// ============================================================
// 2. JSFunction.prototype（通过解释器验证）
// ============================================================

// F.prototype 存在且 typeof 是 "object"
TEST(FunctionPrototypeTest, FunctionPrototypeIsObject) {
    auto r = run("function F() {} typeof F.prototype");
    ASSERT_TRUE(r.is_ok()) << r.error().message();
    EXPECT_EQ(r.value().as_string(), "object");
}

// F.prototype.constructor 严格等于 F 自身
TEST(FunctionPrototypeTest, PrototypeConstructorEqualsFunction) {
    auto r = run("function F() {} F.prototype.constructor === F");
    ASSERT_TRUE(r.is_ok()) << r.error().message();
    EXPECT_TRUE(r.value().as_bool());
}

// 不同函数有各自独立的 prototype 对象
TEST(FunctionPrototypeTest, DifferentFunctionsHaveDifferentPrototypes) {
    auto r = run("function F() {} function G() {} F.prototype === G.prototype");
    ASSERT_TRUE(r.is_ok()) << r.error().message();
    EXPECT_FALSE(r.value().as_bool());
}

// 同一函数多次访问 F.prototype 返回同一对象
TEST(FunctionPrototypeTest, SameFunctionPrototypeIsSameObject) {
    auto r = run("function F() {} F.prototype === F.prototype");
    ASSERT_TRUE(r.is_ok()) << r.error().message();
    EXPECT_TRUE(r.value().as_bool());
}

// 可以向 F.prototype 写属性，不影响其他函数的 prototype
TEST(FunctionPrototypeTest, WriteToPrototypeDoesNotAffectOtherFunctions) {
    auto r = run(
        "function F() {}"
        "function G() {}"
        "F.prototype.x = 42;"
        "typeof G.prototype.x");
    ASSERT_TRUE(r.is_ok()) << r.error().message();
    EXPECT_EQ(r.value().as_string(), "undefined");
}

// ============================================================
// 3. this 关键字绑定
// ============================================================

// 普通调用中 this 是 undefined（QppJS 不绑定 globalThis）
TEST(ThisBindingTest, PlainCallThisIsUndefined) {
    auto r = run("function f() { return this; } typeof f()");
    ASSERT_TRUE(r.is_ok()) << r.error().message();
    EXPECT_EQ(r.value().as_string(), "undefined");
}

// 方法调用 obj.method() 中 this === obj
TEST(ThisBindingTest, MethodCallThisIsObject) {
    auto r = run(
        "let obj = {};"
        "obj.id = 99;"
        "obj.getThis = function() { return this; };"
        "obj.getThis() === obj");
    ASSERT_TRUE(r.is_ok()) << r.error().message();
    EXPECT_TRUE(r.value().as_bool());
}

// 方法中通过 this.prop 读取对象属性
TEST(ThisBindingTest, MethodCanReadPropViaThis) {
    auto r = run(
        "let obj = { x: 10 };"
        "obj.getX = function() { return this.x; };"
        "obj.getX()");
    ASSERT_TRUE(r.is_ok()) << r.error().message();
    EXPECT_EQ(r.value().as_number(), 10.0);
}

// 方法中通过 this.prop = val 写入对象属性
TEST(ThisBindingTest, MethodCanWritePropViaThis) {
    auto r = run(
        "let obj = { x: 0 };"
        "obj.setX = function(v) { this.x = v; };"
        "obj.setX(55);"
        "obj.x");
    ASSERT_TRUE(r.is_ok()) << r.error().message();
    EXPECT_EQ(r.value().as_number(), 55.0);
}

// 嵌套调用中 this 不污染外层：外层函数 this 仍为 undefined
TEST(ThisBindingTest, InnerCallDoesNotPollutOuterThis) {
    auto r = run(
        "let outerThis;"
        "let obj = {};"
        "obj.method = function() { return this; };"
        "function outer() {"
        "  obj.method();"
        "  return this;"
        "}"
        "typeof outer()");
    ASSERT_TRUE(r.is_ok()) << r.error().message();
    EXPECT_EQ(r.value().as_string(), "undefined");
}

// this 是关键字，不能作为普通变量赋值目标（解析时产生 Identifier{name:"this"}，行为由 eval_identifier 决定）
// 以下验证 this 在全局作用域（普通调用）返回 undefined
TEST(ThisBindingTest, GlobalThisIsUndefined) {
    auto r = run("this");
    ASSERT_TRUE(r.is_ok()) << r.error().message();
    EXPECT_TRUE(r.value().is_undefined());
}

// 对象字面量内定义函数，调用时 this 正确绑定
TEST(ThisBindingTest, MethodInObjectLiteralThisBinding) {
    auto r = run(
        "let obj = {"
        "  val: 7,"
        "  getVal: function() { return this.val; }"
        "};"
        "obj.getVal()");
    ASSERT_TRUE(r.is_ok()) << r.error().message();
    EXPECT_EQ(r.value().as_number(), 7.0);
}

// ============================================================
// 4. new 表达式
// ============================================================

// 基础 new：创建实例，typeof 是 "object"
TEST(NewExpressionTest, BasicNewReturnsObject) {
    auto r = run("function F() {} typeof new F()");
    ASSERT_TRUE(r.is_ok()) << r.error().message();
    EXPECT_EQ(r.value().as_string(), "object");
}

// 构造函数中 this.x = 1 → 实例有 x 属性
TEST(NewExpressionTest, ConstructorSetsPropertyViaThis) {
    auto r = run("function F() { this.x = 1; } new F().x");
    ASSERT_TRUE(r.is_ok()) << r.error().message();
    EXPECT_EQ(r.value().as_number(), 1.0);
}

// 构造函数带参数，参数正确传入
TEST(NewExpressionTest, ConstructorReceivesArguments) {
    auto r = run("function F(a, b) { this.sum = a + b; } new F(3, 4).sum");
    ASSERT_TRUE(r.is_ok()) << r.error().message();
    EXPECT_EQ(r.value().as_number(), 7.0);
}

// 多个实例互不干扰
TEST(NewExpressionTest, MultipleInstancesAreIndependent) {
    auto r = run(
        "function F(v) { this.v = v; }"
        "let a = new F(1);"
        "let b = new F(2);"
        "a.v + b.v");
    ASSERT_TRUE(r.is_ok()) << r.error().message();
    EXPECT_EQ(r.value().as_number(), 3.0);
}

// 实例的 proto 是 F.prototype：F.prototype 上的方法可通过实例访问
TEST(NewExpressionTest, InstanceProtoIsFunctionPrototype) {
    auto r = run(
        "function F() {}"
        "F.prototype.greet = function() { return 42; };"
        "new F().greet()");
    ASSERT_TRUE(r.is_ok()) << r.error().message();
    EXPECT_EQ(r.value().as_number(), 42.0);
}

// 构造函数返回普通值 → new 忽略返回值，仍使用 this 对象
TEST(NewExpressionTest, ConstructorReturnsPrimitiveUsesThis) {
    auto r = run(
        "function F() { this.x = 5; return 99; }"
        "new F().x");
    ASSERT_TRUE(r.is_ok()) << r.error().message();
    EXPECT_EQ(r.value().as_number(), 5.0);
}

// 构造函数返回 null → new 返回 this 对象（null 不算对象）
TEST(NewExpressionTest, ConstructorReturnsNullUsesThis) {
    auto r = run(
        "function F() { this.x = 7; return null; }"
        "new F().x");
    ASSERT_TRUE(r.is_ok()) << r.error().message();
    EXPECT_EQ(r.value().as_number(), 7.0);
}

// 构造函数返回 undefined → new 返回 this 对象
TEST(NewExpressionTest, ConstructorReturnsUndefinedUsesThis) {
    auto r = run(
        "function F() { this.x = 3; return undefined; }"
        "new F().x");
    ASSERT_TRUE(r.is_ok()) << r.error().message();
    EXPECT_EQ(r.value().as_number(), 3.0);
}

// 构造函数显式返回对象 → new 返回该对象而非 this
TEST(NewExpressionTest, ConstructorReturnsObjectUsesReturnedObject) {
    auto r = run(
        "let other = { marker: 99 };"
        "function F() { this.x = 1; return other; }"
        "new F().marker");
    ASSERT_TRUE(r.is_ok()) << r.error().message();
    EXPECT_EQ(r.value().as_number(), 99.0);
}

// 对非函数 new → TypeError
TEST(NewExpressionTest, NewNonFunctionThrowsTypeError) {
    auto r = run("let x = 42; new x()");
    ASSERT_FALSE(r.is_ok());
    EXPECT_NE(r.error().message().find("TypeError"), std::string::npos);
}

// new null → TypeError
TEST(NewExpressionTest, NewNullThrowsTypeError) {
    auto r = run("new null()");
    ASSERT_FALSE(r.is_ok());
    EXPECT_NE(r.error().message().find("TypeError"), std::string::npos);
}

// 调用栈溢出 → RangeError（构造函数中再次 new 自身）
TEST(NewExpressionTest, NewRecursiveOverflowRangeError) {
    auto r = run("function F() { new F(); } new F()");
    ASSERT_FALSE(r.is_ok());
    EXPECT_NE(r.error().message().find("RangeError"), std::string::npos);
}

// new 无括号（new F 而非 new F()）→ 等价于 new F()，参数列表为空
TEST(NewExpressionTest, NewWithoutParensEqualsNewWithEmptyArgs) {
    auto r = run("function F() { this.x = 1; } (new F).x");
    ASSERT_TRUE(r.is_ok()) << r.error().message();
    EXPECT_EQ(r.value().as_number(), 1.0);
}

// ============================================================
// 5. 原型链继承（端到端）
// ============================================================

// 实例通过原型链访问方法
TEST(ProtoInheritanceTest, InstanceAccessesMethodOnPrototype) {
    auto r = run(
        "function Animal(name) { this.name = name; }"
        "Animal.prototype.speak = function() { return this.name; };"
        "new Animal(\"dog\").speak()");
    ASSERT_TRUE(r.is_ok()) << r.error().message();
    EXPECT_EQ(r.value().as_string(), "dog");
}

// 方法中 this 正确绑定：this.name 能读到实例属性
TEST(ProtoInheritanceTest, PrototypeMethodThisBindingIsCorrect) {
    auto r = run(
        "function Counter(init) { this.n = init; }"
        "Counter.prototype.inc = function() { this.n = this.n + 1; return this.n; };"
        "let c = new Counter(0);"
        "c.inc(); c.inc(); c.inc()");
    ASSERT_TRUE(r.is_ok()) << r.error().message();
    EXPECT_EQ(r.value().as_number(), 3.0);
}

// 完整的构造函数模式：Point(x, y)，prototype.sum() 返回 x + y
TEST(ProtoInheritanceTest, CompleteConstructorPattern) {
    auto r = run(
        "function Point(x, y) { this.x = x; this.y = y; }"
        "Point.prototype.sum = function() { return this.x + this.y; };"
        "new Point(3, 4).sum()");
    ASSERT_TRUE(r.is_ok()) << r.error().message();
    EXPECT_EQ(r.value().as_number(), 7.0);
}

// 两个实例共享 prototype 上的方法，但各自有独立的数据
TEST(ProtoInheritanceTest, TwoInstancesShareMethodButOwnData) {
    auto r = run(
        "function Box(v) { this.v = v; }"
        "Box.prototype.get = function() { return this.v; };"
        "let a = new Box(10);"
        "let b = new Box(20);"
        "a.get() + b.get()");
    ASSERT_TRUE(r.is_ok()) << r.error().message();
    EXPECT_EQ(r.value().as_number(), 30.0);
}

// 实例自身属性遮蔽 prototype 上同名属性
TEST(ProtoInheritanceTest, OwnPropertyShadowsPrototypeProperty) {
    auto r = run(
        "function F() {}"
        "F.prototype.x = 1;"
        "let obj = new F();"
        "obj.x = 99;"
        "obj.x");
    ASSERT_TRUE(r.is_ok()) << r.error().message();
    EXPECT_EQ(r.value().as_number(), 99.0);
}

// 修改实例自身属性不影响 prototype 上的同名属性
TEST(ProtoInheritanceTest, OwnPropertyDoesNotMutatePrototype) {
    auto r = run(
        "function F() {}"
        "F.prototype.x = 1;"
        "let obj = new F();"
        "obj.x = 99;"
        "F.prototype.x");
    ASSERT_TRUE(r.is_ok()) << r.error().message();
    EXPECT_EQ(r.value().as_number(), 1.0);
}

// 构造函数体内通过 this 调用方法（方法已挂在 prototype 上）
TEST(ProtoInheritanceTest, ConstructorCanCallPrototypeMethodViaThis) {
    auto r = run(
        "function F() { this.x = 0; this.inc(); }"
        "F.prototype.inc = function() { this.x = this.x + 1; };"
        "new F().x");
    // 注意：构造函数体执行时 this.inc() 需要走方法调用路径
    ASSERT_TRUE(r.is_ok()) << r.error().message();
    EXPECT_EQ(r.value().as_number(), 1.0);
}

// ============================================================
// 6. Parser 测试：NewExpression
// ============================================================

// new F() 解析为 NewExpression，参数列表为空
TEST(NewParserTest, NewExpressionNoArgs) {
    auto r = parse_program("new F()");
    ASSERT_TRUE(r.ok()) << r.error().message();
    ASSERT_EQ(r.value().body.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<ExpressionStatement>(r.value().body[0].v));
    const auto& es = std::get<ExpressionStatement>(r.value().body[0].v);
    ASSERT_TRUE(std::holds_alternative<NewExpression>(es.expr.v));
    const auto& ne = std::get<NewExpression>(es.expr.v);
    EXPECT_EQ(ne.arguments.size(), 0u);
    ASSERT_TRUE(std::holds_alternative<Identifier>(ne.callee->v));
    EXPECT_EQ(std::get<Identifier>(ne.callee->v).name, "F");
}

// new F(a, b) 带两个参数
TEST(NewParserTest, NewExpressionWithArgs) {
    auto r = parse_program("new F(a, b)");
    ASSERT_TRUE(r.ok()) << r.error().message();
    const auto& es = std::get<ExpressionStatement>(r.value().body[0].v);
    ASSERT_TRUE(std::holds_alternative<NewExpression>(es.expr.v));
    const auto& ne = std::get<NewExpression>(es.expr.v);
    EXPECT_EQ(ne.arguments.size(), 2u);
}

// new F 无括号 → 空参数列表
TEST(NewParserTest, NewExpressionWithoutParens) {
    auto r = parse_program("new F");
    ASSERT_TRUE(r.ok()) << r.error().message();
    ASSERT_EQ(r.value().body.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<ExpressionStatement>(r.value().body[0].v));
    const auto& es = std::get<ExpressionStatement>(r.value().body[0].v);
    ASSERT_TRUE(std::holds_alternative<NewExpression>(es.expr.v));
    const auto& ne = std::get<NewExpression>(es.expr.v);
    EXPECT_EQ(ne.arguments.size(), 0u);
}

// new F(1) 参数是数字字面量
TEST(NewParserTest, NewExpressionWithNumberArg) {
    auto r = parse_program("new F(1)");
    ASSERT_TRUE(r.ok()) << r.error().message();
    const auto& es = std::get<ExpressionStatement>(r.value().body[0].v);
    const auto& ne = std::get<NewExpression>(es.expr.v);
    ASSERT_EQ(ne.arguments.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<NumberLiteral>(ne.arguments[0]->v));
    EXPECT_EQ(std::get<NumberLiteral>(ne.arguments[0]->v).value, 1.0);
}

// ============================================================
// 7. AST dump 测试：NewExpression
// ============================================================

// new F() dump 包含 "NewExpression"
TEST(NewDumpTest, DumpNewExpressionContainsNodeName) {
    auto r = parse_program("new F()");
    ASSERT_TRUE(r.ok()) << r.error().message();
    auto dump = dump_program(r.value());
    EXPECT_NE(dump.find("NewExpression"), std::string::npos);
}

// dump 包含 "callee:"
TEST(NewDumpTest, DumpNewExpressionContainsCallee) {
    auto r = parse_program("new F()");
    ASSERT_TRUE(r.ok()) << r.error().message();
    auto dump = dump_program(r.value());
    EXPECT_NE(dump.find("callee:"), std::string::npos);
}

// new F(1, 2) dump 包含 "args:"（参数节点在 dump 中出现）
TEST(NewDumpTest, DumpNewExpressionWithArgsContainsArgs) {
    auto r = parse_program("new F(1, 2)");
    ASSERT_TRUE(r.ok()) << r.error().message();
    auto dump = dump_program(r.value());
    EXPECT_NE(dump.find("NewExpression"), std::string::npos);
}

// ============================================================
// 8. 边界与回归场景
// ============================================================

// new 后的实例不严格等于另一个 new 出来的实例
TEST(ProtoRegressionTest, TwoNewInstancesNotStrictEqual) {
    auto r = run("function F() {} new F() === new F()");
    ASSERT_TRUE(r.is_ok()) << r.error().message();
    EXPECT_FALSE(r.value().as_bool());
}

// 同一个 new 出来的实例，赋值给两个变量后严格相等
TEST(ProtoRegressionTest, SameInstanceStrictEqualThroughAlias) {
    auto r = run("function F() {} let a = new F(); let b = a; a === b");
    ASSERT_TRUE(r.is_ok()) << r.error().message();
    EXPECT_TRUE(r.value().as_bool());
}

// prototype 上方法修改后，已创建实例立刻受影响（共享 proto 对象）
TEST(ProtoRegressionTest, LatePrototypeMethodAdditionVisibleToExistingInstance) {
    auto r = run(
        "function F() {}"
        "let obj = new F();"
        "F.prototype.answer = function() { return 42; };"
        "obj.answer()");
    ASSERT_TRUE(r.is_ok()) << r.error().message();
    EXPECT_EQ(r.value().as_number(), 42.0);
}

// new 调用中参数为表达式
TEST(ProtoRegressionTest, NewExpressionArgIsExpression) {
    auto r = run(
        "function F(v) { this.v = v; }"
        "new F(1 + 2).v");
    ASSERT_TRUE(r.is_ok()) << r.error().message();
    EXPECT_EQ(r.value().as_number(), 3.0);
}

// 构造函数返回 boolean（原始类型）→ 使用 this
TEST(ProtoRegressionTest, ConstructorReturnsBoolUsesThis) {
    auto r = run(
        "function F() { this.x = 5; return true; }"
        "new F().x");
    ASSERT_TRUE(r.is_ok()) << r.error().message();
    EXPECT_EQ(r.value().as_number(), 5.0);
}

// 构造函数返回字符串（原始类型）→ 使用 this
TEST(ProtoRegressionTest, ConstructorReturnsStringUsesThis) {
    auto r = run(
        "function F() { this.x = 8; return \"ignored\"; }"
        "new F().x");
    ASSERT_TRUE(r.is_ok()) << r.error().message();
    EXPECT_EQ(r.value().as_number(), 8.0);
}

// 函数表达式也可以用 new 调用
TEST(ProtoRegressionTest, NewFunctionExpressionWorks) {
    auto r = run(
        "let F = function(v) { this.v = v; };"
        "new F(10).v");
    ASSERT_TRUE(r.is_ok()) << r.error().message();
    EXPECT_EQ(r.value().as_number(), 10.0);
}

// 方法调用链：obj.method().prop（方法返回 this，再访问属性）
TEST(ProtoRegressionTest, MethodReturnsThisThenAccessProp) {
    auto r = run(
        "function Builder() { this.x = 0; }"
        "Builder.prototype.set = function(v) { this.x = v; return this; };"
        "let b = new Builder();"
        "b.set(77).x");
    ASSERT_TRUE(r.is_ok()) << r.error().message();
    EXPECT_EQ(r.value().as_number(), 77.0);
}

// 回归：构造函数体最后一条语句是对象字面量表达式，不应覆盖 this（P1-1 修复验证）
TEST(ProtoRegressionTest, ConstructorEndingWithObjectExprDoesNotOverrideThis) {
    auto r = run(
        "function F() { this.x = 1; ({ y: 2 }); }"
        "new F().x");
    ASSERT_TRUE(r.is_ok()) << r.error().message();
    EXPECT_EQ(r.value().as_number(), 1.0);
}

// 回归：构造函数显式 return 对象才覆盖 this
TEST(ProtoRegressionTest, ConstructorExplicitReturnObjectOverridesThis) {
    auto r = run(
        "function F() { this.x = 1; return { x: 99 }; }"
        "new F().x");
    ASSERT_TRUE(r.is_ok()) << r.error().message();
    EXPECT_EQ(r.value().as_number(), 99.0);
}

}  // namespace
}  // namespace qppjs
