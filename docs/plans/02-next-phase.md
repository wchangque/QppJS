# QppJS 下一阶段计划

本文件展开"下一阶段"的可执行内容，是从当前状态进入下一轮开发的直接行动卡。

## 1. 下一阶段

- 下一阶段：Phase 2：AST Interpreter
- 对应路线图：`docs/plans/00-roadmap.md`
- 当前事实源：`docs/plans/01-current-status.md`

## 2. 阶段目标

在已具备完整 Lexer + Parser + AST 的基础上，先在 AST 层完成最小语义实现，避免过早进入 VM 设计。能解释执行简单脚本，处理变量、作用域和基础控制流。

## 3. 进入前提

当前已具备：
- 完整 Lexer（1.1 + 1.2）
- 完整 AST 节点体系（1.3）
- Pratt Parser，支持表达式 + 语句（1.4 + 1.5）
- AST dump（1.6）
- Parser 错误报告，含位置信息（1.7）
- 242/242 测试全部通过

## 4. 本阶段任务分解

### 2.1 设计 Environment / Scope
目标：
- 全局环境
- 块级作用域（`let` / `const` 的块绑定）
- 变量查找与赋值（沿作用域链向上查找）

关键决策：
- 用链式 `Environment` 对象表示作用域嵌套，每个 env 持有 parent 指针
- `var` 提升到函数/全局作用域，`let` / `const` 绑定到当前块

### 2.2 实现表达式求值
目标：
- `NumberLiteral` / `StringLiteral` / `BooleanLiteral` / `NullLiteral`
- `Identifier`（变量读取）
- `UnaryExpression`（`-` `+` `!` `typeof` `void`）
- `BinaryExpression`（算术、比较、相等）
- `LogicalExpression`（`&&` `||`，短路求值）
- `AssignmentExpression`（`=` 及复合赋值）

### 2.3 实现语句执行
目标：
- `ExpressionStatement`
- `VariableDeclaration`（`let` / `const` / `var`）
- `BlockStatement`（创建新 scope）
- `IfStatement`（含 `else`）
- `WhileStatement`
- `ReturnStatement`（暂存结果，为后续函数调用预留）

### 2.4 实现 truthy / falsy 规则
目标：按 ECMAScript 规范实现 `ToBoolean`：
- falsy：`undefined` `null` `false` `0` `NaN` `""`
- 其余均为 truthy

### 2.5 设计最小 Completion 模型
目标：
- 正常完成（Normal）
- 表达式语句的结果值
- `return` 值的传递（为 Phase 4 函数调用预留结构）
- 暂不处理 `throw` / `break` / `continue`

## 5. 建议执行顺序

1. 2.1 Environment / Scope（基础，后续全部依赖）
2. 2.4 truthy / falsy（独立，可与 2.2 并行，但先定义清楚）
3. 2.2 表达式求值（依赖 2.1）
4. 2.5 Completion 模型（配合 2.2 一起确定）
5. 2.3 语句执行（依赖 2.1 + 2.2 + 2.4 + 2.5）

## 6. 验证方式

本阶段完成后，应至少能验证：
- `let x = 1 + 2; x` 求值为 `3`
- `if (x > 2) { x = 10; } x` 求值为 `10`
- `let i = 0; while (i < 3) { i = i + 1; } i` 求值为 `3`
- `const x = 1; x = 2` 报运行时错误
- 块级作用域隔离：内层 `let` 不泄漏到外层

## 7. 暂不处理内容

- 函数声明与调用（Phase 4）
- 对象字面量与属性访问（Phase 3）
- 原型链（Phase 5）
- `throw` / `try` / `catch`（Phase 7）
- VM / 字节码（Phase 6）
- test262 接入

## 8. 退出条件

满足以下条件即可进入 Phase 3：
- Environment / Scope 可正确处理块级绑定与作用域链查找
- 基础表达式求值正确（含 truthy / falsy）
- 基础语句执行正确（含控制流）
- Completion 模型已定义，`return` 值可传递
- 新增测试覆盖上述场景，全部通过
