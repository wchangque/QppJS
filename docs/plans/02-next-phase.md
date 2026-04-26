# QppJS 下一阶段计划

本文件展开"下一阶段"的可执行内容，是从当前状态进入下一轮开发的直接行动卡。

## 1. 下一阶段

- 下一阶段：Phase 11（Promise / Async）
- Phase 10 已全部完成（1458/1458 测试通过，0 LSan 失败）
- 对应路线图：`docs/plans/00-roadmap.md`
- 当前事实源：`docs/plans/01-current-status.md`

## 2. 阶段目标

Phase 11 子任务：
- 11.1 设计 Job Queue（微任务队列）
- 11.2 实现 Promise 最小子集（resolve/reject/then/catch/finally）
- 11.3 实现 async/await 基础语义
- 11.4 补充异步执行样例与测试

也可先做以下技术债务：
- P3-1：JSString 二次堆分配优化（小字符串内联）
- 更多内建对象（Array.map/filter/reduce、String 方法等）

## 3. 进入前提

当前已具备：
- Phase 1～10 全部完成（1458/1458 测试通过，0 LSan 失败）
- GC mark-sweep 已上线，P3-2 已根本修复
- ESM 静态 import/export 完整实现（Load/Link/Evaluate 三阶段，live binding，循环依赖，re-export）
- 异常控制流（throw/try/catch/finally）已完整实现

## 4. 暂不纳入范围

- 完整宿主事件循环
- 动态 import()（依赖 Promise，Phase 11 后再做）
- Top-Level Await（依赖 async/await）
- QuickJS 风格引用计数 + cycle collect 复刻
- 写屏障优化
- 分代 GC
