#pragma once

#include <cstdint>
#include <string_view>

namespace qppjs {

enum class TokenKind {
    Eof,

    // 字面量
    Number,
    String,
    Ident,

    // 关键字（连续排列，is_keyword 用区间判断）
    KwLet,
    KwConst,
    KwVar,
    KwIf,
    KwElse,
    KwWhile,
    KwFor,
    KwBreak,
    KwContinue,
    KwReturn,
    KwFunction,
    KwTrue,
    KwFalse,
    KwNull,
    KwUndefined,
    KwNew,
    KwDelete,
    KwTypeof,
    KwVoid,
    KwThrow,
    KwTry,
    KwCatch,
    KwFinally,

    // 标点
    LParen,
    RParen,
    LBrace,
    RBrace,
    LBracket,
    RBracket,
    Semicolon,
    Colon,
    Comma,
    Dot,
    Question,

    // 操作符
    Plus,
    Minus,
    Star,
    Slash,
    Percent,
    Lt,
    Gt,
    LtEq,
    GtEq,
    EqEq,
    BangEq,
    EqEqEq,
    BangEqEq,
    Eq,
    PlusEq,
    MinusEq,
    StarEq,
    SlashEq,
    PercentEq,
    AmpAmp,
    PipePipe,
    Bang,
    Amp,
    Pipe,
    Caret,
    Tilde,
    PlusPlus,
    MinusMinus,
    Arrow,

    Invalid,
};

struct SourceRange {
    uint32_t offset;
    uint32_t length;
};

struct Token {
    TokenKind kind;
    SourceRange range;
};

struct SourceLocation {
    uint32_t line;    // 1-based
    uint32_t column;  // 1-based，字节列
};

bool is_keyword(TokenKind kind);
std::string_view token_kind_name(TokenKind kind);
SourceLocation compute_location(std::string_view source, uint32_t offset);

}  // namespace qppjs
