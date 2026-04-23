# 12 模块系统（ES Module）

## 概述

QuickJS 实现了完整的 ES Module 规范，包括静态 import/export、动态 `import()`、循环依赖处理（Tarjan SCC）和顶层 await（TLA）。

---

## 核心数据结构

### JSModuleDef

```c
struct JSModuleDef {
    JSGCObjectHeader header;  // 参与 GC
    JSAtom module_name;       // 模块标识符（规范化后的路径）
    struct list_head link;    // 挂在 ctx->loaded_modules

    // === 依赖关系 ===
    JSReqModuleEntry *req_module_entries;  // import 的依赖模块列表
    int req_module_entries_count;

    // === 导出表 ===
    JSExportEntry *export_entries;         // export 声明
    int export_entries_count;
    JSStarExportEntry *star_export_entries; // export * from '...'
    int star_export_entries_count;

    // === 导入绑定 ===
    JSImportEntry *import_entries;         // import 绑定（映射到闭包变量）
    int import_entries_count;

    // === 执行体 ===
    JSValue module_ns;     // 模块命名空间对象（import * as ns）
    JSValue func_obj;      // 模块体字节码（JS 模块）
    JSModuleInitFunc *init_func; // C 模块初始化回调（NULL = JS 模块）

    // === 状态机 ===
    JSModuleStatus status : 8;  // 见下方枚举

    // === DFS 算法状态（链接/求值期临时使用）===
    int dfs_index;              // Tarjan DFS 序号
    int dfs_ancestor_index;     // 最小祖先序号（检测 SCC）
    JSModuleDef *stack_prev;    // DFS 栈链表
    JSModuleDef *cycle_root;    // 所在强连通分量的根节点

    // === 顶层 Await（TLA）===
    BOOL has_tla : 8;
    JSModuleDef **async_parent_modules;    // 依赖本模块的父模块（TLA 传播）
    int async_parent_modules_count;
    int pending_async_dependencies;        // 未完成的异步依赖数
    int64_t async_evaluation_timestamp;    // 求值顺序时间戳

    // === Promise capability（TLA 用）===
    JSValue promise;
    JSValue resolving_funcs[2];  // [resolve, reject]

    // === 异常 ===
    BOOL eval_has_exception : 8;
    JSValue eval_exception;

    // === 其他 ===
    JSValue meta_obj;      // import.meta 对象
    JSValue private_value; // C 模块私有数据
};
```

### 模块状态机（6 态）

```c
typedef enum {
    JS_MODULE_STATUS_UNLINKED,          // 初始：依赖未解析
    JS_MODULE_STATUS_LINKING,           // DFS 链接中（检测循环）
    JS_MODULE_STATUS_LINKED,            // 链接完成，可求值
    JS_MODULE_STATUS_EVALUATING,        // DFS 求值中
    JS_MODULE_STATUS_EVALUATING_ASYNC,  // 等待 TLA 完成
    JS_MODULE_STATUS_EVALUATED,         // 求值完成（或已缓存异常）
} JSModuleStatus;
```

### 导出条目

```c
typedef struct JSExportEntry {
    union {
        struct {
            int var_idx;        // 闭包变量索引
            JSVarRef *var_ref;  // 活引用：指向模块内变量（导入方可见修改）
        } local;                // JS_EXPORT_TYPE_LOCAL
        int req_module_idx;     // JS_EXPORT_TYPE_INDIRECT（re-export）
    } u;
    JSExportTypeEnum export_type;
    JSAtom local_name;   // 模块内的变量名（'*' 表示 export ns from）
    JSAtom export_name;  // 导出的外部名称
} JSExportEntry;
```

---

## 生命周期

### 阶段一：加载（Load）

```
JS_Eval() / import() 触发
  → 调用 module_loader_func(ctx, module_name, opaque)
  → 解析源码，生成 JSModuleDef（status = UNLINKED）
  → 递归加载所有 req_module_entries
```

### 阶段二：链接（Link）

```c
// js_module_link() — Tarjan SCC 算法
static int js_inner_module_linking(JSContext *ctx, JSModuleDef *m,
                                    JSModuleDef **pstack_top, int index) {
    if (m->status == JS_MODULE_STATUS_LINKING) return index;
    if (m->status >= JS_MODULE_STATUS_LINKED) return index;

    m->status = JS_MODULE_STATUS_LINKING;
    m->dfs_index = m->dfs_ancestor_index = index++;
    m->stack_prev = *pstack_top;
    *pstack_top = m;

    // 递归链接所有依赖
    for (int i = 0; i < m->req_module_entries_count; i++) {
        JSModuleDef *req = m->req_module_entries[i].module;
        index = js_inner_module_linking(ctx, req, pstack_top, index);
        m->dfs_ancestor_index = min(m->dfs_ancestor_index, req->dfs_ancestor_index);
    }

    // 检测 SCC 根节点
    if (m->dfs_ancestor_index == m->dfs_index) {
        // 弹出栈上的 SCC 成员，设置 cycle_root
        JSModuleDef *sm = *pstack_top;
        while (sm != m) {
            sm->cycle_root = m;
            sm->status = JS_MODULE_STATUS_LINKED;
            sm = sm->stack_prev;
        }
        m->cycle_root = m;
        m->status = JS_MODULE_STATUS_LINKED;
    }

    // 解析导出绑定：将 import 条目链接到对应的 JSVarRef
    js_link_module_exports(ctx, m);
    return index;
}
```

### 阶段三：求值（Evaluate）

```c
// js_module_evaluate() — 同样使用 DFS
static JSValue js_inner_module_evaluation(JSContext *ctx, JSModuleDef *m,
                                           JSModuleDef **pstack_top, int *pindex) {
    if (m->status == JS_MODULE_STATUS_EVALUATED) {
        // 已求值：若有缓存异常则重新抛出
        if (m->eval_has_exception)
            return JS_Throw(ctx, JS_DupValue(ctx, m->eval_exception));
        return JS_UNDEFINED;
    }

    m->status = JS_MODULE_STATUS_EVALUATING;
    m->dfs_index = m->dfs_ancestor_index = (*pindex)++;

    // 先递归求值所有依赖
    for (int i = 0; i < m->req_module_entries_count; i++) {
        JSValue ret = js_inner_module_evaluation(ctx, req, pstack_top, pindex);
        if (JS_IsException(ret)) {
            // 异常沿 cycle_root 传播
            m->eval_has_exception = TRUE;
            m->eval_exception = JS_GetException(ctx);
            return JS_EXCEPTION;
        }
    }

    // 执行模块体
    if (m->init_func) {
        // C 模块
        m->init_func(ctx, m);
    } else {
        // JS 模块：调用模块体函数
        JS_Call(ctx, m->func_obj, JS_UNDEFINED, 0, NULL);
    }

    // 处理 TLA
    if (m->has_tla) {
        m->status = JS_MODULE_STATUS_EVALUATING_ASYNC;
    } else {
        m->status = JS_MODULE_STATUS_EVALUATED;
    }
}
```

---

## 导出活引用（Live Binding）

```c
// 链接阶段：将导入绑定到导出的 JSVarRef
static int js_link_module_exports(JSContext *ctx, JSModuleDef *m) {
    for (int i = 0; i < m->import_entries_count; i++) {
        JSImportEntry *mi = &m->import_entries[i];
        JSModuleDef *req = m->req_module_entries[mi->req_module_idx].module;

        // 在 req 的导出表中查找 mi->import_name
        JSExportEntry *exp = js_find_export_entry(ctx, req, mi->import_name);

        // 将导入变量的 var_ref 指向导出变量的 var_ref
        // 两者共享同一个 JSVarRef → 导出方修改，导入方立即可见
        m->func_obj.var_refs[mi->var_idx] = JS_DupVarRef(exp->u.local.var_ref);
    }
}
```

---

## 顶层 Await（TLA）

```
模块 A await 某个 Promise
  → A 进入 EVALUATING_ASYNC 状态
  → A 的 pending_async_dependencies 计数器递增到所有父模块

Promise 解决时：
  → async_func_resume 恢复 A 的执行
  → A 完成 → 通知 async_parent_modules[]
  → 每个父模块 pending_async_dependencies--
  → 降为 0 时父模块也完成
  → 按 async_evaluation_timestamp 排序，保证确定性顺序
```

---

## 设计要点

1. **DFS 双阶段**：链接和求值均采用 Tarjan-SCC 算法，`dfs_index` + `dfs_ancestor_index` 检测循环依赖，`cycle_root` 标记 SCC 根节点。

2. **导出绑定是活引用**：`JSExportEntry.u.local.var_ref` 指向闭包变量引用，导入方持有同一 `var_ref`，实现 live binding——导出方修改变量，导入方立即可见。

3. **异常缓存**：`eval_has_exception` + `eval_exception` 缓存求值异常，后续 import 同一模块直接重新抛出，不重复执行。

4. **C 模块与 JS 模块统一接口**：`init_func != NULL` 走 C 路径，`func_obj` 走 JS 字节码路径，外部接口一致。

---

## 实现建议

| 阶段 | 建议 |
|------|------|
| 第一步 | 先实现同步模块（无 TLA）的 6 态状态机和 DFS 链接/求值 |
| 第二步 | 加入 TLA 支持（`has_tla`、`async_parent_modules`、时间戳排序） |
| 循环依赖 | `cycle_root` 字段是处理循环 import 的关键，SCC 内所有模块共享同一根 |
| 活引用 | 链接阶段必须正确建立 `var_ref` 共享，否则导出值修改不可见 |
| 模块缓存 | `ctx->loaded_modules` 链表去重，同一路径只加载一次 |
