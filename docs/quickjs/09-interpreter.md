# 09 执行引擎（Interpreter）

## 概述

QuickJS 的解释器是基于**操作数栈**的字节码解释器，主循环位于 `JS_CallInternal()`，通过 `switch(opcode)` 或 computed goto 分发指令。

---

## 核心数据结构

### JSStackFrame（调用栈帧）

```c
typedef struct JSStackFrame {
    struct JSStackFrame *prev_frame; // 调用链（NULL = 第一帧）
    JSValue cur_func;     // 当前函数对象（detached 时为 JS_UNDEFINED）
    JSValue *arg_buf;     // 参数数组基址
    JSValue *var_buf;     // 局部变量基址（= arg_buf + arg_count）
    struct list_head var_ref_list; // 本帧持有的开放 upvalue 链表
    const uint8_t *cur_pc;  // 字节码 PC（仅字节码函数使用）
    int arg_count;
    int js_mode;          // JS_MODE_STRICT 等标志
    JSValue *cur_sp;      // 操作数栈顶（Generator 挂起时保存；正常运行时为 NULL）
} JSStackFrame;
```

### 栈内存布局

普通调用时，栈帧在 C 栈上分配：

```
alloca(arg_count + var_count + stack_size) 个 JSValue

[  arg_buf  ][  var_buf  ][  stack_buf  ]
 ^             ^             ^         ^
 arg_buf       var_buf       sp_start  sp（栈顶，向上增长）

arg_buf  = alloca 起始
var_buf  = arg_buf + arg_count
sp_start = var_buf + var_count
sp       = sp_start（初始），执行中上下移动
```

---

## 主循环结构

```c
static JSValue JS_CallInternal(JSContext *caller_ctx, JSValueConst func_obj,
                                JSValueConst this_obj, JSValueConst new_target,
                                int argc, JSValue *argv, int flags) {
    // 1. 建立栈帧
    JSStackFrame sf_s, *sf = &sf_s;
    sf->prev_frame = rt->current_stack_frame;
    rt->current_stack_frame = sf;

    // 2. 分配操作数栈（alloca）
    JSValue stack_buf[b->stack_size];
    JSValue *sp = stack_buf;

    // 3. 设置 PC
    const uint8_t *pc = b->byte_code_buf;

    // 4. 主循环
#ifdef DIRECT_DISPATCH
    static const void *dispatch_table[] = { &&case_OP_nop, ... };
    DISPATCH;  // goto *dispatch_table[*pc++]
#else
    for (;;) {
        int opcode = *pc++;
        switch (opcode) {
#endif

        case OP_push_i32:
            *sp++ = JS_NewInt32(ctx, get_u32(pc)); pc += 4;
            DISPATCH;

        case OP_add: {
            JSValue op1 = sp[-2], op2 = sp[-1];
            if (likely(JS_VALUE_IS_BOTH_INT(op1, op2))) {
                // 整数快路径
                int64_t r = (int64_t)JS_VALUE_GET_INT(op1) + JS_VALUE_GET_INT(op2);
                if (unlikely((int32_t)r != r)) goto add_slow; // 溢出
                sp[-2] = JS_NewInt32(ctx, r);
                sp--;
            } else {
            add_slow:
                if (js_add_slow(ctx, sp) < 0) goto exception;
                sp--;
            }
            DISPATCH;
        }

        case OP_get_loc: {
            int idx = get_u16(pc); pc += 2;
            JSValue val = var_buf[idx];
            if (unlikely(JS_VALUE_GET_TAG(val) == JS_TAG_UNINITIALIZED))
                goto uninitialized_error;
            *sp++ = JS_DupValue(ctx, val);
            DISPATCH;
        }

        case OP_call: {
            int argc = get_u16(pc); pc += 2;
            // sp[-argc-1] = 函数, sp[-argc..sp-1] = 参数
            JSValue ret = JS_CallInternal(ctx, sp[-argc-1], ...);
            // 清理栈，压入返回值
            DISPATCH;
        }

        case OP_return:
            ret_val = *--sp;
            goto done;

        exception:
            // 查找 try_buf 中最近的 catch 处理点
            // 找到 → 跳转到 catch 块
            // 找不到 → 展开栈帧，向上传播
            goto done;

        done:
            // 关闭本帧的所有 upvalue
            close_var_refs(rt, sf);
            // 恢复上一帧
            rt->current_stack_frame = sf->prev_frame;
            return ret_val;
    }
}
```

---

## 指令分发优化

### 默认：switch-case

```c
switch (*pc++) {
case OP_xxx: ...; break;
}
```

### 优化：Computed Goto（GNU 扩展）

```c
#define DISPATCH goto *dispatch_table[*pc++]

static const void *dispatch_table[] = {
    &&case_OP_nop, &&case_OP_push_i32, ...
};

case_OP_push_i32:
    *sp++ = JS_NewInt32(ctx, get_u32(pc)); pc += 4;
    DISPATCH;
```

消除 switch 的跳转表开销，热路径提速约 10-20%。

---

## 异常处理

```c
// 编译器为每个 try 块生成一个 try_buf 条目
typedef struct {
    uint32_t start_pc;    // try 块开始 PC
    uint32_t end_pc;      // try 块结束 PC
    uint32_t catch_pc;    // catch 块 PC（-1 = 无 catch）
    uint32_t finally_pc;  // finally 块 PC（-1 = 无 finally）
    int stack_level;      // try 块入口时的栈深度（异常时恢复栈）
} JSTryBlock;

// 异常发生时
exception:
    // 1. 查找 try_buf 中 start_pc <= 当前 PC < end_pc 的最近条目
    // 2. 恢复栈深度到 stack_level
    // 3. 压入异常值，跳转到 catch_pc 或 finally_pc
    // 4. 若无匹配 → 关闭 upvalue，向上传播（return JS_EXCEPTION）
```

---

## 属性访问

```c
case OP_get_field: {
    JSAtom atom = get_u32(pc); pc += 4;
    JSValue obj = sp[-1];
    JSValue val = JS_GetProperty(ctx, obj, atom);
    if (unlikely(JS_IsException(val))) goto exception;
    JS_FreeValue(ctx, obj);
    sp[-1] = val;
    DISPATCH;
}

// JS_GetProperty 内部流程：
// 1. 若 obj 是 JS_TAG_OBJECT：
//    a. 在 shape 中查找 atom → 获取槽位索引
//    b. 按属性类型（NORMAL/GETSET/VARREF）读取值
//    c. 未找到 → 沿原型链向上查找
// 2. 若 obj 是字符串/数字/布尔：
//    → 包装为临时对象，从对应原型查找
```

---

## 函数调用约定

```c
// 调用栈布局（调用前）：
// sp[-argc-1] = 函数对象
// sp[-argc]   = 参数 0
// ...
// sp[-1]      = 参数 argc-1

// JS_CallInternal 处理：
// 1. 检查 func_obj 类型（bytecode / C function / bound function / proxy）
// 2. 字节码函数：建立新 JSStackFrame，递归调用 JS_CallInternal
// 3. C 函数：直接调用 cfunc.c_function(ctx, this_val, argc, argv)
// 4. 返回值通过函数返回值传递
```

---

## 设计要点

1. **栈帧在 C 栈上**：普通调用的 `JSStackFrame` 是 C 栈局部变量，操作数区域用 `alloca` 分配，零堆分配开销。

2. **整数快路径**：`add`/`sub`/`mul` 等先尝试整数运算，溢出再退化为 float64，覆盖绝大多数算术场景。

3. **中断检测**：每条指令递减 `ctx->interrupt_counter`，归零时调用 `interrupt_handler`，实现软中断。

4. **Generator 恢复**：`flags & JS_CALL_FLAG_GENERATOR` 时直接接管 `JSAsyncFunctionState.frame`，从保存的 `cur_pc`/`cur_sp` 续跑。

5. **异常传播哨兵**：函数返回 `JS_EXCEPTION` 时调用方检查 `JS_IsException(ret)`，不使用 errno 或 longjmp。

---

## 实现建议

| 决策点 | 建议 |
|--------|------|
| 初期 | 先实现 switch-case 分发，computed goto 作为后期优化 |
| 栈大小 | 编译期计算 `stack_size`，运行时做溢出检测（`sp < stack_buf` 或 `sp > stack_top`） |
| 整数快路径 | `add`/`sub` 先检查两个操作数的 tag 是否都是 `JS_TAG_INT`，溢出用 `__builtin_add_overflow` |
| 属性缓存 | 可选：inline cache（IC）缓存最近一次 shape + 槽位，命中时 O(1) 访问 |
| 尾调用 | QuickJS 未实现 TCO，初期可忽略 |
