# QppJS 当前开发状态

本文件记录当前真实开发状态，是新 session 接续工作的首要入口。

## 1. 当前阶段

- 当前阶段：Phase 7 已完成，可进入 Phase 8
- 最近更新时间：2026-04-23（P1-1 缺陷修复完成：compile_return_stmt 增加 finally 穿越逻辑，2 个 DISABLED 测试启用，792/792 全量通过）

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

### 已完成（续）
- [x] Phase 7：控制流扩展（break/continue/throw/try/catch/finally）
  - [x] 7.1 规范调研 + 设计
  - [x] 7.2 AST 节点扩展 + Parser 扩展
  - [x] 7.3 AST Interpreter 实现（CompletionType 扩展、6 个新 eval 方法、NativeFn + Error 内建）
  - [x] 7.4 VM 编译器扩展（本轮完成）：6 条新 opcode、Compiler 实现、VM dispatch 扩展、Error 内建 VM 侧注册
  - [x] 7.5 Error 内建对象（Interpreter 7.3 已含，VM 7.4 补充）
  - [x] 7.6 全量测试回归（736/736 通过）
  - [x] 7.7 边界测试补充（Testing Agent）：新增 56 个边界测试，790/790 通过（原 736 + 新增 56），2 个已知 VM 缺陷（return 穿越 finally）标注 DISABLED
  - [x] 7.8 P1-1 缺陷修复：compile_return_stmt 增加 finally_info_stack_ 穿越逻辑（LeaveTry + Gosub），792/792 全量通过

### 未开始
- [ ] Phase 8（待规划）

### 阻塞
- 暂无

## 3. 最近完成内容

- 完成 Phase 7 P1-1 缺陷修复（7.8）：
  - `src/vm/compiler.cpp`：`compile_return_stmt` 在 `finally_info_stack_` 非空时，对每个活跃 finally 从内到外 emit `kLeaveTry` + `kGosub`（patch 位置记录到 `gosub_patches`，由 `compile_try_stmt` 统一 patch），最后 emit `kReturn`；无参 return 情形先 emit `kLoadUndefined` 再走相同路径
  - `tests/unit/vm_phase7_edge_test.cpp`：去掉 `DISABLED_` 前缀启用 `FinallyReturnOverridesTryReturn` 和 `FinallyNormalSideEffectWithTryReturn` 两个测试
  - 792/792 全量通过（原 790 + 新启用 2，0 回归）

- 完成 Phase 7 子任务 7.4：VM 编译器扩展：
  - `include/qppjs/vm/opcode.h`：新增 6 条指令（Throw/EnterTry/LeaveTry/GetException/Gosub/Ret）
  - `include/qppjs/vm/vm.h`：CallFrame 新增 ExceptionHandler 结构体、handler_stack/pending_throw/caught_exception/finally_return_stack/scope_depth 字段
  - `include/qppjs/vm/compiler.h`：新增 LoopEnv/FinallyInfo 结构体、loop_env_stack_/finally_info_stack_ 字段、6 个新 compile_* 方法声明、patch_jump_to/emit_jump_to/current_offset 辅助方法
  - `src/vm/compiler.cpp`：
    - 新增 patch_jump_to/emit_jump_to/current_offset
    - hoist_vars_scan_stmt 扩展（ForStatement/TryStatement/LabeledStatement）
    - compile_while_stmt 重构（接入 LoopEnv）
    - compile_throw_stmt/compile_try_stmt（三分支：try+catch、try+finally、try+catch+finally）
    - compile_break_stmt/compile_continue_stmt（含 LeaveTry + Gosub finally 穿越）
    - compile_labeled_stmt/compile_for_stmt
    - compile_stmt dispatch 替换 assert(false) 为实际调用
  - `src/vm/vm.cpp`：
    - VM 构造函数拆分，新增 init_global_env() 注册 Error 内建（NativeFn）
    - run() 循环顶部新增 exception_handler 逻辑（找 handler/跨 frame 传播，goto dispatch_begin）
    - kPushScope/kPopScope 维护 scope_depth
    - kGetVar/kSetVar/kInitVar/kGetProp/kCall/kCallMethod/kNewCall 内部错误改为 pending_throw + continue
    - kCall/kCallMethod/kNewCall 新增 is_native() 路径支持 native function 调用
    - 新增 kThrow/kEnterTry/kLeaveTry/kGetException/kGosub/kRet handler
  - `tests/unit/vm_phase7_test.cpp`（新建）：29 个 VM Phase 7 测试
  - `tests/CMakeLists.txt`：注册新测试文件
  - 736/736 全量通过（原有 707 个 + 新增 29 个）

- 完成 Phase 7 子任务 7.2：AST 节点扩展 + Parser 扩展：
  - `include/qppjs/frontend/ast.h`：新增 ThrowStatement、CatchClause（辅助结构）、TryStatement、BreakStatement、ContinueStatement、LabeledStatement、ForStatement 共 7 个结构；StmtNode variant 扩展到 13 个类型
  - `src/frontend/parser.cpp`：新增 parse_throw_stmt / parse_block（内部辅助）/ parse_try_stmt / parse_break_stmt / parse_continue_stmt / parse_for_stmt；parse_stmt 扩展分发 KwThrow、KwTry、KwBreak、KwContinue、KwFor、Ident+Colon（LabeledStatement）；stmt_range 扩展覆盖新节点
  - `src/frontend/ast_dump.cpp`：dump_stmt 新增 6 个 dump 分支（Throw/Try/Break/Continue/Labeled/For）
  - `src/runtime/interpreter.cpp`：eval_stmt 的 std::visit 新增 6 个占位分支（返回 "not yet implemented" 错误）

- 完成 Phase 7 子任务 7.3：AST Interpreter 实现：
  - `include/qppjs/runtime/completion.h`：CompletionType 扩展（kThrow/kBreak/kContinue），Completion 新增 target 字段，新增 throw_/break_/continue_/is_throw/is_break/is_continue/is_abrupt
  - `include/qppjs/runtime/js_function.h`：新增 NativeFn 类型别名和 JSFunction::native_fn_ 字段（is_native/native_fn/set_native_fn）
  - `include/qppjs/runtime/interpreter.h`：新增 6 个 eval 方法声明、exec_catch、hoist_vars_stmt；新增 pending_throw_ 字段和 kPendingThrowSentinel；eval_while_stmt/eval_for_stmt 增加可选 label 参数
  - `src/runtime/interpreter.cpp`：全部 6 个新 eval 实现（eval_throw_stmt/eval_try_stmt/exec_catch/eval_break_stmt/eval_continue_stmt/eval_labeled_stmt/eval_for_stmt）；eval_block_stmt/eval_if_stmt/eval_while_stmt 修正 abrupt 传播；exec 和 call_function 传播 kThrow；hoist_vars 拆分为 hoist_vars_stmt + hoist_vars，递归扫描 ForStatement/TryStatement/LabeledStatement；Interpreter 构造函数注册 Error 内建（NativeFn 机制）
  - `tests/unit/interpreter_phase7_test.cpp`（新建）：31 个 Phase 7 测试
  - `tests/CMakeLists.txt`：注册新测试文件
  - 707/707 全量通过（原 676 + 新增 31）
  - `src/vm/compiler.cpp`：compile_stmt 的 std::visit 新增 6 个占位分支（assert false）
  - `tests/unit/parser_test.cpp`：新增 13 个 Phase 7 Parser 测试，全部通过
  - 676/676 全量回归通过（原 663 + 新增 13）

- 完成函数/闭包 ASAN/LSan 泄露修复（彻底收尾）：
  - `include/qppjs/runtime/js_function.h`：将 `captured_bindings_`（BindingMap 快照）改回 `closure_env_`（`shared_ptr<Environment>`），捕获 env 链引用而非 binding 副本
  - `src/runtime/interpreter.cpp`：`make_function_value` 改为 `set_closure_env(current_env_)`；`call_function` 改为以 `closure_env` 为 outer 创建 fn_env；移除 `collect_visible_bindings()`
  - `src/vm/vm.cpp`：`kMakeFunction` 改为 `set_closure_env(env)`；`push_call_frame` 改为以 `closure_env` 为 outer；移除 `CollectVisibleBindings()`
  - 修复了 `VMFunc.TwoClosuresShareSameEnv` 失败（提升的函数声明在 `let n` 定义前执行 `kMakeFunction`，快照模型捕获不到 `n`；env 链模型在调用时自然可见）
  - ASAN 全量回归：663/663 通过，无泄露，无错误

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

## 4. 风险与待决策项

- P2-1（已知，暂不处理）：VM catch 参数与 catch 体共享同一 scope，规范要求两层独立作用域；`catch(e) { let e = 2; }` 在 VM 路径下会失败
- P2-2（已知，暂不处理）：VM `compile_labeled_stmt` 对非循环体的 labeled break 会触发 `assert(false)`；Interpreter 路径已正确实现
- 内部运行时错误（ReferenceError/TypeError）以字符串值抛出，而非 Error 对象；Phase 8 升级为真正的 Error 子类
- JSFunction::body_ 字段继续保留（AST Interpreter 使用）
- GetProp/SetProp 对 JSFunction 的非 prototype 属性当前静默忽略

## 5. 下次进入点

新 session 开始时，优先做以下动作：
1. 读取本文件
2. 读取 `docs/plans/02-next-phase.md`
3. 进入 Phase 8（待规划：可能包括 for...in/of、正则、模块、更完整的内建对象等）

## 6. 收尾检查清单

每次任务结束前，至少检查：
- 当前任务状态是否变化
- 本文件是否已同步
- `docs/plans/02-next-phase.md` 是否仍正确
- 若阶段计划已调整，`docs/plans/00-roadmap.md` 是否已同步
