---
name: implement
description: 编排 QppJS 的 agent team 完成一个具体语言特性或模块的完整实现流程（规范调研 → 设计 → 实现 → 测试 → 审查）。当用户想实现某个 JS 引擎功能、语言特性、运行时模块，或说"实现 X"、"做 X 这个功能"、"开始 X 模块"时触发。支持 --from / --only / --skip 参数跳过已完成的阶段。
---

## 执行前：必须加载状态上下文

在进入任何阶段之前，先读取以下两个文件：

1. `docs/plans/01-current-status-detail.md` — 完整任务状态与已完成内容
2. `docs/plans/02-next-phase.md` — 当前阶段目标与任务分解

如需理解全局路线，再读 `docs/plans/00-roadmap.md`。

---

## 概览

本 skill 编排 6 个专职 agent 完成一个小粒度主题的完整实现周期。主会话负责串联各阶段的输入输出，不直接写代码。

**默认流程：**
```
[并行] es-spec + quickjs-research
    → design-agent
    → perf-agent（方案审查）
    → implementation-agent（首次实现）
    → testing-agent
    → [并行] review-agent（首次审查）+ perf-agent（代码审查）
    → implementation-agent（修复必修问题 + P0 性能问题）
    → review-agent（验证审查，仅确认修复）
```

---

## 参数解析

用户调用时先解析参数，再决定从哪个阶段开始：

| 调用形式 | 起始阶段 | 说明 |
|----------|----------|------|
| `/implement <topic>` | spec + research | 完整流程 |
| `/implement --from design <topic>` | design-agent | 已有调研结果，跳过 spec/research |
| `/implement --from impl <topic>` | implementation-agent | 已有设计方案，跳过前三步 |
| `/implement --only spec <topic>` | spec + research | 只产出设计方案，不实现 |
| `/implement --skip review <topic>` | 完整流程，跳过 review | 快速迭代时使用 |
| `/implement --skip perf <topic>` | 完整流程，跳过两次 perf-agent | 快速迭代时使用 |

**解析后，向用户确认一句话：** "将从 [阶段] 开始，主题：[topic]。"

---

## 阶段 1：规范调研（可并行）

**触发条件：** 未使用 `--from design`、`--from impl`。

同时启动两个 agent（用 Agent 工具，subagent_type 分别为 `es-spec` 和 `quickjs-research`），传入相同的 topic：

**es-spec prompt 模板：**
```
主题：<topic>
请优先查阅 docs/es2021/ 下的本地文档，不清晰再查网络资源。
按你的固定输出格式输出规范摘要、语义约束、错误条件与最小测试点。
```

**quickjs-research prompt 模板：**
```
主题：<topic>
请优先查阅 docs/quickjs/ 下的本地整理文档，不清晰再去 /home/wuzhen/code/QuickJS 源码中查找。
按你的固定输出格式输出实现路径、关键结构、可借鉴点、不建议照搬点。
```

等两个 agent 都完成后，将结果整理为"调研摘要"，格式：
```
## 规范摘要
<es-spec 输出>

## QuickJS 实现参考
<quickjs-research 输出>
```

---

## 阶段 2：设计方案

**触发条件：** 未使用 `--from impl`。

启动 `design-agent`（subagent_type: `design-agent`），传入：

```
主题：<topic>

## 输入材料
<调研摘要 或 用户提供的已有调研结果>

请按你的固定输出格式，输出模块目标、边界、数据结构、关键流程、分阶段建议、风险项。
```

拿到设计方案后，向用户展示关键部分（模块边界 + 核心数据结构 + 分阶段建议），询问是否有调整意见，再继续。

---

## 阶段 2.5：方案性能审查

**触发条件：** 未使用 `--skip perf`，且未使用 `--from impl`（有设计方案产出）。

启动 `perf-agent`（subagent_type: `perf-agent`），传入：

```
主题：<topic>
审查模式：方案审查

## 设计方案摘要
<design-agent 输出的第 3 节（模块边界）、第 4 节（核心数据结构）、第 5 节（关键流程）的内容>
```

**处理规则：**
- P0：必须在进入实现前解决，将问题反馈给用户，等待确认后再继续。
- P1：在实现阶段的 prompt 中附带提醒，要求 implementation-agent 关注。
- P2：记录到主会话上下文，收尾时提示用户。

---

## 阶段 3：实现

**触发条件：** 未使用 `--only spec`。

启动 `implementation-agent`（subagent_type: `implementation-agent`），传入：

```
主题：<topic>

## 设计方案
<design-agent 输出 或 用户提供的已有设计>

请按 TDD 方式完成最小实现，先写测试，再写实现，最后说明未覆盖项。
```

---

## 阶段 4：测试补充

启动 `testing-agent`（subagent_type: `testing-agent`），传入：

```
主题：<topic>

## 规范摘要
<阶段1产出，或 "用户跳过，请根据主题自行推断规范要求">

## 设计方案
<阶段2产出>

## 实现摘要
<implementation-agent 的"代码改动摘要"与"当前已满足的设计点">

请补充缺失的边界测试、错误路径和回归风险点。
```

---

## 阶段 5：审查 + 代码性能审查（并行）

**触发条件：** review 和 perf 至少有一个未被 skip。

同时启动两个 agent（可并行，互不依赖）：

**review-agent**（触发条件：未使用 `--skip review`）：

```
主题：<topic>
审查模式：首次审查

## 设计方案
<阶段2产出>

## 实现摘要
<implementation-agent 输出>

请运行 codex:review 和 codex:adversarial-review，整合输出结构化审查报告。
```

**perf-agent**（触发条件：未使用 `--skip perf`）：

```
主题：<topic>
审查模式：代码审查

## 本次改动文件
<implementation-agent 输出的"代码改动摘要"中涉及的文件列表>

## 方案审查 P1 提醒（如有）
<阶段 2.5 中记录的 P1 问题，或"无">
```

等两个 agent 都完成后，合并处理结果：
- review-agent 的必修问题 → 进入阶段 6 修复
- perf-agent 的 P0 问题 → 追加到阶段 6 修复列表
- perf-agent 的 P1/P2 问题 → 记录到主会话上下文，收尾时汇总提示用户

**处理规则：**
- P0：将问题追加到 implementation-agent 修复轮次的输入中，要求一并修复。
- P1/P2：记录到主会话上下文，收尾时汇总提示用户。

---

## 阶段 6：修复 + 验证（各一轮，不循环）

**触发条件：** review-agent 报告存在必修问题，且未使用 `--skip review`。

**修复：** 再次启动 `implementation-agent`，传入：

```
主题：<topic>

## 设计方案
<阶段2产出>

## Review 必修问题
<review-agent 的"必修问题"列表>

请按最小改动原则逐条修复合理的必修问题，并输出"已修复 / 已跳过（含理由）"清单。
```

**验证：** 再次启动 `review-agent`，传入：

```
主题：<topic>
审查模式：验证审查

## 首次审查必修问题
<首次 review-agent 的"必修问题"列表>

## 修复清单
<implementation-agent 的修复清单>

请逐条确认必修问题是否已解决，输出"通过 / 有遗留问题"结论。不开新问题。
```

---

## 收尾

每轮 skill 执行结束后，主会话必须：

1. 向用户汇报各阶段结论（一句话/阶段）。
2. 更新 `docs/plans/01-current-status.md`。
3. 若阶段推进，更新 `docs/plans/02-next-phase.md`。

---

## 注意事项

- **粒度控制**：topic 必须足够小（如"十进制整数字面量"，而非"整个 tokenizer"）。如果 topic 过大，先拆分再执行。
- **上下文传递**：每个 agent 只接收明确的前序产出，不依赖聊天历史。传入时摘要关键结论，不要全文粘贴。
- **冲突裁决**：规范 > 当前 QppJS 设计 > QuickJS 经验 > 实现便利性。
- **--from / --only / --skip 跳过阶段时**，若缺少该阶段的输入材料，应提示用户补充，而不是凭空推断。
