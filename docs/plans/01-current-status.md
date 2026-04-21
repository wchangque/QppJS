# QppJS 当前开发状态

本文件记录当前真实开发状态，是新 session 接续工作的首要入口。

## 1. 当前阶段

- 当前阶段：Phase 1：Lexer + Parser + AST（Phase 0 已完成）
- 最近更新时间：2026-04-21

## 2. 当前任务状态

### 已完成
- [x] 建立项目目标、参考仓库与 agent team 协作约定
- [x] 创建 6 个项目级 subagents：`es-spec`、`quickjs-research`、`design-agent`、`implementation-agent`、`testing-agent`、`review-agent`
- [x] 形成长期路线图、当前状态、下一阶段计划三文档体系
- [x] 明确当前状态更新机制以“任务完成”而不是“commit”作为主触发点
- [x] 0.1 建立最小目录结构
- [x] 0.2 建立最小构建链路（顶层 CMake 骨架）
- [x] 0.3 建立最小 CLI
- [x] 0.4 设计错误处理基础结构
- [x] 0.5 设计第一版 Value
- [x] 0.6 建立测试基线
- [x] 0.7 建立调试输出入口
- [x] 0.8 建立覆盖率报告链路（lcov + genhtml，HTML 行/分支覆盖率）

### 进行中
- [ ] Phase 1：Lexer + Parser + AST

### 未开始
- [ ] Phase 2：AST Interpreter

### 阻塞
- 暂无 Phase 0 阻塞项

## 3. 最近完成内容

- 已确认 Phase 0 退出条件全部满足，可进入 Phase 1
- 已建立基于 CMake 的最小源码与测试工程结构：`src/`、`include/`、`tests/`、`cmake/`
- 已接入最小 CLI 程序 `qppjs`，支持 `qppjs "1+2"` 风格的源码字符串输入
- 已建立第一版统一错误类型 `Error` 与基础值类型 `Value`
- 已建立 `debug` 输出入口，统一格式化 `Error` 与 `Value`
- 已接入 GoogleTest：优先使用系统 GTest，找不到时通过 `cmake/Dependencies.cmake` 自动获取
- 已通过 CMake 配置、编译与 CTest 验证，当前测试结果为 6/6 通过
- 已接入 CMake Presets（方案 B）：支持 Linux GCC/Clang、macOS Apple Clang、Windows MSVC 共 8 个 preset
- 已新增 `cmake/CompilerFlags.cmake`：统一警告、ASan、UBSan 开关，所有 target 通过 `qppjs_apply_compiler_flags` 接入
- 已新增 `include/qppjs/platform/compiler.h` 与 `arch.h`：跨编译器宏与架构检测
- `CMakeUserPresets.json` 已加入 `.gitignore`
- 已建立覆盖率报告链路：新增 3 个 coverage preset（macos/linux-gcc/linux-clang）、`scripts/coverage.sh` 负责收集与生成 HTML，`build.sh --test --coverage` 一键触发；macOS 用 Homebrew LLVM llvm-cov 作为 gcov wrapper 解决版本错配问题

## 4. 当前风险与待决策项

- Phase 1 需要尽快确定 lexer 的首批语法子集与最小 AST 节点集合
- 当前 `Value` 仍是学习阶段的简单表示，后续进入 interpreter/object model 时需继续保持“简单优先”
- CLI 当前仅支持源码字符串输入；是否在 Phase 1 提前加入文件输入，需要结合 lexer 调试路径再决定

## 5. 下次进入点

新 session 开始时，优先做以下动作：
1. 读取本文件
2. 读取 `docs/plans/02-next-phase.md`
3. 直接进入 Phase 1，先收敛 lexer 的最小支持范围与 AST 基础节点

## 6. 收尾检查清单

每次任务结束前，至少检查：
- 当前任务状态是否变化
- 本文件是否已同步
- `docs/plans/02-next-phase.md` 是否仍正确
- 若阶段计划已调整，`docs/plans/00-roadmap.md` 是否已同步
