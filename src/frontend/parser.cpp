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
                              [](const AsyncFunctionExpression& n) { return n.range; },
                              [](const ImportCallExpression& n) { return n.range; },
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
                              [](const ImportDeclaration& n) { return n.range; },
                              [](const ExportNamedDeclaration& n) { return n.range; },
                              [](const ExportDefaultDeclaration& n) { return n.range; },
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
    bool in_module_;         // TLA: 模块顶层上下文，允许 await 表达式

    explicit Parser(std::string_view src, bool is_module = false)
        : source(src), lex(lexer_init(src)), cur{TokenKind::Eof, {0, 0}}, got_lf(false),
          is_top_level_(true), in_async_function_(false), in_module_(is_module) {
        advance();  // 载入第一个 token
    }

    // 推进一个 token，记录换行状态
    void advance() {
        cur = next_token(lex);
        got_lf = lex.got_lf;
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
    static int lbp(TokenKind kind) {
        switch (kind) {
            case TokenKind::Eq:
            case TokenKind::PlusEq:
            case TokenKind::MinusEq:
            case TokenKind::StarEq:
            case TokenKind::SlashEq:
            case TokenKind::PercentEq:
                return 2;
            case TokenKind::PipePipe:
                return 4;
            case TokenKind::AmpAmp:
                return 6;
            case TokenKind::EqEq:
            case TokenKind::BangEq:
            case TokenKind::EqEqEq:
            case TokenKind::BangEqEq:
                return 8;
            case TokenKind::KwInstanceof:
            case TokenKind::Lt:
            case TokenKind::Gt:
            case TokenKind::LtEq:
            case TokenKind::GtEq:
                return 10;
            case TokenKind::Plus:
            case TokenKind::Minus:
                return 12;
            case TokenKind::Star:
            case TokenKind::Slash:
            case TokenKind::Percent:
                return 14;
            case TokenKind::LParen:
                return 16;
            case TokenKind::Dot:
            case TokenKind::LBracket:
                return 18;
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
                    auto params_result = parse_function_params();
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
                        std::move(fn_name), std::move(params_result.value()),
                        std::move(body_ptr), span(tok.range.offset, fn_end)}});
                }
                // await expr — 在 async 函数体内或模块顶层有效
                if (tok_text == "await" && !got_lf && (in_async_function_ || in_module_)) {
                    // 只有当后续不是分号/}时才解析为 await 表达式
                    if (cur.kind != TokenKind::Semicolon && cur.kind != TokenKind::RBrace &&
                        cur.kind != TokenKind::Eof) {
                        auto arg = parse_expr(14);  // 高优先级，不消费逗号/赋值
                        if (!arg.ok()) return arg;
                        uint32_t end = range_end(expr_range(arg.value()));
                        return ParseResult<ExprNode>::Ok(ExprNode{AwaitExpression{
                            std::make_unique<ExprNode>(std::move(arg.value())),
                            span(tok.range.offset, end)}});
                    }
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
                std::string name{tok_text};
                return ParseResult<ExprNode>::Ok(ExprNode{Identifier{std::move(name), tok.range}});
            }
            case TokenKind::KwThis:
                return ParseResult<ExprNode>::Ok(ExprNode{Identifier{"this", tok.range}});
            case TokenKind::LParen: {
                auto inner = parse_expr(0);
                if (!inner.ok()) return inner;
                auto rp = expect(TokenKind::RParen);
                if (!rp.ok()) return ParseResult<ExprNode>::Err(rp.error());
                return inner;
            }
            // 一元前缀
            case TokenKind::Minus: {
                auto operand = parse_expr(15);
                if (!operand.ok()) return operand;
                auto r = span(tok.range.offset, range_end(expr_range(operand.value())));
                return ParseResult<ExprNode>::Ok(ExprNode{
                        UnaryExpression{UnaryOp::Minus, std::make_unique<ExprNode>(std::move(operand.value())), r}});
            }
            case TokenKind::Plus: {
                auto operand = parse_expr(15);
                if (!operand.ok()) return operand;
                auto r = span(tok.range.offset, range_end(expr_range(operand.value())));
                return ParseResult<ExprNode>::Ok(ExprNode{
                        UnaryExpression{UnaryOp::Plus, std::make_unique<ExprNode>(std::move(operand.value())), r}});
            }
            case TokenKind::Bang: {
                auto operand = parse_expr(15);
                if (!operand.ok()) return operand;
                auto r = span(tok.range.offset, range_end(expr_range(operand.value())));
                return ParseResult<ExprNode>::Ok(ExprNode{
                        UnaryExpression{UnaryOp::Bang, std::make_unique<ExprNode>(std::move(operand.value())), r}});
            }
            case TokenKind::KwTypeof: {
                auto operand = parse_expr(15);
                if (!operand.ok()) return operand;
                auto r = span(tok.range.offset, range_end(expr_range(operand.value())));
                return ParseResult<ExprNode>::Ok(ExprNode{
                        UnaryExpression{UnaryOp::Typeof, std::make_unique<ExprNode>(std::move(operand.value())), r}});
            }
            case TokenKind::KwVoid: {
                auto operand = parse_expr(15);
                if (!operand.ok()) return operand;
                auto r = span(tok.range.offset, range_end(expr_range(operand.value())));
                return ParseResult<ExprNode>::Ok(ExprNode{
                        UnaryExpression{UnaryOp::Void, std::make_unique<ExprNode>(std::move(operand.value())), r}});
            }
            case TokenKind::KwNew: {
                // new callee(args)
                uint32_t new_start = tok.range.offset;
                // Parse callee at higher precedence (member access allowed, but not call)
                // Use lbp(Dot)=18 as min_bp to allow member access but stop before call
                auto callee = parse_expr(17);  // stop before LParen (lbp=16) but allow Dot/LBracket (lbp=18)
                if (!callee.ok()) return callee;
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
                // 函数表达式 function [name](params) { body }
                uint32_t fn_start = tok.range.offset;
                std::optional<std::string> fn_name;
                if (cur.kind == TokenKind::Ident) {
                    fn_name = std::string(token_text(cur));
                    advance();
                }
                auto params_result = parse_function_params();
                if (!params_result.ok()) return ParseResult<ExprNode>::Err(params_result.error());
                // P2-E: non-async function body resets in_async_function_ context
                // TLA: also reset in_module_ so await inside a plain function is not allowed
                bool saved_in_async_fe = in_async_function_;
                bool saved_in_module_fe = in_module_;
                in_async_function_ = false;
                in_module_ = false;
                auto body_result = parse_function_body();
                in_async_function_ = saved_in_async_fe;
                in_module_ = saved_in_module_fe;
                if (!body_result.ok()) return ParseResult<ExprNode>::Err(body_result.error());
                uint32_t fn_end = range_end(body_result.value().second);
                auto body_ptr = std::make_shared<std::vector<StmtNode>>(std::move(body_result.value().first));
                return ParseResult<ExprNode>::Ok(ExprNode{FunctionExpression{
                        std::move(fn_name), std::move(params_result.value()),
                        std::move(body_ptr), span(fn_start, fn_end)}});
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
                // 对象字面量 { key: value, ... }
                uint32_t start = tok.range.offset;
                std::vector<ObjectProperty> props;
                while (cur.kind != TokenKind::RBrace && cur.kind != TokenKind::Eof) {
                    std::string key;
                    uint32_t key_start = cur.range.offset;
                    if (cur.kind == TokenKind::Ident) {
                        key = std::string(token_text(cur));
                        advance();
                    } else if (cur.kind == TokenKind::String) {
                        key = decode_string(token_text(cur));
                        advance();
                    } else if (cur.kind == TokenKind::Number) {
                        double num_val = parse_number_text(token_text(cur));
                        key = number_to_property_key(num_val);
                        advance();
                    } else {
                        return ParseResult<ExprNode>::Err(
                                make_parse_error(source, cur, "expected property key"));
                    }
                    if (cur.kind != TokenKind::Colon) {
                        return ParseResult<ExprNode>::Err(
                                make_parse_error(source, cur, "expected ':' after property key"));
                    }
                    advance();  // 消费 :
                    // parse_expr(1) 在 Comma（lbp=0）处停止
                    auto val = parse_expr(1);
                    if (!val.ok()) return val;
                    uint32_t prop_end = range_end(expr_range(val.value()));
                    ObjectProperty prop;
                    prop.key = key;
                    prop.value = std::make_unique<ExprNode>(std::move(val.value()));
                    prop.range = span(key_start, prop_end);
                    props.push_back(std::move(prop));
                    if (cur.kind == TokenKind::Comma) {
                        advance();  // 消费 ,
                    } else {
                        break;
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

        // 赋值：右结合，检查左侧是 Identifier 或 MemberExpression
        if (kind == TokenKind::Eq || kind == TokenKind::PlusEq || kind == TokenKind::MinusEq ||
            kind == TokenKind::StarEq || kind == TokenKind::SlashEq || kind == TokenKind::PercentEq) {
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
        if (kind == TokenKind::PipePipe || kind == TokenKind::AmpAmp) {
            auto right = parse_expr(bp);
            if (!right.ok()) return right;
            LogicalOp lop = (kind == TokenKind::AmpAmp) ? LogicalOp::And : LogicalOp::Or;
            auto log_r = span(expr_range(left).offset, range_end(expr_range(right.value())));
            return ParseResult<ExprNode>::Ok(
                    ExprNode{LogicalExpression{lop, std::make_unique<ExprNode>(std::move(left)),
                                               std::make_unique<ExprNode>(std::move(right.value())), log_r}});
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
            if (bp <= min_bp) break;
            Token op_tok = cur;
            advance();
            auto result = led(op_tok, std::move(left.value()));
            if (!result.ok()) return result;
            left = std::move(result);
        }

        return left;
    }

    // ---- 函数辅助 ----

    // 解析参数列表 (a, b, c)，返回参数名向量
    ParseResult<std::vector<std::string>> parse_function_params() {
        auto lp = expect(TokenKind::LParen);
        if (!lp.ok()) return ParseResult<std::vector<std::string>>::Err(lp.error());
        std::vector<std::string> params;
        while (cur.kind != TokenKind::RParen && cur.kind != TokenKind::Eof) {
            if (cur.kind != TokenKind::Ident) {
                return ParseResult<std::vector<std::string>>::Err(
                        make_parse_error(source, cur, "expected parameter name"));
            }
            params.push_back(std::string(token_text(cur)));
            advance();
            if (cur.kind == TokenKind::Comma) {
                advance();
            } else {
                break;
            }
        }
        auto rp = expect(TokenKind::RParen);
        if (!rp.ok()) return ParseResult<std::vector<std::string>>::Err(rp.error());
        return ParseResult<std::vector<std::string>>::Ok(std::move(params));
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
        if (cur.kind != TokenKind::Ident) {
            return ParseResult<StmtNode>::Err(
                    make_parse_error(source, cur, "expected function name"));
        }
        std::string fn_name{token_text(cur)};
        advance();
        auto params_result = parse_function_params();
        if (!params_result.ok()) return ParseResult<StmtNode>::Err(params_result.error());
        // P2-E: non-async function body resets in_async_function_ context
        // TLA: also reset in_module_ so await inside a plain function is not allowed
        bool saved_in_async_fd = in_async_function_;
        bool saved_in_module_fd = in_module_;
        in_async_function_ = false;
        in_module_ = false;
        auto body_result = parse_function_body();
        in_async_function_ = saved_in_async_fd;
        in_module_ = saved_in_module_fd;
        if (!body_result.ok()) return ParseResult<StmtNode>::Err(body_result.error());
        uint32_t fn_end = range_end(body_result.value().second);
        auto body_ptr = std::make_shared<std::vector<StmtNode>>(std::move(body_result.value().first));
        return ParseResult<StmtNode>::Ok(StmtNode{FunctionDeclaration{
                std::move(fn_name), std::move(params_result.value()),
                std::move(body_ptr), span(kw.range.offset, fn_end)}});
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

    ParseResult<StmtNode> parse_for_stmt() {
        Token kw = cur;
        advance();  // 消费 for
        auto lp = expect(TokenKind::LParen);
        if (!lp.ok()) return ParseResult<StmtNode>::Err(lp.error());

        // init（var/let/const 声明会内部消费分号；表达式 init 需要在外层消费分号）
        std::optional<std::unique_ptr<StmtNode>> init;
        bool var_decl_init = false;
        if (cur.kind != TokenKind::Semicolon) {
            if (cur.kind == TokenKind::KwLet || cur.kind == TokenKind::KwConst || cur.kind == TokenKind::KwVar) {
                auto init_stmt = parse_var_decl();
                if (!init_stmt.ok()) return init_stmt;
                init = std::make_unique<StmtNode>(std::move(init_stmt.value()));
                var_decl_init = true;
            } else {
                auto expr = parse_expr(0);
                if (!expr.ok()) return ParseResult<StmtNode>::Err(expr.error());
                SourceRange er = expr_range(expr.value());
                init = std::make_unique<StmtNode>(StmtNode{ExpressionStatement{std::move(expr.value()), er}});
            }
        }
        // 若 init 是 var_decl，分号已被 parse_var_decl 消费；否则显式消费
        if (!var_decl_init) {
            auto semi1 = expect(TokenKind::Semicolon);
            if (!semi1.ok()) return ParseResult<StmtNode>::Err(semi1.error());
        }

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
                std::move(init), std::move(test), std::move(update),
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
                auto params_result = parse_function_params();
                if (!params_result.ok()) return ParseResult<StmtNode>::Err(params_result.error());
                auto body_result = parse_function_body();
                if (!body_result.ok()) return ParseResult<StmtNode>::Err(body_result.error());
                uint32_t fn_end = range_end(body_result.value().second);
                auto body_ptr = std::make_shared<std::vector<StmtNode>>(std::move(body_result.value().first));
                std::optional<std::string> saved_fn_name = fn_name;
                auto fe = FunctionExpression{std::move(fn_name), std::move(params_result.value()),
                                             std::move(body_ptr), span(fn_tok.range.offset, fn_end)};
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
                    auto params_result = parse_function_params();
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
                                                      std::move(body_ptr), span(async_tok.range.offset, fn_end)};
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
            auto params_result = parse_function_params();
            if (!params_result.ok()) return ParseResult<StmtNode>::Err(params_result.error());
            bool saved_in_async = in_async_function_;
            in_async_function_ = true;
            auto body_result = parse_function_body();
            in_async_function_ = saved_in_async;
            if (!body_result.ok()) return ParseResult<StmtNode>::Err(body_result.error());
            uint32_t fn_end = range_end(body_result.value().second);
            auto body_ptr = std::make_shared<std::vector<StmtNode>>(std::move(body_result.value().first));
            inner_result = ParseResult<StmtNode>::Ok(StmtNode{AsyncFunctionDeclaration{
                std::move(fn_name), std::move(params_result.value()),
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
            if (text == "import") return parse_import_decl();
            if (text == "export") return parse_export_decl();
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
                        auto params_result = parse_function_params();
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
                            std::move(fn_name), std::move(params_result.value()),
                            std::move(body_ptr), span(async_tok.range.offset, fn_end)}});
                    }
                    // async 后面不是 function：回退，把 async_tok 当普通标识符处理
                    // 将 async_tok 作为表达式 nud 处理
                    Token ident_tok2 = async_tok;
                    auto left2 = nud(ident_tok2);
                    if (!left2.ok()) return ParseResult<StmtNode>::Err(left2.error());
                    while (true) {
                        int bp = lbp(cur.kind);
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
