# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 默认语言

- 默认使用中文与用户沟通，除非用户明确要求使用其他语言。

## 仓库现状

- 当前仓库仅包含 `README.md`，内容只有项目标题 `QppJS`。
- 当前不存在旧的 `CLAUDE.md`、包管理清单、源码目录、测试配置，或其他已提交的 CI / 工具配置文件。

## 常用命令

由于仓库中尚未加入运行时、构建或测试工具链，目前没有可验证的构建、Lint 或测试命令。

当前可用的仓库检查命令：

- `git status` —— 查看工作区状态
- `git ls-tree -r --name-only HEAD` —— 列出当前提交中的已跟踪文件
- `git log --stat --oneline --decorate -n 5` —— 查看最近提交及其改动概览

## 架构概览

当前仓库中还没有应用代码，因此无法推断运行时架构、模块边界或开发流程。

现阶段更准确的理解是：这是一个仅完成初始提交的空项目骨架，包含：

- 一个 `README.md`
- `main` 分支上的一次初始提交
- 尚未形成可识别的源码布局或构建系统

## 后续更新建议

当仓库后续加入应用代码或工具链时，应更新本文件，补充：

- 代码库中实际使用的安装、构建、Lint、测试命令
- 测试框架出现后，如何运行单个测试
- 顶层架构信息，例如入口点、主要包或应用、关键数据流与集成点
- 若后续新增 Cursor 或 Copilot 规则文件，也应同步整理其中的重要约束
