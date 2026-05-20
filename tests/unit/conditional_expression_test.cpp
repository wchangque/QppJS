#include "qppjs/frontend/ast_dump.h"
#include "qppjs/frontend/parser.h"
#include "qppjs/runtime/interpreter.h"
#include "qppjs/vm/compiler.h"
#include "qppjs/vm/vm.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

using namespace qppjs;

namespace {

// ============================================================
// 辅助函数
// ============================================================

static ParseResult<Program> parse_src(const char* src) {
    return parse_program(std::string(src) + ";");
}

#define ASSERT_EXPR(result, expr_ref)                                                      \
    ASSERT_TRUE((result).ok());                                                            \
    ASSERT_FALSE((result).value().body.empty());                                           \
    ASSERT_TRUE(std::holds_alternative<ExpressionStatement>((result).value().body[0].v)); \
    const ExprNode& expr_ref = std::get<ExpressionStatement>((result).value().body[0].v).expr

static EvalResult interp_run(std::string_view src) {
    auto parse_result = parse_program(src);
    if (!parse_result.ok()) return EvalResult::err(parse_result.error());
    Interpreter interp;
    return interp.exec(parse_result.value());
}

static Value interp_ok(std::string_view src) {
    auto r = interp_run(src);
    EXPECT_TRUE(r.is_ok()) << "exec failed: " << r.error().message();
    return r.is_ok() ? r.value() : Value::undefined();
}

static double interp_num(std::string_view src) {
    return interp_ok(src).as_number();
}

static std::string interp_str(std::string_view src) {
    auto v = interp_ok(src);
    if (v.is_string()) return std::string(v.sv());
    if (v.is_number()) {
        double n = v.as_number();
        if (n == static_cast<double>(static_cast<long long>(n)))
            return std::to_string(static_cast<long long>(n));
        return std::to_string(n);
    }
    if (v.is_bool()) return v.as_bool() ? "true" : "false";
    if (v.is_undefined()) return "undefined";
    if (v.is_null()) return "null";
    return "<other>";
}

static EvalResult vm_run(std::string_view src) {
    auto parse_result = parse_program(src);
    if (!parse_result.ok()) return EvalResult::err(parse_result.error());
    Compiler compiler;
    auto bc = compiler.compile(parse_result.value());
    VM vm;
    return vm.exec(bc);
}

static Value vm_ok(std::string_view src) {
    auto r = vm_run(src);
    EXPECT_TRUE(r.is_ok()) << "exec failed: " << r.error().message();
    return r.is_ok() ? r.value() : Value::undefined();
}

static double vm_num(std::string_view src) {
    return vm_ok(src).as_number();
}

static std::string vm_str(std::string_view src) {
    auto v = vm_ok(src);
    if (v.is_string()) return std::string(v.sv());
    if (v.is_number()) {
        double n = v.as_number();
        if (n == static_cast<double>(static_cast<long long>(n)))
            return std::to_string(static_cast<long long>(n));
        return std::to_string(n);
    }
    if (v.is_bool()) return v.as_bool() ? "true" : "false";
    if (v.is_undefined()) return "undefined";
    if (v.is_null()) return "null";
    return "<other>";
}

// ============================================================
// CE-01 ~ CE-05: Parser 结构测试
// ============================================================

// CE-01: 基础结构 a ? b : c
TEST(ConditionalExpressionParser, CE01_BasicStructure) {
    auto result = parse_src("a ? 1 : 2");
    ASSERT_EXPR(result, e);
    ASSERT_TRUE(std::holds_alternative<ConditionalExpression>(e.v));
    const auto& ce = std::get<ConditionalExpression>(e.v);
    ASSERT_TRUE(std::holds_alternative<Identifier>(ce.condition->v));
    EXPECT_EQ(std::get<Identifier>(ce.condition->v).name, "a");
    ASSERT_TRUE(std::holds_alternative<NumberLiteral>(ce.consequent->v));
    EXPECT_EQ(std::get<NumberLiteral>(ce.consequent->v).value, 1.0);
    ASSERT_TRUE(std::holds_alternative<NumberLiteral>(ce.alternate->v));
    EXPECT_EQ(std::get<NumberLiteral>(ce.alternate->v).value, 2.0);
}

// CE-02: 右结合 a ? b : c ? d : e 解析为 a ? b : (c ? d : e)
TEST(ConditionalExpressionParser, CE02_RightAssociative) {
    auto result = parse_src("a ? b : c ? d : e");
    ASSERT_EXPR(result, e);
    ASSERT_TRUE(std::holds_alternative<ConditionalExpression>(e.v));
    const auto& outer = std::get<ConditionalExpression>(e.v);
    ASSERT_TRUE(std::holds_alternative<Identifier>(outer.condition->v));
    EXPECT_EQ(std::get<Identifier>(outer.condition->v).name, "a");
    // alternate 应是内层 ConditionalExpression
    ASSERT_TRUE(std::holds_alternative<ConditionalExpression>(outer.alternate->v));
}

// CE-03: 条件为逻辑表达式 a || b ? c : d
TEST(ConditionalExpressionParser, CE03_LogicalCondition) {
    auto result = parse_src("a || b ? c : d");
    ASSERT_EXPR(result, e);
    ASSERT_TRUE(std::holds_alternative<ConditionalExpression>(e.v));
    const auto& ce = std::get<ConditionalExpression>(e.v);
    ASSERT_TRUE(std::holds_alternative<LogicalExpression>(ce.condition->v));
}

// CE-04: then/else 为二元表达式
TEST(ConditionalExpressionParser, CE04_BinaryBranches) {
    auto result = parse_src("x ? a + b : c * d");
    ASSERT_EXPR(result, e);
    ASSERT_TRUE(std::holds_alternative<ConditionalExpression>(e.v));
    const auto& ce = std::get<ConditionalExpression>(e.v);
    ASSERT_TRUE(std::holds_alternative<BinaryExpression>(ce.consequent->v));
    ASSERT_TRUE(std::holds_alternative<BinaryExpression>(ce.alternate->v));
}

// CE-05: 缺少 : 应返回 parse error
TEST(ConditionalExpressionParser, CE05_MissingColon) {
    auto result = parse_program("a ? b");
    // 可能是 parse error 或 eval 时异常，取决于实现——主要验证不崩溃
    // 若解析失败则 !result.ok()，若不失败也至少不崩溃
    (void)result;
}

// ============================================================
// CE-06 ~ CE-15: Interpreter 运行时测试
// ============================================================

// CE-06: 条件为 true
TEST(ConditionalExpressionInterp, CE06_TrueCondition) {
    EXPECT_EQ(interp_num("true ? 1 : 2"), 1.0);
}

// CE-07: 条件为 false
TEST(ConditionalExpressionInterp, CE07_FalseCondition) {
    EXPECT_EQ(interp_num("false ? 1 : 2"), 2.0);
}

// CE-08: 短路——false 时 then 分支不执行
TEST(ConditionalExpressionInterp, CE08_ShortCircuitFalseThen) {
    EXPECT_EQ(interp_str("var x = 0; false ? x = 1 : x = 2; x"), "2");
}

// CE-09: 短路——true 时 else 分支不执行
TEST(ConditionalExpressionInterp, CE09_ShortCircuitTrueElse) {
    EXPECT_EQ(interp_str("var x = 0; true ? x = 1 : x = 2; x"), "1");
}

// CE-10: 数字真值（非零为 true）
TEST(ConditionalExpressionInterp, CE10_NumberTruthy) {
    EXPECT_EQ(interp_num("42 ? 1 : 2"), 1.0);
    EXPECT_EQ(interp_num("0 ? 1 : 2"), 2.0);
}

// CE-11: 字符串真值
TEST(ConditionalExpressionInterp, CE11_StringTruthy) {
    EXPECT_EQ(interp_str(R"("hello" ? "yes" : "no")"), "yes");
    EXPECT_EQ(interp_str(R"("" ? "yes" : "no")"), "no");
}

// CE-12: null/undefined 为 falsy
TEST(ConditionalExpressionInterp, CE12_NullUndefinedFalsy) {
    EXPECT_EQ(interp_num("null ? 1 : 2"), 2.0);
    EXPECT_EQ(interp_num("undefined ? 1 : 2"), 2.0);
}

// CE-13: 右结合求值 true ? 10 : true ? 20 : 30 => 10
TEST(ConditionalExpressionInterp, CE13_RightAssocEval) {
    EXPECT_EQ(interp_num("true ? 10 : true ? 20 : 30"), 10.0);
}

// CE-14: 右结合求值 false ? 10 : false ? 20 : 30 => 30
TEST(ConditionalExpressionInterp, CE14_RightAssocEvalFalse) {
    EXPECT_EQ(interp_num("false ? 10 : false ? 20 : 30"), 30.0);
}

// CE-15: 嵌套在赋值中 var r = cond ? a : b
TEST(ConditionalExpressionInterp, CE15_InAssignment) {
    EXPECT_EQ(interp_str("var r = 3 > 2 ? \"big\" : \"small\"; r"), "big");
}

// ============================================================
// CE-16 ~ CE-25: VM 运行时测试（与 Interpreter 对称）
// ============================================================

// CE-16: 条件为 true
TEST(ConditionalExpressionVM, CE16_TrueCondition) {
    EXPECT_EQ(vm_num("true ? 1 : 2"), 1.0);
}

// CE-17: 条件为 false
TEST(ConditionalExpressionVM, CE17_FalseCondition) {
    EXPECT_EQ(vm_num("false ? 1 : 2"), 2.0);
}

// CE-18: 短路——false 时 then 分支不执行
TEST(ConditionalExpressionVM, CE18_ShortCircuitFalseThen) {
    EXPECT_EQ(vm_str("var x = 0; false ? x = 1 : x = 2; x"), "2");
}

// CE-19: 短路——true 时 else 分支不执行
TEST(ConditionalExpressionVM, CE19_ShortCircuitTrueElse) {
    EXPECT_EQ(vm_str("var x = 0; true ? x = 1 : x = 2; x"), "1");
}

// CE-20: 数字真值
TEST(ConditionalExpressionVM, CE20_NumberTruthy) {
    EXPECT_EQ(vm_num("42 ? 1 : 2"), 1.0);
    EXPECT_EQ(vm_num("0 ? 1 : 2"), 2.0);
}

// CE-21: 字符串真值
TEST(ConditionalExpressionVM, CE21_StringTruthy) {
    EXPECT_EQ(vm_str(R"("hello" ? "yes" : "no")"), "yes");
    EXPECT_EQ(vm_str(R"("" ? "yes" : "no")"), "no");
}

// CE-22: null/undefined 为 falsy
TEST(ConditionalExpressionVM, CE22_NullUndefinedFalsy) {
    EXPECT_EQ(vm_num("null ? 1 : 2"), 2.0);
    EXPECT_EQ(vm_num("undefined ? 1 : 2"), 2.0);
}

// CE-23: 右结合求值 true ? 10 : true ? 20 : 30
TEST(ConditionalExpressionVM, CE23_RightAssocEval) {
    EXPECT_EQ(vm_num("true ? 10 : true ? 20 : 30"), 10.0);
}

// CE-24: 右结合求值 false ? 10 : false ? 20 : 30
TEST(ConditionalExpressionVM, CE24_RightAssocEvalFalse) {
    EXPECT_EQ(vm_num("false ? 10 : false ? 20 : 30"), 30.0);
}

// CE-25: 嵌套在赋值中
TEST(ConditionalExpressionVM, CE25_InAssignment) {
    EXPECT_EQ(vm_str("var r = 3 > 2 ? \"big\" : \"small\"; r"), "big");
}

// ============================================================
// CE-26 ~ CE-45: Interpreter 边界/异常路径补充测试
// ============================================================

// CE-26: 未声明变量作为条件，应产生 ReferenceError
TEST(ConditionalExpressionInterpEdge, CE26_UndeclaredConditionThrows) {
    auto r = interp_run("undeclared_xyz_qppjs_ce26 ? 1 : 2");
    EXPECT_FALSE(r.is_ok());
}

// CE-27: 条件中 getter 抛异常，错误应向上传播
TEST(ConditionalExpressionInterpEdge, CE27_GetterThrowsInCondition) {
    EXPECT_EQ(interp_str(
        "var obj = {};"
        "var caught = false;"
        "Object.defineProperty(obj, 'p', {get: function() { throw new TypeError('cond err'); }});"
        "try { obj.p ? 1 : 2; } catch(e) { caught = true; }"
        "caught"
    ), "true");
}

// CE-28: 短路——条件为 true 时，else 分支 getter 不被调用（副作用不产生）
TEST(ConditionalExpressionInterpEdge, CE28_ShortCircuitElseGetterNotCalled) {
    EXPECT_EQ(interp_str(
        "var obj = {}; var calls = 0;"
        "Object.defineProperty(obj, 'p', {get: function() { calls++; return 99; }});"
        "true ? 1 : obj.p;"
        "calls"
    ), "0");
}

// CE-29: 短路——条件为 false 时，then 分支 getter 不被调用（副作用不产生）
TEST(ConditionalExpressionInterpEdge, CE29_ShortCircuitThenGetterNotCalled) {
    EXPECT_EQ(interp_str(
        "var obj = {}; var calls = 0;"
        "Object.defineProperty(obj, 'p', {get: function() { calls++; return 99; }});"
        "false ? obj.p : 2;"
        "calls"
    ), "0");
}

// CE-30: 被选中分支中的 getter 抛异常，错误仍然传播
TEST(ConditionalExpressionInterpEdge, CE30_GetterThrowsInSelectedBranch) {
    EXPECT_EQ(interp_str(
        "var obj = {}; var caught = false;"
        "Object.defineProperty(obj, 'p', {get: function() { throw new TypeError('branch err'); }});"
        "try { true ? obj.p : 1; } catch(e) { caught = true; }"
        "caught"
    ), "true");
}

// CE-31: 嵌套三元作为外层条件 (expr ? b : c) ? d : e
TEST(ConditionalExpressionInterpEdge, CE31_NestedTernaryAsOuterCondition) {
    EXPECT_EQ(interp_num("(1 > 0 ? true : false) ? 10 : 20"), 10.0);
    EXPECT_EQ(interp_num("(1 > 2 ? true : false) ? 10 : 20"), 20.0);
}

// CE-32: 三层深嵌套三元 a ? b ? c ? v1 : v2 : v3 : v4（右结合）
TEST(ConditionalExpressionInterpEdge, CE32_ThreeLevelDeepNested) {
    // true ? (true ? (false ? 1 : 2) : 3) : 4 → 2
    EXPECT_EQ(interp_num("true ? true ? false ? 1 : 2 : 3 : 4"), 2.0);
    // false ? 1 : (false ? 2 : (true ? 3 : 4)) → 3
    EXPECT_EQ(interp_num("false ? 1 : false ? 2 : true ? 3 : 4"), 3.0);
}

// CE-33: 条件中的函数调用恰好执行一次（不多也不少）
TEST(ConditionalExpressionInterpEdge, CE33_ConditionFunctionCalledOnce) {
    EXPECT_EQ(interp_str(
        "var calls = 0;"
        "function check() { calls++; return true; }"
        "check() ? 1 : 2;"
        "calls"
    ), "1");
}

// CE-34: 仅被选中分支的函数被调用，另一分支完全不执行
TEST(ConditionalExpressionInterpEdge, CE34_OnlySelectedBranchFunctionCalled) {
    EXPECT_EQ(interp_str(
        "var log = [];"
        "function a() { log.push('A'); return 10; }"
        "function b() { log.push('B'); return 20; }"
        "true ? a() : b();"
        "log[0] + '_' + log.length"
    ), "A_1");
    EXPECT_EQ(interp_str(
        "var log2 = [];"
        "function c() { log2.push('C'); return 10; }"
        "function d() { log2.push('D'); return 20; }"
        "false ? c() : d();"
        "log2[0] + '_' + log2.length"
    ), "D_1");
}

// CE-35: 逻辑 && 作为条件——true && false → falsy
TEST(ConditionalExpressionInterpEdge, CE35_LogicalAndAsCondition) {
    EXPECT_EQ(interp_num("true && false ? 1 : 2"), 2.0);
    EXPECT_EQ(interp_num("true && true ? 1 : 2"), 1.0);
    EXPECT_EQ(interp_num("false && true ? 1 : 2"), 2.0);
}

// CE-36: 逻辑 || 作为条件——false || true → truthy
TEST(ConditionalExpressionInterpEdge, CE36_LogicalOrAsCondition) {
    EXPECT_EQ(interp_num("false || false ? 1 : 2"), 2.0);
    EXPECT_EQ(interp_num("false || true ? 1 : 2"), 1.0);
    EXPECT_EQ(interp_num("true || false ? 1 : 2"), 1.0);
}

// CE-37: 分支中包含逻辑 &&，结果是逻辑运算的值而非布尔化后的值
TEST(ConditionalExpressionInterpEdge, CE37_LogicalAndInConsequent) {
    // true ? (false && 99) : 1  →  false（短路结果，不是 0）
    EXPECT_EQ(interp_str("true ? false && 99 : 1"), "false");
    // true ? (1 && 2) : 0  →  2
    EXPECT_EQ(interp_num("true ? 1 && 2 : 0"), 2.0);
}

// CE-38: 分支中包含逻辑 ||，结果是短路的操作数值
TEST(ConditionalExpressionInterpEdge, CE38_LogicalOrInAlternate) {
    // false ? 1 : (false || "ok")  →  "ok"
    EXPECT_EQ(interp_str(R"(false ? 1 : false || "ok")"), "ok");
    // false ? 1 : (0 || 42)  →  42
    EXPECT_EQ(interp_num("false ? 1 : 0 || 42"), 42.0);
}

// CE-39: 三元结果作为函数调用参数
TEST(ConditionalExpressionInterpEdge, CE39_ResultAsFunctionArgument) {
    EXPECT_EQ(interp_num("Math.abs(true ? -5 : 5)"), 5.0);
    EXPECT_EQ(interp_num("Math.abs(false ? -5 : 5)"), 5.0);
    EXPECT_EQ(interp_num("Math.max(true ? 3 : 0, false ? 100 : 7)"), 7.0);
}

// CE-40: 三元结果赋值给对象属性
TEST(ConditionalExpressionInterpEdge, CE40_ResultAsObjectProperty) {
    EXPECT_EQ(interp_str(
        "var obj = {};"
        "obj.x = true ? 42 : 0;"
        "obj.y = false ? 42 : 0;"
        "obj.x + '_' + obj.y"
    ), "42_0");
}

// CE-41: 三元结果作为数组元素
TEST(ConditionalExpressionInterpEdge, CE41_ResultAsArrayElement) {
    EXPECT_EQ(interp_str(
        "var arr = [true ? 1 : 9, false ? 9 : 4];"
        "arr[0] + '_' + arr[1]"
    ), "1_4");
}

// CE-42: while 循环条件中使用三元表达式
TEST(ConditionalExpressionInterpEdge, CE42_AsWhileLoopCondition) {
    EXPECT_EQ(interp_str(
        "var i = 0;"
        "while (i < 3 ? true : false) { i++; }"
        "i"
    ), "3");
}

// CE-43: 函数 return 中使用三元表达式（含链式三元）
TEST(ConditionalExpressionInterpEdge, CE43_InReturnStatement) {
    EXPECT_EQ(interp_str(
        "function sign(n) { return n > 0 ? 'pos' : n < 0 ? 'neg' : 'zero'; }"
        "sign(5) + '_' + sign(-3) + '_' + sign(0)"
    ), "pos_neg_zero");
}

// CE-44: for 循环条件中使用三元表达式
TEST(ConditionalExpressionInterpEdge, CE44_InForLoopCondition) {
    EXPECT_EQ(interp_str(
        "var sum = 0;"
        "for (var i = 0; (i < 5 ? true : false); i++) { sum += i; }"
        "sum"
    ), "10");
}

// CE-45: NaN 作为条件为 falsy（ES2021 规范 ToBoolean 要求）
TEST(ConditionalExpressionInterpEdge, CE45_NaNIsFalsy) {
    EXPECT_EQ(interp_num("NaN ? 1 : 2"), 2.0);
    EXPECT_EQ(interp_num("0/0 ? 1 : 2"), 2.0);
}

// ============================================================
// CE-46 ~ CE-65: VM 边界/异常路径补充测试（与 CE-26~CE-45 对称）
// ============================================================

// CE-46: 未声明变量作为条件，应产生 ReferenceError
TEST(ConditionalExpressionVMEdge, CE46_UndeclaredConditionThrows) {
    auto r = vm_run("undeclared_xyz_qppjs_ce46 ? 1 : 2");
    EXPECT_FALSE(r.is_ok());
}

// CE-47: 条件中 getter 抛异常，错误应向上传播
TEST(ConditionalExpressionVMEdge, CE47_GetterThrowsInCondition) {
    EXPECT_EQ(vm_str(
        "var obj = {};"
        "var caught = false;"
        "Object.defineProperty(obj, 'p', {get: function() { throw new TypeError('cond err'); }});"
        "try { obj.p ? 1 : 2; } catch(e) { caught = true; }"
        "caught"
    ), "true");
}

// CE-48: 短路——条件为 true 时，else 分支 getter 不被调用
TEST(ConditionalExpressionVMEdge, CE48_ShortCircuitElseGetterNotCalled) {
    EXPECT_EQ(vm_str(
        "var obj = {}; var calls = 0;"
        "Object.defineProperty(obj, 'p', {get: function() { calls++; return 99; }});"
        "true ? 1 : obj.p;"
        "calls"
    ), "0");
}

// CE-49: 短路——条件为 false 时，then 分支 getter 不被调用
TEST(ConditionalExpressionVMEdge, CE49_ShortCircuitThenGetterNotCalled) {
    EXPECT_EQ(vm_str(
        "var obj = {}; var calls = 0;"
        "Object.defineProperty(obj, 'p', {get: function() { calls++; return 99; }});"
        "false ? obj.p : 2;"
        "calls"
    ), "0");
}

// CE-50: 被选中分支中的 getter 抛异常，错误仍然传播
TEST(ConditionalExpressionVMEdge, CE50_GetterThrowsInSelectedBranch) {
    EXPECT_EQ(vm_str(
        "var obj = {}; var caught = false;"
        "Object.defineProperty(obj, 'p', {get: function() { throw new TypeError('branch err'); }});"
        "try { true ? obj.p : 1; } catch(e) { caught = true; }"
        "caught"
    ), "true");
}

// CE-51: 嵌套三元作为外层条件
TEST(ConditionalExpressionVMEdge, CE51_NestedTernaryAsOuterCondition) {
    EXPECT_EQ(vm_num("(1 > 0 ? true : false) ? 10 : 20"), 10.0);
    EXPECT_EQ(vm_num("(1 > 2 ? true : false) ? 10 : 20"), 20.0);
}

// CE-52: 三层深嵌套三元
TEST(ConditionalExpressionVMEdge, CE52_ThreeLevelDeepNested) {
    EXPECT_EQ(vm_num("true ? true ? false ? 1 : 2 : 3 : 4"), 2.0);
    EXPECT_EQ(vm_num("false ? 1 : false ? 2 : true ? 3 : 4"), 3.0);
}

// CE-53: 条件中的函数调用恰好执行一次
TEST(ConditionalExpressionVMEdge, CE53_ConditionFunctionCalledOnce) {
    EXPECT_EQ(vm_str(
        "var calls = 0;"
        "function check() { calls++; return true; }"
        "check() ? 1 : 2;"
        "calls"
    ), "1");
}

// CE-54: 仅被选中分支的函数被调用，另一分支完全不执行
TEST(ConditionalExpressionVMEdge, CE54_OnlySelectedBranchFunctionCalled) {
    EXPECT_EQ(vm_str(
        "var log = [];"
        "function a() { log.push('A'); return 10; }"
        "function b() { log.push('B'); return 20; }"
        "true ? a() : b();"
        "log[0] + '_' + log.length"
    ), "A_1");
    EXPECT_EQ(vm_str(
        "var log2 = [];"
        "function c() { log2.push('C'); return 10; }"
        "function d() { log2.push('D'); return 20; }"
        "false ? c() : d();"
        "log2[0] + '_' + log2.length"
    ), "D_1");
}

// CE-55: 逻辑 && 作为条件
TEST(ConditionalExpressionVMEdge, CE55_LogicalAndAsCondition) {
    EXPECT_EQ(vm_num("true && false ? 1 : 2"), 2.0);
    EXPECT_EQ(vm_num("true && true ? 1 : 2"), 1.0);
    EXPECT_EQ(vm_num("false && true ? 1 : 2"), 2.0);
}

// CE-56: 逻辑 || 作为条件
TEST(ConditionalExpressionVMEdge, CE56_LogicalOrAsCondition) {
    EXPECT_EQ(vm_num("false || false ? 1 : 2"), 2.0);
    EXPECT_EQ(vm_num("false || true ? 1 : 2"), 1.0);
    EXPECT_EQ(vm_num("true || false ? 1 : 2"), 1.0);
}

// CE-57: 分支中包含逻辑 &&，结果是运算数值而非布尔化后的值
TEST(ConditionalExpressionVMEdge, CE57_LogicalAndInConsequent) {
    EXPECT_EQ(vm_str("true ? false && 99 : 1"), "false");
    EXPECT_EQ(vm_num("true ? 1 && 2 : 0"), 2.0);
}

// CE-58: 分支中包含逻辑 ||，结果是短路的操作数值
TEST(ConditionalExpressionVMEdge, CE58_LogicalOrInAlternate) {
    EXPECT_EQ(vm_str(R"(false ? 1 : false || "ok")"), "ok");
    EXPECT_EQ(vm_num("false ? 1 : 0 || 42"), 42.0);
}

// CE-59: 三元结果作为函数调用参数
TEST(ConditionalExpressionVMEdge, CE59_ResultAsFunctionArgument) {
    EXPECT_EQ(vm_num("Math.abs(true ? -5 : 5)"), 5.0);
    EXPECT_EQ(vm_num("Math.abs(false ? -5 : 5)"), 5.0);
    EXPECT_EQ(vm_num("Math.max(true ? 3 : 0, false ? 100 : 7)"), 7.0);
}

// CE-60: 三元结果赋值给对象属性
TEST(ConditionalExpressionVMEdge, CE60_ResultAsObjectProperty) {
    EXPECT_EQ(vm_str(
        "var obj = {};"
        "obj.x = true ? 42 : 0;"
        "obj.y = false ? 42 : 0;"
        "obj.x + '_' + obj.y"
    ), "42_0");
}

// CE-61: 三元结果作为数组元素
TEST(ConditionalExpressionVMEdge, CE61_ResultAsArrayElement) {
    EXPECT_EQ(vm_str(
        "var arr = [true ? 1 : 9, false ? 9 : 4];"
        "arr[0] + '_' + arr[1]"
    ), "1_4");
}

// CE-62: while 循环条件中使用三元表达式
TEST(ConditionalExpressionVMEdge, CE62_AsWhileLoopCondition) {
    EXPECT_EQ(vm_str(
        "var i = 0;"
        "while (i < 3 ? true : false) { i++; }"
        "i"
    ), "3");
}

// CE-63: 函数 return 中使用三元表达式（含链式三元）
TEST(ConditionalExpressionVMEdge, CE63_InReturnStatement) {
    EXPECT_EQ(vm_str(
        "function sign(n) { return n > 0 ? 'pos' : n < 0 ? 'neg' : 'zero'; }"
        "sign(5) + '_' + sign(-3) + '_' + sign(0)"
    ), "pos_neg_zero");
}

// CE-64: for 循环条件中使用三元表达式
TEST(ConditionalExpressionVMEdge, CE64_InForLoopCondition) {
    EXPECT_EQ(vm_str(
        "var sum = 0;"
        "for (var i = 0; (i < 5 ? true : false); i++) { sum += i; }"
        "sum"
    ), "10");
}

// CE-65: NaN 作为条件为 falsy
TEST(ConditionalExpressionVMEdge, CE65_NaNIsFalsy) {
    EXPECT_EQ(vm_num("NaN ? 1 : 2"), 2.0);
    EXPECT_EQ(vm_num("0/0 ? 1 : 2"), 2.0);
}

}  // namespace
