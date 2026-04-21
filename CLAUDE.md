# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working in this repository.

## 默认语言

- 默认使用中文与用户沟通，除非用户明确要求使用其他语言。

## 项目背景

- 当前项目目标：基于 QuickJS 的设计思想，通过 AI 辅助，从零用 C++ 实现一个 JavaScript 引擎。
- 参考仓库位于 `/home/wuzhen/code/QuickJS`，可在需要理解引擎结构、模块划分、测试方式和实现取舍时作为主要对照。
- 本仓库不是对 QuickJS 的简单封装，而是面向“理解并重建 JS 引擎关键机制”的独立实现项目。

## 计划与状态文档

项目当前使用以下文档维护长期路线、当前状态与下一阶段计划：

- `docs/plans/00-roadmap.md`：长期路线图，也是阶段与任务编号的唯一来源。
- `docs/plans/01-current-status.md`：当前真实开发状态，是新 session 接续工作的首要入口。
- `docs/plans/02-next-phase.md`：下一阶段的执行卡，用于明确近期要做什么。

文档优先级如下：

1. `docs/plans/01-current-status.md`
2. `docs/plans/02-next-phase.md`
3. `docs/plans/00-roadmap.md`

规则：

- 新 session 开始实现前，必须先读取 `docs/plans/01-current-status.md` 和 `docs/plans/02-next-phase.md`。
- 如需理解全局背景，再回看 `docs/plans/00-roadmap.md`。
- 每次任务结束前，必须更新 `docs/plans/01-current-status.md`。
- 如果当前阶段推进、下一步已明确，或阶段计划发生调整，也必须同步更新 `docs/plans/02-next-phase.md`。
- 如果实际执行偏离路线图，先更新 `docs/plans/00-roadmap.md`，再更新状态与下一阶段文档。
- 只有在计划与状态文档已同步后，才能把任务标记为完成，或向用户宣称该轮工作已收尾。

## 任务收尾机制

项目当前采用“任务完成 / 本轮任务收尾”作为状态更新主触发点，而不是 commit。

原因：

- commit 是版本控制动作，不等于一个任务在语义上完成。
- 一个任务可能对应多个 commit，也可能多个小改动共享一次 commit。
- 若把状态更新绑定到 commit，计划文档与真实任务状态容易错位。

收尾时至少检查：

1. 当前任务状态是否发生变化
2. `docs/plans/01-current-status.md` 是否已同步
3. `docs/plans/02-next-phase.md` 是否仍正确
4. 若阶段计划已变化，`docs/plans/00-roadmap.md` 是否已同步

后续如果人工漏更频繁，再考虑把这套检查下沉到 hooks、commands 或 skills。

## Agent Team 协作约定

项目默认采用 6 个职责分离的 agent team，并由主会话承担编排角色。

### 角色划分

- `ES Spec Agent`：阅读并总结 ECMAScript 规范，输出语义规则、边界行为、错误条件与最小测试点。
- `QuickJS Research Agent`：研究 `/home/wuzhen/code/QuickJS` 中与当前主题相关的实现，提炼实现路径、关键结构、可借鉴点与不建议照搬点。
- `Design Agent`：综合规范要求与 QuickJS 调研结果，为当前模块制定适合 QppJS 的实现方案。
- `Implementation Agent`：依据设计方案，以 TDD 方式完成当前范围内的最小实现。
- `Testing Agent`：补足测试矩阵、边界场景、异常路径与后续兼容性验证建议。
- `Review Agent`：独立审查实现是否偏离规范和设计，调用 `/codex:review` 和 `/codex:adversarial-review` 获取审查结果，并检查复杂度、可维护性与风险。

### 协作顺序

默认流程：

```
ES Spec Agent + QuickJS Research Agent
  → Design Agent
  → Implementation Agent（首次实现）
  → Testing Agent
  → Review Agent（首次审查，含 codex:review + codex:adversarial-review）
  → Implementation Agent（修复合理的必修问题）
  → Review Agent（验证审查，仅确认修复结果，不开新问题）
```

规则：

- `ES Spec Agent` 与 `QuickJS Research Agent` 可以并行。
- `Design Agent` 必须消费前两者的产出后再给出方案。
- `Implementation Agent`、`Testing Agent`、`Review Agent` 默认串行接力，避免上下文漂移和结论互相覆盖。
- 修复 + 验证只做一轮，验证审查结束后不再循环，防止无限迭代。

### 角色边界

- 研究型 agent 不写代码。
- `Implementation Agent` 不擅自修改需求或设计边界。
- `Testing Agent` 不放宽规范要求。
- `Review Agent` 可以否决实现，但不重写需求。
- 每个 agent 只接收上一步的明确产物，不依赖随意扩散的聊天上下文。
- 每个 agent 的输出应尽量标明对应任务编号、状态变化和建议下一步，便于主会话写回计划文档。

### 冲突裁决顺序

当多个 agent 的结论冲突时，按以下优先级处理：

1. ECMAScript 规范
2. 当前 QppJS 已确认的设计
3. QuickJS 的实现经验
4. 实现便利性

### 单次任务粒度

每次只处理一个足够小的主题，例如：

- tokenizer 的十进制整数字面量
- parser 的二元表达式优先级
- 基础值类型的表示方式
- 最小对象模型
- 函数调用时的参数绑定

不要把“parser 全部”或“runtime 全部”作为单次 agent team 任务。

### 当前推荐的落地方式

- 短期：由主会话按固定 prompt 调用 agent，先把工作流跑顺。
- 中期：把高频流程沉淀成可复用 skills 或模板。
- 后期：在构建和测试链路稳定后，再增加自动化调度、回归检查与定期扫描。

## 仓库现状

- 当前仓库已包含：`README.md`、本 `CLAUDE.md`、`.claude/` 目录下的规则文件、项目级 subagents、计划文档目录 `docs/plans/`，以及本地忽略配置 `.gitignore`。
- 尚未加入实际引擎源码、构建脚本、测试配置或可执行入口。
- 目前更准确地说，这是一个已经明确目标、参考对象、协作方式与阶段计划，但代码实现尚未开始的项目骨架。

## 参考仓库观察

参考仓库 `/home/wuzhen/code/QuickJS` 当前可见：

- 核心源码：`quickjs.c`、`qjs.c`、`qjsc.c`、`quickjs-libc.c` 等
- 支撑组件：Unicode、正则、数值处理、工具函数等源码文件
- 构建与测试：`Makefile`、`tests/`、`examples/`、test262 相关配置与运行器
- 文档：`README.md` 中包含运行方式、测试方式、运行时/字节码/C API 等说明

在本仓库开始实现时，可优先借鉴 QuickJS 在以下方面的设计取舍：

- 小型可嵌入的实现边界
- 运行时与上下文对象的组织方式
- 解释执行与字节码设计
- 内存管理与垃圾回收策略
- 测试组织方式（内置测试 + test262）

## 常用命令

由于当前仓库中尚未加入运行时、构建或测试工具链，目前没有可验证的构建、Lint 或测试命令。

当前可用的仓库检查命令：

- `git status` —— 查看工作区状态
- `git ls-tree -r --name-only HEAD` —— 列出当前提交中的已跟踪文件
- `git log --stat --oneline --decorate -n 5` —— 查看最近提交及其改动概览

参考仓库 `/home/wuzhen/code/QuickJS` 中可用的典型命令（仅供参考，不属于当前仓库命令）：

- `make` —— 构建 QuickJS
- `make test` —— 运行内置测试
- `make test2` —— 运行 test262 测试

## 架构概览

当前仓库尚无应用代码，因此不存在可直接读取的运行时架构。

但从项目目标来看，后续大概率会逐步形成以下模块：

- 前端：词法分析、语法分析、AST
- 中间表示或字节码层
- 执行器 / 虚拟机
- 运行时对象模型（值、对象、函数、作用域、模块）
- 内存管理（引用计数、GC 或其它策略）
- 测试体系（单元测试、语义测试、规范兼容性测试）

在代码尚未出现前，不应把这些推测当成既定事实；它们只应作为后续组织仓库和讨论实现方案的参考。

## 后续更新建议

当仓库后续加入应用代码或工具链时，应更新本文件，补充：

- 实际使用的安装、构建、Lint、测试命令
- 单个测试、单个模块或单阶段编译的运行方式
- 顶层源码布局，例如 `src/`、`include/`、`tests/`、`docs/` 的职责划分
- 当前实现阶段与里程碑，例如 tokenizer、parser、bytecode、runtime、GC 的推进状态
- 本仓库实现与 QuickJS 的关键差异，以及哪些设计是“参考”而非“照搬”
