# 03 Atom 与 String

## 概述

QuickJS 用 **Atom** 系统对属性名和标识符做字符串 interning，所有比较退化为整数比较。**String** 采用 Latin-1/UTF-16 双编码 + Rope 懒拼接两种表示。

---

## Atom 系统

### 核心数据结构

```c
typedef uint32_t JSAtom;

// bit 31 = 1：直接编码的数组整数索引（0 ~ 2^31-1），无需字符串
#define JS_ATOM_TAG_INT      (1U << 31)
#define JS_ATOM_MAX_INT      (JS_ATOM_TAG_INT - 1)

// bit 31 = 0：索引到 JSRuntime.atom_array[] 的字符串条目

// Atom 类型（存储在 JSString.atom_type 的 2 bit 中）
enum {
    JS_ATOM_TYPE_STRING  = 1,  // 普通字符串 atom
    JS_ATOM_TYPE_GLOBAL_SYMBOL = 2,  // Symbol.for() 全局 symbol
    JS_ATOM_TYPE_SYMBOL  = 3,  // Symbol() 本地 symbol
    JS_ATOM_TYPE_PRIVATE = 3,  // 私有字段（hash = JS_ATOM_HASH_PRIVATE 区分）
};
```

### 预定义 Atom（quickjs-atom.h）

```c
// X-Macro 展开为枚举 + 字符串初始化表
#define DEF(name, str) JS_ATOM_##name,
// 示例：
DEF(null, "null")          // → JS_ATOM_null
DEF(length, "length")      // → JS_ATOM_length
DEF(prototype, "prototype")// → JS_ATOM_prototype
DEF(Symbol_iterator, "Symbol.iterator") // Well-Known Symbol
// 约 270 个预定义 atom，启动时批量注册，引用计数永不递减
```

### 哈希表（开链法）

```c
// JSRuntime 中
uint32_t *atom_hash;      // 桶数组，存 atom_array 索引（0 = 空）
JSAtomStruct **atom_array;// atom 条目数组

// 哈希函数（逐字符乘加，与 Linux kernel 同款乘数）
h = h * 263 + c;          // 8-bit 字符
h = h * 1025 + c;         // 16-bit 字符

// 扩容阈值
#define JS_ATOM_COUNT_RESIZE(n) ((n) * 2)  // 负载因子 0.5
```

### 主要操作

```c
JSAtom JS_NewAtom(JSContext *ctx, const char *str);
JSAtom JS_NewAtomLen(JSContext *ctx, const char *str, size_t len);
JSAtom JS_NewAtomUInt32(JSContext *ctx, uint32_t n); // 整数索引快路径
void   JS_FreeAtom(JSContext *ctx, JSAtom atom);
void   JS_DupAtom(JSContext *ctx, JSAtom atom);      // ref++

// 预定义 atom 的引用计数豁免
static inline BOOL __JS_AtomIsConst(JSAtom v) {
    return v < JS_ATOM_END;  // 编译期常量，跳过计数
}
```

### 设计要点

1. **整数索引快路径**：数组下标 `0~2^31-1` 直接用 `JS_ATOM_TAG_INT | n` 编码，完全绕过字符串哈希，属性访问零分配。
2. **比较退化为整数比较**：属性名查找只需比较两个 `uint32_t`，无需 `strcmp`。
3. **预定义常量豁免引用计数**：索引 `< JS_ATOM_END` 的 atom 跳过 `JS_FreeAtom`，消除热路径上的计数开销。
4. **Symbol 与 Private 共用 atom_type=3**：通过 `hash == JS_ATOM_HASH_PRIVATE` 区分私有字段，节省 1 bit。

---

## String 表示

### 核心数据结构

```c
struct JSString {
    JSRefCountHeader header;   // 引用计数，必须在首位
    uint32_t len      : 31;    // 字符数（非字节数）
    uint8_t  is_wide_char : 1; // 0 = Latin-1（uint8_t[]），1 = UTF-16（uint16_t[]）
    uint32_t hash     : 30;    // 字符串哈希（用于 atom 查找）
    uint8_t  atom_type : 2;    // 0 = 非 atom；非0 = 已 intern
    uint32_t hash_next;        // atom 哈希链表下一项（atom_array 索引）
    union {
        uint8_t  str8[0];   // Latin-1，末尾附加隐式 '\0'
        uint16_t str16[0];  // UTF-16 LE
    } u;
};
```

### Rope（懒拼接）

```c
typedef struct JSStringRope {
    JSRefCountHeader header;
    uint32_t len;
    uint8_t  is_wide_char;
    uint8_t  depth;   // rope 树最大深度，超过阈值触发展平
    JSValue  left;    // 左子（JSString 或 JSStringRope）
    JSValue  right;   // 右子
} JSStringRope;

// 相关阈值
#define JS_STRING_ROPE_SHORT_LEN   512   // 短于此长度不用 rope，直接拼接
#define JS_STRING_ROPE_SHORT2_LEN  8192  // 初次创建 rope 的阈值
#define JS_STRING_ROPE_MAX_DEPTH   60    // 超过此深度强制 flatten
```

### 字符访问

```c
// 统一访问接口（内部按 is_wide_char 分支）
static inline uint32_t string_get(const JSString *p, int idx) {
    return p->is_wide_char ? p->u.str16[idx] : p->u.str8[idx];
}
```

### 设计要点

1. **双编码节省内存**：纯 ASCII/Latin-1 内容用 `str8`，每字符 1 字节；含 BMP 外字符时升级为 `str16`，每字符 2 字节。编码升级单向不可降级。

2. **`len` 是字符数而非字节数**：调用方无需关心底层编码，通过 `string_get` 统一按字符索引。

3. **末尾隐式 `\0`**：`str8` 分配时多申请 1 字节并清零，可直接传给 C 字符串 API，避免额外复制。

4. **Rope 懒拼接**：`a + b` 操作推迟到真正需要遍历内容时才展平，`depth` 超阈值强制 flatten，防止树退化为链表（O(n) 深度）。

5. **String 与 Atom 共用结构**：`JSString` 的 `hash`/`atom_type`/`hash_next` 字段兼作 atom 哈希链节点，无需单独的 atom 结构体。

---

## 实现建议

| 决策点 | 建议 |
|--------|------|
| Atom 整数编码 | bit 31 区分整数/字符串，是数组访问性能的关键 |
| 预定义 atom | 用 X-Macro 生成枚举和初始化表，启动时批量注册 |
| 哈希表扩容 | 负载因子 > 0.5 时扩容，桶数始终保持 2 的幂（位运算取模） |
| String 编码 | 先只实现 Latin-1（str8），遇到非 Latin-1 字符升级为 str16 |
| Rope | 初期可不实现，直接拼接；字符串密集场景再加 Rope 优化 |
| 内存布局 | 柔性数组 `str8[0]` 内联字符数据，一次 malloc 同时分配头部和内容 |
