# QppJS 当前开发状态（摘要）

轻量任务读本文件即可。`/implement` 流程或需要完整历史时，读 `docs/plans/01-current-status-detail.md`。

## 当前状态

| 项目 | 值 |
|------|----|
| 当前阶段 | Phase 8 进行中（8.1 已完成） |
| 测试计数 | 917/917 通过 |
| 最近更新 | 2026-04-24 |
| 下一步 | Phase 8.2 — console 对象 |

## 已知遗留问题

- **P2-1**：VM catch 参数与 catch 体共享同一 scope，规范要求两层独立作用域
- **P2-2**：VM `compile_labeled_stmt` 对非循环体的 labeled break 触发 `assert(false)`
- **P3-1**：`JSString` 二次堆分配（`std::string` 成员），已知技术债务，Phase 9 优化
- **P3-2**：循环引用（proto 链、closure env）导致内存泄漏，Phase 9 GC 解决

## 最近完成

- [x] Phase 8.1：Error 子类（TypeError/ReferenceError/RangeError）+ instanceof — 完成（917/917，含 Review M1/M2/M3 修复）
- [x] 构建脚本跨平台探测修复：无 brew 的 Linux/WSL 环境不再因 `brew --prefix llvm` 直接退出，3 个构建脚本验证通过

## 未开始

- [ ] Phase 8.2：console 对象
- [ ] Phase 8.3：Array 基础
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
