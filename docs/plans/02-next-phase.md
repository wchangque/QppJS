# QppJS 下一阶段计划

本文件展开"下一阶段"的可执行内容，是从当前状态进入下一轮开发的直接行动卡。

## 1. 下一阶段

- 下一阶段：Phase 7：异常与复杂控制流（break/continue/throw/try/catch/finally）
- 对应路线图：`docs/plans/00-roadmap.md`
- 当前事实源：`docs/plans/01-current-status.md`

## 2. 阶段目标

在 AST Interpreter 和 Bytecode VM 上同步支持以下特性：
- `break` / `continue` 语句（含 labeled）
- `throw` 语句
- `try / catch / finally` 语句
- `Error` 对象（`new Error(msg)`，基础 built-in）

## 3. 进入前提

当前已具备：
- 完整 Lexer、Parser、AST（Phase 1）
- AST Interpreter，含 Environment/Scope、表达式求值、语句执行（Phase 2）
- Object Model（Phase 3）
- Function（Phase 4）：闭包、递归、var 提升
- 原型链、this、new（Phase 5）
- Bytecode VM（Phase 6）：49 条指令，含函数调用、对象属性、原型链
- 665/665 测试全部通过（529 Interpreter + 134 VM + 2 CLI）
- macOS 覆盖率报告端到端可生成（Homebrew LLVM clang 覆盖率构建，行 82%，函数 94%，分支 73%）
- LeakSanitizer 已打开且无泄露（Homebrew LLVM clang 调试构建）

## 4. 设计方向（待 agent team 细化）

以下是初步方向，需要 ES Spec Agent + QuickJS Research Agent + Design Agent 完成后再细化。

### 4.1 AST Interpreter 侧

- `CompletionType` 扩展：新增 `kBreak`、`kContinue`、`kThrow`
- `StmtResult` 携带 label（break/continue with label）和 thrown value（throw）
- `try / catch / finally` 执行语义：finally 必须执行，catch 捕获 thrown value
- `break` / `continue` 在 while/for 内的穿透与截断

### 4.2 Bytecode VM 侧

新增指令（初步，需 Design Agent 确认）：
- `Throw`：弹出栈顶值，抛出异常
- `EnterTry(offset)`：注册 catch 处理器，offset 指向 catch 块入口
- `LeaveTry`：退出 try 块（正常路径）
- `EnterFinally(offset)`：注册 finally 处理器
- `LeaveFinally`：退出 finally 块，恢复挂起的控制流
- `Break(label_id)` / `Continue(label_id)`：带 label 的跳出

### 4.3 Error 内建对象

- `Error` 构造函数：`new Error(msg)` 创建带 `message` 属性的对象
- `error.message`、`error.name` 属性
- 暂不实现 stack trace

## 5. 本阶段任务分解

### 7.1 规范调研 + 设计

目标：
- ES Spec Agent 输出 `throw` / `try/catch/finally` / `break` / `continue` 的语义规则与边界条件
- QuickJS Research Agent 输出 QuickJS 在异常处理和控制流上的实现取舍
- Design Agent 综合产出 QppJS Phase 7 实现方案（含新增 AST 节点、Completion 模型扩展、VM 新增指令集）

### 7.2 AST 扩展 + Parser

目标：
- 新增 `ThrowStatement`、`TryStatement`（含 catch/finally 子节点）、`BreakStatement`、`ContinueStatement`、`LabeledStatement` AST 节点
- Parser 支持解析上述语句
- AST dump 扩展
- 项目可编译，无运行时测试

### 7.3 AST Interpreter 实现

目标：
- `CompletionType` 扩展（kBreak/kContinue/kThrow）
- `eval_throw_stmt`、`eval_try_stmt`、`eval_break_stmt`、`eval_continue_stmt`
- while/for 循环内正确截断 break/continue
- finally 块无论何种完成类型都必须执行
- 完成后：interpreter 侧相关测试全部通过

### 7.4 VM 编译器 + 指令集扩展

目标：
- 新增 Throw、EnterTry、LeaveTry、EnterFinally、LeaveFinally 等指令
- Compiler 实现 compile_throw、compile_try、compile_break、compile_continue
- VM 实现对应指令的 dispatch 逻辑
- 完成后：VM 侧相关测试全部通过

### 7.5 Error 内建对象

目标：
- 注册全局 `Error` 构造函数（支持 `new Error(msg)`）
- `error.message` 属性可读
- `error.name` 返回 `"Error"`
- 在 Interpreter 和 VM 两侧均可使用

### 7.6 全量测试 + 回归验证

目标：
- 补充 Phase 7 专项测试（throw/try/catch/finally/break/continue/Error）
- 原有 665 个测试无回归
- 覆盖率报告更新

## 6. 建议执行顺序

7.1 → 7.2 → 7.3 → 7.4 → 7.5 → 7.6，每完成一个子任务跑全量测试确认无回归。

## 7. 验证样例

Phase 7 完成后，应至少能验证：

```js
// throw + catch
function divide(a, b) {
    if (b === 0) throw new Error("division by zero");
    return a / b;
}
try {
    divide(1, 0);
} catch (e) {
    e.message  // → "division by zero"
}

// finally
function f() {
    try { return 1; } finally { /* must execute */ }
}
f()  // → 1

// break/continue
let sum = 0;
for (let i = 0; i < 10; i++) {
    if (i === 5) break;
    sum = sum + i;
}
sum  // → 10

// nested try
try {
    try { throw new Error("inner"); }
    catch (e) { throw new Error("rethrow"); }
} catch (e) {
    e.message  // → "rethrow"
}
```

## 8. 暂不处理内容

- `for` 循环（Phase 7 以 while 为主，for 可选）
- `for...in` / `for...of`
- generator / iterator
- 完整 Error 子类（TypeError、RangeError 等）—— Phase 8 处理
- stack trace

## 9. 退出条件

满足以下条件即可进入 Phase 8：
- throw/try/catch/finally/break/continue 在 Interpreter 和 VM 两侧均通过测试
- `new Error(msg)` 可用，`error.message` 可读
- 原有 665 个测试无回归
- 状态文档已同步
