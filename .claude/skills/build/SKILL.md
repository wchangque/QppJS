---
name: build
description: QppJS 项目的构建、测试与覆盖率工具指南，封装 scripts/ 下五个固定入口脚本的用法。每当用户在 QppJS 项目中涉及以下任何场景时，都应主动使用本 skill：构建项目（debug/release/coverage）、运行单元测试、检查测试失败原因、验证代码改动没有回归、检查内存泄露（ASAN/LSan）、生成或查看覆盖率报告、清理重建、询问脚本职责或编译器选择逻辑。即使用户没有明确说"build"或"构建"，只要是在问"怎么跑测试"、"改了代码怎么验证"、"内存泄露用哪个命令"、"覆盖率报告在哪"等与项目构建测试流程相关的问题，都应触发本 skill。
---

## 脚本入口

所有脚本从项目根目录执行，职责固定：

| 脚本 | 用途 |
|------|------|
| `scripts/build_debug.sh` | Debug 构建（开 ASAN），输出到 `build/debug/` |
| `scripts/build_test.sh` | Coverage 构建，输出到 `build/test/` |
| `scripts/run_ut.sh` | 先调用 `build_debug.sh`，再执行 UT；**用于内存泄露检查**（含 ASAN/LSan） |
| `scripts/coverage.sh` | 先调用 `build_test.sh`，再执行 UT 并生成覆盖率报告；**用于 UT 功能验证** |
| `scripts/build_release.sh` | Release 构建（不含测试），输出到 `build/release/`，含编译器选择逻辑 |

## 常用场景

### 验证 UT 功能正确性（最常用）
```bash
./scripts/coverage.sh --quiet  # 静默模式，日志分流到文件（见下）
./scripts/coverage.sh          # 同上，输出到终端
```
coverage 构建不开 ASAN/LSan，失败即功能缺陷，结果干净无噪音。

### 检查内存泄露
```bash
./scripts/run_ut.sh --quiet    # 静默模式，日志分流到文件（见下）
./scripts/run_ut.sh            # 同上，输出到终端
```
run_ut 开 ASAN/LSan，失败可能是功能缺陷也可能是泄露，需结合日志区分。

### 仅构建 Debug（不跑测试）
```bash
./scripts/build_debug.sh
```

### 生成覆盖率报告并打开
```bash
./scripts/coverage.sh --open
```

### 清理重建
```bash
./scripts/coverage.sh --clean
./scripts/run_ut.sh --clean
```

## --quiet 模式日志路径

`--quiet` 时所有输出写入文件而非终端，按成功/失败分流：

### run_ut.sh --quiet

| 文件 | 内容 |
|------|------|
| `build/debug/run_ut_build_success.log` | 构建成功的完整输出 |
| `build/debug/run_ut_build_failure.log` | 构建失败的完整输出 |
| `build/debug/run_ut_success.log` | 所有 UT 通过时的 ctest 完整输出 |
| `build/debug/run_ut_failure.log` | 有 UT 失败时的摘要（仅失败用例 + LSan 泄露块） |

### coverage.sh --quiet

| 文件 | 内容 |
|------|------|
| `build/test/coverage_success.log` | 全流程成功时的完整输出 |
| `build/test/coverage_failure.log` | 有失败时的摘要（仅失败用例 + LSan 泄露块） |

## 编译器选择逻辑（build_debug.sh / build_release.sh）

1. 环境变量 `$CC` / `$CXX` 已设置 → 直接使用
2. 未设置 + Homebrew LLVM 存在（`brew --prefix llvm`）→ 自动切换，并附加对应 libc++ 链接路径
3. 未设置 + 无 Homebrew LLVM → 由 CMake 自动选系统 Apple Clang

`build_test.sh` 的编译器自动检测当前已注释掉，如需指定编译器请手动设置 `$CC`/`$CXX`。

## 覆盖率报告位置

```
build/test/coverage/index.html
```

## 注意事项

- **验证 UT 功能正确性用 `coverage.sh`**：不开 ASAN/LSan，失败即功能缺陷，结果干净
- **检查内存泄露用 `run_ut.sh`**：开 ASAN/LSan，失败可能是功能缺陷或泄露，需结合日志区分
- `build_test.sh` 的 LLVM 自动检测当前已注释掉（coverage 构建不走 ASAN 路径），如需指定编译器请手动设置 `$CC`/`$CXX`
- 覆盖率生成需要 `lcov` 和 `genhtml`（`brew install lcov`）
- `--quiet` 失败时终端只打印日志路径，详情看对应 `.log` 文件；成功时只有 success log，failure log 不存在
