#pragma once

#include "qppjs/frontend/token.h"

#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace qppjs {

// ---- overloaded helper ----

template <typename... Ts>
struct overloaded : Ts... {
    using Ts::operator()...;
};
template <typename... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

// ---- 枚举 ----

enum class UnaryOp { Minus, Plus, Bang, Typeof, Void };
enum class BinaryOp { Add, Sub, Mul, Div, Mod, Lt, Gt, LtEq, GtEq, EqEq, NotEq, EqEqEq, NotEqEq, Instanceof };
enum class LogicalOp { And, Or };
enum class AssignOp { Assign, AddAssign, SubAssign, MulAssign, DivAssign, ModAssign };
enum class VarKind { Var, Let, Const };

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

// 对象字面量的单个属性（不是 ExprNode，只是辅助结构）
struct ObjectProperty {
    std::string key;
    std::unique_ptr<ExprNode> value;
    SourceRange range;
};

// 对象字面量 { key: value, ... }
struct ObjectExpression {
    std::vector<ObjectProperty> properties;
    SourceRange range;
};

// 成员访问 obj.prop 或 obj[expr]
struct MemberExpression {
    std::unique_ptr<ExprNode> object;
    std::unique_ptr<ExprNode> property;  // 点号时为 StringLiteral；方括号时为任意表达式
    bool computed;                        // false=点号, true=方括号
    SourceRange range;
};

// 成员赋值 obj.prop = val 或 obj[expr] = val
struct MemberAssignmentExpression {
    std::unique_ptr<ExprNode> object;
    std::unique_ptr<ExprNode> property;
    bool computed;
    std::unique_ptr<ExprNode> value;
    SourceRange range;
};

// 函数表达式 function [name](params) { body }
struct FunctionExpression {
    std::optional<std::string> name;
    std::vector<std::string> params;
    std::shared_ptr<std::vector<StmtNode>> body;
    SourceRange range;
};

// 调用表达式 callee(args)
struct CallExpression {
    std::unique_ptr<ExprNode> callee;
    std::vector<std::unique_ptr<ExprNode>> arguments;
    SourceRange range;
};

// new 表达式 new callee(args)
struct NewExpression {
    std::unique_ptr<ExprNode> callee;
    std::vector<std::unique_ptr<ExprNode>> arguments;
    SourceRange range;
};

// ---- ExprNode 完整定义（必须在所有表达式 struct 定义之后）----

struct ExprNode {
    std::variant<NumberLiteral, StringLiteral, BooleanLiteral, NullLiteral, Identifier, UnaryExpression,
                 BinaryExpression, LogicalExpression, AssignmentExpression,
                 ObjectExpression, MemberExpression, MemberAssignmentExpression,
                 FunctionExpression, CallExpression, NewExpression>
            v;

    ExprNode() = default;

    template <typename T>
        requires(!std::same_as<std::remove_cvref_t<T>, ExprNode>)
    explicit ExprNode(T&& node) : v(std::forward<T>(node)) {}
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

// 函数声明语句 function name(params) { body }
struct FunctionDeclaration {
    std::string name;
    std::vector<std::string> params;
    std::shared_ptr<std::vector<StmtNode>> body;
    SourceRange range;
};

struct ThrowStatement {
    ExprNode argument;
    SourceRange range;
};

// catch(e) { body }（辅助结构，不进 variant）
struct CatchClause {
    std::string param;
    BlockStatement body;
    SourceRange range;
};

struct TryStatement {
    BlockStatement block;
    std::optional<CatchClause> handler;
    std::optional<BlockStatement> finalizer;
    SourceRange range;
};

struct BreakStatement {
    std::optional<std::string> label;
    SourceRange range;
};

struct ContinueStatement {
    std::optional<std::string> label;
    SourceRange range;
};

struct LabeledStatement {
    std::string label;
    std::unique_ptr<StmtNode> body;
    SourceRange range;
};

struct ForStatement {
    std::optional<std::unique_ptr<StmtNode>> init;
    std::optional<ExprNode> test;
    std::optional<ExprNode> update;
    std::unique_ptr<StmtNode> body;
    SourceRange range;
};

// ---- StmtNode 完整定义（必须在所有语句 struct 定义之后）----

struct StmtNode {
    std::variant<ExpressionStatement, VariableDeclaration, BlockStatement, IfStatement, WhileStatement,
                 ReturnStatement, FunctionDeclaration,
                 ThrowStatement, TryStatement, BreakStatement, ContinueStatement,
                 LabeledStatement, ForStatement>
            v;

    StmtNode() = default;

    template <typename T>
        requires(!std::same_as<std::remove_cvref_t<T>, StmtNode>)
    explicit StmtNode(T&& node) : v(std::forward<T>(node)) {}
};

// ---- 程序根节点 ----

struct Program {
    std::vector<StmtNode> body;
    SourceRange range;
};

}  // namespace qppjs
