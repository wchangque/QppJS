# QppJS 当前开发状态（详情）

本文件是 `01-current-status.md` 的完整版，供 `/implement` 流程和需要完整历史的场景使用。

## 1. 已完成任务

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
