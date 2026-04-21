# QppJS 下一阶段计划

本文件展开"下一阶段"的可执行内容，是从当前状态进入下一轮开发的直接行动卡。

## 1. 下一阶段

- 下一阶段：Phase 4：Function
- 对应路线图：`docs/plans/00-roadmap.md`
- 当前事实源：`docs/plans/01-current-status.md`

## 2. 阶段目标

在已具备完整 Object Model 的基础上，引入函数定义与调用：函数声明/函数表达式、调用表达式、参数绑定、return 语义，为后续闭包和作用域链打基础。

## 3. 进入前提

当前已具备：
- 完整 Lexer（1.1 + 1.2）
- 完整 AST 节点体系（1.3）
- Pratt Parser，支持表达式 + 语句（1.4 + 1.5）
- AST dump（1.6）
- Parser 错误报告，含位置信息（1.7）
- AST Interpreter（Phase 2）：Environment/Scope、表达式求值、语句执行、ToBoolean、Completion 模型
- Object Model（Phase 3）：JSObject、属性读写、对象字面量、成员访问（点号/方括号）、成员赋值
- 387/387 测试全部通过

## 4. 本阶段任务分解

### 4.1 扩展 AST：函数定义与调用

目标：
- `FunctionDeclaration`（`function name(params) { body }`）
- `FunctionExpression`（`function(params) { body }` 或具名）
- `CallExpression`（`callee(args)`）

### 4.2 扩展 Value：JSFunction

目标：
- `JSFunction` 类（继承 Object）：保存参数名列表、函数体 AST、词法环境（closure）
- `ObjectKind::kFunction` 枚举值
- `typeof fn` → `"function"`

### 4.3 扩展 Parser

目标：
- `function` 关键字处理：既可作为声明语句，也可作为前缀表达式（函数表达式）
- 调用表达式 `callee(args)`：`(` 作为中缀操作符（lbp 高于成员访问），led 分支处理参数列表
- 参数列表：逗号分隔的表达式列表

### 4.4 扩展 Interpreter

目标：
- `eval_function_decl`：创建 JSFunction，绑定到当前环境
- `eval_call_expr`：求值 callee，检查是否为函数；创建新 Environment（父为闭包环境），绑定参数，执行函数体，处理 return
- 简单的调用栈保护（最大深度限制，防止无限递归）

## 5. 建议执行顺序

1. 4.1 AST 扩展（FunctionDeclaration/FunctionExpression/CallExpression）
2. 4.2 JSFunction（ObjectKind::kFunction，保存 params/body/closure）
3. 4.3 Parser 扩展（function 关键字，调用表达式）
4. 4.4 Interpreter 扩展（函数创建/调用/参数绑定）

## 6. 验证方式

本阶段完成后，应至少能验证：
- `function add(a, b) { return a + b; } add(1, 2)` → `3`
- `let f = function(x) { return x * 2; }; f(5)` → `10`
- `typeof add` → `"function"`
- 函数内局部变量不泄露到外部

## 7. 暂不处理内容

- `arguments` 对象
- 箭头函数（`=>`）
- 默认参数 / rest 参数
- `this` 绑定（Phase 5）
- 原型链与方法调用（Phase 5）
- 递归超出深度的具体错误类型

## 8. 退出条件

满足以下条件即可进入 Phase 5：
- 函数声明和函数表达式可定义并调用
- 参数正确绑定
- return 语句在函数内正确终止并返回值
- 闭包可读取外部变量
- 新增测试覆盖上述场景，全部通过
