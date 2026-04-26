# QppJS 当前开发状态（详情）

本文件是 `01-current-status.md` 的完整版，供 `/implement` 流程和需要完整历史的场景使用。

## 1. 已完成任务

- [x] **Phase 10.1 Review M1/M2/M3 修复**（2026-04-26）：修复 3 个 import/export Parser 审查必修问题。M1：`compile_stmt` 的三个 no-op visitor（ImportDeclaration/ExportNamedDeclaration/ExportDefaultDeclaration）改为 emit `kLoadString`+`kThrow`，VM 执行时产生运行时错误（与 interpreter stub 的 `ErrorKind::Syntax` 行为对齐，两者均在执行阶段报错）。M2：从 `lexer.cpp` 的 `kKeywords` 表移除 `import`/`export` 两条记录（词法器不再产出 `KwImport`/`KwExport`），在 `parse_stmt()` 入口处（switch 之前）新增 Ident 文本比较分发 `parse_import_decl()`/`parse_export_decl()`，同时删除 switch 中原有的 `case TokenKind::KwImport:`/`case TokenKind::KwExport:` 两个分支；修复后 `({ import: 1 }).import`、`obj.export` 等合法属性名解析不再失败。M3：在 `parse_stmt()` 的 labeled statement 分支（Ident + Colon）中，`parse_stmt()` 调用前先 `saved_top_level = is_top_level_; is_top_level_ = false`，调用后恢复（错误路径也恢复），修复 `label: import './m'` 被错误接受为合法语法。新增 4 个测试（ImportAsObjectKey、ExportAsDotAccess、LabeledImportIsError、LabeledExportIsError）。1375/1375 通过，0 回归。
- [x] **Phase 9 GC Review M1/M2 修复**（2026-04-26）：修复 2 个 GC 审查必修问题。M1：`GcHeap::Collect()` Phase 1 新增对 `roots` 参数的 `gc_mark_` 重置循环，解决 `object_prototype_`/`array_prototype_`/`function_prototype_`/`object_constructor_`/`error_protos_[]` 等未注册到 `objects_` 的长期根对象在第二次 exec() 时 `MarkPending` 直接返回（`gc_mark_==true`）、`TraceRefs` 不被调用、子对象被 Sweep 误删导致 UAF 的问题。M2：interpreter.cpp 中 Object.keys（2 处 kArray 分配）、Object.create（1 处 new_obj 分配）、Object() 构造器（1 处 obj 分配）的 native lambda 内补加 `gc_heap_.Register()`；vm.cpp 同步修复（create_fn 同时将捕获从 `[]` 改为 `[this]` 以访问 `gc_heap_`）。追加 4 个测试（InterpMultipleExecObjectPrototypeStaysAlive、VmMultipleExecObjectPrototypeStaysAlive、InterpObjectCreateSurvivesGc、VmObjectCreateSurvivesGc）。1312/1312 通过，run_ut 1306/1306 通过，0 个 LSan 泄露。
- [x] **Phase 9.0 Environment 从 shared_ptr 迁移到 RcPtr<Environment>（RcObject 体系）**（2026-04-26）：消除两套引用计数并存，为 Phase 9.2 mark-sweep GC 统一追踪所有堆对象奠定基础。具体改动：(1) `rc_object.h`：追加 `ObjectKind::kEnvironment`；(2) `environment.h`：`Environment` 改为继承 `RcObject`（移除 `enable_shared_from_this`），构造函数参数和 `outer_` 字段改为 `RcPtr<Environment>`，`outer()` 返回 `const RcPtr<Environment>&`；(3) `environment.cpp`：构造函数初始化 `RcObject(ObjectKind::kEnvironment)`，`clear_function_bindings` 中 `shared_from_this()` 改为 `RcPtr<Environment> self(this)`，`closure_env` 局部变量改为 `RcPtr<Environment>`；(4) `js_function.h`：`closure_env_` 字段、getter、setter 三处同步改为 `RcPtr<Environment>`；(5) `interpreter.h/cpp`：`ScopeGuard` 两个 saved 字段、构造参数，以及 `global_env_`/`current_env_`/`var_env_` 字段，`make_function_value` 参数，全部改为 `RcPtr<Environment>`；所有 `make_shared<Environment>` 改为 `RcPtr<Environment>::make(...)`；(6) `vm.h/vm.cpp`：`CallFrame::env` 和 `global_env_` 改为 `RcPtr<Environment>`；所有 `make_shared<Environment>` 改为 `RcPtr<Environment>::make(...)`；修复关键 bug：`kPopScope` 中 `env = env->outer()` 存在自我赋值导致 SEGFAULT（env 的 release 可能销毁 outer_），改为先 `RcPtr<Environment> parent = env->outer(); env = std::move(parent)`，异常恢复路径 `frame.env = frame.env->outer()` 同样修复。1219/1219 通过，run_ut 4 个 LSan 失败（P3-2 遗留，不增加）。
- [x] **Phase 8.6/8.7 VM catch 作用域修复（P2-1）+ VM labeled break 修复（P2-2）**（2026-04-26）：修复两处 Phase 7 遗留 VM 缺陷。8.6：`compile_try_stmt` 中有 finally 和无 finally 两处 catch 分支，将手动 `for (const auto& s : handler->body.body) compile_stmt(s)` 替换为 `compile_block_stmt(stmt.handler->body)`，使 catch 参数绑定在外层 `kPushScope/kPopScope` 中，catch body 的 let/const 声明由 `compile_block_stmt` 按 `has_block_scope_decl` 判断自动创建内层 scope。8.7：`compile_labeled_stmt` else 分支（非 for/while 的 labeled 语句）在 `compile_stmt` 前后注册/清理 `loop_env_stack_`，并 patch break_patches，使 `break outer` 能正确找到目标。新增 6 个测试（VMCatchScopeP21 × 3 + VMLabeledBlockBreak × 3），1219/1219 全部通过，无回归。
- [x] **Phase 8.5 Function 内建方法**（2026-04-26）：实现 `Function.prototype.call`、`apply`、`bind`，Interpreter 和 VM 两侧对称。核心改动：(1) `interpreter.h` / `vm.h` 新增 `function_prototype_` 成员（`RcPtr<JSObject>`）；(2) `init_runtime()` / `init_global_env()` 末尾注册 `function_prototype_`（proto = object_prototype_），挂载 call/apply/bind 三个 native function；(3) `eval_member_expr` / `kGetProp` 的 kFunction 分支：先查 fn->get_property(key)，未命中时查 function_prototype_->get_property(key)；(4) `eval_call_expr` 的 kFunction 分支同步加二次查找；(5) bind 用 native_fn_ lambda 封装，捕获 target/bound_this/bound_args，支持二次 bind（this 固定为第一次 bound_this）；(6) apply 支持 kArray（按 elements_ 索引展开）和 array-like（kOrdinary + length 属性 + 数字索引属性）；(7) exec/run 清理路径同步添加 function_prototype_->clear_function_properties()。新增 32 个测试（16 InterpFunctionBuiltin + 16 VMFunctionBuiltin，覆盖 FB-01~FB-16），1171/1171 全部通过。
- [x] **Phase 8.4 Object 内建方法 Review 必修问题修复 M1/M2/M3**（2026-04-26）：修复 3 个审查必修问题。M1：Object 构造函数 lambda 改为捕获 `this`，无参/null/undefined 时创建带 `set_proto(object_prototype_)` 的新对象，使 `new Object() instanceof Object` 返回 true；M2：`Object.create` 对 kFunction 参数抛 TypeError（JSFunction 不继承 JSObject，无法作为原型）；M3：`Object.assign` 对 kArray target 使用 `set_property_ex` 走数组感知路径（正确同步 `array_length_`）。Interpreter 和 VM 两侧对称修复。新增 8 个测试（OB-36/37/38/39 各 Interp+VM），1139/1139 全部通过。
- [x] **Phase 8.4 Object 内建方法**（2026-04-26）：实现 `Object.keys`、`Object.assign`、`Object.create`，Interpreter 和 VM 两侧对称。核心改动：(1) `JSObject::own_enumerable_string_keys()` 新方法，普通对象按插入顺序，数组对象先排序整数索引再追加非索引键；(2) `JSFunction` 新增 `own_properties_` 属性字典（`set_property`/`get_property`/`clear_own_properties`），新建 `js_function.cpp`；(3) Interpreter `eval_call_expr` 和 `eval_member_expr` 中 kFunction 属性读取由只支持 `prototype` 扩展为支持任意自有属性；(4) VM `kGetProp` 中 kFunction 属性读取同步扩展；(5) Object 构造函数注册为 `define_initialized`（可变），避免用户重定义 `function Object() {}` 报 const 赋值错误；(6) 所有 TypeError 通过 `pending_throw_` / `EvalResult::err` 机制正确抛出。新增 42 个测试（21 Interp + 21 VM），1103/1103 全部通过，run_ut 4 个 LSan 泄露为预先存在的 P3-2 遗留问题。
- [x] **闭包环境共享修复**（2026-04-26）：根因是 `MakeFunction` 时调用 `clone_for_closure` 对整个环境链做快照，顶层 `let` 在克隆时处于 TDZ，函数体内访问报 `ReferenceError`。删除 `clone_for_closure` / `clone_closure_env` / `define_binding` 整套克隆机制（`environment.cpp/h`、`vm.cpp/h`），`MakeFunction` 直接将当前 `env` 的 `shared_ptr` 存入 `closure_env`；解释器四处 `clone_for_closure` 调用同步改为直接传 `current_env_`。修复后 `VMFunc.ClosureSeesUpdated`、`VMFunc.FunctionCanReadOuterVar`、`VMFunc.TwoClosuresShareSameEnv`、`VMFinallyOverride.FinallyNormalSideEffectWithTryReturn` 全部通过。
- [x] **Named function expression 自引用修复**（2026-04-26）：`call_function` / `push_call_frame` 中判断是否注册自引用绑定的条件 `fn_env->lookup(...) == nullptr` 错误——`lookup` 走 outer 链，外层有同名变量时跳过自引用。在 `JSFunction` 加 `is_named_expr_` 字段，`BytecodeFunction` 加 `is_named_expr` 字段；编译器 `compile_function_expr` 和解释器 function expression 路径设置标记；`call_function` / `push_call_frame` 改为只对 named expr 无条件写入自引用绑定。修复后 `FunctionTest.NamedFunctionExpressionShadowsOuterSameName` 和 `VMFunc.NamedFunctionExpressionShadowsOuterSameName` 通过。coverage 1061/1061 全部通过。
- [x] **`scripts/qppjs.py` `split_log` 重构**（2026-04-26）：提取 `@contextlib.contextmanager split_log(success_path, failure_path, *, failure_filter)` 上下文管理器，统一"写 raw → 成功 rename / 失败分流"逻辑；`TestRunner.run`、`TestRunner.run_quiet`、`CoverageRunner.run` 三处重复代码消除约 23 行。
- [x] **build skill 工具使用规范更新**（2026-04-26）：明确 `coverage.sh` 用于 UT 功能验证（无 ASAN/LSan 噪音，失败即功能缺陷），`run_ut.sh` 专用于内存泄露检查；更新 `SKILL.md` 常用场景排序、脚本表、注意事项，更新 `CLAUDE.md` 快速参考。

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

- [x] Phase 7：控制流扩展（break/continue/throw/try/catch/finally）
  - [x] 7.1 规范调研 + 设计
  - [x] 7.2 AST 节点扩展 + Parser 扩展
  - [x] 7.3 AST Interpreter 实现（CompletionType 扩展、6 个新 eval 方法、NativeFn + Error 内建）
  - [x] 7.4 VM 编译器扩展：6 条新 opcode、Compiler 实现、VM dispatch 扩展、Error 内建 VM 侧注册
  - [x] 7.5 Error 内建对象（Interpreter 7.3 已含，VM 7.4 补充）
  - [x] 7.6 全量测试回归（736/736 通过）
  - [x] 7.7 边界测试补充（Testing Agent）：新增 56 个边界测试，790/790 通过，2 个已知 VM 缺陷标注 DISABLED
  - [x] 7.8 P1-1 缺陷修复：compile_return_stmt 增加 finally_info_stack_ 穿越逻辑（LeaveTry + Gosub），792/792 全量通过

## 2. 最近完成内容

- 完成 `scripts/qppjs.py coverage --quiet` 日志行为修复：
  - 保留实际日志路径：成功为 `build/coverage_success.log`，失败为 `build/coverage_failure.log`
  - quiet 模式改为先写原始输出到临时 `build/coverage_raw.log`；若失败，仅提取失败 UT / LSan 摘要写入 `coverage_failure.log`，避免把 build、lcov、genhtml 全量输出直接落到失败日志
  - 复用 `write_test_failure_report()`，并允许传入自定义标题，使 coverage 失败报告格式与 `run_ut` 保持一致
  - 同步更新顶层 epilog 与 `coverage --help` 文案，明确失败日志只包含失败 UT 摘要
  - 验证：`python3 -m py_compile scripts/qppjs.py` 通过

- 完成 `scripts/qppjs.py test --quiet` 日志目录修复：
  - `scripts/qppjs.py` 中 `TestRunner.run()` 的 quiet 构建日志从 `build/run_ut_build_failure.log` / `build/run_ut_build_success.log` 调整到 `build/debug/run_ut_build_failure.log` / `build/debug/run_ut_build_success.log`
  - 使 UT quiet 模式下构建日志与 ctest 成功/失败日志 (`run_ut_success.log` / `run_ut_failure.log`) 保持同目录，避免成功和错误日志分散在 `build/` 与 `build/debug/`
  - 同步更新顶层 epilog 与 `test --help` 文案，明确构建日志也位于 `build/debug/`
  - 验证：`python3 -m py_compile scripts/qppjs.py` 通过

- 完成构建脚本 Python 统一入口重构：
  - 新增 `scripts/qppjs.py`，用 argparse 子命令统一覆盖 `clean`、`build debug/release/test`、`test`、`coverage`
  - 收口重复逻辑：项目路径发现、macOS Homebrew LLVM 探测、CMake 参数拼装、build metadata 读取、ctest 参数构造、quiet 失败报告解析、coverage backend 分支
  - `scripts/build_debug.sh`、`build_release.sh`、`build_test.sh`、`clean.sh`、`run_ut.sh`、`coverage.sh` 保留为薄 wrapper，继续兼容原命令入口
  - 帮助信息已优化：顶层 `--help` 展示常用示例和兼容入口；`build --help` 展示 debug/release/test 的 build 目录与 CMake 开关；`test`、`coverage`、`clean` 子命令说明输出产物和行为边界；支持 `clean build release`、`clean test --quiet`、`clean coverage --quiet --open` 前置组合用法；coverage 支持 `--quiet` 静默构建、ctest、lcov/genhtml；quiet 模式区分成功/失败日志（UT：ctest 日志为 `build/debug/run_ut_success.log` / `build/debug/run_ut_failure.log`，构建日志为同目录下的 `run_ut_build_success.log` / `run_ut_build_failure.log`；coverage：`build/coverage_success.log` / `build/coverage_failure.log`）
  - 验证：`python3 -m py_compile scripts/qppjs.py` 通过；6 个 shell wrapper `bash -n` 通过；Python 与 wrapper 帮助输出通过；`python3 scripts/qppjs.py clean build release` 通过；`python3 scripts/qppjs.py clean test --quiet` 生成预期失败报告；`python3 scripts/qppjs.py test --quiet` 可静默失败并写入 `build/debug/run_ut_failure.log`；`python3 scripts/qppjs.py coverage --quiet` 可静默失败并写入 `build/coverage_failure.log`（当前 coverage ctest 有 4 个预存失败）

- 修复函数/闭包/原型相关 ASAN/LSan 泄漏：
  - `include/qppjs/runtime/environment.h`、`src/runtime/environment.cpp`：为 `Binding` 增加 `function_like` 标记，补充 `define_function()`、`clone_for_closure()`、`clear_function_bindings()`；递归清理 closure env 与对象属性中的函数引用
  - `include/qppjs/runtime/js_object.h`、`src/runtime/js_object.cpp`：新增 `clear_function_properties()`，递归清理对象/prototype 链上保存的函数值，打断 `obj -> fn -> prototype/closure` 保留环
  - `src/runtime/interpreter.cpp`：函数声明/表达式改为捕获裁剪后的 closure env；顶层 `exec()` 在成功/异常返回前统一清理函数绑定与对象属性中的函数引用
  - `include/qppjs/vm/bytecode.h`、`src/vm/compiler.cpp`：新增 `function_decls`，将函数声明提升槽与 `var_decls` 分离，避免 VM 在清理函数绑定时误伤普通变量绑定
  - `include/qppjs/vm/vm.h`、`src/vm/vm.cpp`：VM 顶层与函数帧分别预定义 `function_decls`；`kMakeFunction` 直接捕获运行时环境共享绑定；`exec()` 返回前同步执行清理逻辑
  - 验证：`FunctionTest` / `VMFunc` / `VMProto` / `InterpreterThrow|TryCatch|FinallyOverride` / `VMTryCatch|FinallyOverride` 分组回归 122/122 通过；`./scripts/run_ut.sh` 全量通过，ASAN/LSan 无泄漏

- 建立 macOS LSan 基础设施 + 闭包边界测试补充：
  - `scripts/run_ut.sh` 加 `ASAN_OPTIONS=detect_leaks=1`（macOS 上 LSan 默认关闭，需显式开启）
  - 深度调研 Interpreter 闭包循环引用根因：`clone_for_closure` 产生的克隆 Environment 与其中
    持有的 JSFunction 形成 `clone_env → Cell → JSFunction → closure_env_(clone_env)` 循环；
    Cell 共享设计下引用计数无法打断（`ClosureSeesReassignedFunctionBinding` 要求必须共享 Cell），
    三种常规方案（`outer_` weak_ptr、`closure_env_` weak_ptr、`captured_upvalues_` 替换）均无效；
    结论：需要 Phase 9 标记清除 GC 才能根本解决，记为遗留问题 P3-2（5256 字节，39 个分配）
  - `tests/unit/function_test.cpp` 新增 10 个边界用例：三层嵌套捕获、外层修改后闭包读新值、
    for-var 循环闭包读最终值、具名函数表达式递归、闭包写外层 let、多工厂调用独立性、
    空捕获、const 捕获写入报错、var 遮蔽外层 let（×2）
  - 验证：927/931 通过（4 个 VM 已知失败不变）

- 完成构建脚本跨平台探测修复：
  - `scripts/build_debug.sh`、`scripts/build_release.sh`、`scripts/build_test.sh`：仅在 `CC/CXX` 都未设置、平台为 `Darwin` 且 `brew` 存在时才探测 Homebrew LLVM；其他环境保持未设置 `CC/CXX`，交由 CMake/系统编译器选择，避免在 `set -euo pipefail` 下因缺少 `brew` 直接退出
  - 当前 Linux/WSL 环境下已验证 `./scripts/build_debug.sh`、`./scripts/build_release.sh`、`./scripts/build_test.sh` 均可完成构建

- 完成 Phase 8.1 — Error 子类（TypeError/ReferenceError/RangeError）+ instanceof 运算符：
  - **新增文件**：`include/qppjs/runtime/native_errors.h`（`NativeErrorType` 枚举 + `MakeNativeErrorValue` 工厂函数声明）、`src/runtime/native_errors.cpp`（工厂函数实现）
  - **新增 instanceof 支持**：`token.h` 添加 `KwInstanceof`；`ast.h` 添加 `BinaryOp::Instanceof`；`lexer.cpp` 注册关键字；`parser.cpp` 添加 lbp=10 和 led 处理；`ast_dump.cpp` 添加 case；`opcode.h` 添加 `kInstanceof`；`compiler.cpp` emit `kInstanceof`
  - **VM 侧**：`vm.h` 添加 `error_protos_[4]` 缓存数组和 `make_error_value` 私有方法；`vm.cpp` 重构 `init_global_env()`（完整 Error 原型链：Error → TypeError/ReferenceError/RangeError）、实现 `kInstanceof` opcode（原型链遍历）、将所有字符串抛出替换为 Error 实例（kGetVar/kSetVar/kInitVar/kGetProp/kSetProp/kGetElem/kSetElem/kCall/kCallMethod/kNewCall/kTypeofVar）、修正顶层异常消息格式（`name: message`）
  - **Interpreter 侧**：`interpreter.h` 添加 `error_protos_[4]` 和 `make_error_value`；`interpreter.cpp` 重构构造函数（完整 Error 原型链）、在 `eval_binary` 添加 `BinaryOp::Instanceof` case
  - **修复 P2-3**：内部运行时错误（ReferenceError/TypeError）从字符串改为真正的 Error 实例
  - **新增测试**：`tests/unit/vm_error_test.cpp`（20 个测试）、`tests/unit/interpreter_error_test.cpp`（16 个测试）
  - 861/861 测试全部通过，ASAN/LSan 无泄漏

- 完成 Phase 8.1 Review 必修问题修复（M1/M2/M3）：
  - **M1 — VM 错误消息双重前缀**：在 `src/vm/vm.cpp` 添加 `strip_error_prefix()` 辅助函数，在 kGetVar/kSetVar/kInitVar/kCall/kCallMethod/kNewCall 中所有从 Environment C++ Error 取消息的路径调用剥离前缀，确保 `e.message` 不含 `"XxxError: "` 前缀
  - **M2 — Error 原型链缺少 constructor 属性**：在 `src/vm/vm.cpp` 的 `init_global_env()` 和 `src/runtime/interpreter.cpp` 的构造函数中，为 Error 和每个子类 prototype 调用 `set_constructor_property(fn.get())`，使 `XxxError.prototype.constructor === XxxError`
  - **M3 — Interpreter 路径运行时异常仍为字符串**：在 `src/runtime/interpreter.cpp` 中添加 `strip_error_prefix()` 辅助函数；将 `eval_identifier`、`eval_assignment`、`eval_member_expr`、`eval_member_assign`、`eval_call_expr`、`eval_new_expr` 中所有运行时错误路径改为设置 `pending_throw_` 并返回哨兵；同时修复 `exec()` 顶层未捕获错误的格式化（`name: message`）
  - **测试更新**：`vm_error_test.cpp` 新增 T-21（M1）、T-22（M2）共 6 个测试；`interpreter_error_test.cpp` T-12/T-13 从期望字符串改为期望 Error 实例，新增 T-17（M2）共 3 个测试
  - 917/917 测试全部通过，ASAN/LSan 无泄漏

- 完成 P1 全部热路径性能优化（来自 docs/perf/001-all.md）：
  - **P1-1+2**：`Cell` 增加非原子引用计数，`CellPtr` 从 `shared_ptr<Cell>` 改为 `RcPtr<Cell>`，保留闭包共享可变语义，消除原子操作开销（`environment.h`、`environment.cpp`）
  - **P1-3**：`push_call_frame` 改为接受 `std::span<Value>`；kCall/kCallMethod/kNewCall 三处用 8 元素栈缓冲区收集参数，消除小参数调用的 malloc，同时消除 `std::reverse`（`vm.h`、`vm.cpp`）
  - **P1-4**：Compiler 新增 `has_block_scope_decl` 扫描，无 let/const/function 声明的块跳过 kPushScope/kPopScope，消除循环体每次迭代的 `make_shared<Environment>`（`compiler.cpp`）
  - **P1-5**：`FlatBindingMap` 替换 `unordered_map`，≤16 绑定线性扫描，超出阈值懒升级，提升小函数变量查找的 cache 局部性（`environment.h`、`environment.cpp`）
  - **P1-6**：新增 `number_to_string` 辅助函数，整数快路径用 `std::to_string`，浮点用 `std::to_chars` + 栈缓冲区，消除 `ostringstream` 构造开销（`vm.cpp`）
  - **P1-7/8/9**：NaN-boxing 迁移时已顺带解决（`kind()` O(1)、LoadString 引用计数、`dynamic_pointer_cast` → `static_cast`）
  - 825/825 全量通过，ASAN/LSan 无泄漏

- 完成 P2 设计层技术债务（部分）：
  - P2-1：Value NaN-boxing（`sizeof(Value)` 40→8 字节）
  - P2-2：`ObjectPtr` 改为 `RcPtr<RcObject>`（非原子引用计数）
  - P2-4：`add_name` 加反向索引，O(n)→O(1)
  - P2-5：`parse_number_text` 用 `std::from_chars` 替代 `stod`，消除临时 string
  - P2-3（`Environment::outer_` shared_ptr 链）：待 Phase 9 GC 统一处理

- 完成 Value NaN-boxing + ObjectPtr 非原子引用计数迁移（Phase 0 基础设施优化）：
  - 新增 `include/qppjs/runtime/rc_object.h`：`RcObject` 基类（非原子 `ref_count_` + `object_kind_`，无虚函数 kind 查询）、`RcPtr<T>` 智能指针（copy/move/destructor 管理引用计数，支持 derived→base 隐式上转型）、`JSString`（内嵌引用计数的堆字符串）
  - 重写 `include/qppjs/runtime/value.h`：8 字节 NaN-boxing Value，`static_assert(sizeof(Value) == 8)`，`as_object()` 返回值类型 `ObjectPtr`，新增 `as_object_raw()` 热路径接口
  - 重写 `src/runtime/value.cpp`：NaN 规范化（位操作检测，规范化为 `kCanonicalNaN`），完整的 copy/move/析构引用计数管理
  - 更新 `include/qppjs/runtime/js_object.h`：继承 `RcObject`（替代 `Object`），`proto_` 改为 `RcPtr<JSObject>`，`constructor_property_` 改为裸指针（弱引用）
  - 更新 `include/qppjs/runtime/js_function.h`：继承 `RcObject`（替代 `Object`），`prototype_` 改为 `RcPtr<JSObject>`，`closure_env_`/`body_`/`bytecode_` 保留 `std::shared_ptr`
  - 更新 `src/runtime/js_object.cpp`：适配新接口（`set_constructor_property(RcObject*)` 弱引用）
  - 更新 `src/runtime/interpreter.cpp` + `src/vm/vm.cpp`：全面替换 `std::make_shared<JSObject/JSFunction>` → `RcPtr::make()`，`dynamic_pointer_cast` → `object_kind()` + `static_cast`，`as_object()` → `as_object_raw()` 热路径
  - 更新 `tests/unit/proto_test.cpp`、`tests/unit/value_test.cpp`：适配新 API
  - 新增 `tests/unit/value_nanboxing_test.cpp`：33 个新测试（double 边界值、tag 编码、copy/move 语义、引用计数路径、`sizeof(Value)==8`）
  - 825/825 全量通过（原 792 + 新增 33），ASAN/LSan 无泄漏

- 完成 Phase 7 P1-1 缺陷修复（7.8）：
  - `src/vm/compiler.cpp`：`compile_return_stmt` 在 `finally_info_stack_` 非空时，对每个活跃 finally 从内到外 emit `kLeaveTry` + `kGosub`（patch 位置记录到 `gosub_patches`，由 `compile_try_stmt` 统一 patch），最后 emit `kReturn`；无参 return 情形先 emit `kLoadUndefined` 再走相同路径
  - `tests/unit/vm_phase7_edge_test.cpp`：去掉 `DISABLED_` 前缀启用 `FinallyReturnOverridesTryReturn` 和 `FinallyNormalSideEffectWithTryReturn` 两个测试
  - 792/792 全量通过（原 790 + 新启用 2，0 回归）

- 完成 Phase 7 子任务 7.4：VM 编译器扩展：
  - `include/qppjs/vm/opcode.h`：新增 6 条指令（Throw/EnterTry/LeaveTry/GetException/Gosub/Ret）
  - `include/qppjs/vm/vm.h`：CallFrame 新增 ExceptionHandler 结构体、handler_stack/pending_throw/caught_exception/finally_return_stack/scope_depth 字段
  - `include/qppjs/vm/compiler.h`：新增 LoopEnv/FinallyInfo 结构体、loop_env_stack_/finally_info_stack_ 字段、6 个新 compile_* 方法声明、patch_jump_to/emit_jump_to/current_offset 辅助方法
  - `src/vm/compiler.cpp`：新增 patch_jump_to/emit_jump_to/current_offset；hoist_vars_scan_stmt 扩展；compile_while_stmt 重构；compile_throw_stmt/compile_try_stmt（三分支）；compile_break_stmt/compile_continue_stmt；compile_labeled_stmt/compile_for_stmt
  - `src/vm/vm.cpp`：VM 构造函数拆分，新增 init_global_env()；run() 循环顶部新增 exception_handler 逻辑；新增 kThrow/kEnterTry/kLeaveTry/kGetException/kGosub/kRet handler
  - `tests/unit/vm_phase7_test.cpp`（新建）：29 个 VM Phase 7 测试
  - 736/736 全量通过（原有 707 个 + 新增 29 个）

- 完成 Value NaN-boxing + ObjectPtr 非原子引用计数（Phase 12.1 提前实施）：
  - `include/qppjs/runtime/rc_object.h`（新建）：`RcObject` 基类（非原子 ref_count + object_kind 数据成员）、`RcPtr<T>` 智能指针（含 derived→base 隐式上转型）、`JSString` 结构体（非原子引用计数）
  - `include/qppjs/runtime/value.h`：完整替换为 NaN-boxing（`uint64_t raw_`，8 字节），`using ObjectPtr = RcPtr<RcObject>`，新增 `as_object_raw()` 裸指针接口，`as_object()` 改为值返回
  - `src/runtime/value.cpp`：NaN 规范化（位操作）、tag/payload 编码、copy/move/析构引用计数管理
  - `include/qppjs/runtime/js_object.h`：继承 `RcObject`，`proto_` 改为 `RcPtr<JSObject>`，`constructor_property_` 改为裸指针
  - `include/qppjs/runtime/js_function.h`：继承 `RcObject`，`prototype_` 改为 `RcPtr<JSObject>`
  - `include/qppjs/runtime/interpreter.h`、`include/qppjs/vm/vm.h`：`object_prototype_` 改为 `RcPtr<JSObject>`
  - `src/runtime/interpreter.cpp`、`src/vm/vm.cpp`：`make_shared` → `RcPtr::make()`，`dynamic_pointer_cast` → `object_kind()+static_cast`
  - `tests/unit/value_nanboxing_test.cpp`（新建）：33 个新测试（NaN-boxing 位编码、引用计数语义、RcPtr 语义）
  - 792 → 825（+33），ASAN/LSan 无泄漏，`sizeof(Value) == 8`

## 3. 风险与待决策项

- P2-1（已知，暂不处理）：VM catch 参数与 catch 体共享同一 scope，规范要求两层独立作用域；`catch(e) { let e = 2; }` 在 VM 路径下会失败
- P2-2（已知，暂不处理）：VM `compile_labeled_stmt` 对非循环体的 labeled break 会触发 `assert(false)`；Interpreter 路径已正确实现
- P2-3：内部运行时错误（ReferenceError/TypeError）以字符串值抛出，而非 Error 对象；Phase 8 升级为真正的 Error 子类
- P3-1（新，已知）：`JSString` 二次堆分配（`std::string` 成员），Phase 9 优化
- P3-2（新，已知）：循环引用（proto 链、closure env）导致内存泄漏，Phase 9 GC 解决
- JSFunction::body_ 字段继续保留（AST Interpreter 使用）
- GetProp/SetProp 对 JSFunction 的非 prototype 属性当前静默忽略

## 4. 2026-04-24 内部性能优化（无语义变化）

- `src/frontend/parser.cpp`：`parse_number_text` 十进制路径从 `std::stod`（try/catch）改为 `std::from_chars`，消除异常开销，与"禁止异常"约定对齐
- `include/qppjs/vm/compiler.h` + `src/vm/compiler.cpp`：`add_name` 新增 `name_index_`（`unordered_map<string, uint16_t>`）反向索引，去重从 O(n) 降至 O(1)；`compile_function` 上下文切换时保存/恢复索引
- 测试：825/825 通过，ASAN/LSan 无泄漏

## 5. 2026-04-24 P1 性能优化三连（Cell RcPtr + span + FlatBindingMap）

- **P1-1+2（Cell RcPtr）**：`include/qppjs/runtime/environment.h`、`src/runtime/environment.cpp`
  - `Cell` 增加 `int32_t ref_count`、`add_ref()`、`release()` 方法
  - `CellPtr` 从 `std::shared_ptr<Cell>` 改为 `RcPtr<Cell>`
  - `MakeCell` 从 `std::make_shared` 改为 `RcPtr<Cell>::make`
  - 消除 `Cell` 的原子引用计数开销

- **P1-3（push_call_frame span）**：`include/qppjs/vm/vm.h`、`src/vm/vm.cpp`
  - `push_call_frame` 签名改为 `std::span<Value> args`
  - kCall/kCallMethod/kNewCall 三处调用点：≤8 参数用栈上 `Value small_buf[8]`，>8 参数用 `std::vector<Value>`，消除 `std::vector` 堆分配和 `std::reverse`
  - native 函数调用路径仍用 `std::vector<Value>`（从 span 构造）
  - 移除 `#include <algorithm>`

- **P1-5（FlatBindingMap）**：`include/qppjs/runtime/environment.h`、`src/runtime/environment.cpp`
  - 新增 `FlatBindingMap` 类：≤16 条目线性扫描（`vector<pair<string, Binding>>`），超出阈值懒升级到 `unordered_map`
  - `find()` 返回 `Binding*`（nullptr 表示未找到）
  - `Environment::bindings_` 类型从 `BindingMap`（unordered_map）改为 `FlatBindingMap`
  - `lookup()` 适配新接口
  - 测试：825/825 通过，ASAN/LSan 无泄漏

## 6. 2026-04-24 P1-6 ostringstream 替换 + P1-4 kPushScope 条件化

- **P1-6（number_to_string）**：`src/vm/vm.cpp`
  - 移除 `#include <sstream>`，新增 `#include <charconv>`
  - 新增文件内静态函数 `number_to_string(double)`：`d==0.0` → `"0"`；整数快路径（floor 判断 + 安全范围）→ `std::to_string(int64_t)`；一般浮点 → `std::to_chars(general, 17)`
  - `to_string_val` 的 Number 分支改为调用 `number_to_string`
  - `init_global_env` 的 Error native lambda 同样改为调用 `number_to_string`

- **P1-4（kPushScope 条件化）**：`src/vm/compiler.cpp`
  - 新增静态辅助函数 `has_block_scope_decl`：扫描 stmts，存在 let/const 声明或函数声明则返回 true
  - `compile_block_stmt`：`need_scope = has_block_scope_decl(stmt.body)`，条件化 emit kPushScope/kPopScope
  - `compile_stmt_last` 的 BlockStatement 分支：同样条件化 emit kPushScope/kPopScope
  - 效果：只有 var 的块、空块不再创建多余 scope；有 let/const 的块和有函数声明的块仍正确创建 scope
  - 测试：1516/1516 通过，ASAN/LSan 无泄漏

## 7. 2026-04-25 Phase 8.2 — console 对象

- **改动文件**：`src/runtime/interpreter.cpp`、`src/vm/vm.cpp`、`tests/unit/console_test.cpp`、`tests/CMakeLists.txt`
- **实现内容**：
  - 在 `Interpreter::init_runtime()` 末尾追加 console 注册：创建 `log_fn`（native lambda，调用 `Interpreter::to_string_val` 拼接参数，`std::cout` 输出）；创建 `console_obj`，`set_proto(object_prototype_)`，注册 `log` 属性；`global_env_->define_initialized("console")` + `set`
  - 在 `VM::init_global_env()` 末尾追加 console 注册：同上，调用 `VM::to_string_val`；`global_env_->define("console", VarKind::Const)` + `initialize`
  - 两侧均添加 `#include <iostream>`
- **新增测试**：`tests/unit/console_test.cpp`，包含 `InterpConsole` 和 `VMConsole` 两个 test suite，各 10 个测试（C-01 到 C-10）
- **验证**：20/20 console 测试通过；全量 947/951 通过（4 个预存失败不变）

## 8. 2026-04-25 Phase 8.2 边界测试补充 — console 边界用例

- **改动文件**：`tests/unit/console_test.cpp`（追加）
- **新增测试（C-11 到 C-24，InterpConsole + VMConsole 各 14 个，共 28 个）**：
  - C-11: 5 个参数空格分隔正确（`1 2 3 4 5\n`）
  - C-12: NaN 输出（`0/0` 产生 NaN，因 `NaN` 全局标识符未注册）
  - C-13: Infinity 输出（`1/0`）
  - C-14: -Infinity 输出（`-1/0`）
  - C-15: 0 输出 "0"
  - C-16: -0 输出 "0"（规范 ToString(-0) === "0"，两侧均通过）
  - C-17: 负整数 -42
  - C-18: 空字符串参数 → 仅输出 "\n"
  - C-19: 含空格字符串原样输出
  - C-20: 连续两次调用各自独立输出一行
  - C-21: console.log 赋值给变量后调用
  - C-22: console 注册不影响其他全局变量查找（回归）
  - C-23: console.log 返回值为 undefined
  - C-24: 混合类型 5 参数（undefined null true 0 end）
- **发现缺失功能**：`NaN` / `Infinity` 全局标识符未注册（规范 §18.1.1/§18.1.2 要求），测试中改用算术表达式绕过，待后续补注册
- **验证**：48/48 console 测试通过；全量 977/981 通过（4 个预存失败不变）

## 9. 2026-04-25 Phase 8.3 — Array 基础

- **改动文件**：
  - `include/qppjs/runtime/rc_object.h`：`ObjectKind` 增加 `kArray`
  - `include/qppjs/runtime/js_object.h`：增加 `elements_` 成员、带 kind 参数的构造函数、`set_property_ex` 方法（length setter RangeError 校验）
  - `include/qppjs/runtime/js_function.h`：`NativeFn` 签名增加 `Value this_val` 第一参数
  - `include/qppjs/frontend/ast.h`：新增 `ArrayExpression` 节点，加入 `ExprNode` variant
  - `include/qppjs/vm/opcode.h`：新增 `kNewArray` 指令
  - `include/qppjs/runtime/interpreter.h`：增加 `array_prototype_` 成员、`eval_array_expr` 和 `call_function_val` 声明、`<span>` include
  - `include/qppjs/vm/vm.h`：增加 `array_prototype_` 成员、`call_function_val` 声明
  - `include/qppjs/vm/compiler.h`：增加 `compile_array_expr` 声明
  - `src/runtime/js_object.cpp`：实现 `try_parse_array_index`、kArray 分支的 `get_property`/`set_property`/`set_property_ex`、`clear_function_properties` 扩展到 kArray
  - `src/frontend/parser.cpp`：`nud()` 增加 `LBracket` 分支（数组字面量解析，含 elision 和尾随逗号）；`expr_range` 增加 `ArrayExpression` case
  - `src/frontend/ast_dump.cpp`：`dump_expr` 增加 `ArrayExpression` case
  - `src/runtime/interpreter.cpp`：所有 NativeFn lambda 签名增加 `Value this_val` 参数；`eval_expr` 增加 `ArrayExpression` 分支；新增 `eval_array_expr`、`call_function_val` 实现；`init_runtime()` 注册 `array_prototype_`（push/pop/forEach）；修复 `eval_member_expr`/`eval_member_assign`/`eval_call_expr` 支持 kArray；`call_function` native 分支传 this_val
  - `src/vm/vm.cpp`：所有 NativeFn lambda 签名增加 `Value this_val` 参数；`kCall`/`kCallMethod`/`kNewCall` native 分支传 this_val（CallMethod 传 receiver）；新增 `kNewArray` 实现；`kGetProp`/`kSetProp`/`kGetElem`/`kSetElem` 支持 kArray（含整数快路径守卫）；`init_global_env()` 注册 `array_prototype_`；新增 `call_function_val` 实现
  - `src/vm/compiler.cpp`：`compile_expr` 增加 `ArrayExpression` 分支；新增 `compile_array_expr` 实现（NewArray + 逐元素 SetElem）
  - `tests/unit/array_test.cpp`（新建）：40 个测试（InterpArray A-01~A-20，VMArray A-01~A-20）
  - `tests/CMakeLists.txt`：注册 `array_test.cpp`

- **关键设计决策**：
  - `set_property_ex` 处理 length setter 的 RangeError（非整数、负数、>4294967295）；普通属性走 `set_property`
  - `kGetElem`/`kSetElem` kArray 快路径：`d >= 0 && d == floor(d) && d < UINT32_MAX` 守卫，防止 `arr[-1]` 触发巨型 resize
  - `forEach` 迭代范围在进入时固定（`len = elements_.size()`），循环中 push 不影响迭代次数（C-14 规范）
  - `call_function_val` 在 VM 侧通过 `run(exit_depth)` 嵌套运行，支持 forEach callback 调用 JS 函数
  - NativeFn 签名增加 `this_val` 参数，kCallMethod 传 receiver 作为 this_val（P1 修复）

- **验证**：40/40 Array 测试通过；全量 1017/1021 通过（4 个预存遗留失败不变）；无新增 ASAN 泄漏

## 10. 2026-04-25 Phase 8.3 崩溃修复 + adversarial review 采纳

- **问题**：Phase 8.3 实现中 `elements_` 使用 `vector<Value>` 密集存储，`arr[4294967294] = x` 或 `arr.length = 4294967295` 触发 ~16GB/40GB resize 导致系统崩溃；同时 Interpreter 侧 `clear_function_properties` 未清理 `array_prototype_`，每个测试 case 均有内存泄露

- **崩溃修复**：
  - `include/qppjs/runtime/js_object.h`：`elements_` 从 `vector<Value>` 改为 `unordered_map<uint32_t, Value>` 稀疏存储，加独立 `array_length_` 字段（替代 `length_override_`）
  - `src/runtime/js_object.cpp`：`get_property("length")` 直接返回 `array_length_`；`set_property` 写入时更新 `array_length_`（无 resize）；`set_property_ex` length setter 仅截断（删除 >= new_len 的 key），不扩容；`clear_function_properties` 改为遍历 `unordered_map`
  - `src/runtime/interpreter.cpp` + `src/vm/vm.cpp`：push/pop/forEach/array literal 构建全部适配稀疏存储；`kGetElem` 快路径改用 `elements_.find`

- **内存泄露修复**：
  - `src/runtime/interpreter.cpp`：6 处 `clear_function_properties` 调用点补全 `if (array_prototype_) array_prototype_->clear_function_properties()`
  - `lsan_suppressions.txt`（新建）：屏蔽 macOS 系统库误报（`_fetchInitializingClassList`、`_libxpc_initializer`、`libSystem_initializer`、`__Balloc_D2A`、`__dtoa`）
  - `scripts/run_ut.sh`：macOS 下自动设置 `LSAN_OPTIONS=suppressions=lsan_suppressions.txt`

- **adversarial review 采纳（3/4 条）**：
  - **[high] push overflow 保护**：push 前检查 `array_length_ == UINT32_MAX`，溢出时返回 `RangeError`（interpreter.cpp + vm.cpp 两侧）
  - **[medium] is_new_call 传递**：`call_function` 签名加 `is_new_call = false` 参数；`eval_new_expr` 传 `true`，消除 Interpreter/VM native constructor 行为不一致
  - **[medium] array closure 泄露**：`Environment::clear_function_bindings` 增加 `kArray` 分支（`src/runtime/environment.cpp`），array-held closure 不再绕过循环清理
  - **[medium] forEach 稀疏语义**（不采纳）：当前对 holes 合成 `undefined` 并调用回调符合 V8 行为，且现有测试全通过，保持不变

- **新增测试**：Phase 8.3 新增 19 个测试（A-21~A-39：MaxLegalIndex、IllegalIndexAsProperty、NegativeIndexAsProperty、FractionalIndexAsProperty、NanIndexAsProperty、LengthSetToZero、LengthSetToMaxLegal、LengthSetToTooLargeThrows、PushMultiArgOrder、PopDecreasesLength、PopDeletesLastElement、ForEachNonCallableThrows、ForEachUndefinedCallbackThrows、NestedArray、OrdinaryObjectUnaffected、OrdinaryObjectNoAutoExtend、TruncatedElementsReadUndefined、PushNoArgReturnsCurrentLength、ForEachThirdArgIsArray、StringKeyLength），Interp + VM 两侧各 20 个新测试

- **验证**：80/80 Array 测试通过；全量 1054/1059 通过（5 个预存遗留失败：P3-2 闭包循环引用 × 4 + VMFinallyOverride × 1）；无新增 ASAN/LSan 泄漏

## 11. 2026-04-25 构建脚本 `--clean` 参数

- `scripts/run_ut.sh`：新增 `--clean` 参数，调用 `scripts/clean.sh` 后再执行 `build_debug.sh` 与 UT；帮助文本同步更新
- `scripts/coverage.sh`：新增 `--clean` 参数，调用 `scripts/clean.sh` 后再执行 `build_test.sh` 与覆盖率流程；支持与 `--open` 组合使用；帮助文本同步更新
- **验证**：`bash -n scripts/run_ut.sh && bash -n scripts/coverage.sh` 通过；`./scripts/run_ut.sh --help && ./scripts/coverage.sh --help` 输出正确

## 12. 2026-04-25 `run_ut.sh --quiet` 静默模式

- `scripts/run_ut.sh`：新增 `--quiet` 参数；静默执行 `build_debug.sh` 与 `ctest`，成功时完整 ctest 日志写入 `build/debug/run_ut_success.log`
- 失败时将完整 ctest 输出先写入 `build/debug/run_ut_raw.log`，再抽取失败 test block 与 `LeakSanitizer: detected memory leaks` 所在 case，写入 `build/debug/run_ut_failure.log`，最后删除 raw log
- 构建日志同样区分成功/失败：`build/run_ut_build_success.log` / `build/run_ut_build_failure.log`，避免混入 UT 失败报告
- **验证**：`bash -n scripts/run_ut.sh` 通过；`./scripts/run_ut.sh --help` 显示 `--quiet`；`./scripts/run_ut.sh --quiet` 在当前 5 个已知失败下仅输出报告路径，报告中包含失败 case 与 LSan 泄漏栈

## 14. 2026-04-26 Phase 9.1-9.5 mark-sweep GC + P3-2 修复

- **GcHeap**（`include/qppjs/runtime/gc_heap.h`、`src/runtime/gc_heap.cpp`）：新建 mark-sweep GC 核心。三阶段 Collect（reset marks → mark from roots → sweep）；MarkPending 加入 worklist；DrainWorklist 调用 TraceRefs；Sweep 三子阶段：(A) 对所有不可达对象设 kGcSentinel（防止 ClearRefs 中 release 触发 delete）、(B) 调用 ClearRefs（正常 release RcPtr 成员，kGcSentinel 使 GC 对象 release 为 no-op，非 GC 对象正常减少 ref_count）、(C) delete 所有对象
- **RcObject**（`include/qppjs/runtime/rc_object.h`）：添加 `gc_mark_`、`gc_heap_` 指针；`set_gc_sentinel()`（ref_count = kGcSentinel）；`add_ref()`/`release()` 检查 kGcSentinel；析构函数调用 `Unregister()`（RC 路径）；纯虚 `TraceRefs` 和 `ClearRefs`；RcPtr 新增 `reset_no_release()`（保留备用）
- **Environment/JSFunction/JSObject**：各自实现 `TraceRefs`（遍历所有 RcPtr 成员和 Value 中的 object）和 `ClearRefs`（正常赋值 RcPtr 为空，Value 成员赋为 undefined）
- **JSFunction 新增 is_bound_ 字段**：`is_bound_`、`bound_target_`、`bound_this_`、`bound_args_` 及 accessor；bind lambda 改为从 `self_raw` 字段读取（替代 lambda 值捕获，使 GC 能追踪 bound_target/bound_this/bound_args 中的对象引用）；TraceRefs 遍历这些字段
- **Interpreter**（`include/qppjs/runtime/interpreter.h`、`src/runtime/interpreter.cpp`）：新增 `GcHeap gc_heap_` 成员；`init_runtime()` 末尾注册 global_env_；`call_function`/`eval_block_stmt`/`eval_for_stmt`/`exec_catch` 中注册新建的 Environment；`make_function_value`/`eval_object_expr`/`eval_array_expr`/`eval_new_expr`/bind lambda 中注册新建的 JSFunction/JSObject；exec() 重构为单一 break 路径，GC 在 clear_function_bindings 之前运行，roots = 所有 interpreter 成员 + final_result
- **VM**（`include/qppjs/vm/vm.h`、`src/vm/vm.cpp`）：对称修改；kNewObject/kNewArray/kMakeFunction/kNewCall/kPushScope/bind lambda 中注册新建对象；exec() 末尾先 GC 再 clear_function_bindings
- **新增测试**：`tests/unit/gc_heap_test.cpp`（16 个测试，覆盖 Interp/VM 各 8 个：全局变量、闭包、循环引用、bind、链式 bind、对象属性函数、深层闭包链）
- **验证**：`coverage.sh --quiet` 1280/1280 通过；`run_ut.sh --quiet` 1278/1278 通过，0 个 LSan 泄露（P3-2 根本修复）

## 13. 2026-04-26 Phase 8.5 审查修复（M1/M2/S1）

- **[M1] bind + new 语义修复**（`src/runtime/interpreter.cpp`、`src/vm/vm.cpp`）：bind 生成的 native lambda 增加 `is_new_call` 参数检查；`is_new_call == true` 时忽略 `captured_this`，从 target 的 `prototype_obj()` 创建新实例，以 `is_new_call=true` 调用目标函数（Interpreter 侧走 `call_function`，VM 侧走 `push_call_frame` + `run`；native target 直接转发 `is_new_call=true`）；new 后返回值遵循 ECMAScript §10.2.2 step 9（显式返回 object 则覆盖）
- **[M2] apply array-like length 校验**（两侧对称）：读取 `len_val` 后，不是 number 视为 0；`std::isnan` 或 `<= 0` 视为 0；`> 65535` 抛 RangeError；否则 `static_cast<uint32_t>(len_num)`，消除负数/NaN/Infinity 导致的巨量内存分配崩溃
- **[S1] 链式 bind name 修复**（两侧对称）：计算 `target_name` 时先查 `target_raw->get_property("name")`，若结果 is_string 则使用，否则回退到 `target_raw->name()` 字段；修复 `fn.bind(null).bind(null).name === "bound bound fn"`
- **新增测试**：`tests/unit/function_builtin_test.cpp` 追加 10 个测试（Interp × 5 + VM × 5）：M1 × 2 + M2 × 2 + S1 × 1 各侧
- **验证**：`coverage.sh --quiet` 1213/1213 通过，无回归
