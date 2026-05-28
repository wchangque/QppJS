#include "qppjs/frontend/parser.h"

#include "qppjs/frontend/lexer.h"

#include <cassert>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_set>

namespace qppjs {

// ---- 字符串解码 ----

static bool is_hex_digit_char(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return c - 'A' + 10;
}

// 把 token 原始文本（含引号）解码为 std::string
static std::string decode_string(std::string_view raw) {
    // raw 包含首尾引号
    std::string result;
    result.reserve(raw.size());
    std::size_t i = 1;                 // 跳过开头引号
    std::size_t end = raw.size() - 1;  // 跳过结尾引号
    while (i < end) {
        char c = raw[i];
        if (c != '\\') {
            result += c;
            ++i;
            continue;
        }
        // 转义序列
        ++i;
        if (i >= end) break;
        char esc = raw[i];
        ++i;
        switch (esc) {
            case 'n':
                result += '\n';
                break;
            case 't':
                result += '\t';
                break;
            case 'r':
                result += '\r';
                break;
            case 'b':
                result += '\b';
                break;
            case 'f':
                result += '\f';
                break;
            case 'v':
                result += '\v';
                break;
            case '\\':
                result += '\\';
                break;
            case '\'':
                result += '\'';
                break;
            case '"':
                result += '"';
                break;
            case '0':
                result += '\0';
                break;
            case 'x': {
                // \xNN
                if (i + 2 <= end && is_hex_digit_char(raw[i]) && is_hex_digit_char(raw[i + 1])) {
                    int val = hex_val(raw[i]) * 16 + hex_val(raw[i + 1]);
                    result += static_cast<char>(val);
                    i += 2;
                }
                break;
            }
            case 'u': {
                // \uNNNN
                auto read_hex4 = [&](std::size_t pos) -> int {
                    if (pos + 4 > end) return -1;
                    if (!is_hex_digit_char(raw[pos]) || !is_hex_digit_char(raw[pos + 1]) ||
                        !is_hex_digit_char(raw[pos + 2]) || !is_hex_digit_char(raw[pos + 3]))
                        return -1;
                    return hex_val(raw[pos]) << 12 | hex_val(raw[pos + 1]) << 8 | hex_val(raw[pos + 2]) << 4 |
                           hex_val(raw[pos + 3]);
                };
                int hi = read_hex4(i);
                if (hi < 0) break;
                i += 4;
                uint32_t cp = static_cast<uint32_t>(hi);
                // 高代理：尝试消费后续 \uNNNN 低代理，合并为非 BMP 码点
                if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < end && raw[i] == '\\' && raw[i + 1] == 'u') {
                    int lo = read_hex4(i + 2);
                    if (lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (static_cast<uint32_t>(lo) - 0xDC00);
                        i += 6;  // 消费 \uNNNN
                    }
                }
                // 编码为 UTF-8
                if (cp < 0x80) {
                    result += static_cast<char>(cp);
                } else if (cp < 0x800) {
                    result += static_cast<char>(0xC0 | (cp >> 6));
                    result += static_cast<char>(0x80 | (cp & 0x3F));
                } else if (cp < 0x10000) {
                    result += static_cast<char>(0xE0 | (cp >> 12));
                    result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                    result += static_cast<char>(0x80 | (cp & 0x3F));
                } else {
                    result += static_cast<char>(0xF0 | (cp >> 18));
                    result += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                    result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                    result += static_cast<char>(0x80 | (cp & 0x3F));
                }
                break;
            }
            case '\n':
                // LineContinuation: \LF — 跳过，不追加任何字节
                break;
            case '\r':
                // LineContinuation: \CR 或 \CRLF — 跳过
                if (i < end && raw[i] == '\n') ++i;
                break;
            default:
                // 其他 \X 保留 X
                result += esc;
                break;
        }
    }
    return result;
}

// 解码模板字符串的 cooked 文本（不含起始/结束定界符的内容部分）
// raw_text: 不含起始 ` 或 }、不含结束 ` 或 ${ 的原始内容
// 返回 nullopt 表示存在非法转义（SyntaxError）
static std::optional<std::string> decode_template_cooked(std::string_view raw_text) {
    std::string result;
    result.reserve(raw_text.size());
    std::size_t i = 0;
    const std::size_t end = raw_text.size();
    while (i < end) {
        char c = raw_text[i];
        if (c != '\\') {
            // 规范化换行：\r\n 和 \r 均转为 \n
            if (c == '\r') {
                result += '\n';
                ++i;
                if (i < end && raw_text[i] == '\n') ++i;
            } else {
                result += c;
                ++i;
            }
            continue;
        }
        // 转义序列
        ++i;
        if (i >= end) break;
        char esc = raw_text[i];
        ++i;
        switch (esc) {
            case 'n':  result += '\n'; break;
            case 't':  result += '\t'; break;
            case 'r':  result += '\r'; break;
            case 'b':  result += '\b'; break;
            case 'f':  result += '\f'; break;
            case 'v':  result += '\v'; break;
            case '\\': result += '\\'; break;
            case '`':  result += '`';  break;
            case '$':  result += '$';  break;
            case '\'': result += '\''; break;
            case '"':  result += '"';  break;
            case '0': {
                if (i < end && is_hex_digit_char(raw_text[i]) && raw_text[i] >= '0' && raw_text[i] <= '9') {
                    // \01 等遗留八进制 -> 非法
                    return std::nullopt;
                }
                result += '\0';
                break;
            }
            case '1': case '2': case '3': case '4':
            case '5': case '6': case '7':
            case '8': case '9':
                // 遗留八进制转义 \1-\9 在模板字符串中均为 NotEscapeSequence
                return std::nullopt;
            case 'x': {
                if (i + 2 <= end && is_hex_digit_char(raw_text[i]) && is_hex_digit_char(raw_text[i + 1])) {
                    int val = hex_val(raw_text[i]) * 16 + hex_val(raw_text[i + 1]);
                    result += static_cast<char>(val);
                    i += 2;
                } else {
                    return std::nullopt;
                }
                break;
            }
            case 'u': {
                if (i < end && raw_text[i] == '{') {
                    // \u{H...}
                    ++i;
                    uint32_t cp = 0;
                    bool has_digit = false;
                    while (i < end && raw_text[i] != '}') {
                        if (!is_hex_digit_char(raw_text[i])) return std::nullopt;
                        cp = cp * 16 + static_cast<uint32_t>(hex_val(raw_text[i]));
                        ++i;
                        has_digit = true;
                    }
                    if (!has_digit || i >= end) return std::nullopt;
                    ++i;  // 消耗 }
                    if (cp > 0x10FFFF) return std::nullopt;
                    // 编码为 UTF-8
                    if (cp < 0x80) {
                        result += static_cast<char>(cp);
                    } else if (cp < 0x800) {
                        result += static_cast<char>(0xC0 | (cp >> 6));
                        result += static_cast<char>(0x80 | (cp & 0x3F));
                    } else if (cp < 0x10000) {
                        result += static_cast<char>(0xE0 | (cp >> 12));
                        result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                        result += static_cast<char>(0x80 | (cp & 0x3F));
                    } else {
                        result += static_cast<char>(0xF0 | (cp >> 18));
                        result += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                        result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                        result += static_cast<char>(0x80 | (cp & 0x3F));
                    }
                } else {
                    // \uNNNN
                    auto read_hex4 = [&](std::size_t pos) -> int {
                        if (pos + 4 > end) return -1;
                        if (!is_hex_digit_char(raw_text[pos]) || !is_hex_digit_char(raw_text[pos + 1]) ||
                            !is_hex_digit_char(raw_text[pos + 2]) || !is_hex_digit_char(raw_text[pos + 3]))
                            return -1;
                        return hex_val(raw_text[pos]) << 12 | hex_val(raw_text[pos + 1]) << 8 |
                               hex_val(raw_text[pos + 2]) << 4 | hex_val(raw_text[pos + 3]);
                    };
                    int hi = read_hex4(i);
                    if (hi < 0) return std::nullopt;
                    i += 4;
                    uint32_t cp = static_cast<uint32_t>(hi);
                    // 高代理对
                    if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < end && raw_text[i] == '\\' && raw_text[i + 1] == 'u') {
                        int lo = read_hex4(i + 2);
                        if (lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (static_cast<uint32_t>(lo) - 0xDC00);
                            i += 6;
                        }
                    }
                    if (cp < 0x80) {
                        result += static_cast<char>(cp);
                    } else if (cp < 0x800) {
                        result += static_cast<char>(0xC0 | (cp >> 6));
                        result += static_cast<char>(0x80 | (cp & 0x3F));
                    } else if (cp < 0x10000) {
                        result += static_cast<char>(0xE0 | (cp >> 12));
                        result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                        result += static_cast<char>(0x80 | (cp & 0x3F));
                    } else {
                        result += static_cast<char>(0xF0 | (cp >> 18));
                        result += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                        result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                        result += static_cast<char>(0x80 | (cp & 0x3F));
                    }
                }
                break;
            }
            case '\n':
                // 行延续：\LF 消除
                break;
            case '\r':
                // 行延续：\CR 或 \CRLF 消除
                if (i < end && raw_text[i] == '\n') ++i;
                break;
            default:
                // 其他 \X 保留 X（如 \a → 'a'）
                result += esc;
                break;
        }
    }
    return result;
}

// ---- 数字解析 ----

static double parse_number_text(std::string_view text) {
    if (text.size() >= 2 && text[0] == '0') {
        char p = text[1];
        if (p == 'x' || p == 'X') {
            // 逐字符累加到 double，避免 stoull 的 64 位上限
            double v = 0.0;
            for (char c : text.substr(2)) {
                v = v * 16.0 + hex_val(c);
            }
            return v;
        }
        if (p == 'b' || p == 'B') {
            double v = 0.0;
            for (char c : text.substr(2)) {
                v = v * 2.0 + (c - '0');
            }
            return v;
        }
        if (p == 'o' || p == 'O') {
            double v = 0.0;
            for (char c : text.substr(2)) {
                v = v * 8.0 + (c - '0');
            }
            return v;
        }
    }
    double result = 0.0;
    auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), result);
    if (ec == std::errc{}) {
        return result;
    }
    if (ec == std::errc::result_out_of_range) {
        return std::numeric_limits<double>::infinity();
    }
    return 0.0;
}

// 将 double 格式化为属性键字符串，与 to_string_val 对 Number 的整数化逻辑保持一致
static std::string number_to_property_key(double n) {
    if (std::isnan(n)) return "NaN";
    if (std::isinf(n)) return n > 0 ? "Infinity" : "-Infinity";
    if (n == static_cast<double>(static_cast<long long>(n)) && std::abs(n) < 1e15) {
        std::ostringstream oss;
        oss << static_cast<long long>(n);
        return oss.str();
    }
    std::ostringstream oss;
    oss << n;
    return oss.str();
}

// ---- 错误构造辅助 ----

static Error make_parse_error(std::string_view source, const Token& tok, std::string_view msg) {
    auto loc = compute_location(source, tok.range.offset);
    std::string full_msg =
            "line " + std::to_string(loc.line) + ", column " + std::to_string(loc.column) + ": " + std::string(msg);
    return Error{ErrorKind::Syntax, std::move(full_msg)};
}

// 从 SourceRange 计算结束偏移（offset + length）
static uint32_t range_end(SourceRange r) { return r.offset + r.length; }

// 构造覆盖 [start_offset, end_offset) 的 SourceRange
static SourceRange span(uint32_t start, uint32_t end) { return {start, end > start ? end - start : 0}; }

// 取 ExprNode 的 SourceRange
static SourceRange expr_range(const ExprNode& e) {
    return std::visit(overloaded{
                              [](const NumberLiteral& n) { return n.range; },
                              [](const StringLiteral& n) { return n.range; },
                              [](const BooleanLiteral& n) { return n.range; },
                              [](const NullLiteral& n) { return n.range; },
                              [](const Identifier& n) { return n.range; },
                              [](const UnaryExpression& n) { return n.range; },
                              [](const BinaryExpression& n) { return n.range; },
                              [](const LogicalExpression& n) { return n.range; },
                              [](const AssignmentExpression& n) { return n.range; },
                              [](const ObjectExpression& n) { return n.range; },
                              [](const MemberExpression& n) { return n.range; },
                              [](const MemberAssignmentExpression& n) { return n.range; },
                              [](const FunctionExpression& n) { return n.range; },
                              [](const CallExpression& n) { return n.range; },
                              [](const NewExpression& n) { return n.range; },
                              [](const ArrayExpression& n) { return n.range; },
                              [](const AwaitExpression& n) { return n.range; },
                              [](const UpdateExpression& n) { return n.range; },
                              [](const AsyncFunctionExpression& n) { return n.range; },
                              [](const MetaProperty& n) { return n.range; },
                              [](const ImportCallExpression& n) { return n.range; },
                              [](const RegexLiteral& n) { return n.range; },
                              [](const TemplateLiteral& n) { return n.range; },
                              [](const ArrowFunctionExpression& n) { return n.range; },
                              [](const ConditionalExpression& n) { return n.range; },
                              [](const SpreadElement& n) { return n.range; },
                              [](const DestructuringAssignmentExpression& n) { return n.range; },
                              [](const OptionalChainExpression& n) { return n.range; },
                              [](const YieldExpression& n) { return n.range; },
                      },
                      e.v);
}

// 取 StmtNode 的 SourceRange
static SourceRange stmt_range(const StmtNode& s) {
    return std::visit(overloaded{
                              [](const ExpressionStatement& n) { return n.range; },
                              [](const VariableDeclaration& n) { return n.range; },
                              [](const BlockStatement& n) { return n.range; },
                              [](const IfStatement& n) { return n.range; },
                              [](const WhileStatement& n) { return n.range; },
                              [](const ReturnStatement& n) { return n.range; },
                              [](const FunctionDeclaration& n) { return n.range; },
                              [](const AsyncFunctionDeclaration& n) { return n.range; },
                              [](const ThrowStatement& n) { return n.range; },
                              [](const TryStatement& n) { return n.range; },
                              [](const BreakStatement& n) { return n.range; },
                              [](const ContinueStatement& n) { return n.range; },
                              [](const LabeledStatement& n) { return n.range; },
                              [](const ForStatement& n) { return n.range; },
                              [](const ForInStatement& n) { return n.range; },
                              [](const ForOfStatement& n) { return n.range; },
                              [](const ImportDeclaration& n) { return n.range; },
                              [](const ExportNamedDeclaration& n) { return n.range; },
                              [](const ExportDefaultDeclaration& n) { return n.range; },
                              [](const DestructuringDeclaration& n) { return n.range; },
                      },
                      s.v);
}

// ---- Parser 状态 ----

struct Parser {
    std::string_view source;
    LexerState lex;
    Token cur;         // 当前已消费 token（lookahead）
    bool got_lf;       // cur 前是否有换行（ASI 用）
    bool is_top_level_; // import/export 只允许在顶层
    bool in_async_function_; // P2-E: await 只在 async 函数体内有效
    bool in_generator_function_; // yield 只在 generator 函数体内有效
    bool in_module_;         // TLA: 模块顶层上下文，允许 await 表达式
    bool in_module_context_; // import.meta: 模块内任意位置（含函数体）均合法
    // When true, the `in` identifier is not treated as a binary operator.
    // Set during the LHS of for...in to prevent ambiguity with the `in` keyword.
    bool no_in_ = false;

    explicit Parser(std::string_view src, bool is_module = false)
        : source(src), lex(lexer_init(src)), cur{TokenKind::Eof, {0, 0}}, got_lf(false),
          is_top_level_(true), in_async_function_(false), in_generator_function_(false),
          in_module_(is_module), in_module_context_(is_module) {
        advance();  // 载入第一个 token
    }

    static bool is_expr_end_token(TokenKind kind) {
        switch (kind) {
            case TokenKind::Ident: case TokenKind::Number: case TokenKind::String:
            case TokenKind::Regex: case TokenKind::KwTrue: case TokenKind::KwFalse:
            case TokenKind::KwNull: case TokenKind::KwThis:
            case TokenKind::RBracket: case TokenKind::RParen:
            case TokenKind::PlusPlus: case TokenKind::MinusMinus:
            case TokenKind::TemplateNoSub: case TokenKind::TemplateTail:
                return true;
            default:
                return false;
        }
    }

    // 推进一个 token，记录换行状态
    void advance() {
        lex.scan_regex = !is_expr_end_token(cur.kind);
        cur = next_token(lex);
        got_lf = lex.got_lf;
    }

    // 在解析完 ${...} 内的表达式后调用（此时 cur 应为 RBrace）。
    // 将 lex.pos 回退到 } 的位置，然后调用 scan_template_part 消耗 } 并扫描下一段。
    // 更新 cur 为 TemplateMiddle 或 TemplateTail（或 Invalid）。
    void advance_template_part() {
        // cur 是 RBrace，next_token 对 } 只推进 1 字节，所以 lex.pos - 1 即为 } 的位置
        --lex.pos;
        cur = scan_template_part(lex);
        got_lf = false;
    }

    // 返回当前 token 的原始文本
    std::string_view token_text(const Token& tok) const { return source.substr(tok.range.offset, tok.range.length); }

    // 检查当前 token 是 Ident 且文本等于 name（上下文关键字，如 from/as）
    bool is_contextual_keyword(std::string_view name) const {
        return cur.kind == TokenKind::Ident && token_text(cur) == name;
    }

    // 期望当前 token 是 kind，消费并推进；否则返回错误
    ParseResult<Token> expect(TokenKind kind) {
        if (cur.kind != kind) {
            return ParseResult<Token>::Err(make_parse_error(
                    source, cur, std::string("unexpected token: ") + std::string(token_kind_name(cur.kind))));
        }
        Token t = cur;
        advance();
        return ParseResult<Token>::Ok(t);
    }

    // 最小 ASI 分号消费：消费 ; 或自动插入
    ParseResult<Token> consume_semicolon() {
        if (cur.kind == TokenKind::Semicolon) {
            Token t = cur;
            advance();
            return ParseResult<Token>::Ok(t);
        }
        // got_lf 在 advance() 后已经更新为"当前 cur 前是否有换行"
        // 但 ASI 判断的是 cur 之前是否有换行，即上次 advance 后记录的 got_lf
        if (got_lf || cur.kind == TokenKind::RBrace || cur.kind == TokenKind::Eof) {
            return ParseResult<Token>::Ok(Token{TokenKind::Semicolon, {cur.range.offset, 0}});
        }
        return ParseResult<Token>::Err(make_parse_error(source, cur, "missing semicolon"));
    }

    // ---- 表达式解析（Pratt Parser）----

    // 返回操作符的左绑定力（0 表示非中缀操作符）
    // 优先级（低→高）：
    //   2: 赋值（=, +=, ... &=, |=, ^=, <<=, >>=, >>>=）
    //   3: =>（箭头）
    //   4: ?:（三元）、||
    //   6: &&
    //   7: | (BitOR)
    //   8: ^ (BitXOR)
    //   9: & (BitAND)
    //  11: == != === !==
    //  12: in（contextual）
    //  13: instanceof < > <= >=
    //  14: << >> >>>
    //  15: + -
    //  17: * / %
    //  19: 函数调用 (
    //  20: 后缀 ++ --
    //  21: 成员访问 . [
    static int lbp(TokenKind kind) {
        switch (kind) {
            case TokenKind::Arrow:
                return 3;
            case TokenKind::Eq:
            case TokenKind::PlusEq:
            case TokenKind::MinusEq:
            case TokenKind::StarEq:
            case TokenKind::SlashEq:
            case TokenKind::PercentEq:
            case TokenKind::AmpEq:
            case TokenKind::PipeEq:
            case TokenKind::CaretEq:
            case TokenKind::LShiftEq:
            case TokenKind::RShiftEq:
            case TokenKind::URShiftEq:
                return 2;
            case TokenKind::Question:
            case TokenKind::QuestionQuestion:
                return 3;
            case TokenKind::PipePipe:
                return 4;
            case TokenKind::AmpAmp:
                return 6;
            case TokenKind::Pipe:
                return 7;
            case TokenKind::Caret:
                return 8;
            case TokenKind::Amp:
                return 9;
            case TokenKind::EqEq:
            case TokenKind::BangEq:
            case TokenKind::EqEqEq:
            case TokenKind::BangEqEq:
                return 11;
            case TokenKind::KwInstanceof:
            case TokenKind::Lt:
            case TokenKind::Gt:
            case TokenKind::LtEq:
            case TokenKind::GtEq:
                return 13;
            case TokenKind::LShift:
            case TokenKind::RShift:
            case TokenKind::URShift:
                return 14;
            case TokenKind::Plus:
            case TokenKind::Minus:
                return 15;
            case TokenKind::Star:
            case TokenKind::Slash:
            case TokenKind::Percent:
                return 17;
            case TokenKind::LParen:
                return 19;
            case TokenKind::PlusPlus:
            case TokenKind::MinusMinus:
                return 20;
            case TokenKind::Dot:
            case TokenKind::LBracket:
            case TokenKind::QuestionDot:
                return 21;
            default:
                return 0;
        }
    }

    // 前缀处理（nud）
    ParseResult<ExprNode> nud(Token tok) {
        switch (tok.kind) {
            case TokenKind::Number: {
                auto text = token_text(tok);
                double val = parse_number_text(text);
                return ParseResult<ExprNode>::Ok(ExprNode{NumberLiteral{val, tok.range}});
            }
            case TokenKind::String: {
                auto raw = token_text(tok);
                std::string val = decode_string(raw);
                return ParseResult<ExprNode>::Ok(ExprNode{StringLiteral{std::move(val), tok.range}});
            }
            case TokenKind::KwTrue:
                return ParseResult<ExprNode>::Ok(ExprNode{BooleanLiteral{true, tok.range}});
            case TokenKind::KwFalse:
                return ParseResult<ExprNode>::Ok(ExprNode{BooleanLiteral{false, tok.range}});
            case TokenKind::KwNull:
                return ParseResult<ExprNode>::Ok(ExprNode{NullLiteral{tok.range}});
            case TokenKind::Ident: {
                std::string_view tok_text = token_text(tok);
                // async function [name](params) { body }  — async 函数表达式
                // 条件：tok 文本为 "async"，且 cur 为 KwFunction，且 tok 与 cur 之间无换行
                if (tok_text == "async" && cur.kind == TokenKind::KwFunction && !got_lf) {
                    advance();  // 消费 function
                    std::optional<std::string> fn_name;
                    if (cur.kind == TokenKind::Ident) {
                        fn_name = std::string(token_text(cur));
                        advance();
                    }
                    std::optional<std::string> fn_rest1;
                    auto params_result = parse_function_params(fn_rest1);
                    if (!params_result.ok()) return ParseResult<ExprNode>::Err(params_result.error());
                    bool saved_in_async = in_async_function_;
                    in_async_function_ = true;
                    auto body_result = parse_function_body();
                    in_async_function_ = saved_in_async;
                    if (!body_result.ok()) return ParseResult<ExprNode>::Err(body_result.error());
                    uint32_t fn_end = range_end(body_result.value().second);
                    auto body_ptr = std::make_shared<std::vector<StmtNode>>(
                        std::move(body_result.value().first));
                    return ParseResult<ExprNode>::Ok(ExprNode{AsyncFunctionExpression{
                        std::move(fn_name), std::move(params_result.value()), std::move(fn_rest1),
                        std::move(body_ptr), span(tok.range.offset, fn_end)}});
                }
                // await expr — 在 async 函数体内或模块顶层有效
                if (tok_text == "await" && !got_lf && (in_async_function_ || in_module_)) {
                    // 只有当后续不是分号/}时才解析为 await 表达式
                    if (cur.kind != TokenKind::Semicolon && cur.kind != TokenKind::RBrace &&
                        cur.kind != TokenKind::Eof) {
                        auto arg = parse_expr(18);  // 高优先级，不消费算术运算符
                        if (!arg.ok()) return arg;
                        uint32_t end = range_end(expr_range(arg.value()));
                        return ParseResult<ExprNode>::Ok(ExprNode{AwaitExpression{
                            std::make_unique<ExprNode>(std::move(arg.value())),
                            span(tok.range.offset, end)}});
                    }
                }
                // yield [*] expr — 在 generator 函数体内有效
                if (tok_text == "yield" && in_generator_function_) {
                    bool is_delegate = false;
                    if (cur.kind == TokenKind::Star) {
                        is_delegate = true;
                        advance();  // 消费 *
                    }
                    // 无参数 yield：后续是换行、};、或 EOF
                    if (!is_delegate && (got_lf || cur.kind == TokenKind::Semicolon ||
                        cur.kind == TokenKind::RBrace || cur.kind == TokenKind::Eof ||
                        cur.kind == TokenKind::RParen || cur.kind == TokenKind::RBracket ||
                        cur.kind == TokenKind::Comma || cur.kind == TokenKind::Colon)) {
                        return ParseResult<ExprNode>::Ok(ExprNode{YieldExpression{
                            false, nullptr, tok.range}});
                    }
                    // 有参数 yield：解析 AssignmentExpression
                    auto arg = parse_expr(2);
                    if (!arg.ok()) return arg;
                    uint32_t end = range_end(expr_range(arg.value()));
                    return ParseResult<ExprNode>::Ok(ExprNode{YieldExpression{
                        is_delegate,
                        std::make_unique<ExprNode>(std::move(arg.value())),
                        span(tok.range.offset, end)}});
                }
                // import(specifier) — 动态 import 表达式
                if (tok_text == "import" && cur.kind == TokenKind::LParen) {
                    advance();  // 消费 (
                    auto spec = parse_expr(2);  // 解析 specifier 表达式
                    if (!spec.ok()) return spec;
                    auto rp = expect(TokenKind::RParen);
                    if (!rp.ok()) return ParseResult<ExprNode>::Err(rp.error());
                    uint32_t end = range_end(rp.value().range);
                    return ParseResult<ExprNode>::Ok(ExprNode{ImportCallExpression{
                        std::make_unique<ExprNode>(std::move(spec.value())),
                        span(tok.range.offset, end)}});
                }
                // import.meta — 元属性（仅在模块上下文合法）
                if (tok_text == "import" && cur.kind == TokenKind::Dot) {
                    advance();  // 消费 '.'
                    if (cur.kind != TokenKind::Ident || token_text(cur) != "meta") {
                        return ParseResult<ExprNode>::Err(
                            make_parse_error(source, cur, "expected 'meta' after 'import.'"));
                    }
                    if (!in_module_context_) {
                        return ParseResult<ExprNode>::Err(
                            make_parse_error(source, tok, "'import.meta' is only valid in modules"));
                    }
                    advance();  // 消费 'meta'
                    return ParseResult<ExprNode>::Ok(ExprNode{MetaProperty{span(tok.range.offset, range_end(cur.range))}});
                }
                std::string name{tok_text};
                return ParseResult<ExprNode>::Ok(ExprNode{Identifier{std::move(name), tok.range}});
            }
            case TokenKind::KwThis:
                return ParseResult<ExprNode>::Ok(ExprNode{Identifier{"this", tok.range}});
            case TokenKind::LParen: {
                uint32_t paren_start = tok.range.offset;
                // () => ...  无参箭头函数
                if (cur.kind == TokenKind::RParen) {
                    advance();  // 消费 )
                    if (cur.kind == TokenKind::Arrow && !got_lf) {
                        advance();  // 消费 =>
                        return parse_arrow_body({}, paren_start);
                    }
                    return ParseResult<ExprNode>::Err(
                        make_parse_error(source, cur, "unexpected ')'"));
                }

                // 解析第一个表达式（允许赋值表达式，以支持 (a = 1) => a 语法）
                auto first = parse_expr(1);
                if (!first.ok()) return first;

                // (a, b, ...) — 先收集所有表达式，再按 ) 后是否跟 => 决定路径
                if (cur.kind == TokenKind::Comma) {
                    std::vector<ExprNode> items;
                    items.push_back(std::move(first.value()));
                    while (cur.kind == TokenKind::Comma) {
                        advance();  // 消费 ,
                        auto item = parse_expr(1);
                        if (!item.ok()) return item;
                        items.push_back(std::move(item.value()));
                    }
                    auto rp2 = expect(TokenKind::RParen);
                    if (!rp2.ok()) return ParseResult<ExprNode>::Err(rp2.error());
                    if (cur.kind == TokenKind::Arrow && !got_lf) {
                        // 箭头函数：每项为 Identifier / AssignmentExpression(=) / 末尾 SpreadElement
                        std::vector<ParamDef> params;
                        std::optional<std::string> rest;
                        for (size_t i = 0; i < items.size(); ++i) {
                            auto& item = items[i];
                            if (std::holds_alternative<SpreadElement>(item.v)) {
                                if (i != items.size() - 1) {
                                    return ParseResult<ExprNode>::Err(make_parse_error(
                                        source, cur, "rest element must be last parameter"));
                                }
                                const auto& sp = std::get<SpreadElement>(item.v);
                                if (!std::holds_alternative<Identifier>(sp.argument->v)) {
                                    return ParseResult<ExprNode>::Err(make_parse_error(
                                        source, cur, "rest parameter must be an identifier"));
                                }
                                rest = std::get<Identifier>(sp.argument->v).name;
                            } else if (std::holds_alternative<Identifier>(item.v)) {
                                params.push_back(ParamDef{std::get<Identifier>(item.v).name, nullptr});
                            } else if (std::holds_alternative<AssignmentExpression>(item.v)) {
                                // (a = expr) => ...  参数默认值
                                auto& ae = std::get<AssignmentExpression>(item.v);
                                if (ae.op != AssignOp::Assign) {
                                    return ParseResult<ExprNode>::Err(make_parse_error(
                                        source, cur, "arrow function parameter must be an identifier"));
                                }
                                auto default_expr = std::make_shared<ExprNode>(std::move(*ae.value));
                                params.push_back(ParamDef{std::move(ae.target), std::move(default_expr)});
                            } else {
                                return ParseResult<ExprNode>::Err(make_parse_error(
                                    source, cur, "arrow function parameter must be an identifier"));
                            }
                        }
                        advance();  // 消费 =>
                        return parse_arrow_body(std::move(params), paren_start, std::move(rest));
                    }
                    // 逗号表达式：(a, b) 的值为最后一项
                    ExprNode last_item = std::move(items.back());
                    last_item.is_parenthesized = true;
                    return ParseResult<ExprNode>::Ok(std::move(last_item));
                }

                // 消费 )
                auto rp = expect(TokenKind::RParen);
                if (!rp.ok()) return ParseResult<ExprNode>::Err(rp.error());

                // (a) => ...  单参括号箭头函数 或 (...rest) => ... 或 (a = 1) => ...
                if (cur.kind == TokenKind::Arrow && !got_lf) {
                    if (std::holds_alternative<SpreadElement>(first.value().v)) {
                        // (...rest) => body
                        const auto& sp = std::get<SpreadElement>(first.value().v);
                        if (!std::holds_alternative<Identifier>(sp.argument->v)) {
                            return ParseResult<ExprNode>::Err(make_parse_error(
                                source, cur, "rest parameter must be an identifier"));
                        }
                        std::string rest_name = std::get<Identifier>(sp.argument->v).name;
                        advance();  // 消费 =>
                        return parse_arrow_body({}, paren_start, std::move(rest_name));
                    }
                    if (std::holds_alternative<Identifier>(first.value().v)) {
                        std::string param_name = std::get<Identifier>(first.value().v).name;
                        advance();  // 消費 =>
                        return parse_arrow_body({ParamDef{std::move(param_name), nullptr}}, paren_start);
                    }
                    if (std::holds_alternative<AssignmentExpression>(first.value().v)) {
                        // (a = expr) => ...  单参数默认值
                        auto& ae = std::get<AssignmentExpression>(first.value().v);
                        if (ae.op != AssignOp::Assign) {
                            return ParseResult<ExprNode>::Err(
                                make_parse_error(source, cur, "arrow function parameter must be an identifier"));
                        }
                        auto default_expr = std::make_shared<ExprNode>(std::move(*ae.value));
                        advance();  // 消费 =>
                        return parse_arrow_body({ParamDef{std::move(ae.target), std::move(default_expr)}},
                                                paren_start);
                    }
                    return ParseResult<ExprNode>::Err(
                        make_parse_error(source, cur, "arrow function parameter must be an identifier"));
                }

                // 普通括号表达式
                first.value().is_parenthesized = true;
                return first;
            }
            // 一元前缀
            case TokenKind::Minus: {
                auto operand = parse_expr(18);
                if (!operand.ok()) return operand;
                auto r = span(tok.range.offset, range_end(expr_range(operand.value())));
                return ParseResult<ExprNode>::Ok(ExprNode{
                        UnaryExpression{UnaryOp::Minus, std::make_unique<ExprNode>(std::move(operand.value())), r}});
            }
            case TokenKind::Plus: {
                auto operand = parse_expr(18);
                if (!operand.ok()) return operand;
                auto r = span(tok.range.offset, range_end(expr_range(operand.value())));
                return ParseResult<ExprNode>::Ok(ExprNode{
                        UnaryExpression{UnaryOp::Plus, std::make_unique<ExprNode>(std::move(operand.value())), r}});
            }
            case TokenKind::Bang: {
                auto operand = parse_expr(18);
                if (!operand.ok()) return operand;
                auto r = span(tok.range.offset, range_end(expr_range(operand.value())));
                return ParseResult<ExprNode>::Ok(ExprNode{
                        UnaryExpression{UnaryOp::Bang, std::make_unique<ExprNode>(std::move(operand.value())), r}});
            }
            case TokenKind::Tilde: {
                auto operand = parse_expr(18);
                if (!operand.ok()) return operand;
                auto r = span(tok.range.offset, range_end(expr_range(operand.value())));
                return ParseResult<ExprNode>::Ok(ExprNode{
                        UnaryExpression{UnaryOp::BitNot, std::make_unique<ExprNode>(std::move(operand.value())), r}});
            }
            case TokenKind::KwTypeof: {
                auto operand = parse_expr(18);
                if (!operand.ok()) return operand;
                auto r = span(tok.range.offset, range_end(expr_range(operand.value())));
                return ParseResult<ExprNode>::Ok(ExprNode{
                        UnaryExpression{UnaryOp::Typeof, std::make_unique<ExprNode>(std::move(operand.value())), r}});
            }
            case TokenKind::KwVoid: {
                auto operand = parse_expr(18);
                if (!operand.ok()) return operand;
                auto r = span(tok.range.offset, range_end(expr_range(operand.value())));
                return ParseResult<ExprNode>::Ok(ExprNode{
                        UnaryExpression{UnaryOp::Void, std::make_unique<ExprNode>(std::move(operand.value())), r}});
            }
            case TokenKind::KwDelete: {
                auto operand = parse_expr(18);
                if (!operand.ok()) return operand;
                auto r = span(tok.range.offset, range_end(expr_range(operand.value())));
                return ParseResult<ExprNode>::Ok(ExprNode{
                        UnaryExpression{UnaryOp::Delete, std::make_unique<ExprNode>(std::move(operand.value())), r}});
            }
            // 前缀自增/自减：++x / --x
            case TokenKind::PlusPlus:
            case TokenKind::MinusMinus: {
                UpdateOp uop = (tok.kind == TokenKind::PlusPlus) ? UpdateOp::Inc : UpdateOp::Dec;
                auto operand = parse_expr(18);
                if (!operand.ok()) return operand;
                // operand must be a valid assignment target (Identifier or MemberExpression)
                if (!std::holds_alternative<Identifier>(operand.value().v) &&
                    !std::holds_alternative<MemberExpression>(operand.value().v)) {
                    return ParseResult<ExprNode>::Err(
                        make_parse_error(source, tok,
                            "invalid left-hand side expression in prefix operation (expected assignment target)"));
                }
                auto r = span(tok.range.offset, range_end(expr_range(operand.value())));
                return ParseResult<ExprNode>::Ok(ExprNode{UpdateExpression{
                    uop, std::make_unique<ExprNode>(std::move(operand.value())), true, r}});
            }
            case TokenKind::KwNew: {
                // new callee(args)
                uint32_t new_start = tok.range.offset;
                // Parse callee at higher precedence (member access allowed, but not call)
                // Use lbp(Dot)=18 as min_bp to allow member access but stop before call
                auto callee = parse_expr(20);  // stop before LParen (lbp=19) and ++/--(lbp=20), allow Dot/LBracket (lbp=21)
                if (!callee.ok()) return callee;
                if (std::holds_alternative<OptionalChainExpression>(callee.value().v)) {
                    return ParseResult<ExprNode>::Err(
                        make_parse_error(source, tok, "cannot use 'new' with optional chaining"));
                }
                std::vector<std::unique_ptr<ExprNode>> args;
                if (cur.kind == TokenKind::LParen) {
                    advance();  // consume '('
                    while (cur.kind != TokenKind::RParen && cur.kind != TokenKind::Eof) {
                        auto arg = parse_expr(2);
                        if (!arg.ok()) return arg;
                        args.push_back(std::make_unique<ExprNode>(std::move(arg.value())));
                        if (cur.kind == TokenKind::Comma) {
                            advance();
                        } else {
                            break;
                        }
                    }
                    auto rp = expect(TokenKind::RParen);
                    if (!rp.ok()) return ParseResult<ExprNode>::Err(rp.error());
                    uint32_t new_end = range_end(rp.value().range);
                    return ParseResult<ExprNode>::Ok(ExprNode{NewExpression{
                            std::make_unique<ExprNode>(std::move(callee.value())),
                            std::move(args),
                            span(new_start, new_end)}});
                }
                // new F without parentheses: treat as new F()
                uint32_t new_end = range_end(expr_range(callee.value()));
                return ParseResult<ExprNode>::Ok(ExprNode{NewExpression{
                        std::make_unique<ExprNode>(std::move(callee.value())),
                        std::move(args),
                        span(new_start, new_end)}});
            }
            case TokenKind::KwFunction: {
                // 函数表达式 function[*] [name](params) { body }
                uint32_t fn_start = tok.range.offset;
                bool is_gen_fe = (cur.kind == TokenKind::Star);
                if (is_gen_fe) advance();  // 消费 *
                std::optional<std::string> fn_name;
                if (cur.kind == TokenKind::Ident) {
                    fn_name = std::string(token_text(cur));
                    advance();
                }
                std::optional<std::string> fn_rest2;
                auto params_result = parse_function_params(fn_rest2);
                if (!params_result.ok()) return ParseResult<ExprNode>::Err(params_result.error());
                // P2-E: non-async function body resets in_async_function_ context
                // TLA: also reset in_module_ so await inside a plain function is not allowed
                bool saved_in_async_fe = in_async_function_;
                bool saved_in_module_fe = in_module_;
                bool saved_in_gen_fe = in_generator_function_;
                in_async_function_ = false;
                in_module_ = false;
                in_generator_function_ = is_gen_fe;
                auto body_result = parse_function_body();
                in_async_function_ = saved_in_async_fe;
                in_module_ = saved_in_module_fe;
                in_generator_function_ = saved_in_gen_fe;
                if (!body_result.ok()) return ParseResult<ExprNode>::Err(body_result.error());
                uint32_t fn_end = range_end(body_result.value().second);
                auto body_ptr = std::make_shared<std::vector<StmtNode>>(std::move(body_result.value().first));
                FunctionExpression fe{std::move(fn_name), std::move(params_result.value()), std::move(fn_rest2),
                        std::move(body_ptr), span(fn_start, fn_end)};
                fe.is_generator = is_gen_fe;
                return ParseResult<ExprNode>::Ok(ExprNode{std::move(fe)});
            }
            case TokenKind::LBracket: {
                // 数组字面量 [elem0, elem1, ...]
                uint32_t start = tok.range.offset;
                std::vector<std::optional<std::unique_ptr<ExprNode>>> elements;
                while (cur.kind != TokenKind::RBracket && cur.kind != TokenKind::Eof) {
                    if (cur.kind == TokenKind::Comma) {
                        // elision: hole — nullopt 表示真正的稀疏 hole
                        elements.push_back(std::nullopt);
                        advance();  // 消费 ,
                    } else {
                        auto elem = parse_expr(1);
                        if (!elem.ok()) return elem;
                        elements.push_back(std::make_unique<ExprNode>(std::move(elem.value())));
                        if (cur.kind == TokenKind::Comma) {
                            advance();  // 消费 ,
                            // 尾随逗号：消耗后若是 ']' 则不追加元素，直接退出
                        }
                        // 不是逗号也不是 ']'：让外层检查报错
                    }
                }
                auto rb = expect(TokenKind::RBracket);
                if (!rb.ok()) return ParseResult<ExprNode>::Err(rb.error());
                uint32_t end = range_end(rb.value().range);
                return ParseResult<ExprNode>::Ok(
                    ExprNode{ArrayExpression{std::move(elements), span(start, end)}});
            }
            case TokenKind::LBrace: {
                // 对象字面量 { key: value, key, ...spread, method(){}, get/set accessor, async method, *gen }
                uint32_t start = tok.range.offset;
                std::vector<ObjectProperty> props;
                while (cur.kind != TokenKind::RBrace && cur.kind != TokenKind::Eof) {
                    uint32_t key_start = cur.range.offset;

                    // spread: ...expr
                    if (cur.kind == TokenKind::DotDotDot) {
                        advance();  // 消费 ...
                        auto arg = parse_expr(1);
                        if (!arg.ok()) return arg;
                        uint32_t prop_end = range_end(expr_range(arg.value()));
                        ObjectProperty prop;
                        prop.key = "";  // sentinel for spread
                        prop.value = std::make_unique<ExprNode>(ExprNode{SpreadElement{
                            std::make_unique<ExprNode>(std::move(arg.value())),
                            span(key_start, prop_end)}});
                        prop.range = span(key_start, prop_end);
                        props.push_back(std::move(prop));
                        if (cur.kind == TokenKind::Comma) advance();
                        continue;
                    }

                    // --- Generator method: *foo() {} or *[expr]() {} ---
                    if (cur.kind == TokenKind::Star) {
                        advance();  // 消费 *
                        std::string gkey;
                        std::unique_ptr<ExprNode> gkey_expr;
                        bool gcomputed = false;
                        if (cur.kind == TokenKind::LBracket) {
                            advance();  // 消费 [
                            auto ke = parse_expr(1);
                            if (!ke.ok()) return ParseResult<ExprNode>::Err(ke.error());
                            auto rb = expect(TokenKind::RBracket);
                            if (!rb.ok()) return ParseResult<ExprNode>::Err(rb.error());
                            gkey_expr = std::make_unique<ExprNode>(std::move(ke.value()));
                            gcomputed = true;
                        } else if (cur.kind == TokenKind::Ident) {
                            gkey = std::string(token_text(cur)); advance();
                        } else if (cur.kind == TokenKind::String) {
                            gkey = decode_string(token_text(cur)); advance();
                        } else if (cur.kind == TokenKind::Number) {
                            gkey = number_to_property_key(parse_number_text(token_text(cur))); advance();
                        } else if (is_keyword(cur.kind)) {
                            gkey = std::string(token_text(cur)); advance();
                        } else {
                            return ParseResult<ExprNode>::Err(
                                make_parse_error(source, cur, "expected method name after *"));
                        }
                        std::optional<std::string> fn_rest_g;
                        auto params_g = parse_function_params(fn_rest_g);
                        if (!params_g.ok()) return ParseResult<ExprNode>::Err(params_g.error());
                        bool saved_async_g = in_async_function_;
                        bool saved_gen_g = in_generator_function_;
                        in_async_function_ = false;
                        in_generator_function_ = true;
                        auto body_g = parse_function_body();
                        in_async_function_ = saved_async_g;
                        in_generator_function_ = saved_gen_g;
                        if (!body_g.ok()) return ParseResult<ExprNode>::Err(body_g.error());
                        uint32_t prop_end = range_end(body_g.value().second);
                        auto body_ptr = std::make_shared<std::vector<StmtNode>>(
                            std::move(body_g.value().first));
                        ObjectProperty prop;
                        prop.key = gkey;
                        prop.computed = gcomputed;
                        if (gcomputed) prop.key_expr = std::move(gkey_expr);
                        prop.method_kind = MethodKind::kGenerator;
                        prop.value = std::make_unique<ExprNode>(ExprNode{FunctionExpression{
                            gcomputed ? std::optional<std::string>{} : std::optional<std::string>{gkey},
                            std::move(params_g.value()),
                            std::move(fn_rest_g), std::move(body_ptr), span(key_start, prop_end)}});
                        prop.range = span(key_start, prop_end);
                        props.push_back(std::move(prop));
                        if (cur.kind == TokenKind::Comma) advance();
                        continue;
                    }

                    // --- get/set disambiguation ---
                    bool already_handled = false;
                    bool is_computed = false;
                    std::unique_ptr<ExprNode> key_expr_ptr;
                    std::string key;
                    Token key_tok = cur;

                    if (cur.kind == TokenKind::Ident &&
                        (token_text(cur) == "get" || token_text(cur) == "set")) {
                        std::string mod = std::string(token_text(cur));
                        Token mod_tok = cur;
                        advance();  // 消费 get/set
                        if (cur.kind != TokenKind::LParen && cur.kind != TokenKind::Colon &&
                            cur.kind != TokenKind::Comma && cur.kind != TokenKind::RBrace &&
                            cur.kind != TokenKind::Eq) {
                            // 真正的 getter/setter: 接下来解析属性名
                            std::string accessor_key;
                            std::unique_ptr<ExprNode> accessor_key_expr;
                            bool ac_computed = false;
                            if (cur.kind == TokenKind::LBracket) {
                                advance();  // 消费 [
                                auto ke = parse_expr(1);
                                if (!ke.ok()) return ParseResult<ExprNode>::Err(ke.error());
                                auto rb = expect(TokenKind::RBracket);
                                if (!rb.ok()) return ParseResult<ExprNode>::Err(rb.error());
                                accessor_key_expr = std::make_unique<ExprNode>(std::move(ke.value()));
                                ac_computed = true;
                            } else if (cur.kind == TokenKind::Ident) {
                                accessor_key = std::string(token_text(cur)); advance();
                            } else if (cur.kind == TokenKind::String) {
                                accessor_key = decode_string(token_text(cur)); advance();
                            } else if (cur.kind == TokenKind::Number) {
                                accessor_key = number_to_property_key(
                                    parse_number_text(token_text(cur))); advance();
                            } else if (is_keyword(cur.kind)) {
                                accessor_key = std::string(token_text(cur)); advance();
                            } else {
                                return ParseResult<ExprNode>::Err(
                                    make_parse_error(source, cur, "expected property name"));
                            }
                            std::optional<std::string> fn_rest_ac;
                            auto params_ac = parse_function_params(fn_rest_ac);
                            if (!params_ac.ok()) return ParseResult<ExprNode>::Err(params_ac.error());
                            bool saved_async_ac = in_async_function_;
                            in_async_function_ = false;
                            auto body_ac = parse_function_body();
                            in_async_function_ = saved_async_ac;
                            if (!body_ac.ok()) return ParseResult<ExprNode>::Err(body_ac.error());
                            uint32_t prop_end = range_end(body_ac.value().second);
                            auto body_ptr = std::make_shared<std::vector<StmtNode>>(
                                std::move(body_ac.value().first));
                            MethodKind mk = (mod == "get") ? MethodKind::kGetter : MethodKind::kSetter;
                            ObjectProperty prop;
                            prop.key = accessor_key;
                            prop.computed = ac_computed;
                            if (ac_computed) prop.key_expr = std::move(accessor_key_expr);
                            prop.method_kind = mk;
                            prop.value = std::make_unique<ExprNode>(ExprNode{FunctionExpression{
                                ac_computed ? std::optional<std::string>{} : std::optional<std::string>{accessor_key},
                                std::move(params_ac.value()),
                                std::move(fn_rest_ac), std::move(body_ptr), span(key_start, prop_end)}});
                            prop.range = span(key_start, prop_end);
                            props.push_back(std::move(prop));
                            if (cur.kind == TokenKind::Comma) advance();
                            continue;
                        }
                        // get/set 作为普通属性名
                        key = mod;
                        key_tok = mod_tok;
                    } else if (cur.kind == TokenKind::Ident && token_text(cur) == "async") {
                        // --- async method disambiguation ---
                        Token async_tok = cur;
                        advance();  // 消费 async
                        // got_lf 此时反映 async 后是否有换行
                        if (cur.kind != TokenKind::LParen && cur.kind != TokenKind::Colon &&
                            cur.kind != TokenKind::Comma && cur.kind != TokenKind::RBrace &&
                            cur.kind != TokenKind::Eq) {
                            // async method（或 async generator，降级处理）
                            bool is_gen = (cur.kind == TokenKind::Star);
                            if (is_gen) advance();  // 消费 *
                            std::string akey;
                            std::unique_ptr<ExprNode> akey_expr;
                            bool am_computed = false;
                            if (cur.kind == TokenKind::LBracket) {
                                advance();  // 消费 [
                                auto ke = parse_expr(1);
                                if (!ke.ok()) return ParseResult<ExprNode>::Err(ke.error());
                                auto rb = expect(TokenKind::RBracket);
                                if (!rb.ok()) return ParseResult<ExprNode>::Err(rb.error());
                                akey_expr = std::make_unique<ExprNode>(std::move(ke.value()));
                                am_computed = true;
                            } else if (cur.kind == TokenKind::Ident) {
                                akey = std::string(token_text(cur)); advance();
                            } else if (cur.kind == TokenKind::String) {
                                akey = decode_string(token_text(cur)); advance();
                            } else if (cur.kind == TokenKind::Number) {
                                akey = number_to_property_key(
                                    parse_number_text(token_text(cur))); advance();
                            } else if (is_keyword(cur.kind)) {
                                akey = std::string(token_text(cur)); advance();
                            } else {
                                return ParseResult<ExprNode>::Err(
                                    make_parse_error(source, cur, "expected method name after async"));
                            }
                            std::optional<std::string> fn_rest_am;
                            auto params_am = parse_function_params(fn_rest_am);
                            if (!params_am.ok()) return ParseResult<ExprNode>::Err(params_am.error());
                            bool saved_async_am = in_async_function_;
                            in_async_function_ = true;
                            auto body_am = parse_function_body();
                            in_async_function_ = saved_async_am;
                            if (!body_am.ok()) return ParseResult<ExprNode>::Err(body_am.error());
                            uint32_t prop_end = range_end(body_am.value().second);
                            auto body_ptr = std::make_shared<std::vector<StmtNode>>(
                                std::move(body_am.value().first));
                            MethodKind mk = is_gen ? MethodKind::kGenerator : MethodKind::kAsyncMethod;
                            ObjectProperty prop;
                            prop.key = akey;
                            prop.computed = am_computed;
                            if (am_computed) prop.key_expr = std::move(akey_expr);
                            prop.method_kind = mk;
                            prop.value = std::make_unique<ExprNode>(ExprNode{AsyncFunctionExpression{
                                am_computed ? std::optional<std::string>{} : std::optional<std::string>{akey},
                                std::move(params_am.value()),
                                std::move(fn_rest_am), std::move(body_ptr), span(key_start, prop_end)}});
                            prop.range = span(key_start, prop_end);
                            props.push_back(std::move(prop));
                            if (cur.kind == TokenKind::Comma) advance();
                            continue;
                        }
                        // async 作为普通属性名
                        key = "async";
                        key_tok = async_tok;
                    } else {
                        // 普通键解析（Ident / String / Number / 关键字 / 计算键 [expr]）
                        if (cur.kind == TokenKind::LBracket) {
                            advance();  // 消费 [
                            auto ke = parse_expr(1);
                            if (!ke.ok()) return ParseResult<ExprNode>::Err(ke.error());
                            auto rb = expect(TokenKind::RBracket);
                            if (!rb.ok()) return ParseResult<ExprNode>::Err(rb.error());
                            key_expr_ptr = std::make_unique<ExprNode>(std::move(ke.value()));
                            is_computed = true;
                        } else if (cur.kind == TokenKind::Ident) {
                            key = std::string(token_text(cur));
                            advance();
                        } else if (cur.kind == TokenKind::String) {
                            key = decode_string(token_text(cur));
                            advance();
                        } else if (cur.kind == TokenKind::Number) {
                            double num_val = parse_number_text(token_text(cur));
                            key = number_to_property_key(num_val);
                            advance();
                        } else if (is_keyword(cur.kind)) {
                            key = std::string(token_text(cur));
                            advance();
                        } else {
                            return ParseResult<ExprNode>::Err(
                                    make_parse_error(source, cur, "expected property key"));
                        }
                    }

                    if (!already_handled) {
                        if (cur.kind == TokenKind::Colon) {
                            advance();  // 消费 :
                            auto val = parse_expr(1);
                            if (!val.ok()) return val;
                            uint32_t prop_end = range_end(expr_range(val.value()));
                            ObjectProperty prop;
                            prop.key = key;
                            prop.computed = is_computed;
                            if (is_computed) prop.key_expr = std::move(key_expr_ptr);
                            prop.value = std::make_unique<ExprNode>(std::move(val.value()));
                            prop.range = span(key_start, prop_end);
                            props.push_back(std::move(prop));
                        } else if (cur.kind == TokenKind::LParen) {
                            // method shorthand: foo() {} or [expr]() {}
                            std::optional<std::string> fn_rest_m;
                            auto params_m = parse_function_params(fn_rest_m);
                            if (!params_m.ok()) return ParseResult<ExprNode>::Err(params_m.error());
                            bool saved_async_m = in_async_function_;
                            in_async_function_ = false;
                            auto body_m = parse_function_body();
                            in_async_function_ = saved_async_m;
                            if (!body_m.ok()) return ParseResult<ExprNode>::Err(body_m.error());
                            uint32_t prop_end = range_end(body_m.value().second);
                            auto body_ptr = std::make_shared<std::vector<StmtNode>>(
                                std::move(body_m.value().first));
                            ObjectProperty prop;
                            prop.key = key;
                            prop.computed = is_computed;
                            if (is_computed) prop.key_expr = std::move(key_expr_ptr);
                            prop.method_kind = MethodKind::kMethod;
                            prop.value = std::make_unique<ExprNode>(ExprNode{FunctionExpression{
                                is_computed ? std::optional<std::string>{} : std::optional<std::string>{key},
                                std::move(params_m.value()),
                                std::move(fn_rest_m), std::move(body_ptr), span(key_start, prop_end)}});
                            prop.range = span(key_start, prop_end);
                            props.push_back(std::move(prop));
                        } else if (!is_computed && cur.kind == TokenKind::Eq) {
                            // shorthand with default: { a = expr } (cover grammar for destructuring)
                            advance();  // 消费 =
                            auto def_val = parse_expr(1);
                            if (!def_val.ok()) return def_val;
                            uint32_t prop_end = range_end(expr_range(def_val.value()));
                            ObjectProperty prop;
                            prop.key = key;
                            prop.value = std::make_unique<ExprNode>(ExprNode{AssignmentExpression{
                                AssignOp::Assign,
                                key,
                                std::make_unique<ExprNode>(std::move(def_val.value())),
                                span(key_start, prop_end)}});
                            prop.range = span(key_start, prop_end);
                            props.push_back(std::move(prop));
                        } else if (!is_computed) {
                            // shorthand: { a } = { a: a }
                            ObjectProperty prop;
                            prop.key = key;
                            prop.value = std::make_unique<ExprNode>(ExprNode{Identifier{key, key_tok.range}});
                            prop.range = span(key_start, range_end(key_tok.range));
                            props.push_back(std::move(prop));
                        } else {
                            // computed key must be followed by : or (
                            return ParseResult<ExprNode>::Err(
                                make_parse_error(source, cur, "expected ':' or '(' after computed key"));
                        }
                        if (cur.kind == TokenKind::Comma) {
                            advance();  // 消费 ,
                        } else {
                            break;
                        }
                    }
                }
                if (cur.kind != TokenKind::RBrace) {
                    return ParseResult<ExprNode>::Err(
                            make_parse_error(source, cur, "expected '}'"));
                }
                uint32_t end = range_end(cur.range);
                advance();  // 消费 }
                return ParseResult<ExprNode>::Ok(
                        ExprNode{ObjectExpression{std::move(props), span(start, end)}});
            }
            case TokenKind::Regex: {
                std::string_view text = token_text(tok);
                size_t last_slash = text.rfind('/');
                std::string pattern(text.substr(1, last_slash - 1));
                std::string flags(text.substr(last_slash + 1));
                return ParseResult<ExprNode>::Ok(ExprNode{RegexLiteral{std::move(pattern), std::move(flags), tok.range}});
            }
            case TokenKind::TemplateNoSub: {
                // `...`  无插值完整模板
                std::string_view text = token_text(tok);
                // raw_text: 去掉首尾定界符（` 和 `）
                std::string_view raw_text = text.substr(1, text.size() - 2);
                auto cooked = decode_template_cooked(raw_text);
                if (!cooked.has_value()) {
                    return ParseResult<ExprNode>::Err(
                        make_parse_error(source, tok, "invalid escape sequence in template literal"));
                }
                TemplateElement elem{std::move(cooked.value()), ""};
                std::vector<TemplateElement> quasis;
                quasis.push_back(std::move(elem));
                return ParseResult<ExprNode>::Ok(ExprNode{TemplateLiteral{
                    std::move(quasis), {}, tok.range}});
            }
            case TokenKind::TemplateHead: {
                // `...${ 有插值模板头
                uint32_t tpl_start = tok.range.offset;
                std::vector<TemplateElement> quasis;
                std::vector<std::unique_ptr<ExprNode>> expressions;

                // 解码第一段（去掉首 ` 和尾 ${）
                {
                    std::string_view text = token_text(tok);
                    // text = `...${ ，raw_text 去掉首 ` 和尾 ${
                    std::string_view raw_text = text.substr(1, text.size() - 3);
                    auto cooked = decode_template_cooked(raw_text);
                    if (!cooked.has_value()) {
                        return ParseResult<ExprNode>::Err(
                            make_parse_error(source, tok, "invalid escape sequence in template literal"));
                    }
                    quasis.push_back(TemplateElement{std::move(cooked.value()), ""});
                }

                // 循环：解析表达式 + 下一段
                while (true) {
                    auto expr_res = parse_expr(0);
                    if (!expr_res.ok()) return expr_res;
                    expressions.push_back(std::make_unique<ExprNode>(std::move(expr_res.value())));

                    // cur 应为 RBrace（}）
                    if (cur.kind != TokenKind::RBrace) {
                        return ParseResult<ExprNode>::Err(
                            make_parse_error(source, cur, "expected '}' in template literal"));
                    }
                    // 消耗 } 并扫描下一模板段
                    advance_template_part();

                    if (cur.kind == TokenKind::TemplateTail) {
                        // 末尾段：去掉首 } 和尾 `
                        std::string_view text = token_text(cur);
                        std::string_view raw_text = text.substr(1, text.size() - 2);
                        auto cooked = decode_template_cooked(raw_text);
                        if (!cooked.has_value()) {
                            return ParseResult<ExprNode>::Err(
                                make_parse_error(source, cur, "invalid escape sequence in template literal"));
                        }
                        uint32_t tpl_end = range_end(cur.range);
                        quasis.push_back(TemplateElement{std::move(cooked.value()), ""});
                        advance();  // 消耗 TemplateTail
                        return ParseResult<ExprNode>::Ok(ExprNode{TemplateLiteral{
                            std::move(quasis), std::move(expressions),
                            span(tpl_start, tpl_end)}});
                    } else if (cur.kind == TokenKind::TemplateMiddle) {
                        // 中间段：去掉首 } 和尾 ${
                        std::string_view text = token_text(cur);
                        std::string_view raw_text = text.substr(1, text.size() - 3);
                        auto cooked = decode_template_cooked(raw_text);
                        if (!cooked.has_value()) {
                            return ParseResult<ExprNode>::Err(
                                make_parse_error(source, cur, "invalid escape sequence in template literal"));
                        }
                        quasis.push_back(TemplateElement{std::move(cooked.value()), ""});
                        advance();  // 消耗 TemplateMiddle，为下一次循环准备
                        // 继续循环
                    } else {
                        return ParseResult<ExprNode>::Err(
                            make_parse_error(source, cur, "unexpected token in template literal"));
                    }
                }
            }
            case TokenKind::DotDotDot: {
                // SpreadElement: ...expr（用于数组字面量和调用参数）
                auto arg = parse_expr(2);
                if (!arg.ok()) return arg;
                SourceRange r = span(tok.range.offset, range_end(expr_range(arg.value())));
                return ParseResult<ExprNode>::Ok(ExprNode{SpreadElement{
                    std::make_unique<ExprNode>(std::move(arg.value())), r}});
            }
            default:
                return ParseResult<ExprNode>::Err(make_parse_error(
                        source, tok,
                        std::string("unexpected token in expression: ") + std::string(token_kind_name(tok.kind))));
        }
    }

    // 中缀处理（led）
    ParseResult<ExprNode> led(Token op_tok, ExprNode left) {
        auto kind = op_tok.kind;
        int bp = lbp(kind);

        // 箭头函数：x => ...（left 必须是 Identifier）
        if (kind == TokenKind::Arrow) {
            if (got_lf) {
                return ParseResult<ExprNode>::Err(
                    make_parse_error(source, op_tok, "no line break allowed before '=>'"));
            }
            if (!std::holds_alternative<Identifier>(left.v)) {
                return ParseResult<ExprNode>::Err(
                    make_parse_error(source, op_tok, "arrow function parameter must be an identifier"));
            }
            std::string param_name = std::get<Identifier>(left.v).name;
            uint32_t fn_start = std::get<Identifier>(left.v).range.offset;
            return parse_arrow_body({ParamDef{std::move(param_name), nullptr}}, fn_start);
        }

        // 后缀自增/自减：x++ / x--
        if (kind == TokenKind::PlusPlus || kind == TokenKind::MinusMinus) {
            if (got_lf) {
                return ParseResult<ExprNode>::Err(
                    make_parse_error(source, op_tok, "unexpected line break after operand"));
            }
            // Optional chaining is not a valid assignment target
            if (std::holds_alternative<OptionalChainExpression>(left.v)) {
                return ParseResult<ExprNode>::Err(
                    make_parse_error(source, op_tok,
                        "invalid left-hand side expression in postfix operation: optional chain is not a valid assignment target"));
            }
            // left must be a valid assignment target (Identifier or MemberExpression)
            if (!std::holds_alternative<Identifier>(left.v) &&
                !std::holds_alternative<MemberExpression>(left.v)) {
                return ParseResult<ExprNode>::Err(
                    make_parse_error(source, op_tok,
                        "invalid left-hand side expression in postfix operation (expected assignment target)"));
            }
            UpdateOp uop = (kind == TokenKind::PlusPlus) ? UpdateOp::Inc : UpdateOp::Dec;
            auto r = span(expr_range(left).offset, range_end(op_tok.range));
            return ParseResult<ExprNode>::Ok(ExprNode{UpdateExpression{
                uop, std::make_unique<ExprNode>(std::move(left)), false, r}});
        }

        // 调用表达式：callee(args)
        if (kind == TokenKind::LParen) {
            std::vector<std::unique_ptr<ExprNode>> args;
            while (cur.kind != TokenKind::RParen && cur.kind != TokenKind::Eof) {
                auto arg = parse_expr(2);  // stop before comma (lbp=0 for comma, but assignment lbp=2)
                if (!arg.ok()) return arg;
                args.push_back(std::make_unique<ExprNode>(std::move(arg.value())));
                if (cur.kind == TokenKind::Comma) {
                    advance();
                } else {
                    break;
                }
            }
            auto rp = expect(TokenKind::RParen);
            if (!rp.ok()) return ParseResult<ExprNode>::Err(rp.error());
            uint32_t call_start = expr_range(left).offset;
            uint32_t call_end = range_end(rp.value().range);
            return ParseResult<ExprNode>::Ok(ExprNode{CallExpression{
                    std::make_unique<ExprNode>(std::move(left)),
                    std::move(args),
                    span(call_start, call_end)}});
        }

        // 成员访问：obj.prop
        // 属性名可以是标识符或关键字（如 obj.catch, obj.finally, obj.return 等）
        if (kind == TokenKind::Dot) {
            bool is_prop_name = cur.kind == TokenKind::Ident || is_keyword(cur.kind);
            if (!is_prop_name) {
                return ParseResult<ExprNode>::Err(
                        make_parse_error(source, cur, "expected property name after '.'"));
            }
            std::string prop_name = std::string(token_text(cur));
            SourceRange prop_range = cur.range;
            uint32_t end = range_end(cur.range);
            advance();
            ExprNode prop_node{StringLiteral{prop_name, prop_range}};
            uint32_t obj_start = expr_range(left).offset;
            return ParseResult<ExprNode>::Ok(ExprNode{MemberExpression{
                    std::make_unique<ExprNode>(std::move(left)),
                    std::make_unique<ExprNode>(std::move(prop_node)),
                    false,
                    span(obj_start, end)}});
        }

        // 成员访问：obj[expr]
        if (kind == TokenKind::LBracket) {
            auto prop = parse_expr(0);
            if (!prop.ok()) return prop;
            if (cur.kind != TokenKind::RBracket) {
                return ParseResult<ExprNode>::Err(
                        make_parse_error(source, cur, "expected ']'"));
            }
            uint32_t end = range_end(cur.range);
            advance();  // 消费 ]
            uint32_t obj_start = expr_range(left).offset;
            return ParseResult<ExprNode>::Ok(ExprNode{MemberExpression{
                    std::make_unique<ExprNode>(std::move(left)),
                    std::make_unique<ExprNode>(std::move(prop.value())),
                    true,
                    span(obj_start, end)}});
        }

        // Optional chaining: base?.prop / base?.[expr] / base?.()
        if (kind == TokenKind::QuestionDot) {
            uint32_t chain_start = expr_range(left).offset;
            OptionalChainExpression chain;
            chain.base = std::make_unique<ExprNode>(std::move(left));
            uint32_t last_end = range_end(op_tok.range);

            // Parse a single link after consuming QuestionDot or Dot
            // optional=true for ?. trigger, false for . / [ / (
            auto parse_one_link = [&](bool optional)
                -> ParseResult<OptionalChainExpression::ChainLink> {
                if (cur.kind == TokenKind::LParen) {
                    advance();  // consume '('
                    std::vector<std::unique_ptr<ExprNode>> args;
                    while (cur.kind != TokenKind::RParen && cur.kind != TokenKind::Eof) {
                        auto arg = parse_expr(2);
                        if (!arg.ok()) return ParseResult<OptionalChainExpression::ChainLink>::Err(arg.error());
                        args.push_back(std::make_unique<ExprNode>(std::move(arg.value())));
                        if (cur.kind == TokenKind::Comma) advance();
                        else break;
                    }
                    auto rp = expect(TokenKind::RParen);
                    if (!rp.ok()) return ParseResult<OptionalChainExpression::ChainLink>::Err(rp.error());
                    last_end = range_end(rp.value().range);
                    return ParseResult<OptionalChainExpression::ChainLink>::Ok(
                        OptionalChainExpression::CallLink{optional, std::move(args)});
                } else if (cur.kind == TokenKind::LBracket) {
                    advance();  // consume '['
                    auto key = parse_expr(0);
                    if (!key.ok()) return ParseResult<OptionalChainExpression::ChainLink>::Err(key.error());
                    auto rb = expect(TokenKind::RBracket);
                    if (!rb.ok()) return ParseResult<OptionalChainExpression::ChainLink>::Err(rb.error());
                    last_end = range_end(rb.value().range);
                    return ParseResult<OptionalChainExpression::ChainLink>::Ok(
                        OptionalChainExpression::ElemLink{optional,
                            std::make_unique<ExprNode>(std::move(key.value()))});
                } else if (cur.kind == TokenKind::Ident || is_keyword(cur.kind)) {
                    std::string name = std::string(token_text(cur));
                    last_end = range_end(cur.range);
                    advance();
                    return ParseResult<OptionalChainExpression::ChainLink>::Ok(
                        OptionalChainExpression::PropLink{optional, std::move(name)});
                } else {
                    return ParseResult<OptionalChainExpression::ChainLink>::Err(
                        make_parse_error(source, cur, "expected property name, '[', or '(' after '?.'"));
                }
            };

            // First link: triggered by ?. (optional=true)
            auto first = parse_one_link(true);
            if (!first.ok()) return ParseResult<ExprNode>::Err(first.error());
            chain.links.push_back(std::move(first.value()));

            // Continue consuming subsequent links
            while (true) {
                if (cur.kind == TokenKind::QuestionDot) {
                    advance();  // consume ?.
                    auto lnk = parse_one_link(true);
                    if (!lnk.ok()) return ParseResult<ExprNode>::Err(lnk.error());
                    chain.links.push_back(std::move(lnk.value()));
                } else if (cur.kind == TokenKind::Dot) {
                    advance();  // consume .
                    if (cur.kind != TokenKind::Ident && !is_keyword(cur.kind)) {
                        return ParseResult<ExprNode>::Err(
                            make_parse_error(source, cur, "expected property name after '.'"));
                    }
                    std::string name = std::string(token_text(cur));
                    last_end = range_end(cur.range);
                    advance();
                    chain.links.push_back(OptionalChainExpression::PropLink{false, std::move(name)});
                } else if (cur.kind == TokenKind::LBracket) {
                    advance();  // consume [
                    auto key = parse_expr(0);
                    if (!key.ok()) return key;
                    auto rb = expect(TokenKind::RBracket);
                    if (!rb.ok()) return ParseResult<ExprNode>::Err(rb.error());
                    last_end = range_end(rb.value().range);
                    chain.links.push_back(OptionalChainExpression::ElemLink{false,
                        std::make_unique<ExprNode>(std::move(key.value()))});
                } else if (cur.kind == TokenKind::LParen) {
                    advance();  // consume (
                    std::vector<std::unique_ptr<ExprNode>> args;
                    while (cur.kind != TokenKind::RParen && cur.kind != TokenKind::Eof) {
                        auto arg = parse_expr(2);
                        if (!arg.ok()) return arg;
                        args.push_back(std::make_unique<ExprNode>(std::move(arg.value())));
                        if (cur.kind == TokenKind::Comma) advance();
                        else break;
                    }
                    auto rp = expect(TokenKind::RParen);
                    if (!rp.ok()) return ParseResult<ExprNode>::Err(rp.error());
                    last_end = range_end(rp.value().range);
                    chain.links.push_back(OptionalChainExpression::CallLink{false, std::move(args)});
                } else {
                    break;
                }
            }

            chain.range = span(chain_start, last_end);
            return ParseResult<ExprNode>::Ok(ExprNode{std::move(chain)});
        }

        // 赋值：右结合，检查左侧是 Identifier 或 MemberExpression 或解构模式
        if (kind == TokenKind::Eq || kind == TokenKind::PlusEq || kind == TokenKind::MinusEq ||
            kind == TokenKind::StarEq || kind == TokenKind::SlashEq || kind == TokenKind::PercentEq ||
            kind == TokenKind::AmpEq || kind == TokenKind::PipeEq || kind == TokenKind::CaretEq ||
            kind == TokenKind::LShiftEq || kind == TokenKind::RShiftEq || kind == TokenKind::URShiftEq) {
            // Optional chaining 不能作为赋值左值
            if (std::holds_alternative<OptionalChainExpression>(left.v)) {
                return ParseResult<ExprNode>::Err(
                    make_parse_error(source, op_tok, "invalid left-hand side: optional chain is not a valid assignment target"));
            }
            // 解构赋值模式：左侧为 ArrayExpression 或 ObjectExpression，且 op 为 =
            if (kind == TokenKind::Eq &&
                (std::holds_alternative<ArrayExpression>(left.v) ||
                 std::holds_alternative<ObjectExpression>(left.v))) {
                uint32_t left_start = expr_range(left).offset;  // 先计算，convert 可能 move 内部成员
                auto pat_r = convert_expr_to_pattern(left);
                if (!pat_r.ok()) return ParseResult<ExprNode>::Err(pat_r.error());
                auto right = parse_expr(bp - 1);
                if (!right.ok()) return right;
                auto range = span(left_start, range_end(expr_range(right.value())));
                return ParseResult<ExprNode>::Ok(ExprNode{DestructuringAssignmentExpression{
                    std::make_unique<PatternNode>(std::move(pat_r.value())),
                    std::make_unique<ExprNode>(std::move(right.value())),
                    range}});
            }
            if (std::holds_alternative<Identifier>(left.v)) {
                std::string target = std::get<Identifier>(left.v).name;
                uint32_t left_start = std::get<Identifier>(left.v).range.offset;
                auto right = parse_expr(bp - 1);  // 右结合
                if (!right.ok()) return right;
                AssignOp aop;
                switch (kind) {
                    case TokenKind::Eq:
                        aop = AssignOp::Assign;
                        break;
                    case TokenKind::PlusEq:
                        aop = AssignOp::AddAssign;
                        break;
                    case TokenKind::MinusEq:
                        aop = AssignOp::SubAssign;
                        break;
                    case TokenKind::StarEq:
                        aop = AssignOp::MulAssign;
                        break;
                    case TokenKind::SlashEq:
                        aop = AssignOp::DivAssign;
                        break;
                    case TokenKind::PercentEq:
                        aop = AssignOp::ModAssign;
                        break;
                    case TokenKind::AmpEq:
                        aop = AssignOp::BitAndAssign;
                        break;
                    case TokenKind::PipeEq:
                        aop = AssignOp::BitOrAssign;
                        break;
                    case TokenKind::CaretEq:
                        aop = AssignOp::BitXorAssign;
                        break;
                    case TokenKind::LShiftEq:
                        aop = AssignOp::ShlAssign;
                        break;
                    case TokenKind::RShiftEq:
                        aop = AssignOp::SarAssign;
                        break;
                    case TokenKind::URShiftEq:
                        aop = AssignOp::ShrAssign;
                        break;
                    default:
                        aop = AssignOp::Assign;
                        break;
                }
                auto asgn_r = span(left_start, range_end(expr_range(right.value())));
                return ParseResult<ExprNode>::Ok(ExprNode{AssignmentExpression{
                        aop, std::move(target), std::make_unique<ExprNode>(std::move(right.value())), asgn_r}});
            }
            if (std::holds_alternative<MemberExpression>(left.v)) {
                if (kind != TokenKind::Eq) {
                    return ParseResult<ExprNode>::Err(
                            make_parse_error(source, op_tok, "compound assignment to member not supported"));
                }
                auto& mem = std::get<MemberExpression>(left.v);
                uint32_t left_start = mem.range.offset;
                bool computed = mem.computed;
                auto obj_ptr = std::move(mem.object);
                auto prop_ptr = std::move(mem.property);
                auto right = parse_expr(bp - 1);  // 右结合
                if (!right.ok()) return right;
                auto mae_r = span(left_start, range_end(expr_range(right.value())));
                return ParseResult<ExprNode>::Ok(ExprNode{MemberAssignmentExpression{
                        std::move(obj_ptr),
                        std::move(prop_ptr),
                        computed,
                        std::make_unique<ExprNode>(std::move(right.value())),
                        mae_r}});
            }
            return ParseResult<ExprNode>::Err(
                    make_parse_error(source, op_tok, "invalid left-hand side in assignment"));
        }

        // || 和 &&：LogicalExpression
        // 条件（三元）表达式：condition ? consequent : alternate
        // parse_expr(1): 允许 AssignmentExpression（lbp=2>1），停在逗号（lbp=0）处
        if (kind == TokenKind::Question) {
            auto then_result = parse_expr(1);
            if (!then_result.ok()) return then_result;
            auto colon = expect(TokenKind::Colon);
            if (!colon.ok()) return ParseResult<ExprNode>::Err(colon.error());
            auto else_result = parse_expr(1);
            if (!else_result.ok()) return else_result;
            auto r = span(expr_range(left).offset, range_end(expr_range(else_result.value())));
            return ParseResult<ExprNode>::Ok(ExprNode{ConditionalExpression{
                std::make_unique<ExprNode>(std::move(left)),
                std::make_unique<ExprNode>(std::move(then_result.value())),
                std::make_unique<ExprNode>(std::move(else_result.value())),
                r}});
        }

        if (kind == TokenKind::PipePipe || kind == TokenKind::AmpAmp) {
            if (auto* le = std::get_if<LogicalExpression>(&left.v)) {
                if (le->op == LogicalOp::Nullish && !left.is_parenthesized) {
                    return ParseResult<ExprNode>::Err(make_parse_error(
                        source, op_tok, "cannot mix '&&' or '||' with '?\?'"));
                }
            }
            auto right = parse_expr(bp);
            if (!right.ok()) return right;
            LogicalOp lop = (kind == TokenKind::AmpAmp) ? LogicalOp::And : LogicalOp::Or;
            auto log_r = span(expr_range(left).offset, range_end(expr_range(right.value())));
            return ParseResult<ExprNode>::Ok(
                    ExprNode{LogicalExpression{lop, std::make_unique<ExprNode>(std::move(left)),
                                               std::make_unique<ExprNode>(std::move(right.value())), log_r}});
        }

        if (kind == TokenKind::QuestionQuestion) {
            if (auto* le = std::get_if<LogicalExpression>(&left.v)) {
                if ((le->op == LogicalOp::And || le->op == LogicalOp::Or) && !left.is_parenthesized) {
                    return ParseResult<ExprNode>::Err(make_parse_error(
                        source, op_tok, "cannot mix '?\?' with '&&' or '||'"));
                }
            }
            auto right = parse_expr(3);  // lbp=3，左结合
            if (!right.ok()) return right;
            if (auto* le = std::get_if<LogicalExpression>(&right.value().v)) {
                if ((le->op == LogicalOp::And || le->op == LogicalOp::Or) && !right.value().is_parenthesized) {
                    return ParseResult<ExprNode>::Err(make_parse_error(
                        source, op_tok, "'&&' and '||' cannot appear in the right side of '?\?' without parentheses"));
                }
            }
            auto null_r = span(expr_range(left).offset, range_end(expr_range(right.value())));
            return ParseResult<ExprNode>::Ok(
                ExprNode{LogicalExpression{LogicalOp::Nullish, std::make_unique<ExprNode>(std::move(left)),
                                           std::make_unique<ExprNode>(std::move(right.value())), null_r}});
        }

        // 'in' 运算符（contextual keyword，TokenKind::Ident，text=="in"）
        if (kind == TokenKind::Ident && token_text(op_tok) == "in") {
            auto right = parse_expr(12);
            if (!right.ok()) return right;
            auto bin_r = span(expr_range(left).offset, range_end(expr_range(right.value())));
            return ParseResult<ExprNode>::Ok(
                ExprNode{BinaryExpression{BinaryOp::In, std::make_unique<ExprNode>(std::move(left)),
                                          std::make_unique<ExprNode>(std::move(right.value())), bin_r}});
        }

        // 其他二元操作符：BinaryExpression（左结合）
        auto right = parse_expr(bp);
        if (!right.ok()) return right;

        BinaryOp bop;
        switch (kind) {
            case TokenKind::Plus:
                bop = BinaryOp::Add;
                break;
            case TokenKind::Minus:
                bop = BinaryOp::Sub;
                break;
            case TokenKind::Star:
                bop = BinaryOp::Mul;
                break;
            case TokenKind::Slash:
                bop = BinaryOp::Div;
                break;
            case TokenKind::Percent:
                bop = BinaryOp::Mod;
                break;
            case TokenKind::Lt:
                bop = BinaryOp::Lt;
                break;
            case TokenKind::Gt:
                bop = BinaryOp::Gt;
                break;
            case TokenKind::LtEq:
                bop = BinaryOp::LtEq;
                break;
            case TokenKind::GtEq:
                bop = BinaryOp::GtEq;
                break;
            case TokenKind::EqEq:
                bop = BinaryOp::EqEq;
                break;
            case TokenKind::BangEq:
                bop = BinaryOp::NotEq;
                break;
            case TokenKind::EqEqEq:
                bop = BinaryOp::EqEqEq;
                break;
            case TokenKind::BangEqEq:
                bop = BinaryOp::NotEqEq;
                break;
            case TokenKind::KwInstanceof:
                bop = BinaryOp::Instanceof;
                break;
            case TokenKind::Amp:
                bop = BinaryOp::BitAnd;
                break;
            case TokenKind::Pipe:
                bop = BinaryOp::BitOr;
                break;
            case TokenKind::Caret:
                bop = BinaryOp::BitXor;
                break;
            case TokenKind::LShift:
                bop = BinaryOp::Shl;
                break;
            case TokenKind::RShift:
                bop = BinaryOp::Sar;
                break;
            case TokenKind::URShift:
                bop = BinaryOp::Shr;
                break;
            default:
                return ParseResult<ExprNode>::Err(make_parse_error(source, op_tok, "unknown binary operator"));
        }
        auto bin_r = span(expr_range(left).offset, range_end(expr_range(right.value())));
        return ParseResult<ExprNode>::Ok(
                ExprNode{BinaryExpression{bop, std::make_unique<ExprNode>(std::move(left)),
                                          std::make_unique<ExprNode>(std::move(right.value())), bin_r}});
    }

    // Pratt parser 主循环
    ParseResult<ExprNode> parse_expr(int min_bp) {
        // 消费当前 token 作为前缀
        Token tok = cur;
        advance();
        auto left = nud(tok);
        if (!left.ok()) return left;

        while (true) {
            int bp = lbp(cur.kind);
            // Contextual 'in' operator: lbp=9, suppressed inside for-init via no_in_
            if (bp == 0 && !no_in_ && is_in_token()) bp = 12;
            if (bp <= min_bp) break;
            Token op_tok = cur;
            // got_lf at this point is "before the operator". Save it for led()
            // handlers (suffix ++/--) that need the LF state between operand and operator.
            bool op_got_lf = got_lf;
            advance();
            // advance() overwrites got_lf to "before the token after the operator".
            // Set it back to pre-operator state for led(); led's internal recursive
            // parsing will update got_lf correctly, so we don't need to restore.
            got_lf = op_got_lf;
            auto result = led(op_tok, std::move(left.value()));
            if (!result.ok()) return result;
            left = std::move(result);
        }

        return left;
    }

    // ---- 函数辅助 ----

    // 解析参数列表 (a, b = expr, ...rest)，返回 ParamDef 向量
    ParseResult<std::vector<ParamDef>> parse_function_params(
            std::optional<std::string>& rest_param_out) {
        rest_param_out = std::nullopt;
        auto lp = expect(TokenKind::LParen);
        if (!lp.ok()) return ParseResult<std::vector<ParamDef>>::Err(lp.error());
        std::vector<ParamDef> params;
        while (cur.kind != TokenKind::RParen && cur.kind != TokenKind::Eof) {
            if (cur.kind == TokenKind::DotDotDot) {
                // rest parameter: ...name（不允许默认值）
                advance();  // 消费 ...
                if (cur.kind != TokenKind::Ident) {
                    return ParseResult<std::vector<ParamDef>>::Err(
                        make_parse_error(source, cur, "expected identifier for rest parameter"));
                }
                rest_param_out = std::string(token_text(cur));
                advance();
                if (cur.kind == TokenKind::Eq) {
                    return ParseResult<std::vector<ParamDef>>::Err(
                        make_parse_error(source, cur, "SyntaxError: rest parameter may not have a default initializer"));
                }
                if (cur.kind == TokenKind::Comma) {
                    return ParseResult<std::vector<ParamDef>>::Err(
                        make_parse_error(source, cur, "rest element must be last parameter"));
                }
                break;
            }
            if (cur.kind != TokenKind::Ident) {
                return ParseResult<std::vector<ParamDef>>::Err(
                        make_parse_error(source, cur, "expected parameter name"));
            }
            std::string pname = std::string(token_text(cur));
            advance();
            // 默认值
            std::shared_ptr<ExprNode> default_init;
            if (cur.kind == TokenKind::Eq) {
                advance();  // 消费 =
                auto dexpr = parse_expr(1);  // AssignmentExpression 级，允许赋值，不吞逗号
                if (!dexpr.ok()) return ParseResult<std::vector<ParamDef>>::Err(dexpr.error());
                default_init = std::make_shared<ExprNode>(std::move(dexpr.value()));
            }
            params.push_back(ParamDef{std::move(pname), std::move(default_init)});
            if (cur.kind == TokenKind::Comma) {
                advance();
            } else {
                break;
            }
        }
        auto rp = expect(TokenKind::RParen);
        if (!rp.ok()) return ParseResult<std::vector<ParamDef>>::Err(rp.error());
        return ParseResult<std::vector<ParamDef>>::Ok(std::move(params));
    }

    // 解析函数体 { stmts }，返回 (body, range)
    ParseResult<std::pair<std::vector<StmtNode>, SourceRange>> parse_function_body() {
        if (cur.kind != TokenKind::LBrace) {
            return ParseResult<std::pair<std::vector<StmtNode>, SourceRange>>::Err(
                    make_parse_error(source, cur, "expected '{' before function body"));
        }
        Token lbrace = cur;
        advance();
        bool saved_top_level = is_top_level_;
        is_top_level_ = false;
        std::vector<StmtNode> body;
        while (cur.kind != TokenKind::RBrace && cur.kind != TokenKind::Eof) {
            auto stmt = parse_stmt();
            if (!stmt.ok()) {
                is_top_level_ = saved_top_level;
                return ParseResult<std::pair<std::vector<StmtNode>, SourceRange>>::Err(stmt.error());
            }
            body.push_back(std::move(stmt.value()));
        }
        auto rb = expect(TokenKind::RBrace);
        is_top_level_ = saved_top_level;
        if (!rb.ok()) {
            return ParseResult<std::pair<std::vector<StmtNode>, SourceRange>>::Err(rb.error());
        }
        SourceRange r{lbrace.range.offset, rb.value().range.offset + 1 - lbrace.range.offset};
        return ParseResult<std::pair<std::vector<StmtNode>, SourceRange>>::Ok(
                std::make_pair(std::move(body), r));
    }

    // 解析箭头函数体（已消费 =>）
    // params: 参数列表；range_start: 整个箭头函数起始偏移；rest_param: 可选 rest 参数名
    ParseResult<ExprNode> parse_arrow_body(std::vector<ParamDef> params, uint32_t range_start,
                                           std::optional<std::string> rest_param = std::nullopt) {
        std::vector<StmtNode> stmts;
        SourceRange body_range;
        if (cur.kind == TokenKind::LBrace) {
            // 块体：复用 parse_function_body
            auto body_result = parse_function_body();
            if (!body_result.ok()) return ParseResult<ExprNode>::Err(body_result.error());
            stmts = std::move(body_result.value().first);
            body_range = body_result.value().second;
        } else {
            // 表达式体：解析表达式，合成 ReturnStatement
            auto expr_result = parse_expr(2);
            if (!expr_result.ok()) return expr_result;
            uint32_t expr_end = range_end(expr_range(expr_result.value()));
            body_range = span(range_start, expr_end);
            ReturnStatement ret;
            ret.argument = std::move(expr_result.value());
            ret.range = body_range;
            stmts.push_back(StmtNode{std::move(ret)});
        }
        uint32_t fn_end = range_end(body_range);
        return ParseResult<ExprNode>::Ok(ExprNode{ArrowFunctionExpression{
            std::move(params),
            std::move(rest_param),
            std::make_shared<std::vector<StmtNode>>(std::move(stmts)),
            span(range_start, fn_end)}});
    }

    // ---- 解构模式解析辅助 ----

    // 解析绑定模式（用于 let/const/var 声明）
    // 当前 token 应为 [ 或 {
    ParseResult<PatternNode> parse_binding_pattern() {
        if (cur.kind == TokenKind::LBracket) {
            return parse_array_binding_pattern();
        }
        if (cur.kind == TokenKind::LBrace) {
            return parse_object_binding_pattern();
        }
        if (cur.kind == TokenKind::Ident) {
            Token id = cur;
            std::string name{token_text(cur)};
            advance();
            return ParseResult<PatternNode>::Ok(
                PatternNode{IdentifierPattern{std::move(name), id.range}});
        }
        return ParseResult<PatternNode>::Err(
            make_parse_error(source, cur, "expected binding pattern"));
    }

    ParseResult<PatternNode> parse_array_binding_pattern() {
        Token lb = cur;
        advance();  // 消费 [
        std::vector<std::optional<ArrayPatternElement>> elements;
        std::unique_ptr<PatternNode> rest_pat;

        while (cur.kind != TokenKind::RBracket && cur.kind != TokenKind::Eof) {
            if (cur.kind == TokenKind::Comma) {
                // elision hole
                elements.push_back(std::nullopt);
                advance();
                continue;
            }
            if (cur.kind == TokenKind::DotDotDot) {
                advance();  // 消费 ...
                auto rest_r = parse_binding_pattern();
                if (!rest_r.ok()) return ParseResult<PatternNode>::Err(rest_r.error());
                rest_pat = std::make_unique<PatternNode>(std::move(rest_r.value()));
                break;
            }
            // 普通元素
            Token elem_start = cur;
            auto pat_r = parse_binding_pattern();
            if (!pat_r.ok()) return ParseResult<PatternNode>::Err(pat_r.error());
            std::optional<std::unique_ptr<ExprNode>> default_val;
            if (cur.kind == TokenKind::Eq) {
                advance();  // 消费 =
                auto dv = parse_expr(1);
                if (!dv.ok()) return ParseResult<PatternNode>::Err(dv.error());
                default_val = std::make_unique<ExprNode>(std::move(dv.value()));
            }
            elements.push_back(ArrayPatternElement{
                std::make_unique<PatternNode>(std::move(pat_r.value())),
                std::move(default_val),
                elem_start.range});
            if (cur.kind == TokenKind::Comma) {
                advance();
            } else {
                break;
            }
        }

        if (cur.kind == TokenKind::RBracket) {
            Token rb = cur;
            advance();
            SourceRange range = span(lb.range.offset, range_end(rb.range));
            return ParseResult<PatternNode>::Ok(
                PatternNode{ArrayPattern{std::move(elements), std::move(rest_pat), range}});
        }
        return ParseResult<PatternNode>::Err(
            make_parse_error(source, cur, "expected ']' after array pattern"));
    }

    ParseResult<PatternNode> parse_object_binding_pattern() {
        Token lb = cur;
        advance();  // 消费 {
        std::vector<ObjectPatternProperty> properties;
        std::unique_ptr<PatternNode> rest_pat;

        while (cur.kind != TokenKind::RBrace && cur.kind != TokenKind::Eof) {
            if (cur.kind == TokenKind::DotDotDot) {
                advance();  // 消费 ...
                // rest 必须是 IdentifierPattern
                if (cur.kind != TokenKind::Ident) {
                    return ParseResult<PatternNode>::Err(
                        make_parse_error(source, cur, "rest element must be an identifier"));
                }
                Token rest_id = cur;
                std::string rest_name{token_text(cur)};
                advance();
                rest_pat = std::make_unique<PatternNode>(
                    PatternNode{IdentifierPattern{std::move(rest_name), rest_id.range}});
                break;
            }

            // 解析 key（支持 Ident / String / Number / 关键字 / 计算键 [expr]）
            std::string key;
            Token key_tok = cur;
            bool computed = false;
            std::unique_ptr<ExprNode> key_expr_bp;
            if (cur.kind == TokenKind::LBracket) {
                advance();  // 消费 [
                auto ke = parse_expr(1);
                if (!ke.ok()) return ParseResult<PatternNode>::Err(ke.error());
                auto rb = expect(TokenKind::RBracket);
                if (!rb.ok()) return ParseResult<PatternNode>::Err(rb.error());
                key_expr_bp = std::make_unique<ExprNode>(std::move(ke.value()));
                computed = true;
            } else if (cur.kind == TokenKind::Ident) {
                key = std::string{token_text(cur)};
                advance();
            } else if (cur.kind == TokenKind::String) {
                key = decode_string(token_text(cur));
                advance();
            } else if (cur.kind == TokenKind::Number) {
                key = number_to_property_key(parse_number_text(token_text(cur)));
                advance();
            } else if (is_keyword(cur.kind)) {
                // 允许关键字作为属性键（如 {for: x}）
                key = std::string{token_text(cur)};
                advance();
            } else {
                return ParseResult<PatternNode>::Err(
                    make_parse_error(source, cur, "expected property key in object pattern"));
            }

            std::unique_ptr<PatternNode> value_pat;
            std::optional<std::unique_ptr<ExprNode>> default_val;

            if (cur.kind == TokenKind::Colon) {
                // key: pattern[= default]  (必须有冒号，computed 键必须有冒号)
                advance();  // 消费 :
                auto vp_r = parse_binding_pattern();
                if (!vp_r.ok()) return ParseResult<PatternNode>::Err(vp_r.error());
                value_pat = std::make_unique<PatternNode>(std::move(vp_r.value()));
                if (cur.kind == TokenKind::Eq) {
                    advance();
                    auto dv = parse_expr(1);
                    if (!dv.ok()) return ParseResult<PatternNode>::Err(dv.error());
                    default_val = std::make_unique<ExprNode>(std::move(dv.value()));
                }
            } else if (!computed) {
                // shorthand: {key} 或 {key = default}
                value_pat = std::make_unique<PatternNode>(
                    PatternNode{IdentifierPattern{key, key_tok.range}});
                if (cur.kind == TokenKind::Eq) {
                    advance();
                    auto dv = parse_expr(1);
                    if (!dv.ok()) return ParseResult<PatternNode>::Err(dv.error());
                    default_val = std::make_unique<ExprNode>(std::move(dv.value()));
                }
            } else {
                return ParseResult<PatternNode>::Err(
                    make_parse_error(source, cur, "expected ':' after computed key in object pattern"));
            }

            ObjectPatternProperty opp;
            opp.key = std::move(key);
            opp.computed = computed;
            if (computed) opp.key_expr = std::move(key_expr_bp);
            opp.value_pattern = std::move(value_pat);
            opp.default_value = std::move(default_val);
            opp.range = key_tok.range;
            properties.push_back(std::move(opp));

            if (cur.kind == TokenKind::Comma) {
                advance();
            } else {
                break;
            }
        }

        if (cur.kind == TokenKind::RBrace) {
            Token rb = cur;
            advance();
            SourceRange range = span(lb.range.offset, range_end(rb.range));
            return ParseResult<PatternNode>::Ok(
                PatternNode{ObjectPattern{std::move(properties), std::move(rest_pat), range}});
        }
        return ParseResult<PatternNode>::Err(
            make_parse_error(source, cur, "expected '}' after object pattern"));
    }

    // 将 ArrayExpression 转换为 ArrayPattern（赋值模式）
    // arr 是可修改的引用，以便 move 内部 unique_ptr 成员
    static ParseResult<PatternNode> convert_array_to_pattern(ArrayExpression& arr) {
        std::vector<std::optional<ArrayPatternElement>> elements;
        std::unique_ptr<PatternNode> rest_pat;
        for (size_t i = 0; i < arr.elements.size(); ++i) {
            auto& elem_opt = arr.elements[i];
            if (!elem_opt.has_value()) {
                elements.push_back(std::nullopt);
                continue;
            }
            ExprNode& elem = **elem_opt;
            if (std::holds_alternative<SpreadElement>(elem.v)) {
                // rest 元素
                auto& sp = std::get<SpreadElement>(elem.v);
                auto rest_r = convert_expr_to_pattern(*sp.argument);
                if (!rest_r.ok()) return rest_r;
                rest_pat = std::make_unique<PatternNode>(std::move(rest_r.value()));
                break;
            }
            // 可能是 AssignmentExpression（默认值）
            if (std::holds_alternative<AssignmentExpression>(elem.v)) {
                auto& ae = std::get<AssignmentExpression>(elem.v);
                if (ae.op != AssignOp::Assign) {
                    return ParseResult<PatternNode>::Err(
                        Error{ErrorKind::Syntax, "invalid destructuring assignment"});
                }
                SourceRange elem_range = ae.range;
                auto sub_pat = PatternNode{IdentifierPattern{ae.target, ae.range}};
                auto default_val = std::move(ae.value);
                elements.push_back(ArrayPatternElement{
                    std::make_unique<PatternNode>(std::move(sub_pat)),
                    std::optional<std::unique_ptr<ExprNode>>{std::move(default_val)},
                    elem_range});
                continue;
            }
            SourceRange elem_range = expr_range(elem);
            auto sub_pat_r = convert_expr_to_pattern(elem);
            if (!sub_pat_r.ok()) return sub_pat_r;
            elements.push_back(ArrayPatternElement{
                std::make_unique<PatternNode>(std::move(sub_pat_r.value())),
                std::nullopt,
                elem_range});
        }
        return ParseResult<PatternNode>::Ok(
            PatternNode{ArrayPattern{std::move(elements), std::move(rest_pat), arr.range}});
    }

    // 将 ObjectExpression 转换为 ObjectPattern（赋值模式）
    // obj 是可修改的引用，以便 move 内部 unique_ptr 成员
    static ParseResult<PatternNode> convert_object_to_pattern(ObjectExpression& obj) {
        std::vector<ObjectPatternProperty> properties;
        std::unique_ptr<PatternNode> rest_pat;
        for (size_t i = 0; i < obj.properties.size(); ++i) {
            auto& prop = obj.properties[i];

            // spread: ...rest  (key == "" sentinel, value is SpreadElement, not computed)
            if (!prop.computed && prop.key.empty() &&
                std::holds_alternative<SpreadElement>(prop.value->v)) {
                auto& sp = std::get<SpreadElement>(prop.value->v);
                auto rest_r = convert_expr_to_pattern(*sp.argument);
                if (!rest_r.ok()) return rest_r;
                rest_pat = std::make_unique<PatternNode>(std::move(rest_r.value()));
                continue;
            }

            // value 可能是 Identifier（shorthand）或 AssignmentExpression（带默认值）
            std::unique_ptr<PatternNode> val_pat;
            std::optional<std::unique_ptr<ExprNode>> default_val;
            if (std::holds_alternative<AssignmentExpression>(prop.value->v)) {
                auto& ae = std::get<AssignmentExpression>(prop.value->v);
                if (ae.op != AssignOp::Assign) {
                    return ParseResult<PatternNode>::Err(
                        Error{ErrorKind::Syntax, "invalid destructuring assignment"});
                }
                val_pat = std::make_unique<PatternNode>(
                    PatternNode{IdentifierPattern{ae.target, ae.range}});
                default_val = std::move(ae.value);
            } else {
                auto vpr = convert_expr_to_pattern(*prop.value);
                if (!vpr.ok()) return vpr;
                val_pat = std::make_unique<PatternNode>(std::move(vpr.value()));
            }
            ObjectPatternProperty opp;
            opp.key = prop.key;
            opp.computed = prop.computed;
            if (prop.computed) opp.key_expr = std::move(prop.key_expr);
            opp.value_pattern = std::move(val_pat);
            opp.default_value = std::move(default_val);
            opp.range = prop.range;
            properties.push_back(std::move(opp));
        }
        return ParseResult<PatternNode>::Ok(
            PatternNode{ObjectPattern{std::move(properties), std::move(rest_pat), obj.range}});
    }

    // 将表达式节点转换为绑定模式节点（用于赋值模式 cover grammar）
    // expr 是可修改的引用，以便 move 内部 unique_ptr 成员
    static ParseResult<PatternNode> convert_expr_to_pattern(ExprNode& expr) {
        if (std::holds_alternative<Identifier>(expr.v)) {
            const auto& id = std::get<Identifier>(expr.v);
            return ParseResult<PatternNode>::Ok(
                PatternNode{IdentifierPattern{id.name, id.range}});
        }
        if (std::holds_alternative<ArrayExpression>(expr.v)) {
            return convert_array_to_pattern(std::get<ArrayExpression>(expr.v));
        }
        if (std::holds_alternative<ObjectExpression>(expr.v)) {
            return convert_object_to_pattern(std::get<ObjectExpression>(expr.v));
        }
        return ParseResult<PatternNode>::Err(
            Error{ErrorKind::Syntax, "invalid assignment target"});
    }

    // 检查 token kind 是否为关键字（用于对象属性键）
    static bool is_keyword(TokenKind k) {
        switch (k) {
            case TokenKind::KwVar: case TokenKind::KwLet: case TokenKind::KwConst:
            case TokenKind::KwIf: case TokenKind::KwElse: case TokenKind::KwWhile:
            case TokenKind::KwFor: case TokenKind::KwReturn: case TokenKind::KwFunction:
            case TokenKind::KwNew: case TokenKind::KwDelete: case TokenKind::KwTypeof:
            case TokenKind::KwVoid: case TokenKind::KwTrue: case TokenKind::KwFalse:
            case TokenKind::KwNull: case TokenKind::KwThis: case TokenKind::KwThrow:
            case TokenKind::KwTry: case TokenKind::KwCatch: case TokenKind::KwFinally:
            case TokenKind::KwBreak: case TokenKind::KwContinue:
                return true;
            default:
                return false;
        }
    }

    // ---- 语句解析 ----

    ParseResult<StmtNode> parse_var_decl() {
        // cur 已是 KwLet/KwConst/KwVar
        Token kw = cur;
        advance();
        VarKind kind;
        switch (kw.kind) {
            case TokenKind::KwLet:
                kind = VarKind::Let;
                break;
            case TokenKind::KwConst:
                kind = VarKind::Const;
                break;
            case TokenKind::KwVar:
                kind = VarKind::Var;
                break;
            default:
                kind = VarKind::Var;
                break;
        }

        // 解构声明：let/const/var { 或 [
        if (cur.kind == TokenKind::LBrace || cur.kind == TokenKind::LBracket) {
            auto pat_r = parse_binding_pattern();
            if (!pat_r.ok()) return ParseResult<StmtNode>::Err(pat_r.error());

            // const/let 必须有初始化器
            if (kind == VarKind::Const || kind == VarKind::Let) {
                if (cur.kind != TokenKind::Eq) {
                    return ParseResult<StmtNode>::Err(
                        make_parse_error(source, cur, "destructuring declaration must have an initializer"));
                }
            }

            std::unique_ptr<ExprNode> init_ptr;
            if (cur.kind == TokenKind::Eq) {
                advance();  // 消费 =
                auto expr = parse_expr(0);
                if (!expr.ok()) return ParseResult<StmtNode>::Err(expr.error());
                init_ptr = std::make_unique<ExprNode>(std::move(expr.value()));
            }

            auto semi = consume_semicolon();
            if (!semi.ok()) return ParseResult<StmtNode>::Err(semi.error());

            uint32_t decl_end = range_end(semi.value().range);
            if (decl_end == semi.value().range.offset && init_ptr) {
                decl_end = range_end(expr_range(*init_ptr));
            }
            SourceRange range = span(kw.range.offset, decl_end);
            return ParseResult<StmtNode>::Ok(StmtNode{DestructuringDeclaration{
                kind,
                std::make_unique<PatternNode>(std::move(pat_r.value())),
                std::move(init_ptr),
                range}});
        }

        // 期望标识符
        if (cur.kind != TokenKind::Ident) {
            return ParseResult<StmtNode>::Err(make_parse_error(source, cur, "expected identifier after var/let/const"));
        }
        std::string name{token_text(cur)};
        Token name_tok = cur;
        advance();

        std::optional<ExprNode> init;
        if (cur.kind == TokenKind::Eq) {
            advance();  // 消费 =
            auto expr = parse_expr(0);
            if (!expr.ok()) return ParseResult<StmtNode>::Err(expr.error());
            init = std::move(expr.value());
        } else if (kind == VarKind::Const) {
            // const 必须有初始化器
            return ParseResult<StmtNode>::Err(
                    make_parse_error(source, cur, "const declaration must have an initializer"));
        }

        auto semi = consume_semicolon();
        if (!semi.ok()) return ParseResult<StmtNode>::Err(semi.error());

        uint32_t decl_end = range_end(semi.value().range);
        if (decl_end == semi.value().range.offset) {
            // 虚拟分号（ASI），用 init 或 name 的末尾
            if (init.has_value()) {
                decl_end = range_end(expr_range(*init));
            } else {
                decl_end = range_end(name_tok.range);
            }
        }
        SourceRange range = span(kw.range.offset, decl_end);
        return ParseResult<StmtNode>::Ok(StmtNode{VariableDeclaration{kind, std::move(name), std::move(init), range}});
    }

    ParseResult<StmtNode> parse_block_stmt() {
        // cur 是 LBrace
        Token lbrace = cur;
        advance();  // 消费 {
        bool saved_top_level = is_top_level_;
        is_top_level_ = false;
        std::vector<StmtNode> body;
        while (cur.kind != TokenKind::RBrace && cur.kind != TokenKind::Eof) {
            auto stmt = parse_stmt();
            if (!stmt.ok()) {
                is_top_level_ = saved_top_level;
                return stmt;
            }
            body.push_back(std::move(stmt.value()));
        }
        auto rb = expect(TokenKind::RBrace);
        is_top_level_ = saved_top_level;
        if (!rb.ok()) return ParseResult<StmtNode>::Err(rb.error());
        SourceRange range{lbrace.range.offset, rb.value().range.offset + 1 - lbrace.range.offset};
        return ParseResult<StmtNode>::Ok(StmtNode{BlockStatement{std::move(body), range}});
    }

    ParseResult<StmtNode> parse_if_stmt() {
        // cur 是 KwIf
        Token kw = cur;
        advance();
        auto lp = expect(TokenKind::LParen);
        if (!lp.ok()) return ParseResult<StmtNode>::Err(lp.error());
        auto test = parse_expr(0);
        if (!test.ok()) return ParseResult<StmtNode>::Err(test.error());
        auto rp = expect(TokenKind::RParen);
        if (!rp.ok()) return ParseResult<StmtNode>::Err(rp.error());
        bool saved_top_level = is_top_level_;
        is_top_level_ = false;
        auto consequent = parse_stmt();
        if (!consequent.ok()) {
            is_top_level_ = saved_top_level;
            return consequent;
        }
        std::unique_ptr<StmtNode> alt_ptr = nullptr;
        if (cur.kind == TokenKind::KwElse) {
            advance();
            auto alt = parse_stmt();
            if (!alt.ok()) {
                is_top_level_ = saved_top_level;
                return alt;
            }
            alt_ptr = std::make_unique<StmtNode>(std::move(alt.value()));
        }
        is_top_level_ = saved_top_level;
        uint32_t if_end = alt_ptr ? range_end(stmt_range(*alt_ptr)) : range_end(stmt_range(consequent.value()));
        return ParseResult<StmtNode>::Ok(
                StmtNode{IfStatement{std::move(test.value()), std::make_unique<StmtNode>(std::move(consequent.value())),
                                     std::move(alt_ptr), span(kw.range.offset, if_end)}});
    }

    ParseResult<StmtNode> parse_while_stmt() {
        Token kw = cur;
        advance();
        auto lp = expect(TokenKind::LParen);
        if (!lp.ok()) return ParseResult<StmtNode>::Err(lp.error());
        auto test = parse_expr(0);
        if (!test.ok()) return ParseResult<StmtNode>::Err(test.error());
        auto rp = expect(TokenKind::RParen);
        if (!rp.ok()) return ParseResult<StmtNode>::Err(rp.error());
        bool saved_top_level = is_top_level_;
        is_top_level_ = false;
        auto body = parse_stmt();
        is_top_level_ = saved_top_level;
        if (!body.ok()) return body;
        uint32_t while_end = range_end(stmt_range(body.value()));
        return ParseResult<StmtNode>::Ok(
                StmtNode{WhileStatement{std::move(test.value()), std::make_unique<StmtNode>(std::move(body.value())),
                                        span(kw.range.offset, while_end)}});
    }

    ParseResult<StmtNode> parse_return_stmt() {
        Token kw = cur;
        advance();
        std::optional<ExprNode> arg;
        if (!got_lf && cur.kind != TokenKind::Semicolon && cur.kind != TokenKind::RBrace &&
            cur.kind != TokenKind::Eof) {
            auto expr = parse_expr(0);
            if (!expr.ok()) return ParseResult<StmtNode>::Err(expr.error());
            arg = std::move(expr.value());
        }
        auto semi = consume_semicolon();
        if (!semi.ok()) return ParseResult<StmtNode>::Err(semi.error());
        uint32_t ret_end = range_end(semi.value().range);
        if (ret_end == semi.value().range.offset) {
            ret_end = arg.has_value() ? range_end(expr_range(*arg)) : range_end(kw.range);
        }
        return ParseResult<StmtNode>::Ok(StmtNode{ReturnStatement{std::move(arg), span(kw.range.offset, ret_end)}});
    }

    ParseResult<StmtNode> parse_expr_stmt() {
        Token start = cur;
        auto expr = parse_expr(0);
        if (!expr.ok()) return ParseResult<StmtNode>::Err(expr.error());
        auto semi = consume_semicolon();
        if (!semi.ok()) return ParseResult<StmtNode>::Err(semi.error());
        uint32_t es_end = range_end(semi.value().range);
        if (es_end == semi.value().range.offset) {
            es_end = range_end(expr_range(expr.value()));
        }
        return ParseResult<StmtNode>::Ok(
                StmtNode{ExpressionStatement{std::move(expr.value()), span(start.range.offset, es_end)}});
    }

    ParseResult<StmtNode> parse_function_decl_stmt() {
        // cur 是 KwFunction
        Token kw = cur;
        advance();
        bool is_gen = (cur.kind == TokenKind::Star);
        if (is_gen) advance();  // 消费 *
        if (cur.kind != TokenKind::Ident) {
            return ParseResult<StmtNode>::Err(
                    make_parse_error(source, cur, "expected function name"));
        }
        std::string fn_name{token_text(cur)};
        advance();
        std::optional<std::string> fn_rest3;
        auto params_result = parse_function_params(fn_rest3);
        if (!params_result.ok()) return ParseResult<StmtNode>::Err(params_result.error());
        // P2-E: non-async function body resets in_async_function_ context
        // TLA: also reset in_module_ so await inside a plain function is not allowed
        bool saved_in_async_fd = in_async_function_;
        bool saved_in_module_fd = in_module_;
        bool saved_in_gen_fd = in_generator_function_;
        in_async_function_ = false;
        in_module_ = false;
        in_generator_function_ = is_gen;
        auto body_result = parse_function_body();
        in_async_function_ = saved_in_async_fd;
        in_module_ = saved_in_module_fd;
        in_generator_function_ = saved_in_gen_fd;
        if (!body_result.ok()) return ParseResult<StmtNode>::Err(body_result.error());
        uint32_t fn_end = range_end(body_result.value().second);
        auto body_ptr = std::make_shared<std::vector<StmtNode>>(std::move(body_result.value().first));
        FunctionDeclaration fdecl{std::move(fn_name), std::move(params_result.value()), std::move(fn_rest3),
                std::move(body_ptr), span(kw.range.offset, fn_end)};
        fdecl.is_generator = is_gen;
        return ParseResult<StmtNode>::Ok(StmtNode{std::move(fdecl)});
    }

    ParseResult<StmtNode> parse_throw_stmt() {
        Token kw = cur;
        advance();
        // throw 后若立即换行则为语法错误
        if (got_lf) {
            return ParseResult<StmtNode>::Err(make_parse_error(source, cur, "illegal newline after throw"));
        }
        auto arg = parse_expr(0);
        if (!arg.ok()) return ParseResult<StmtNode>::Err(arg.error());
        auto semi = consume_semicolon();
        if (!semi.ok()) return ParseResult<StmtNode>::Err(semi.error());
        uint32_t end = range_end(semi.value().range);
        if (end == semi.value().range.offset) {
            end = range_end(expr_range(arg.value()));
        }
        return ParseResult<StmtNode>::Ok(
                StmtNode{ThrowStatement{std::move(arg.value()), span(kw.range.offset, end)}});
    }

    // 解析 block statement 并返回 BlockStatement（不包装为 StmtNode）
    ParseResult<BlockStatement> parse_block() {
        Token lbrace = cur;
        advance();  // 消费 {
        bool saved_top_level = is_top_level_;
        is_top_level_ = false;
        std::vector<StmtNode> body;
        while (cur.kind != TokenKind::RBrace && cur.kind != TokenKind::Eof) {
            auto stmt = parse_stmt();
            if (!stmt.ok()) {
                is_top_level_ = saved_top_level;
                return ParseResult<BlockStatement>::Err(stmt.error());
            }
            body.push_back(std::move(stmt.value()));
        }
        auto rb = expect(TokenKind::RBrace);
        is_top_level_ = saved_top_level;
        if (!rb.ok()) return ParseResult<BlockStatement>::Err(rb.error());
        SourceRange range{lbrace.range.offset, rb.value().range.offset + 1 - lbrace.range.offset};
        return ParseResult<BlockStatement>::Ok(BlockStatement{std::move(body), range});
    }

    ParseResult<StmtNode> parse_try_stmt() {
        Token kw = cur;
        advance();  // 消费 try
        if (cur.kind != TokenKind::LBrace) {
            return ParseResult<StmtNode>::Err(make_parse_error(source, cur, "expected '{' after try"));
        }
        auto block = parse_block();
        if (!block.ok()) return ParseResult<StmtNode>::Err(block.error());

        std::optional<CatchClause> handler;
        std::optional<BlockStatement> finalizer;

        if (cur.kind == TokenKind::KwCatch) {
            Token catch_tok = cur;
            advance();  // 消费 catch
            auto lp = expect(TokenKind::LParen);
            if (!lp.ok()) return ParseResult<StmtNode>::Err(lp.error());
            if (cur.kind != TokenKind::Ident) {
                return ParseResult<StmtNode>::Err(make_parse_error(source, cur, "expected catch parameter"));
            }
            std::string param{token_text(cur)};
            advance();
            auto rp = expect(TokenKind::RParen);
            if (!rp.ok()) return ParseResult<StmtNode>::Err(rp.error());
            if (cur.kind != TokenKind::LBrace) {
                return ParseResult<StmtNode>::Err(make_parse_error(source, cur, "expected '{' after catch(...)"));
            }
            auto catch_body = parse_block();
            if (!catch_body.ok()) return ParseResult<StmtNode>::Err(catch_body.error());
            SourceRange catch_range = span(catch_tok.range.offset, range_end(catch_body.value().range));
            handler = CatchClause{std::move(param), std::move(catch_body.value()), catch_range};
        }

        if (cur.kind == TokenKind::KwFinally) {
            advance();  // 消费 finally
            if (cur.kind != TokenKind::LBrace) {
                return ParseResult<StmtNode>::Err(make_parse_error(source, cur, "expected '{' after finally"));
            }
            auto fin = parse_block();
            if (!fin.ok()) return ParseResult<StmtNode>::Err(fin.error());
            finalizer = std::move(fin.value());
        }

        if (!handler.has_value() && !finalizer.has_value()) {
            return ParseResult<StmtNode>::Err(
                    make_parse_error(source, cur, "try statement must have catch or finally"));
        }

        uint32_t try_end = finalizer.has_value() ? range_end(finalizer->range)
                                                  : range_end(handler->range);
        return ParseResult<StmtNode>::Ok(StmtNode{TryStatement{
                std::move(block.value()), std::move(handler), std::move(finalizer),
                span(kw.range.offset, try_end)}});
    }

    ParseResult<StmtNode> parse_break_stmt() {
        Token kw = cur;
        advance();
        std::optional<std::string> label;
        if (!got_lf && cur.kind == TokenKind::Ident) {
            label = std::string(token_text(cur));
            advance();
        }
        auto semi = consume_semicolon();
        if (!semi.ok()) return ParseResult<StmtNode>::Err(semi.error());
        uint32_t end = range_end(semi.value().range);
        if (end == semi.value().range.offset) {
            end = range_end(kw.range);
        }
        return ParseResult<StmtNode>::Ok(StmtNode{BreakStatement{std::move(label), span(kw.range.offset, end)}});
    }

    ParseResult<StmtNode> parse_continue_stmt() {
        Token kw = cur;
        advance();
        std::optional<std::string> label;
        if (!got_lf && cur.kind == TokenKind::Ident) {
            label = std::string(token_text(cur));
            advance();
        }
        auto semi = consume_semicolon();
        if (!semi.ok()) return ParseResult<StmtNode>::Err(semi.error());
        uint32_t end = range_end(semi.value().range);
        if (end == semi.value().range.offset) {
            end = range_end(kw.range);
        }
        return ParseResult<StmtNode>::Ok(StmtNode{ContinueStatement{std::move(label), span(kw.range.offset, end)}});
    }

    // Returns true if cur is the identifier "in" (not a keyword, just contextual).
    bool is_in_token() const {
        return cur.kind == TokenKind::Ident && token_text(cur) == "in";
    }

    // Returns true if cur is the contextual keyword "of".
    bool is_of_token() const {
        return cur.kind == TokenKind::Ident && token_text(cur) == "of";
    }

    ParseResult<StmtNode> parse_for_stmt() {
        Token kw = cur;
        advance();  // 消费 for
        auto lp = expect(TokenKind::LParen);
        if (!lp.ok()) return ParseResult<StmtNode>::Err(lp.error());

        // ---- for...in 检测 ----
        // Case 1: for (var/let/const binding in right)
        if (cur.kind == TokenKind::KwVar || cur.kind == TokenKind::KwLet || cur.kind == TokenKind::KwConst) {
            VarKind var_kind = (cur.kind == TokenKind::KwVar)   ? VarKind::Var
                               : (cur.kind == TokenKind::KwLet) ? VarKind::Let
                                                                 : VarKind::Const;
            Token kw_tok = cur;
            advance();  // 消费 var/let/const
            // 解构模式：for (const/let/var [pattern] of ...) 或 for (const/let/var {pattern} of ...)
            if (cur.kind == TokenKind::LBracket || cur.kind == TokenKind::LBrace) {
                auto pat_r = parse_binding_pattern();
                if (!pat_r.ok()) return ParseResult<StmtNode>::Err(pat_r.error());
                if (!is_of_token()) {
                    return ParseResult<StmtNode>::Err(
                        make_parse_error(source, cur, "expected 'of' after destructuring pattern in for"));
                }
                advance();  // 消费 `of`
                auto right = parse_expr(0);
                if (!right.ok()) return ParseResult<StmtNode>::Err(right.error());
                auto rp = expect(TokenKind::RParen);
                if (!rp.ok()) return ParseResult<StmtNode>::Err(rp.error());
                bool saved_top = is_top_level_;
                is_top_level_ = false;
                auto body = parse_stmt();
                is_top_level_ = saved_top;
                if (!body.ok()) return body;
                uint32_t end = range_end(stmt_range(body.value()));
                ForOfStatement fos;
                fos.has_decl = true;
                fos.var_kind = var_kind;
                fos.binding = "";
                fos.pattern_binding = std::make_unique<PatternNode>(std::move(pat_r.value()));
                fos.right = std::make_unique<ExprNode>(std::move(right.value()));
                fos.body = std::make_unique<StmtNode>(std::move(body.value()));
                fos.range = span(kw.range.offset, end);
                return ParseResult<StmtNode>::Ok(StmtNode{std::move(fos)});
            }
            // Must be followed by an identifier (binding name)
            if (cur.kind == TokenKind::Ident) {
                std::string binding_name{token_text(cur)};
                Token id_tok = cur;
                advance();  // 消费 binding name
                // Check for `in`
                if (is_in_token()) {
                    advance();  // 消费 `in`
                    auto right = parse_expr(0);
                    if (!right.ok()) return ParseResult<StmtNode>::Err(right.error());
                    auto rp = expect(TokenKind::RParen);
                    if (!rp.ok()) return ParseResult<StmtNode>::Err(rp.error());
                    bool saved_top = is_top_level_;
                    is_top_level_ = false;
                    auto body = parse_stmt();
                    is_top_level_ = saved_top;
                    if (!body.ok()) return body;
                    uint32_t end = range_end(stmt_range(body.value()));
                    return ParseResult<StmtNode>::Ok(StmtNode{ForInStatement{
                            true, var_kind, std::move(binding_name),
                            std::make_unique<ExprNode>(std::move(right.value())),
                            std::make_unique<StmtNode>(std::move(body.value())),
                            span(kw.range.offset, end)}});
                }
                // Check for `of`
                if (is_of_token()) {
                    advance();  // 消费 `of`
                    auto right = parse_expr(0);
                    if (!right.ok()) return ParseResult<StmtNode>::Err(right.error());
                    auto rp = expect(TokenKind::RParen);
                    if (!rp.ok()) return ParseResult<StmtNode>::Err(rp.error());
                    bool saved_top = is_top_level_;
                    is_top_level_ = false;
                    auto body = parse_stmt();
                    is_top_level_ = saved_top;
                    if (!body.ok()) return body;
                    uint32_t end = range_end(stmt_range(body.value()));
                    return ParseResult<StmtNode>::Ok(StmtNode{ForOfStatement{
                            true, var_kind, std::move(binding_name), nullptr,
                            std::make_unique<ExprNode>(std::move(right.value())),
                            std::make_unique<StmtNode>(std::move(body.value())),
                            span(kw.range.offset, end)}});
                }
                // Not for...in or for...of — reconstruct a var_decl init and fall through to ForStatement.
                // Build a VariableDeclaration StmtNode manually.
                std::optional<ExprNode> var_init;
                SourceRange decl_end = id_tok.range;
                if (cur.kind == TokenKind::Eq) {
                    advance();  // 消费 =
                    auto init_expr = parse_expr(0);
                    if (!init_expr.ok()) return ParseResult<StmtNode>::Err(init_expr.error());
                    decl_end = expr_range(init_expr.value());
                    var_init = std::move(init_expr.value());
                }
                auto semi1 = expect(TokenKind::Semicolon);
                if (!semi1.ok()) return ParseResult<StmtNode>::Err(semi1.error());
                SourceRange decl_range = span(kw_tok.range.offset, range_end(decl_end));
                auto init_node = std::make_unique<StmtNode>(StmtNode{VariableDeclaration{
                        var_kind, std::move(binding_name), std::move(var_init), decl_range}});

                // test
                std::optional<ExprNode> test;
                if (cur.kind != TokenKind::Semicolon) {
                    auto t = parse_expr(0);
                    if (!t.ok()) return ParseResult<StmtNode>::Err(t.error());
                    test = std::move(t.value());
                }
                {
                    auto semi2 = expect(TokenKind::Semicolon);
                    if (!semi2.ok()) return ParseResult<StmtNode>::Err(semi2.error());
                }
                // update
                std::optional<ExprNode> update;
                if (cur.kind != TokenKind::RParen) {
                    auto u = parse_expr(0);
                    if (!u.ok()) return ParseResult<StmtNode>::Err(u.error());
                    update = std::move(u.value());
                }
                {
                    auto rp2 = expect(TokenKind::RParen);
                    if (!rp2.ok()) return ParseResult<StmtNode>::Err(rp2.error());
                }
                bool saved_top = is_top_level_;
                is_top_level_ = false;
                auto body = parse_stmt();
                is_top_level_ = saved_top;
                if (!body.ok()) return body;
                uint32_t for_end = range_end(stmt_range(body.value()));
                return ParseResult<StmtNode>::Ok(StmtNode{ForStatement{
                        std::move(init_node), std::move(test), std::move(update),
                        std::make_unique<StmtNode>(std::move(body.value())),
                        span(kw.range.offset, for_end)}});
            }
            // Not an ident after var/let/const — fall through to normal ForStatement parsing
            // by re-invoking parse_var_decl won't work cleanly since we consumed the keyword.
            // Handle by building the VariableDeclaration: cur must have been a non-ident token.
            // This is a rare case (e.g. `for (var ; ...)`) — use parse_var_decl-like logic here.
            // Actually: parse the rest of the declaration (may have no binding name at all).
            // For simplicity, return a parse error since `for (var ; ...)` is not valid JS.
            return ParseResult<StmtNode>::Err(
                    make_parse_error(source, cur, "expected identifier after var/let/const in for statement"));
        }

        // Case 2: for (expr in right) — no declaration, existing variable
        if (cur.kind != TokenKind::Semicolon) {
            // Save no_in_ state and set to true to prevent `in` from being parsed as binary op
            bool saved_no_in = no_in_;
            no_in_ = true;
            auto expr = parse_expr(0);
            no_in_ = saved_no_in;
            if (!expr.ok()) return ParseResult<StmtNode>::Err(expr.error());

            // Check if this is for...in
            if (is_in_token()) {
                // LHS must be an identifier for simple binding
                if (!std::holds_alternative<Identifier>(expr.value().v)) {
                    return ParseResult<StmtNode>::Err(
                            make_parse_error(source, cur, "invalid for...in left-hand side"));
                }
                std::string binding_name = std::get<Identifier>(expr.value().v).name;
                advance();  // 消费 `in`
                auto right = parse_expr(0);
                if (!right.ok()) return ParseResult<StmtNode>::Err(right.error());
                auto rp = expect(TokenKind::RParen);
                if (!rp.ok()) return ParseResult<StmtNode>::Err(rp.error());
                bool saved_top = is_top_level_;
                is_top_level_ = false;
                auto body = parse_stmt();
                is_top_level_ = saved_top;
                if (!body.ok()) return body;
                uint32_t end = range_end(stmt_range(body.value()));
                return ParseResult<StmtNode>::Ok(StmtNode{ForInStatement{
                        false, VarKind::Var /* unused */, std::move(binding_name),
                        std::make_unique<ExprNode>(std::move(right.value())),
                        std::make_unique<StmtNode>(std::move(body.value())),
                        span(kw.range.offset, end)}});
            }

            // Check if this is for...of
            if (is_of_token()) {
                // LHS must be an identifier for simple binding
                if (!std::holds_alternative<Identifier>(expr.value().v)) {
                    return ParseResult<StmtNode>::Err(
                            make_parse_error(source, cur, "invalid for...of left-hand side"));
                }
                std::string binding_name = std::get<Identifier>(expr.value().v).name;
                advance();  // 消费 `of`
                auto right = parse_expr(0);
                if (!right.ok()) return ParseResult<StmtNode>::Err(right.error());
                auto rp = expect(TokenKind::RParen);
                if (!rp.ok()) return ParseResult<StmtNode>::Err(rp.error());
                bool saved_top = is_top_level_;
                is_top_level_ = false;
                auto body = parse_stmt();
                is_top_level_ = saved_top;
                if (!body.ok()) return body;
                uint32_t end = range_end(stmt_range(body.value()));
                return ParseResult<StmtNode>::Ok(StmtNode{ForOfStatement{
                        false, VarKind::Var /* unused */, std::move(binding_name), nullptr,
                        std::make_unique<ExprNode>(std::move(right.value())),
                        std::make_unique<StmtNode>(std::move(body.value())),
                        span(kw.range.offset, end)}});
            }

            // Not for...in or for...of: treat as ForStatement with expression init
            SourceRange er = expr_range(expr.value());
            auto init = std::make_unique<StmtNode>(StmtNode{ExpressionStatement{std::move(expr.value()), er}});

            auto semi1 = expect(TokenKind::Semicolon);
            if (!semi1.ok()) return ParseResult<StmtNode>::Err(semi1.error());

            // test
            std::optional<ExprNode> test;
            if (cur.kind != TokenKind::Semicolon) {
                auto t = parse_expr(0);
                if (!t.ok()) return ParseResult<StmtNode>::Err(t.error());
                test = std::move(t.value());
            }
            {
                auto semi2 = expect(TokenKind::Semicolon);
                if (!semi2.ok()) return ParseResult<StmtNode>::Err(semi2.error());
            }
            // update
            std::optional<ExprNode> update;
            if (cur.kind != TokenKind::RParen) {
                auto u = parse_expr(0);
                if (!u.ok()) return ParseResult<StmtNode>::Err(u.error());
                update = std::move(u.value());
            }
            {
                auto rp2 = expect(TokenKind::RParen);
                if (!rp2.ok()) return ParseResult<StmtNode>::Err(rp2.error());
            }
            bool saved_top = is_top_level_;
            is_top_level_ = false;
            auto body = parse_stmt();
            is_top_level_ = saved_top;
            if (!body.ok()) return body;
            uint32_t for_end = range_end(stmt_range(body.value()));
            return ParseResult<StmtNode>::Ok(StmtNode{ForStatement{
                    std::move(init), std::move(test), std::move(update),
                    std::make_unique<StmtNode>(std::move(body.value())),
                    span(kw.range.offset, for_end)}});
        }

        // Case 3: for (; ...) — empty init
        auto semi1 = expect(TokenKind::Semicolon);
        if (!semi1.ok()) return ParseResult<StmtNode>::Err(semi1.error());

        // test
        std::optional<ExprNode> test;
        if (cur.kind != TokenKind::Semicolon) {
            auto t = parse_expr(0);
            if (!t.ok()) return ParseResult<StmtNode>::Err(t.error());
            test = std::move(t.value());
        }
        {
            auto semi2 = expect(TokenKind::Semicolon);
            if (!semi2.ok()) return ParseResult<StmtNode>::Err(semi2.error());
        }

        // update
        std::optional<ExprNode> update;
        if (cur.kind != TokenKind::RParen) {
            auto u = parse_expr(0);
            if (!u.ok()) return ParseResult<StmtNode>::Err(u.error());
            update = std::move(u.value());
        }
        {
            auto rp2 = expect(TokenKind::RParen);
            if (!rp2.ok()) return ParseResult<StmtNode>::Err(rp2.error());
        }

        bool saved_top_level = is_top_level_;
        is_top_level_ = false;
        auto body = parse_stmt();
        is_top_level_ = saved_top_level;
        if (!body.ok()) return body;
        uint32_t for_end = range_end(stmt_range(body.value()));
        return ParseResult<StmtNode>::Ok(StmtNode{ForStatement{
                std::nullopt, std::move(test), std::move(update),
                std::make_unique<StmtNode>(std::move(body.value())),
                span(kw.range.offset, for_end)}});
    }

    ParseResult<StmtNode> parse_import_decl() {
        // cur 是 Ident("import")，由 parse_stmt 的上下文关键字检查分发至此
        Token kw = cur;
        advance();  // 消费 import

        // import(specifier) — 动态 import 表达式语句（顶层或非顶层均可）
        if (cur.kind == TokenKind::LParen) {
            advance();  // 消费 (
            auto spec = parse_expr(2);
            if (!spec.ok()) return ParseResult<StmtNode>::Err(spec.error());
            auto rp = expect(TokenKind::RParen);
            if (!rp.ok()) return ParseResult<StmtNode>::Err(rp.error());
            uint32_t call_end = range_end(rp.value().range);
            ExprNode import_call{ImportCallExpression{
                std::make_unique<ExprNode>(std::move(spec.value())),
                span(kw.range.offset, call_end)}};
            // 继续解析可能的 .then() 等方法调用（作为 led 处理）
            while (true) {
                int bp = lbp(cur.kind);
                if (bp <= 0) break;
                Token op_tok = cur;
                advance();
                auto res = led(op_tok, std::move(import_call));
                if (!res.ok()) return ParseResult<StmtNode>::Err(res.error());
                import_call = std::move(res.value());
            }
            auto semi = consume_semicolon();
            if (!semi.ok()) return ParseResult<StmtNode>::Err(semi.error());
            uint32_t es_end = range_end(semi.value().range);
            if (es_end == semi.value().range.offset) {
                es_end = range_end(expr_range(import_call));
            }
            return ParseResult<StmtNode>::Ok(StmtNode{ExpressionStatement{
                std::move(import_call), span(kw.range.offset, es_end)}});
        }

        if (!is_top_level_) {
            return ParseResult<StmtNode>::Err(
                    make_parse_error(source, kw, "import declarations may only appear at top level"));
        }

        // 副作用导入：import 'specifier'
        if (cur.kind == TokenKind::String) {
            std::string spec = decode_string(token_text(cur));
            Token spec_tok = cur;
            advance();
            auto semi = consume_semicolon();
            if (!semi.ok()) return ParseResult<StmtNode>::Err(semi.error());
            uint32_t end = range_end(semi.value().range);
            if (end == semi.value().range.offset) end = range_end(spec_tok.range);
            return ParseResult<StmtNode>::Ok(StmtNode{ImportDeclaration{
                    std::move(spec), {}, span(kw.range.offset, end)}});
        }

        std::vector<ImportSpecifier> specifiers;

        // import * as ns from '...'
        if (cur.kind == TokenKind::Star) {
            Token star_tok = cur;
            advance();  // 消费 *
            if (!is_contextual_keyword("as")) {
                return ParseResult<StmtNode>::Err(
                        make_parse_error(source, cur, "expected 'as' after '*' in import"));
            }
            advance();  // 消费 as
            if (cur.kind != TokenKind::Ident) {
                return ParseResult<StmtNode>::Err(
                        make_parse_error(source, cur, "expected identifier after 'as'"));
            }
            std::string local{token_text(cur)};
            SourceRange spec_range = span(star_tok.range.offset, range_end(cur.range));
            advance();
            specifiers.push_back(ImportSpecifier{"*", std::move(local), true, spec_range});
        } else if (cur.kind == TokenKind::LBrace) {
            // import { x, x as y, ... } from '...'
            advance();  // 消费 {
            while (cur.kind != TokenKind::RBrace && cur.kind != TokenKind::Eof) {
                if (cur.kind != TokenKind::Ident) {
                    return ParseResult<StmtNode>::Err(
                            make_parse_error(source, cur, "expected identifier in import specifier"));
                }
                std::string imported{token_text(cur)};
                uint32_t spec_start = cur.range.offset;
                advance();
                std::string local = imported;
                if (is_contextual_keyword("as")) {
                    advance();  // 消费 as
                    if (cur.kind != TokenKind::Ident) {
                        return ParseResult<StmtNode>::Err(
                                make_parse_error(source, cur, "expected identifier after 'as'"));
                    }
                    local = std::string(token_text(cur));
                    advance();
                }
                SourceRange spec_range = span(spec_start, range_end(cur.range));
                specifiers.push_back(ImportSpecifier{std::move(imported), std::move(local), false, spec_range});
                if (cur.kind == TokenKind::Comma) {
                    advance();
                } else {
                    break;
                }
            }
            auto rb = expect(TokenKind::RBrace);
            if (!rb.ok()) return ParseResult<StmtNode>::Err(rb.error());
        } else if (cur.kind == TokenKind::Ident) {
            // import defaultExport from '...'
            std::string local{token_text(cur)};
            SourceRange spec_range = cur.range;
            advance();
            specifiers.push_back(ImportSpecifier{"default", std::move(local), false, spec_range});
        } else {
            return ParseResult<StmtNode>::Err(
                    make_parse_error(source, cur, "unexpected token in import declaration"));
        }

        // 消费 from
        if (!is_contextual_keyword("from")) {
            return ParseResult<StmtNode>::Err(
                    make_parse_error(source, cur, "expected 'from' in import declaration"));
        }
        advance();  // 消费 from

        if (cur.kind != TokenKind::String) {
            return ParseResult<StmtNode>::Err(
                    make_parse_error(source, cur, "expected module specifier string"));
        }
        std::string spec = decode_string(token_text(cur));
        Token spec_tok = cur;
        advance();

        auto semi = consume_semicolon();
        if (!semi.ok()) return ParseResult<StmtNode>::Err(semi.error());
        uint32_t end = range_end(semi.value().range);
        if (end == semi.value().range.offset) end = range_end(spec_tok.range);

        return ParseResult<StmtNode>::Ok(StmtNode{ImportDeclaration{
                std::move(spec), std::move(specifiers), span(kw.range.offset, end)}});
    }

    ParseResult<StmtNode> parse_export_decl() {
        // cur 是 Ident("export")，由 parse_stmt 的上下文关键字检查分发至此
        Token kw = cur;
        advance();  // 消费 export

        if (!is_top_level_) {
            return ParseResult<StmtNode>::Err(
                    make_parse_error(source, kw, "export declarations may only appear at top level"));
        }

        // export default ...
        if (is_contextual_keyword("default")) {
            advance();  // 消费 default
            // export default function [name]() {}
            if (cur.kind == TokenKind::KwFunction) {
                Token fn_tok = cur;
                advance();  // 消费 function
                std::optional<std::string> fn_name;
                if (cur.kind == TokenKind::Ident) {
                    fn_name = std::string(token_text(cur));
                    advance();
                }
                std::optional<std::string> fn_rest4;
                auto params_result = parse_function_params(fn_rest4);
                if (!params_result.ok()) return ParseResult<StmtNode>::Err(params_result.error());
                auto body_result = parse_function_body();
                if (!body_result.ok()) return ParseResult<StmtNode>::Err(body_result.error());
                uint32_t fn_end = range_end(body_result.value().second);
                auto body_ptr = std::make_shared<std::vector<StmtNode>>(std::move(body_result.value().first));
                std::optional<std::string> saved_fn_name = fn_name;
                auto fe = FunctionExpression{std::move(fn_name), std::move(params_result.value()),
                                             std::move(fn_rest4), std::move(body_ptr),
                                             span(fn_tok.range.offset, fn_end)};
                auto expr_node = std::make_unique<ExprNode>(std::move(fe));
                uint32_t decl_end = fn_end;
                return ParseResult<StmtNode>::Ok(StmtNode{ExportDefaultDeclaration{
                        std::move(expr_node), std::move(saved_fn_name), span(kw.range.offset, decl_end)}});
            }
            // export default async function [name]() {}
            if (is_contextual_keyword("async")) {
                Token async_tok = cur;
                advance();  // 消费 async
                if (cur.kind == TokenKind::KwFunction && !got_lf) {
                    advance();  // 消费 function
                    std::optional<std::string> fn_name;
                    if (cur.kind == TokenKind::Ident) {
                        fn_name = std::string(token_text(cur));
                        advance();
                    }
                    std::optional<std::string> fn_rest5;
                    auto params_result = parse_function_params(fn_rest5);
                    if (!params_result.ok()) return ParseResult<StmtNode>::Err(params_result.error());
                    bool saved_in_async = in_async_function_;
                    in_async_function_ = true;
                    auto body_result = parse_function_body();
                    in_async_function_ = saved_in_async;
                    if (!body_result.ok()) return ParseResult<StmtNode>::Err(body_result.error());
                    uint32_t fn_end = range_end(body_result.value().second);
                    auto body_ptr = std::make_shared<std::vector<StmtNode>>(std::move(body_result.value().first));
                    std::optional<std::string> saved_fn_name = fn_name;
                    auto afe = AsyncFunctionExpression{std::move(fn_name), std::move(params_result.value()),
                                                      std::move(fn_rest5), std::move(body_ptr),
                                                      span(async_tok.range.offset, fn_end)};
                    auto expr_node = std::make_unique<ExprNode>(std::move(afe));
                    return ParseResult<StmtNode>::Ok(StmtNode{ExportDefaultDeclaration{
                            std::move(expr_node), std::move(saved_fn_name), span(kw.range.offset, fn_end)}});
                }
                // async 后不是 function（或有换行）：把 async 当作表达式继续
                // 已经消费了 async，需要把它作为 Identifier nud 处理，然后继续 parse_expr
                Token ident_tok = async_tok;
                auto left = nud(ident_tok);
                if (!left.ok()) return ParseResult<StmtNode>::Err(left.error());
                while (true) {
                    int bp = lbp(cur.kind);
                    if (bp <= 0) break;
                    Token op_tok = cur;
                    advance();
                    auto res = led(op_tok, std::move(left.value()));
                    if (!res.ok()) return ParseResult<StmtNode>::Err(res.error());
                    left = std::move(res);
                }
                auto semi2 = consume_semicolon();
                if (!semi2.ok()) return ParseResult<StmtNode>::Err(semi2.error());
                uint32_t end2 = range_end(semi2.value().range);
                if (end2 == semi2.value().range.offset) end2 = range_end(expr_range(left.value()));
                auto expr_node2 = std::make_unique<ExprNode>(std::move(left.value()));
                return ParseResult<StmtNode>::Ok(StmtNode{ExportDefaultDeclaration{
                        std::move(expr_node2), std::nullopt, span(kw.range.offset, end2)}});
            }
            // export default expr
            auto expr = parse_expr(0);
            if (!expr.ok()) return ParseResult<StmtNode>::Err(expr.error());
            auto semi = consume_semicolon();
            if (!semi.ok()) return ParseResult<StmtNode>::Err(semi.error());
            uint32_t end = range_end(semi.value().range);
            if (end == semi.value().range.offset) end = range_end(expr_range(expr.value()));
            auto expr_node = std::make_unique<ExprNode>(std::move(expr.value()));
            return ParseResult<StmtNode>::Ok(StmtNode{ExportDefaultDeclaration{
                    std::move(expr_node), std::nullopt, span(kw.range.offset, end)}});
        }

        // export { x, y as z }
        if (cur.kind == TokenKind::LBrace) {
            advance();  // 消费 {
            std::vector<ExportSpecifier> specifiers;
            while (cur.kind != TokenKind::RBrace && cur.kind != TokenKind::Eof) {
                if (cur.kind != TokenKind::Ident) {
                    return ParseResult<StmtNode>::Err(
                            make_parse_error(source, cur, "expected identifier in export specifier"));
                }
                std::string local{token_text(cur)};
                uint32_t spec_start = cur.range.offset;
                advance();
                std::string exported = local;
                if (is_contextual_keyword("as")) {
                    advance();  // 消费 as
                    if (cur.kind != TokenKind::Ident) {
                        return ParseResult<StmtNode>::Err(
                                make_parse_error(source, cur, "expected identifier after 'as'"));
                    }
                    exported = std::string(token_text(cur));
                    advance();
                }
                SourceRange spec_range = span(spec_start, range_end(cur.range));
                specifiers.push_back(ExportSpecifier{std::move(local), std::move(exported), spec_range});
                if (cur.kind == TokenKind::Comma) {
                    advance();
                } else {
                    break;
                }
            }
            auto rb = expect(TokenKind::RBrace);
            if (!rb.ok()) return ParseResult<StmtNode>::Err(rb.error());
            // 可选 from 子句（re-export）
            std::optional<std::string> re_source;
            if (is_contextual_keyword("from")) {
                advance();  // 消费 from
                if (cur.kind != TokenKind::String) {
                    return ParseResult<StmtNode>::Err(
                            make_parse_error(source, cur, "expected module specifier string after 'from'"));
                }
                re_source = decode_string(token_text(cur));
                advance();
            }
            auto semi = consume_semicolon();
            if (!semi.ok()) return ParseResult<StmtNode>::Err(semi.error());
            uint32_t end = range_end(semi.value().range);
            if (end == semi.value().range.offset) end = range_end(rb.value().range);
            return ParseResult<StmtNode>::Ok(StmtNode{ExportNamedDeclaration{
                    nullptr, std::move(specifiers), std::move(re_source), span(kw.range.offset, end)}});
        }

        // export const/let/var/function/async function ...
        ParseResult<StmtNode> inner_result = ParseResult<StmtNode>::Err(
                make_parse_error(source, cur, "unexpected token after export"));
        if (cur.kind == TokenKind::KwConst || cur.kind == TokenKind::KwLet || cur.kind == TokenKind::KwVar) {
            inner_result = parse_var_decl();
        } else if (cur.kind == TokenKind::KwFunction) {
            inner_result = parse_function_decl_stmt();
        } else if (is_contextual_keyword("async")) {
            Token async_tok = cur;
            advance();  // 消费 async
            if (cur.kind != TokenKind::KwFunction || got_lf) {
                return ParseResult<StmtNode>::Err(
                    make_parse_error(source, cur, "expected 'function' after 'async' in export declaration"));
            }
            advance();  // 消费 function
            if (cur.kind != TokenKind::Ident) {
                return ParseResult<StmtNode>::Err(
                    make_parse_error(source, cur, "expected function name after 'export async function'"));
            }
            std::string fn_name{token_text(cur)};
            advance();
            std::optional<std::string> fn_rest6;
            auto params_result = parse_function_params(fn_rest6);
            if (!params_result.ok()) return ParseResult<StmtNode>::Err(params_result.error());
            bool saved_in_async = in_async_function_;
            in_async_function_ = true;
            auto body_result = parse_function_body();
            in_async_function_ = saved_in_async;
            if (!body_result.ok()) return ParseResult<StmtNode>::Err(body_result.error());
            uint32_t fn_end = range_end(body_result.value().second);
            auto body_ptr = std::make_shared<std::vector<StmtNode>>(std::move(body_result.value().first));
            inner_result = ParseResult<StmtNode>::Ok(StmtNode{AsyncFunctionDeclaration{
                std::move(fn_name), std::move(params_result.value()), std::move(fn_rest6),
                std::move(body_ptr), span(async_tok.range.offset, fn_end)}});
        } else {
            return inner_result;
        }
        if (!inner_result.ok()) return inner_result;
        auto decl_ptr = std::make_unique<StmtNode>(std::move(inner_result.value()));
        uint32_t end = range_end(stmt_range(*decl_ptr));
        return ParseResult<StmtNode>::Ok(StmtNode{ExportNamedDeclaration{
                std::move(decl_ptr), {}, std::nullopt, span(kw.range.offset, end)}});
    }

    ParseResult<StmtNode> parse_stmt() {
        // import/export are contextual keywords (not in kKeywords), so the lexer
        // produces Ident tokens for them. Only treat them as module declarations
        // when at top level; otherwise they are ordinary identifiers.
        if (is_top_level_ && cur.kind == TokenKind::Ident) {
            auto text = token_text(cur);
            if (text == "import") {
                // peek 下一个 token：import.meta / import['meta'] 是表达式，不是 import 声明
                LexerState saved_lex = lex;
                Token saved_cur = cur;
                bool saved_got_lf = got_lf;
                advance();
                if (cur.kind == TokenKind::Dot || cur.kind == TokenKind::LBracket) {
                    // import.xxx / import[xxx] — 作为表达式语句解析
                    lex = saved_lex;
                    cur = saved_cur;
                    got_lf = saved_got_lf;
                } else {
                    // 普通 import 声明
                    lex = saved_lex;
                    cur = saved_cur;
                    got_lf = saved_got_lf;
                    return parse_import_decl();
                }
            } else if (text == "export") {
                return parse_export_decl();
            }
        }
        switch (cur.kind) {
            case TokenKind::KwLet:
            case TokenKind::KwConst:
            case TokenKind::KwVar:
                return parse_var_decl();
            case TokenKind::LBrace:
                return parse_block_stmt();
            case TokenKind::KwIf:
                return parse_if_stmt();
            case TokenKind::KwWhile:
                return parse_while_stmt();
            case TokenKind::KwReturn:
                return parse_return_stmt();
            case TokenKind::KwFunction:
                return parse_function_decl_stmt();
            case TokenKind::KwThrow:
                return parse_throw_stmt();
            case TokenKind::KwTry:
                return parse_try_stmt();
            case TokenKind::KwBreak:
                return parse_break_stmt();
            case TokenKind::KwContinue:
                return parse_continue_stmt();
            case TokenKind::KwFor:
                return parse_for_stmt();
            case TokenKind::Ident: {
                // async function name(params) { body } — async 函数声明
                if (token_text(cur) == "async") {
                    Token async_tok = cur;
                    advance();  // 消费 async
                    if (cur.kind == TokenKind::KwFunction && !got_lf) {
                        advance();  // 消费 function
                        if (cur.kind != TokenKind::Ident) {
                            return ParseResult<StmtNode>::Err(
                                make_parse_error(source, cur, "expected function name after 'async function'"));
                        }
                        std::string fn_name{token_text(cur)};
                        advance();
                        std::optional<std::string> fn_rest7;
                        auto params_result = parse_function_params(fn_rest7);
                        if (!params_result.ok()) return ParseResult<StmtNode>::Err(params_result.error());
                        bool saved_in_async2 = in_async_function_;
                        in_async_function_ = true;
                        auto body_result = parse_function_body();
                        in_async_function_ = saved_in_async2;
                        if (!body_result.ok()) return ParseResult<StmtNode>::Err(body_result.error());
                        uint32_t fn_end = range_end(body_result.value().second);
                        auto body_ptr = std::make_shared<std::vector<StmtNode>>(
                            std::move(body_result.value().first));
                        return ParseResult<StmtNode>::Ok(StmtNode{AsyncFunctionDeclaration{
                            std::move(fn_name), std::move(params_result.value()), std::move(fn_rest7),
                            std::move(body_ptr), span(async_tok.range.offset, fn_end)}});
                    }
                    // async 后面不是 function：回退，把 async_tok 当普通标识符处理
                    // 将 async_tok 作为表达式 nud 处理
                    Token ident_tok2 = async_tok;
                    auto left2 = nud(ident_tok2);
                    if (!left2.ok()) return ParseResult<StmtNode>::Err(left2.error());
                    while (true) {
                        int bp = lbp(cur.kind);
                        if (bp == 0 && !no_in_ && is_in_token()) bp = 12;
                        if (bp <= 0) break;
                        Token op_tok3 = cur;
                        advance();
                        auto res3 = led(op_tok3, std::move(left2.value()));
                        if (!res3.ok()) return ParseResult<StmtNode>::Err(res3.error());
                        left2 = std::move(res3);
                    }
                    auto semi3 = consume_semicolon();
                    if (!semi3.ok()) return ParseResult<StmtNode>::Err(semi3.error());
                    uint32_t es_end3 = range_end(semi3.value().range);
                    if (es_end3 == semi3.value().range.offset) {
                        es_end3 = range_end(expr_range(left2.value()));
                    }
                    return ParseResult<StmtNode>::Ok(StmtNode{ExpressionStatement{
                        std::move(left2.value()), span(async_tok.range.offset, es_end3)}});
                }
                // 向前看：若下一个 token 是 ':'，则解析为 LabeledStatement
                Token ident_tok = cur;
                advance();
                if (cur.kind == TokenKind::Colon) {
                    advance();  // 消费 :
                    std::string lbl{token_text(ident_tok)};
                    bool saved_top_level = is_top_level_;
                    is_top_level_ = false;
                    auto body = parse_stmt();
                    is_top_level_ = saved_top_level;
                    if (!body.ok()) return body;
                    uint32_t lbl_end = range_end(stmt_range(body.value()));
                    return ParseResult<StmtNode>::Ok(StmtNode{LabeledStatement{
                            std::move(lbl),
                            std::make_unique<StmtNode>(std::move(body.value())),
                            span(ident_tok.range.offset, lbl_end)}});
                }
                // 否则回退：将 ident_tok 作为表达式 nud 处理，继续走表达式语句路径
                // 因为已经 advance 了，需要在当前 cur 基础上继续 parse_expr
                Token start = ident_tok;
                auto left = nud(ident_tok);
                if (!left.ok()) return ParseResult<StmtNode>::Err(left.error());
                // 继续 Pratt loop
                while (true) {
                    int bp = lbp(cur.kind);
                    if (bp == 0 && !no_in_ && is_in_token()) bp = 12;
                    if (bp <= 0) break;
                    Token op_tok2 = cur;
                    advance();
                    auto res = led(op_tok2, std::move(left.value()));
                    if (!res.ok()) return ParseResult<StmtNode>::Err(res.error());
                    left = std::move(res);
                }
                auto semi = consume_semicolon();
                if (!semi.ok()) return ParseResult<StmtNode>::Err(semi.error());
                uint32_t es_end = range_end(semi.value().range);
                if (es_end == semi.value().range.offset) {
                    es_end = range_end(expr_range(left.value()));
                }
                return ParseResult<StmtNode>::Ok(StmtNode{ExpressionStatement{
                        std::move(left.value()), span(start.range.offset, es_end)}});
            }
            default:
                return parse_expr_stmt();
        }
    }

    ParseResult<Program> parse() {
        std::vector<StmtNode> body;
        while (cur.kind != TokenKind::Eof) {
            auto stmt = parse_stmt();
            if (!stmt.ok()) return ParseResult<Program>::Err(stmt.error());
            body.push_back(std::move(stmt.value()));
        }

        // 检查重复导出名
        std::unordered_set<std::string> export_names;
        for (const auto& stmt : body) {
            if (const auto* e = std::get_if<ExportNamedDeclaration>(&stmt.v)) {
                // 带声明的 export：收集声明名
                if (e->declaration) {
                    std::string decl_name;
                    if (const auto* vd = std::get_if<VariableDeclaration>(&e->declaration->v)) {
                        decl_name = vd->name;
                    } else if (const auto* fd = std::get_if<FunctionDeclaration>(&e->declaration->v)) {
                        decl_name = fd->name;
                    } else if (const auto* afd = std::get_if<AsyncFunctionDeclaration>(&e->declaration->v)) {
                        decl_name = afd->name;
                    }
                    if (!decl_name.empty()) {
                        if (!export_names.insert(decl_name).second) {
                            return ParseResult<Program>::Err(
                                    Error{ErrorKind::Syntax, "duplicate export name: " + decl_name});
                        }
                    }
                }
                // 带 specifiers 的 export
                for (const auto& spec : e->specifiers) {
                    if (!export_names.insert(spec.export_name).second) {
                        return ParseResult<Program>::Err(
                                Error{ErrorKind::Syntax, "duplicate export name: " + spec.export_name});
                    }
                }
            } else if (std::holds_alternative<ExportDefaultDeclaration>(stmt.v)) {
                if (!export_names.insert(std::string("default")).second) {
                    return ParseResult<Program>::Err(
                            Error{ErrorKind::Syntax, "duplicate export name: default"});
                }
            }
        }

        uint32_t len = static_cast<uint32_t>(source.size());
        return ParseResult<Program>::Ok(Program{std::move(body), {0, len}});
    }
};

ParseResult<Program> parse_program(std::string_view source, bool is_module) {
    Parser p(source, is_module);
    return p.parse();
}

}  // namespace qppjs
