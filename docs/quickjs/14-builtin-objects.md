# 14 内置对象注册

## 概述

QuickJS 通过 **JSClassDef**（类定义）+ **JSCFunctionListEntry**（属性批量注册表）的组合，以数据驱动的方式注册所有内置对象，最小化重复代码。

---

## 核心数据结构

### JSClass（运行时类定义）

```c
struct JSClass {
    uint32_t class_id;           // 0 = 空闲槽
    JSAtom class_name;           // 类名（用于 Object.prototype.toString）
    JSClassFinalizer *finalizer; // GC 回收时调用（释放 C 资源）
    JSClassGCMark *gc_mark;      // 标记子对象（GC 遍历）
    JSClassCall *call;           // 非 NULL 则对象可调用（函数/构造器）
    const JSClassExoticMethods *exotic; // 重写内部方法（Proxy/Array 等）
};

// 注册新类
void JS_NewClass(JSRuntime *rt, JSClassID class_id, const JSClassDef *class_def);
// 绑定原型（每个 context 独立）
void JS_SetClassProto(JSContext *ctx, JSClassID class_id, JSValue obj);
```

### JSClassExoticMethods（内部方法重写）

```c
typedef struct JSClassExoticMethods {
    // 重写 [[GetOwnProperty]]
    int (*get_own_property)(JSContext *ctx, JSPropertyDescriptor *desc,
                             JSValue obj, JSAtom prop);
    // 重写 [[GetOwnPropertyNames]]
    int (*get_own_property_names)(JSContext *ctx, JSPropertyEnum **ptab,
                                   uint32_t *plen, JSValue obj);
    // 重写 [[Delete]]
    int (*delete_property)(JSContext *ctx, JSValue obj, JSAtom prop);
    // 重写 [[DefineOwnProperty]]
    int (*define_own_property)(JSContext *ctx, JSValue this_obj, JSAtom prop,
                                JSValue val, JSValue getter, JSValue setter,
                                int flags);
    // 重写 [[HasProperty]]
    int (*has_property)(JSContext *ctx, JSValue obj, JSAtom atom);
    // 重写 [[Get]]
    JSValue (*get_property)(JSContext *ctx, JSValue obj, JSAtom atom,
                             JSValue receiver);
    // 重写 [[Set]]
    int (*set_property)(JSContext *ctx, JSValue obj, JSAtom atom,
                         JSValue value, JSValue receiver, int flags);
} JSClassExoticMethods;
```

### JSCFunctionListEntry（属性批量注册）

```c
typedef struct JSCFunctionListEntry {
    const char *name;       // 属性名
    uint8_t prop_flags;     // JS_PROP_WRITABLE | CONFIGURABLE | ENUMERABLE
    uint8_t def_type;       // 见下方枚举
    int16_t magic;          // 传给 JSCFunctionMagic 的整数标签

    union {
        struct {
            uint8_t length;          // function.length
            uint8_t cproto;          // JSCFunctionEnum（调用约定）
            JSCFunctionType cfunc;   // C 函数指针
        } func;
        struct {
            JSCFunctionType get;     // getter
            JSCFunctionType set;     // setter（NULL = 只读）
        } getset;
        struct {
            const char *name;        // 别名目标属性名
            int base;                // 在哪个原型层级查找
        } alias;
        struct {
            const struct JSCFunctionListEntry *tab; // 嵌套属性表
            int len;
        } prop_list;
        const char *str;             // 字符串常量属性
        int32_t i32;                 // 整数常量属性
        int64_t i64;
        double f64;
    } u;
} JSCFunctionListEntry;

// def_type 枚举
enum {
    JS_DEF_CFUNC,         // 普通 C 函数
    JS_DEF_CGETSET,       // getter/setter 对
    JS_DEF_CGETSET_MAGIC, // 带 magic 的 getter/setter
    JS_DEF_PROP_STRING,   // 字符串常量属性
    JS_DEF_PROP_INT32,    // 整数常量属性
    JS_DEF_PROP_INT64,
    JS_DEF_PROP_DOUBLE,
    JS_DEF_PROP_UNDEFINED,
    JS_DEF_OBJECT,        // 嵌套对象（如 Math.E、Number.MAX_VALUE）
    JS_DEF_ALIAS,         // 属性别名
};
```

---

## 批量注册 API

```c
// 将 JSCFunctionListEntry 数组注册到对象 obj 上
void JS_SetPropertyFunctionList(JSContext *ctx, JSValue obj,
                                 const JSCFunctionListEntry *tab, int len);

// 典型用法
static const JSCFunctionListEntry js_array_proto_funcs[] = {
    JS_CFUNC_DEF("push",         1, js_array_push),
    JS_CFUNC_DEF("pop",          0, js_array_pop),
    JS_CFUNC_MAGIC_DEF("indexOf", 1, js_array_indexOf, 0),  // magic=0: 正向
    JS_CFUNC_MAGIC_DEF("lastIndexOf", 1, js_array_indexOf, 1), // magic=1: 反向
    JS_CGETSET_DEF("length", js_array_get_length, js_array_set_length),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "Array", JS_PROP_CONFIGURABLE),
};

// 注册
JS_SetPropertyFunctionList(ctx, proto, js_array_proto_funcs,
                            countof(js_array_proto_funcs));
```

### 辅助宏

```c
#define JS_CFUNC_DEF(name, length, func) \
    { name, JS_PROP_WRITABLE|JS_PROP_CONFIGURABLE, JS_DEF_CFUNC, 0, \
      .u = { .func = { length, JS_CFUNC_generic, { .generic = func } } } }

#define JS_CFUNC_MAGIC_DEF(name, length, func, magic) \
    { name, JS_PROP_WRITABLE|JS_PROP_CONFIGURABLE, JS_DEF_CFUNC, magic, \
      .u = { .func = { length, JS_CFUNC_generic_magic, { .generic_magic = func } } } }

#define JS_CGETSET_DEF(name, fgetter, fsetter) \
    { name, JS_PROP_CONFIGURABLE, JS_DEF_CGETSET, 0, \
      .u = { .getset = { .get = { .getter = fgetter }, \
                         .set = { .setter = fsetter } } } }

#define JS_PROP_STRING_DEF(name, cstr, prop_flags) \
    { name, prop_flags, JS_DEF_PROP_STRING, 0, \
      .u = { .str = cstr } }
```

---

## magic 字段的妙用

`magic` 是传递给 C 函数的整数标签，允许多个属性共用一个 C 函数：

```c
// 一个函数处理多种行为
static JSValue js_string_includes(JSContext *ctx, JSValue this_val,
                                   int argc, JSValue *argv, int magic) {
    // magic = 0: String.prototype.includes
    // magic = 1: String.prototype.startsWith
    // magic = 2: String.prototype.endsWith
    switch (magic) {
    case 0: /* includes 逻辑 */ break;
    case 1: /* startsWith 逻辑 */ break;
    case 2: /* endsWith 逻辑 */ break;
    }
}

// 注册
JS_CFUNC_MAGIC_DEF("includes",   1, js_string_includes, 0),
JS_CFUNC_MAGIC_DEF("startsWith", 1, js_string_includes, 1),
JS_CFUNC_MAGIC_DEF("endsWith",   1, js_string_includes, 2),
```

---

## 内置类注册流程

### 两步注册

```c
// Step 1: Runtime 级别注册类定义（全局共享）
static const JSClassDef js_array_class_def = {
    "Array",
    .finalizer = js_array_finalizer,
    .gc_mark = js_array_mark,
    .exotic = &js_array_exotic_methods,  // 重写 [[DefineOwnProperty]] 等
};
JS_NewClass(rt, JS_CLASS_ARRAY, &js_array_class_def);

// Step 2: Context 级别绑定原型（每个 context 独立）
JSValue proto = JS_NewObject(ctx);
JS_SetPropertyFunctionList(ctx, proto, js_array_proto_funcs, countof(js_array_proto_funcs));
JS_SetClassProto(ctx, JS_CLASS_ARRAY, proto);
```

### 内置类 ID 列表（部分）

```c
JS_CLASS_OBJECT = 1,       // 基础对象
JS_CLASS_ARRAY,            // Array（fast_array 优化）
JS_CLASS_ERROR,
JS_CLASS_NUMBER,           // Number 包装对象
JS_CLASS_STRING,           // String 包装对象
JS_CLASS_REGEXP,
JS_CLASS_MAP, JS_CLASS_SET,
JS_CLASS_WEAKMAP, JS_CLASS_WEAKSET,
JS_CLASS_PROMISE,
JS_CLASS_PROXY,
JS_CLASS_ARRAY_BUFFER,
JS_CLASS_UINT8C_ARRAY, ... JS_CLASS_FLOAT64_ARRAY, // TypedArray 系列
JS_CLASS_GENERATOR,
JS_CLASS_ASYNC_FUNCTION,
// ...共约 50 个预定义类
```

---

## 典型内置对象注册示例

### Math 对象

```c
static const JSCFunctionListEntry js_math_funcs[] = {
    JS_CFUNC_MAGIC_DEF("min",   2, js_math_min_max, 0),  // magic=0: min
    JS_CFUNC_MAGIC_DEF("max",   2, js_math_min_max, 1),  // magic=1: max
    JS_CFUNC_DEF("abs",         1, js_math_abs),
    JS_CFUNC_DEF("floor",       1, js_math_floor),
    JS_PROP_DOUBLE_DEF("PI",    M_PI,    JS_PROP_CONFIGURABLE),
    JS_PROP_DOUBLE_DEF("E",     M_E,     JS_PROP_CONFIGURABLE),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "Math", JS_PROP_CONFIGURABLE),
};

// 注册为 globalThis.Math
JSValue math_obj = JS_NewObject(ctx);
JS_SetPropertyFunctionList(ctx, math_obj, js_math_funcs, countof(js_math_funcs));
JS_SetPropertyStr(ctx, ctx->global_obj, "Math", math_obj);
```

---

## 设计要点

1. **两步注册分离 Runtime 与 Context**：Runtime 共享类定义，Context 持有原型——多 context 场景下类定义只存一份。

2. **`magic` 字段大幅减少函数数量**：行为相似、仅方向/类型不同的内置方法合并为带 magic 的单个 C 函数，如 `indexOf`/`lastIndexOf`、`includes`/`startsWith`/`endsWith`。

3. **静态描述表驱动**：`JSCFunctionListEntry` 数组声明为 `static const`，编译期确定，运行时零动态分配（除了 JSValue 对象本身）。

4. **`JSClassExoticMethods` 仅按需设置**：普通对象留 NULL 走快速路径，只有 Proxy、TypedArray、Array（length 特殊语义）等才需要 exotic handler。

5. **`JSClassCall` 使对象可调用**：`call != NULL` 时，`JS_Call(obj, ...)` 会调用此函数，用于实现函数对象（bytecode/C function/bound function）。

---

## 实现建议

| 决策点 | 建议 |
|--------|------|
| class_id 分配 | 从 1 开始递增，0 为无效 ID；预定义类用枚举固定，用户自定义类动态分配 |
| 批量注册宏 | 实现 `JS_CFUNC_DEF` 等宏，大幅减少内置对象注册代码量 |
| magic 字段 | 优先识别哪些内置方法行为相似，合并为带 magic 的单函数 |
| exotic 方法 | 初期只实现 Array 的 length exotic（最常用），其他按需添加 |
| 注册顺序 | Object → Function → Array → ... 有依赖关系，Object 必须最先注册 |
