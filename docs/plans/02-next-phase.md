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

**新一批候选目标（第二轮）：**

9. ~~**`for...in` 循环** — 已完成（2026-05-20）~~
10. ~~**`for...of` 循环** — 已完成（2026-05-21）~~
11. ~~**展开运算符 `...`** — 已完成（2026-05-25）~~
12. ~~**rest 参数 `...args`** — 已完成（2026-05-25）~~
13. ~~**默认参数值 `(a = 1)`** — 已完成（2026-05-25）~~
14. ~~**解构赋值** — 已完成（2026-05-25，3694/3694 测试通过，0 LSan 泄漏）~~
15. ~~**三元运算符 `?:`** — 已完成（2026-05-20）~~
16. ~~**`in` 运算符** — 已完成（2026-05-26，3733/3733 测试通过，0 LSan 泄漏）~~
17. ~~**位运算符 `&` `|` `^` `~` `<<` `>>` `>>>` 及复合赋值** — 已完成（2026-05-26，含 Review M1/M2/M3 修复，3919/3919 测试通过，0 LSan 泄漏）~~

**第三轮候选目标（test262 通过率提升）：**

18. ~~**方法简写 `{foo() {}}` [T262-P1]** — 已完成（2026-05-26，32 测试，3949/3949 通过）~~
19. ~~**计算属性键 `{[expr]: val}` [T262-P2]** — 已完成（2026-05-26，4053/4053 通过，0 LSan 泄漏）~~
19. ~~**`??` nullish coalescing [T262-P3]** — 已完成（2026-05-27，4132/4132 通过，0 LSan 泄漏）~~
20. ~~**`?.` optional chaining [T262-P4]** — 已完成（2026-05-27，59 个测试，4191/4191 通过，0 LSan 泄漏）~~
21. ~~**Generator functions `function*` / `yield` / `yield*` [T262-P5]** — 已完成（2026-05-28，43 个测试，4232/4232 通过，0 LSan 泄漏）~~
22. ~~**JavaScript class 语法基础 + Review 必修修复 M1-M6 [T262-P6]** — 已完成（2026-06-01，112 个测试，4344/4344 通过，0 LSan 泄漏）~~
23. ~~**Logical Assignment Operators (&&=, ||=, ??=) + Tagged Template Literals** — 已完成（2026-06-01，57 个新测试（LA-01～LA-15 × Interp+VM + TTL-01～TTL-10 × Interp+VM + 3 Parser），4403/4403 通过，0 LSan 泄漏）~~
24. ~~**ES2022 Class Public Instance Fields & Static Fields** — 已完成（2026-06-01，38 个新测试（CF-01～CF-17 × Interp+VM + 2 Extra），4441/4441 通过，0 LSan 泄漏）~~
25. ~~**Exponentiation `**`/`**=` + async generator `async function*`** — 已完成（2026-06-01，23+16=39 个新测试，4480/4480 通过，0 LSan 泄漏）~~
26. ~~**Map、Set、WeakMap、WeakSet 内建对象** — 已完成（2026-06-01，90 个新测试（MS-01～MS-45 × Interp+VM），4570/4570 通过（coverage），4568/4568 通过（run_ut ASAN），0 LSan 泄漏）~~
27. ~~**ES2022 Class Private Fields（`#x`）** — 已完成（2026-06-02，20 个新测试（PF-01～PF-10 × Interp+VM），4590/4590 通过（coverage），0 LSan 泄漏）~~
28. ~~**Promise 静态方法（all/race/allSettled/any）+ Array.from 完整 + Array.of + Object.entries/values/fromEntries/getOwnPropertyNames** — 已完成（2026-06-02，40 个新测试（PAO-01～PAO-20 × Interp+VM），4630/4630 通过（coverage），0 LSan 泄漏（预期））~~
29. ~~**Number.prototype 方法 + String.prototype 缺失方法** — 已完成（2026-06-02，50 个新测试（NS-01～NS-25 × Interp+VM），4680/4680 通过（coverage），0 LSan 泄漏（预期））~~
30. ~~**5 组内建功能补充（globalThis/Object.is/setPrototypeOf/hasOwn/Array.at/JSON/queueMicrotask）** — 已完成（2026-06-02，32 个新测试（GJ-01～GJ-16 × Interp+VM），4712/4712 通过（coverage），0 LSan 泄漏（预期））~~
31. ~~**3 组功能补充（Symbol WKS + Symbol.toPrimitive/hasInstance/toStringTag + Function constructor + eval）** — 已完成（2026-06-02，34 个新测试（SE-01～SE-15 × Interp+VM），4746/4746 通过（coverage），0 LSan 泄漏（预期））~~
32. ~~**switch 语句（switch/case/default/break/fallthrough 语义）** — 已完成（2026-06-02，24 个新测试（SW-01～SW-12 × Interp+VM），4770/4770 通过（coverage），0 LSan 泄漏（预期））~~

## 3. 进入前提

当前已具备：
- Phase 1～11 + P2-A 全部完成（1891/1891 测试通过，0 LSan 失败）
- for...of 循环已完成（2026-05-21，3489/3489 测试通过，0 LSan 泄漏）
- 展开运算符 Spread/Rest 已完成（2026-05-25，3551/3551 测试通过，0 LSan 泄漏）
- 函数默认参数值已完成（含 Review 必修修复 M1/M2/M3/M4，2026-05-25，3601/3601 测试通过，0 LSan 泄漏）
- in 运算符已完成（2026-05-26，3733/3733 测试通过，0 LSan 泄漏）
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
