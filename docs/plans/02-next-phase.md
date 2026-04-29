# QppJS 下一阶段计划

本文件展开"下一阶段"的可执行内容，是从当前状态进入下一轮开发的直接行动卡。

## 1. 下一阶段

- 下一阶段：Phase 12（待定）
- Phase 11 + P2-A 全部完成（1618/1618 测试通过，0 LSan 失败）
- String.prototype 8 个方法已完成（1967/1967 测试通过，0 LSan 失败）
- 对应路线图：`docs/plans/00-roadmap.md`
- 当前事实源：`docs/plans/01-current-status.md`

## 2. 候选目标

- ~~**P2-A（遗留）**：已完成（2026-04-27）~~
- ~~**export async function 解析**：已完成（2026-04-28）~~
- P3-1：JSString 二次堆分配优化（小字符串内联）
- ~~**Array.map/filter/reduce/reduceRight**：已完成（2026-04-28）~~
- ~~**Array.find/findIndex/some/every/indexOf/includes**：已完成（2026-04-28）~~
- ~~**String.prototype 8 个方法**：已完成（2026-04-29）~~
- 更多内建对象（Array.sort/splice/slice、Number/Math 方法等）
- 动态 import()（依赖 Phase 11 Promise）
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
