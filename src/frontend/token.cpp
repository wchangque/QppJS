#include "qppjs/frontend/token.h"

namespace qppjs {

bool is_keyword(TokenKind kind) { return kind >= TokenKind::KwLet && kind <= TokenKind::KwFinally; }

std::string_view token_kind_name(TokenKind kind) {
    switch (kind) {
        case TokenKind::Eof:
            return "Eof";
        case TokenKind::Number:
            return "Number";
        case TokenKind::String:
            return "String";
        case TokenKind::Ident:
            return "Ident";
        case TokenKind::KwLet:
            return "KwLet";
        case TokenKind::KwConst:
            return "KwConst";
        case TokenKind::KwVar:
            return "KwVar";
        case TokenKind::KwIf:
            return "KwIf";
        case TokenKind::KwElse:
            return "KwElse";
        case TokenKind::KwWhile:
            return "KwWhile";
        case TokenKind::KwFor:
            return "KwFor";
        case TokenKind::KwBreak:
            return "KwBreak";
        case TokenKind::KwContinue:
            return "KwContinue";
        case TokenKind::KwReturn:
            return "KwReturn";
        case TokenKind::KwFunction:
            return "KwFunction";
        case TokenKind::KwTrue:
            return "KwTrue";
        case TokenKind::KwFalse:
            return "KwFalse";
        case TokenKind::KwNull:
            return "KwNull";
        case TokenKind::KwUndefined:
            return "KwUndefined";
        case TokenKind::KwNew:
            return "KwNew";
        case TokenKind::KwDelete:
            return "KwDelete";
        case TokenKind::KwTypeof:
            return "KwTypeof";
        case TokenKind::KwVoid:
            return "KwVoid";
        case TokenKind::KwThrow:
            return "KwThrow";
        case TokenKind::KwTry:
            return "KwTry";
        case TokenKind::KwCatch:
            return "KwCatch";
        case TokenKind::KwFinally:
            return "KwFinally";
        case TokenKind::LParen:
            return "LParen";
        case TokenKind::RParen:
            return "RParen";
        case TokenKind::LBrace:
            return "LBrace";
        case TokenKind::RBrace:
            return "RBrace";
        case TokenKind::LBracket:
            return "LBracket";
        case TokenKind::RBracket:
            return "RBracket";
        case TokenKind::Semicolon:
            return "Semicolon";
        case TokenKind::Colon:
            return "Colon";
        case TokenKind::Comma:
            return "Comma";
        case TokenKind::Dot:
            return "Dot";
        case TokenKind::Question:
            return "Question";
        case TokenKind::Plus:
            return "Plus";
        case TokenKind::Minus:
            return "Minus";
        case TokenKind::Star:
            return "Star";
        case TokenKind::Slash:
            return "Slash";
        case TokenKind::Percent:
            return "Percent";
        case TokenKind::Lt:
            return "Lt";
        case TokenKind::Gt:
            return "Gt";
        case TokenKind::LtEq:
            return "LtEq";
        case TokenKind::GtEq:
            return "GtEq";
        case TokenKind::EqEq:
            return "EqEq";
        case TokenKind::BangEq:
            return "BangEq";
        case TokenKind::EqEqEq:
            return "EqEqEq";
        case TokenKind::BangEqEq:
            return "BangEqEq";
        case TokenKind::Eq:
            return "Eq";
        case TokenKind::PlusEq:
            return "PlusEq";
        case TokenKind::MinusEq:
            return "MinusEq";
        case TokenKind::StarEq:
            return "StarEq";
        case TokenKind::SlashEq:
            return "SlashEq";
        case TokenKind::PercentEq:
            return "PercentEq";
        case TokenKind::AmpAmp:
            return "AmpAmp";
        case TokenKind::PipePipe:
            return "PipePipe";
        case TokenKind::Bang:
            return "Bang";
        case TokenKind::Amp:
            return "Amp";
        case TokenKind::Pipe:
            return "Pipe";
        case TokenKind::Caret:
            return "Caret";
        case TokenKind::Tilde:
            return "Tilde";
        case TokenKind::PlusPlus:
            return "PlusPlus";
        case TokenKind::MinusMinus:
            return "MinusMinus";
        case TokenKind::Arrow:
            return "Arrow";
        case TokenKind::Invalid:
            return "Invalid";
    }
    return "Unknown";
}

SourceLocation compute_location(std::string_view source, uint32_t offset) {
    uint32_t line = 1;
    uint32_t col = 1;
    uint32_t i = 0;
    while (i < offset && i < static_cast<uint32_t>(source.size())) {
        if (source[i] == '\r') {
            // \r\n 计为单个换行
            if (i + 1 < static_cast<uint32_t>(source.size()) && source[i + 1] == '\n') {
                ++i;
            }
            ++line;
            col = 1;
        } else if (source[i] == '\n') {
            ++line;
            col = 1;
        } else {
            ++col;
        }
        ++i;
    }
    return {line, col};
}

}  // namespace qppjs
