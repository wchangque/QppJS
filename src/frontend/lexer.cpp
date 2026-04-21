#include "qppjs/frontend/lexer.h"

#include <unordered_map>

namespace qppjs {

static const std::unordered_map<std::string_view, TokenKind> kKeywords = {
    {"let",       TokenKind::KwLet},
    {"const",     TokenKind::KwConst},
    {"var",       TokenKind::KwVar},
    {"if",        TokenKind::KwIf},
    {"else",      TokenKind::KwElse},
    {"while",     TokenKind::KwWhile},
    {"for",       TokenKind::KwFor},
    {"break",     TokenKind::KwBreak},
    {"continue",  TokenKind::KwContinue},
    {"return",    TokenKind::KwReturn},
    {"function",  TokenKind::KwFunction},
    {"true",      TokenKind::KwTrue},
    {"false",     TokenKind::KwFalse},
    {"null",      TokenKind::KwNull},
    {"new",       TokenKind::KwNew},
    {"delete",    TokenKind::KwDelete},
    {"typeof",    TokenKind::KwTypeof},
    {"void",      TokenKind::KwVoid},
    {"throw",     TokenKind::KwThrow},
    {"try",       TokenKind::KwTry},
    {"catch",     TokenKind::KwCatch},
    {"finally",   TokenKind::KwFinally},
};

LexerState lexer_init(std::string_view source) {
    return {source, 0, 1, false};
}

static bool is_whitespace(char c) {
    return c == ' ' || c == '\t' || c == '\v' || c == '\f'
        || static_cast<unsigned char>(c) == 0xA0;  // U+00A0 NBSP (Latin-1)
}

static bool is_ident_start(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == '$';
}

static bool is_ident_part(char c) {
    return is_ident_start(c) || (c >= '0' && c <= '9');
}

static bool is_dec_digit(char c) {
    return c >= '0' && c <= '9';
}

static bool is_hex_digit(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

// 扫描十进制数字字面量（pos 已指向第一个字符，可能是数字或 '.'）
// 返回 true 表示合法，pos 已推进到 token 末尾
// start: token 起始位置（用于计算 Invalid 范围）
static Token scan_number(LexerState& state, uint32_t start) {
    const auto& src = state.source;
    const uint32_t len = static_cast<uint32_t>(src.size());

    // 辅助 lambda：推进到 token 末尾并返回 Invalid
    auto make_invalid = [&]() -> Token {
        // 尽量吞掉后续的 ident_part 字符，避免连锁错误
        while (state.pos < len && is_ident_part(src[state.pos])) {
            ++state.pos;
        }
        return {TokenKind::Invalid, {start, state.pos - start}};
    };

    // 点开头：.5 形式，pos 已指向 '.'
    if (state.pos < len && src[state.pos] == '.') {
        ++state.pos;  // 消耗 '.'
        // 消耗小数部分
        while (state.pos < len && is_dec_digit(src[state.pos])) {
            ++state.pos;
        }
        // 指数部分
        if (state.pos < len && (src[state.pos] == 'e' || src[state.pos] == 'E')) {
            ++state.pos;
            if (state.pos < len && (src[state.pos] == '+' || src[state.pos] == '-')) {
                ++state.pos;
            }
            if (state.pos >= len || !is_dec_digit(src[state.pos])) {
                return make_invalid();
            }
            while (state.pos < len && is_dec_digit(src[state.pos])) {
                ++state.pos;
            }
        }
        // 后缀检查
        if (state.pos < len && is_ident_start(src[state.pos])) {
            return make_invalid();
        }
        return {TokenKind::Number, {start, state.pos - start}};
    }

    // 以 '0' 开头：可能是 0x/0b/0o 或单独 0 或前导零错误
    if (src[state.pos] == '0') {
        ++state.pos;
        if (state.pos >= len) {
            return {TokenKind::Number, {start, state.pos - start}};
        }
        char next = src[state.pos];

        if (next == 'x' || next == 'X') {
            ++state.pos;
            if (state.pos >= len || !is_hex_digit(src[state.pos])) {
                return make_invalid();
            }
            while (state.pos < len && is_hex_digit(src[state.pos])) {
                ++state.pos;
            }
            if (state.pos < len && is_ident_start(src[state.pos])) {
                return make_invalid();
            }
            return {TokenKind::Number, {start, state.pos - start}};
        }

        if (next == 'b' || next == 'B') {
            ++state.pos;
            if (state.pos >= len || (src[state.pos] != '0' && src[state.pos] != '1')) {
                return make_invalid();
            }
            while (state.pos < len && (src[state.pos] == '0' || src[state.pos] == '1')) {
                ++state.pos;
            }
            // 后跟其他数字或字母 -> Invalid
            if (state.pos < len && (is_dec_digit(src[state.pos]) || is_ident_start(src[state.pos]))) {
                return make_invalid();
            }
            return {TokenKind::Number, {start, state.pos - start}};
        }

        if (next == 'o' || next == 'O') {
            ++state.pos;
            if (state.pos >= len || src[state.pos] < '0' || src[state.pos] > '7') {
                return make_invalid();
            }
            while (state.pos < len && src[state.pos] >= '0' && src[state.pos] <= '7') {
                ++state.pos;
            }
            // 后跟其他数字或字母 -> Invalid
            if (state.pos < len && (is_dec_digit(src[state.pos]) || is_ident_start(src[state.pos]))) {
                return make_invalid();
            }
            return {TokenKind::Number, {start, state.pos - start}};
        }

        // '0' 后跟十进制数字 -> 前导零，Invalid
        if (is_dec_digit(next)) {
            return make_invalid();
        }

        // '0' 后跟 '.' -> 0.xxx
        if (next == '.') {
            ++state.pos;
            while (state.pos < len && is_dec_digit(src[state.pos])) {
                ++state.pos;
            }
            // 指数
            if (state.pos < len && (src[state.pos] == 'e' || src[state.pos] == 'E')) {
                ++state.pos;
                if (state.pos < len && (src[state.pos] == '+' || src[state.pos] == '-')) {
                    ++state.pos;
                }
                if (state.pos >= len || !is_dec_digit(src[state.pos])) {
                    return make_invalid();
                }
                while (state.pos < len && is_dec_digit(src[state.pos])) {
                    ++state.pos;
                }
            }
            if (state.pos < len && is_ident_start(src[state.pos])) {
                return make_invalid();
            }
            return {TokenKind::Number, {start, state.pos - start}};
        }

        // '0' 后跟 'e'/'E' -> 0e3
        if (next == 'e' || next == 'E') {
            ++state.pos;
            if (state.pos < len && (src[state.pos] == '+' || src[state.pos] == '-')) {
                ++state.pos;
            }
            if (state.pos >= len || !is_dec_digit(src[state.pos])) {
                return make_invalid();
            }
            while (state.pos < len && is_dec_digit(src[state.pos])) {
                ++state.pos;
            }
            if (state.pos < len && is_ident_start(src[state.pos])) {
                return make_invalid();
            }
            return {TokenKind::Number, {start, state.pos - start}};
        }

        // 单独 '0'（后跟标点/空白/eof）
        if (state.pos < len && is_ident_start(src[state.pos])) {
            return make_invalid();
        }
        return {TokenKind::Number, {start, state.pos - start}};
    }

    // 1-9 开头
    while (state.pos < len && is_dec_digit(src[state.pos])) {
        ++state.pos;
    }

    // 可选小数部分
    if (state.pos < len && src[state.pos] == '.') {
        ++state.pos;
        while (state.pos < len && is_dec_digit(src[state.pos])) {
            ++state.pos;
        }
    }

    // 可选指数部分
    if (state.pos < len && (src[state.pos] == 'e' || src[state.pos] == 'E')) {
        ++state.pos;
        if (state.pos < len && (src[state.pos] == '+' || src[state.pos] == '-')) {
            ++state.pos;
        }
        if (state.pos >= len || !is_dec_digit(src[state.pos])) {
            return make_invalid();
        }
        while (state.pos < len && is_dec_digit(src[state.pos])) {
            ++state.pos;
        }
    }

    // 后缀检查：不允许紧跟 ASCII 字母、_ 或 $
    if (state.pos < len && is_ident_start(src[state.pos])) {
        return make_invalid();
    }

    return {TokenKind::Number, {start, state.pos - start}};
}

// 扫描字符串字面量，pos 已指向开头引号
static Token scan_string(LexerState& state, uint32_t start) {
    const auto& src = state.source;
    const uint32_t len = static_cast<uint32_t>(src.size());

    char quote = src[state.pos];
    ++state.pos;  // 消耗开头引号

    auto make_invalid = [&]() -> Token {
        return {TokenKind::Invalid, {start, state.pos - start}};
    };

    while (state.pos < len) {
        char c = src[state.pos];

        // 行终止符 -> Invalid（未闭合）
        if (c == '\n' || c == '\r') {
            return make_invalid();
        }

        // 闭合引号
        if (c == quote) {
            ++state.pos;
            return {TokenKind::String, {start, state.pos - start}};
        }

        // 转义序列
        if (c == '\\') {
            ++state.pos;
            if (state.pos >= len) {
                return make_invalid();
            }
            char esc = src[state.pos];
            ++state.pos;

            if (esc == 'x') {
                // \xNN：必须恰好两位十六进制
                if (state.pos + 2 > len
                    || !is_hex_digit(src[state.pos])
                    || !is_hex_digit(src[state.pos + 1])) {
                    return make_invalid();
                }
                state.pos += 2;
            } else if (esc == 'u') {
                // \uNNNN：必须恰好四位十六进制
                if (state.pos + 4 > len
                    || !is_hex_digit(src[state.pos])
                    || !is_hex_digit(src[state.pos + 1])
                    || !is_hex_digit(src[state.pos + 2])
                    || !is_hex_digit(src[state.pos + 3])) {
                    return make_invalid();
                }
                state.pos += 4;
            } else if (esc == '0') {
                // \0 后跟数字 -> Invalid（遗留八进制）
                if (state.pos < len && is_dec_digit(src[state.pos])) {
                    return make_invalid();
                }
            } else if (esc == '\n') {
                // LineContinuation: \ + LF，行号递增
                ++state.line;
            } else if (esc == '\r') {
                // LineContinuation: \ + CR 或 \ + CRLF，整体消耗
                ++state.line;
                if (state.pos < len && src[state.pos] == '\n') {
                    ++state.pos;
                }
            }
            // 其他转义字符（\t \\ \' \" \b \f \v 及未识别）均合法，继续
            continue;
        }

        ++state.pos;
    }

    // 到达 EOF 未闭合
    return make_invalid();
}

Token next_token(LexerState& state) {
    state.got_lf = false;

    const auto& src = state.source;
    const uint32_t len = static_cast<uint32_t>(src.size());

    // 跳过空白和注释
    while (state.pos < len) {
        char c = src[state.pos];

        // 换行
        if (c == '\n') {
            state.got_lf = true;
            ++state.line;
            ++state.pos;
            continue;
        }
        if (c == '\r') {
            state.got_lf = true;
            ++state.line;
            ++state.pos;
            // \r\n 计为单个换行
            if (state.pos < len && src[state.pos] == '\n') {
                ++state.pos;
            }
            continue;
        }

        // 水平空白（含 NBSP U+00A0，在 Latin-1 中为 0xA0）
        if (is_whitespace(c)) {
            ++state.pos;
            continue;
        }

        // 单行注释 //
        if (c == '/' && state.pos + 1 < len && src[state.pos + 1] == '/') {
            state.pos += 2;
            while (state.pos < len && src[state.pos] != '\n' && src[state.pos] != '\r') {
                ++state.pos;
            }
            continue;
        }

        // 块注释 /* */
        if (c == '/' && state.pos + 1 < len && src[state.pos + 1] == '*') {
            uint32_t blk_start = state.pos;
            state.pos += 2;
            bool closed = false;
            while (state.pos + 1 < len) {
                char cc = src[state.pos];
                if (cc == '\n') {
                    state.got_lf = true;
                    ++state.line;
                    ++state.pos;
                } else if (cc == '\r') {
                    state.got_lf = true;
                    ++state.line;
                    ++state.pos;
                    if (state.pos < len && src[state.pos] == '\n') {
                        ++state.pos;
                    }
                } else if (cc == '*' && src[state.pos + 1] == '/') {
                    state.pos += 2;
                    closed = true;
                    break;
                } else {
                    ++state.pos;
                }
            }
            if (!closed) {
                return {TokenKind::Invalid, {blk_start, len - blk_start}};
            }
            continue;
        }

        // 遇到非空白非注释字符，退出跳过循环
        break;
    }

    // Eof
    if (state.pos >= len) {
        return {TokenKind::Eof, {state.pos, 0}};
    }

    uint32_t start = state.pos;
    char c = src[state.pos];

    // 字符串字面量
    if (c == '\'' || c == '"') {
        return scan_string(state, start);
    }

    // 数字字面量（0-9 开头）
    if (is_dec_digit(c)) {
        return scan_number(state, start);
    }

    // 点开头：lookahead 判断是否为数字字面量
    if (c == '.') {
        if (state.pos + 1 < len && is_dec_digit(src[state.pos + 1])) {
            return scan_number(state, start);
        }
        ++state.pos;
        return {TokenKind::Dot, {start, 1}};
    }

    // 多字符操作符与单字符操作符（最长匹配）
    auto peek = [&](uint32_t offset) -> char {
        uint32_t p = state.pos + offset;
        return (p < len) ? src[p] : '\0';
    };

    switch (c) {
        case '=':
            if (peek(1) == '=') {
                if (peek(2) == '=') { state.pos += 3; return {TokenKind::EqEqEq, {start, 3}}; }
                state.pos += 2; return {TokenKind::EqEq, {start, 2}};
            }
            if (peek(1) == '>') { state.pos += 2; return {TokenKind::Arrow, {start, 2}}; }
            ++state.pos; return {TokenKind::Eq, {start, 1}};

        case '!':
            if (peek(1) == '=') {
                if (peek(2) == '=') { state.pos += 3; return {TokenKind::BangEqEq, {start, 3}}; }
                state.pos += 2; return {TokenKind::BangEq, {start, 2}};
            }
            ++state.pos; return {TokenKind::Bang, {start, 1}};

        case '<':
            if (peek(1) == '=') { state.pos += 2; return {TokenKind::LtEq, {start, 2}}; }
            ++state.pos; return {TokenKind::Lt, {start, 1}};

        case '>':
            if (peek(1) == '=') { state.pos += 2; return {TokenKind::GtEq, {start, 2}}; }
            ++state.pos; return {TokenKind::Gt, {start, 1}};

        case '+':
            if (peek(1) == '+') { state.pos += 2; return {TokenKind::PlusPlus, {start, 2}}; }
            if (peek(1) == '=') { state.pos += 2; return {TokenKind::PlusEq, {start, 2}}; }
            ++state.pos; return {TokenKind::Plus, {start, 1}};

        case '-':
            if (peek(1) == '-') { state.pos += 2; return {TokenKind::MinusMinus, {start, 2}}; }
            if (peek(1) == '=') { state.pos += 2; return {TokenKind::MinusEq, {start, 2}}; }
            ++state.pos; return {TokenKind::Minus, {start, 1}};

        case '*':
            if (peek(1) == '=') { state.pos += 2; return {TokenKind::StarEq, {start, 2}}; }
            ++state.pos; return {TokenKind::Star, {start, 1}};

        case '/':
            if (peek(1) == '=') { state.pos += 2; return {TokenKind::SlashEq, {start, 2}}; }
            ++state.pos; return {TokenKind::Slash, {start, 1}};

        case '%':
            if (peek(1) == '=') { state.pos += 2; return {TokenKind::PercentEq, {start, 2}}; }
            ++state.pos; return {TokenKind::Percent, {start, 1}};

        case '&':
            if (peek(1) == '&') { state.pos += 2; return {TokenKind::AmpAmp, {start, 2}}; }
            ++state.pos; return {TokenKind::Amp, {start, 1}};

        case '|':
            if (peek(1) == '|') { state.pos += 2; return {TokenKind::PipePipe, {start, 2}}; }
            ++state.pos; return {TokenKind::Pipe, {start, 1}};

        case '^': ++state.pos; return {TokenKind::Caret, {start, 1}};
        case '~': ++state.pos; return {TokenKind::Tilde, {start, 1}};

        // 单字符标点
        case '(': ++state.pos; return {TokenKind::LParen,    {start, 1}};
        case ')': ++state.pos; return {TokenKind::RParen,    {start, 1}};
        case '{': ++state.pos; return {TokenKind::LBrace,    {start, 1}};
        case '}': ++state.pos; return {TokenKind::RBrace,    {start, 1}};
        case '[': ++state.pos; return {TokenKind::LBracket,  {start, 1}};
        case ']': ++state.pos; return {TokenKind::RBracket,  {start, 1}};
        case ';': ++state.pos; return {TokenKind::Semicolon, {start, 1}};
        case ':': ++state.pos; return {TokenKind::Colon,     {start, 1}};
        case ',': ++state.pos; return {TokenKind::Comma,     {start, 1}};
        case '?': ++state.pos; return {TokenKind::Question,  {start, 1}};
        default: break;
    }

    // 标识符与关键字
    if (is_ident_start(c)) {
        ++state.pos;
        while (state.pos < len && is_ident_part(src[state.pos])) {
            ++state.pos;
        }
        uint32_t ident_len = state.pos - start;
        std::string_view text = src.substr(start, ident_len);
        auto it = kKeywords.find(text);
        if (it != kKeywords.end()) {
            return {it->second, {start, ident_len}};
        }
        return {TokenKind::Ident, {start, ident_len}};
    }

    // 其他字符 -> Invalid，pos 推进 1
    ++state.pos;
    return {TokenKind::Invalid, {start, 1}};
}

} // namespace qppjs
