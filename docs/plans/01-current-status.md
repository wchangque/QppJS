# QppJS 当前开发状态

本文件记录当前真实开发状态，是新 session 接续工作的首要入口。

## 1. 当前阶段

- 当前阶段：Phase 6 已全部完成，下一阶段为 Phase 7
- 最近更新时间：2026-04-23（构建系统脚本已收缩为固定职责入口，CMake option 已统一收口到 `cmake/Options.cmake`，全局编译/链接选项改为根 `CMakeLists.txt` 统一注入并在 `CompilerFlags.cmake` 直接用 `set/list(APPEND)` 组装，UT 脚本已切到 debug 构建链路并与 CLI 测试分离，macOS 覆盖率链路已验证通过；当前正在收尾函数/闭包路径的 ASAN/LSan 泄露修复，已完成 Cell 化绑定与 interpreter/vm 捕获模型切换，单例泄露样例已消失，但函数相关批量回归仍需继续收敛）

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
- [x] 构建系统重构：完成 CMake 公共配置收口（`cmake/Options.cmake` 统一管理 options，根 `CMakeLists.txt` 通过 `QPPJS_GLOBAL_COMPILER_OPTIONS` + `add_compile_options()` 注入全局编译选项，按需通过 `add_link_options()` 注入全局链接选项）、构建 metadata 导出、移除 `CMakePresets.json`、构建脚本收缩为 `build_release.sh` / `build_debug.sh` / `build_test.sh` / `run_ut.sh` / `coverage.sh` 固定入口

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
- 函数/闭包 ASAN 泄露修复：已将 `Environment::Binding` 改为共享 `Cell`，`JSFunction` 从 `closure_env` 切到 `captured_bindings`，Interpreter/VM 已能编译并通过单个泄露样例 `FunctionTest.BasicFunctionDeclarationAndCall`；但递归/互递归/兄弟函数互引场景仍存在残余泄露，尚未完成 focused function/vm 回归

### 未开始
- [ ] Phase 7：控制流扩展（break/continue/throw/try/catch/finally）

### 阻塞
- 暂无

## 3. 最近完成内容

- 正在收尾函数/闭包路径的 ASAN/LSan 泄露修复：
  - `include/qppjs/runtime/environment.h`、`src/runtime/environment.cpp`：`Binding` 改为共享 `Cell` 持值，新增 `BindingMap` 与 `define_binding()`
  - `include/qppjs/runtime/js_function.h`：移除 `closure_env_`，改为 `captured_bindings_`
  - `include/qppjs/runtime/interpreter.h`、`src/runtime/interpreter.cpp`：新增可见绑定收集，函数调用改为“全局环境 + 捕获绑定 + 参数/局部绑定”模型
  - `include/qppjs/vm/vm.h`、`src/vm/vm.cpp`：同步对齐为 `captured_bindings` 闭包模型，并补上 VM 侧 `global_env_`
  - 当前验证状态：debug + ASAN 构建恢复可编译，`FunctionTest.BasicFunctionDeclarationAndCall` 已不再触发泄露；但 focused function/vm 批量回归仍有残余泄露，根因进一步收敛到递归/互递归函数通过 captured binding 自引用形成的新强环，尚未完全修复

- 已完成构建系统重构收尾验证与脚本职责收缩：
  - 移除 `CMakePresets.json`
  - 构建入口收缩为五个固定脚本：`scripts/build_release.sh`、`scripts/build_debug.sh`、`scripts/build_test.sh`、`scripts/run_ut.sh`、`scripts/coverage.sh`
  - `scripts/build_release.sh`：固定构建 `build/release`，Release，关闭 `QPPJS_BUILD_TESTS`
  - `scripts/build_debug.sh`：固定构建 `build/debug`，Debug，开启 `QPPJS_BUILD_TESTS` 与 `QPPJS_ENABLE_ASAN`
  - `scripts/build_test.sh`：固定构建 `build/test`，Debug，开启 `QPPJS_BUILD_TESTS` 与 `QPPJS_ENABLE_COVERAGE`；warning 默认视为 error，不再提供单独关闭开关
  - `scripts/run_ut.sh`：先调用 `build_debug.sh`，再以 `ctest --test-dir build/debug -E '^qppjs_cli_'` 仅执行 UT（排除 2 个 CLI 测试）
  - `scripts/coverage.sh`：先调用 `build_test.sh`，再执行 UT，并基于 build metadata 收集覆盖率
  - 历史 macOS 验证结论保持有效：Homebrew LLVM clang 调试构建 665/665 测试通过，LeakSanitizer 已激活且无泄露；覆盖率报告端到端生成成功（行 82.0%、函数 93.9%、分支 73.2%），`--open` 可打开浏览器

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

- 当前最高优先级风险：函数/闭包 captured binding 模型仍有残余 shared_ptr 环，主要集中在递归、互递归与兄弟函数互引场景；Phase 7 前应先完成该问题收尾并恢复函数相关 ASAN 回归
- 需确认原生 CMake 工作流在 Windows/MSVC 多配置生成器上 configure/build/test 不回归
- macOS 若要继续使用 Homebrew LLVM + libc++ / LSan，需要在原生 CMake 命令里显式传 `CMAKE_CXX_COMPILER` 与相关 flags；不再由 preset 自动选中
- JSFunction::body_ 字段继续保留（AST Interpreter 使用）
- VM 尚不支持 break/continue/throw/try/catch（Phase 7）
- GetProp/SetProp 对 JSFunction 的非 prototype 属性当前静默忽略

## 5. 下次进入点

新 session 开始时，优先做以下动作：
1. 读取本文件
2. 读取 `docs/plans/02-next-phase.md`
3. 先继续收尾函数/闭包路径的 ASAN/LSan 泄露修复，重点检查递归/互递归与兄弟函数互引场景
4. 函数相关 ASAN 回归恢复后，再进入 Phase 7：控制流扩展

## 6. 收尾检查清单

每次任务结束前，至少检查：
- 当前任务状态是否变化
- 本文件是否已同步
- `docs/plans/02-next-phase.md` 是否仍正确
- 若阶段计划已调整，`docs/plans/00-roadmap.md` 是否已同步
