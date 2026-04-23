# 02-抽象操作 (Abstract Operations)

参考规范：**ES2021 Chapter 7**

抽象操作是引擎必须实现的一系列“内部工具函数”。JS 中的弱类型转换（如 `1 + "2"`）完全由这些抽象操作定义。

在 QppJS 中，这些操作应直接映射为 C++ 函数。

## 1. 类型转换 (Type Conversion)

- `ToPrimitive ( input [ , preferredType ] )`: 将对象转换为原始值（调用 `valueOf` 或 `toString`）。
- `ToBoolean ( argument )`: 转换为布尔值（Falsy 值：`undefined`, `null`, `false`, `+0`, `-0`, `NaN`, `""`，其余全为 `true`）。
- `ToNumeric ( value )`: 转换为 Number 或 BigInt。
- `ToNumber ( argument )`: 转换为 Number。
- `ToIntegerOrInfinity ( argument )`: 转换为整数或无穷大。
- `ToInt32 ( argument )`: 转换为 32 位整数（常用于位运算）。
- `ToString ( argument )`: 转换为字符串。
- `ToObject ( argument )`: 将原始类型包装为对象（`undefined` 和 `null` 会抛出 TypeError）。
- `ToPropertyDescriptor ( Obj )`: 将对象转换为属性描述符（用于 `Object.defineProperty`）。
- `ToPropertyKey ( argument )`: 将值转换为属性键（String 或 Symbol）。

## 2. 测试与比较 (Testing and Comparison)

- `RequireObjectCoercible ( argument )`: 如果参数是 `undefined` 或 `null`，抛出 TypeError。
- `IsCallable ( argument )`: 判断对象是否有 `[[Call]]` 内部方法。
- `IsConstructor ( argument )`: 判断对象是否有 `[[Construct]]` 内部方法。
- `IsExtensible ( O )`: 判断对象是否可以添加新属性。
- `IsArray ( argument )`: 判断是否为数组对象。
- `SameValue ( x, y )`: `Object.is` 的内部实现，`NaN` 等于 `NaN`，`+0` 不等于 `-0`。
- `SameValueZero ( x, y )`: Set 和 Map 中使用的比较，`NaN` 等于 `NaN`，`+0` 等于 `-0`。
- `Strict Equality Comparison ( x === y )`: `===` 的内部实现。
- `Abstract Equality Comparison ( x == y )`: `==` 的内部实现，包含复杂的隐式类型转换逻辑。
- `SameValueNonNumeric ( x, y )`: 用于比较 BigInt 或 Symbol。
- `CompareArrayElements ( x, y, comparefn )`: 用于数组排序的比较抽象操作。

## 3. 对象内部方法 (Object Internal Methods)

所有对象（无论是普通对象还是代理、数组）在底层都有如下操作（对应 C++ 里的虚函数/接口）：

- `[[GetPrototypeOf]] ()`
- `[[SetPrototypeOf]] (V)`
- `[[IsExtensible]] ()`
- `[[PreventExtensions]] ()`
- `[[GetOwnProperty]] (P)`
- `[[DefineOwnProperty]] (P, Desc)`
- `[[HasProperty]] (P)`
- `[[Get]] (P, Receiver)`
- `[[Set]] (P, V, Receiver)`
- `[[Delete]] (P)`
- `[[OwnPropertyKeys]] ()`
- `[[Call]] (thisArgument, argumentsList)` (仅函数有)
- `[[Construct]] (argumentsList, newTarget)` (仅构造器有)
