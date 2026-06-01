#include "qppjs/frontend/parser.h"

#include "qppjs/frontend/ast.h"
#include "qppjs/frontend/ast_dump.h"
#include "qppjs/frontend/lexer.h"
#include "qppjs/runtime/interpreter.h"
#include "qppjs/vm/compiler.h"
#include "qppjs/vm/vm.h"

#include <gtest/gtest.h>
#include <string>

using namespace qppjs;

// ---- 辅助 ----

static ParseResult<Program> parse_src(const char* src) { return parse_program(std::string(src)); }

#define ASSERT_EXPR(result, expr_ref)                                                     \
    ASSERT_TRUE((result).ok()) << (result).error().message();                             \
    ASSERT_FALSE((result).value().body.empty());                                          \
    ASSERT_TRUE(std::holds_alternative<ExpressionStatement>((result).value().body[0].v)); \
    const ExprNode& expr_ref = std::get<ExpressionStatement>((result).value().body[0].v).expr

static Value interp_eval(const char* src) {
    auto parse_result = parse_program(std::string(src));
    if (!parse_result.ok()) return Value::undefined();
    Interpreter interp;
    auto result = interp.exec(parse_result.value());
    if (!result.is_ok()) return Value::undefined();
    return result.value();
}

static Value vm_eval(const char* src) {
    auto parse_result = parse_program(std::string(src));
    if (!parse_result.ok()) return Value::undefined();
    Compiler compiler;
    auto bytecode = compiler.compile(parse_result.value());
    if (!bytecode) return Value::undefined();
    VM vm;
    auto result = vm.exec(bytecode);
    if (!result.is_ok()) return Value::undefined();
    return result.value();
}

// ============================================================
// Lexer 测试
// ============================================================

TEST(TemplateLiteralLexer, NoSubstitution) {
    LexerState state = lexer_init("`hello`");
    Token tok = next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::TemplateNoSub);
    EXPECT_EQ(tok.range.offset, 0u);
    EXPECT_EQ(tok.range.length, 7u);  // `hello`
}

TEST(TemplateLiteralLexer, WithSubstitution) {
    LexerState state = lexer_init("`hello ${");
    Token tok = next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::TemplateHead);
    EXPECT_EQ(tok.range.length, 9u);  // `hello ${
}

TEST(TemplateLiteralLexer, TemplateTail) {
    // 模拟 } 开始的末尾段
    LexerState state = lexer_init("} world`");
    Token tok = scan_template_part(state);
    EXPECT_EQ(tok.kind, TokenKind::TemplateTail);
}

TEST(TemplateLiteralLexer, TemplateMiddle) {
    LexerState state = lexer_init("} middle ${");
    Token tok = scan_template_part(state);
    EXPECT_EQ(tok.kind, TokenKind::TemplateMiddle);
}

TEST(TemplateLiteralLexer, Multiline) {
    LexerState state = lexer_init("`line1\nline2`");
    Token tok = next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::TemplateNoSub);
}

TEST(TemplateLiteralLexer, Unclosed) {
    LexerState state = lexer_init("`unclosed");
    Token tok = next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::Invalid);
}

TEST(TemplateLiteralLexer, EscapeInTemplate) {
    LexerState state = lexer_init("`\\n`");
    Token tok = next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::TemplateNoSub);
}

// ============================================================
// Parser 测试
// ============================================================

// TL-01: 无插值模板字符串
TEST(TemplateLiteralParser, TL01_NoSub) {
    auto result = parse_src("`hello`");
    ASSERT_EXPR(result, e);
    ASSERT_TRUE(std::holds_alternative<TemplateLiteral>(e.v));
    const auto& tl = std::get<TemplateLiteral>(e.v);
    EXPECT_EQ(tl.quasis.size(), 1u);
    EXPECT_EQ(tl.expressions.size(), 0u);
    EXPECT_EQ(tl.quasis[0].cooked, "hello");
}

// TL-02: 空模板字符串
TEST(TemplateLiteralParser, TL02_Empty) {
    auto result = parse_src("``");
    ASSERT_EXPR(result, e);
    ASSERT_TRUE(std::holds_alternative<TemplateLiteral>(e.v));
    const auto& tl = std::get<TemplateLiteral>(e.v);
    EXPECT_EQ(tl.quasis[0].cooked, "");
}

// TL-03: 单插值
TEST(TemplateLiteralParser, TL03_SingleExpr) {
    auto result = parse_src("`hello ${name}`");
    ASSERT_EXPR(result, e);
    ASSERT_TRUE(std::holds_alternative<TemplateLiteral>(e.v));
    const auto& tl = std::get<TemplateLiteral>(e.v);
    EXPECT_EQ(tl.quasis.size(), 2u);
    EXPECT_EQ(tl.expressions.size(), 1u);
    EXPECT_EQ(tl.quasis[0].cooked, "hello ");
    EXPECT_EQ(tl.quasis[1].cooked, "");
}

// TL-04: 多插值
TEST(TemplateLiteralParser, TL04_MultiExpr) {
    auto result = parse_src("`${a} + ${b} = ${c}`");
    ASSERT_EXPR(result, e);
    ASSERT_TRUE(std::holds_alternative<TemplateLiteral>(e.v));
    const auto& tl = std::get<TemplateLiteral>(e.v);
    EXPECT_EQ(tl.quasis.size(), 4u);
    EXPECT_EQ(tl.expressions.size(), 3u);
    EXPECT_EQ(tl.quasis[0].cooked, "");
    EXPECT_EQ(tl.quasis[1].cooked, " + ");
    EXPECT_EQ(tl.quasis[2].cooked, " = ");
    EXPECT_EQ(tl.quasis[3].cooked, "");
}

// TL-05: 转义序列解码
TEST(TemplateLiteralParser, TL05_EscapeSequences) {
    auto result = parse_src("`\\n\\t\\\\`");
    ASSERT_EXPR(result, e);
    const auto& tl = std::get<TemplateLiteral>(e.v);
    EXPECT_EQ(tl.quasis[0].cooked, "\n\t\\");
}

// TL-06: 多行模板字符串
TEST(TemplateLiteralParser, TL06_Multiline) {
    auto result = parse_src("`line1\nline2`");
    ASSERT_EXPR(result, e);
    const auto& tl = std::get<TemplateLiteral>(e.v);
    EXPECT_EQ(tl.quasis[0].cooked, "line1\nline2");
}

// TL-07: \r\n 规范化为 \n
TEST(TemplateLiteralParser, TL07_CRLFNormalization) {
    auto result = parse_src("`line1\r\nline2`");
    ASSERT_EXPR(result, e);
    const auto& tl = std::get<TemplateLiteral>(e.v);
    EXPECT_EQ(tl.quasis[0].cooked, "line1\nline2");
}

// TL-08: 插值中有表达式
TEST(TemplateLiteralParser, TL08_ExprInInterp) {
    auto result = parse_src("`result: ${1 + 2}`");
    ASSERT_EXPR(result, e);
    const auto& tl = std::get<TemplateLiteral>(e.v);
    EXPECT_EQ(tl.quasis.size(), 2u);
    EXPECT_EQ(tl.expressions.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<BinaryExpression>(tl.expressions[0]->v));
}

// TL-09: AST Dump
TEST(TemplateLiteralDump, TL09_NoSub) {
    auto result = parse_src("`hello`");
    ASSERT_TRUE(result.ok());
    auto dump = dump_stmt(result.value().body[0], 0);
    EXPECT_NE(dump.find("TemplateLiteral"), std::string::npos);
    EXPECT_NE(dump.find("quasi[0]"), std::string::npos);
    EXPECT_NE(dump.find("\"hello\""), std::string::npos);
}

// TL-10: AST Dump with interpolation
TEST(TemplateLiteralDump, TL10_WithExpr) {
    auto result = parse_src("`${x}`");
    ASSERT_TRUE(result.ok());
    auto dump = dump_stmt(result.value().body[0], 0);
    EXPECT_NE(dump.find("TemplateLiteral"), std::string::npos);
    EXPECT_NE(dump.find("expr[0]"), std::string::npos);
}

// ============================================================
// Interpreter 测试
// ============================================================

// TL-11: 无插值（Interpreter）
TEST(TemplateLiteralInterp, TL11_NoSub) {
    auto v = interp_eval("`hello world`");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "hello world");
}

// TL-12: 空模板（Interpreter）
TEST(TemplateLiteralInterp, TL12_Empty) {
    auto v = interp_eval("``");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "");
}

// TL-13: 数字插值（Interpreter）
TEST(TemplateLiteralInterp, TL13_NumberInterp) {
    auto v = interp_eval("let x = 42; `value: ${x}`");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "value: 42");
}

// TL-14: 字符串插值（Interpreter）
TEST(TemplateLiteralInterp, TL14_StringInterp) {
    auto v = interp_eval("let name = 'world'; `hello ${name}`");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "hello world");
}

// TL-15: 表达式插值（Interpreter）
TEST(TemplateLiteralInterp, TL15_ExprInterp) {
    auto v = interp_eval("`${1 + 2}`");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "3");
}

// TL-16: 多插值（Interpreter）
TEST(TemplateLiteralInterp, TL16_MultiInterp) {
    auto v = interp_eval("let a = 1; let b = 2; `${a} + ${b} = ${a + b}`");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "1 + 2 = 3");
}

// TL-17: 多行（Interpreter）
TEST(TemplateLiteralInterp, TL17_Multiline) {
    auto v = interp_eval("`line1\nline2`");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "line1\nline2");
}

// TL-18: 转义序列（Interpreter）
TEST(TemplateLiteralInterp, TL18_Escape) {
    auto v = interp_eval("`\\n\\t`");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "\n\t");
}

// TL-19: boolean 插值（Interpreter）
TEST(TemplateLiteralInterp, TL19_BoolInterp) {
    auto v = interp_eval("`${true}`");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "true");
}

// TL-20: null 插值（Interpreter）
TEST(TemplateLiteralInterp, TL20_NullInterp) {
    auto v = interp_eval("`${null}`");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "null");
}

// TL-21: undefined 插值（Interpreter）
TEST(TemplateLiteralInterp, TL21_UndefinedInterp) {
    auto v = interp_eval("`${undefined}`");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "undefined");
}

// TL-22: 嵌套模板（Interpreter）
TEST(TemplateLiteralInterp, TL22_Nested) {
    auto v = interp_eval("`outer ${ `inner` }`");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "outer inner");
}

// TL-23: 反引号转义（Interpreter）
TEST(TemplateLiteralInterp, TL23_BacktickEscape) {
    auto v = interp_eval("`a\\`b`");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "a`b");
}

// TL-24: $ 转义（Interpreter）
TEST(TemplateLiteralInterp, TL24_DollarEscape) {
    auto v = interp_eval("`\\${not interpolated}`");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "${not interpolated}");
}

// ============================================================
// VM 测试（对称）
// ============================================================

// TL-11: 无插值（VM）
TEST(TemplateLiteralVM, TL11_NoSub) {
    auto v = vm_eval("`hello world`");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "hello world");
}

// TL-12: 空模板（VM）
TEST(TemplateLiteralVM, TL12_Empty) {
    auto v = vm_eval("``");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "");
}

// TL-13: 数字插值（VM）
TEST(TemplateLiteralVM, TL13_NumberInterp) {
    auto v = vm_eval("let x = 42; `value: ${x}`");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "value: 42");
}

// TL-14: 字符串插值（VM）
TEST(TemplateLiteralVM, TL14_StringInterp) {
    auto v = vm_eval("let name = 'world'; `hello ${name}`");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "hello world");
}

// TL-15: 表达式插值（VM）
TEST(TemplateLiteralVM, TL15_ExprInterp) {
    auto v = vm_eval("`${1 + 2}`");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "3");
}

// TL-16: 多插值（VM）
TEST(TemplateLiteralVM, TL16_MultiInterp) {
    auto v = vm_eval("let a = 1; let b = 2; `${a} + ${b} = ${a + b}`");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "1 + 2 = 3");
}

// TL-17: 多行（VM）
TEST(TemplateLiteralVM, TL17_Multiline) {
    auto v = vm_eval("`line1\nline2`");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "line1\nline2");
}

// TL-18: 转义序列（VM）
TEST(TemplateLiteralVM, TL18_Escape) {
    auto v = vm_eval("`\\n\\t`");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "\n\t");
}

// TL-19: boolean 插值（VM）
TEST(TemplateLiteralVM, TL19_BoolInterp) {
    auto v = vm_eval("`${true}`");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "true");
}

// TL-20: null 插值（VM）
TEST(TemplateLiteralVM, TL20_NullInterp) {
    auto v = vm_eval("`${null}`");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "null");
}

// TL-21: undefined 插值（VM）
TEST(TemplateLiteralVM, TL21_UndefinedInterp) {
    auto v = vm_eval("`${undefined}`");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "undefined");
}

// TL-22: 嵌套模板（VM）
TEST(TemplateLiteralVM, TL22_Nested) {
    auto v = vm_eval("`outer ${ `inner` }`");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "outer inner");
}

// TL-23: 反引号转义（VM）
TEST(TemplateLiteralVM, TL23_BacktickEscape) {
    auto v = vm_eval("`a\\`b`");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "a`b");
}

// TL-24: $ 转义（VM）
TEST(TemplateLiteralVM, TL24_DollarEscape) {
    auto v = vm_eval("`\\${not interpolated}`");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "${not interpolated}");
}

// ============================================================
// TL-25～ 边界测试、错误路径、回归风险点
// ============================================================

// ---- ToString 边界：特殊数值 ----

// TL-25: -0 插值应输出 "0"（Interpreter）
TEST(TemplateLiteralInterp, TL25_NegZeroInterp) {
    auto v = interp_eval("`${-0}`");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "0");
}

// TL-25: -0 插值应输出 "0"（VM）
TEST(TemplateLiteralVM, TL25_NegZeroInterp) {
    auto v = vm_eval("`${-0}`");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "0");
}

// TL-26: NaN 插值应输出 "NaN"（Interpreter）
TEST(TemplateLiteralInterp, TL26_NaNInterp) {
    auto v = interp_eval("`${NaN}`");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "NaN");
}

// TL-26: NaN 插值应输出 "NaN"（VM）
TEST(TemplateLiteralVM, TL26_NaNInterp) {
    auto v = vm_eval("`${NaN}`");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "NaN");
}

// TL-27: Infinity 插值应输出 "Infinity"（Interpreter）
TEST(TemplateLiteralInterp, TL27_InfinityInterp) {
    auto v = interp_eval("`${Infinity}`");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "Infinity");
}

// TL-27: Infinity 插值应输出 "Infinity"（VM）
TEST(TemplateLiteralVM, TL27_InfinityInterp) {
    auto v = vm_eval("`${Infinity}`");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "Infinity");
}

// TL-28: -Infinity 插值应输出 "-Infinity"（Interpreter）
TEST(TemplateLiteralInterp, TL28_NegInfinityInterp) {
    auto v = interp_eval("`${-Infinity}`");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "-Infinity");
}

// TL-28: -Infinity 插值应输出 "-Infinity"（VM）
TEST(TemplateLiteralVM, TL28_NegInfinityInterp) {
    auto v = vm_eval("`${-Infinity}`");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "-Infinity");
}

// TL-29: 浮点数插值（Interpreter）
TEST(TemplateLiteralInterp, TL29_FloatInterp) {
    auto v = interp_eval("`${1.5}`");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "1.5");
}

// TL-29: 浮点数插值（VM）
TEST(TemplateLiteralVM, TL29_FloatInterp) {
    auto v = vm_eval("`${1.5}`");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "1.5");
}

// ---- 对象 toString ----

// TL-30: 普通对象插值输出 "[object Object]"（Interpreter）
TEST(TemplateLiteralInterp, TL30_PlainObjectInterp) {
    auto v = interp_eval("`${{a:1}}`");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "[object Object]");
}

// TL-30: 普通对象插值输出 "[object Object]"（VM）
TEST(TemplateLiteralVM, TL30_PlainObjectInterp) {
    auto v = vm_eval("`${{a:1}}`");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "[object Object]");
}

// ---- 数组插值 ----

// TL-31: 数组插值（Interpreter）
// 规范预期：`${[1,2,3]}` → "1,2,3"（Array.prototype.toString 调用 join）
// 当前实现：to_string_val 对数组（对象）返回 "[object Object]"，是已知实现限制
TEST(TemplateLiteralInterp, TL31_ArrayInterp) {
    auto v = interp_eval("`${[1,2,3]}`");
    EXPECT_TRUE(v.is_string());
    // 当前实现不调用 Array.prototype.toString，记录实际行为作为回归基线
    EXPECT_EQ(v.as_string(), "[object Object]");
}

// TL-31: 数组插值（VM）
// 规范预期：`${[1,2,3]}` → "1,2,3"（Array.prototype.toString 调用 join）
// 当前实现：to_string_val 对数组（对象）返回 "[object Object]"，是已知实现限制
TEST(TemplateLiteralVM, TL31_ArrayInterp) {
    auto v = vm_eval("`${[1,2,3]}`");
    EXPECT_TRUE(v.is_string());
    // 当前实现不调用 Array.prototype.toString，记录实际行为作为回归基线
    EXPECT_EQ(v.as_string(), "[object Object]");
}

// TL-32: 空数组插值（Interpreter）
// 规范预期：`${[]}` → ""（Array.prototype.toString 对空数组返回 ""）
// 当前实现：to_string_val 对数组（对象）返回 "[object Object]"，是已知实现限制
TEST(TemplateLiteralInterp, TL32_EmptyArrayInterp) {
    auto v = interp_eval("`${[]}`");
    EXPECT_TRUE(v.is_string());
    // 当前实现不调用 Array.prototype.toString，记录实际行为作为回归基线
    EXPECT_EQ(v.as_string(), "[object Object]");
}

// TL-32: 空数组插值（VM）
// 规范预期：`${[]}` → ""（Array.prototype.toString 对空数组返回 ""）
// 当前实现：to_string_val 对数组（对象）返回 "[object Object]"，是已知实现限制
TEST(TemplateLiteralVM, TL32_EmptyArrayInterp) {
    auto v = vm_eval("`${[]}`");
    EXPECT_TRUE(v.is_string());
    // 当前实现不调用 Array.prototype.toString，记录实际行为作为回归基线
    EXPECT_EQ(v.as_string(), "[object Object]");
}

// ---- 表达式类型：函数调用、逻辑运算 ----

// TL-33: 函数调用插值（Interpreter）
TEST(TemplateLiteralInterp, TL33_FuncCallInterp) {
    auto v = interp_eval("function f() { return 42; } `result: ${f()}`");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "result: 42");
}

// TL-33: 函数调用插值（VM）
TEST(TemplateLiteralVM, TL33_FuncCallInterp) {
    auto v = vm_eval("function f() { return 42; } `result: ${f()}`");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "result: 42");
}

// TL-34: 逻辑运算插值（Interpreter）
TEST(TemplateLiteralInterp, TL34_LogicalOrInterp) {
    auto v = interp_eval("let a = 0; let b = 'fallback'; `${a || b}`");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "fallback");
}

// TL-34: 逻辑运算插值（VM）
TEST(TemplateLiteralVM, TL34_LogicalOrInterp) {
    auto v = vm_eval("let a = 0; let b = 'fallback'; `${a || b}`");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "fallback");
}

// ---- 空模板段 ----

// TL-35: 首段为空（`${x}suffix`）（Interpreter）
TEST(TemplateLiteralInterp, TL35_EmptyHeadQuasi) {
    auto v = interp_eval("let x = 'hi'; `${x} world`");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "hi world");
}

// TL-35: 首段为空（`${x}suffix`）（VM）
TEST(TemplateLiteralVM, TL35_EmptyHeadQuasi) {
    auto v = vm_eval("let x = 'hi'; `${x} world`");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "hi world");
}

// TL-36: 中间段为空（`prefix${x}${y}suffix`）（Interpreter）
TEST(TemplateLiteralInterp, TL36_EmptyMiddleQuasi) {
    auto v = interp_eval("`${1}${2}`");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "12");
}

// TL-36: 中间段为空（`prefix${x}${y}suffix`）（VM）
TEST(TemplateLiteralVM, TL36_EmptyMiddleQuasi) {
    auto v = vm_eval("`${1}${2}`");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "12");
}

// TL-37: 尾段为空（`prefix${x}`）（Interpreter）
TEST(TemplateLiteralInterp, TL37_EmptyTailQuasi) {
    auto v = interp_eval("`prefix${42}`");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "prefix42");
}

// TL-37: 尾段为空（`prefix${x}`）（VM）
TEST(TemplateLiteralVM, TL37_EmptyTailQuasi) {
    auto v = vm_eval("`prefix${42}`");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "prefix42");
}

// ---- 副作用顺序验证 ----

// TL-38: 多插值表达式从左到右求值，副作用严格有序（Interpreter）
TEST(TemplateLiteralInterp, TL38_SideEffectOrder) {
    auto v = interp_eval(
        "let log = [];"
        "function rec(n) { log.push(n); return n; }"
        "`${rec(1)}-${rec(2)}-${rec(3)}`;"
        "log[0] + ',' + log[1] + ',' + log[2]"
    );
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "1,2,3");
}

// TL-38: 多插值表达式从左到右求值，副作用严格有序（VM）
TEST(TemplateLiteralVM, TL38_SideEffectOrder) {
    auto v = vm_eval(
        "let log = [];"
        "function rec(n) { log.push(n); return n; }"
        "`${rec(1)}-${rec(2)}-${rec(3)}`;"
        "log[0] + ',' + log[1] + ',' + log[2]"
    );
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "1,2,3");
}

// ---- 非法转义 SyntaxError ----

// TL-39: \8 在模板字符串中应报 SyntaxError（Parser 阶段）
TEST(TemplateLiteralParser, TL39_IllegalEscape8) {
    auto result = parse_src("`\\8`");
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error().message().find("invalid escape"), std::string::npos);
}

// TL-40: \9 在模板字符串中应报 SyntaxError（Parser 阶段）
TEST(TemplateLiteralParser, TL40_IllegalEscape9) {
    auto result = parse_src("`\\9`");
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error().message().find("invalid escape"), std::string::npos);
}

// TL-41: \01 遗留八进制在模板字符串中应报 SyntaxError
TEST(TemplateLiteralParser, TL41_IllegalOctalEscape) {
    auto result = parse_src("`\\01`");
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error().message().find("invalid escape"), std::string::npos);
}

// TL-42: \8 在插值模板头部应报 SyntaxError
TEST(TemplateLiteralParser, TL42_IllegalEscapeInHead) {
    auto result = parse_src("`\\8${x}`");
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error().message().find("invalid escape"), std::string::npos);
}

// TL-43: \8 在插值模板尾部应报 SyntaxError
TEST(TemplateLiteralParser, TL43_IllegalEscapeInTail) {
    auto result = parse_src("`${x}\\8`");
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error().message().find("invalid escape"), std::string::npos);
}

// ---- 嵌套模板边界 ----

// TL-44: 三层嵌套模板（Interpreter）
TEST(TemplateLiteralInterp, TL44_TripleNested) {
    auto v = interp_eval("`a${ `b${ `c` }d` }e`");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "abcde");
}

// TL-44: 三层嵌套模板（VM）
TEST(TemplateLiteralVM, TL44_TripleNested) {
    auto v = vm_eval("`a${ `b${ `c` }d` }e`");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "abcde");
}

// TL-45: 嵌套模板内含表达式（Interpreter）
TEST(TemplateLiteralInterp, TL45_NestedWithExpr) {
    auto v = interp_eval("let x = 2; let y = 3; `outer ${ `inner ${x + y}` }`");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "outer inner 5");
}

// TL-45: 嵌套模板内含表达式（VM）
TEST(TemplateLiteralVM, TL45_NestedWithExpr) {
    auto v = vm_eval("let x = 2; let y = 3; `outer ${ `inner ${x + y}` }`");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "outer inner 5");
}

// ---- 字符串内容验证：转义序列 ----

// TL-46: \x41 应解码为 "A"（Interpreter）
TEST(TemplateLiteralInterp, TL46_HexEscapeContent) {
    auto v = interp_eval("`\\x41`");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "A");
}

// TL-46: \x41 应解码为 "A"（VM）
TEST(TemplateLiteralVM, TL46_HexEscapeContent) {
    auto v = vm_eval("`\\x41`");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "A");
}

// TL-47: A 应解码为 "A"（Interpreter）
TEST(TemplateLiteralInterp, TL47_UnicodeEscapeContent) {
    auto v = interp_eval("`\\u0041`");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "A");
}

// TL-47: A 应解码为 "A"（VM）
TEST(TemplateLiteralVM, TL47_UnicodeEscapeContent) {
    auto v = vm_eval("`\\u0041`");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "A");
}

// TL-48: \u{41} 应解码为 "A"（Interpreter）
TEST(TemplateLiteralInterp, TL48_UnicodeBraceEscapeContent) {
    auto v = interp_eval("`\\u{41}`");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "A");
}

// TL-48: \u{41} 应解码为 "A"（VM）
TEST(TemplateLiteralVM, TL48_UnicodeBraceEscapeContent) {
    auto v = vm_eval("`\\u{41}`");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "A");
}

// TL-49: \0 应解码为 NUL 字符（Interpreter）
TEST(TemplateLiteralInterp, TL49_NullEscapeContent) {
    auto v = interp_eval("`\\0`");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string().size(), 1u);
    EXPECT_EQ(v.as_string()[0], '\0');
}

// TL-49: \0 应解码为 NUL 字符（VM）
TEST(TemplateLiteralVM, TL49_NullEscapeContent) {
    auto v = vm_eval("`\\0`");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string().size(), 1u);
    EXPECT_EQ(v.as_string()[0], '\0');
}

// TL-50: 行延续 \<LF> 在 cooked 中消除（Parser 验证）
TEST(TemplateLiteralParser, TL50_LineContinuation) {
    // 在 C++ 字符串中，\n 就是真实的换行符，对应 JS 模板字符串中的 \<LF>
    auto result = parse_src("`a\\\nb`");
    ASSERT_EXPR(result, e);
    const auto& tl = std::get<TemplateLiteral>(e.v);
    EXPECT_EQ(tl.quasis[0].cooked, "ab");
}

// TL-51: \r 单独规范化为 \n（Parser 验证）
TEST(TemplateLiteralParser, TL51_CRNormalization) {
    auto result = parse_src("`line1\rline2`");
    ASSERT_EXPR(result, e);
    const auto& tl = std::get<TemplateLiteral>(e.v);
    EXPECT_EQ(tl.quasis[0].cooked, "line1\nline2");
}

// ---- \1-\7 遗留八进制 SyntaxError（P2-1 修复验证）----

// TL-52: \1 在模板字符串中应报 SyntaxError
TEST(TemplateLiteralParser, TL52_IllegalEscape1) {
    auto result = parse_src("`\\1`");
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error().message().find("invalid escape"), std::string::npos);
}

// TL-53: \5 在模板字符串中应报 SyntaxError
TEST(TemplateLiteralParser, TL53_IllegalEscape5) {
    auto result = parse_src("`\\5`");
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error().message().find("invalid escape"), std::string::npos);
}

// TL-54: \7 在模板字符串中应报 SyntaxError
TEST(TemplateLiteralParser, TL54_IllegalEscape7) {
    auto result = parse_src("`\\7`");
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error().message().find("invalid escape"), std::string::npos);
}

// ============================================================
// Tagged Template Literal Tests (TTL-01 ~ TTL-10, Interp+VM)
// ============================================================

// TTL-01: basic tagged template, no interpolation
TEST(TaggedTemplateLiteralInterp, TTL01_Basic_NoSub) {
    auto v = interp_eval(
        "function tag(strings) { return strings[0]; }"
        "tag`hello`"
    );
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "hello");
}
TEST(TaggedTemplateLiteralVM, TTL01_Basic_NoSub) {
    auto v = vm_eval(
        "function tag(strings) { return strings[0]; }"
        "tag`hello`"
    );
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "hello");
}

// TTL-02: tagged template with interpolation
TEST(TaggedTemplateLiteralInterp, TTL02_WithInterp) {
    auto v = interp_eval(
        "let x = 42;"
        "function tag(strings, val) { return strings[0] + val + strings[1]; }"
        "tag`a ${x} b`"
    );
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "a 42 b");
}
TEST(TaggedTemplateLiteralVM, TTL02_WithInterp) {
    auto v = vm_eval(
        "let x = 42;"
        "function tag(strings, val) { return strings[0] + val + strings[1]; }"
        "tag`a ${x} b`"
    );
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "a 42 b");
}

// TTL-03: tag receives strings array and values
TEST(TaggedTemplateLiteralInterp, TTL03_StringsAndValues) {
    auto v = interp_eval(
        "let result = '';"
        "function tag(strings, a, b) {"
        "  result = strings[0] + a + strings[1] + b + strings[2];"
        "  return result;"
        "}"
        "tag`x=${1} y=${2}`"
    );
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "x=1 y=2");
}
TEST(TaggedTemplateLiteralVM, TTL03_StringsAndValues) {
    auto v = vm_eval(
        "let result = '';"
        "function tag(strings, a, b) {"
        "  result = strings[0] + a + strings[1] + b + strings[2];"
        "  return result;"
        "}"
        "tag`x=${1} y=${2}`"
    );
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "x=1 y=2");
}

// TTL-04: strings.raw contains raw strings (unescaped)
TEST(TaggedTemplateLiteralInterp, TTL04_StringsRaw) {
    auto v = interp_eval(
        "function tag(strings) { return strings.raw[0]; }"
        "tag`\\n`"  // cooked = "\n", raw = "\\n" (two chars)
    );
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "\\n");
}
TEST(TaggedTemplateLiteralVM, TTL04_StringsRaw) {
    auto v = vm_eval(
        "function tag(strings) { return strings.raw[0]; }"
        "tag`\\n`"
    );
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "\\n");
}

// TTL-05: strings.length === expressions.length + 1
TEST(TaggedTemplateLiteralInterp, TTL05_StringsLength) {
    auto v = interp_eval(
        "function tag(strings, a, b) { return strings.length; }"
        "tag`${1} and ${2}`"
    );
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 3.0);  // ["", " and ", ""]
}
TEST(TaggedTemplateLiteralVM, TTL05_StringsLength) {
    auto v = vm_eval(
        "function tag(strings, a, b) { return strings.length; }"
        "tag`${1} and ${2}`"
    );
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 3.0);
}

// TTL-06: tag return value is the expression result
TEST(TaggedTemplateLiteralInterp, TTL06_ReturnValue) {
    auto v = interp_eval(
        "function tag(strings) { return 'custom'; }"
        "tag`ignored`"
    );
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "custom");
}
TEST(TaggedTemplateLiteralVM, TTL06_ReturnValue) {
    auto v = vm_eval(
        "function tag(strings) { return 'custom'; }"
        "tag`ignored`"
    );
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "custom");
}

// TTL-07: tag can modify interpolation result
TEST(TaggedTemplateLiteralInterp, TTL07_ModifyInterp) {
    auto v = interp_eval(
        "function tag(strings, val) { return strings[0] + val * 2 + strings[1]; }"
        "tag`result: ${10}!`"
    );
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "result: 20!");
}
TEST(TaggedTemplateLiteralVM, TTL07_ModifyInterp) {
    auto v = vm_eval(
        "function tag(strings, val) { return strings[0] + val * 2 + strings[1]; }"
        "tag`result: ${10}!`"
    );
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "result: 20!");
}

// TTL-08: multiple interpolations
TEST(TaggedTemplateLiteralInterp, TTL08_MultipleInterp) {
    auto v = interp_eval(
        "function tag(s, a, b, c) { return s[0] + a + s[1] + b + s[2] + c + s[3]; }"
        "tag`a${1}b${2}c${3}d`"
    );
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "a1b2c3d");
}
TEST(TaggedTemplateLiteralVM, TTL08_MultipleInterp) {
    auto v = vm_eval(
        "function tag(s, a, b, c) { return s[0] + a + s[1] + b + s[2] + c + s[3]; }"
        "tag`a${1}b${2}c${3}d`"
    );
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "a1b2c3d");
}

// TTL-09: tag is method call (this binding correct)
TEST(TaggedTemplateLiteralInterp, TTL09_MethodTag) {
    auto v = interp_eval(
        "let obj = { prefix: 'PRE', tag: function(strings) { return this.prefix + ':' + strings[0]; } };"
        "obj.tag`hello`"
    );
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "PRE:hello");
}
TEST(TaggedTemplateLiteralVM, TTL09_MethodTag) {
    auto v = vm_eval(
        "let obj = { prefix: 'PRE', tag: function(strings) { return this.prefix + ':' + strings[0]; } };"
        "obj.tag`hello`"
    );
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "PRE:hello");
}

// TTL-10: raw vs cooked content (basic validation)
TEST(TaggedTemplateLiteralInterp, TTL10_RawVsCooked) {
    auto v = interp_eval(
        "function tag(strings) { return strings[0].length + ':' + strings.raw[0].length; }"
        "tag`\\n`"  // cooked="\n" (length 1), raw="\\n" (length 2)
    );
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "1:2");
}
TEST(TaggedTemplateLiteralVM, TTL10_RawVsCooked) {
    auto v = vm_eval(
        "function tag(strings) { return strings[0].length + ':' + strings.raw[0].length; }"
        "tag`\\n`"
    );
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "1:2");
}
