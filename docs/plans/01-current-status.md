# QppJS 当前开发状态（摘要）

轻量任务读本文件即可。`/implement` 流程或需要完整历史时，读 `docs/plans/01-current-status-detail.md`。

## 当前状态

| 项目 | 值 |
|------|----|
| 当前阶段 | Phase 7 已完成，可进入 Phase 8 |
| 测试计数 | 792/792 通过 |
| 最近更新 | 2026-04-23 |
| 下一步 | Phase 8.1 — Error 子类（TypeError/ReferenceError/RangeError） |

## 已知遗留问题

- **P2-1**：VM catch 参数与 catch 体共享同一 scope，规范要求两层独立作用域
- **P2-2**：VM `compile_labeled_stmt` 对非循环体的 labeled break 触发 `assert(false)`
- **P2-3**：内部运行时错误（ReferenceError/TypeError）以字符串值抛出，而非 Error 对象

## 未开始

- [ ] Phase 8：基础内建对象（8.1 Error 子类 → 8.2 console → 8.3 Array → 8.4 Object → 8.5 Function → 8.6 P2-1 → 8.7 P2-2）

## 阻塞

暂无

## 收尾检查清单

每次任务结束前，至少检查：
- 当前任务状态是否变化
- 本文件（摘要）是否已同步
- `docs/plans/01-current-status-detail.md` 最近完成内容是否已追加
- `docs/plans/02-next-phase.md` 是否仍正确
- 若阶段计划已调整，`docs/plans/00-roadmap.md` 是否已同步
