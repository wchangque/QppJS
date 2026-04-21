---
name: build
description: Build, test, and generate coverage reports for QppJS using scripts/build.sh. Use this skill whenever the user wants to build the project, run tests, check test results, generate a coverage report, clean the build directory, or asks about build presets. Trigger on phrases like "build", "compile", "run tests", "coverage", "clean build", "构建", "编译", "跑测试", "覆盖率", "清理构建".
---

## 能力范围

本 skill 封装了 `scripts/build.sh` 的全部功能，适用于 QppJS 项目根目录。

## 基本用法

所有命令均从项目根目录执行：

```bash
bash scripts/build.sh [preset] [options]
```

## 常用场景

### 1. 快速构建（自动检测 preset）
```bash
bash scripts/build.sh
```

### 2. 构建 + 运行测试
```bash
bash scripts/build.sh --test
```

### 3. 清理后重新构建 + 测试
```bash
bash scripts/build.sh --clean --test
```

### 4. 构建 + 测试 + 生成覆盖率报告
```bash
bash scripts/build.sh --test --coverage
```
覆盖率 preset 会自动选择（debug/release 会被替换为对应的 coverage variant）。

### 5. 生成覆盖率报告并在浏览器打开
```bash
bash scripts/build.sh --test --coverage --open
```

### 6. 指定 preset
```bash
bash scripts/build.sh macos-llvmclang-debug --test
```

## 可用 Preset

| Preset | 平台 | 编译器 | 模式 |
|--------|------|--------|------|
| macos-appleclang-debug | macOS | Apple Clang | Debug + ASan |
| macos-appleclang-release | macOS | Apple Clang | Release |
| macos-appleclang-coverage | macOS | Apple Clang | Coverage |
| macos-llvmclang-debug | macOS | LLVM Clang | Debug + ASan |
| macos-llvmclang-release | macOS | LLVM Clang | Release |
| macos-llvmclang-coverage | macOS | LLVM Clang | Coverage |
| linux-gcc-debug | Linux | GCC | Debug + ASan |
| linux-gcc-release | Linux | GCC | Release |
| linux-gcc-coverage | Linux | GCC | Coverage |
| linux-clang-debug | Linux | Clang | Debug + ASan |
| linux-clang-release | Linux | Clang | Release |
| linux-clang-coverage | Linux | Clang | Coverage |

Preset 自动检测规则：
- macOS + Homebrew LLVM → `macos-llvmclang-*`
- macOS + Apple Clang → `macos-appleclang-*`
- Linux + clang++ → `linux-clang-*`
- Linux + gcc → `linux-gcc-*`

## 覆盖率报告位置

生成后报告位于：
```
build/<preset>/coverage/index.html
```

## 注意事项

- `--coverage` 必须配合 `--test` 使用，否则报错
- `--open` 必须配合 `--coverage` 使用
- `macos-llvmclang-*` preset 需要 Homebrew LLVM，会自动从 `clang++` 路径推导 `LLVM_PREFIX`
- 覆盖率生成需要 `lcov` 和 `genhtml`（`brew install lcov`）
