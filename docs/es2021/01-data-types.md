# 01-数据类型与值 (Data Types & Values)

参考规范：**ES2021 Chapter 6**

ES 语言定义了两种核心类型层次：
1. **ECMAScript 语言类型（Language Types）：** 开发者在代码中能直接操作的类型。
2. **规范类型（Specification Types）：** 引擎内部用于描述语义的元类型（仅存在于规范中，不对外暴露）。

## 1. 语言类型 (Language Types)

QppJS 引擎的 `Value` 设计必须能表示以下所有的值：

- **Undefined:** 只有一个值 `undefined`。变量未初始化时的默认值。
- **Null:** 只有一个值 `null`。表示有意缺少的对象值。
- **Boolean:** `true` 和 `false`。
- **String:** 零个或多个 16 位无符号整数值（UTF-16 代码单元）组成的不可变序列。
- **Symbol:** 唯一且不可变的原始值，通常用于对象属性键。
- **Number:** 符合 IEEE 754 规范的双精度 64 位浮点数（包含 `NaN` 和 `±Infinity`）。
- **BigInt:** 任意精度的整数（ES2020 引入）。
- **Object:** 属性（Properties）的集合。每个属性分为数据属性（Data Property）或访问器属性（Accessor Property）。

## 2. 规范类型 (Specification Types)

引擎内部用来辅助运行的内部结构。QppJS 必须在 C++ 层实现与之对应的实体：

- **Reference (引用):** 用于解析像 `a.b` 或 `a` 这样的变量与属性赋值目标。包含基值（Base Value）、引用名称（Referenced Name）和严格模式标志（Strict Reference flag）。
- **List (列表):** 引擎内部的数组表示，例如用来存放函数调用时传入的参数。
- **Completion Record (完成记录):** 用于描述语句执行的最终状态（`Normal`, `Break`, `Continue`, `Return`, `Throw`），在 QppJS 的解释器中对应 `Completion` / `StmtResult` 结构。
- **Environment Record (环境记录):** 用于解析变量作用域，在 QppJS 中对应 `Environment` 类。
- **Property Descriptor (属性描述符):** 包含 `[[Value]]`, `[[Writable]]`, `[[Enumerable]]`, `[[Configurable]]` 等。
- **Data Block (数据块):** 用于表示 `ArrayBuffer` 和 `SharedArrayBuffer` 的底层原始二进制数据区。
