#include "qppjs/frontend/ast_dump.h"
#include "qppjs/frontend/ast.h"
#include "qppjs/frontend/parser.h"
#include <gtest/gtest.h>
#include <string>

using namespace qppjs;

// ---- 测试 1：数字字面量 ----

TEST(AstDumpExpr, NumberLiteral) {
    std::string out = dump_expr(ExprNode{NumberLiteral{42.0, {0, 2}}});
    EXPECT_NE(out.find("NumberLiteral(42)"), std::string::npos) << "actual: " << out;
}

// ---- 测试 2：字符串字面量 ----

TEST(AstDumpExpr, StringLiteral) {
    std::string out = dump_expr(ExprNode{StringLiteral{"hello", {0, 7}}});
    EXPECT_NE(out.find("StringLiteral(\"hello\")"), std::string::npos) << "actual: " << out;
}

// ---- 测试 3：布尔字面量 true ----

TEST(AstDumpExpr, BooleanLiteralTrue) {
    std::string out = dump_expr(ExprNode{BooleanLiteral{true, {0, 4}}});
    EXPECT_NE(out.find("BooleanLiteral(true)"), std::string::npos) << "actual: " << out;
}

// ---- 测试 4：null 字面量 ----

TEST(AstDumpExpr, NullLiteral) {
    std::string out = dump_expr(ExprNode{NullLiteral{{0, 4}}});
    EXPECT_NE(out.find("NullLiteral"), std::string::npos) << "actual: " << out;
}

// ---- 测试 5：标识符 ----

TEST(AstDumpExpr, Identifier) {
    std::string out = dump_expr(ExprNode{Identifier{"x", {0, 1}}});
    EXPECT_NE(out.find("Identifier(x)"), std::string::npos) << "actual: " << out;
}

// ---- 测试 6：一元表达式 ----

TEST(AstDumpExpr, UnaryExpression) {
    std::string out = dump_expr(ExprNode{UnaryExpression{
        UnaryOp::Minus,
        std::make_unique<ExprNode>(NumberLiteral{1.0, {1, 1}}),
        {0, 2}
    }});
    EXPECT_NE(out.find("UnaryExpression(-)"), std::string::npos) << "actual: " << out;
    EXPECT_NE(out.find("NumberLiteral(1)"), std::string::npos) << "actual: " << out;
}

// ---- 测试 7：二元表达式（验证缩进层级）----

TEST(AstDumpExpr, BinaryExpression) {
    // 1 + 2
    std::string out = dump_expr(ExprNode{BinaryExpression{
        BinaryOp::Add,
        std::make_unique<ExprNode>(NumberLiteral{1.0, {0, 1}}),
        std::make_unique<ExprNode>(NumberLiteral{2.0, {4, 1}}),
        {0, 5}
    }});
    EXPECT_NE(out.find("BinaryExpression(+)"), std::string::npos) << "actual: " << out;
    // 子节点应有缩进
    EXPECT_NE(out.find("  NumberLiteral(1)"), std::string::npos) << "actual: " << out;
    EXPECT_NE(out.find("  NumberLiteral(2)"), std::string::npos) << "actual: " << out;
}

// ---- 测试 8：逻辑表达式 ----

TEST(AstDumpExpr, LogicalExpression) {
    // a && b
    std::string out = dump_expr(ExprNode{LogicalExpression{
        LogicalOp::And,
        std::make_unique<ExprNode>(Identifier{"a", {0, 1}}),
        std::make_unique<ExprNode>(Identifier{"b", {5, 1}}),
        {0, 6}
    }});
    EXPECT_NE(out.find("LogicalExpression(&&)"), std::string::npos) << "actual: " << out;
}

// ---- 测试 9：赋值表达式 ----

TEST(AstDumpExpr, AssignmentExpression) {
    // x = 1
    std::string out = dump_expr(ExprNode{AssignmentExpression{
        AssignOp::Assign,
        "x",
        std::make_unique<ExprNode>(NumberLiteral{1.0, {4, 1}}),
        {0, 5}
    }});
    EXPECT_NE(out.find("AssignmentExpression(=)"), std::string::npos) << "actual: " << out;
    EXPECT_NE(out.find("target: x"), std::string::npos) << "actual: " << out;
}

// ---- 测试 10：VariableDeclaration（let x = 1）----

TEST(AstDumpStmt, VariableDeclaration) {
    std::string out = dump_stmt(StmtNode{VariableDeclaration{
        VarKind::Let,
        "x",
        ExprNode{NumberLiteral{1.0, {8, 1}}},
        {0, 9}
    }});
    EXPECT_NE(out.find("VariableDeclaration(let)"), std::string::npos) << "actual: " << out;
    EXPECT_NE(out.find("name: x"), std::string::npos) << "actual: " << out;
}

// ---- 测试 11：BlockStatement（空）----

TEST(AstDumpStmt, BlockStatementEmpty) {
    std::string out = dump_stmt(StmtNode{BlockStatement{{}, {0, 2}}});
    EXPECT_NE(out.find("BlockStatement"), std::string::npos) << "actual: " << out;
}

// ---- 测试 12：IfStatement（无 else）----

TEST(AstDumpStmt, IfStatementNoElse) {
    // if (true) {}
    IfStatement is{
        ExprNode{BooleanLiteral{true, {3, 4}}},
        std::make_unique<StmtNode>(BlockStatement{{}, {9, 2}}),
        nullptr,
        {0, 11}
    };
    std::string out = dump_stmt(StmtNode{std::move(is)});
    EXPECT_NE(out.find("IfStatement"), std::string::npos) << "actual: " << out;
    EXPECT_NE(out.find("test:"), std::string::npos) << "actual: " << out;
    EXPECT_NE(out.find("alternate: (none)"), std::string::npos) << "actual: " << out;
}

// ---- 测试 13：IfStatement（有 else）----

TEST(AstDumpStmt, IfStatementWithElse) {
    // if (true) {} else {}
    IfStatement is{
        ExprNode{BooleanLiteral{true, {3, 4}}},
        std::make_unique<StmtNode>(BlockStatement{{}, {9, 2}}),
        std::make_unique<StmtNode>(BlockStatement{{}, {17, 2}}),
        {0, 19}
    };
    std::string out = dump_stmt(StmtNode{std::move(is)});
    // 有 alternate 时不应出现 (none)
    EXPECT_EQ(out.find("alternate: (none)"), std::string::npos) << "actual: " << out;
    EXPECT_NE(out.find("alternate:"), std::string::npos) << "actual: " << out;
}

// ---- 测试 14：WhileStatement ----

TEST(AstDumpStmt, WhileStatement) {
    // while (true) {}
    WhileStatement ws{
        ExprNode{BooleanLiteral{true, {6, 4}}},
        std::make_unique<StmtNode>(BlockStatement{{}, {12, 2}}),
        {0, 14}
    };
    std::string out = dump_stmt(StmtNode{std::move(ws)});
    EXPECT_NE(out.find("WhileStatement"), std::string::npos) << "actual: " << out;
}

// ---- 测试 15：ReturnStatement（无参数）----

TEST(AstDumpStmt, ReturnStatementNoArg) {
    std::string out = dump_stmt(StmtNode{ReturnStatement{std::nullopt, {0, 6}}});
    EXPECT_NE(out.find("ReturnStatement"), std::string::npos) << "actual: " << out;
    EXPECT_NE(out.find("argument: (none)"), std::string::npos) << "actual: " << out;
}

// ---- 测试 16：Program dump ----

TEST(AstDumpProgram, BasicProgram) {
    auto result = parse_program("1+2;");
    ASSERT_TRUE(result.ok());
    std::string out = dump_program(result.value());
    EXPECT_NE(out.find("Program"), std::string::npos) << "actual: " << out;
    EXPECT_NE(out.find("ExpressionStatement"), std::string::npos) << "actual: " << out;
}

// ---- 测试 17：嵌套缩进验证（1+2*3）----

TEST(AstDumpExpr, NestedIndent) {
    // 1 + 2*3 -> BinaryExpression(+) 的 right 是 BinaryExpression(*)
    auto result = parse_program("1+2*3;");
    ASSERT_TRUE(result.ok());
    ASSERT_FALSE(result.value().body.empty());
    ASSERT_TRUE(std::holds_alternative<ExpressionStatement>(result.value().body[0].v));
    const ExprNode& expr = std::get<ExpressionStatement>(result.value().body[0].v).expr;
    std::string out = dump_expr(expr);
    // 顶层节点
    EXPECT_EQ(out.find("BinaryExpression(+)"), 0u) << "actual: " << out;
    // 第一级子节点缩进 2 空格
    EXPECT_NE(out.find("  NumberLiteral(1)"), std::string::npos) << "actual: " << out;
    EXPECT_NE(out.find("  BinaryExpression(*)"), std::string::npos) << "actual: " << out;
    // 第二级子节点缩进 4 空格
    EXPECT_NE(out.find("    NumberLiteral(2)"), std::string::npos) << "actual: " << out;
    EXPECT_NE(out.find("    NumberLiteral(3)"), std::string::npos) << "actual: " << out;
}

TEST(AstDumpExpr, LargeIntegerNumber) {
    // 1e20 超出 long long 范围，应正常输出而不产生 UB（输出科学计数法也可接受）
    std::string out = dump_expr(ExprNode{NumberLiteral{1e20, {0, 4}}});
    EXPECT_NE(out.find("NumberLiteral("), std::string::npos) << "actual: " << out;
    // 不应包含 ".0" 小数点形式
    EXPECT_EQ(out.find(".0"), std::string::npos) << "should not have .0: " << out;
}
