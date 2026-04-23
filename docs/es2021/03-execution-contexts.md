# 03-执行上下文与作用域 (Execution Contexts)

参考规范：**ES2021 Chapter 8 & Chapter 9**

此部分是 JavaScript 引擎核心状态机的抽象。QppJS 的虚拟机和解释器必须维护这些上下文。

## 1. 执行上下文 (Execution Context)

执行上下文用于跟踪 ECMAScript 代码的运行时评估（即栈帧）。

执行上下文包含的组件：
- **LexicalEnvironment:** 词法环境（识别由 `let` / `const` 创建的标识符）。
- **VariableEnvironment:** 变量环境（识别由 `var` 创建的标识符）。
- **PrivateEnvironment:** 私有环境（ES2022，用于类的私有字段 `#field`）。
- **Realm:** 全局环境及其内置对象的容器（在 iframe 场景下会存在多个 Realm）。
- **ScriptOrModule:** 当前执行的代码来源。
- **Function:** 如果是函数执行上下文，指向对应的函数对象（用于获取函数名、长度等元信息）。

引擎内部需要维护一个 **执行上下文栈 (Execution Context Stack)**。调用函数时 Push，返回时 Pop。

## 2. 词法环境 (Lexical Environments)

词法环境是作用域（Scope）的规范定义。它由两部分组成：
1. **环境记录 (Environment Record):** 实际存储变量绑定的字典。
2. **外部词法环境引用 (Outer Lexical Environment Reference):** 指向外层作用域（形成作用域链），如果是全局环境则为 `null`。

### 环境记录类型：
- **声明式环境记录 (Declarative Environment Record):** 用于函数作用域、块作用域（`let`/`const`）和 `catch` 块。
- **对象式环境记录 (Object Environment Record):** 用于 `with` 语句和顶层全局环境（将 `window` / `globalThis` 的属性作为变量）。
- **全局环境记录 (Global Environment Record):** 声明式和对象式的复合体，并管理全局的 `this`。
- **函数环境记录 (Function Environment Record):** 声明式的子类，额外包含 `this` 绑定状态、`super` 以及 `new.target` 状态。

## 3. Realm (领域)

在执行任何 JS 之前，必须先创建一个 Realm。
Realm 包含：
- 全局环境记录（Global Environment）
- 全局对象（Global Object）
- 所有内置对象（如 `Array.prototype`, `Object.prototype` 等）的原始副本。
