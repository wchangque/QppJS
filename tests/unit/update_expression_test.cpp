#include "qppjs/frontend/ast_dump.h"
#include "qppjs/frontend/parser.h"
#include "qppjs/runtime/interpreter.h"
#include "qppjs/vm/compiler.h"
#include "qppjs/vm/vm.h"

#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <string_view>

using namespace qppjs;

namespace {

// ============================================================
// 辅助函数
// ============================================================

static ParseResult<Program> parse_expr_src(const char* src) { return parse_program(std::string(src) + ";"); }

#define ASSERT_EXPR(result, expr_ref)                                                     \
    ASSERT_TRUE((result).ok());                                                           \
    ASSERT_FALSE((result).value().body.empty());                                          \
    ASSERT_TRUE(std::holds_alternative<ExpressionStatement>((result).value().body[0].v)); \
    const ExprNode& expr_ref = std::get<ExpressionStatement>((result).value().body[0].v).expr

// ============================================================
// Parser 测试：前缀 ++/-- (nud)
// ============================================================

TEST(UpdateExpressionParser, PrefixIncIdentifier) {
    auto result = parse_expr_src("++x");
    ASSERT_EXPR(result, e);
    ASSERT_TRUE(std::holds_alternative<UpdateExpression>(e.v));
    const auto& ue = std::get<UpdateExpression>(e.v);
    EXPECT_EQ(ue.op, UpdateOp::Inc);
    EXPECT_TRUE(ue.prefix);
    ASSERT_TRUE(ue.operand);
    ASSERT_TRUE(std::holds_alternative<Identifier>(ue.operand->v));
    EXPECT_EQ(std::get<Identifier>(ue.operand->v).name, "x");
}

TEST(UpdateExpressionParser, PrefixDecIdentifier) {
    auto result = parse_expr_src("--x");
    ASSERT_EXPR(result, e);
    ASSERT_TRUE(std::holds_alternative<UpdateExpression>(e.v));
    const auto& ue = std::get<UpdateExpression>(e.v);
    EXPECT_EQ(ue.op, UpdateOp::Dec);
    EXPECT_TRUE(ue.prefix);
    ASSERT_TRUE(ue.operand);
    ASSERT_TRUE(std::holds_alternative<Identifier>(ue.operand->v));
    EXPECT_EQ(std::get<Identifier>(ue.operand->v).name, "x");
}

TEST(UpdateExpressionParser, PrefixIncMemberDot) {
    auto result = parse_expr_src("++obj.x");
    ASSERT_EXPR(result, e);
    ASSERT_TRUE(std::holds_alternative<UpdateExpression>(e.v));
    const auto& ue = std::get<UpdateExpression>(e.v);
    EXPECT_EQ(ue.op, UpdateOp::Inc);
    EXPECT_TRUE(ue.prefix);
    ASSERT_TRUE(ue.operand);
    ASSERT_TRUE(std::holds_alternative<MemberExpression>(ue.operand->v));
}

TEST(UpdateExpressionParser, PrefixDecMemberBracket) {
    auto result = parse_expr_src("--obj[0]");
    ASSERT_EXPR(result, e);
    ASSERT_TRUE(std::holds_alternative<UpdateExpression>(e.v));
    const auto& ue = std::get<UpdateExpression>(e.v);
    EXPECT_EQ(ue.op, UpdateOp::Dec);
    EXPECT_TRUE(ue.prefix);
    ASSERT_TRUE(ue.operand);
    ASSERT_TRUE(std::holds_alternative<MemberExpression>(ue.operand->v));
}

// ============================================================
// Parser 测试：后缀 ++/-- (led)
// ============================================================

TEST(UpdateExpressionParser, PostfixIncIdentifier) {
    auto result = parse_expr_src("x++");
    ASSERT_EXPR(result, e);
    ASSERT_TRUE(std::holds_alternative<UpdateExpression>(e.v));
    const auto& ue = std::get<UpdateExpression>(e.v);
    EXPECT_EQ(ue.op, UpdateOp::Inc);
    EXPECT_FALSE(ue.prefix);
    ASSERT_TRUE(ue.operand);
    ASSERT_TRUE(std::holds_alternative<Identifier>(ue.operand->v));
    EXPECT_EQ(std::get<Identifier>(ue.operand->v).name, "x");
}

TEST(UpdateExpressionParser, PostfixDecIdentifier) {
    auto result = parse_expr_src("x--");
    ASSERT_EXPR(result, e);
    ASSERT_TRUE(std::holds_alternative<UpdateExpression>(e.v));
    const auto& ue = std::get<UpdateExpression>(e.v);
    EXPECT_EQ(ue.op, UpdateOp::Dec);
    EXPECT_FALSE(ue.prefix);
    ASSERT_TRUE(ue.operand);
    ASSERT_TRUE(std::holds_alternative<Identifier>(ue.operand->v));
    EXPECT_EQ(std::get<Identifier>(ue.operand->v).name, "x");
}

TEST(UpdateExpressionParser, PostfixIncMemberDot) {
    auto result = parse_expr_src("obj.x++");
    ASSERT_EXPR(result, e);
    ASSERT_TRUE(std::holds_alternative<UpdateExpression>(e.v));
    const auto& ue = std::get<UpdateExpression>(e.v);
    EXPECT_EQ(ue.op, UpdateOp::Inc);
    EXPECT_FALSE(ue.prefix);
    ASSERT_TRUE(ue.operand);
    ASSERT_TRUE(std::holds_alternative<MemberExpression>(ue.operand->v));
}

TEST(UpdateExpressionParser, PostfixDecMemberBracket) {
    auto result = parse_expr_src("obj[0]--");
    ASSERT_EXPR(result, e);
    ASSERT_TRUE(std::holds_alternative<UpdateExpression>(e.v));
    const auto& ue = std::get<UpdateExpression>(e.v);
    EXPECT_EQ(ue.op, UpdateOp::Dec);
    EXPECT_FALSE(ue.prefix);
    ASSERT_TRUE(ue.operand);
    ASSERT_TRUE(std::holds_alternative<MemberExpression>(ue.operand->v));
}

// postfix ++ has higher precedence than assignment
TEST(UpdateExpressionParser, PostfixPrecedenceOverAssign) {
    auto result = parse_program("var a = x++;");
    ASSERT_TRUE(result.ok());
    ASSERT_FALSE(result.value().body.empty());
    ASSERT_TRUE(std::holds_alternative<VariableDeclaration>(result.value().body[0].v));
    const auto& vd = std::get<VariableDeclaration>(result.value().body[0].v);
    ASSERT_TRUE(vd.init.has_value());
    ASSERT_TRUE(std::holds_alternative<UpdateExpression>(vd.init->v));
}

// ============================================================
// Parser 错误路径
// ============================================================

TEST(UpdateExpressionParser, PrefixOnNonAssignableError) {
    auto result = parse_expr_src("++42");
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error().message().find("assignment"), std::string::npos);
}

TEST(UpdateExpressionParser, PostfixOnNonAssignableError) {
    auto result = parse_expr_src("42++");
    EXPECT_FALSE(result.ok());
}

// ============================================================
// Interpreter 执行测试：变量自增/自减
// ============================================================

Value exec_ok(std::string_view source) {
    auto parse_result = parse_program(source);
    EXPECT_TRUE(parse_result.ok()) << "parse failed: " << parse_result.error().message();
    Interpreter interp;
    auto result = interp.exec(parse_result.value());
    EXPECT_TRUE(result.is_ok()) << "exec failed: " << result.error().message();
    return result.value();
}

TEST(UpdateExpressionInterp, PrefixIncVar) {
    auto v = exec_ok("let x = 1; ++x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.0);
}

TEST(UpdateExpressionInterp, PrefixDecVar) {
    auto v = exec_ok("let x = 3; --x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.0);
}

TEST(UpdateExpressionInterp, PostfixIncVar) {
    auto v = exec_ok("let x = 1; x++");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

TEST(UpdateExpressionInterp, PostfixDecVar) {
    auto v = exec_ok("let x = 3; x--");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 3.0);
}

TEST(UpdateExpressionInterp, PrefixIncVarSideEffect) {
    auto v = exec_ok("let x = 1; ++x; x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.0);
}

TEST(UpdateExpressionInterp, PostfixIncVarSideEffect) {
    auto v = exec_ok("let x = 1; x++; x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.0);
}

TEST(UpdateExpressionInterp, PrefixDecVarSideEffect) {
    auto v = exec_ok("let x = 5; --x; x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 4.0);
}

TEST(UpdateExpressionInterp, PostfixDecVarSideEffect) {
    auto v = exec_ok("let x = 5; x--; x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 4.0);
}

TEST(UpdateExpressionInterp, MultiplePrefixInc) {
    auto v = exec_ok("let x = 1; ++x; ++x; x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 3.0);
}

TEST(UpdateExpressionInterp, MultiplePostfixInc) {
    auto v = exec_ok("let x = 1; x++; x++; x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 3.0);
}

TEST(UpdateExpressionInterp, PrefixIncMemberDot) {
    auto v = exec_ok("let o = {a: 1}; ++o.a");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.0);
}

TEST(UpdateExpressionInterp, PrefixDecMemberDot) {
    auto v = exec_ok("let o = {a: 3}; --o.a");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.0);
}

TEST(UpdateExpressionInterp, PostfixIncMemberDot) {
    auto v = exec_ok("let o = {a: 1}; o.a++");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

TEST(UpdateExpressionInterp, PostfixDecMemberDot) {
    auto v = exec_ok("let o = {a: 3}; o.a--");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 3.0);
}

TEST(UpdateExpressionInterp, PrefixIncMemberBracket) {
    auto v = exec_ok("let o = {a: 1}; ++o['a']");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.0);
}

TEST(UpdateExpressionInterp, PostfixIncMemberBracket) {
    auto v = exec_ok("let o = {a: 1}; o['a']++");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

TEST(UpdateExpressionInterp, MemberDotSideEffect) {
    auto v = exec_ok("let o = {a: 1}; o.a++; o.a");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.0);
}

TEST(UpdateExpressionInterp, MemberBracketSideEffect) {
    auto v = exec_ok("let o = {a: 5}; ++o['a']; o['a']");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 6.0);
}

// ============================================================
// Interpreter 执行测试：边界情况
// ============================================================

TEST(UpdateExpressionInterp, PrefixIncOnStringCoercion) {
    auto v = exec_ok("let x = '1'; ++x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.0);
}

TEST(UpdateExpressionInterp, PostfixIncOnStringCoercion) {
    auto v = exec_ok("let x = '1'; x++");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

TEST(UpdateExpressionInterp, PrefixIncAssignmentTarget) {
    auto v = exec_ok("let x = 1; let y = ++x; x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.0);
}

TEST(UpdateExpressionInterp, PostfixIncAssignmentTarget) {
    auto v = exec_ok("let x = 1; let y = x++; x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.0);
}

TEST(UpdateExpressionInterp, PostfixIncReturnOldValue) {
    auto v = exec_ok("let x = 1; let y = x++; y");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

TEST(UpdateExpressionInterp, PrefixIncReturnNewValue) {
    auto v = exec_ok("let x = 1; let y = ++x; y");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.0);
}

// ============================================================
// VM 执行测试：变量自增/自减
// ============================================================

Value vm_exec_ok(std::string_view source) {
    auto parse_result = parse_program(source);
    EXPECT_TRUE(parse_result.ok()) << "parse failed: " << parse_result.error().message();
    if (!parse_result.ok()) return Value::undefined();
    Compiler compiler;
    auto bytecode = compiler.compile(parse_result.value());
    VM vm;
    auto result = vm.exec(bytecode);
    EXPECT_TRUE(result.is_ok()) << "exec failed: " << result.error().message();
    if (!result.is_ok()) return Value::undefined();
    return result.value();
}

TEST(UpdateExpressionVM, PrefixIncVar) {
    auto v = vm_exec_ok("let x = 1; ++x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.0);
}

TEST(UpdateExpressionVM, PrefixDecVar) {
    auto v = vm_exec_ok("let x = 3; --x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.0);
}

TEST(UpdateExpressionVM, PostfixIncVar) {
    auto v = vm_exec_ok("let x = 1; x++");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

TEST(UpdateExpressionVM, PostfixDecVar) {
    auto v = vm_exec_ok("let x = 3; x--");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 3.0);
}

TEST(UpdateExpressionVM, PrefixIncVarSideEffect) {
    auto v = vm_exec_ok("let x = 1; ++x; x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.0);
}

TEST(UpdateExpressionVM, PostfixIncVarSideEffect) {
    auto v = vm_exec_ok("let x = 1; x++; x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.0);
}

TEST(UpdateExpressionVM, PrefixIncMemberDot) {
    auto v = vm_exec_ok("let o = {a: 1}; ++o.a");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.0);
}

TEST(UpdateExpressionVM, PostfixIncMemberDot) {
    auto v = vm_exec_ok("let o = {a: 1}; o.a++");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

TEST(UpdateExpressionVM, MemberDotSideEffect) {
    auto v = vm_exec_ok("let o = {a: 1}; o.a++; o.a");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.0);
}

TEST(UpdateExpressionVM, PrefixIncMemberBracket) {
    auto v = vm_exec_ok("let o = {a: 1}; ++o['a']");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.0);
}

TEST(UpdateExpressionVM, PostfixIncMemberBracket) {
    auto v = vm_exec_ok("let o = {a: 1}; o['a']++");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

TEST(UpdateExpressionVM, PostfixIncReturnOldValue) {
    auto v = vm_exec_ok("let x = 1; let y = x++; y");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

TEST(UpdateExpressionVM, PrefixIncReturnNewValue) {
    auto v = vm_exec_ok("let x = 1; let y = ++x; y");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.0);
}

// ============================================================
// ast_dump 测试
// ============================================================

TEST(UpdateExpressionAstDump, PrefixInc) {
    auto parse_result = parse_program("++x;");
    ASSERT_TRUE(parse_result.ok());
    auto d = dump_program(parse_result.value());
    EXPECT_NE(d.find("UpdateExpression"), std::string::npos);
    EXPECT_NE(d.find("prefix"), std::string::npos);
    EXPECT_NE(d.find("Inc"), std::string::npos);
    EXPECT_NE(d.find("Identifier(x)"), std::string::npos);
}

TEST(UpdateExpressionAstDump, PostfixDec) {
    auto parse_result = parse_program("x--;");
    ASSERT_TRUE(parse_result.ok());
    auto d = dump_program(parse_result.value());
    EXPECT_NE(d.find("UpdateExpression"), std::string::npos);
    EXPECT_NE(d.find("postfix"), std::string::npos);
    EXPECT_NE(d.find("Dec"), std::string::npos);
    EXPECT_NE(d.find("Identifier(x)"), std::string::npos);
}

}  // namespace
