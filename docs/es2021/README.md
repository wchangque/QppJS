# ECMAScript 2021 (ES12) 规范开发手册

本文档库为 QppJS 引擎开发团队整理的 ES2021 语言规范核心模块导读。通过将庞大繁杂的官方规范（ECMA-262）按照引擎开发逻辑分类，协助我们快速查阅并正确实现对应功能。

## 目录结构

- [01-数据类型与值 (Data Types & Values)](./01-data-types.md)
  涵盖 JavaScript 中的基础类型：Undefined, Null, Boolean, String, Symbol, Number, BigInt, Object。对应规范 **Chapter 6**。

- [02-抽象操作 (Abstract Operations)](./02-abstract-operations.md)
  核心隐式转换与内部行为：`ToPrimitive`, `ToBoolean`, `ToNumber`, `ToObject` 等。对应规范 **Chapter 7**。

- [03-执行上下文与作用域 (Execution Contexts & Lexical Environments)](./03-execution-contexts.md)
  引擎运行机制：执行上下文栈、词法环境（作用域链）、Realm。对应规范 **Chapter 8 & Chapter 9**。

- [04-语法与表达式 (Syntax, Expressions & Statements)](./04-syntax-and-semantics.md)
  语言词法法法规则、表达式求值逻辑、控制流语句（if, for, return, try-catch）。对应规范 **Chapters 11 - 15**。

- [05-内置对象 (Standard Built-in Objects)](./05-built-in-objects.md)
  标准库实现：Global 对象、`Object`、`Function`、`Array`、`String`、`Promise` 等。对应规范 **Chapters 18 - 28**。

---
> 💡 **引擎实现原则：**
> - QppJS 内部的 C++ 函数命名应尽量贴合规范的 Abstract Operations 名称（例如：将类型转换方法命名为 `ToNumber()`、`ToPrimitive()`）。
> - 内部操作使用 `[[ ]]` 包裹（如 `[[Get]]`、`[[Prototype]]`），这也是 QppJS 设计 `JSObject` 虚表时的核心参考依据。
