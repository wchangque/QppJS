#include "qppjs/frontend/lexer.h"
#include "qppjs/frontend/token.h"

#include <gtest/gtest.h>
#include <string_view>
#include <vector>

using qppjs::TokenKind;

// 辅助函数：收集所有 token 直到 Eof
static std::vector<qppjs::Token> collect_tokens(std::string_view source) {
    auto state = qppjs::lexer_init(source);
    std::vector<qppjs::Token> tokens;
    while (true) {
        qppjs::Token tok = qppjs::next_token(state);
        tokens.push_back(tok);
        if (tok.kind == TokenKind::Eof) break;
    }
    return tokens;
}

// --- Eof 与空白 ---

TEST(LexerTest, EmptyStringGivesEof) {
    auto state = qppjs::lexer_init("");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::Eof);
    EXPECT_EQ(tok.range.offset, 0u);
    EXPECT_EQ(tok.range.length, 0u);
}

TEST(LexerTest, WhitespaceOnlyGivesEof) {
    auto state = qppjs::lexer_init("   ");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::Eof);
}

// --- 注释 ---

TEST(LexerTest, LineCommentGivesEofWithGotLf) {
    auto state = qppjs::lexer_init("// comment\n");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::Eof);
    EXPECT_TRUE(state.got_lf);
}

TEST(LexerTest, BlockCommentGivesEof) {
    auto state = qppjs::lexer_init("/* x */");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::Eof);
}

TEST(LexerTest, BlockCommentWithNewlineGivesGotLf) {
    auto state = qppjs::lexer_init("/* \n */");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::Eof);
    EXPECT_TRUE(state.got_lf);
}

TEST(LexerTest, UnclosedBlockCommentGivesInvalid) {
    auto state = qppjs::lexer_init("/* unclosed");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::Invalid);
}

// --- 单字符标点 ---

TEST(LexerTest, SingleCharPunctuation) {
    auto state = qppjs::lexer_init("(");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::LParen);
    EXPECT_EQ(tok.range.offset, 0u);
    EXPECT_EQ(tok.range.length, 1u);
}

TEST(LexerTest, AllSingleCharPunctuations) {
    struct Case { char ch; TokenKind kind; };
    const Case cases[] = {
        {'(', TokenKind::LParen},
        {')', TokenKind::RParen},
        {'{', TokenKind::LBrace},
        {'}', TokenKind::RBrace},
        {'[', TokenKind::LBracket},
        {']', TokenKind::RBracket},
        {';', TokenKind::Semicolon},
        {':', TokenKind::Colon},
        {',', TokenKind::Comma},
        {'.', TokenKind::Dot},
    };
    for (const auto& c : cases) {
        char buf[2] = {c.ch, '\0'};
        auto state = qppjs::lexer_init(std::string_view(buf, 1));
        auto tok = qppjs::next_token(state);
        EXPECT_EQ(tok.kind, c.kind) << "failed for char: " << c.ch;
        EXPECT_EQ(tok.range.length, 1u);
    }
}

// --- 标识符与关键字 ---

TEST(LexerTest, IdentifierToken) {
    auto state = qppjs::lexer_init("foo");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::Ident);
    EXPECT_EQ(tok.range.length, 3u);
}

TEST(LexerTest, KeywordLet) {
    auto state = qppjs::lexer_init("let");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::KwLet);
}

TEST(LexerTest, KeywordTrue) {
    auto state = qppjs::lexer_init("true");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::KwTrue);
}

TEST(LexerTest, KeywordWhile) {
    auto state = qppjs::lexer_init("while");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::KwWhile);
}

TEST(LexerTest, IdentStartingWithKeyword) {
    // "letter" 不是关键字，应产出 Ident
    auto state = qppjs::lexer_init("letter");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::Ident);
}

// --- Invalid ---

TEST(LexerTest, UnknownCharGivesInvalid) {
    auto state = qppjs::lexer_init("@");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::Invalid);
    EXPECT_EQ(tok.range.length, 1u);
}

// --- 多 token 序列 ---

TEST(LexerTest, MultiTokenLetX) {
    auto tokens = collect_tokens("let x");
    ASSERT_EQ(tokens.size(), 3u);  // KwLet, Ident, Eof
    EXPECT_EQ(tokens[0].kind, TokenKind::KwLet);
    EXPECT_EQ(tokens[0].range.offset, 0u);
    EXPECT_EQ(tokens[0].range.length, 3u);
    EXPECT_EQ(tokens[1].kind, TokenKind::Ident);
    EXPECT_EQ(tokens[1].range.offset, 4u);
    EXPECT_EQ(tokens[1].range.length, 1u);
    EXPECT_EQ(tokens[2].kind, TokenKind::Eof);
}

// --- 换行处理 ---

TEST(LexerTest, CrLfCountsAsSingleNewline) {
    // "a\r\nb" -> 扫完 'a'，再调 next_token 跳过 \r\n 得到 'b'
    auto state = qppjs::lexer_init("a\r\nb");
    // 消费 'a'
    auto tok1 = qppjs::next_token(state);
    EXPECT_EQ(tok1.kind, TokenKind::Ident);
    // 消费 'b'，此时应经历 \r\n，got_lf 应为 true，line 应为 2
    auto tok2 = qppjs::next_token(state);
    EXPECT_EQ(tok2.kind, TokenKind::Ident);
    EXPECT_TRUE(state.got_lf);
    EXPECT_EQ(state.line, 2u);
}

// --- compute_location ---

TEST(LexerTest, ComputeLocationFirstChar) {
    auto loc = qppjs::compute_location("hello\nworld", 0);
    EXPECT_EQ(loc.line, 1u);
    EXPECT_EQ(loc.column, 1u);
}

TEST(LexerTest, ComputeLocationAfterNewline) {
    auto loc = qppjs::compute_location("hello\nworld", 6);
    EXPECT_EQ(loc.line, 2u);
    EXPECT_EQ(loc.column, 1u);
}

TEST(LexerTest, ComputeLocationAfterCrLf) {
    // "hello\r\nworld" offset=7 -> 'w' 是第 2 行第 1 列
    auto loc = qppjs::compute_location("hello\r\nworld", 7);
    EXPECT_EQ(loc.line, 2u);
    EXPECT_EQ(loc.column, 1u);
}

TEST(LexerTest, ComputeLocationMidLine) {
    auto loc = qppjs::compute_location("hello\nworld", 8);
    EXPECT_EQ(loc.line, 2u);
    EXPECT_EQ(loc.column, 3u);
}

// --- 数字字面量（合法）---

TEST(LexerTest, NumberZero) {
    auto state = qppjs::lexer_init("0");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::Number);
    EXPECT_EQ(tok.range.length, 1u);
}

TEST(LexerTest, NumberDecimalInt) {
    auto state = qppjs::lexer_init("42");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::Number);
    EXPECT_EQ(tok.range.length, 2u);
}

TEST(LexerTest, NumberDecimalFloat) {
    auto state = qppjs::lexer_init("3.14");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::Number);
    EXPECT_EQ(tok.range.length, 4u);
}

TEST(LexerTest, NumberDotLeading) {
    auto state = qppjs::lexer_init(".5");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::Number);
    EXPECT_EQ(tok.range.length, 2u);
}

TEST(LexerTest, NumberTrailingDot) {
    auto state = qppjs::lexer_init("1.");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::Number);
    EXPECT_EQ(tok.range.length, 2u);
}

TEST(LexerTest, NumberExponentLower) {
    auto state = qppjs::lexer_init("1e3");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::Number);
    EXPECT_EQ(tok.range.length, 3u);
}

TEST(LexerTest, NumberExponentUpper) {
    auto state = qppjs::lexer_init("1E3");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::Number);
    EXPECT_EQ(tok.range.length, 3u);
}

TEST(LexerTest, NumberExponentWithSign) {
    auto state = qppjs::lexer_init("1.5e-2");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::Number);
    EXPECT_EQ(tok.range.length, 6u);
}

TEST(LexerTest, NumberHex) {
    auto state = qppjs::lexer_init("0xFF");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::Number);
    EXPECT_EQ(tok.range.length, 4u);
}

TEST(LexerTest, NumberBinary) {
    auto state = qppjs::lexer_init("0b1010");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::Number);
    EXPECT_EQ(tok.range.length, 6u);
}

TEST(LexerTest, NumberOctal) {
    auto state = qppjs::lexer_init("0o755");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::Number);
    EXPECT_EQ(tok.range.length, 5u);
}

// --- 数字字面量（非法）---

TEST(LexerTest, NumberInvalidLetterSuffix) {
    auto state = qppjs::lexer_init("123abc");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::Invalid);
}

TEST(LexerTest, NumberInvalidHexNoDigit) {
    auto state = qppjs::lexer_init("0x");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::Invalid);
}

TEST(LexerTest, NumberInvalidBinaryBadDigit) {
    auto state = qppjs::lexer_init("0b2");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::Invalid);
}

TEST(LexerTest, NumberInvalidOctalBadDigit) {
    auto state = qppjs::lexer_init("0o9");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::Invalid);
}

TEST(LexerTest, NumberInvalidEmptyExponent) {
    auto state = qppjs::lexer_init("1e");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::Invalid);
}

TEST(LexerTest, NumberInvalidLeadingZero) {
    auto state = qppjs::lexer_init("01");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::Invalid);
}

// --- 字符串字面量（合法）---

TEST(LexerTest, StringSingleQuote) {
    auto state = qppjs::lexer_init("'hello'");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::String);
    EXPECT_EQ(tok.range.length, 7u);
}

TEST(LexerTest, StringDoubleQuote) {
    auto state = qppjs::lexer_init("\"hello\"");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::String);
    EXPECT_EQ(tok.range.length, 7u);
}

TEST(LexerTest, StringEmpty) {
    auto state = qppjs::lexer_init("''");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::String);
    EXPECT_EQ(tok.range.length, 2u);
}

TEST(LexerTest, StringEscapeN) {
    // '\n' 字面上是 4 个字符：' \ n '
    auto state = qppjs::lexer_init("'\\n'");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::String);
    EXPECT_EQ(tok.range.length, 4u);
}

TEST(LexerTest, StringEscapeHex) {
    // '\xFF' 字面上是 6 个字符
    auto state = qppjs::lexer_init("'\\xFF'");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::String);
    EXPECT_EQ(tok.range.length, 6u);
}

TEST(LexerTest, StringEscapeUnicode) {
    // 'A' 字面上是 8 个字符
    auto state = qppjs::lexer_init("'\\u0041'");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::String);
    EXPECT_EQ(tok.range.length, 8u);
}

// --- 字符串字面量（非法）---

TEST(LexerTest, StringInvalidUnclosed) {
    auto state = qppjs::lexer_init("\"hello");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::Invalid);
}

TEST(LexerTest, StringInvalidNewlineInside) {
    // 字符串中直接出现 LF
    auto state = qppjs::lexer_init("'hello\nworld'");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::Invalid);
}

TEST(LexerTest, StringInvalidHexShort) {
    // \x 只有一位
    auto state = qppjs::lexer_init("'\\x4'");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::Invalid);
}

TEST(LexerTest, StringInvalidUnicodeShort) {
    // \u 只有三位
    auto state = qppjs::lexer_init("'\\u004'");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::Invalid);
}

TEST(LexerTest, StringInvalidNullOctal) {
    // \0 后跟数字
    auto state = qppjs::lexer_init("'\\01'");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::Invalid);
}

// --- 多字符操作符 ---

TEST(LexerTest, OperatorEqEq) {
    auto state = qppjs::lexer_init("==");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::EqEq);
    EXPECT_EQ(tok.range.length, 2u);
}

TEST(LexerTest, OperatorEqEqEq) {
    auto state = qppjs::lexer_init("===");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::EqEqEq);
    EXPECT_EQ(tok.range.length, 3u);
}

TEST(LexerTest, OperatorBangEq) {
    auto state = qppjs::lexer_init("!=");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::BangEq);
    EXPECT_EQ(tok.range.length, 2u);
}

TEST(LexerTest, OperatorBangEqEq) {
    auto state = qppjs::lexer_init("!==");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::BangEqEq);
    EXPECT_EQ(tok.range.length, 3u);
}

TEST(LexerTest, OperatorLtEq) {
    auto state = qppjs::lexer_init("<=");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::LtEq);
    EXPECT_EQ(tok.range.length, 2u);
}

TEST(LexerTest, OperatorGtEq) {
    auto state = qppjs::lexer_init(">=");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::GtEq);
    EXPECT_EQ(tok.range.length, 2u);
}

TEST(LexerTest, OperatorPlusPlus) {
    auto state = qppjs::lexer_init("++");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::PlusPlus);
    EXPECT_EQ(tok.range.length, 2u);
}

TEST(LexerTest, OperatorMinusMinus) {
    auto state = qppjs::lexer_init("--");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::MinusMinus);
    EXPECT_EQ(tok.range.length, 2u);
}

TEST(LexerTest, OperatorAmpAmp) {
    auto state = qppjs::lexer_init("&&");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::AmpAmp);
    EXPECT_EQ(tok.range.length, 2u);
}

TEST(LexerTest, OperatorPipePipe) {
    auto state = qppjs::lexer_init("||");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::PipePipe);
    EXPECT_EQ(tok.range.length, 2u);
}

TEST(LexerTest, OperatorPlusEq) {
    auto state = qppjs::lexer_init("+=");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::PlusEq);
    EXPECT_EQ(tok.range.length, 2u);
}

TEST(LexerTest, OperatorMinusEq) {
    auto state = qppjs::lexer_init("-=");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::MinusEq);
    EXPECT_EQ(tok.range.length, 2u);
}

TEST(LexerTest, OperatorStarEq) {
    auto state = qppjs::lexer_init("*=");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::StarEq);
    EXPECT_EQ(tok.range.length, 2u);
}

TEST(LexerTest, OperatorSlashEq) {
    auto state = qppjs::lexer_init("/=");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::SlashEq);
    EXPECT_EQ(tok.range.length, 2u);
}

TEST(LexerTest, OperatorPercentEq) {
    auto state = qppjs::lexer_init("%=");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::PercentEq);
    EXPECT_EQ(tok.range.length, 2u);
}

TEST(LexerTest, OperatorArrow) {
    auto state = qppjs::lexer_init("=>");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::Arrow);
    EXPECT_EQ(tok.range.length, 2u);
}

TEST(LexerTest, OperatorEqSingle) {
    auto state = qppjs::lexer_init("=");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::Eq);
    EXPECT_EQ(tok.range.length, 1u);
}

TEST(LexerTest, OperatorBang) {
    auto state = qppjs::lexer_init("!");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::Bang);
    EXPECT_EQ(tok.range.length, 1u);
}

TEST(LexerTest, OperatorLt) {
    auto state = qppjs::lexer_init("<");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::Lt);
    EXPECT_EQ(tok.range.length, 1u);
}

TEST(LexerTest, OperatorGt) {
    auto state = qppjs::lexer_init(">");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::Gt);
    EXPECT_EQ(tok.range.length, 1u);
}

TEST(LexerTest, OperatorPlus) {
    auto state = qppjs::lexer_init("+");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::Plus);
    EXPECT_EQ(tok.range.length, 1u);
}

TEST(LexerTest, OperatorMinus) {
    auto state = qppjs::lexer_init("-");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::Minus);
    EXPECT_EQ(tok.range.length, 1u);
}

TEST(LexerTest, OperatorStar) {
    auto state = qppjs::lexer_init("*");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::Star);
    EXPECT_EQ(tok.range.length, 1u);
}

TEST(LexerTest, OperatorSlash) {
    auto state = qppjs::lexer_init("/");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::Slash);
    EXPECT_EQ(tok.range.length, 1u);
}

TEST(LexerTest, OperatorPercent) {
    auto state = qppjs::lexer_init("%");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::Percent);
    EXPECT_EQ(tok.range.length, 1u);
}

TEST(LexerTest, OperatorAmp) {
    auto state = qppjs::lexer_init("&");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::Amp);
    EXPECT_EQ(tok.range.length, 1u);
}

TEST(LexerTest, OperatorPipe) {
    auto state = qppjs::lexer_init("|");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::Pipe);
    EXPECT_EQ(tok.range.length, 1u);
}

TEST(LexerTest, OperatorCaret) {
    auto state = qppjs::lexer_init("^");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::Caret);
    EXPECT_EQ(tok.range.length, 1u);
}

TEST(LexerTest, OperatorTilde) {
    auto state = qppjs::lexer_init("~");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::Tilde);
    EXPECT_EQ(tok.range.length, 1u);
}

// --- 最长匹配验证 ---

TEST(LexerTest, LongestMatchBangEqEq) {
    auto tokens = collect_tokens("!==");
    // 应为 [BangEqEq, Eof]，即一个 token
    ASSERT_EQ(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].kind, TokenKind::BangEqEq);
    EXPECT_EQ(tokens[0].range.length, 3u);
}

TEST(LexerTest, LongestMatchEqEqEq) {
    auto tokens = collect_tokens("===");
    ASSERT_EQ(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].kind, TokenKind::EqEqEq);
    EXPECT_EQ(tokens[0].range.length, 3u);
}

TEST(LexerTest, LongestMatchPlusEqThenEq) {
    // "+==" -> [PlusEq, Eq, Eof]
    auto tokens = collect_tokens("+==");
    ASSERT_EQ(tokens.size(), 3u);
    EXPECT_EQ(tokens[0].kind, TokenKind::PlusEq);
    EXPECT_EQ(tokens[1].kind, TokenKind::Eq);
}

// --- Dot 与数字字面量 lookahead 验证 ---

TEST(LexerTest, DotAloneIsDot) {
    auto state = qppjs::lexer_init(".");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::Dot);
    EXPECT_EQ(tok.range.length, 1u);
}

TEST(LexerTest, DotFollowedByLetterIsDot) {
    // ".x" -> Dot, Ident
    auto tokens = collect_tokens(".x");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].kind, TokenKind::Dot);
    EXPECT_EQ(tokens[1].kind, TokenKind::Ident);
}

// ============================================================
// 以下为 Phase 1 (1.1 + 1.2) 边界测试补充
// ============================================================

// --- 换行处理边界 ---

TEST(LexerTest, SoloCrCountsAsNewline) {
    // 单独 '\r'（无 '\n' 跟随）：got_lf = true，line 递增为 2
    auto state = qppjs::lexer_init("a\rb");
    qppjs::next_token(state);        // 消耗 'a'
    auto tok = qppjs::next_token(state);  // 跳过 '\r'，消耗 'b'
    EXPECT_EQ(tok.kind, TokenKind::Ident);
    EXPECT_TRUE(state.got_lf);
    EXPECT_EQ(state.line, 2u);
}

TEST(LexerTest, CrLfCountsAsSingleNewlineLineNumber) {
    // '\r\n' 应计为单个换行，line 仅递增 1
    auto state = qppjs::lexer_init("a\r\nb");
    qppjs::next_token(state);        // 消耗 'a'
    auto tok = qppjs::next_token(state);  // 跳过 '\r\n'，消耗 'b'
    EXPECT_EQ(tok.kind, TokenKind::Ident);
    EXPECT_EQ(state.line, 2u);
}

TEST(LexerTest, MultipleNewlinesLineCount) {
    // "a\n\n\nb" -> 经过 3 个 '\n'，line 应为 4
    auto state = qppjs::lexer_init("a\n\n\nb");
    qppjs::next_token(state);        // 消耗 'a'
    qppjs::next_token(state);        // 消耗 'b'
    EXPECT_EQ(state.line, 4u);
}

TEST(LexerTest, BlockCommentCrLfLineCount) {
    // 多行注释中含 '\r\n'，'\r\n' 应计为单个换行
    // "/* \r\n */" 含一个 \r\n，结束后 line 应为 2
    auto state = qppjs::lexer_init("/* \r\n */x");
    auto tok = qppjs::next_token(state);  // 跳过注释，消耗 'x'
    EXPECT_EQ(tok.kind, TokenKind::Ident);
    EXPECT_EQ(state.line, 2u);
    EXPECT_TRUE(state.got_lf);
}

TEST(LexerTest, BlockCommentMultipleCrLfLineCount) {
    // 注释含两个 \r\n，结束后 line 应为 3
    auto state = qppjs::lexer_init("/*\r\n\r\n*/x");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::Ident);
    EXPECT_EQ(state.line, 3u);
}

TEST(LexerTest, LineCommentSoloCrGivesGotLf) {
    // 单行注释以 '\r' 结束（非 '\n'）：'\r' 本身不是注释内容的终止符
    // 实现中单行注释扫描到 '\n' 或 '\r' 停止，但 '\r' 本身作为独立换行
    // 此处验证：注释扫描结束后，'\r' 在下一次 next_token 的空白跳过中被消耗
    auto state = qppjs::lexer_init("// comment\rx");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::Ident);  // 跳过注释+'\r'，得到 'x'
    EXPECT_TRUE(state.got_lf);
    EXPECT_EQ(state.line, 2u);
}

// U+2028 (LINE SEPARATOR) 和 U+2029 (PARAGRAPH SEPARATOR) 的 UTF-8 编码
// U+2028 = E2 80 A8，U+2029 = E2 80 A9
// 当前实现仅识别 '\n' 和 '\r' 作为换行，不识别 U+2028/U+2029
// ECMAScript 规范将其视为行终止符，但当前实现尚未支持
// 此处记录实际行为：U+2028/U+2029 被当作普通字符，不触发 got_lf，不递增 line

TEST(LexerTest, U2028NotTreatedAsNewlineCurrentImpl) {
    // NOTE: ECMAScript 规范要求 U+2028 为行终止符，但当前实现未支持。
    // 以下断言验证现有行为（U+2028 不触发换行），作为回归基线。
    // 如需规范合规，需在 next_token 中识别 U+2028/U+2029 的 UTF-8 编码。
    const char src[] = "a\xE2\x80\xA8" "b";  // "a" + U+2028(UTF-8) + "b"
    auto state = qppjs::lexer_init(std::string_view(src, sizeof(src) - 1));
    qppjs::next_token(state);       // 消耗 'a'
    qppjs::next_token(state);       // 跳过 U+2028 字节序列，消耗 'b'
    // 当前实现：got_lf == false，line == 1（未将 U+2028 视为换行）
    EXPECT_FALSE(state.got_lf);
    EXPECT_EQ(state.line, 1u);
}

TEST(LexerTest, U2029NotTreatedAsNewlineCurrentImpl) {
    // NOTE: 同 U+2028，U+2029 在当前实现中未被视为行终止符。
    const char src[] = "a\xE2\x80\xA9" "b";  // "a" + U+2029(UTF-8) + "b"
    auto state = qppjs::lexer_init(std::string_view(src, sizeof(src) - 1));
    qppjs::next_token(state);
    qppjs::next_token(state);
    EXPECT_FALSE(state.got_lf);
    EXPECT_EQ(state.line, 1u);
}

// --- 数字字面量边界 ---

TEST(LexerTest, NumberZeroAloneIsNumber) {
    // '0' 单独作为 token，合法
    auto tokens = collect_tokens("0");
    ASSERT_EQ(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].kind, TokenKind::Number);
    EXPECT_EQ(tokens[0].range.length, 1u);
}

TEST(LexerTest, NumberZeroDotTrailing) {
    // '0.' 尾随点，合法（0. == 0.0）
    auto state = qppjs::lexer_init("0.");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::Number);
    EXPECT_EQ(tok.range.length, 2u);
}

TEST(LexerTest, NumberZeroExponent) {
    // '0e0' 合法
    auto state = qppjs::lexer_init("0e0");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::Number);
    EXPECT_EQ(tok.range.length, 3u);
}

TEST(LexerTest, NumberMinHex) {
    // '0x0' 最小十六进制，合法
    auto state = qppjs::lexer_init("0x0");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::Number);
    EXPECT_EQ(tok.range.length, 3u);
}

TEST(LexerTest, NumberMinBinary) {
    // '0b0' 最小二进制，合法
    auto state = qppjs::lexer_init("0b0");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::Number);
    EXPECT_EQ(tok.range.length, 3u);
}

TEST(LexerTest, NumberMinOctal) {
    // '0o0' 最小八进制，合法
    auto state = qppjs::lexer_init("0o0");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::Number);
    EXPECT_EQ(tok.range.length, 3u);
}

TEST(LexerTest, NumberHexUpperPrefix) {
    // '0X' 大写前缀，与 '0x' 等价，但无后续十六进制数字 -> Invalid
    auto state = qppjs::lexer_init("0X");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::Invalid);
}

TEST(LexerTest, NumberHexUpperPrefixValid) {
    // '0XFF' 大写前缀，合法
    auto state = qppjs::lexer_init("0XFF");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::Number);
    EXPECT_EQ(tok.range.length, 4u);
}

TEST(LexerTest, NumberBinaryUpperPrefix) {
    // '0B1' 大写前缀，合法
    auto state = qppjs::lexer_init("0B1");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::Number);
    EXPECT_EQ(tok.range.length, 3u);
}

TEST(LexerTest, NumberOctalUpperPrefix) {
    // '0O7' 大写前缀，合法
    auto state = qppjs::lexer_init("0O7");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::Number);
    EXPECT_EQ(tok.range.length, 3u);
}

TEST(LexerTest, NumberExponentPlusSign) {
    // '1e+3' 带正号指数，合法
    auto state = qppjs::lexer_init("1e+3");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::Number);
    EXPECT_EQ(tok.range.length, 4u);
}

TEST(LexerTest, NumberExponentMinusSign) {
    // '1e-3' 带负号指数，合法
    auto state = qppjs::lexer_init("1e-3");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::Number);
    EXPECT_EQ(tok.range.length, 4u);
}

TEST(LexerTest, NumberInvalidUnderscoreSuffix) {
    // '42_' 数字后跟下划线（is_ident_start），Invalid
    auto state = qppjs::lexer_init("42_");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::Invalid);
}

TEST(LexerTest, NumberInvalidDollarSuffix) {
    // '42$' 数字后跟 '$'（is_ident_start），Invalid
    auto state = qppjs::lexer_init("42$");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::Invalid);
}

TEST(LexerTest, DotAloneThenDot) {
    // '..' -> Dot + Dot（两个点，均不触发数字扫描）
    auto tokens = collect_tokens("..");
    ASSERT_EQ(tokens.size(), 3u);
    EXPECT_EQ(tokens[0].kind, TokenKind::Dot);
    EXPECT_EQ(tokens[1].kind, TokenKind::Dot);
    EXPECT_EQ(tokens[2].kind, TokenKind::Eof);
}

TEST(LexerTest, NumberZeroThenDotIsNumberThenDot) {
    // '0.' 是数字，但 '0..' 应为 Number(0.) + Dot
    // 注意：'0.' 中的 '.' 已被数字扫描消耗，第二个 '.' 单独成 Dot
    auto tokens = collect_tokens("0..");
    ASSERT_EQ(tokens.size(), 3u);
    EXPECT_EQ(tokens[0].kind, TokenKind::Number);
    EXPECT_EQ(tokens[0].range.length, 2u);  // "0."
    EXPECT_EQ(tokens[1].kind, TokenKind::Dot);
    EXPECT_EQ(tokens[2].kind, TokenKind::Eof);
}

TEST(LexerTest, NumberHexAllUpperDigits) {
    // '0xABCDEF' 全大写十六进制位，合法
    auto state = qppjs::lexer_init("0xABCDEF");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::Number);
    EXPECT_EQ(tok.range.length, 8u);
}

TEST(LexerTest, NumberInvalidExponentSignOnly) {
    // '1e+' 指数符号后无数字 -> Invalid
    auto state = qppjs::lexer_init("1e+");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::Invalid);
}

TEST(LexerTest, NumberInvalidExponentMinusOnly) {
    // '1e-' 指数符号后无数字 -> Invalid
    auto state = qppjs::lexer_init("1e-");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::Invalid);
}

// --- 字符串字面量边界 ---

TEST(LexerTest, StringEmptyDoubleQuote) {
    // '""' 空双引号字符串，合法
    auto state = qppjs::lexer_init("\"\"");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::String);
    EXPECT_EQ(tok.range.length, 2u);
}

TEST(LexerTest, StringNulEscape) {
    // '\0' NUL 转义，合法（后跟非数字）
    // 字面量为 4 字节：' \ 0 '
    auto state = qppjs::lexer_init("'\\0'");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::String);
    EXPECT_EQ(tok.range.length, 4u);
}

TEST(LexerTest, StringNulEscapeFollowedByNonDigit) {
    // '\0a' NUL 后跟非数字字符，合法（\0 = NUL，a 是普通字符）
    // 字面量为 5 字节：' \ 0 a '
    auto state = qppjs::lexer_init("'\\0a'");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::String);
    EXPECT_EQ(tok.range.length, 5u);
}

TEST(LexerTest, StringNulEscapeFollowedByDigitIsInvalid) {
    // '\01' \0 后跟数字 -> Invalid（遗留八进制）
    auto state = qppjs::lexer_init("'\\01'");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::Invalid);
}

TEST(LexerTest, StringHexEscapeMaxValue) {
    // '\xFF' 最大双位十六进制，合法
    auto state = qppjs::lexer_init("'\\xFF'");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::String);
    EXPECT_EQ(tok.range.length, 6u);
}

TEST(LexerTest, StringUnicodeEscapeMinValue) {
    // ' ' 最小 Unicode 转义，合法
    auto state = qppjs::lexer_init("'\\u0000'");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::String);
    EXPECT_EQ(tok.range.length, 8u);
}

TEST(LexerTest, StringUnicodeEscapeMaxBMPValue) {
    // '￿' 最大 BMP Unicode 转义，合法
    auto state = qppjs::lexer_init("'\\uFFFF'");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::String);
    EXPECT_EQ(tok.range.length, 8u);
}

TEST(LexerTest, StringCrInsideIsInvalid) {
    // 字符串中直接出现 '\r'（非转义），Invalid
    // '\r' 在 scan_string 中被行终止符检查截获
    const char src[] = "'hello\rworld'";
    auto state = qppjs::lexer_init(std::string_view(src, sizeof(src) - 1));
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::Invalid);
}

TEST(LexerTest, StringLineContinuationLf) {
    // 字符串内 '\\\n'（反斜杠 + LF）为合法续行（LineContinuation）
    // 当前实现：esc 字符为 '\n' 时走 default 分支，合法继续
    // 字面量为 6 字节：' \ \n '，token 跨越换行
    const char src[] = "'a\\\nb'";
    auto state = qppjs::lexer_init(std::string_view(src, sizeof(src) - 1));
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::String);
    EXPECT_EQ(tok.range.length, 6u);
}

TEST(LexerTest, StringLineContinuationCr) {
    // 字符串内 '\\\r'（反斜杠 + CR）为合法续行
    // 当前实现同 LF 路径：esc == '\r' 走 default 分支，合法
    const char src[] = "'a\\\rb'";
    auto state = qppjs::lexer_init(std::string_view(src, sizeof(src) - 1));
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::String);
    EXPECT_EQ(tok.range.length, 6u);
}

TEST(LexerTest, StringLineContinuationCrLf) {
    // 字符串内 '\\\r\n'（反斜杠 + CRLF）为合法续行，\r\n 应整体消耗
    // 若只消耗 \r，下一轮循环遇 \n 会误判为行终止符并返回 Invalid
    const char src[] = "'a\\\r\nb'";
    auto state = qppjs::lexer_init(std::string_view(src, sizeof(src) - 1));
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::String);
    EXPECT_EQ(tok.range.length, 7u);
}

TEST(LexerTest, StringBackslashAtEofIsInvalid) {
    // 字符串末尾 '\\' 后直接 EOF -> Invalid
    auto state = qppjs::lexer_init("'\\");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::Invalid);
}

TEST(LexerTest, StringUnknownEscapeIsValid) {
    // '\q' 未知转义序列，在非严格模式下合法（SV = 'q'）
    // 当前实现：未识别转义走 default，合法
    auto state = qppjs::lexer_init("'\\q'");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::String);
    EXPECT_EQ(tok.range.length, 4u);
}

TEST(LexerTest, StringHexEscapeNonHexDigitIsInvalid) {
    // '\xGG' \x 后跟非十六进制字符 -> Invalid
    auto state = qppjs::lexer_init("'\\xGG'");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::Invalid);
}

TEST(LexerTest, StringUnicodeEscapeNonHexIsInvalid) {
    // '\uGGGG' \u 后跟非十六进制字符 -> Invalid
    auto state = qppjs::lexer_init("'\\uGGGG'");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::Invalid);
}

// --- 操作符边界（未定义操作符的实际切分行为）---

TEST(LexerTest, OperatorLtLtSplitsToLtLt) {
    // '<<' 当前未定义为单一 token，应切分为 Lt + Lt
    auto tokens = collect_tokens("<<");
    ASSERT_EQ(tokens.size(), 3u);
    EXPECT_EQ(tokens[0].kind, TokenKind::Lt);
    EXPECT_EQ(tokens[1].kind, TokenKind::Lt);
    EXPECT_EQ(tokens[2].kind, TokenKind::Eof);
}

TEST(LexerTest, OperatorGtGtSplitsToGtGt) {
    // '>>' 当前未定义为单一 token，应切分为 Gt + Gt
    auto tokens = collect_tokens(">>");
    ASSERT_EQ(tokens.size(), 3u);
    EXPECT_EQ(tokens[0].kind, TokenKind::Gt);
    EXPECT_EQ(tokens[1].kind, TokenKind::Gt);
}

TEST(LexerTest, OperatorLtLtEqSplitsToLtLtEq) {
    // '<<=' 当前实现：'<' 只能匹配 '<' 或 '<='，
    // 所以 '<<=' 应为 Lt + LtEq
    auto tokens = collect_tokens("<<=");
    ASSERT_EQ(tokens.size(), 3u);
    EXPECT_EQ(tokens[0].kind, TokenKind::Lt);
    EXPECT_EQ(tokens[1].kind, TokenKind::LtEq);
    EXPECT_EQ(tokens[2].kind, TokenKind::Eof);
}

TEST(LexerTest, OperatorGtGtEqSplitsToGtGtEq) {
    // '>>=' 当前实现：'>' 只能匹配 '>' 或 '>='，
    // 所以 '>>=' 应为 Gt + GtEq
    auto tokens = collect_tokens(">>=");
    ASSERT_EQ(tokens.size(), 3u);
    EXPECT_EQ(tokens[0].kind, TokenKind::Gt);
    EXPECT_EQ(tokens[1].kind, TokenKind::GtEq);
    EXPECT_EQ(tokens[2].kind, TokenKind::Eof);
}

TEST(LexerTest, OperatorStarStarSplitsToStarStar) {
    // '**' 幂运算，当前未定义，应切分为 Star + Star
    auto tokens = collect_tokens("**");
    ASSERT_EQ(tokens.size(), 3u);
    EXPECT_EQ(tokens[0].kind, TokenKind::Star);
    EXPECT_EQ(tokens[1].kind, TokenKind::Star);
}

TEST(LexerTest, OperatorQuestionQuestionSplits) {
    // '??' 空值合并，当前未定义，应切分为 Question + Question
    auto tokens = collect_tokens("??");
    ASSERT_EQ(tokens.size(), 3u);
    EXPECT_EQ(tokens[0].kind, TokenKind::Question);
    EXPECT_EQ(tokens[1].kind, TokenKind::Question);
}

TEST(LexerTest, OperatorSpreadSplitsToDotDotDot) {
    // '...' 展开运算符，当前未定义，应切分为 Dot + Dot + Dot
    auto tokens = collect_tokens("...");
    ASSERT_EQ(tokens.size(), 4u);
    EXPECT_EQ(tokens[0].kind, TokenKind::Dot);
    EXPECT_EQ(tokens[1].kind, TokenKind::Dot);
    EXPECT_EQ(tokens[2].kind, TokenKind::Dot);
    EXPECT_EQ(tokens[3].kind, TokenKind::Eof);
}

// --- 回归风险：关键字前缀的标识符 ---

TEST(LexerTest, RegressionLetFollowedByDigitIsIdent) {
    // 'let1' 应为 Ident，不是 KwLet
    auto state = qppjs::lexer_init("let1");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::Ident);
    EXPECT_EQ(tok.range.length, 4u);
}

TEST(LexerTest, RegressionLettersIsIdent) {
    // 'letters' 以 'let' 开头但更长，应为 Ident
    auto state = qppjs::lexer_init("letters");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::Ident);
    EXPECT_EQ(tok.range.length, 7u);
}

TEST(LexerTest, RegressionWhile2IsIdent) {
    // 'while2' 关键字前缀 + 数字，应为 Ident
    auto state = qppjs::lexer_init("while2");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::Ident);
    EXPECT_EQ(tok.range.length, 6u);
}

TEST(LexerTest, RegressionAllKeywordPrefixIdents) {
    // 验证各关键字加后缀后均为 Ident 而非关键字
    struct Case { const char* src; uint32_t len; };
    const Case cases[] = {
        {"const2",     6u},
        {"var_",       4u},
        {"ifx",        3u},
        {"else_",      5u},
        {"forx",       4u},
        {"break_",     6u},
        {"continue1",  9u},
        {"return_",    7u},
        {"function1", 9u},
        {"truefalse", 9u},
        {"nullx",      5u},
        {"newx",       4u},
        {"typeof_",    7u},
        {"throwx",     6u},
    };
    for (const auto& c : cases) {
        auto state = qppjs::lexer_init(std::string_view(c.src, c.len));
        auto tok = qppjs::next_token(state);
        EXPECT_EQ(tok.kind, TokenKind::Ident) << "failed for: " << c.src;
        EXPECT_EQ(tok.range.length, c.len) << "length mismatch for: " << c.src;
    }
}

TEST(LexerTest, RegressionCommentThenToken) {
    // '/* x */foo' -> 注释后紧跟 Ident(foo)，range 正确
    auto tokens = collect_tokens("/* x */foo");
    ASSERT_EQ(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].kind, TokenKind::Ident);
    EXPECT_EQ(tokens[0].range.offset, 7u);
    EXPECT_EQ(tokens[0].range.length, 3u);
    EXPECT_EQ(tokens[1].kind, TokenKind::Eof);
}

TEST(LexerTest, RegressionEmptyBlockComment) {
    // '/**/' 空块注释，合法，结果为 Eof
    auto state = qppjs::lexer_init("/**/");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::Eof);
}

TEST(LexerTest, RegressionNestedBlockCommentNotSupported) {
    // '/* /* */ extra */' 嵌套块注释不受支持
    // 第一个 '*/' 关闭注释，后续 'extra' 和 '*/' 暴露为普通 token
    // 期望：Ident("extra") + Star + Slash + Eof
    auto tokens = collect_tokens("/* /* */ extra */");
    // 注释 "/* /* */" 结束后，剩余 " extra */" 被正常扫描
    ASSERT_GE(tokens.size(), 3u);
    EXPECT_EQ(tokens[0].kind, TokenKind::Ident);  // "extra"
    EXPECT_EQ(tokens[1].kind, TokenKind::Star);
    EXPECT_EQ(tokens[2].kind, TokenKind::Slash);
}

// --- 数字字面量后续 token 的 range 验证（回归：偏移量不因 Invalid 混乱）---

TEST(LexerTest, RegressionInvalidNumberRangeDoesNotCorruptNextToken) {
    // '123abc def' -> Invalid("123abc") + Ident("def")
    // 验证 Invalid 消耗完 ident_part 后，下一个 token 的 offset 正确
    auto tokens = collect_tokens("123abc def");
    ASSERT_EQ(tokens.size(), 3u);
    EXPECT_EQ(tokens[0].kind, TokenKind::Invalid);
    EXPECT_EQ(tokens[0].range.offset, 0u);
    EXPECT_EQ(tokens[0].range.length, 6u);  // "123abc"
    EXPECT_EQ(tokens[1].kind, TokenKind::Ident);
    EXPECT_EQ(tokens[1].range.offset, 7u);  // 空格后
    EXPECT_EQ(tokens[1].range.length, 3u);  // "def"
}

TEST(LexerTest, RegressionUnclosedStringThenNewline) {
    // '"hello\n' 未闭合字符串（'\n' 中断），Invalid 范围不包含 '\n'
    // scan_string 在 '\n' 处返回 Invalid，pos 停在 '\n' 前
    const char src[] = "\"hello\nworld\"";
    auto tokens = collect_tokens(std::string_view(src, sizeof(src) - 1));
    // 预期：Invalid("hello...) + Ident/String("world") + ...
    // 关键：第一个 token 为 Invalid
    EXPECT_EQ(tokens[0].kind, TokenKind::Invalid);
    // 第二个 token 从换行后开始，应为 Ident "world" 或 String "world"
    // 实际上 '\n' 后是 'world"'，没有开头引号，所以是 Ident "world" 后跟 Invalid('"')
    EXPECT_EQ(tokens[1].kind, TokenKind::Ident);
}

// --- 特殊字符的 Invalid 行为 ---

TEST(LexerTest, AtSignIsInvalid) {
    // '@' 不是合法 JS token 起始字符，Invalid，range.length == 1
    auto state = qppjs::lexer_init("@");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::Invalid);
    EXPECT_EQ(tok.range.length, 1u);
}

TEST(LexerTest, HashIsInvalid) {
    // '#' 不是合法 JS token 起始字符（当前阶段不支持私有字段），Invalid
    auto state = qppjs::lexer_init("#");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::Invalid);
    EXPECT_EQ(tok.range.length, 1u);
}

TEST(LexerTest, BacktickIsInvalid) {
    // '`' 模板字符串，当前未实现，Invalid
    auto state = qppjs::lexer_init("`");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::Invalid);
    EXPECT_EQ(tok.range.length, 1u);
}

// --- 数字字面量：点起始数字的边界 ---

TEST(LexerTest, DotStartNumberWithExponent) {
    // '.5e2' 合法
    auto state = qppjs::lexer_init(".5e2");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::Number);
    EXPECT_EQ(tok.range.length, 4u);
}

TEST(LexerTest, DotStartNumberWithSignedExponent) {
    // '.5e-2' 合法
    auto state = qppjs::lexer_init(".5e-2");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::Number);
    EXPECT_EQ(tok.range.length, 5u);
}

TEST(LexerTest, DotStartNumberInvalidLetterSuffix) {
    // '.5abc' 点起始数字后跟字母 -> Invalid
    auto state = qppjs::lexer_init(".5abc");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::Invalid);
}

// --- LexerState 初始状态验证 ---

TEST(LexerTest, UndefinedIsIdent) {
    // undefined 不是保留字，应词法化为 Ident
    auto state = qppjs::lexer_init("undefined");
    auto tok = qppjs::next_token(state);
    EXPECT_EQ(tok.kind, TokenKind::Ident);
    EXPECT_EQ(tok.range.offset, 0u);
    EXPECT_EQ(tok.range.length, 9u);
}

TEST(LexerTest, LexerInitState) {
    // lexer_init 后 pos==0，line==1，got_lf==false
    auto state = qppjs::lexer_init("hello");
    EXPECT_EQ(state.pos, 0u);
    EXPECT_EQ(state.line, 1u);
    EXPECT_FALSE(state.got_lf);
}

TEST(LexerTest, GotLfResetsBetweenTokens) {
    // got_lf 在每次 next_token 调用时重置
    auto state = qppjs::lexer_init("a\nb");
    qppjs::next_token(state);  // 消耗 'a'，got_lf == false
    EXPECT_FALSE(state.got_lf);
    qppjs::next_token(state);  // 跳过 '\n'，消耗 'b'，got_lf == true
    EXPECT_TRUE(state.got_lf);
    // 初始状态下 got_lf 应该在 'a' 的 token 时为 false
    // 注意：第一次 next_token 调用时，'\n' 还没被扫描
}
