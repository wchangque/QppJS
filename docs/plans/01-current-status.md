# QppJS 当前开发状态（摘要）

轻量任务读本文件即可。`/implement` 流程或需要完整历史时，读 `docs/plans/01-current-status-detail.md`。

## 当前状态

| 项目 | 值 |
|------|----|
| 当前阶段 | Phase 7 已完成，可进入 Phase 8 |
| 测试计数 | 1516/1516 通过 |
| 最近更新 | 2026-04-24 |
| 下一步 | Phase 8.1 — Error 子类（TypeError/ReferenceError/RangeError） |
| 最近优化 | P1-6: ostringstream→number_to_string(charconv)；P1-4: kPushScope 跳过无 let/const 的块 |

## 已知遗留问题

- **P2-1**：VM catch 参数与 catch 体共享同一 scope，规范要求两层独立作用域
- **P2-2**：VM `compile_labeled_stmt` 对非循环体的 labeled break 触发 `assert(false)`
- **P2-3**：内部运行时错误（ReferenceError/TypeError）以字符串值抛出，而非 Error 对象
- **P3-1**（新）：`JSString` 二次堆分配（`std::string` 成员），已知技术债务，Phase 9 优化
- **P3-2**（新）：循环引用（proto 链、closure env）导致内存泄漏，Phase 9 GC 解决

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
