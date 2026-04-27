# QppJS 下一阶段计划

本文件展开"下一阶段"的可执行内容，是从当前状态进入下一轮开发的直接行动卡。

## 1. 下一阶段

- 下一阶段：Phase 12（待定）
- Phase 11 全部完成（1596/1596 测试通过，0 LSan 失败，Review 验证通过）
- 对应路线图：`docs/plans/00-roadmap.md`
- 当前事实源：`docs/plans/01-current-status.md`

## 2. 候选目标

- **P2-A（遗留）**：async/await 真正异步顺序保证——await 后的代码应在下一个 microtask tick 执行，需要协程挂起/恢复机制（当前 await 是同步 DrainAll）
- **export async function 解析**（小修复）：parser.cpp `parse_export_decl()` 未处理 `async function` 声明
- P3-1：JSString 二次堆分配优化（小字符串内联）
- 更多内建对象（Array.map/filter/reduce、String 方法等）
- 动态 import()（依赖 Phase 11 Promise）
- Top-Level Await

## 3. 进入前提

当前已具备：
- Phase 1～11 全部完成（1596/1596 测试通过，0 LSan 失败）
- Promise/async/await 基础实现（P2-A 遗留）
- GC mark-sweep 已上线，P3-2 已根本修复
- ESM 静态 import/export 完整实现

## 4. 暂不纳入范围

- 完整宿主事件循环
- QuickJS 风格引用计数 + cycle collect 复刻
- 写屏障优化
- 分代 GC
