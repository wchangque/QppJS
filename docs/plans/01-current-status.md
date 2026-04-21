# QppJS 当前开发状态

本文件记录当前真实开发状态，是新 session 接续工作的首要入口。

## 1. 当前阶段

- 当前阶段：Phase 6 已全部完成，下一阶段为 Phase 7
- 最近更新时间：2026-04-22（Phase 6 Bytecode VM 完成）

## 2. 当前任务状态

### 已完成
- [x] 建立项目目标、参考仓库与 agent team 协作约定
- [x] 创建 6 个项目级 subagents：`es-spec`、`quickjs-research`、`design-agent`、`implementation-agent`、`testing-agent`、`review-agent`
- [x] 形成长期路线图、当前状态、下一阶段计划三文档体系
- [x] 明确当前状态更新机制以"任务完成"而不是"commit"作为主触发点
- [x] 0.1 建立最小目录结构
- [x] 0.2 建立最小构建链路（顶层 CMake 骨架）
- [x] 0.3 建立最小 CLI
- [x] 0.4 设计错误处理基础结构
- [x] 0.5 设计第一版 Value
- [x] 0.6 建立测试基线
- [x] 0.7 建立调试输出入口
- [x] 0.8 建立覆盖率报告链路（lcov + genhtml，HTML 行/分支覆盖率）

- [x] Phase 1：Lexer + Parser + AST（已全部完成）
  - [x] 1.1 实现 Tokenizer 基础结构（TokenKind、Token、SourceRange、SourceLocation、LexerState、next_token 最小实现）
  - [x] 1.2 支持基础词法元素（数字字面量、字符串字面量、多字符操作符）
  - [x] 1.3 设计 AST 节点体系（header-only，9 个表达式节点 + 6 个语句节点 + Program，variant 包装，overloaded helper）
  - [x] 1.4 实现表达式解析（Pratt Parser：原子/一元/二元/逻辑/赋值，含优先级与结合性）
  - [x] 1.5 实现语句解析（ExpressionStatement、VariableDeclaration、BlockStatement、IfStatement、WhileStatement、ReturnStatement + 最小 ASI）
  - [x] 1.6 实现 AST dump（dump_expr / dump_stmt / dump_program，缩进树形格式，17 个测试全部通过）
  - [x] 1.7 建立 parser 错误报告（make_parse_error 辅助函数，位置信息拼入 message，格式 "line N, column M: <描述>"，5 个新测试，242/242 全部通过）

- [x] Phase 2：AST Interpreter（已全部完成）
  - [x] 2.1 Environment / Scope（Binding struct，链式 Environment，VarKind define，TDZ，get/set/initialize）
  - [x] 2.2 表达式求值（NumberLiteral/StringLiteral/BooleanLiteral/NullLiteral/Identifier/UnaryExpression/BinaryExpression/LogicalExpression/AssignmentExpression）
  - [x] 2.3 语句执行（ExpressionStatement/VariableDeclaration/BlockStatement/IfStatement/WhileStatement/ReturnStatement + var 提升）
  - [x] 2.4 ToBoolean falsy 规则（undefined/null/false/0/NaN/""）
  - [x] 2.5 Completion 模型（CompletionType kNormal/kReturn，EvalResult/StmtResult，顶层 return 视为正常完成）
  - Parser 调整：允许顶层 return（移除 function_depth 检查），更新 3 个相关 parser 测试
  - CLI 更新：接入 Interpreter，parse + exec + format_value
  - 321/321 测试全部通过（含新增 65 个 interpreter 测试）
  - Bug 修复（Review + Testing Agent 审查后）：typeof TDZ 应抛 ReferenceError、let 无初始化值应为 undefined、字符串关系比较使用词典序；Testing Agent 补充 35 个边界测试；359/359 测试全部通过

- [x] Phase 3：Object Model（已全部完成）
- [x] Phase 4：Function（已全部完成）
  - [x] 4.1 Environment outer_ 改为 shared_ptr，Interpreter 增加 var_env_、call_depth_
  - [x] 4.2 AST 扩展（FunctionDeclaration/FunctionExpression/CallExpression）
  - [x] 4.3 JSFunction 类（ObjectKind::kFunction，private 成员，accessor 访问）
  - [x] 4.4 Parser 扩展（lbp(LParen)=16，nud KwFunction，led LParen，parse_function_params/body）
  - [x] 4.5 hoist_vars 修正（var_target 参数，var 提升到函数作用域）
  - [x] 4.6 eval_call_expr/eval_function_decl/eval_function_expr（RAII call_depth_，闭包，递归）
  - [x] 4.7 AST dump 扩展（FunctionDeclaration/FunctionExpression/CallExpression）
  - Bug 修复（Review Agent 审查后）：eval_function_decl 改为 var_env_->set()、eval_member_expr/assign 中 assert 改为 if + 返回 undefined/TypeError、call_depth_ 纳入 ScopeGuard RAII 管理
  - 475/475 测试全部通过（原有 417 个 + 新增 58 个函数测试）

- [x] Phase 5：原型链、this、new（已全部完成）
  - [x] 5.1 JSObject proto_ 字段 + 原型链查找 + object_prototype_
  - [x] 5.2 JSFunction prototype_ 字段 + 急切初始化（make_function_value）
  - [x] 5.3 this 关键字支持（KwThis token）+ ScopeGuard 扩展（saved_this/new_this）
  - [x] 5.4 方法调用 this 提取 + call_function 抽取（返回 StmtResult）
  - [x] 5.5 NewExpression AST + Parser + eval_new_expr
  - Bug 修复（Review Agent P1-1）：call_function 返回 StmtResult 区分显式 return object 与自然完成
  - 531/531 测试全部通过（原有 475 个 + 新增 56 个原型/this/new 测试）

- [x] Phase 6：Bytecode VM（已全部完成）
  - [x] 6.1 Opcode 枚举（49 条，X-Macro）+ BytecodeFunction 结构体
  - [x] 6.2 Compiler 框架 + 字面量 + 算术表达式 + VM 骨架
  - [x] 6.3 变量、作用域、控制流（let/const/var、BlockStatement、if/while、logical）
  - [x] 6.4 函数声明与调用（闭包、递归、var 提升、函数声明提升）
  - [x] 6.5 对象、属性、方法调用、this、new（含 JSFunction.prototype 读写）
  - [x] 6.6 typeof 特殊处理（TypeofVar 指令，未声明变量安全返回 "undefined"）
  - [x] 6.7 全量 VM 测试（134 个测试）+ main.cpp --vm flag
  - 529（interpreter）+ 134（VM）= 663 个测试全部通过

### 进行中
- 暂无（Phase 6 已全部完成）

### 未开始
- [ ] Phase 7：控制流扩展（break/continue/throw/try/catch/finally）

### 阻塞
- 暂无

## 3. 最近完成内容

- 已完成 Phase 6 Bytecode VM：
  - `include/qppjs/vm/opcode.h`（新建）：49 条指令，X-Macro 形式
  - `include/qppjs/vm/bytecode.h`（新建）：BytecodeFunction 结构体
  - `include/qppjs/vm/compiler.h`（新建）：Compiler 类声明
  - `src/vm/compiler.cpp`（新建）：完整编译器，含 compile_stmt_last、hoist_vars_scan、方法调用 Dup 模式
  - `include/qppjs/vm/vm.h`（新建）：CallFrame + VM 类声明（flat dispatch loop 设计）
  - `src/vm/vm.cpp`（新建）：完整 VM，含 JSFunction 特殊处理（GetProp/SetProp），NewCall，flat run()
  - `include/qppjs/runtime/environment.h`：添加 outer() accessor
  - `include/qppjs/runtime/js_function.h`：添加 bytecode_ 字段
  - `src/main/main.cpp`：添加 --vm flag 切换执行路径
  - `src/CMakeLists.txt`、`tests/CMakeLists.txt`：注册新文件
  - `tests/unit/vm_test.cpp`（新建）：134 个 VM 路径行为测试
  - 663/663 测试全部通过

## 4. 当前风险与待决策项

- JSFunction::body_ 字段继续保留（AST Interpreter 使用）
- VM 尚不支持 break/continue/throw/try/catch（Phase 7）
- GetProp/SetProp 对 JSFunction 的非 prototype 属性当前静默忽略

## 5. 下次进入点

新 session 开始时，优先做以下动作：
1. 读取本文件
2. 读取 `docs/plans/02-next-phase.md`
3. Phase 6 所有任务已完成，进入 Phase 7：控制流扩展

## 6. 收尾检查清单

每次任务结束前，至少检查：
- 当前任务状态是否变化
- 本文件是否已同步
- `docs/plans/02-next-phase.md` 是否仍正确
- 若阶段计划已调整，`docs/plans/00-roadmap.md` 是否已同步
