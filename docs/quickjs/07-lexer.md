# 07 词法分析器（Lexer）

## 概述

QuickJS 的词法分析器是手写的单遍扫描器，内嵌在解析器状态中，支持 UTF-8 源码、自动分号插入（ASI）和单 token 前瞻。

---

## 核心数据结构

### JSParseState（解析器 + 词法器共用状态）

```c
typedef struct JSParseState {
    JSContext *ctx;
    int last_line_num;           // 上一个 token 的行号
    int line_num;                // 当前 token 的行号
    const char *filename;

    JSToken token;               // 当前 token（单 token 前瞻）
    BOOL got_lf;                 // 当前 token 前是否有换行（ASI 关键标志）

    const uint8_t *buf_ptr;      // 当前扫描位置
    const uint8_t *buf_end;      // 源码末尾（含终止符 '\0'）
    const uint8_t *buf_start;    // 源码起始（用于错误定位）

    JSFunctionDef *cur_func;     // 当前正在编译的函数
    BOOL is_module;              // 是否为模块模式
    BOOL allow_html_close;       // 是否允许 --> 注释（非模块）
} JSParseState;
```

### JSToken（token 语义值）

```c
typedef struct JSToken {
    int val;                     // token 类型（枚举值或 ASCII 字符）
    int line_num;                // token 所在行号
    const uint8_t *ptr;          // token 在源码中的起始位置（用于错误报告）
    union {
        struct {
            JSValue str;         // 字符串值（已解码转义）
            int sep;             // 分隔符：'`' 或 '{' 或 '\0'（模板字面量）
        } str;
        struct {
            JSValue val;         // 数字值（JSValue 包装）
        } num;
        struct {
            JSAtom atom;         // 标识符对应的 atom
            BOOL has_escape;     // 是否含 \uXXXX 转义（影响关键字识别）
            BOOL is_reserved;    // 是否为保留字
        } ident;
        struct {
            JSValue body;        // 正则 pattern 字符串
            JSValue flags;       // 正则 flags 字符串
        } regexp;
    } u;
} JSToken;
```

### Token 类型枚举

```c
// ASCII 字符（< 256）直接用字符值
// 复合 token 从 256 开始
enum {
    TOK_NUMBER = 256,    // 数字字面量
    TOK_STRING,          // 字符串字面量
    TOK_TEMPLATE,        // 模板字面量片段
    TOK_IDENT,           // 标识符
    TOK_REGEXP,          // 正则字面量
    TOK_EOF,             // 文件结束

    // 复合运算符
    TOK_SHL,             // <<
    TOK_SAR,             // >>
    TOK_SHR,             // >>>
    TOK_LT,              // <
    TOK_LTE,             // <=
    TOK_EQ,              // ==
    TOK_NEQ,             // !=
    TOK_STRICT_EQ,       // ===
    TOK_STRICT_NEQ,      // !==
    TOK_ARROW,           // =>
    TOK_ELLIPSIS,        // ...
    TOK_DOUBLE_QUESTION_MARK, // ??
    TOK_OPTIONAL_CHAIN,  // ?.

    // 关键字（与 atom 表顺序对应，O(1) 映射）
    TOK_FIRST_KEYWORD,
    TOK_null = TOK_FIRST_KEYWORD,
    TOK_false, TOK_true,
    TOK_if, TOK_else, TOK_return, TOK_var, TOK_this,
    TOK_delete, TOK_void, TOK_typeof,
    TOK_new, TOK_in, TOK_instanceof,
    TOK_do, TOK_while, TOK_for, TOK_break, TOK_continue,
    TOK_switch, TOK_case, TOK_default, TOK_throw, TOK_try,
    TOK_catch, TOK_finally, TOK_function, TOK_debugger,
    TOK_with, TOK_class, TOK_const, TOK_enum,
    TOK_export, TOK_import, TOK_super, TOK_extends,
    // 严格模式保留字
    TOK_implements, TOK_interface, TOK_let, TOK_package,
    TOK_private, TOK_protected, TOK_public, TOK_static,
    TOK_yield, TOK_await,
    TOK_LAST_KEYWORD,
};
```

---

## 词法分析核心流程

### next_token 主函数

```c
static __exception int next_token(JSParseState *s) {
    const uint8_t *p = s->buf_ptr;
    s->got_lf = FALSE;

redo:
    switch (*p) {
    case '\n':
        s->got_lf = TRUE;          // 记录换行，用于 ASI
        p++;
        goto redo;
    case ' ': case '\t': case '\r':
        p++;
        goto redo;

    case '/':
        if (p[1] == '/') {         // 单行注释
            while (*p && *p != '\n') p++;
            goto redo;
        } else if (p[1] == '*') {  // 多行注释
            // 扫描到 */，期间记录换行
            goto redo;
        } else if (is_regexp_allowed(s->token.val)) {
            return js_parse_regexp(s); // 正则字面量
        }
        // 否则是除法运算符
        break;

    case '`':
        return js_parse_template_part(s, p); // 模板字面量

    case '\'': case '"':
        return js_parse_string(s, *p, ...);  // 字符串字面量

    case '0' ... '9':
        return js_parse_number(s, p);        // 数字字面量

    default:
        if (*p >= 0x80) {
            // UTF-8 多字节字符：解码后判断是否为标识符起始字符
            c = unicode_from_utf8(p, 6, &p);
        }
        if (is_ident_first(c)) {
            return parse_ident(s, &p, c);   // 标识符/关键字
        }
    }
    // 处理单字符和双字符运算符...
}
```

### 关键字识别

```c
// 利用 atom 表与 token 枚举的顺序对应关系，O(1) 映射
static int parse_ident(JSParseState *s, const uint8_t **pp, int c) {
    JSAtom atom = parse_ident_token(s, pp, c);
    // atom 在预定义关键字范围内？
    if (atom >= JS_ATOM_null && atom <= JS_ATOM_yield) {
        // 直接映射：token = TOK_FIRST_KEYWORD + (atom - JS_ATOM_null)
        s->token.val = TOK_FIRST_KEYWORD + (atom - JS_ATOM_null);
    } else {
        s->token.val = TOK_IDENT;
        s->token.u.ident.atom = atom;
    }
}
```

### ASI（自动分号插入）

```c
static int js_parse_expect_semi(JSParseState *s) {
    if (s->token.val == ';') {
        return next_token(s);        // 显式分号
    }
    if (s->token.val == TOK_EOF     // 文件结束
     || s->token.val == '}'         // 右花括号
     || s->got_lf) {                // 前有换行
        return 0;                   // 静默插入分号
    }
    return js_parse_error(s, "';' expected");
}
```

### 正则 vs 除法歧义

```c
// 通过上一个 token 判断当前 '/' 是正则还是除法
static BOOL is_regexp_allowed(int tok) {
    switch (tok) {
    case TOK_NUMBER: case TOK_STRING: case TOK_REGEXP:
    case TOK_IDENT:  case ')':        case ']':
    case TOK_TEMPLATE:
        return FALSE;  // 这些之后的 / 是除法
    default:
        return TRUE;   // 其他情况是正则
    }
}
```

---

## 设计要点

1. **单 token 前瞻**：`JSParseState.token` 只保存当前 token，`simple_next_token()` 做有限前瞻（仅识别 `:`、`=>`、`let`、`function` 等歧义点），避免完整词法分析的开销。

2. **UTF-8 原生处理**：快路径保持纯 ASCII 速度（`switch (*p)`），遇到 `>= 0x80` 的字节才调用 `unicode_from_utf8()` 解码。

3. **`got_lf` 是一等公民**：与 token 值同等重要，ASI 的正确性依赖它，必须在每次 `next_token` 时准确设置。

4. **关键字与 atom 顺序对应**：关键字 token 枚举值与 atom 表顺序严格对应，可通过加减偏移 O(1) 互相映射，无需哈希或 trie。

5. **模板字面量特殊处理**：`js_parse_template_part()` 在词法层处理 `${` 插值边界，`sep` 字段记录结束字符，解析器据此决定是否继续扫描下一段。

---

## 实现建议

| 决策点 | 建议 |
|--------|------|
| 字符扫描 | 主循环用 `switch (*p)` + goto redo 跳过空白，比 while 循环更快 |
| 关键字识别 | 先 intern 为 atom，再用范围比较映射到 token，O(1) 且无 strcmp |
| `got_lf` | 多行注释中的换行也要设置 `got_lf`，这是常见遗漏 |
| 正则歧义 | 维护一个"上一个 token"状态，按规范列表判断是否允许正则 |
| 错误位置 | `token.ptr` 指向源码中的位置，结合 `line_num` 提供精确错误信息 |
