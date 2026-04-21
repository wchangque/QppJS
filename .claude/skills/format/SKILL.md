---
name: format
description: Format C/C++ source files in the QppJS project using clang-format. Use this skill whenever the user wants to format code, run clang-format, fix code style, clean up formatting, or says things like "格式化代码", "format", "clang-format", "整理代码风格", "代码格式化". Also trigger after writing new C++ files or making significant edits, to keep the codebase consistent.
---

## 能力范围

使用项目根目录的 `.clang-format` 配置，对 `src/`、`include/`、`tests/` 下的所有 `.cpp` 和 `.h` 文件执行 `clang-format -i`（原地格式化）。

## 执行步骤

### 1. 确定格式化范围

- **全量格式化**（用户说"格式化所有代码"、"format all"等）：扫描 `src/`、`include/`、`tests/` 下所有 `.cpp` 和 `.h` 文件。
- **局部格式化**（用户指定了文件或目录）：只处理指定范围。
- **改动文件**（默认行为，用户说"格式化改动文件"、"format changed"，或刚写完代码后）：用 git 精确获取改动文件列表。

### 2. 收集文件列表

**全量格式化**：用 Glob 工具分别扫描三个目录：

```
src/**/*.{cpp,h}
include/**/*.{cpp,h}
tests/**/*.{cpp,h}
```

**改动文件**：通过 git 获取已改动和新增的文件：

```bash
{ git diff --name-only HEAD; git ls-files --others --exclude-standard; } \
  | grep -E '\.(cpp|h)$'
```

若结果为空（没有改动），告知用户无需格式化。

### 3. 执行格式化

将所有文件拼成一条命令，一次执行：

```bash
clang-format -i <file1> <file2> ... && echo "done"
```

### 4. 验证（可选但推荐）

格式化后，如果项目有测试，运行构建验证格式化没有破坏代码：

```bash
bash scripts/build.sh --test 2>&1 | tail -5
```

## 注意事项

- 格式化依赖项目根目录的 `.clang-format` 文件，若不存在则提示用户先生成（参考 `docs/google-cpp-style-guide.md`）。
- `clang-format` 是纯文本变换，不会改变代码语义，但格式化后建议跑一遍测试确认无误。
- 如果用户只是想检查哪些文件需要格式化（不修改），可用 `--dry-run --Werror` 模式：
  ```bash
  clang-format --dry-run --Werror <files>
  ```
  有输出的文件即为需要格式化的文件。
