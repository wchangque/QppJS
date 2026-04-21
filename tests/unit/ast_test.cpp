#include "qppjs/frontend/ast.h"
#include <gtest/gtest.h>

using namespace qppjs;

TEST(AstTest, NodeConstruction) {
    // 1. 各节点可以构造
    NumberLiteral nl{1.0, {0, 1}};
    StringLiteral sl{"hello", {0, 7}};
    BooleanLiteral bl{true, {0, 4}};
    NullLiteral null_l{{0, 4}};
    Identifier id{"x", {0, 1}};

    EXPECT_EQ(nl.value, 1.0);
    EXPECT_EQ(sl.value, "hello");
    EXPECT_TRUE(bl.value);
    EXPECT_EQ(null_l.range.offset, 0u);
    EXPECT_EQ(id.name, "x");
}

TEST(AstTest, ExprNodeConstruction) {
    // 2. ExprNode 可以从各节点构造
    ExprNode e1{NumberLiteral{42.0, {0, 2}}};
    ExprNode e2{Identifier{"foo", {0, 3}}};

    EXPECT_TRUE(std::holds_alternative<NumberLiteral>(e1.v));
    EXPECT_TRUE(std::holds_alternative<Identifier>(e2.v));
}

TEST(AstTest, UnaryExpression) {
    // 3. UnaryExpression 可以构造（含 unique_ptr）
    UnaryExpression ue{UnaryOp::Minus, std::make_unique<ExprNode>(NumberLiteral{1.0, {1, 1}}), {0, 2}};
    EXPECT_EQ(ue.op, UnaryOp::Minus);
    EXPECT_TRUE(ue.operand != nullptr);
}

TEST(AstTest, BinaryExpression) {
    // 4. BinaryExpression 可以构造
    BinaryExpression be{
        BinaryOp::Add,
        std::make_unique<ExprNode>(NumberLiteral{1.0, {0, 1}}),
        std::make_unique<ExprNode>(NumberLiteral{2.0, {4, 1}}),
        {0, 5}
    };
    EXPECT_EQ(be.op, BinaryOp::Add);
}

TEST(AstTest, LogicalExpression) {
    // 5. LogicalExpression 独立于 BinaryExpression
    LogicalExpression le{
        LogicalOp::And,
        std::make_unique<ExprNode>(BooleanLiteral{true, {0, 4}}),
        std::make_unique<ExprNode>(BooleanLiteral{false, {8, 5}}),
        {0, 13}
    };
    EXPECT_EQ(le.op, LogicalOp::And);
}

TEST(AstTest, AssignmentExpression) {
    // 6. AssignmentExpression
    AssignmentExpression ae{AssignOp::Assign, "x", std::make_unique<ExprNode>(NumberLiteral{1.0, {4, 1}}), {0, 5}};
    EXPECT_EQ(ae.op, AssignOp::Assign);
    EXPECT_EQ(ae.target, "x");
}

TEST(AstTest, VariableDeclaration) {
    // 7. VariableDeclaration（含 optional init）
    VariableDeclaration vd_with_init{VarKind::Let, "x", ExprNode{NumberLiteral{1.0, {8, 1}}}, {0, 9}};
    VariableDeclaration vd_no_init{VarKind::Var, "y", std::nullopt, {0, 5}};

    EXPECT_EQ(vd_with_init.kind, VarKind::Let);
    EXPECT_TRUE(vd_with_init.init.has_value());
    EXPECT_EQ(vd_no_init.kind, VarKind::Var);
    EXPECT_FALSE(vd_no_init.init.has_value());
}

TEST(AstTest, BlockStatement) {
    // 8. BlockStatement
    BlockStatement bs{std::vector<StmtNode>{}, {0, 2}};
    EXPECT_TRUE(bs.body.empty());
}

TEST(AstTest, IfStatement) {
    // 9. IfStatement（含 nullptr alternate）
    IfStatement is{
        ExprNode{BooleanLiteral{true, {3, 4}}},
        std::make_unique<StmtNode>(BlockStatement{{}, {8, 2}}),
        nullptr,
        {0, 10}
    };
    EXPECT_TRUE(is.consequent != nullptr);
    EXPECT_TRUE(is.alternate == nullptr);
}

TEST(AstTest, WhileStatement) {
    // 10. WhileStatement
    WhileStatement ws{
        ExprNode{BooleanLiteral{true, {6, 4}}},
        std::make_unique<StmtNode>(BlockStatement{{}, {12, 2}}),
        {0, 14}
    };
    EXPECT_TRUE(ws.body != nullptr);
}

TEST(AstTest, ReturnStatement) {
    // 11. ReturnStatement
    ReturnStatement rs_empty{std::nullopt, {0, 6}};
    ReturnStatement rs_with{ExprNode{NumberLiteral{1.0, {7, 1}}}, {0, 8}};

    EXPECT_FALSE(rs_empty.argument.has_value());
    EXPECT_TRUE(rs_with.argument.has_value());
}

TEST(AstTest, Program) {
    // 12. Program
    Program prog{std::vector<StmtNode>{}, {0, 0}};
    EXPECT_TRUE(prog.body.empty());
}

TEST(AstTest, StdVisitWorks) {
    // 13. std::visit 工作
    std::visit(overloaded{
        [](const NumberLiteral& n) { EXPECT_EQ(n.value, 42.0); },
        [](const auto&) { FAIL() << "unexpected node type"; }
    }, ExprNode{NumberLiteral{42.0, {0, 2}}}.v);
}

TEST(AstTest, VarKindDistinct) {
    // 14. VarKind 区分
    {
        VariableDeclaration vd_const{VarKind::Const, "x", ExprNode{NumberLiteral{1.0, {0, 1}}}, {0, 1}};
        EXPECT_EQ(vd_const.kind, VarKind::Const);
    }
    {
        VariableDeclaration vd_let{VarKind::Let, "x", std::nullopt, {0, 1}};
        EXPECT_EQ(vd_let.kind, VarKind::Let);
    }
    {
        VariableDeclaration vd_var{VarKind::Var, "x", std::nullopt, {0, 1}};
        EXPECT_EQ(vd_var.kind, VarKind::Var);
    }
}

TEST(AstTest, UndefinedIsIdentifier) {
    // 15. Identifier "undefined" 不是 NullLiteral
    ExprNode undef_node{Identifier{"undefined", {0, 9}}};
    EXPECT_TRUE(std::holds_alternative<Identifier>(undef_node.v));
    EXPECT_FALSE(std::holds_alternative<NullLiteral>(undef_node.v));
}
