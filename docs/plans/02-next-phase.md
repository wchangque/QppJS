# QppJS 下一阶段计划

本文件展开"下一阶段"的可执行内容，是从当前状态进入下一轮开发的直接行动卡。

## 1. 下一阶段

- 下一阶段：Phase 10.2（ESM 运行时语义，待定）
- Phase 10.1 已完成（1332/1332 测试通过）
- 对应路线图：`docs/plans/00-roadmap.md`
- 当前事实源：`docs/plans/01-current-status.md`

## 2. 阶段目标

Phase 10.2 候选方向：
- ESM 模块加载器（静态 import/export 语义）
- P3-1：JSString 二次堆分配优化（小字符串内联）
- 更多内建对象（Array.map/filter/reduce、String 方法等）

## 3. 进入前提

当前已具备：
- Phase 1～10.1 全部完成（1332/1332 测试通过，0 LSan 失败）
- GC mark-sweep 已上线，P3-2 已根本修复
- import/export AST 节点与 Parser 已就绪，可供 Phase 10.2 消费

## 4. 暂不纳入范围

- QuickJS 风格引用计数 + cycle collect 复刻
- 写屏障优化
- 分代 GC
