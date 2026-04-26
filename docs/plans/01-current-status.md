# QppJS 当前开发状态（摘要）

轻量任务读本文件即可。`/implement` 流程或需要完整历史时，读 `docs/plans/01-current-status-detail.md`。

## 当前状态

| 项目 | 值 |
|------|----|
| 当前阶段 | Phase 10.2 Review M1/M2/M3 修复完成（ESM live binding、TDZ、export default 具名函数绑定） |
| 测试计数 | 1458/1458 通过（coverage + run_ut），0 LSan 泄漏 |
| 最近更新 | 2026-04-26 |
| 下一步 | Phase 11（待定，可能是 P3-1 JSString 优化或更多内建对象） |

## 已知遗留问题

- ~~**P2-1**：已在 8.6 修复~~
- ~~**P2-2**：已在 8.7 修复~~
- **P3-1**：`JSString` 二次堆分配（`std::string` 成员），已知技术债务，Phase 10 优化
- ~~**P3-2**：已在 Phase 9.1-9.5 通过 mark-sweep GC 修复~~

## 最近完成

- [x] Phase 10.2 Review M1/M2/M3 修复：M1：`export { x as y }` live binding 修复（Load 阶段为 export_name 分配 Cell，Link 阶段以 local_name 为 key 注入 module_env，删除 exec_module_body 末尾快照逻辑，Interpreter + VM 两侧对称）；M2：`export let/const` TDZ 跨模块共享（Cell 增加 `initialized` 字段，`define_binding_with_cell` 增加 `initialized` 参数，`define_import_binding` 改为 `initialized=false`，`get()` 改为检查 `cell->initialized`，`initialize()` 同时设置两者；hoist 阶段为 var/function 标记 `cell->initialized=true`）；M3：`export default function foo(){}` 模块内 `foo` 绑定（AST `ExportDefaultDeclaration` 增加 `local_name` 字段，Parser 保留 fn_name，Interpreter hoist_module_vars 建立 foo Binding，执行时 set(foo, fn_val)；VM compiler emit kDup+kSetVar，exec_module_body 预定义 foo Binding）。新增 6 个测试（M36/M37/M38 各 Interp+VM 对称）。1458/1458 通过，0 LSan 泄漏。
- [x] Phase 10.2 Testing Agent 边界补测：追加 42 个测试（21 组 Interp+VM 对称）。新增覆盖：export var live binding、export function 提升（同时修复 Interpreter 侧 hoist_module_vars 未对 export function 做提升的 bug）、模块执行顺序、同一模块被多个 importer 共享 Cell、相对路径 `../` 跨目录、裸模块说明符报错、模块作用域隔离（非导出 let/var 不泄漏）、错误缓存两次 import 同一失败模块、多条 import 语句从同一模块导入、循环依赖中 var 无 TDZ、副作用模块确实被执行、re-export 别名、re-export live binding（Cell 共享）、默认+具名导出共存（分两条 import）、多别名 export specifier、导入不存在的默认导出报 SyntaxError、导入不存在的 re-export 名报 SyntaxError、export let 无初始值为 undefined、非导出 var 隔离、模块缓存幂等性（三个 importer 读同一模块只执行一次）。1452/1452 通过，0 LSan 泄漏。
- [x] Phase 10.2：ESM 模块系统实现。新增 `ModuleRecord`（继承 RcObject，kModule）、`ModuleLoader`（文件加载+缓存）；扩展 AST `ExportNamedDeclaration` 添加 `source` 字段（re-export）；Parser 支持 `export { v } from './a.js'`；扩展 Environment（`define_binding_with_cell`、`define_import_binding`）；Interpreter 和 VM 各自实现 Load/Link/Evaluate 三阶段；VM 新增 `kSetExportDefault` 指令；Compiler 修改 export function 提升逻辑；循环依赖、模块缓存、错误缓存、live binding、re-export、副作用导入全部实现。新增 36 个测试（18 Interp + 18 VM），1410/1410 通过，0 LSan 泄漏。
- [x] Phase 10.1 Review M1/M2/M3 修复：M1：VM compiler 三个 no-op visitor 改为 emit kLoadString+kThrow，执行时产生运行时错误（与 interpreter stub 行为对齐）；M2：从 lexer kKeywords 移除 import/export（改为 contextual keyword），在 parse_stmt() 入口用文本比较识别，修复 `({ import: 1 }).import`/`obj.export` 等合法属性名解析失败；M3：labeled statement body 解析前将 is_top_level_ 置 false（先保存后恢复），修复 `label: import './m'` 被错误接受。新增 4 个测试（M2 回归 ×2 + M3 回归 ×2）。1375/1375 通过，0 回归。
- [x] Phase 10.1 Testing Agent 边界补测：追加 39 个测试，覆盖 import/export 错误路径（缺少 `}`/`as`/`from`/specifier、非法 token）、合法边界（空 `{}`、尾随逗号、混合 alias、`from` 作为 local name、路径 specifier）、export 边界（`let`/`var` 形式、空 specifier 列表、`default` 表达式类型、具名函数含参数）、重复导出混合场景、`is_top_level_` 在 `if`/`while`/`for` non-block 语句体内的遗漏（修复 parser.cpp 三处）、ASI 行为、多语句顺序、contextual keyword 不污染普通标识符。1371/1371 通过，0 回归。
- [x] Phase 10.1：import/export 语法节点与 Parser 扩展。新增 `KwImport`/`KwExport` token，扩展 `StmtNode` variant（`ImportDeclaration`、`ExportNamedDeclaration`、`ExportDefaultDeclaration`），Parser 实现全部 5 种 import 形式和 4 种 export 形式，`is_top_level_` 检查确保 import/export 只在顶层，重复导出名检查，`from`/`as`/`default` 作为 contextual keyword，interpreter/compiler 添加 stub。新增 20 个测试，1332/1332 通过，0 回归。
- [x] Phase 9 GC Review M1/M2 修复：`GcHeap::Collect()` Phase 1 同时重置 roots 的 `gc_mark_`（修复长期根对象第二次 exec 后子对象被误回收的 UAF）；interpreter.cpp 和 vm.cpp 中 Object.keys/Object.create/Object() 构造器的 native lambda 内新分配的 JSObject 补加 `gc_heap_.Register()`（vm.cpp create_fn 同步改为 `[this]` 捕获）；追加 4 个测试（M1/M2 各 Interp+VM 对称）。1312/1312 通过，run_ut 1306/1306 通过，0 个 LSan 泄露。
- [x] Phase 9.1-9.5 Testing Agent 边界补测：在 `tests/unit/gc_heap_test.cpp` 追加 28 个测试（14 组 Interp+VM 对称）。新增覆盖：对象自环/二元环 GC 回收、exec() 返回值作为 GC 根不被误回收、throw 路径 GC 不崩溃、try/catch 捕获对象 GC 后可访问、数组持有函数 GC 后可调用、闭包捕获数组 GC 后可访问、named function expression 递归 GC 后正确、bound function 作为构造器 GC 后实例属性正确、大量短生命周期对象不积累内存、原型链 GC 后 instanceof 正确、多次 exec() 调用 GcHeap 状态不残留、空程序 GC 不崩溃、三重闭包循环 kGcSentinel no-op 不 double-free。1308/1308 通过，run_ut 1306/1306 通过，0 个 LSan 泄露。
- [x] Phase 9.1-9.5：mark-sweep GC 实现 + P3-2 修复。新增 `GcHeap`（gc_heap.h/cpp），三阶段 mark-sweep（reset → mark from roots → sweep）；RcObject 添加 `gc_mark_`、`gc_heap_` 指针、`set_gc_sentinel()`、纯虚 `TraceRefs` 和 `ClearRefs`；JSObject/JSFunction/Environment 各自实现 TraceRefs（遍历子对象）和 ClearRefs（正常 release RcPtr 成员，kGcSentinel 确保 GC 对象 release 是 no-op）；JSFunction 新增 `is_bound_` 字段及 accessor，bind lambda 从显式字段读取（替代 lambda 捕获，解决 GC 无法追踪 lambda 捕获的问题）；Interpreter 和 VM 各自注册执行期间创建的对象（fn_env、block_env、for_env、catch_env、JSFunction、JSObject），exec() 末尾先 GC（roots = 所有 interpreter 成员 + final_result）再 clear_function_bindings；新增 `tests/unit/gc_heap_test.cpp`（16 个测试）。1280/1280 通过，run_ut 1278/1278 通过，0 个 LSan 泄露（P3-2 根本修复）。
- [x] Phase 9.0 测试补充（Testing Agent）：新增 `tests/unit/env_rc_lifecycle_test.cpp`，45 个测试覆盖 RcPtr<Environment> 引用计数正确性与生命周期安全：kPopScope 自我赋值修复回归（深度嵌套 block/for 循环）、闭包 outer_ 链不提前释放（工厂返回后读写、三层链不断裂）、多闭包共享 env 独立性、named function expression 自引用语义、try/catch/finally 各路径作用域弹出、break/continue/labeled-break 路径作用域弹出、函数调用帧恢复、TDZ 错误路径、压力测试（50 个闭包）。1264/1264 通过，run_ut 4 个 LSan 失败（P3-2 遗留，不增加）。
- [x] Phase 9.0：Environment 从 shared_ptr 迁移到 RcPtr<Environment>（RcObject 体系）。追加 ObjectKind::kEnvironment；Environment 继承 RcObject；outer_/closure_env_ 等字段全部改为 RcPtr<Environment>；interpreter.h/cpp 和 vm.h/vm.cpp 同步迁移；修复 kPopScope 的自我赋值 SEGFAULT（先拷贝 outer 再赋值）。1219/1219 通过，run_ut 4 个 LSan 失败（P3-2 遗留，不增加）。
- [x] Phase 8.6/8.7：VM catch 作用域修复（P2-1）+ VM labeled break 修复（P2-2）。compile_try_stmt 两处 catch 分支改用 compile_block_stmt（外层 scope 绑定 catch 参数，内层 scope 由 compile_block_stmt 按需创建）；compile_labeled_stmt else 分支注册到 loop_env_stack_，支持 labeled block break。新增 6 个测试，1219/1219 通过。
- [x] Phase 8.5 审查修复（M1/M2/S1）：bind 生成函数用作构造器时正确忽略 bound_this、创建新实例（Interpreter 和 VM 两侧对称）；apply array-like 分支对负数/NaN/Infinity length 做 ToLength 语义校验（视为 0，>65535 抛 RangeError）；链式 bind name 优先读 own_properties_["name"] 修复 "bound bound foo"。新增 10 个测试（Interp 5 + VM 5），1213/1213 通过。
- [x] Phase 8.5：Function 内建方法 — `Function.prototype.call`、`apply`、`bind`，Interpreter 和 VM 两侧对称。新增 `function_prototype_` 成员（JSObject），注册 call/apply/bind；eval_member_expr/kGetProp 的 kFunction 分支加 function_prototype_ 二次查找；bind 用 native_fn_ lambda 封装（捕获 target/bound_this/bound_args），支持二次 bind；apply 支持 kArray 和 array-like（kOrdinary + length 属性）展开；exec/run 清理路径同步添加 function_prototype_ 清理。新增 32 个测试（16 Interp + 16 VM），1171/1171 通过。
- [x] Phase 8.4 审查修复（M1/M2/M3）：Object 构造函数原型链修复（`new Object() instanceof Object` → true）、Object.create 对函数参数抛 TypeError、Object.assign 对数组 target 走 set_property_ex，新增 8 个测试，1139/1139 通过
- [x] Phase 8.4：Object 内建方法 — Object.keys / Object.assign / Object.create，Interpreter 和 VM 两侧对称，新增 42 个测试（21 Interp + 21 VM），1103/1103 通过。新增 JSFunction 属性字典（`own_properties_`），修复 Interpreter `eval_call_expr` 和 `eval_member_expr` 中 kFunction 属性读取，修复 VM `GetProp` 中 kFunction 属性读取；Object 注册为可覆盖绑定（`define_initialized`）避免用户重定义冲突
- [x] 闭包环境共享修复 + named function expression 自引用修复：删除 `clone_for_closure` / `clone_closure_env` / `define_binding` 整套克隆机制，`MakeFunction` 直接共享当前 env；解释器/VM 同步修复。新增 `is_named_expr` 标记，named function expression 在 `fn_env` 本层无条件注册自引用绑定（不走 `lookup` 链）。coverage 1061/1061 全部通过；run_ut 4 个 LSan 泄露为预先存在的 P3-2 遗留问题
- [x] `scripts/qppjs.py` `split_log` 重构：提取上下文管理器统一"写 raw → 成功 rename / 失败分流"逻辑，`TestRunner` 和 `CoverageRunner` 三处重复代码消除
- [x] build skill 工具使用规范：明确 `coverage.sh` 用于 UT 功能验证（无 ASAN/LSan 噪音），`run_ut.sh` 专用于内存泄露检查；更新 SKILL.md 和 CLAUDE.md 快速参考
- [x] 构建脚本 Python 统一入口重构：新增 `scripts/qppjs.py` 覆盖 `clean`、`build debug/release/test`、`test --clean --quiet`、`coverage --clean --quiet --open`；支持 `clean build release` / `clean test --quiet` / `clean coverage --quiet` 前置组合用法；原 shell 脚本保留为兼容 wrapper；帮助信息已补充命令说明、构建类型解释和常用示例；quiet 模式区分成功/失败日志（UT：ctest 日志为 `build/debug/run_ut_success.log` / `build/debug/run_ut_failure.log`，构建日志为同目录下的 `run_ut_build_success.log` / `run_ut_build_failure.log`；coverage：成功日志为 `build/coverage_success.log`，失败摘要为 `build/coverage_failure.log`）；已验证语法、帮助输出、release/test 构建与静默失败报告
- [x] `scripts/qppjs.py coverage --quiet` 日志行为修复：保留实际失败路径 `build/coverage_failure.log` / 成功路径 `build/coverage_success.log`；quiet 模式改为先写原始日志到临时文件，失败时仅提取失败 UT / 泄漏摘要写入 `coverage_failure.log`，成功时再落盘为 `coverage_success.log`；复用 `run_ut` 的失败摘要格式；已验证 `python3 -m py_compile scripts/qppjs.py`
- [x] `run_ut.sh --quiet` 静默模式：构建和 ctest 输出静默化；失败时仅提示报告路径，并将失败 case / LSan 泄漏信息写入 `build/debug/run_ut_failure.log`；已通过语法、帮助输出和实际失败路径验证
- [x] 构建脚本 `--clean` 参数：`run_ut.sh` 与 `coverage.sh` 支持先调用 `scripts/clean.sh` 再构建；已通过 `bash -n` 与 `--help` 输出验证
- [x] Phase 8.3 bug 修复 + adversarial review 采纳：
  - **崩溃修复**：`elements_` 从 `vector<Value>` 改为 `unordered_map<uint32_t, Value>` 稀疏存储，彻底消除大索引（`arr[4294967294]`、`arr.length=4294967295`）触发的巨量内存分配
  - **内存泄露修复**：Interpreter 侧 6 处 `clear_function_properties` 调用补全 `array_prototype_` 清理；macOS 系统库误报通过 `lsan_suppressions.txt` 屏蔽
  - **push overflow 保护**（adversarial review [high]）：push 前检查 `array_length_ == UINT32_MAX`，溢出时返回 `RangeError`
  - **is_new_call 传递修复**（adversarial review [medium]）：`call_function` 加 `is_new_call` 参数，`eval_new_expr` 传 `true`，消除 Interpreter/VM 行为不一致
  - **array closure 泄露修复**（adversarial review [medium]）：`Environment::clear_function_bindings` 增加 `kArray` 分支，array-held closure 不再绕过循环清理
  - 1054/1059 通过（5 个为预先存在的遗留失败：P3-2 闭包循环引用 × 4 + VMFinallyOverride × 1）
- [x] Phase 8.3：Array 基础 — 数组字面量、下标读写、length（含 RangeError 校验）、push/pop/forEach，Interpreter 和 VM 两侧对称，新增 40 个测试（20 Interp + 20 VM），1017/1021 通过
- [x] Phase 8.2 边界测试补充：新增 28 个 console 边界用例（NaN/Infinity/-Infinity 通过算术表达式、-0、负整数、空字符串、含空格字符串、5 参数、连续调用、log 赋值变量调用、返回 undefined、回归全局查找），977/981 通过；同时发现 NaN/Infinity 全局标识符未注册（已记录为待修复项）
- [x] Phase 8.2：console 对象 — 在 Interpreter 和 VM 两侧注册全局 console 对象，支持 console.log(...args)，20 个测试全部通过
- [x] 闭包边界测试补充：新增 10 个 function_test 边界用例（三层嵌套捕获、var 遮蔽外层 let、具名函数表达式递归、多工厂调用独立性等），927/931 通过
- [x] macOS LSan 基础设施：`run_ut.sh` 加 `ASAN_OPTIONS=detect_leaks=1`，确认 Interpreter 闭包循环引用泄漏（5256 字节）为已知遗留问题（需 GC 才能根本解决，记为 P3-2）
- [x] 函数/闭包/原型相关 ASAN/LSan 泄漏修复：Interpreter 与 VM 统一清理 closure env / 对象属性中的函数引用，分离 VM `function_decls` 与 `var_decls`，相关泄漏回归通过
- [x] Phase 8.1：Error 子类（TypeError/ReferenceError/RangeError）+ instanceof — 完成（917/917，含 Review M1/M2/M3 修复）
- [x] 构建脚本跨平台探测修复：无 brew 的 Linux/WSL 环境不再因 `brew --prefix llvm` 直接退出，3 个构建脚本验证通过

## 未开始

- Phase 11（Promise / Async）

## 阻塞

暂无

## 收尾检查清单

每次任务结束前，至少检查：
- 当前任务状态是否变化
- 本文件（摘要）是否已同步
- `docs/plans/01-current-status-detail.md` 最近完成内容是否已追加
- `docs/plans/02-next-phase.md` 是否仍正确
- 若阶段计划已调整，`docs/plans/00-roadmap.md` 是否已同步
