# 02 Runtime 与 Context

## 概述

QuickJS 将引擎分为两层：**JSRuntime**（全局资源池，可跨 Context 共享）和 **JSContext**（执行沙箱，持有独立的全局对象和原型链）。

---

## 核心数据结构

### JSRuntime

```c
struct JSRuntime {
    // === 内存管理 ===
    JSMallocFunctions mf;        // 可替换的分配器函数表
    JSMallocState malloc_state;  // 内存统计：count / size / limit

    // === Atom 表（全局字符串 interning）===
    int atom_hash_size;          // 哈希桶数（2的幂）
    int atom_count;              // 当前 atom 数量
    uint32_t *atom_hash;         // 哈希表（开链法，存 atom_array 索引）
    JSAtomStruct **atom_array;   // atom 数组，索引即 JSAtom 值
    int atom_free_index;         // 空闲槽链表头（0 = 无空闲）

    // === 类型系统 ===
    int class_count;
    JSClass *class_array;        // 按 JSClassID 索引的类定义表

    // === GC ===
    struct list_head gc_obj_list;           // 所有 GC 对象链表
    struct list_head gc_zero_ref_count_list;// 引用计数归零待处理队列
    struct list_head tmp_obj_list;          // GC 三步算法临时链表
    JSGCPhaseEnum gc_phase : 8;             // 防重入标志
    size_t malloc_gc_threshold;            // 触发 GC 的内存阈值

    // === 执行状态 ===
    JSValue current_exception;             // 当前异常（跨 context 唯一）
    BOOL current_exception_is_uncatchable : 8;
    struct JSStackFrame *current_stack_frame; // 当前调用栈顶

    // === 宿主钩子 ===
    JSInterruptHandler *interrupt_handler; // 中断回调（超时/Ctrl+C）
    void *interrupt_opaque;
    JSHostPromiseRejectionTracker *host_promise_rejection_tracker;

    // === 微任务队列 ===
    struct list_head job_list;             // Promise microtask FIFO

    // === 模块加载 ===
    JSModuleNormalizeFunc *module_normalize_func;
    JSModuleLoaderFunc *module_loader_func;

    // === Shape 缓存（隐藏类全局共享）===
    int shape_hash_bits;
    JSShape **shape_hash;

    // === 弱引用 ===
    struct list_head weakref_list;

    // === 上下文列表 ===
    struct list_head context_list;         // 挂载的所有 JSContext
};
```

### JSContext

```c
struct JSContext {
    JSGCObjectHeader header; // 自身也是 GC 对象，必须在首位
    JSRuntime *rt;
    struct list_head link;   // 挂在 rt->context_list

    // === 原型链（每个 context 独立）===
    JSValue *class_proto;    // class_proto[class_id] = 该类的原型对象
    JSValue function_proto;
    JSValue function_ctor;
    JSValue array_ctor;
    JSValue regexp_ctor;
    JSValue promise_ctor;
    JSValue native_error_proto[JS_NATIVE_ERROR_COUNT];

    // === 全局对象 ===
    JSValue global_obj;      // globalThis
    JSValue global_var_obj;  // let/const 全局声明的容器对象

    // === 模块 ===
    struct list_head loaded_modules;

    // === 可裁剪功能（NULL = 禁用）===
    JSValue (*compile_regexp)(JSContext *ctx, ...);
    JSValue (*eval_internal)(JSContext *ctx, ...);

    // === 中断计数器 ===
    int interrupt_counter;   // 每条指令递减，归零时调用 interrupt_handler
};
```

### 内存管理

```c
typedef struct {
    void  *(*js_malloc)(JSMallocState *s, size_t size);
    void   (*js_free)(JSMallocState *s, void *ptr);
    void  *(*js_realloc)(JSMallocState *s, void *ptr, size_t size);
    size_t (*js_malloc_usable_size)(const void *ptr);
} JSMallocFunctions;

typedef struct {
    size_t malloc_count;  // 已分配块数
    size_t malloc_size;   // 已用字节（含 usable_size + MALLOC_OVERHEAD）
    size_t malloc_limit;  // 硬上限，0 = 无限制
} JSMallocState;
```

---

## 生命周期

```
JS_NewRuntime()
  ├── 初始化分配器
  ├── 初始化 atom 表（注册预定义 atom）
  ├── 初始化 class 表（注册内置类）
  └── 初始化 shape 哈希

JS_NewContext(rt)
  ├── 分配 JSContext（自身加入 rt->gc_obj_list）
  ├── 初始化 class_proto[] 数组
  ├── 注册内置对象（Object/Array/Function/...）
  └── 设置 global_obj / global_var_obj

[执行脚本...]

JS_FreeContext(ctx)
  └── 释放 class_proto[]、global_obj 等，从 rt->context_list 摘除

JS_FreeRuntime(rt)
  ├── 运行最终 GC（释放残留对象）
  ├── 释放 atom 表
  └── 释放 class 表
```

---

## 设计要点

1. **Runtime 是全局资源池**：atom 表、class 表、shape 缓存、GC 链表、异常状态、microtask 队列全部归 Runtime 所有，多个 Context 共享这些资源。

2. **Context 是执行沙箱**：持有独立的原型链（`class_proto[]`）、全局对象、模块列表。可用于隔离不同脚本域（类比浏览器中的 iframe）。

3. **Context 本身是 GC 对象**：`header` 字段放在首位，GC 扫描时可统一追踪 context 内所有 `JSValue` 引用，防止原型对象被误回收。

4. **微任务跨 Context 统一排队**：`job_list` 挂在 Runtime，所有 Context 的 Promise 回调共用同一队列，执行顺序由入队时间决定。

5. **功能可裁剪**：`compile_regexp` 和 `eval_internal` 为函数指针，传 NULL 即禁用正则/eval，适合嵌入式场景减小代码体积。

6. **中断机制**：`interrupt_counter` 每条字节码指令递减，归零时调用 `interrupt_handler`，实现超时/Ctrl+C 等软中断，无需操作系统信号。

---

## 实现建议

| 决策点 | 建议 |
|--------|------|
| Runtime 初始化顺序 | 分配器 → atom 表 → class 表 → shape 哈希，顺序不能颠倒 |
| 多 Context 隔离 | 同一 Runtime 的多个 Context 共享 atom/class，但各有独立 global；若需完全隔离则创建独立 Runtime |
| 内存限制 | `malloc_limit` 设 0 = 无限制，开发阶段先不限制，稳定后再加 |
| 中断计数器 | 初始值 `JS_INTERRUPT_COUNTER_INIT = 10000`，太小影响性能，太大响应延迟高 |
| 嵌入式裁剪 | 将 `compile_regexp`/`eval_internal` 设为 NULL，并在链接时排除 `libregexp.c` |
