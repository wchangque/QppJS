# 04-语法与表达式 (Syntax & Semantics)

参考规范：**ES2021 Chapters 11 - 15**

定义了 JavaScript 代码被解析成 AST（抽象语法树）后的求值语义（Evaluation Semantics）。

## 1. 表达式求值 (Expressions)

在实现 AST 解释器或字节码编译器时，每种表达式都有严格的求值顺序要求。

- **属性访问 (`a.b` 或 `a[b]`):** 求值结果是一个 `Reference` 规范类型，包含 `base` 为 `a` 和 `name` 为 `b`。
- **赋值 (`=`):** `GetValue` -> 计算右侧值 -> `PutValue` 写入左侧 Reference。
- **二元运算 (`+`, `-`, `*`, 等):** 
  - 先算左边 `GetValue`
  - 再算右边 `GetValue`
  - 如果是 `+` 且包含 String 则执行字符串拼接；否则转为 Number/BigInt 做算术。
- **逻辑运算 (`&&`, `||`, `??`):** 具有短路特性，右侧可能不求值。

## 2. 语句 (Statements)

语句的执行结果必须返回 `Completion Record`。

- **If Statement:** 根据条件表达式的 `ToBoolean` 结果选择分支。
- **Iteration (for / while):** 需要处理 `Break` 和 `Continue` completion 记录。
- **Return Statement:** 产生一个 `Return` completion 记录，并携带返回值。
- **Try...Catch...Finally:** 捕获 `Throw` completion 记录，如果是正常执行，则进入 finally；如果是 throw，则解包 error 对象传入 catch 的词法环境中。
- **Switch Statement:** 支持字面量和表达式匹配，使用 `Strict Equality Comparison` 进行 case 匹配。需正确处理 Fall-through 和 `default` 分支。
- **Throw Statement:** 显式抛出异常，生成 `Throw` completion 记录。

## 3. 函数定义 (Functions)

规范定义了多种函数，它们的内部初始化逻辑有所不同：
- 普通函数 (`function f() {}`)
- 箭头函数 (`() => {}`): 没有自己的 `this`，继承外层环境。
- 异步函数 (`async function`): 隐式返回 Promise。
- 生成器函数 (`function*`): 返回 Iterator。

## 4. 变量声明与作用域规则

- **var 声明:** 函数作用域，存在变量提升（Hoisting），初始化为 `undefined`。
- **let / const 声明:** 块级作用域，存在暂时性死区（Temporal Dead Zone, TDZ），在声明前访问会抛出 `ReferenceError`。
- **函数声明:** 存在整体提升（包括函数体）。
- **类声明:** 类似 `let`，存在 TDZ，不会提升。

## 5. ES2021 新特性 (ES2021 Specific Features)

- **Logical Assignment Operators (`??=`, `||=`, `&&=`):** 逻辑赋值运算符，仅在左侧满足条件时才赋值。
- **Numeric Separators (`1_000_000`):** 数字分隔符，允许在数字字面量中使用下划线 `_` 增加可读性。
- **String.prototype.replaceAll:** 字符串全局替换方法。
- **Promise.any / AggregateError:** 返回第一个成功的 Promise，全部失败时抛出 `AggregateError`。
- **WeakRef / FinalizationRegistry:** 弱引用对象和清理回调注册（用于实现缓存等场景）。
