# 06 字节码与指令集

## 概述

QuickJS 编译器将 JS 源码直接编译为栈式字节码（无 AST 中间表示）。指令集用 X-Macro 统一描述，支持变长指令，常量通过常量池引用。

---

## 指令集定义（X-Macro）

### 格式

```c
// quickjs-opcode.h
// DEF(名称, 字节大小, 弹出数, 压入数, 格式)
DEF(invalid,      1, 0, 0, none)
DEF(push_i32,     5, 0, 1, i32)     // 1字节 opcode + 4字节 i32
DEF(push_const,   5, 0, 1, const)   // 1字节 opcode + 4字节 常量池索引
DEF(push_atom_value, 5, 0, 1, atom) // 压入 atom 对应的字符串值
DEF(undefined,    1, 0, 1, none)    // 压入 undefined
DEF(null,         1, 0, 1, none)
DEF(push_false,   1, 0, 1, none)
DEF(push_true,    1, 0, 1, none)
DEF(dup,          1, 1, 2, none)    // 复制栈顶
DEF(drop,         1, 1, 0, none)    // 弹出栈顶
DEF(nop,          1, 0, 0, none)
// 算术
DEF(add,          1, 2, 1, none)
DEF(sub,          1, 2, 1, none)
DEF(mul,          1, 2, 1, none)
DEF(div,          1, 2, 1, none)
// 比较
DEF(lt,           1, 2, 1, none)
DEF(lte,          1, 2, 1, none)
DEF(eq,           1, 2, 1, none)
DEF(strict_eq,    1, 2, 1, none)
// 控制流
DEF(goto,         5, 0, 0, label)   // 无条件跳转，4字节偏移
DEF(if_true,      5, 1, 0, label)   // 条件跳转
DEF(if_false,     5, 1, 0, label)
// 函数调用
DEF(call,         3, -1, 1, argc)   // 2字节 argc
DEF(call_method,  3, -1, 1, argc)
DEF(return,       1, 1, 0, none)
DEF(return_undef, 1, 0, 0, none)
// 变量访问
DEF(get_loc,      3, 0, 1, loc)     // 2字节局部变量索引
DEF(put_loc,      3, 1, 0, loc)
DEF(get_arg,      3, 0, 1, arg)     // 2字节参数索引
DEF(put_arg,      3, 1, 0, arg)
DEF(get_var_ref,  3, 0, 1, var_ref) // 2字节 upvalue 索引
DEF(put_var_ref,  3, 1, 0, var_ref)
// 属性访问
DEF(get_field,    5, 1, 1, atom)    // obj.prop → value
DEF(put_field,    5, 2, 0, atom)    // obj.prop = value
DEF(get_array_el, 1, 2, 1, none)    // obj[key] → value
DEF(put_array_el, 1, 3, 0, none)
// 对象/数组构造
DEF(object,       1, 0, 1, none)    // 创建空对象 {}
DEF(array_from,   3, -1, 1, argc)   // 创建数组 [a, b, c]
// 闭包
DEF(make_closure, 5, 0, 1, const)   // 从字节码对象创建闭包
DEF(close_var_refs, 1, 0, 0, none)  // 关闭所有 upvalue
// 异常
DEF(throw,        1, 1, 0, none)
DEF(catch,        5, 0, 1, label)   // 设置 catch 处理点
// yield/await
DEF(yield,        1, 1, 1, none)
DEF(await,        1, 1, 1, none)
```

### X-Macro 展开

同一份 `quickjs-opcode.h` 通过不同的 `#define DEF(...)` 展开为：
- `OPCodeEnum`（枚举值）
- 指令大小表 `opcode_info[].size`
- 反汇编器的名称表
- 字节码验证器的栈效果表

---

## JSFunctionBytecode 结构

```c
typedef struct JSFunctionBytecode {
    JSGCObjectHeader header;     // 本身是 GC 对象，可被 closure 引用

    uint8_t js_mode;             // JS_MODE_STRICT 等标志
    uint8_t has_prototype : 1;   // 是否需要 .prototype 属性
    uint8_t has_simple_parameter_list : 1;
    uint8_t is_derived_class_constructor : 1;
    uint8_t func_kind : 2;       // JSFunctionKindEnum：NORMAL/GENERATOR/ASYNC

    uint8_t *byte_code_buf;      // 字节码流（变长指令序列）
    int byte_code_len;

    JSAtom func_name;            // 函数名（用于 .name 属性和 Error.stack）

    JSVarDef *vardefs;           // 参数 + 局部变量定义（arg_count + var_count 个）
    JSClosureVar *closure_var;   // 闭包捕获变量列表
    uint16_t arg_count;          // 形参数量
    uint16_t var_count;          // 局部变量数量
    uint16_t defined_arg_count;  // 用于 .length 属性（排除默认参数后的数量）
    uint16_t stack_size;         // 最大操作数栈深度（编译期计算）

    JSContext *realm;            // 函数所属的 context（用于 new.target 等）

    JSValue *cpool;              // 常量池：字符串/数字/嵌套函数/正则
    int cpool_count;
    int closure_var_count;

    struct {
        JSAtom   filename;
        int      source_len;
        int      pc2line_len;
        uint8_t *pc2line_buf;    // PC→行号映射（差分压缩编码）
        char    *source;         // 原始源码（可选，Error.stack 用）
    } debug;
} JSFunctionBytecode;
```

---

## 常量池

```c
// 编译期填充，运行时只读
// 类型：字符串字面量、数字字面量、嵌套函数字节码、正则字节码
JSValue cpool[cpool_count];

// 字节码引用方式
// OP_push_const  [4字节索引] → 压入 cpool[index]
// OP_make_closure [4字节索引] → 从 cpool[index]（JSFunctionBytecode）创建闭包
```

---

## pc2line 压缩编码

```c
// 差分编码：存储 (delta_pc, delta_line) 对
// PC2LINE_BASE = -1, PC2LINE_RANGE = 5, PC2LINE_OP_FIRST = 1
// 编码：op = (delta_line - PC2LINE_BASE) * PC2LINE_DIFF_PC_MAX + delta_pc + PC2LINE_OP_FIRST
// 超范围时用多字节编码

// 解码（用于 Error.stack / 异常行号）
int find_line_num(JSContext *ctx, JSFunctionBytecode *b, uint32_t pc_value) {
    // 从头遍历 pc2line_buf，累加 delta 直到超过目标 pc
}
```

---

## 变量定义

```c
typedef struct JSVarDef {
    JSAtom var_name;
    int    scope_level;    // 词法作用域层级（0 = 函数顶层）
    int    scope_next;     // 同作用域下一个变量的索引（链表）
    uint8_t is_const    : 1;
    uint8_t is_lexical  : 1; // let/const（非 var）
    uint8_t is_captured : 1; // 是否被闭包捕获（需要 JSVarRef）
    uint8_t var_kind    : 4; // JSVarKindEnum
} JSVarDef;
```

---

## 设计要点

1. **X-Macro 统一描述**：`DEF(id, size, n_pop, n_push, fmt)` 一处定义，所有派生数据（枚举、大小表、反汇编）自动生成，零冗余。

2. **变长指令**：size 从 1 到 9 字节，常用指令（`undefined`/`drop`/`dup`）保持 1 字节，减少代码体积。

3. **常量池统一管理**：所有编译期常量存入 `cpool`，指令只携带 4 字节索引，运行时无需重复解析字面量。

4. **字节码对象参与 GC**：`JSFunctionBytecode` 有 `JSGCObjectHeader`，闭包持有字节码引用时引用计数自动管理，无需特殊处理。

5. **pc2line 差分压缩**：行号信息仅在调试/异常时展开，不影响正常执行路径性能。

---

## 实现建议

| 决策点 | 建议 |
|--------|------|
| 指令集设计 | 用 X-Macro 模式，强烈推荐；一处定义，多处生成 |
| 栈深度 | 编译期静态计算 `stack_size`，运行时可做栈溢出快速检测 |
| 常量池 | 先用动态数组，编译完成后 shrink_to_fit |
| pc2line | 初期用简单数组存行号，稳定后再做差分压缩 |
| 字节码验证 | 可选：加载时验证 n_pop/n_push 的栈平衡，防止恶意字节码 |
