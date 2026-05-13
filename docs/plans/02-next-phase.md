# QppJS 下一阶段计划

本文件展开"下一阶段"的可执行内容，是从当前状态进入下一轮开发的直接行动卡。

## 1. 下一阶段

- 下一阶段：Phase 12 — Array.sort/splice/slice 内建方法，或 QuickJS 风格优化调研
- import.meta 已完成（2512/2512 测试通过，0 LSan 泄漏）
- 所有 Phase 1～11 已全部完成
- 对应路线图：`docs/plans/00-roadmap.md`
- 当前事实源：`docs/plans/01-current-status.md`

## 2. 候选目标

- ~~**import.meta**：已完成（2026-05-13）~~
- ~~**P2-A（遗留）**：已完成（2026-04-27）~~
- ~~**export async function 解析**：已完成（2026-04-28）~~
- ~~**P3-1**：已完成（2026-05-13）—— JSString SSO 布局~~
- ~~**Array.map/filter/reduce/reduceRight**：已完成（2026-04-28）~~
- ~~**Array.find/findIndex/some/every/indexOf/includes**：已完成（2026-04-28）~~
- ~~**String.prototype 8 个方法**：已完成（2026-04-29）~~
- ~~**Number/Math 内建对象**：已完成（2026-04-29）~~
- 更多内建对象（Array.sort/splice/slice 等）
- ~~动态 import()（依赖 Phase 11 Promise）~~（已完成 2026-05-13）
- Top-Level Await

## 3. 进入前提

当前已具备：
- Phase 1～11 + P2-A 全部完成（1891/1891 测试通过，0 LSan 失败）
- Array.map/filter/reduce/reduceRight 已完成（2026-04-28）
- Array.find/findIndex/some/every/indexOf/includes 已完成（2026-04-28）
- export async function 解析修复已完成（2026-04-28）
- Promise/async/await 完整实现（含真正异步顺序保证）
- GC mark-sweep 已上线，P3-2 已根本修复
- ESM 静态 import/export 完整实现
- 稀疏数组 hole 语义修复（Parser elision 正确处理）

## 4. 暂不纳入范围

- 完整宿主事件循环
- QuickJS 风格引用计数 + cycle collect 复刻
- 写屏障优化
- 分代 GC
