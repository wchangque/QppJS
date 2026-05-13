# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working in this repository.

## 默认语言

- 默认使用中文与用户沟通，除非用户明确要求使用其他语言。

## 项目背景

- 当前项目目标：基于 QuickJS 的设计思想，通过 AI 辅助，从零用 C++ 实现一个 JavaScript 引擎。
- 参考仓库位于 `/home/wuzhen/code/pc/QuickJS`，设计文档已整理至 `docs/quickjs/`。
- 本仓库不是对 QuickJS 的简单封装，而是面向"理解并重建 JS 引擎关键机制"的独立实现项目。

## 计划与状态文档

- `docs/plans/00-roadmap.md`：长期路线图，阶段与任务编号的唯一来源。
- `docs/plans/01-current-status.md`：当前状态摘要（约 30 行），轻量任务读这个。
- `docs/plans/01-current-status-detail.md`：完整任务历史，`/implement` 流程读这个。
- `docs/plans/02-next-phase.md`：下一阶段执行卡。

读取规则：

- **轻量任务**（查询状态、代码问答、单文件修改等）：只读 `01-current-status.md` 摘要，按需读其他文档。
- **`/implement` 流程**：skill 内部会强制读取完整状态文档，无需在此处重复加载。
- 每次任务结束前，必须同步更新 `01-current-status.md`（摘要）和 `01-current-status-detail.md`（追加最近完成内容）。
- 如果当前阶段推进或计划调整，同步更新 `02-next-phase.md`；偏离路线图时先更新 `00-roadmap.md`。
- 只有在计划与状态文档已同步后，才能把任务标记为完成，或向用户宣称该轮工作已收尾。

## 任务收尾机制

项目采用"任务完成"而非 commit 作为状态更新触发点。收尾时至少检查：

1. 当前任务状态是否发生变化
2. `docs/plans/01-current-status.md` 是否已同步
3. `docs/plans/02-next-phase.md` 是否仍正确
4. 若阶段计划已变化，`docs/plans/00-roadmap.md` 是否已同步

## Agent Team 协作约定

实现新功能时使用 `/implement <topic>` skill，它封装了完整的 6 agent 协作流程（规范调研 → 设计 → 实现 → 测试 → 审查）。支持 `--from design`、`--from impl`、`--only spec`、`--skip review` 等参数跳过已完成的阶段。详见 `.claude/skills/implement/SKILL.md`。

## 常用命令

构建、测试、覆盖率命令详见 `build` skill（`/build`）。快速参考：

- `./scripts/coverage.sh --quiet` —— 验证 UT 功能正确性（无 ASAN/LSan 噪音）
- `./scripts/run_ut.sh --quiet` —— 检查内存泄露（含 ASAN/LSan）
- `./scripts/build_release.sh` —— Release 构建
