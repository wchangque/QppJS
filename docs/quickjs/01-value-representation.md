# 01 Value 表示系统

## 概述

QuickJS 的 `JSValue` 是引擎中所有 JS 值的统一载体，采用 **NaN-boxing** 编码（32位平台）或 **tag+union 结构体**（64位平台）。

---

## 核心数据结构

### 类型标签（Tag）

```c
// quickjs.h
#define JS_TAG_FIRST         (-11)  // 最小有效 tag
#define JS_TAG_BIG_INT        (-9)  // BigInt（堆对象）
#define JS_TAG_SYMBOL         (-8)  // Symbol（堆对象）
#define JS_TAG_STRING         (-7)  // String（堆对象）
#define JS_TAG_MODULE         (-3)  // Module（内部，堆对象）
#define JS_TAG_FUNCTION_BYTECODE (-2) // 字节码（内部，堆对象）
#define JS_TAG_OBJECT         (-1)  // 普通对象（堆对象）

#define JS_TAG_INT             (0)  // 31位整数（直接嵌入）
#define JS_TAG_BOOL            (1)  // true/false
#define JS_TAG_NULL            (2)  // null
#define JS_TAG_UNDEFINED       (3)  // undefined
#define JS_TAG_UNINITIALIZED   (5)  // 内部：未初始化变量
#define JS_TAG_CATCH_OFFSET    (6)  // 内部：catch 偏移
#define JS_TAG_EXCEPTION       (7)  // 内部：异常传播标记
#define JS_TAG_FLOAT64         (8)  // 64位浮点（NaN-boxing 中 >= 8 均为 float）
```

**规律**：`tag < 0` 的值指向堆上带引用计数的对象；`tag >= 0` 的值直接编码在 JSValue 中。

### 64位平台：结构体形式

```c
typedef struct JSValue {
    union {
        int32_t  int32;
        double   float64;
        void    *ptr;        // 堆对象指针
    } u;
    int64_t tag;             // 类型标签
} JSValue;

// 访问宏
#define JS_VALUE_GET_TAG(v)   ((int)(v).tag)
#define JS_VALUE_GET_INT(v)   ((v).u.int32)
#define JS_VALUE_GET_FLOAT64(v) ((v).u.float64)
#define JS_VALUE_GET_PTR(v)   ((v).u.ptr)
```

### 32位平台：NaN-boxing

```c
typedef uint64_t JSValue;

// IEEE 754 quiet NaN: 0x7FF80000_00000000
// tag 存在高32位，payload 存在低32位
#define JS_VALUE_GET_TAG(v)   (int)((uint64_t)(v) >> 32)
#define JS_VALUE_GET_INT(v)   (int)(v)          // 低32位即整数
#define JS_VALUE_GET_PTR(v)   (void*)(uintptr_t)(v) // 低32位为指针

// Float64 编码：真实 double 加上偏移量后存入
#define JS_FLOAT64_TAG_ADDEND (0x7ff80000 - JS_TAG_FIRST + 1)
// 解码：tag >= JS_TAG_FLOAT64 时，value = raw_double - addend
```

### 引用计数头

所有堆对象（tag < 0）的首字段必须是：

```c
typedef struct {
    int ref_count;  // 32位，必须在 struct 首位
} JSRefCountHeader;
```

---

## 核心操作

```c
// 构造值
#define JS_MKVAL(tag, val)   // 构造非指针值（int/bool/null 等）
#define JS_MKPTR(tag, ptr)   // 构造指针值（object/string 等）

// 判断是否有引用计数
static inline BOOL JS_VALUE_HAS_REF_COUNT(JSValue v) {
    return (unsigned)JS_VALUE_GET_TAG(v) >= (unsigned)JS_TAG_FIRST;
    // tag < 0 → 无符号比较后为大值 → TRUE
}

// 引用计数操作
static inline JSValue JS_DupValue(JSContext *ctx, JSValue v);  // ref++
static inline void JS_FreeValue(JSContext *ctx, JSValue v);    // ref--，到0则释放

// 特殊常量
#define JS_NULL      JS_MKVAL(JS_TAG_NULL, 0)
#define JS_UNDEFINED JS_MKVAL(JS_TAG_UNDEFINED, 0)
#define JS_FALSE     JS_MKVAL(JS_TAG_BOOL, 0)
#define JS_TRUE      JS_MKVAL(JS_TAG_BOOL, 1)
#define JS_EXCEPTION JS_MKVAL(JS_TAG_EXCEPTION, 0)  // 错误传播哨兵
```

---

## 设计要点

1. **负 tag = 堆对象**：`tag < 0` 统一用 `JS_VALUE_HAS_REF_COUNT` 判断，引用计数操作无需 switch，一个比较覆盖所有堆类型。

2. **整数快路径**：`JS_TAG_INT` 直接在低32位存 31 位有符号整数，加减法可先尝试整数运算，溢出再退化为 float64，避免绝大多数算术进入浮点路径。

3. **NaN-boxing 的 Float64 编码**：利用 IEEE 754 quiet NaN 的高位空间存放非 float 标签。真实 float64 存储时加上 `JS_FLOAT64_TAG_ADDEND` 偏移，读取时减去，保证所有合法 double（含 NaN）都能正确往返编码。

4. **内部 tag 不暴露给 JS**：`JS_TAG_EXCEPTION`、`JS_TAG_UNINITIALIZED`、`JS_TAG_CATCH_OFFSET` 仅在引擎内部传递，永远不会作为 JS 语义值出现。`JS_EXCEPTION` 是函数返回失败的哨兵，调用方检查返回值是否等于它来判断是否有异常待处理。

5. **值语义 vs 引用语义**：`JSValue` 是值类型（copy-on-assign），但堆对象通过指针共享。每次"持有"一个 JSValue 都需要 `JS_DupValue`，每次"释放"都需要 `JS_FreeValue`，是最常见的 bug 来源。

---

## 实现建议

| 阶段 | 建议 |
|------|------|
| 初期 | 先实现 64 位结构体版本，逻辑清晰；NaN-boxing 是性能优化，最后再做 |
| Tag 设计 | 负值 tag 对应堆对象的约定要在最初确定，后期改动代价极高 |
| 异常传播 | 用 `JS_EXCEPTION` 哨兵而非 errno/全局变量，函数签名干净，调用链简单 |
| 整数优化 | `JS_TAG_INT` 快路径是高频代码，加法溢出检测用 `__builtin_add_overflow` |
| 调试 | 开发阶段在 `JS_FreeValue` 中加断言：`ref_count > 0`，能快速定位 double-free |
