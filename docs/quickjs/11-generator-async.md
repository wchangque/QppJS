# 11 Generator 与 Async

## 概述

QuickJS 用 **JSAsyncFunctionState** 统一实现 Generator 和 Async 函数的协程语义：将整个栈帧保存到堆上，通过 `cur_pc`/`cur_sp` 实现挂起与恢复。

---

## 核心数据结构

### JSAsyncFunctionState（协程状态）

```c
typedef struct JSAsyncFunctionState {
    JSGCObjectHeader header;    // 参与 GC
    JSValue this_val;           // 'this' 参数
    int argc;                   // 函数参数数量
    BOOL throw_flag;            // 恢复时是否注入异常（generator.throw()）
    BOOL is_completed;          // TRUE 后 frame 已释放，不可再恢复
    JSValue resolving_funcs[2]; // async 函数的 [resolve, reject]（generator 不用）
    JSStackFrame frame;         // 完整栈帧（含 arg_buf/var_buf/cur_sp/cur_pc）
} JSAsyncFunctionState;
```

`frame.arg_buf` 指向堆上分配的连续内存：`[arg_buf | var_buf | stack_buf]`。

### JSGeneratorData（Generator 状态机）

```c
typedef enum JSGeneratorStateEnum {
    JS_GENERATOR_STATE_SUSPENDED_START,  // 首次调用前
    JS_GENERATOR_STATE_SUSPENDED_YIELD,  // yield 挂起
    JS_GENERATOR_STATE_SUSPENDED_YIELD_STAR, // yield* 挂起
    JS_GENERATOR_STATE_EXECUTING,        // 正在运行
    JS_GENERATOR_STATE_COMPLETED,        // 已完成（return 或抛出）
} JSGeneratorStateEnum;

typedef struct JSGeneratorData {
    JSGeneratorStateEnum state;
    JSAsyncFunctionState *func_state; // NULL 表示已完成
} JSGeneratorData;
```

### JSAsyncGeneratorData（AsyncGenerator 状态机）

```c
typedef enum JSAsyncGeneratorStateEnum {
    JS_ASYNC_GENERATOR_STATE_SUSPENDED_START,
    JS_ASYNC_GENERATOR_STATE_SUSPENDED_YIELD,
    JS_ASYNC_GENERATOR_STATE_EXECUTING,
    JS_ASYNC_GENERATOR_STATE_AWAITING_RETURN,
    JS_ASYNC_GENERATOR_STATE_COMPLETED,
} JSAsyncGeneratorStateEnum;

typedef struct JSAsyncGeneratorData {
    JSAsyncGeneratorStateEnum state;
    JSAsyncFunctionState *func_state;
    struct list_head queue;  // 挂起的 .next()/.return()/.throw() 请求队列
} JSAsyncGeneratorData;
```

---

## Generator 实现

### 创建

```c
// 调用 generator function 时不执行函数体，只创建 generator 对象
JSValue js_create_generator(JSContext *ctx, ...) {
    // 1. 创建 JSAsyncFunctionState，分配堆上栈帧
    async_func = js_async_function_init(ctx, func_obj, this_val, argc, argv);
    // 2. 创建 generator 对象（class_id = JS_CLASS_GENERATOR）
    gen_obj = JS_NewObjectClass(ctx, JS_CLASS_GENERATOR);
    // 3. 绑定 JSGeneratorData，初始状态 SUSPENDED_START
    gen_data->state = JS_GENERATOR_STATE_SUSPENDED_START;
    gen_data->func_state = async_func;
    return gen_obj;
}
```

### next() / return() / throw()

```c
JSValue js_generator_next(JSContext *ctx, JSValue gen_obj, int magic,
                           JSValue *argv, int argc) {
    JSGeneratorData *s = JS_GetOpaque(gen_obj, JS_CLASS_GENERATOR);
    JSAsyncFunctionState *func_state = s->func_state;

    switch (s->state) {
    case JS_GENERATOR_STATE_COMPLETED:
        return js_create_iterator_result(ctx, JS_UNDEFINED, TRUE);

    case JS_GENERATOR_STATE_SUSPENDED_YIELD:
    case JS_GENERATOR_STATE_SUSPENDED_START:
        // 设置注入值（yield 表达式的返回值）
        if (magic == GEN_MAGIC_NEXT) {
            // 将 argv[0] 写入 cur_sp[-1]（yield 表达式位置）
            *func_state->frame.cur_sp = JS_DupValue(ctx, argv[0]);
            func_state->throw_flag = FALSE;
        } else if (magic == GEN_MAGIC_THROW) {
            func_state->throw_flag = TRUE;
            rt->current_exception = JS_DupValue(ctx, argv[0]);
        }

        s->state = JS_GENERATOR_STATE_EXECUTING;
        // 恢复执行
        JSValue ret = async_func_resume(ctx, func_state);

        if (func_state->is_completed) {
            s->state = JS_GENERATOR_STATE_COMPLETED;
            // ret 是函数返回值，包装为 { value: ret, done: true }
            return js_create_iterator_result(ctx, ret, TRUE);
        } else {
            // 遇到 yield，ret 是 yield 的值
            // 包装为 { value: ret, done: false }
            return js_create_iterator_result(ctx, ret, FALSE);
        }
    }
}
```

### OP_yield 指令处理

```c
case OP_yield: {
    JSValue val = *--sp;  // yield 的值
    // 保存当前状态
    sf->cur_sp = sp;      // 保存栈顶
    // 返回特殊标记，告知调用者这是 yield 而非 return
    ret_val = JS_MKVAL(JS_TAG_INT, FUNC_RET_YIELD);
    // 将 yield 值存入 func_state（调用者从这里取）
    func_state->yield_value = val;
    goto done;  // 退出 JS_CallInternal，但不释放堆上栈帧
}
```

---

## Async 函数实现

### 创建与启动

```c
// 调用 async function 时：
// 1. 创建 Promise capability（resolve/reject）
// 2. 创建 JSAsyncFunctionState
// 3. 立即开始执行（不像 generator 那样延迟）
JSValue js_async_function_call(JSContext *ctx, ...) {
    JSValue promise = JS_NewPromiseCapability(ctx, resolving_funcs);
    JSAsyncFunctionState *s = js_async_function_init(ctx, ...);
    s->resolving_funcs[0] = resolving_funcs[0]; // resolve
    s->resolving_funcs[1] = resolving_funcs[1]; // reject
    async_func_resume(ctx, s);
    return promise;
}
```

### OP_await 指令处理

```c
case OP_await: {
    JSValue val = *--sp;  // await 的值
    sf->cur_sp = sp;      // 保存栈顶

    // 1. 将 val 包装为 Promise（若不是 Promise 则 Promise.resolve(val)）
    // 2. 注册 then 回调：回调触发时恢复执行
    js_async_function_resolve_thenable(ctx, func_state, val);

    ret_val = JS_MKVAL(JS_TAG_INT, FUNC_RET_AWAIT);
    goto done;  // 挂起，等待 Promise 解决
}

// then 回调（microtask）触发时：
void js_async_function_resume_cb(JSContext *ctx, JSValue result, BOOL is_reject,
                                  JSAsyncFunctionState *s) {
    if (is_reject) {
        s->throw_flag = TRUE;
        rt->current_exception = JS_DupValue(ctx, result);
    } else {
        // 将 result 写入 cur_sp（await 表达式的返回值）
        *s->frame.cur_sp++ = JS_DupValue(ctx, result);
        s->throw_flag = FALSE;
    }
    async_func_resume(ctx, s);
}
```

### 完成处理

```c
// async 函数 return 时（is_completed = TRUE）：
void js_async_function_finish(JSContext *ctx, JSAsyncFunctionState *s,
                               JSValue ret_val) {
    if (JS_IsException(ret_val)) {
        // 调用 reject(exception)
        JS_Call(ctx, s->resolving_funcs[1], JS_UNDEFINED, 1, &exception);
    } else {
        // 调用 resolve(ret_val)
        JS_Call(ctx, s->resolving_funcs[0], JS_UNDEFINED, 1, &ret_val);
    }
    // 释放 resolving_funcs 和 func_state
}
```

---

## 堆上栈帧管理

```c
// 初始化：将参数和变量复制到堆上
static JSAsyncFunctionState *js_async_function_init(JSContext *ctx,
                                                      JSValue func_obj,
                                                      JSValue this_val,
                                                      int argc, JSValue *argv) {
    JSFunctionBytecode *b = ...;
    int alloc_size = (b->arg_count + b->var_count + b->stack_size) * sizeof(JSValue);

    s = js_mallocz(ctx, sizeof(*s) + alloc_size);
    s->frame.arg_buf = (JSValue *)(s + 1);
    s->frame.var_buf = s->frame.arg_buf + b->arg_count;
    // 复制参数，初始化局部变量为 JS_UNDEFINED
    // ...
    s->frame.cur_pc = b->byte_code_buf;  // PC 指向函数开头
    s->frame.cur_sp = s->frame.var_buf + b->var_count; // sp 指向栈底
    return s;
}

// GC mark：遍历堆上栈帧中的所有 JSValue
static void js_async_function_mark(JSRuntime *rt, JSValue val,
                                    JS_MarkFunc *mark_func) {
    JSAsyncFunctionState *s = ...;
    if (!s->is_completed) {
        // 标记 arg_buf / var_buf / stack（sp_start 到 cur_sp）
        for (JSValue *p = s->frame.arg_buf; p < s->frame.cur_sp; p++)
            mark_func(rt, p);
    }
}
```

---

## 设计要点

1. **堆上栈帧**：`async_func_init` 将整个 `[arg_buf|var_buf|stack_buf]` 用 `js_malloc` 分配到堆，`frame.cur_sp` 记录挂起时的栈顶，恢复时原样传入 `JS_CallInternal`。

2. **`throw_flag` 注入异常**：恢复执行时若 `throw_flag=TRUE`，`JS_CallInternal` 开头直接跳 `exception:` 标签，实现 `generator.throw()` 语义。

3. **`is_completed` 防止重入**：`is_completed=TRUE` 后 `frame.arg_buf` 已释放，任何继续调用都返回 `{ done: true }`，不再进入 `JS_CallInternal`。

4. **Async 集成 Promise**：`resolving_funcs[0/1]` 在完成时调用，将返回值 resolve 或将异常 reject，协程本身无需感知 Promise 细节。

5. **Generator 与 Async 共用 JSAsyncFunctionState**：两者只在"是否立即执行"和"是否有 resolving_funcs"上有区别，核心挂起/恢复机制完全共用。

---

## 实现建议

| 阶段 | 建议 |
|------|------|
| 第一步 | 先实现同步 Generator：`yield` 挂起 + `.next()` 恢复，核心是把栈帧搬到堆 |
| 第二步 | 在 Generator 基础上加 Async：`await` 挂起 + Promise.then 恢复 |
| 第三步 | 实现 AsyncGenerator（两者结合） |
| GC 必须正确 | 堆上栈帧中的 JSValue 必须被 GC mark，否则会出现悬空引用 |
| `cur_sp` 精确性 | `cur_sp` 必须精确指向栈顶，GC 只扫描 `[arg_buf, cur_sp)` 范围 |
