# QppJS 当前开发状态

本文件记录当前真实开发状态，是新 session 接续工作的首要入口。

## 1. 当前阶段

- 当前阶段：Phase 1 已全部完成，下一阶段为 Phase 2：AST Interpreter
- 最近更新时间：2026-04-21（1.7 parser 错误报告完成）

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

- [x] Phase 1：Lexer + Parser + AST（已全部完成）
  - [x] 1.1 实现 Tokenizer 基础结构（TokenKind、Token、SourceRange、SourceLocation、LexerState、next_token 最小实现）
  - [x] 1.2 支持基础词法元素（数字字面量、字符串字面量、多字符操作符）
  - [x] 1.3 设计 AST 节点体系（header-only，9 个表达式节点 + 6 个语句节点 + Program，variant 包装，overloaded helper）
  - [x] 1.4 实现表达式解析（Pratt Parser：原子/一元/二元/逻辑/赋值，含优先级与结合性）
  - [x] 1.5 实现语句解析（ExpressionStatement、VariableDeclaration、BlockStatement、IfStatement、WhileStatement、ReturnStatement + 最小 ASI）
  - [x] 1.6 实现 AST dump（dump_expr / dump_stmt / dump_program，缩进树形格式，17 个测试全部通过）
  - [x] 1.7 建立 parser 错误报告（make_parse_error 辅助函数，位置信息拼入 message，格式 "line N, column M: <描述>"，5 个新测试，242/242 全部通过）

### 进行中
- 暂无（Phase 1 已全部完成）

### 未开始
- [ ] Phase 2：AST Interpreter

### 阻塞
- 暂无 Phase 0 阻塞项

## 3. 最近完成内容

- 已完成 1.7 parser 错误报告：
  - `src/frontend/parser.cpp`：新增 `make_parse_error(source, tok, msg)` 静态辅助函数，调用 `compute_location` 将行列信息拼入 message，格式 `"line N, column M: <原有消息>"`
  - 替换全部 8 处 `Error(ErrorKind::Syntax, ...)` 调用点（expect、consume_semicolon、nud、led ×2、parse_var_decl ×2、parse_return_stmt）
  - `tests/unit/parser_test.cpp`：追加 5 个 `ParserErrorTest` 测试（ConstNoInitHasLocation、InvalidAssignTargetHasLocation、TopLevelReturnHasLocation、MultilineErrorShowsCorrectLine、ErrorMessageContainsDescription）
  - 242/242 测试全部通过（含原有 237 个）

- 已完成 1.6 AST dump：
  - `include/qppjs/frontend/ast_dump.h`：公开接口 `dump_expr` / `dump_stmt` / `dump_program`
  - `src/frontend/ast_dump.cpp`：std::visit + overloaded 遍历，缩进树形格式，操作符/关键字转字符串，数字整数化显示
  - `tests/unit/ast_dump_test.cpp`：17 个测试（含所有节点类型、嵌套缩进验证、Program dump）
  - 237/237 测试全部通过（含原有 220 个）

- 已完成 1.4 + 1.5 Parser（Pratt Parser + 语句解析 + 最小 ASI）：
  - `include/qppjs/base/result.h`：ParseResult<T>（std::variant<T, Error>，含 Ok/Err 工厂方法）
  - `include/qppjs/frontend/parser.h`：公开接口 `parse_program(std::string_view) -> ParseResult<Program>`
  - `src/frontend/parser.cpp`：Parser struct（LexerState 驱动，Pratt 主循环 nud/led，语句分发，最小 ASI，Early Error）
  - `tests/unit/parser_test.cpp`：47 个新测试（阶段 A-G），覆盖原子/一元/二元/逻辑/赋值表达式、优先级/结合性、6 种语句、3 个 Early Error、ASI
  - 220/220 测试全部通过（含原有 173 个）

- 已完成 1.3 AST 节点体系：
  - `include/qppjs/frontend/ast.h`（header-only）：5 个枚举（UnaryOp、BinaryOp、LogicalOp、AssignOp、VarKind）、9 个表达式节点 struct、6 个语句节点 struct、Program 根节点、ExprNode/StmtNode variant 包装 struct（模板构造函数）、overloaded helper
  - 定义顺序遵循依赖关系：表达式 struct -> ExprNode -> 语句 struct -> StmtNode -> Program
  - `tests/unit/ast_test.cpp`：15 个编译+运行时验证点，覆盖所有节点类型构造、std::visit、VarKind 区分、undefined 是 Identifier
  - 173/173 测试全部通过

- 已完成 1.2 基础词法元素：
  - `src/frontend/lexer.cpp`：扩展 `next_token`，新增数字字面量（十进制整数/浮点/指数、十六进制/二进制/八进制前缀、`0x`/`0b`/`0o` 错误处理、前导零 Invalid）、字符串字面量（单双引号、`\x`/`\u`/`\0` 转义验证、行终止符中断 Invalid）、多字符操作符（`==` `===` `!=` `!==` `<=` `>=` `++` `--` `&&` `||` `+=` `-=` `*=` `/=` `%=` `=>`）及所有单字符操作符（`=` `!` `<` `>` `+` `-` `*` `/` `%` `&` `|` `^` `~`）
  - `.` 处理改为 lookahead：下一字符为数字则进入数字扫描，否则返回 Dot
  - `tests/unit/lexer_test.cpp`：追加 64 个新测试，覆盖所有合法/非法场景及最长匹配验证
  - 共 92 个测试全部通过（含原有 28 个）

- 已完成 1.1 Tokenizer 基础结构：
  - `include/qppjs/frontend/token.h`：TokenKind 枚举（关键字区间连续）、Token、SourceRange、SourceLocation、is_keyword、token_kind_name、compute_location 声明
  - `include/qppjs/frontend/lexer.h`：LexerState、lexer_init、next_token 声明
  - `src/frontend/token.cpp`：token_kind_name（switch 全覆盖）、is_keyword（区间判断）、compute_location（\r\n 计为单换行）
  - `src/frontend/lexer.cpp`：next_token 最小实现（空白跳过、单行/块注释、Eof、单字符标点、标识符+关键字查表、Invalid）
  - `tests/unit/token_test.cpp` 与 `tests/unit/lexer_test.cpp`：30 个测试全部通过（含原有 6 个）
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
3. Phase 1 所有任务已完成，进入 Phase 2：AST Interpreter

## 6. 收尾检查清单

每次任务结束前，至少检查：
- 当前任务状态是否变化
- 本文件是否已同步
- `docs/plans/02-next-phase.md` 是否仍正确
- 若阶段计划已调整，`docs/plans/00-roadmap.md` 是否已同步
