# 10 闭包与作用域

## 概述

QuickJS 用 **JSVarRef（upvalue）** 实现闭包变量捕获，采用"开放 upvalue 指向栈，关闭 upvalue 复制到堆"的经典设计（与 Lua 5.x 相同）。

---

## 核心数据结构

### JSVarRef（Upvalue）

```c
typedef struct JSVarRef {
    union {
        JSGCObjectHeader header; // 参与 GC，必须在首位
        struct {
            int __gc_ref_count;
            uint8_t __gc_mark;
            uint8_t is_detached;  // FALSE=开放（指向栈），TRUE=关闭（值在堆上）
        };
    };
    JSValue *pvalue;  // 始终指向"当前有效值"（统一访问接口）
    union {
        JSValue value;  // is_detached=TRUE 时，值存这里，pvalue = &value
        struct {
            struct list_head var_ref_link;  // 挂在 JSStackFrame.var_ref_list
            struct JSAsyncFunctionState *async_func; // 异步帧反向引用（非NULL时）
        };  // is_detached=FALSE 时使用
    };
} JSVarRef;
```

### JSClosureVar（编译期：描述捕获来源）

```c
typedef struct JSClosureVar {
    uint8_t is_local : 1;  // TRUE=来自直接父函数的局部变量
                            // FALSE=来自父函数的 upvalue（穿透捕获）
    uint8_t is_arg   : 1;  // 是否为参数
    uint8_t is_const : 1;  // 是否为 const（运行时写入检查）
    uint8_t is_lexical : 1;
    uint8_t var_kind : 4;  // JSVarKindEnum
    uint16_t var_idx;      // 父函数中的变量/upvalue 索引
    JSAtom var_name;
} JSClosureVar;
```

### JSVarDef（编译期：变量定义）

```c
typedef struct JSVarDef {
    JSAtom var_name;
    int scope_level;     // 词法作用域层级（0 = 函数顶层）
    int scope_next;      // 同作用域下一个变量的索引（链表）
    uint8_t is_const    : 1;
    uint8_t is_lexical  : 1;  // let/const（非 var）
    uint8_t is_captured : 1;  // 是否被闭包捕获（需要 JSVarRef）
    uint8_t var_kind    : 4;
} JSVarDef;
```

---

## 开放 Upvalue（Open）

函数仍在运行时，upvalue 直接指向 C 栈上的变量槽：

```
JSVarRef.is_detached = FALSE
JSVarRef.pvalue = &sf->var_buf[var_idx]  ← 指向栈上变量
JSVarRef.var_ref_link → 挂在 sf->var_ref_list

读取：*pvalue  （直接解引用，零开销）
写入：*pvalue = new_val  （直接写入栈槽）
```

所有共享同一变量的闭包持有同一个 `JSVarRef`，通过 `pvalue` 间接访问，实现"活引用"语义。

---

## 关闭 Upvalue（Close）

函数返回时，`close_var_refs()` 关闭所有开放 upvalue：

```c
static void close_var_refs(JSRuntime *rt, JSStackFrame *sf) {
    struct list_head *el, *el1;
    list_for_each_safe(el, el1, &sf->var_ref_list) {
        JSVarRef *var_ref = list_entry(el, JSVarRef, var_ref_link);
        var_ref->value = *var_ref->pvalue;  // 将栈值复制到堆上
        var_ref->pvalue = &var_ref->value;  // pvalue 改为指向堆上的 value
        var_ref->is_detached = TRUE;
        list_del(&var_ref->var_ref_link);
    }
}
```

关闭后，`pvalue` 解引用语义不变，所有持有该 upvalue 的闭包无需修改代码。

---

## 多级捕获（穿透）

```
// 源码
function outer() {
    let x = 1;
    function middle() {
        function inner() {
            return x;  // 穿透两层捕获
        }
    }
}

// 编译结果
outer.closure_var = []          // outer 不捕获任何变量
middle.closure_var = [
    { is_local: true, var_idx: outer.vars["x"] }  // 直接捕获 outer 的局部变量
]
inner.closure_var = [
    { is_local: false, var_idx: 0 }  // 捕获 middle 的 upvalue[0]（间接引用 x）
]
```

运行期只需查 `var_refs[]` 数组，无需递归向上查找。

---

## 字节码指令

```c
// 读取 upvalue
OP_get_var_ref  [2字节索引]
// 等价于：*sp++ = JS_DupValue(ctx, *func.var_refs[idx]->pvalue)

// 写入 upvalue
OP_put_var_ref  [2字节索引]
// 等价于：JS_FreeValue(ctx, *func.var_refs[idx]->pvalue);
//          *func.var_refs[idx]->pvalue = *--sp;

// 关闭 upvalue（离开块级作用域时）
OP_close_var_refs
// 将 var_ref_list 中 pvalue 指向 >= scope_start 的 upvalue 全部关闭
```

---

## 作用域管理

### 词法作用域层级

```
function f() {          // scope_level = 0（函数顶层）
    var x;              // scope_level = 0（var 提升到函数顶层）
    {                   // scope_level = 1
        let y;          // scope_level = 1
        {               // scope_level = 2
            const z;    // scope_level = 2
        }               // 离开：关闭 scope_level >= 2 的 upvalue
    }                   // 离开：关闭 scope_level >= 1 的 upvalue
}
```

### 变量查找顺序

```
1. 当前函数 vars[]（局部变量）→ OP_get_loc / OP_put_loc
2. 当前函数 args[]（参数）    → OP_get_arg / OP_put_arg
3. 父函数的变量（闭包捕获）   → OP_get_var_ref / OP_put_var_ref
4. 全局变量                  → OP_get_var / OP_put_var
```

---

## 设计要点

1. **`pvalue` 统一接口**：读写 upvalue 始终通过 `*pvalue` 解引用，开放/关闭状态对调用方透明，切换时只需修改 `pvalue` 指向，无需修改读写代码。

2. **链表管理开放 upvalue**：所有开放 upvalue 通过 `var_ref_link` 挂在帧的 `var_ref_list`，帧销毁时 O(n) 线性扫描一次性关闭全部。

3. **多级捕获编译期解决**：`JSClosureVar.is_local=FALSE` 表示穿透捕获，编译器递归向上传递 upvalue 索引，运行期只是数组下标访问。

4. **`is_captured` 标志**：编译期标记，告知代码生成器该变量需要分配 `JSVarRef`，未被捕获的变量直接用栈偏移访问，无 upvalue 对象开销。

5. **异步帧反向引用**：`async_func` 字段用于 Generator/Async 场景，upvalue 关闭时需要通知异步帧（因为异步帧的栈在堆上）。

---

## 实现建议

| 决策点 | 建议 |
|--------|------|
| 先实现简单版 | 先用"每次捕获都复制值"（不支持共享修改），验证基本功能后再实现 pvalue 间接访问 |
| pvalue 间接访问 | 是正确实现闭包共享语义的关键，不能省略 |
| close 时机 | 离开 `{}` 块（`OP_close_var_refs`）和函数返回（`close_var_refs`）各触发一次 |
| const 检查 | 写入 const 变量时，检查 `JSClosureVar.is_const`，抛 TypeError |
| 调试 | 在 `close_var_refs` 后断言 `var_ref_list` 为空，快速发现 upvalue 泄漏 |
