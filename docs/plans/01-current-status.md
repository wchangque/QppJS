# QppJS 当前开发状态（摘要）

轻量任务读本文件即可。`/implement` 流程或需要完整历史时，读 `docs/plans/01-current-status-detail.md`。

## 当前状态

| 项目 | 值 |
|------|----|
| 当前阶段 | Phase 8 进行中（8.1、8.2、8.3 已完成） |
| 测试计数 | 1054/1059 通过（5 个为预先存在的遗留失败） |
| 最近更新 | 2026-04-25 |
| 下一步 | Phase 8.4 — Object 内建方法 |

## 已知遗留问题

- **P2-1**：VM catch 参数与 catch 体共享同一 scope，规范要求两层独立作用域
- **P2-2**：VM `compile_labeled_stmt` 对非循环体的 labeled break 触发 `assert(false)`
- **P3-1**：`JSString` 二次堆分配（`std::string` 成员），已知技术债务，Phase 9 优化
- **P3-2**：Interpreter 闭包循环引用（`clone_env ↔ Cell ↔ JSFunction`），5256 字节，39 个分配，LSan 已验证；根因是 Cell 共享设计下引用计数无法打断循环，需要标记清除 GC（Phase 9）才能根本解决

## 最近完成

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

- [ ] Phase 8.4：Object 内建方法
- [ ] Phase 8.5：Function 内建方法
- [ ] Phase 8.6：VM catch 作用域修复（P2-1）
- [ ] Phase 8.7：VM labeled break 修复（P2-2）

## 阻塞

暂无

## 收尾检查清单

每次任务结束前，至少检查：
- 当前任务状态是否变化
- 本文件（摘要）是否已同步
- `docs/plans/01-current-status-detail.md` 最近完成内容是否已追加
- `docs/plans/02-next-phase.md` 是否仍正确
- 若阶段计划已调整，`docs/plans/00-roadmap.md` 是否已同步
