#include "qppjs/frontend/parser.h"
#include "qppjs/runtime/interpreter.h"
#include "qppjs/runtime/js_object.h"
#include "qppjs/runtime/value.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>

namespace qppjs {

// ============================================================
// JSObject 单元测试
// ============================================================

TEST(JSObjectTest, EmptyObjectGetReturnsUndefined) {
    JSObject obj;
    Value v = obj.get_property("x");
    EXPECT_TRUE(v.is_undefined());
}

TEST(JSObjectTest, SetThenGet) {
    JSObject obj;
    obj.set_property("x", Value::number(42.0));
    Value v = obj.get_property("x");
    ASSERT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 42.0);
}

TEST(JSObjectTest, SetOverwrites) {
    JSObject obj;
    obj.set_property("x", Value::number(1.0));
    obj.set_property("x", Value::number(2.0));
    Value v = obj.get_property("x");
    ASSERT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.0);
}

TEST(JSObjectTest, HasOwnPropertyExisting) {
    JSObject obj;
    obj.set_property("y", Value::string("hello"));
    EXPECT_TRUE(obj.has_own_property("y"));
}

TEST(JSObjectTest, HasOwnPropertyMissing) {
    JSObject obj;
    EXPECT_FALSE(obj.has_own_property("z"));
}

TEST(JSObjectTest, ObjectKindIsOrdinary) {
    JSObject obj;
    EXPECT_EQ(obj.object_kind(), ObjectKind::kOrdinary);
}

TEST(JSObjectTest, MultipleProperties) {
    JSObject obj;
    obj.set_property("a", Value::number(1.0));
    obj.set_property("b", Value::string("two"));
    obj.set_property("c", Value::boolean(true));
    EXPECT_EQ(obj.get_property("a").as_number(), 1.0);
    EXPECT_EQ(obj.get_property("b").as_string(), "two");
    EXPECT_TRUE(obj.get_property("c").as_bool());
}

// ============================================================
// Parser 测试
// ============================================================

TEST(ParserObjectTest, EmptyObjectLiteral) {
    auto prog = parse_program("({})");
    ASSERT_TRUE(prog.ok()) << prog.error().message();
    const auto& stmt = prog.value().body[0];
    ASSERT_TRUE(std::holds_alternative<ExpressionStatement>(stmt.v));
    const auto& expr = std::get<ExpressionStatement>(stmt.v).expr;
    EXPECT_TRUE(std::holds_alternative<ObjectExpression>(expr.v));
    const auto& obj_expr = std::get<ObjectExpression>(expr.v);
    EXPECT_EQ(obj_expr.properties.size(), 0u);
}

TEST(ParserObjectTest, ObjectLiteralOneProperty) {
    auto prog = parse_program("({x: 1})");
    ASSERT_TRUE(prog.ok()) << prog.error().message();
    const auto& stmt = prog.value().body[0];
    const auto& expr = std::get<ExpressionStatement>(stmt.v).expr;
    ASSERT_TRUE(std::holds_alternative<ObjectExpression>(expr.v));
    const auto& obj_expr = std::get<ObjectExpression>(expr.v);
    ASSERT_EQ(obj_expr.properties.size(), 1u);
    EXPECT_EQ(obj_expr.properties[0].key, "x");
}

TEST(ParserObjectTest, MemberExpressionDotNotComputed) {
    auto prog = parse_program("obj.x");
    ASSERT_TRUE(prog.ok()) << prog.error().message();
    const auto& stmt = prog.value().body[0];
    const auto& expr = std::get<ExpressionStatement>(stmt.v).expr;
    ASSERT_TRUE(std::holds_alternative<MemberExpression>(expr.v));
    const auto& mem = std::get<MemberExpression>(expr.v);
    EXPECT_FALSE(mem.computed);
    ASSERT_TRUE(std::holds_alternative<Identifier>(mem.object->v));
    EXPECT_EQ(std::get<Identifier>(mem.object->v).name, "obj");
    ASSERT_TRUE(std::holds_alternative<StringLiteral>(mem.property->v));
    EXPECT_EQ(std::get<StringLiteral>(mem.property->v).value, "x");
}

TEST(ParserObjectTest, MemberExpressionBracketComputed) {
    auto prog = parse_program("obj[\"x\"]");
    ASSERT_TRUE(prog.ok()) << prog.error().message();
    const auto& stmt = prog.value().body[0];
    const auto& expr = std::get<ExpressionStatement>(stmt.v).expr;
    ASSERT_TRUE(std::holds_alternative<MemberExpression>(expr.v));
    const auto& mem = std::get<MemberExpression>(expr.v);
    EXPECT_TRUE(mem.computed);
}

TEST(ParserObjectTest, MemberAssignmentExpression) {
    auto prog = parse_program("obj.x = 1");
    ASSERT_TRUE(prog.ok()) << prog.error().message();
    const auto& stmt = prog.value().body[0];
    const auto& expr = std::get<ExpressionStatement>(stmt.v).expr;
    ASSERT_TRUE(std::holds_alternative<MemberAssignmentExpression>(expr.v));
    const auto& mae = std::get<MemberAssignmentExpression>(expr.v);
    EXPECT_FALSE(mae.computed);
}

TEST(ParserObjectTest, ChainedMemberExpressionLeftAssoc) {
    // a.b.c should parse as (a.b).c, i.e. MemberExpression{object=MemberExpression{...}, ...}
    auto prog = parse_program("a.b.c");
    ASSERT_TRUE(prog.ok()) << prog.error().message();
    const auto& stmt = prog.value().body[0];
    const auto& expr = std::get<ExpressionStatement>(stmt.v).expr;
    ASSERT_TRUE(std::holds_alternative<MemberExpression>(expr.v));
    const auto& outer = std::get<MemberExpression>(expr.v);
    // property should be "c"
    ASSERT_TRUE(std::holds_alternative<StringLiteral>(outer.property->v));
    EXPECT_EQ(std::get<StringLiteral>(outer.property->v).value, "c");
    // object should be a MemberExpression
    ASSERT_TRUE(std::holds_alternative<MemberExpression>(outer.object->v));
    const auto& inner = std::get<MemberExpression>(outer.object->v);
    ASSERT_TRUE(std::holds_alternative<StringLiteral>(inner.property->v));
    EXPECT_EQ(std::get<StringLiteral>(inner.property->v).value, "b");
}

// ============================================================
// Interpreter 端到端测试
// ============================================================

static EvalResult exec(const std::string& source) {
    auto prog = parse_program(source);
    if (!prog.ok()) {
        return EvalResult::err(prog.error());
    }
    Interpreter interp;
    return interp.exec(prog.value());
}

TEST(InterpreterObjectTest, TypeofEmptyObject) {
    auto result = exec("let obj = {}; typeof obj");
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    EXPECT_EQ(result.value().as_string(), "object");
}

TEST(InterpreterObjectTest, DotAccessExistingProperty) {
    auto result = exec("let obj = { x: 1 }; obj.x");
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    ASSERT_TRUE(result.value().is_number());
    EXPECT_EQ(result.value().as_number(), 1.0);
}

TEST(InterpreterObjectTest, DotAccessSecondProperty) {
    auto result = exec("let obj = { x: 1, y: 2 }; obj.y");
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    ASSERT_TRUE(result.value().is_number());
    EXPECT_EQ(result.value().as_number(), 2.0);
}

TEST(InterpreterObjectTest, DotAccessMissingProperty) {
    auto result = exec("let obj = {}; obj.x");
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    EXPECT_TRUE(result.value().is_undefined());
}

TEST(InterpreterObjectTest, BracketAccessExistingProperty) {
    auto result = exec("let obj = { x: 1 }; obj[\"x\"]");
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    ASSERT_TRUE(result.value().is_number());
    EXPECT_EQ(result.value().as_number(), 1.0);
}

TEST(InterpreterObjectTest, WriteNewProperty) {
    auto result = exec("let obj = {}; obj.x = 42; obj.x");
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    ASSERT_TRUE(result.value().is_number());
    EXPECT_EQ(result.value().as_number(), 42.0);
}

TEST(InterpreterObjectTest, OverwriteExistingProperty) {
    auto result = exec("let obj = { x: 1 }; obj.x = 2; obj.x");
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    ASSERT_TRUE(result.value().is_number());
    EXPECT_EQ(result.value().as_number(), 2.0);
}

TEST(InterpreterObjectTest, BracketWriteThenDotRead) {
    auto result = exec("let obj = {}; obj[\"k\"] = \"v\"; obj.k");
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    ASSERT_TRUE(result.value().is_string());
    EXPECT_EQ(result.value().as_string(), "v");
}

TEST(InterpreterObjectTest, DuplicateKeyLastWins) {
    auto result = exec("let obj = { a: 1, a: 2 }; obj.a");
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    ASSERT_TRUE(result.value().is_number());
    EXPECT_EQ(result.value().as_number(), 2.0);
}

TEST(InterpreterObjectTest, ComputedKeyFromVariable) {
    auto result = exec("let key = \"x\"; let obj = { x: 1 }; obj[key]");
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    ASSERT_TRUE(result.value().is_number());
    EXPECT_EQ(result.value().as_number(), 1.0);
}

TEST(InterpreterObjectTest, NullDotAccessThrowsTypeError) {
    auto result = exec("null.x");
    EXPECT_FALSE(result.is_ok());
    EXPECT_NE(result.error().message().find("TypeError"), std::string::npos);
}

TEST(InterpreterObjectTest, UndefinedDotAccessThrowsTypeError) {
    auto result = exec("undefined.x");
    EXPECT_FALSE(result.is_ok());
    EXPECT_NE(result.error().message().find("TypeError"), std::string::npos);
}

TEST(InterpreterObjectTest, NullWriteThrowsTypeError) {
    auto result = exec("null.x = 1");
    EXPECT_FALSE(result.is_ok());
    EXPECT_NE(result.error().message().find("TypeError"), std::string::npos);
}

TEST(InterpreterObjectTest, NestedObjectAccess) {
    auto result = exec("let obj = { nested: { a: 1 } }; obj.nested.a");
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    ASSERT_TRUE(result.value().is_number());
    EXPECT_EQ(result.value().as_number(), 1.0);
}

TEST(InterpreterObjectTest, PropertyArithmetic) {
    auto result = exec("let obj = {}; obj.x = 1; obj.y = 2; obj.x + obj.y");
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    ASSERT_TRUE(result.value().is_number());
    EXPECT_EQ(result.value().as_number(), 3.0);
}

// ============================================================
// ToPropertyKey: 数字/null/undefined/bool 键转字符串
// ============================================================

TEST(InterpreterObjectTest, NumberKeyEqualsStringKey) {
    // obj[0] 和 obj["0"] 访问同一属性（ToPropertyKey 将数字转为字符串 "0"）
    auto result = exec("let obj = {}; obj[\"0\"] = 99; obj[0]");
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    ASSERT_TRUE(result.value().is_number());
    EXPECT_EQ(result.value().as_number(), 99.0);
}

TEST(InterpreterObjectTest, NumberKeyWriteThenStringKeyRead) {
    // obj[0] = 1; obj["0"] 应读到同一属性
    auto result = exec("let obj = {}; obj[0] = 1; obj[\"0\"]");
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    ASSERT_TRUE(result.value().is_number());
    EXPECT_EQ(result.value().as_number(), 1.0);
}

TEST(InterpreterObjectTest, NullKeyEqualsStringNull) {
    // obj[null] 等价 obj["null"]
    auto result = exec("let obj = {}; obj[\"null\"] = 7; obj[null]");
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    ASSERT_TRUE(result.value().is_number());
    EXPECT_EQ(result.value().as_number(), 7.0);
}

TEST(InterpreterObjectTest, UndefinedKeyEqualsStringUndefined) {
    // obj[undefined] 等价 obj["undefined"]
    auto result = exec("let obj = {}; obj[\"undefined\"] = 5; obj[undefined]");
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    ASSERT_TRUE(result.value().is_number());
    EXPECT_EQ(result.value().as_number(), 5.0);
}

TEST(InterpreterObjectTest, BoolKeyTrueEqualsStringTrue) {
    // obj[true] 等价 obj["true"]
    auto result = exec("let obj = {}; obj[\"true\"] = 3; obj[true]");
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    ASSERT_TRUE(result.value().is_number());
    EXPECT_EQ(result.value().as_number(), 3.0);
}

TEST(InterpreterObjectTest, BoolKeyFalseEqualsStringFalse) {
    // obj[false] 等价 obj["false"]
    auto result = exec("let obj = {}; obj[\"false\"] = 4; obj[false]");
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    ASSERT_TRUE(result.value().is_number());
    EXPECT_EQ(result.value().as_number(), 4.0);
}

// ============================================================
// 引用语义：两个变量指向同一对象
// ============================================================

TEST(InterpreterObjectTest, SharedReferenceWriteThroughAlias) {
    // let a = {}; let b = a; b.x = 1; a.x 应返回 1
    auto result = exec("let a = {}; let b = a; b.x = 1; a.x");
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    ASSERT_TRUE(result.value().is_number());
    EXPECT_EQ(result.value().as_number(), 1.0);
}

TEST(InterpreterObjectTest, TwoDistinctObjectsAreIndependent) {
    // 两个独立字面量对象写入互不影响
    auto result = exec("let a = {}; let b = {}; a.x = 1; b.x = 2; a.x");
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    ASSERT_TRUE(result.value().is_number());
    EXPECT_EQ(result.value().as_number(), 1.0);
}

// ============================================================
// 属性值类型覆盖：undefined / null / bool / string 作为属性值
// ============================================================

TEST(InterpreterObjectTest, PropertyValueUndefined) {
    auto result = exec("let obj = {}; obj.k = undefined; obj.k");
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    EXPECT_TRUE(result.value().is_undefined());
}

TEST(InterpreterObjectTest, PropertyValueNull) {
    auto result = exec("let obj = {}; obj.k = null; obj.k");
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    EXPECT_TRUE(result.value().is_null());
}

TEST(InterpreterObjectTest, PropertyValueBoolFalse) {
    auto result = exec("let obj = {}; obj.k = false; obj.k");
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    ASSERT_TRUE(result.value().is_bool());
    EXPECT_EQ(result.value().as_bool(), false);
}

TEST(InterpreterObjectTest, PropertyValueString) {
    auto result = exec("let obj = { msg: \"hello\" }; obj.msg");
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    ASSERT_TRUE(result.value().is_string());
    EXPECT_EQ(result.value().as_string(), "hello");
}

// ============================================================
// bracket 访问缺失属性
// ============================================================

TEST(InterpreterObjectTest, BracketAccessMissingPropertyReturnsUndefined) {
    auto result = exec("let obj = {}; obj[\"missing\"]");
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    EXPECT_TRUE(result.value().is_undefined());
}

// ============================================================
// null / undefined bracket 读写：TypeError
// ============================================================

TEST(InterpreterObjectTest, NullBracketReadThrowsTypeError) {
    auto result = exec("null[\"x\"]");
    EXPECT_FALSE(result.is_ok());
    EXPECT_NE(result.error().message().find("TypeError"), std::string::npos);
}

TEST(InterpreterObjectTest, UndefinedBracketReadThrowsTypeError) {
    auto result = exec("undefined[\"x\"]");
    EXPECT_FALSE(result.is_ok());
    EXPECT_NE(result.error().message().find("TypeError"), std::string::npos);
}

TEST(InterpreterObjectTest, UndefinedDotWriteThrowsTypeError) {
    auto result = exec("undefined.x = 1");
    EXPECT_FALSE(result.is_ok());
    EXPECT_NE(result.error().message().find("TypeError"), std::string::npos);
}

TEST(InterpreterObjectTest, NullBracketWriteThrowsTypeError) {
    auto result = exec("null[\"x\"] = 1");
    EXPECT_FALSE(result.is_ok());
    EXPECT_NE(result.error().message().find("TypeError"), std::string::npos);
}

// ============================================================
// 嵌套对象写入然后读取
// ============================================================

TEST(InterpreterObjectTest, NestedObjectWriteThenRead) {
    // let obj = { a: {} }; obj.a.b = 3; obj.a.b 应返回 3
    auto result = exec("let obj = { a: {} }; obj.a.b = 3; obj.a.b");
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    ASSERT_TRUE(result.value().is_number());
    EXPECT_EQ(result.value().as_number(), 3.0);
}

TEST(InterpreterObjectTest, DeepNestedAccessThreeLevels) {
    // 三层嵌套读取
    auto result = exec("let obj = { a: { b: { c: 42 } } }; obj.a.b.c");
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    ASSERT_TRUE(result.value().is_number());
    EXPECT_EQ(result.value().as_number(), 42.0);
}

// ============================================================
// typeof 覆盖：null 和非空对象
// ============================================================

TEST(InterpreterObjectTest, TypeofNullIsObject) {
    // typeof null === "object"（ECMAScript 历史遗留规定）
    auto result = exec("typeof null");
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    ASSERT_TRUE(result.value().is_string());
    EXPECT_EQ(result.value().as_string(), "object");
}

TEST(InterpreterObjectTest, TypeofNonEmptyObjectIsObject) {
    auto result = exec("let obj = { x: 1 }; typeof obj");
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    ASSERT_TRUE(result.value().is_string());
    EXPECT_EQ(result.value().as_string(), "object");
}

// ============================================================
// 对象引用严格相等
// ============================================================

TEST(InterpreterObjectTest, SameObjectReferenceStrictEqual) {
    // 同一对象引用严格相等
    auto result = exec("let a = {}; a === a");
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    ASSERT_TRUE(result.value().is_bool());
    EXPECT_EQ(result.value().as_bool(), true);
}

TEST(InterpreterObjectTest, TwoDifferentObjectLiteralsNotStrictEqual) {
    // 两个不同对象字面量不严格相等
    auto result = exec("let a = {}; let b = {}; a === b");
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    ASSERT_TRUE(result.value().is_bool());
    EXPECT_EQ(result.value().as_bool(), false);
}

// ============================================================
// JSObject 单元：属性插入顺序保留
// ============================================================

TEST(JSObjectTest, InsertionOrderPreserved) {
    // 插入顺序不影响按键读取的正确性，且多次插入后 has_own_property 仍正确
    JSObject obj;
    obj.set_property("z", Value::number(1.0));
    obj.set_property("a", Value::number(2.0));
    obj.set_property("m", Value::number(3.0));
    EXPECT_EQ(obj.get_property("z").as_number(), 1.0);
    EXPECT_EQ(obj.get_property("a").as_number(), 2.0);
    EXPECT_EQ(obj.get_property("m").as_number(), 3.0);
    EXPECT_TRUE(obj.has_own_property("z"));
    EXPECT_TRUE(obj.has_own_property("a"));
    EXPECT_TRUE(obj.has_own_property("m"));
    EXPECT_FALSE(obj.has_own_property("b"));
}

TEST(JSObjectTest, SetPropertyToUndefinedStillExists) {
    // 将属性设为 undefined 后，has_own_property 仍返回 true
    JSObject obj;
    obj.set_property("x", Value::undefined());
    EXPECT_TRUE(obj.has_own_property("x"));
    EXPECT_TRUE(obj.get_property("x").is_undefined());
}

TEST(JSObjectTest, SetPropertyToNull) {
    JSObject obj;
    obj.set_property("n", Value::null());
    EXPECT_TRUE(obj.has_own_property("n"));
    EXPECT_TRUE(obj.get_property("n").is_null());
}

TEST(JSObjectTest, OverwriteWithDifferentType) {
    // 将 number 属性覆盖为 string
    JSObject obj;
    obj.set_property("k", Value::number(1.0));
    obj.set_property("k", Value::string("new"));
    ASSERT_TRUE(obj.get_property("k").is_string());
    EXPECT_EQ(obj.get_property("k").as_string(), "new");
}

// ============================================================
// 回归：dot-write 后 bracket-read 互通
// ============================================================

TEST(InterpreterObjectTest, DotWriteThenBracketRead) {
    // obj.k = "v"; obj["k"] 应读到同一属性
    auto result = exec("let obj = {}; obj.k = \"v\"; obj[\"k\"]");
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    ASSERT_TRUE(result.value().is_string());
    EXPECT_EQ(result.value().as_string(), "v");
}

// ============================================================
// Bug 1 回归：对象字面量数字键经 ToPropertyKey 规范化
// ============================================================

TEST(InterpreterObjectTest, HexLiteralKeyNormalizedToDecimal) {
    // { 0x1: "v" } 的键应规范化为 "1"，通过 obj[1] 可访问到 "v"
    auto result = exec("let obj = { 0x1: \"v\" }; obj[1]");
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    ASSERT_TRUE(result.value().is_string());
    EXPECT_EQ(result.value().as_string(), "v");
}

TEST(InterpreterObjectTest, DecimalLiteralKeyAccessByNumberAndString) {
    // { 1: "one" } 通过 obj[1] 和 obj["1"] 均可访问
    auto result_num = exec("let obj = { 1: \"one\" }; obj[1]");
    ASSERT_TRUE(result_num.is_ok()) << result_num.error().message();
    ASSERT_TRUE(result_num.value().is_string());
    EXPECT_EQ(result_num.value().as_string(), "one");

    auto result_str = exec("let obj = { 1: \"one\" }; obj[\"1\"]");
    ASSERT_TRUE(result_str.is_ok()) << result_str.error().message();
    ASSERT_TRUE(result_str.value().is_string());
    EXPECT_EQ(result_str.value().as_string(), "one");
}

}  // namespace qppjs
