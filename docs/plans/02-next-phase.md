# QppJS 下一阶段计划

本文件展开"下一阶段"的可执行内容，是从当前状态进入下一轮开发的直接行动卡。

## 1. 下一阶段

- 下一阶段：Phase 9（GC）
- Phase 8 已全部完成（1219/1219 测试通过）
- 对应路线图：`docs/plans/00-roadmap.md`
- 当前事实源：`docs/plans/01-current-status.md`

## 2. 阶段目标

引入简单、正确、可理解的垃圾回收模型，解决 P3-2 闭包循环引用内存泄漏问题。

## 3. 进入前提

当前已具备：
- Phase 1～8 全部完成（1219/1219 测试通过）
- 已知遗留问题：P3-2（闭包循环引用，4 个 LSan 失败），需要 GC 解决

## 4. 本阶段任务分解

### 9.1 明确 root 集合

目标：
- 确定 GC root：全局环境、调用栈中的 Value、native 函数持有的 Value 等
- 设计对象图遍历接口（mark 方法）

### 9.2 实现第一版 mark-sweep GC

目标：
- 实现最小 mark-sweep：从 root 出发标记可达对象，sweep 回收不可达对象
- 不要求高性能，要求正确性和可理解性

### 9.3 验证对象图与闭包环境可达性

目标：
- 确认 Environment 链、JSFunction closure_env_、JSObject proto_ 均在 mark 阶段被正确遍历
- 验证循环引用可被正确回收

### 9.4 建立最小 GC 回归测试

目标：
- 新增 GC 触发与回收的基础测试
- 验证 GC 后对象仍可正常访问（可达对象不被误回收）

### 9.5 修复 P3-2 闭包循环引用内存泄漏

目标：
- GC 上线后，LSan 确认 `FunctionTest.TwoClosuresFromSameFactoryShareBinding` 等场景无泄漏
- run_ut 全量通过，0 个 LSan 失败

## 5. 建议执行顺序

9.1 → 9.2 → 9.3 → 9.4 → 9.5

## 6. 退出条件

- 基本对象图与闭包环境可正确回收
- P3-2 修复：LSan 全量 0 失败
- 原有 1219 个测试无回归
- 状态文档已同步

## 7. 暂不纳入范围

- QuickJS 风格引用计数 + cycle collect 复刻
- 写屏障优化
- 分代 GC
