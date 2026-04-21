#pragma once

#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "qppjs/frontend/token.h"

namespace qppjs {

// ---- overloaded helper ----

template<typename... Ts>
struct overloaded : Ts... { using Ts::operator()...; };
template<typename... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

// ---- 枚举 ----

enum class UnaryOp  { Minus, Plus, Bang, Typeof, Void };
enum class BinaryOp { Add, Sub, Mul, Div, Mod, Lt, Gt, LtEq, GtEq, EqEq, NotEq, EqEqEq, NotEqEq };
enum class LogicalOp{ And, Or };
enum class AssignOp { Assign, AddAssign, SubAssign, MulAssign, DivAssign, ModAssign };
enum class VarKind  { Var, Let, Const };

// ---- 前向声明 ----

struct ExprNode;
struct StmtNode;

// ---- 表达式节点（递归子节点均用 unique_ptr<ExprNode>）----

struct NumberLiteral {
    double value;
    SourceRange range;
};

struct StringLiteral {
    std::string value;
    SourceRange range;
};

struct BooleanLiteral {
    bool value;
    SourceRange range;
};

struct NullLiteral {
    SourceRange range;
};

struct Identifier {
    std::string name;
    SourceRange range;
};

struct UnaryExpression {
    UnaryOp op;
    std::unique_ptr<ExprNode> operand;
    SourceRange range;
};

struct BinaryExpression {
    BinaryOp op;
    std::unique_ptr<ExprNode> left;
    std::unique_ptr<ExprNode> right;
    SourceRange range;
};

struct LogicalExpression {
    LogicalOp op;
    std::unique_ptr<ExprNode> left;
    std::unique_ptr<ExprNode> right;
    SourceRange range;
};

struct AssignmentExpression {
    AssignOp op;
    std::string target;
    std::unique_ptr<ExprNode> value;
    SourceRange range;
};

// ---- ExprNode 完整定义（必须在所有表达式 struct 定义之后）----

struct ExprNode {
    std::variant<
        NumberLiteral,
        StringLiteral,
        BooleanLiteral,
        NullLiteral,
        Identifier,
        UnaryExpression,
        BinaryExpression,
        LogicalExpression,
        AssignmentExpression
    > v;

    template<typename T>
    ExprNode(T&& node) : v(std::forward<T>(node)) {}
};

// ---- 语句节点（ExprNode 已完整；BlockStatement 的 vector<StmtNode> 在 StmtNode 完整前声明，
//      但 libc++/libstdc++ 均作为扩展支持 vector 持有不完整类型）----

struct ExpressionStatement {
    ExprNode expr;
    SourceRange range;
};

struct VariableDeclaration {
    VarKind kind;
    std::string name;
    std::optional<ExprNode> init;
    SourceRange range;
};

struct BlockStatement {
    std::vector<StmtNode> body;
    SourceRange range;
};

struct IfStatement {
    ExprNode test;
    std::unique_ptr<StmtNode> consequent;
    std::unique_ptr<StmtNode> alternate;
    SourceRange range;
};

struct WhileStatement {
    ExprNode test;
    std::unique_ptr<StmtNode> body;
    SourceRange range;
};

struct ReturnStatement {
    std::optional<ExprNode> argument;
    SourceRange range;
};

// ---- StmtNode 完整定义（必须在所有语句 struct 定义之后）----

struct StmtNode {
    std::variant<
        ExpressionStatement,
        VariableDeclaration,
        BlockStatement,
        IfStatement,
        WhileStatement,
        ReturnStatement
    > v;

    template<typename T>
    StmtNode(T&& node) : v(std::forward<T>(node)) {}
};

// ---- 程序根节点 ----

struct Program {
    std::vector<StmtNode> body;
    SourceRange range;
};

} // namespace qppjs
