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
| `scripts/run_ut.sh` | 先调用 `build_debug.sh`，再仅执行 UT（排除 CLI 测试） |
| `scripts/coverage.sh` | 先调用 `build_test.sh`，再执行 UT 并生成覆盖率报告 |
| `scripts/build_release.sh` | Release 构建（不含测试），输出到 `build/release/`，含编译器选择逻辑 |

## 常用场景

### 构建 + 运行所有 UT（最常用）+ 检查内存泄露
```bash
./scripts/run_ut.sh
```

### 仅构建 Debug（不跑测试）
```bash
./scripts/build_debug.sh
```

### 生成覆盖率报告
```bash
./scripts/coverage.sh          # 生成报告
./scripts/coverage.sh --open   # 生成后在浏览器打开
```

### 清理重建
```bash
rm -rf build/debug && ./scripts/run_ut.sh
rm -rf build/test  && ./scripts/coverage.sh
```

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

- **UT 回归和内存泄露检查都使用 `run_ut.sh`**：它调用 `build_debug.sh`（含 ASAN/LSan），一条命令同时验证功能和内存安全
- `build_test.sh` 的 LLVM 自动检测当前已注释掉（coverage 构建不走 ASAN 路径），如需指定编译器请手动设置 `$CC`/`$CXX`
- 覆盖率生成需要 `lcov` 和 `genhtml`（`brew install lcov`）
