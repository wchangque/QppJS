# QppJS 下一阶段计划

本文件展开"下一阶段"的可执行内容，是从当前状态进入下一轮开发的直接行动卡。

## 1. 下一阶段

- 下一阶段：test262 通过率提升（按优先级逐项实现缺失特性）
- `++`/`--` 运算符已完成（2708/2708 测试通过，0 LSan 泄漏）
- 对应路线图：`docs/plans/00-roadmap.md`
- 当前事实源：`docs/plans/01-current-status.md`

## 2. test262 通过率提升候选目标（按优先级）

1. ~~**正则表达式 RegExp 运行时** — 已完成（2026-05-18）~~
2. ~~**模板字符串 `` ` ``** — 已完成（2026-05-18，仅无标签形式；Tagged Template 暂不支持）~~
3. ~~**`Array` + `String` + `Boolean` 全局构造函数** — 已完成（2026-05-19）~~
4. ~~**`delete` 运算符** — 已完成（2026-05-19）~~
5. ~~**`Symbol` 基础支持** — 已完成（2026-05-19）~~
6. ~~**`arguments` 对象** — 已完成~~
7. ~~**`Object.defineProperty` / `Object.getOwnPropertyDescriptor`** — 已完成（2026-05-19）~~
8. ~~**箭头函数 `()=>`** — 已完成（2026-05-19）~~

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
