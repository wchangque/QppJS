# 04 对象模型与 Shape

## 概述

QuickJS 的对象系统由三层组成：**JSShape**（隐藏类，描述属性布局）、**JSProperty**（属性值）、**JSObject**（对象实例）。Shape 在运行时全局共享，多个同结构对象共用一份元数据。

---

## 核心数据结构

### JSProperty（属性值）

```c
typedef struct JSProperty {
    union {
        JSValue   value;            // JS_PROP_NORMAL：普通值
        struct {
            JSObject *getter;       // JS_PROP_GETSET：getter（NULL = undefined）
            JSObject *setter;       // JS_PROP_GETSET：setter（NULL = undefined）
        } getset;
        JSVarRef *var_ref;          // JS_PROP_VARREF：闭包变量引用（模块导出用）
        struct {
            uintptr_t realm_and_id; // JS_PROP_AUTOINIT：延迟初始化（低2位存 init_id）
            void *opaque;
        } init;
    } u;
} JSProperty;

// 属性类型掩码（存在 JSShapeProperty.flags 的高2位）
#define JS_PROP_TMASK        (3 << 4)
#define JS_PROP_NORMAL       (0 << 4)  // 普通值
#define JS_PROP_GETSET       (1 << 4)  // getter/setter
#define JS_PROP_VARREF       (2 << 4)  // 变量引用
#define JS_PROP_AUTOINIT     (3 << 4)  // 延迟初始化

// 属性特性标志（ES 规范三特性）
#define JS_PROP_CONFIGURABLE (1 << 0)
#define JS_PROP_WRITABLE     (1 << 1)
#define JS_PROP_ENUMERABLE   (1 << 2)
```

### JSShapeProperty（属性槽元数据）

```c
typedef struct JSShapeProperty {
    uint32_t hash_next : 26; // 属性内部哈希链表下一项（0 = 链尾）
    uint32_t flags     : 6;  // JS_PROP_xxx 标志位
    JSAtom   atom;           // 属性名；JS_ATOM_NULL = 已删除的空槽
} JSShapeProperty;
```

### JSShape（隐藏类）

```c
struct JSShape {
    // 注意：属性哈希表（uint32_t[]）紧接在 JSShape 之前的内存中
    // 通过 prop_hash_end(sh) = (uint32_t*)sh 反向寻址
    JSGCObjectHeader header;

    uint8_t  is_hashed;              // 是否已注册到 rt->shape_hash
    uint8_t  has_small_array_index;  // 是否含数字索引属性（优化标志）
    uint32_t hash;                   // 当前 shape 的哈希值
    uint32_t prop_hash_mask;         // 内部属性哈希表掩码（size - 1）
    int      prop_size;              // 已分配槽数（含空槽）
    int      prop_count;             // 总属性数（含已删除）
    int      deleted_prop_count;     // 已删除属性数

    JSShape *shape_hash_next;        // rt->shape_hash 全局哈希链表
    JSObject *proto;                 // 原型对象（NULL = Object.create(null)）
    JSShapeProperty prop[0];         // 属性元数据数组（紧跟结构体）
};
```

内存布局（单次 malloc）：
```
[ prop_hash_table | JSShape | JSShapeProperty[0] | JSShapeProperty[1] | ... ]
        ↑                ↑
  prop_hash_end(sh)     sh
```

### JSObject（对象实例）

```c
struct JSObject {
    union {
        JSGCObjectHeader header;
        struct {
            int     __gc_ref_count;       // 引用计数
            uint8_t __gc_mark;            // GC 标记位
            uint8_t extensible       : 1; // 是否可扩展
            uint8_t free_mark        : 1; // 释放时临时标记（防循环）
            uint8_t is_exotic        : 1; // 有 exotic handler（Proxy/Array）
            uint8_t fast_array       : 1; // 使用 u.array 快速路径
            uint8_t is_constructor   : 1; // 可作为构造函数
            uint8_t has_immutable_prototype : 1; // 不可修改原型
            uint16_t class_id;            // 类型标签（JS_CLASS_xxx）
        };
    };
    uint32_t weakref_count;  // 弱引用计数；ref+weak 均为 0 才释放结构体

    JSShape    *shape;       // 共享 shape（原型 + 属性名布局）
    JSProperty *prop;        // 属性值数组，与 shape.prop[] 槽位一一对应

    union {
        // JS_CLASS_BYTECODE_FUNCTION / GENERATOR_FUNCTION / ASYNC_FUNCTION
        struct {
            JSFunctionBytecode *function_bytecode;
            JSVarRef **var_refs;      // upvalue 数组
            JSObject *home_object;    // super 访问用
        } func;

        // JS_CLASS_C_FUNCTION
        struct {
            JSContext *realm;
            JSCFunctionType c_function;
            uint8_t length;
            uint8_t cproto;           // JSCFunctionEnum
            int16_t magic;
        } cfunc;

        // JS_CLASS_ARRAY / JS_CLASS_ARGUMENTS / TypedArray
        struct {
            union {
                uint32_t size;               // Array/Arguments 的 length
                struct JSTypedArray *typed_array;
            } u1;
            union {
                JSValue  *values;            // Array/Arguments 元素
                void     *ptr;               // TypedArray 原始指针
                uint8_t  *uint8_ptr;
                // ... 各种类型指针
            } u;
            uint32_t count;                  // 元素个数
        } array;

        JSRegExp   regexp;       // JS_CLASS_REGEXP
        JSValue    object_data;  // 包装对象（Number/String/Boolean/Symbol）
        // ... 其他内置类型
    } u;
};
```

---

## Shape 共享机制

### 查找流程

```
添加属性 (obj, atom, flags)
  ↓
在 rt->shape_hash 中查找匹配的 shape
  ↓ 找到 → 复用，obj->shape = existing_shape
  ↓ 未找到 → clone 当前 shape，添加新属性槽，注册到 shape_hash
```

### 哈希函数

```c
// Linux kernel 同款乘法哈希，雪崩效果好
static inline uint32_t shape_hash(uint32_t h, uint32_t val) {
    return (h + val) * 0x9e370001U;
}
// shape 的哈希 = hash(proto指针, atom_0, flags_0, atom_1, flags_1, ...)
```

### 删除属性

删除属性时**不压缩槽位**：
- 将 `JSShapeProperty.atom` 设为 `JS_ATOM_NULL`（标记空槽）
- `deleted_prop_count++`
- 当 `deleted_prop_count > prop_count/2` 时触发 compact（重建 shape）

---

## 属性查找

```c
// 1. 在 shape 的内部哈希表中找 atom 对应的槽位索引
int find_own_property(JSShapeProperty **ppr, JSShape *sh, JSAtom atom) {
    uint32_t h = atom & sh->prop_hash_mask;
    uint32_t i = prop_hash_end(sh)[-h - 1]; // 哈希桶入口
    while (i != 0) {
        JSShapeProperty *pr = &sh->prop[i - 1];
        if (pr->atom == atom) { *ppr = pr; return i - 1; }
        i = pr->hash_next;
    }
    return -1;
}

// 2. 用槽位索引直接访问属性值
JSProperty *p = &obj->prop[slot_index];
```

---

## 设计要点

1. **Shape 与值分离**：`JSShape` 只存元数据（属性名、flags、顺序），`JSObject.prop[]` 存实际值。相同结构的对象共享同一 `JSShape` 指针，内存中只有一份元数据。

2. **内存布局技巧**：属性哈希表紧接在 `JSShape` 之前，通过 `prop_hash_end(sh) = (uint32_t*)sh` 反向寻址，一次 malloc 包含哈希表 + Shape + 属性数组三段。

3. **fast_array 快速路径**：`class_id == JS_CLASS_ARRAY` 且 `fast_array == 1` 时，元素直接存在 `u.array.values`（连续 `JSValue[]`），跳过 shape 属性查找，性能接近 C 数组。

4. **class_id 驱动多态**：对象行为（get/set/call/construct）由 `rt->class_array[class_id]` 中的虚函数表决定，无需在 JSObject 内嵌函数指针。

5. **双重计数释放**：`weakref_count > 0` 时，即使 `ref_count == 0` 也保留结构体（置 zombie 状态），待弱引用全部清除后才释放内存。

---

## 实现建议

| 阶段 | 建议 |
|------|------|
| 初期 | 先实现"字典模式"（每个对象独立 hash 表），功能稳定后再引入 shape 共享 |
| fast_array | 数组优化优先实现，JS 代码大量依赖数组性能 |
| Shape 哈希 | 初始桶数 16（`1 << 4`），按 2 倍扩容，负载因子 0.5 |
| 属性初始大小 | `JS_PROP_INITIAL_SIZE = 2`，`JS_ARRAY_INITIAL_SIZE = 2`，按需扩容 |
| class_id 虚表 | 添加新内置类型只需注册新 class_id，不修改核心解释器 |
