#include "qppjs/frontend/parser.h"
#include "qppjs/frontend/ast.h"
#include <cmath>
#include <gtest/gtest.h>
#include <string>

using namespace qppjs;

// 测试辅助：将表达式字符串包装为语句，解析并返回 Program
static ParseResult<Program> parse_expr_src(const char* src) {
    return parse_program(std::string(src) + ";");
}

// 从 Program 的第一条语句中取出 ExprNode（ExpressionStatement），
// 断言结构正确，返回 ExprNode 的 const 引用（引用稳定，因 Program 仍在 result 中）
#define ASSERT_EXPR(result, expr_ref) \
    ASSERT_TRUE((result).ok()); \
    ASSERT_FALSE((result).value().body.empty()); \
    ASSERT_TRUE(std::holds_alternative<ExpressionStatement>((result).value().body[0].v)); \
    const ExprNode& expr_ref = std::get<ExpressionStatement>((result).value().body[0].v).expr

// ---- 阶段 A：原子表达式 ----

TEST(ParserAtom, IntegerLiteral) {
    auto result = parse_expr_src("42");
    ASSERT_EXPR(result, e);
    ASSERT_TRUE(std::holds_alternative<NumberLiteral>(e.v));
    EXPECT_DOUBLE_EQ(std::get<NumberLiteral>(e.v).value, 42.0);
}

TEST(ParserAtom, FloatLiteral) {
    auto result = parse_expr_src("3.14");
    ASSERT_EXPR(result, e);
    ASSERT_TRUE(std::holds_alternative<NumberLiteral>(e.v));
    EXPECT_DOUBLE_EQ(std::get<NumberLiteral>(e.v).value, 3.14);
}

TEST(ParserAtom, HexLiteral) {
    auto result = parse_expr_src("0xFF");
    ASSERT_EXPR(result, e);
    ASSERT_TRUE(std::holds_alternative<NumberLiteral>(e.v));
    EXPECT_DOUBLE_EQ(std::get<NumberLiteral>(e.v).value, 255.0);
}

TEST(ParserAtom, BinaryLiteral) {
    auto result = parse_expr_src("0b1010");
    ASSERT_EXPR(result, e);
    ASSERT_TRUE(std::holds_alternative<NumberLiteral>(e.v));
    EXPECT_DOUBLE_EQ(std::get<NumberLiteral>(e.v).value, 10.0);
}

TEST(ParserAtom, OctalLiteral) {
    auto result = parse_expr_src("0o755");
    ASSERT_EXPR(result, e);
    ASSERT_TRUE(std::holds_alternative<NumberLiteral>(e.v));
    EXPECT_DOUBLE_EQ(std::get<NumberLiteral>(e.v).value, 493.0);
}

TEST(ParserAtom, StringLiteralHello) {
    auto result = parse_expr_src("\"hello\"");
    ASSERT_EXPR(result, e);
    ASSERT_TRUE(std::holds_alternative<StringLiteral>(e.v));
    EXPECT_EQ(std::get<StringLiteral>(e.v).value, "hello");
}

TEST(ParserAtom, StringLiteralEscapeNewline) {
    auto result = parse_expr_src("\"\\n\"");
    ASSERT_EXPR(result, e);
    ASSERT_TRUE(std::holds_alternative<StringLiteral>(e.v));
    EXPECT_EQ(std::get<StringLiteral>(e.v).value, "\n");
}

TEST(ParserAtom, BooleanTrue) {
    auto result = parse_expr_src("true");
    ASSERT_EXPR(result, e);
    ASSERT_TRUE(std::holds_alternative<BooleanLiteral>(e.v));
    EXPECT_TRUE(std::get<BooleanLiteral>(e.v).value);
}

TEST(ParserAtom, BooleanFalse) {
    auto result = parse_expr_src("false");
    ASSERT_EXPR(result, e);
    ASSERT_TRUE(std::holds_alternative<BooleanLiteral>(e.v));
    EXPECT_FALSE(std::get<BooleanLiteral>(e.v).value);
}

TEST(ParserAtom, NullLiteralNode) {
    auto result = parse_expr_src("null");
    ASSERT_EXPR(result, e);
    EXPECT_TRUE(std::holds_alternative<NullLiteral>(e.v));
}

TEST(ParserAtom, UndefinedIsIdentifier) {
    auto result = parse_expr_src("undefined");
    ASSERT_EXPR(result, e);
    ASSERT_TRUE(std::holds_alternative<Identifier>(e.v));
    EXPECT_EQ(std::get<Identifier>(e.v).name, "undefined");
}

TEST(ParserAtom, Identifier) {
    auto result = parse_expr_src("x");
    ASSERT_EXPR(result, e);
    ASSERT_TRUE(std::holds_alternative<Identifier>(e.v));
    EXPECT_EQ(std::get<Identifier>(e.v).name, "x");
}

TEST(ParserAtom, Parenthesized) {
    auto result = parse_expr_src("(42)");
    ASSERT_EXPR(result, e);
    ASSERT_TRUE(std::holds_alternative<NumberLiteral>(e.v));
    EXPECT_DOUBLE_EQ(std::get<NumberLiteral>(e.v).value, 42.0);
}

// ---- 阶段 B：一元表达式 ----

TEST(ParserUnary, UnaryMinus) {
    auto result = parse_expr_src("-1");
    ASSERT_EXPR(result, e);
    ASSERT_TRUE(std::holds_alternative<UnaryExpression>(e.v));
    const auto& ue = std::get<UnaryExpression>(e.v);
    EXPECT_EQ(ue.op, UnaryOp::Minus);
    ASSERT_TRUE(std::holds_alternative<NumberLiteral>(ue.operand->v));
    EXPECT_DOUBLE_EQ(std::get<NumberLiteral>(ue.operand->v).value, 1.0);
}

TEST(ParserUnary, UnaryPlus) {
    auto result = parse_expr_src("+1");
    ASSERT_EXPR(result, e);
    ASSERT_TRUE(std::holds_alternative<UnaryExpression>(e.v));
    EXPECT_EQ(std::get<UnaryExpression>(e.v).op, UnaryOp::Plus);
}

TEST(ParserUnary, LogicalNot) {
    auto result = parse_expr_src("!false");
    ASSERT_EXPR(result, e);
    ASSERT_TRUE(std::holds_alternative<UnaryExpression>(e.v));
    const auto& ue = std::get<UnaryExpression>(e.v);
    EXPECT_EQ(ue.op, UnaryOp::Bang);
    ASSERT_TRUE(std::holds_alternative<BooleanLiteral>(ue.operand->v));
    EXPECT_FALSE(std::get<BooleanLiteral>(ue.operand->v).value);
}

TEST(ParserUnary, Typeof) {
    auto result = parse_expr_src("typeof x");
    ASSERT_EXPR(result, e);
    ASSERT_TRUE(std::holds_alternative<UnaryExpression>(e.v));
    const auto& ue = std::get<UnaryExpression>(e.v);
    EXPECT_EQ(ue.op, UnaryOp::Typeof);
    ASSERT_TRUE(std::holds_alternative<Identifier>(ue.operand->v));
    EXPECT_EQ(std::get<Identifier>(ue.operand->v).name, "x");
}

TEST(ParserUnary, Void) {
    auto result = parse_expr_src("void 0");
    ASSERT_EXPR(result, e);
    ASSERT_TRUE(std::holds_alternative<UnaryExpression>(e.v));
    const auto& ue = std::get<UnaryExpression>(e.v);
    EXPECT_EQ(ue.op, UnaryOp::Void);
    ASSERT_TRUE(std::holds_alternative<NumberLiteral>(ue.operand->v));
    EXPECT_DOUBLE_EQ(std::get<NumberLiteral>(ue.operand->v).value, 0.0);
}

// ---- 阶段 C：二元表达式与优先级 ----

TEST(ParserBinary, Add) {
    auto result = parse_expr_src("1+2");
    ASSERT_EXPR(result, e);
    ASSERT_TRUE(std::holds_alternative<BinaryExpression>(e.v));
    const auto& be = std::get<BinaryExpression>(e.v);
    EXPECT_EQ(be.op, BinaryOp::Add);
    EXPECT_DOUBLE_EQ(std::get<NumberLiteral>(be.left->v).value, 1.0);
    EXPECT_DOUBLE_EQ(std::get<NumberLiteral>(be.right->v).value, 2.0);
}

TEST(ParserBinary, AddMulPrecedence) {
    // 1+2*3 -> Add(1, Mul(2,3))
    auto result = parse_expr_src("1+2*3");
    ASSERT_EXPR(result, e);
    ASSERT_TRUE(std::holds_alternative<BinaryExpression>(e.v));
    const auto& be = std::get<BinaryExpression>(e.v);
    EXPECT_EQ(be.op, BinaryOp::Add);
    EXPECT_DOUBLE_EQ(std::get<NumberLiteral>(be.left->v).value, 1.0);
    ASSERT_TRUE(std::holds_alternative<BinaryExpression>(be.right->v));
    const auto& rbe = std::get<BinaryExpression>(be.right->v);
    EXPECT_EQ(rbe.op, BinaryOp::Mul);
    EXPECT_DOUBLE_EQ(std::get<NumberLiteral>(rbe.left->v).value, 2.0);
    EXPECT_DOUBLE_EQ(std::get<NumberLiteral>(rbe.right->v).value, 3.0);
}

TEST(ParserBinary, MulAddPrecedence) {
    // 1*2+3 -> Add(Mul(1,2), 3)
    auto result = parse_expr_src("1*2+3");
    ASSERT_EXPR(result, e);
    ASSERT_TRUE(std::holds_alternative<BinaryExpression>(e.v));
    const auto& be = std::get<BinaryExpression>(e.v);
    EXPECT_EQ(be.op, BinaryOp::Add);
    ASSERT_TRUE(std::holds_alternative<BinaryExpression>(be.left->v));
    const auto& lbe = std::get<BinaryExpression>(be.left->v);
    EXPECT_EQ(lbe.op, BinaryOp::Mul);
    EXPECT_DOUBLE_EQ(std::get<NumberLiteral>(be.right->v).value, 3.0);
}

TEST(ParserBinary, LeftAssociativity) {
    // 1-2-3 -> Sub(Sub(1,2), 3)
    auto result = parse_expr_src("1-2-3");
    ASSERT_EXPR(result, e);
    ASSERT_TRUE(std::holds_alternative<BinaryExpression>(e.v));
    const auto& be = std::get<BinaryExpression>(e.v);
    EXPECT_EQ(be.op, BinaryOp::Sub);
    ASSERT_TRUE(std::holds_alternative<BinaryExpression>(be.left->v));
    const auto& lbe = std::get<BinaryExpression>(be.left->v);
    EXPECT_EQ(lbe.op, BinaryOp::Sub);
    EXPECT_DOUBLE_EQ(std::get<NumberLiteral>(be.right->v).value, 3.0);
}

TEST(ParserBinary, LessThan) {
    auto result = parse_expr_src("1<2");
    ASSERT_EXPR(result, e);
    ASSERT_TRUE(std::holds_alternative<BinaryExpression>(e.v));
    EXPECT_EQ(std::get<BinaryExpression>(e.v).op, BinaryOp::Lt);
}

TEST(ParserBinary, EqEq) {
    auto result = parse_expr_src("1==2");
    ASSERT_EXPR(result, e);
    ASSERT_TRUE(std::holds_alternative<BinaryExpression>(e.v));
    EXPECT_EQ(std::get<BinaryExpression>(e.v).op, BinaryOp::EqEq);
}

TEST(ParserBinary, EqEqEq) {
    auto result = parse_expr_src("1===2");
    ASSERT_EXPR(result, e);
    ASSERT_TRUE(std::holds_alternative<BinaryExpression>(e.v));
    EXPECT_EQ(std::get<BinaryExpression>(e.v).op, BinaryOp::EqEqEq);
}

// ---- 阶段 D：逻辑表达式 ----

TEST(ParserLogical, And) {
    auto result = parse_expr_src("a&&b");
    ASSERT_EXPR(result, e);
    ASSERT_TRUE(std::holds_alternative<LogicalExpression>(e.v));
    const auto& le = std::get<LogicalExpression>(e.v);
    EXPECT_EQ(le.op, LogicalOp::And);
    EXPECT_EQ(std::get<Identifier>(le.left->v).name, "a");
    EXPECT_EQ(std::get<Identifier>(le.right->v).name, "b");
}

TEST(ParserLogical, Or) {
    auto result = parse_expr_src("a||b");
    ASSERT_EXPR(result, e);
    ASSERT_TRUE(std::holds_alternative<LogicalExpression>(e.v));
    EXPECT_EQ(std::get<LogicalExpression>(e.v).op, LogicalOp::Or);
}

TEST(ParserLogical, OrAndPrecedence) {
    // a||b&&c -> Or(a, And(b, c))
    auto result = parse_expr_src("a||b&&c");
    ASSERT_EXPR(result, e);
    ASSERT_TRUE(std::holds_alternative<LogicalExpression>(e.v));
    const auto& le = std::get<LogicalExpression>(e.v);
    EXPECT_EQ(le.op, LogicalOp::Or);
    EXPECT_EQ(std::get<Identifier>(le.left->v).name, "a");
    ASSERT_TRUE(std::holds_alternative<LogicalExpression>(le.right->v));
    const auto& rle = std::get<LogicalExpression>(le.right->v);
    EXPECT_EQ(rle.op, LogicalOp::And);
    EXPECT_EQ(std::get<Identifier>(rle.left->v).name, "b");
    EXPECT_EQ(std::get<Identifier>(rle.right->v).name, "c");
}

// ---- 阶段 E：赋值表达式 ----

TEST(ParserAssign, SimpleAssign) {
    auto result = parse_expr_src("x=1");
    ASSERT_EXPR(result, e);
    ASSERT_TRUE(std::holds_alternative<AssignmentExpression>(e.v));
    const auto& ae = std::get<AssignmentExpression>(e.v);
    EXPECT_EQ(ae.op, AssignOp::Assign);
    EXPECT_EQ(ae.target, "x");
    ASSERT_TRUE(std::holds_alternative<NumberLiteral>(ae.value->v));
    EXPECT_DOUBLE_EQ(std::get<NumberLiteral>(ae.value->v).value, 1.0);
}

TEST(ParserAssign, RightAssociativity) {
    // x=y=1 -> Assign(x, Assign(y, 1))
    auto result = parse_expr_src("x=y=1");
    ASSERT_EXPR(result, e);
    ASSERT_TRUE(std::holds_alternative<AssignmentExpression>(e.v));
    const auto& ae = std::get<AssignmentExpression>(e.v);
    EXPECT_EQ(ae.target, "x");
    ASSERT_TRUE(std::holds_alternative<AssignmentExpression>(ae.value->v));
    const auto& inner = std::get<AssignmentExpression>(ae.value->v);
    EXPECT_EQ(inner.target, "y");
    EXPECT_DOUBLE_EQ(std::get<NumberLiteral>(inner.value->v).value, 1.0);
}

TEST(ParserAssign, AddAssign) {
    auto result = parse_expr_src("x+=1");
    ASSERT_EXPR(result, e);
    ASSERT_TRUE(std::holds_alternative<AssignmentExpression>(e.v));
    EXPECT_EQ(std::get<AssignmentExpression>(e.v).op, AssignOp::AddAssign);
}

TEST(ParserAssign, InvalidLHS) {
    // 1=x 应该报错
    auto result = parse_program("1=x;");
    EXPECT_FALSE(result.ok());
}

// ---- 阶段 F：语句 ----

TEST(ParserStmt, ExpressionStatement) {
    auto result = parse_program("1+2;");
    ASSERT_TRUE(result.ok());
    const auto& body = result.value().body;
    ASSERT_EQ(body.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<ExpressionStatement>(body[0].v));
    const auto& es = std::get<ExpressionStatement>(body[0].v);
    ASSERT_TRUE(std::holds_alternative<BinaryExpression>(es.expr.v));
    EXPECT_EQ(std::get<BinaryExpression>(es.expr.v).op, BinaryOp::Add);
}

TEST(ParserStmt, LetNoInit) {
    auto result = parse_program("let x;");
    ASSERT_TRUE(result.ok());
    const auto& body = result.value().body;
    ASSERT_EQ(body.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<VariableDeclaration>(body[0].v));
    const auto& vd = std::get<VariableDeclaration>(body[0].v);
    EXPECT_EQ(vd.kind, VarKind::Let);
    EXPECT_EQ(vd.name, "x");
    EXPECT_FALSE(vd.init.has_value());
}

TEST(ParserStmt, LetWithInit) {
    auto result = parse_program("let x=1;");
    ASSERT_TRUE(result.ok());
    const auto& body = result.value().body;
    ASSERT_EQ(body.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<VariableDeclaration>(body[0].v));
    const auto& vd = std::get<VariableDeclaration>(body[0].v);
    EXPECT_EQ(vd.kind, VarKind::Let);
    EXPECT_EQ(vd.name, "x");
    ASSERT_TRUE(vd.init.has_value());
    EXPECT_DOUBLE_EQ(std::get<NumberLiteral>(vd.init->v).value, 1.0);
}

TEST(ParserStmt, ConstWithInit) {
    auto result = parse_program("const x=1;");
    ASSERT_TRUE(result.ok());
    const auto& body = result.value().body;
    ASSERT_EQ(body.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<VariableDeclaration>(body[0].v));
    EXPECT_EQ(std::get<VariableDeclaration>(body[0].v).kind, VarKind::Const);
}

TEST(ParserStmt, ConstWithoutInit) {
    auto result = parse_program("const x;");
    EXPECT_FALSE(result.ok());
}

TEST(ParserStmt, VarWithInit) {
    auto result = parse_program("var x=2;");
    ASSERT_TRUE(result.ok());
    const auto& body = result.value().body;
    ASSERT_EQ(body.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<VariableDeclaration>(body[0].v));
    const auto& vd = std::get<VariableDeclaration>(body[0].v);
    EXPECT_EQ(vd.kind, VarKind::Var);
    EXPECT_EQ(vd.name, "x");
    ASSERT_TRUE(vd.init.has_value());
    EXPECT_DOUBLE_EQ(std::get<NumberLiteral>(vd.init->v).value, 2.0);
}

TEST(ParserStmt, EmptyBlock) {
    auto result = parse_program("{}");
    ASSERT_TRUE(result.ok());
    const auto& body = result.value().body;
    ASSERT_EQ(body.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<BlockStatement>(body[0].v));
    EXPECT_TRUE(std::get<BlockStatement>(body[0].v).body.empty());
}

TEST(ParserStmt, BlockWithDecl) {
    auto result = parse_program("{let x=1;}");
    ASSERT_TRUE(result.ok());
    const auto& body = result.value().body;
    ASSERT_EQ(body.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<BlockStatement>(body[0].v));
    const auto& bs = std::get<BlockStatement>(body[0].v);
    ASSERT_EQ(bs.body.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<VariableDeclaration>(bs.body[0].v));
}

TEST(ParserStmt, IfNoElse) {
    auto result = parse_program("if(true){}");
    ASSERT_TRUE(result.ok());
    const auto& body = result.value().body;
    ASSERT_EQ(body.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<IfStatement>(body[0].v));
    const auto& is = std::get<IfStatement>(body[0].v);
    ASSERT_TRUE(std::holds_alternative<BooleanLiteral>(is.test.v));
    EXPECT_TRUE(std::get<BooleanLiteral>(is.test.v).value);
    EXPECT_TRUE(std::holds_alternative<BlockStatement>(is.consequent->v));
    EXPECT_TRUE(is.alternate == nullptr);
}

TEST(ParserStmt, IfElse) {
    auto result = parse_program("if(a){}else{}");
    ASSERT_TRUE(result.ok());
    const auto& body = result.value().body;
    ASSERT_EQ(body.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<IfStatement>(body[0].v));
    const auto& is = std::get<IfStatement>(body[0].v);
    EXPECT_TRUE(is.consequent != nullptr);
    EXPECT_TRUE(is.alternate != nullptr);
    EXPECT_TRUE(std::holds_alternative<BlockStatement>(is.alternate->v));
}

TEST(ParserStmt, WhileLoop) {
    auto result = parse_program("while(true){}");
    ASSERT_TRUE(result.ok());
    const auto& body = result.value().body;
    ASSERT_EQ(body.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<WhileStatement>(body[0].v));
    const auto& ws = std::get<WhileStatement>(body[0].v);
    ASSERT_TRUE(std::holds_alternative<BooleanLiteral>(ws.test.v));
    EXPECT_TRUE(std::holds_alternative<BlockStatement>(ws.body->v));
}

TEST(ParserStmt, TopLevelReturnEmpty) {
    auto result = parse_program("return;");
    EXPECT_FALSE(result.ok());
}

TEST(ParserStmt, TopLevelReturnValue) {
    auto result = parse_program("return 1;");
    EXPECT_FALSE(result.ok());
}

// ---- 阶段 G：分号自动插入 ----

TEST(ParserASI, NewlineTriggersASI) {
    auto result = parse_program("1+2\n3+4");
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value().body.size(), 2u);
    EXPECT_TRUE(std::holds_alternative<ExpressionStatement>(result.value().body[0].v));
    EXPECT_TRUE(std::holds_alternative<ExpressionStatement>(result.value().body[1].v));
}

TEST(ParserASI, NewlineBetweenDecls) {
    auto result = parse_program("let x\nlet y");
    ASSERT_TRUE(result.ok());
    ASSERT_EQ(result.value().body.size(), 2u);
    EXPECT_TRUE(std::holds_alternative<VariableDeclaration>(result.value().body[0].v));
    EXPECT_TRUE(std::holds_alternative<VariableDeclaration>(result.value().body[1].v));
}

// ---- 阶段 H：错误消息包含位置信息 ----

// ---- 数字超范围 ----

TEST(ParserAtom, OverflowDecimalIsInfinity) {
    // 1e309 超出 double 范围，应解析为 Infinity 而不是崩溃
    auto result = parse_expr_src("1e309");
    ASSERT_EXPR(result, e);
    ASSERT_TRUE(std::holds_alternative<NumberLiteral>(e.v));
    EXPECT_TRUE(std::isinf(std::get<NumberLiteral>(e.v).value));
}

TEST(ParserAtom, LargeHexIsFinite) {
    // 0x10000000000000000 (2^64) 超出 uint64 但仍是有限 double，不应变成 Infinity
    auto result = parse_expr_src("0x10000000000000000");
    ASSERT_EXPR(result, e);
    ASSERT_TRUE(std::holds_alternative<NumberLiteral>(e.v));
    double v = std::get<NumberLiteral>(e.v).value;
    EXPECT_TRUE(std::isfinite(v));
    EXPECT_DOUBLE_EQ(v, 1.8446744073709552e19);
}

// ---- undefined 可用于绑定位置 ----

TEST(ParserStmt, UndefinedAsBindingName) {
    // undefined 不是保留字，let undefined = 1; 应合法解析
    auto result = parse_program("let undefined = 1;");
    ASSERT_TRUE(result.ok());
    ASSERT_EQ(result.value().body.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<VariableDeclaration>(result.value().body[0].v));
    const auto& decl = std::get<VariableDeclaration>(result.value().body[0].v);
    EXPECT_EQ(decl.name, "undefined");
}

// ---- 字符串 LineContinuation 解码 ----

TEST(ParserAtom, StringLineContinuationLfDecoded) {
    // "a\<LF>b" 应解码为 "ab"
    auto result = parse_expr_src("\"a\\\nb\"");
    ASSERT_EXPR(result, e);
    ASSERT_TRUE(std::holds_alternative<StringLiteral>(e.v));
    EXPECT_EQ(std::get<StringLiteral>(e.v).value, "ab");
}

TEST(ParserAtom, StringLineContinuationCrLfDecoded) {
    // "a\<CR><LF>b" 应解码为 "ab"
    auto result = parse_expr_src("\"a\\\r\nb\"");
    ASSERT_EXPR(result, e);
    ASSERT_TRUE(std::holds_alternative<StringLiteral>(e.v));
    EXPECT_EQ(std::get<StringLiteral>(e.v).value, "ab");
}

TEST(ParserAtom, SurrogatePairDecoded) {
    // "😀" 是 😀 (U+1F600)，应解码为 4 字节 UTF-8
    auto result = parse_expr_src("\"\\uD83D\\uDE00\"");
    ASSERT_EXPR(result, e);
    ASSERT_TRUE(std::holds_alternative<StringLiteral>(e.v));
    // U+1F600 的 UTF-8 编码：F0 9F 98 80
    const std::string expected = "\xF0\x9F\x98\x80";
    EXPECT_EQ(std::get<StringLiteral>(e.v).value, expected);
    EXPECT_EQ(std::get<StringLiteral>(e.v).value.size(), 4u);
}

TEST(ParserAtom, LoneSurrogateWtf8) {
    // 孤立高代理 \uD800 采用 WTF-8 编码（3 字节），不崩溃、不产生 UB
    auto result = parse_expr_src("\"\\uD800\"");
    ASSERT_EXPR(result, e);
    ASSERT_TRUE(std::holds_alternative<StringLiteral>(e.v));
    // WTF-8 编码 U+D800：ED A0 80
    const std::string expected = "\xED\xA0\x80";
    EXPECT_EQ(std::get<StringLiteral>(e.v).value, expected);
    EXPECT_EQ(std::get<StringLiteral>(e.v).value.size(), 3u);
}

// ---- 阶段 I：source range 正确性 ----

TEST(ParserRange, UnaryExprFullSpan) {
    // "-1" offset=0 length=2
    auto result = parse_expr_src("-1");
    ASSERT_EXPR(result, e);
    const auto& ue = std::get<UnaryExpression>(e.v);
    EXPECT_EQ(ue.range.offset, 0u);
    EXPECT_EQ(ue.range.length, 2u);
}

TEST(ParserRange, BinaryExprFullSpan) {
    // "1+2" offset=0 length=3
    auto result = parse_expr_src("1+2");
    ASSERT_EXPR(result, e);
    const auto& be = std::get<BinaryExpression>(e.v);
    EXPECT_EQ(be.range.offset, 0u);
    EXPECT_EQ(be.range.length, 3u);
}

TEST(ParserRange, AssignmentExprFullSpan) {
    // "x=1" offset=0 length=3
    auto result = parse_expr_src("x=1");
    ASSERT_EXPR(result, e);
    const auto& ae = std::get<AssignmentExpression>(e.v);
    EXPECT_EQ(ae.range.offset, 0u);
    EXPECT_EQ(ae.range.length, 3u);
}

TEST(ParserRange, VarDeclFullSpan) {
    // "let x = 1;" offset=0 length=10
    auto result = parse_program("let x = 1;");
    ASSERT_TRUE(result.ok());
    const auto& decl = std::get<VariableDeclaration>(result.value().body[0].v);
    EXPECT_EQ(decl.range.offset, 0u);
    EXPECT_EQ(decl.range.length, 10u);
}

TEST(ParserRange, IfStmtFullSpan) {
    // "if(x){}" offset=0 length=7
    auto result = parse_program("if(x){}");
    ASSERT_TRUE(result.ok());
    const auto& is = std::get<IfStatement>(result.value().body[0].v);
    EXPECT_EQ(is.range.offset, 0u);
    EXPECT_EQ(is.range.length, 7u);
}

TEST(ParserErrorTest, ConstNoInitHasLocation) {
    auto result = parse_program("const x;");
    ASSERT_FALSE(result.ok());
    EXPECT_NE(result.error().message().find("line 1"), std::string::npos);
    EXPECT_NE(result.error().message().find("column"), std::string::npos);
}

TEST(ParserErrorTest, InvalidAssignTargetHasLocation) {
    auto result = parse_program("1 = x;");
    ASSERT_FALSE(result.ok());
    EXPECT_NE(result.error().message().find("line 1"), std::string::npos);
}

TEST(ParserErrorTest, TopLevelReturnHasLocation) {
    auto result = parse_program("return 1;");
    ASSERT_FALSE(result.ok());
    EXPECT_NE(result.error().message().find("line 1"), std::string::npos);
}

TEST(ParserErrorTest, MultilineErrorShowsCorrectLine) {
    auto result = parse_program("let x = 1;\nconst y;");
    ASSERT_FALSE(result.ok());
    EXPECT_NE(result.error().message().find("line 2"), std::string::npos);
}

TEST(ParserErrorTest, ErrorMessageContainsDescription) {
    auto result = parse_program("const x;");
    ASSERT_FALSE(result.ok());
    EXPECT_FALSE(result.error().message().empty());
    EXPECT_GT(result.error().message().size(), 20u);
}
