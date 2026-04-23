# 05 垃圾回收

## 概述

QuickJS 采用**引用计数 + 循环检测**两阶段策略：引用计数处理无环对象的即时回收，循环检测（三步标记清除）处理循环引用。

---

## 核心数据结构

### GC 对象头

```c
struct JSGCObjectHeader {
    int ref_count;               // 引用计数，必须在首位（32位）
    JSGCObjectTypeEnum gc_obj_type : 4;
    uint8_t mark : 4;            // GC 标记位（0 = 未标记）
    uint8_t dummy1;
    uint16_t dummy2;
    struct list_head link;       // 挂在 rt->gc_obj_list 的侵入式链表节点
};

typedef enum {
    JS_GC_OBJ_TYPE_JS_OBJECT,          // 普通 JS 对象
    JS_GC_OBJ_TYPE_FUNCTION_BYTECODE,  // 字节码对象
    JS_GC_OBJ_TYPE_SHAPE,              // Shape（隐藏类）
    JS_GC_OBJ_TYPE_VAR_REF,            // Upvalue
    JS_GC_OBJ_TYPE_ASYNC_FUNCTION,     // 异步函数状态
    JS_GC_OBJ_TYPE_JS_CONTEXT,         // JSContext 自身
    JS_GC_OBJ_TYPE_MODULE,             // 模块定义
} JSGCObjectTypeEnum;
```

### GC 阶段

```c
typedef enum {
    JS_GC_PHASE_NONE,           // 正常运行
    JS_GC_PHASE_DECREF,         // 第一步：减引用计数
    JS_GC_PHASE_REMOVE_CYCLES,  // 第三步：释放循环垃圾
} JSGCPhaseEnum;
```

---

## 两阶段策略

### 阶段一：引用计数（正常路径）

```c
// 每次赋值/函数返回时调用
static inline void JS_FreeValue(JSContext *ctx, JSValue v) {
    if (JS_VALUE_HAS_REF_COUNT(v)) {
        JSRefCountHeader *p = JS_VALUE_GET_PTR(v);
        if (--p->ref_count == 0)
            __JS_FreeValue(ctx, v);  // 触发释放
    }
}

// 释放时：若对象在 gc_obj_list 中，先加入 gc_zero_ref_count_list
// 再统一处理（防止在析构中途触发递归释放）
```

### 阶段二：循环检测（显式触发）

触发条件：`malloc_size > malloc_gc_threshold` 时在分配前自动触发，或手动调用 `JS_RunGC(rt)`。

#### 三步算法

```
Step 1: gc_decref()
  对 gc_obj_list 中所有对象的子对象调用 mark_children(decref)
  → 找出 ref_count 降为 0 的候选（潜在循环垃圾）
  → 将候选移入 tmp_obj_list

Step 2: gc_scan()
  从 gc_obj_list 中仍有外部引用（ref_count > 0）的对象出发
  → 调用 mark_children(incref) 恢复被误减的引用计数
  → 标记所有可达对象（mark = 1）

Step 3: gc_free_cycles()
  遍历 tmp_obj_list，释放 mark = 0 的对象（真正的循环垃圾）
  将 mark = 1 的对象（误判，实际可达）移回 gc_obj_list
```

#### 关键函数

```c
// 标记子对象（递归遍历所有 JSValue 字段）
static void mark_children(JSRuntime *rt, JSGCObjectHeader *gp,
                           JS_MarkFunc *mark_func) {
    switch (gp->gc_obj_type) {
    case JS_GC_OBJ_TYPE_JS_OBJECT: {
        JSObject *p = (JSObject *)gp;
        // 遍历所有属性
        for (int i = 0; i < sh->prop_count; i++)
            mark_func(rt, &p->prop[i].u.value);
        // 遍历内部数据（array.values、func.var_refs 等）
        break;
    }
    case JS_GC_OBJ_TYPE_FUNCTION_BYTECODE:
        // 遍历常量池 cpool[]
        break;
    // ...
    }
}
```

---

## 弱引用处理

```c
// GC 三步之前先运行
static void gc_remove_weak_objects(JSRuntime *rt) {
    // 遍历 rt->weakref_list
    // 对 WeakMap/WeakRef/FinalizationRegistry 中的 key/target
    // 若其 ref_count 在 gc_decref 后降为 0，则清除弱引用条目
    // 防止 GC 后出现悬空指针
}
```

---

## 对象生命周期

```
创建对象
  → add_gc_object(rt, header, type)  // 加入 gc_obj_list
  → ref_count = 1

JS_DupValue()   → ref_count++
JS_FreeValue()  → ref_count--
                → ref_count == 0 时：
                    if gc_phase == NONE:
                        free_gc_object()  // 立即释放（无环路径）
                    else:
                        加入 gc_zero_ref_count_list（延迟释放）

GC 触发时：
  gc_remove_weak_objects()
  gc_decref()      // Step 1
  gc_scan()        // Step 2
  gc_free_cycles() // Step 3
```

---

## 设计要点

1. **两阶段协同**：引用计数处理 99% 的对象（无环），GC 只处理剩余的循环引用，实际 GC 触发频率很低。

2. **侵入式链表零开销**：`list_head` 内嵌在对象内，遍历所有 GC 对象无需额外的指针数组。

3. **gc_phase 防重入**：`JS_GC_PHASE_REMOVE_CYCLES` 阶段不再递归进入 `free_zero_refcount`，防止在析构中途触发二次 GC。

4. **弱引用前置处理**：`gc_remove_weak_objects` 先于三步流程运行，清理 WeakMap/WeakRef/FinalizationRegistry，避免悬空指针。

5. **所有 GC 对象头布局统一**：`JSObject *`、`JSFunctionBytecode *`、`JSShape *` 等都可安全强转为 `JSGCObjectHeader *`，因为 `header` 字段始终在首位。

---

## 实现建议

| 决策点 | 建议 |
|--------|------|
| 初期 | 先只实现引用计数，不实现循环 GC；大多数 JS 代码无循环引用 |
| GC 触发 | 每次 malloc 后检查 `malloc_size > threshold`，超出则触发 GC |
| mark_children | 每种 GC 对象类型都要实现，遗漏会导致内存泄漏 |
| ref_count 边界 | `JS_FreeValue` 中加断言 `ref_count > 0`，快速发现 double-free |
| 循环引用测试 | 用 `a = {}; a.self = a; a = null` 这类简单用例验证 GC 正确性 |
