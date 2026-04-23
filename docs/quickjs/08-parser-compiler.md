# 08 解析器与编译器

## 概述

QuickJS 采用**递归下降解析器**，最关键的设计决策是**无 AST**：解析过程中直接调用 `emit_op()` 产出字节码，省去 AST 构建和遍历的内存与时间开销。

---

## 核心数据结构

### JSFunctionDef（编译期函数定义）

```c
typedef struct JSFunctionDef {
    JSContext *ctx;
    struct JSFunctionDef *parent;  // 父函数（嵌套函数编译时压栈）
    int parent_cpool_idx;          // 在父函数常量池中的索引

    // === 字节码缓冲区 ===
    DynBuf byte_code;              // 动态字节码缓冲
    int last_opcode_pos;           // 最后一条指令的偏移（用于死代码检测）
    int last_opcode_line_num;

    // === 标签系统 ===
    LabelSlot *label_slots;        // 标签槽数组
    int label_count;
    int label_size;

    // === 变量定义 ===
    JSVarDef *vars;                // 局部变量定义
    int var_count, var_size;
    JSVarDef *args;                // 形参定义
    int arg_count, arg_size;
    JSClosureVar *closure_var;     // 闭包捕获变量
    int closure_var_count, closure_var_size;

    // === 作用域 ===
    JSVarScope *scopes;            // 作用域链
    int scope_count, scope_size;
    int scope_level;               // 当前作用域层级

    // === 常量池 ===
    JSValue *cpool;
    int cpool_count, cpool_size;

    // === 控制流栈 ===
    BlockEnv *top_break;           // break/continue 目标栈顶

    // === 函数属性 ===
    JSFunctionKindEnum func_kind;  // NORMAL/GENERATOR/ASYNC
    JSParseFunctionEnum func_type; // STATEMENT/ARROW/GETTER/SETTER/...
    BOOL is_strict_mode;
    JSAtom func_name;
} JSFunctionDef;
```

### LabelSlot（标签回填）

```c
typedef struct LabelSlot {
    int ref_count;          // 引用计数（为 0 可消除死跳转）
    int pos;                // 阶段1：OP_label 在 byte_code 中的偏移
    int pos2;               // 阶段2：peephole 优化后的偏移
    int addr;               // 阶段3：最终字节码地址
    RelocEntry *first_reloc;// 待回填的跳转指令链表
} LabelSlot;

typedef struct RelocEntry {
    struct RelocEntry *next;
    uint32_t addr;          // 需要回填的字节码位置
    int size;               // 地址字段宽度：1/2/4 字节
} RelocEntry;
```

### BlockEnv（循环/switch 控制流栈帧）

```c
typedef struct BlockEnv {
    struct BlockEnv *prev;
    JSAtom label_name;      // 带标签的 break/continue（无标签则为 JS_ATOM_NULL）
    int label_break;        // break 目标标签 ID（-1 = 无）
    int label_cont;         // continue 目标标签 ID（-1 = 无）
    int drop_count;         // 跳出时需要 pop 的栈元素数（try/finally 用）
    int scope_level;        // 对应的词法作用域层级
    BOOL has_iterator;      // for...of 迭代器需要特殊清理
} BlockEnv;
```

---

## 解析器层次（优先级从低到高）

```
js_parse_program()
└── js_parse_source_element()     // 语句 or 函数声明
    └── js_parse_statement()      // 各种语句
        └── js_parse_expr()       // 逗号表达式
            └── js_parse_assign_expr()     // 赋值、箭头函数
                └── js_parse_cond_expr()   // 三元 ?:
                    └── js_parse_binary_exprs()  // 二元运算（优先级爬升）
                        └── js_parse_unary()     // 一元前缀
                            └── js_parse_postfix_expr()  // 后缀 ++ --
                                └── js_parse_call_expr() // 函数调用 ()
                                    └── js_parse_left_hand_side_expr() // 成员访问
                                        └── js_parse_primary_expr()    // 字面量/标识符
```

### 函数调用链即优先级表

每层函数只处理本层运算符，调用下一层获取操作数，天然实现运算符优先级。

---

## 字节码发射

### 基础 emit 函数

```c
static void emit_u8(JSParseState *s, uint8_t val) {
    dbuf_putc(&s->cur_func->byte_code, val);
}
static void emit_op(JSParseState *s, uint8_t val) {
    JSFunctionDef *fd = s->cur_func;
    fd->last_opcode_pos = fd->byte_code.size;  // 记录最后指令位置
    dbuf_putc(&fd->byte_code, val);
}
static void emit_u32(JSParseState *s, uint32_t val) {
    dbuf_put_u32(&s->cur_func->byte_code, val);
}
static void emit_atom(JSParseState *s, JSAtom name) {
    emit_u32(s, name);  // atom 作为 4 字节立即数
}
```

### 标签与跳转

```c
// 分配新标签（返回标签 ID）
static int new_label(JSParseState *s) {
    JSFunctionDef *fd = s->cur_func;
    int idx = fd->label_count++;
    fd->label_slots[idx] = (LabelSlot){ .ref_count = 0, .pos = -1, .addr = -1 };
    return idx;
}

// 发射跳转指令（地址暂填 0，加入回填链表）
static int emit_goto(JSParseState *s, int opcode, int label) {
    emit_op(s, opcode);
    int pos = s->cur_func->byte_code.size;
    emit_u32(s, 0);  // 占位符
    // 将 pos 加入 label_slots[label].first_reloc 链表
    add_reloc(s, label, pos, 4);
    return label;
}

// 定义标签位置（回填所有引用）
static void emit_label(JSParseState *s, int label) {
    LabelSlot *ls = &s->cur_func->label_slots[label];
    int addr = s->cur_func->byte_code.size;
    ls->addr = addr;
    // 遍历 first_reloc 链表，将所有占位符替换为真实地址
    RelocEntry *re = ls->first_reloc;
    while (re) {
        put_u32(s->cur_func->byte_code.buf + re->addr, addr);
        re = re->next;
    }
}
```

### if 语句编译示例

```c
// if (cond) then_body else else_body
// 编译为：
//   [cond 表达式]
//   OP_if_false  L_else
//   [then_body]
//   OP_goto      L_end
// L_else:
//   [else_body]
// L_end:

int label_else = new_label(s);
int label_end  = new_label(s);

js_parse_expr(s);              // 编译条件
emit_goto(s, OP_if_false, label_else);

js_parse_statement(s);         // 编译 then 分支
emit_goto(s, OP_goto, label_end);

emit_label(s, label_else);
if (token == TOK_else) {
    js_parse_statement(s);     // 编译 else 分支
}
emit_label(s, label_end);
```

---

## 作用域与变量解析

```c
// 变量查找顺序：
// 1. 当前函数的局部变量（vars[]）
// 2. 当前函数的参数（args[]）
// 3. 父函数的局部变量 → 创建 closure_var 条目（upvalue）
// 4. 全局对象属性

// 作用域层级（scope_level）
// scope_level = 0：函数顶层（var 声明的作用域）
// scope_level > 0：块级作用域（let/const 声明的作用域）

// 变量声明
static int define_var(JSParseState *s, JSFunctionDef *fd, JSAtom name,
                      JSVarDefEnum var_def_type) {
    // var：添加到 vars[]，scope_level = 0，提升到函数顶层
    // let/const：添加到 vars[]，scope_level = 当前层级
    // 检查重复声明
}
```

---

## 函数嵌套编译

```c
// 遇到 function/class/箭头函数时：
// 1. 创建新的 JSFunctionDef，parent = cur_func
// 2. s->cur_func = 新函数
// 3. 递归编译子函数体
// 4. 调用 js_create_function() 生成 JSFunctionBytecode
// 5. 将字节码对象加入父函数 cpool[]
// 6. s->cur_func = parent
// 7. 发射 OP_make_closure [cpool_index]
```

---

## 设计要点

1. **无 AST 直接产出字节码**：省去 AST 构建和遍历，内存占用低，编译速度快，是 QuickJS 轻量化的核心决策。

2. **两阶段标签解析**：`emit_goto` 先写占位符，`emit_label` 回填真实地址，`RelocEntry` 链表管理所有待回填位置。

3. **`last_opcode_pos` 死代码检测**：始终指向最后一条指令，`js_is_live_code()` 通过检查它是否为无条件跳转/return/throw 来判断死代码。

4. **`BlockEnv` 栈管理 break/continue**：进入/退出循环和 switch 时维护栈，`drop_count` 确保跳出时栈平衡。

5. **`parse_flags` 位掩码传递上下文**：`for...in` 头部禁止 `in` 运算符、解构模式等上下文通过 flags 参数传递，避免全局状态污染。

---

## 实现建议

| 决策点 | 建议 |
|--------|------|
| 无 AST | 直接产出字节码，适合嵌入式；若需优化 pass，可先实现 AST 版本 |
| 标签回填 | 用整数 ID 而非指针引用标签，每次 `emit_goto` 时同步更新 `ref_count` |
| 作用域 | `scope_level` 从 0 开始，每个 `{` 递增，`}` 递减，let/const 按层级检查 |
| 优先级爬升 | 二元运算符可改用 Pratt parsing，比分层递归更易维护 |
| 常量折叠 | 编译期可做简单常量折叠（`1 + 2` → `3`），在 emit 前检查两个操作数是否都是常量 |
