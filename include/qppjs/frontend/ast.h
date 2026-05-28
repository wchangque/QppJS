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

enum class UnaryOp { Minus, Plus, Bang, Typeof, Void, Delete, BitNot };
enum class UpdateOp { Inc, Dec };
enum class BinaryOp {
    Add, Sub, Mul, Div, Mod, Lt, Gt, LtEq, GtEq, EqEq, NotEq, EqEqEq, NotEqEq, Instanceof, In,
    BitAnd, BitOr, BitXor, Shl, Sar, Shr
};
enum class LogicalOp { And, Or, Nullish };
enum class AssignOp {
    Assign, AddAssign, SubAssign, MulAssign, DivAssign, ModAssign,
    BitAndAssign, BitOrAssign, BitXorAssign, ShlAssign, SarAssign, ShrAssign
};
enum class VarKind { Var, Let, Const };

// 对象字面量属性的种类
enum class MethodKind {
    kData,        // key: value / shorthand {x}
    kMethod,      // foo() {}
    kGetter,      // get foo() {}
    kSetter,      // set foo(v) {}
    kAsyncMethod, // async foo() {}
    kGenerator,   // *foo() {} — 解析时记录，执行层降级为 kMethod
};

// ---- 前向声明 ----

struct ExprNode;
struct StmtNode;
struct PatternNode;

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
    std::string key;                            // 静态键；computed=true 时为空
    std::unique_ptr<ExprNode> key_expr;         // computed=true 时非空，存放键表达式
    std::unique_ptr<ExprNode> value;
    SourceRange range;
    MethodKind method_kind = MethodKind::kData;
    bool computed = false;                      // true = [expr] 计算键
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

// 函数参数定义：可含默认值
struct ParamDef {
    std::string name;
    std::shared_ptr<ExprNode> default_init;  // nullptr = 无默认值
};

// 函数表达式 function [name](params) { body }
struct FunctionExpression {
    std::optional<std::string> name;
    std::vector<ParamDef> params;
    std::optional<std::string> rest_param;  // ...rest，nullopt 表示无 rest
    std::shared_ptr<std::vector<StmtNode>> body;
    SourceRange range;
    bool is_generator = false;  // true for function* expressions
};

// yield 表达式（只在 generator 函数体内合法）
struct YieldExpression {
    bool is_delegate;                          // yield* 时为 true
    std::unique_ptr<ExprNode> argument;        // 可为 nullptr（yield; == yield undefined;）
    SourceRange range;
};

// 数组字面量 [elem0, elem1, ...]
struct ArrayExpression {
    // nullopt entries represent elision holes (e.g. [1,,3] has nullopt at index 1)
    std::vector<std::optional<std::unique_ptr<ExprNode>>> elements;
    SourceRange range;
};

// 展开元素：...expr（用于数组字面量和调用参数中）
struct SpreadElement {
    std::unique_ptr<ExprNode> argument;
    SourceRange range;
};

// await 表达式（只在 async 函数体内合法）
struct AwaitExpression {
    std::unique_ptr<ExprNode> argument;
    SourceRange range;
};

// import.meta 元属性（仅在模块上下文合法）
struct MetaProperty {
    SourceRange range;
};

// 动态 import() 表达式 import(specifier)
struct ImportCallExpression {
    std::unique_ptr<ExprNode> specifier;
    SourceRange range;
};

// 箭头函数表达式 [params] => body
// 表达式体已在 Parser 中合成为含单条 ReturnStatement 的块体
struct ArrowFunctionExpression {
    std::vector<ParamDef> params;
    std::optional<std::string> rest_param;  // ...rest，nullopt 表示无 rest
    std::shared_ptr<std::vector<StmtNode>> body_stmts;
    SourceRange range;
};

// 正则表达式字面量 /pattern/flags
struct RegexLiteral {
    std::string pattern;
    std::string flags;
    SourceRange range;
};

// 模板字符串的文本段
struct TemplateElement {
    std::string cooked;  // 已解转义的文本
    std::string raw;     // 原始文本（暂不用于非标签模板，存空串）
};

// 模板字符串字面量 `...${expr}...`
struct TemplateLiteral {
    std::vector<TemplateElement> quasis;                 // 文本段，长度 = exprs.size() + 1
    std::vector<std::unique_ptr<ExprNode>> expressions;  // 插值表达式
    SourceRange range;
};

// ++/-- 前缀或后缀自增/自减表达式
struct UpdateExpression {
    UpdateOp op;
    std::unique_ptr<ExprNode> operand;
    bool prefix;  // true=++x, false=x++
    SourceRange range;
};

// 三元条件表达式 condition ? consequent : alternate
struct ConditionalExpression {
    std::unique_ptr<ExprNode> condition;
    std::unique_ptr<ExprNode> consequent;
    std::unique_ptr<ExprNode> alternate;
    SourceRange range;
};

// async 函数表达式 async function [name](params) { body }
struct AsyncFunctionExpression {
    std::optional<std::string> name;
    std::vector<ParamDef> params;
    std::optional<std::string> rest_param;  // ...rest，nullopt 表示无 rest
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

// 解构赋值表达式（[a, b] = rhs 或 ({a} = rhs)）
// 定义在 ExprNode 之前：成员均为 unique_ptr（只需前向声明）
struct DestructuringAssignmentExpression {
    std::unique_ptr<PatternNode> pattern;
    std::unique_ptr<ExprNode> value;
    SourceRange range;
};

// Optional chaining 表达式：base?.prop / base?.[key] / base?.()
// 定义在 ExprNode 之前：ElemLink/CallLink 内的 unique_ptr 只需前向声明
struct OptionalChainExpression {
    struct PropLink  { bool optional; std::string name; };
    struct ElemLink  { bool optional; std::unique_ptr<ExprNode> key; };
    struct CallLink  { bool optional; std::vector<std::unique_ptr<ExprNode>> args; };
    using ChainLink = std::variant<PropLink, ElemLink, CallLink>;

    std::unique_ptr<ExprNode> base;
    std::vector<ChainLink> links;
    SourceRange range;
};

// ---- ExprNode 完整定义（必须在所有表达式 struct 定义之后）----

struct ExprNode {
    std::variant<NumberLiteral, StringLiteral, BooleanLiteral, NullLiteral, Identifier, UnaryExpression,
                 BinaryExpression, LogicalExpression, AssignmentExpression,
                 ObjectExpression, MemberExpression, MemberAssignmentExpression,
                 FunctionExpression, CallExpression, NewExpression, ArrayExpression,
                 AwaitExpression, UpdateExpression, AsyncFunctionExpression,
                 MetaProperty, ImportCallExpression, RegexLiteral, TemplateLiteral,
                 ArrowFunctionExpression, ConditionalExpression, SpreadElement,
                 DestructuringAssignmentExpression, OptionalChainExpression,
                 YieldExpression>
            v;

    bool is_parenthesized = false;  // set by Parser when wrapped in ( )

    ExprNode() = default;

    template <typename T>
        requires(!std::same_as<std::remove_cvref_t<T>, ExprNode>)
    explicit ExprNode(T&& node) : v(std::forward<T>(node)) {}
};

// ---- 解构模式节点 ----

// 简单标识符模式（叶子节点）
struct IdentifierPattern {
    std::string name;
    SourceRange range;
};

// 数组模式中单个元素（含可选默认值）
struct ArrayPatternElement {
    std::unique_ptr<PatternNode> pattern;
    std::optional<std::unique_ptr<ExprNode>> default_value;
    SourceRange range;
};

// 数组解构模式 [a, b, ...rest]
struct ArrayPattern {
    // 每个位置：nullopt = elision hole；有值 = ArrayPatternElement
    std::vector<std::optional<ArrayPatternElement>> elements;
    std::unique_ptr<PatternNode> rest;  // nullptr = 无 rest
    SourceRange range;
};

// 对象模式中单个属性
struct ObjectPatternProperty {
    std::string key;                            // 静态键；computed=true 时为空
    std::unique_ptr<ExprNode> key_expr;         // computed=true 时非空，存放键表达式
    bool computed = false;
    std::unique_ptr<PatternNode> value_pattern;
    std::optional<std::unique_ptr<ExprNode>> default_value;
    SourceRange range;
};

// 对象解构模式 {a, b: renamed, ...rest}
struct ObjectPattern {
    std::vector<ObjectPatternProperty> properties;
    std::unique_ptr<PatternNode> rest;  // nullptr = 无 rest
    SourceRange range;
};

// PatternNode 完整定义（必须在 IdentifierPattern/ArrayPattern/ObjectPattern 之后）
struct PatternNode {
    std::variant<IdentifierPattern, ArrayPattern, ObjectPattern> v;
    PatternNode() = default;
    template <typename T>
        requires(!std::same_as<std::remove_cvref_t<T>, PatternNode>)
    explicit PatternNode(T&& node) : v(std::forward<T>(node)) {}
};

// 解构声明语句（let/const/var {pattern} = init）
struct DestructuringDeclaration {
    VarKind kind;
    std::unique_ptr<PatternNode> pattern;
    std::unique_ptr<ExprNode> init;  // var 无初始化器时可为 nullptr
    SourceRange range;
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
    std::vector<ParamDef> params;
    std::optional<std::string> rest_param;  // ...rest，nullopt 表示无 rest
    std::shared_ptr<std::vector<StmtNode>> body;
    SourceRange range;
    bool is_generator = false;  // true for function* declarations
};

// async 函数声明语句 async function name(params) { body }
struct AsyncFunctionDeclaration {
    std::string name;
    std::vector<ParamDef> params;
    std::optional<std::string> rest_param;  // ...rest，nullopt 表示无 rest
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

// for (var/let/const binding in right) body
// or  for (binding in right) body  (has_decl=false)
struct ForInStatement {
    bool has_decl;
    VarKind var_kind;          // valid when has_decl=true
    std::string binding;
    std::unique_ptr<ExprNode> right;
    std::unique_ptr<StmtNode> body;
    SourceRange range;
};

// for (var/let/const binding of right) body
// or  for (binding of right) body  (has_decl=false)
// pattern_binding != nullptr 时使用解构绑定（替代 binding 字段）
struct ForOfStatement {
    bool has_decl;
    VarKind var_kind;          // valid when has_decl=true
    std::string binding;       // 简单绑定名（pattern_binding 为 nullptr 时使用）
    std::unique_ptr<PatternNode> pattern_binding;  // 解构模式绑定（非 nullptr 时忽略 binding）
    std::unique_ptr<ExprNode> right;
    std::unique_ptr<StmtNode> body;
    SourceRange range;
};

struct ImportSpecifier {
    std::string imported_name;  // 模块内名称；默认导入时为 "default"
    std::string local_name;     // 本地绑定名
    bool is_namespace = false;  // true => import * as ns
    SourceRange range;
};

struct ImportDeclaration {
    std::string specifier;                   // 模块路径字符串
    std::vector<ImportSpecifier> specifiers; // 空 = 副作用导入
    SourceRange range;
};

struct ExportSpecifier {
    std::string local_name;
    std::string export_name;
    SourceRange range;
};

struct ExportNamedDeclaration {
    std::unique_ptr<StmtNode> declaration;  // 含声明时非空
    std::vector<ExportSpecifier> specifiers;
    std::optional<std::string> source;  // re-export 来源模块（如 "./a.js"），非空时为 re-export
    SourceRange range;
};

struct ExportDefaultDeclaration {
    std::unique_ptr<ExprNode> expression;
    std::optional<std::string> local_name;  // 具名 function/class 在模块作用域的绑定名
    SourceRange range;
};

// ---- StmtNode 完整定义（必须在所有语句 struct 定义之后）----

struct StmtNode {
    std::variant<ExpressionStatement, VariableDeclaration, BlockStatement, IfStatement, WhileStatement,
                 ReturnStatement, FunctionDeclaration, AsyncFunctionDeclaration,
                 ThrowStatement, TryStatement, BreakStatement, ContinueStatement,
                 LabeledStatement, ForStatement, ForInStatement, ForOfStatement,
                 ImportDeclaration, ExportNamedDeclaration, ExportDefaultDeclaration,
                 DestructuringDeclaration>
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
