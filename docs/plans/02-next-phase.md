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

### 1.1 实现 Tokenizer 基础结构 [已完成]
目标：
- 定义 token 类型
- 支持位置信息记录
- 明确输入流与输出 token 序列接口

实际结果：
- `include/qppjs/frontend/token.h`：TokenKind、Token、SourceRange、SourceLocation
- `include/qppjs/frontend/lexer.h`：LexerState、lexer_init、next_token
- `src/frontend/token.cpp`、`src/frontend/lexer.cpp` 实现完毕
- 26 个新测试（含 token_test + lexer_test）全部通过

### 1.2 支持基础词法元素 [已完成]
目标：
- 支持标识符、关键字、数字字面量、字符串字面量、标点与基础操作符
- 处理空白、换行与注释

实际结果：
- 数字字面量：十进制整数/浮点/指数、0x/0b/0o 前缀、错误检测（前导零、空前缀、非法字符、空指数、字母后缀）
- 字符串字面量：单双引号、\x/\u/\0 转义验证、行终止符中断 Invalid
- 多字符操作符（最长匹配）与全部单字符操作符
- . 的 lookahead 处理
- 新增 64 个测试，92/92 全部通过

### 1.3 设计 AST 节点体系 [已完成]
目标：
- 建立 `Program`、`Statement`、`Expression` 等最小节点骨架
- 保持结构清晰，不提前优化表示

实际结果：
- `include/qppjs/frontend/ast.h`（header-only）：5 个枚举 + 9 个表达式节点 + 6 个语句节点 + Program + ExprNode/StmtNode variant 包装 + overloaded helper
- 定义顺序遵循依赖关系，避免不完整类型问题
- 15 个 ast_test 全部通过，173/173 测试全绿

### 1.4 实现表达式解析 [已完成]
目标：
- 处理基础运算符优先级与结合性
- 选定 Pratt parser 或递归下降策略

实际结果：
- Pratt Parser，nud/led 分离，绑定力表与设计方案一致
- 支持原子（Number/String/Boolean/Null/Identifier/括号）、一元前缀（- + ! typeof void）、二元（加减乘除取模、比较、相等）、逻辑（&& ||）、赋值（= += -= *= /= %=）
- 字符串解码（\n \t \r \b \f \v \\ \' \" \xNN \uNNNN \0）在 Parser 内完成
- 数字解析（十进制/0x/0b/0o）在 Parser 内完成

### 1.5 实现语句解析 [已完成]
目标：
- 支持 expression statement、`let`、block、`if / else`、`while`

实际结果：
- 支持 ExpressionStatement、VariableDeclaration（let/const/var）、BlockStatement、IfStatement（含 else）、WhileStatement、ReturnStatement
- Early Error：const 无初始化器、顶层 return、赋值左侧非 Identifier
- 最小 ASI：; / got_lf / } / Eof 四种自动分号场景
- 47 个新测试，220/220 全部通过

### 1.6 实现 AST dump [已完成]
目标：
- 提供文本形式 AST 输出
- 复用现有 `debug` 输出入口

实际结果：
- `include/qppjs/frontend/ast_dump.h`：dump_expr / dump_stmt / dump_program 公开接口
- `src/frontend/ast_dump.cpp`：std::visit + overloaded，缩进树形格式，数字整数化显示
- `tests/unit/ast_dump_test.cpp`：17 个测试，237/237 全部通过

### 1.7 建立 parser 错误报告 [已完成]
目标：
- 提供基础语法错误
- 带位置信息
- 输出最小可读错误信息

实际结果：
- `src/frontend/parser.cpp`：新增 `make_parse_error(source, tok, msg)` 静态辅助函数
- 位置信息格式：`"line N, column M: <原有消息>"`，通过 `compute_location` 计算
- 替换全部 8 处 `Error(ErrorKind::Syntax, ...)` 调用点
- `tests/unit/parser_test.cpp`：追加 5 个 `ParserErrorTest` 测试，242/242 全部通过
- 未修改 `Error` 结构体或 `parser.h` 公开接口

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
