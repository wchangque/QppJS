# QppJS 下一阶段计划

本文件展开“下一阶段”的可执行内容，是从当前状态进入下一轮开发的直接行动卡。

## 1. 下一阶段

- 下一阶段：Phase 1：Lexer + Parser + AST
- 对应路线图：`docs/plans/00-roadmap.md`
- 当前事实源：`docs/plans/01-current-status.md`

## 2. 阶段目标

在已具备构建、测试、CLI、错误模型、Value 与调试输出基础设施的前提下，进入前端实现阶段，先完成最小 lexer、parser 与 AST 骨架，为后续 AST Interpreter 打基础。

## 3. 进入前提

当前已具备：
- 明确的项目目标
- 参考仓库与研究方向
- agent team 协作方式
- 路线图与状态文档体系
- Phase 0 的目录骨架
- 顶层 CMake 骨架
- GoogleTest 测试基线
- 最小 CLI 程序入口
- 第一版 `Error` 与 `Value`
- `debug` 输出入口

因此可以从工程搭建阶段进入前端实现阶段。

## 4. 本阶段任务分解

### 1.1 实现 Tokenizer 基础结构
目标：
- 定义 token 类型
- 支持位置信息记录
- 明确输入流与输出 token 序列接口

建议结果：
- `src/frontend/lexer/` 中有 tokenizer 主体结构
- `include/qppjs/frontend/lexer/` 中有最小公开声明
- token 带有基础 source location

### 1.2 支持基础词法元素
目标：
- 支持标识符、关键字、数字字面量、字符串字面量、标点与基础操作符
- 处理空白、换行与注释

建议：
- 先覆盖最小 JS 子集
- 每增加一类 token 都配套单测

### 1.3 设计 AST 节点体系
目标：
- 建立 `Program`、`Statement`、`Expression` 等最小节点骨架
- 保持结构清晰，不提前优化表示

建议：
- 先聚焦 literal / identifier / unary / binary / variable declaration / block
- 节点设计应便于后续 dump 与解释执行

### 1.4 实现表达式解析
目标：
- 处理基础运算符优先级与结合性
- 选定 Pratt parser 或递归下降策略

建议：
- 先支持 literal、identifier、括号、基础 unary/binary expression
- 优先让简单表达式稳定可测

### 1.5 实现语句解析
目标：
- 支持 expression statement、`let`、block、`if / else`、`while`

建议：
- 先从 expression statement 与 `let` 开始
- 逐步增加 block 与控制流

### 1.6 实现 AST dump
目标：
- 提供文本形式 AST 输出
- 复用现有 `debug` 输出入口

建议：
- 输出格式优先清晰稳定
- 方便测试断言与人工检查

### 1.7 建立 parser 错误报告
目标：
- 提供基础语法错误
- 带位置信息
- 输出最小可读错误信息

建议：
- 尽量复用现有 `Error` 结构
- 与 CLI/debug 输出路径保持一致

## 5. 建议执行顺序

建议严格按以下顺序推进：
1. 1.1 实现 Tokenizer 基础结构
2. 1.2 支持基础词法元素
3. 1.3 设计 AST 节点体系
4. 1.4 实现表达式解析
5. 1.5 实现语句解析
6. 1.6 实现 AST dump
7. 1.7 建立 parser 错误报告

原因：
- 先打通 token 流，再建立 AST 与 parser
- 先有正确结构，再补调试输出与错误细节
- 每一步都可以继续复用当前 CMake、GoogleTest、CLI 与 debug 基础设施

## 6. 验证方式

本阶段开始前，已可使用以下真实验证路径：
- `cmake -S . -B build`
- `cmake --build build`
- `ctest --test-dir build --output-on-failure`

本阶段完成后，应至少能验证：
- 能把最小 JS 子集稳定切分为 token
- 能把基础表达式与语句解析为 AST
- 可输出 AST dump
- 语法错误可报告基础位置信息

## 7. 暂不处理内容

本阶段不进入：
- AST Interpreter
- 对象模型
- 函数调用语义
- VM
- GC
- test262 接入

## 8. 退出条件

满足以下条件即可进入 Phase 2：
- 最小 lexer 可用
- 基础 parser 可用
- AST 节点骨架稳定
- AST dump 可用
- 基础语法错误可报告
