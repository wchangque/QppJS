#include "qppjs/frontend/parser.h"

#include "qppjs/frontend/lexer.h"

#include <cassert>
#include <cstdlib>
#include <limits>
#include <string>

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
    try {
        return std::stod(std::string(text));
    } catch (const std::out_of_range&) {
        return std::numeric_limits<double>::infinity();
    }
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
                      },
                      s.v);
}

// ---- Parser 状态 ----

struct Parser {
    std::string_view source;
    LexerState lex;
    Token cur;    // 当前已消费 token（lookahead）
    bool got_lf;  // cur 前是否有换行（ASI 用）
    int function_depth;

    explicit Parser(std::string_view src)
        : source(src), lex(lexer_init(src)), cur{TokenKind::Eof, {0, 0}}, got_lf(false), function_depth(0) {
        advance();  // 载入第一个 token
    }

    // 推进一个 token，记录换行状态
    void advance() {
        cur = next_token(lex);
        got_lf = lex.got_lf;
    }

    // 返回当前 token 的原始文本
    std::string_view token_text(const Token& tok) const { return source.substr(tok.range.offset, tok.range.length); }

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
                std::string name{token_text(tok)};
                return ParseResult<ExprNode>::Ok(ExprNode{Identifier{std::move(name), tok.range}});
            }
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

        // 赋值：右结合，检查左侧是 Identifier
        if (kind == TokenKind::Eq || kind == TokenKind::PlusEq || kind == TokenKind::MinusEq ||
            kind == TokenKind::StarEq || kind == TokenKind::SlashEq || kind == TokenKind::PercentEq) {
            // Early Error：左侧必须是 Identifier
            if (!std::holds_alternative<Identifier>(left.v)) {
                return ParseResult<ExprNode>::Err(
                        make_parse_error(source, op_tok, "invalid left-hand side in assignment"));
            }
            std::string target = std::get<Identifier>(left.v).name;
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
            auto asgn_r = span(std::get<Identifier>(left.v).range.offset, range_end(expr_range(right.value())));
            return ParseResult<ExprNode>::Ok(ExprNode{AssignmentExpression{
                    aop, std::move(target), std::make_unique<ExprNode>(std::move(right.value())), asgn_r}});
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
        std::vector<StmtNode> body;
        while (cur.kind != TokenKind::RBrace && cur.kind != TokenKind::Eof) {
            auto stmt = parse_stmt();
            if (!stmt.ok()) return stmt;
            body.push_back(std::move(stmt.value()));
        }
        auto rb = expect(TokenKind::RBrace);
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
        auto consequent = parse_stmt();
        if (!consequent.ok()) return consequent;
        std::unique_ptr<StmtNode> alt_ptr = nullptr;
        if (cur.kind == TokenKind::KwElse) {
            advance();
            auto alt = parse_stmt();
            if (!alt.ok()) return alt;
            alt_ptr = std::make_unique<StmtNode>(std::move(alt.value()));
        }
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
        auto body = parse_stmt();
        if (!body.ok()) return body;
        uint32_t while_end = range_end(stmt_range(body.value()));
        return ParseResult<StmtNode>::Ok(
                StmtNode{WhileStatement{std::move(test.value()), std::make_unique<StmtNode>(std::move(body.value())),
                                        span(kw.range.offset, while_end)}});
    }

    ParseResult<StmtNode> parse_return_stmt() {
        Token kw = cur;
        advance();
        if (function_depth == 0) {
            return ParseResult<StmtNode>::Err(make_parse_error(source, kw, "return statement outside of function"));
        }
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

    ParseResult<StmtNode> parse_stmt() {
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
        uint32_t len = static_cast<uint32_t>(source.size());
        return ParseResult<Program>::Ok(Program{std::move(body), {0, len}});
    }
};

ParseResult<Program> parse_program(std::string_view source) {
    Parser p(source);
    return p.parse();
}

}  // namespace qppjs
