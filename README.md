# QppJS

QppJS 是一个从零开始实现的 JavaScript 引擎项目。

当前目标是基于 QuickJS 的设计思想，以 C++ 重新实现一个可逐步演进的 JS 引擎，并在开发过程中结合 AI 辅助完成调研、设计、编码与文档整理。

## 项目定位

- 以 **理解与复现 JavaScript 引擎核心机制** 为主线，而不是简单封装现有引擎
- 参考 QuickJS 的整体设计取舍，例如小型化、可嵌入、实现聚焦、运行时与执行模型的清晰边界
- 使用 C++ 作为实现语言，逐步建立词法分析、语法分析、AST、字节码/执行模型、运行时对象系统、GC 等核心模块
- 借助 AI 提高从 0 到 1 的推进效率，但项目实现与架构决策保持可审阅、可演进、可验证

## 参考项目

当前主要参考仓库：`/home/wuzhen/code/QuickJS`

从现有参考仓库可确认，QuickJS 包含以下与引擎实现直接相关的内容：

- 核心运行时与解释器源码，例如 `quickjs.c`、`qjs.c`、`qjsc.c`
- 支撑组件，例如正则、Unicode、数值处理、工具函数等源码
- `Makefile`、`tests/`、`examples/`、test262 运行相关文件
- 关于运行时、字节码、模块、C API 与测试方式的较完整文档

## 当前仓库状态

当前仓库已包含实际源码、CMake 构建系统、单元测试与覆盖率脚本。

## 构建与测试

默认情况下，编译器由原生 CMake 自行探测。

### `scripts/build_release.sh`

构建 release 版本，关闭 UT 编译。

```bash
./scripts/build_release.sh
```

### `scripts/build_debug.sh`

构建 debug 版本，开启 UT 编译，默认打开 ASAN，不开启覆盖率统计。

```bash
./scripts/build_debug.sh
ctest --test-dir build/debug --output-on-failure
```

### `scripts/build_test.sh`

构建 test 版本，开启 UT 编译与覆盖率统计，默认不启用 ASAN。

```bash
./scripts/build_test.sh
```

### `scripts/coverage.sh`

先调用 `build_test.sh`，再执行 UT 并生成覆盖率报告。

```bash
./scripts/coverage.sh
./scripts/coverage.sh --open
```
