# 13 Promise 与 Job Queue

## 概述

QuickJS 实现了完整的 ES Promise 规范，Microtask（微任务）通过 **Job Queue** 实现。引擎只提供 `JS_ExecutePendingJob`，不内置事件循环——宿主负责驱动。

---

## 核心数据结构

### JSJobEntry（Microtask 队列项）

```c
typedef struct JSJobEntry {
    struct list_head link;   // 双向链表节点（挂在 rt->job_list）
    JSContext *realm;        // 所属 context（决定执行域）
    JSJobFunc *job_func;     // 回调函数指针
    int argc;
    JSValue argv[0];         // 变长参数（紧跟结构体内存，一次 malloc）
} JSJobEntry;

typedef JSValue JSJobFunc(JSContext *ctx, int argc, JSValue *argv);
```

`rt->job_list` 是全局 FIFO 双向链表，所有 microtask 均入队此处。

### JSPromiseData（Promise 内部状态）

```c
typedef struct JSPromiseData {
    JSPromiseStateEnum promise_state; // PENDING / FULFILLED / REJECTED
    JSValue promise_result;           // fulfilled 值或 rejected 原因
    struct list_head promise_reactions[2]; // [0]=fulfill reactions, [1]=reject reactions
} JSPromiseData;

typedef enum {
    JS_PROMISE_PENDING,
    JS_PROMISE_FULFILLED,
    JS_PROMISE_REJECTED,
} JSPromiseStateEnum;
```

---

## Job Queue 操作

### 入队

```c
// 将一个 microtask 加入队列尾部
static int JS_EnqueueJob(JSContext *ctx, JSJobFunc *job_func,
                          int argc, JSValueConst *argv) {
    JSRuntime *rt = ctx->rt;
    // 一次 malloc 分配 JSJobEntry + argv 数组
    JSJobEntry *e = js_malloc(ctx, sizeof(*e) + argc * sizeof(JSValue));
    e->realm = JS_DupContext(ctx);
    e->job_func = job_func;
    e->argc = argc;
    for (int i = 0; i < argc; i++)
        e->argv[i] = JS_DupValue(ctx, argv[i]);
    list_add_tail(&e->link, &rt->job_list); // FIFO：加到队尾
    return 0;
}
```

### 执行一个 Job

```c
// 宿主调用：每次执行队头一个 job
// 返回值：1=成功, 0=队空, -1=异常
int JS_ExecutePendingJob(JSRuntime *rt, JSContext **pctx) {
    if (list_empty(&rt->job_list)) return 0;  // 队空

    JSJobEntry *e = list_entry(rt->job_list.next, JSJobEntry, link);
    list_del(&e->link);  // 出队

    JSContext *ctx = e->realm;
    *pctx = ctx;

    JSValue res = e->job_func(ctx, e->argc, e->argv);

    // 释放参数和 entry
    for (int i = 0; i < e->argc; i++)
        JS_FreeValue(ctx, e->argv[i]);
    JS_FreeContext(ctx);
    js_free_rt(rt, e);

    if (JS_IsException(res)) return -1;
    JS_FreeValue(ctx, res);
    return 1;
}
```

### 宿主事件循环模板

```c
// 宿主需要实现的事件循环
void event_loop(JSRuntime *rt) {
    JSContext *ctx;
    int err;
    while ((err = JS_ExecutePendingJob(rt, &ctx)) != 0) {
        if (err < 0) {
            // 处理未捕获的 Promise rejection
            JSValue exception = JS_GetException(ctx);
            // log / report...
            JS_FreeValue(ctx, exception);
        }
    }
    // 队空后可以等待 I/O 事件（epoll/kqueue），有事件时再循环
}
```

---

## Promise 实现

### Promise 状态转换

```
new Promise(executor)
  → 状态 PENDING
  → 同步执行 executor(resolve, reject)

resolve(value) 被调用：
  → 状态 PENDING → FULFILLED
  → 将 fulfill_reactions 中的所有回调入队 job_list

reject(reason) 被调用：
  → 状态 PENDING → REJECTED
  → 将 reject_reactions 中的所有回调入队 job_list
  → 若无 reject handler，触发 host_promise_rejection_tracker
```

### then() 实现

```c
// promise.then(onFulfilled, onRejected)
JSValue js_promise_then(JSContext *ctx, JSValue this_val, int argc, JSValue *argv) {
    JSPromiseData *s = JS_GetOpaque(this_val, JS_CLASS_PROMISE);

    // 创建新 Promise（链式调用）
    JSValue result_promise = JS_NewPromiseCapability(ctx, resolving_funcs);

    // 创建 reaction 记录
    JSPromiseReactionData *reaction = {
        .resolving_funcs = { resolving_funcs[0], resolving_funcs[1] },
        .handler = JS_DupValue(ctx, argv[0]),  // onFulfilled
    };

    if (s->promise_state == JS_PROMISE_PENDING) {
        // 还未决议：加入等待队列
        list_add_tail(&reaction->link, &s->promise_reactions[0]);
    } else {
        // 已决议：直接入队 microtask
        JS_EnqueueJob(ctx, js_promise_reaction_job, 2,
                      (JSValue[]){ reaction->handler, s->promise_result });
    }
    return result_promise;
}
```

### Promise reaction job

```c
// microtask：执行 then/catch 回调
static JSValue js_promise_reaction_job(JSContext *ctx, int argc, JSValue *argv) {
    JSValue handler = argv[0];   // onFulfilled / onRejected
    JSValue argument = argv[1];  // 决议值/拒绝原因
    JSValue resolve = argv[2];   // 结果 Promise 的 resolve
    JSValue reject = argv[3];    // 结果 Promise 的 reject

    JSValue result;
    if (JS_IsUndefined(handler)) {
        // 无 handler：透传
        result = JS_DupValue(ctx, argument);
        JS_Call(ctx, resolve, JS_UNDEFINED, 1, &result);
    } else {
        result = JS_Call(ctx, handler, JS_UNDEFINED, 1, &argument);
        if (JS_IsException(result)) {
            JSValue exc = JS_GetException(ctx);
            JS_Call(ctx, reject, JS_UNDEFINED, 1, &exc);
        } else {
            // Promise Resolution Procedure
            js_promise_resolve(ctx, resolve, result);
        }
    }
    return JS_UNDEFINED;
}
```

---

## Promise.all / Promise.race

```c
// Promise.all 通过 magic 字段区分变体：
// magic = 0: Promise.all
// magic = 1: Promise.allSettled
// magic = 2: Promise.any
// magic = 3: Promise.race

static JSValue js_promise_all(JSContext *ctx, JSValue this_val,
                               int argc, JSValue *argv, int magic) {
    // 迭代 argv[0]（iterable）
    // 对每个值调用 Promise.resolve()
    // 注册 then 回调，收集结果
    // 全部完成后 resolve 结果数组
}
```

---

## 未捕获 Rejection 追踪

```c
// 宿主可注册追踪器
JS_SetHostPromiseRejectionTracker(rt, tracker_func, opaque);

// 引擎在以下时机调用：
// 1. Promise 被 reject 且无 rejection handler（isHandled=false）
// 2. 后续添加了 rejection handler（isHandled=true，可取消警告）
typedef void JSHostPromiseRejectionTracker(JSContext *ctx,
                                            JSValue promise,
                                            JSValue reason,
                                            BOOL is_handled,
                                            void *opaque);
```

---

## 设计要点

1. **单次执行一个 Job**：`JS_ExecutePendingJob` 每次只取队头一个 job，宿主事件循环需循环调用直到返回 0。

2. **参数所有权**：job 入队时 `JS_DupValue` 每个参数，执行后统一 `JS_FreeValue`，生命周期明确。

3. **Context 引用计数保护**：`JS_DupContext` 确保 job 执行期间 context 不被销毁，执行后 `JS_FreeContext`。

4. **柔性数组一次 malloc**：`argv[0]` 将参数与 entry 合并为单次 malloc，减少内存碎片。

5. **引擎不内置事件循环**：`JS_ExecutePendingJob` 只处理 microtask，宏任务（setTimeout/I/O）由宿主管理，职责清晰。

---

## 实现建议

| 决策点 | 建议 |
|--------|------|
| Job Queue | 用侵入式双向链表（list_head），避免额外分配 |
| 事件循环 | 宿主循环：`while (JS_ExecutePendingJob(rt, NULL) > 0);` |
| Promise 先实现 | 先实现 `new Promise`、`.then`、`.catch`，再加 `Promise.all` 等静态方法 |
| Rejection 追踪 | 开发阶段默认打印未捕获 rejection，生产环境由宿主决定 |
| microtask 顺序 | 严格 FIFO，不允许插队（符合规范） |
