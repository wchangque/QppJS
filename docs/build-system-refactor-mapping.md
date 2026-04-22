# QppJS 构建系统重构映射表

本文档给出当前构建系统到目标架构的**实际重构映射表**，用于指导后续重构 `CMakeLists.txt`、`cmake/*.cmake`、`CMakePresets.json` 与 `scripts/coverage.sh`。

当前设计原则：

- 平台 / 编译器 / 生成器差异统一由 **CMake** 处理
- 覆盖率**编译期开关**由 **CMake** 处理
- 覆盖率**测试后报告收集**由 **脚本**处理
- `CMakePresets.json` 与 `scripts/build.sh` 降级为**便捷入口**，不再承载核心构建逻辑

---

## 1. 当前问题摘要

| 当前位置 | 当前职责 | 主要问题 |
|---|---|---|
| `CMakeLists.txt` | 项目入口、测试开关 | 顶层职责偏薄，平台/编译器/backend 识别未沉淀为统一模型 |
| `src/CMakeLists.txt` / `tests/CMakeLists.txt` | 目标定义 + 每 target 调用 `qppjs_apply_compiler_flags()` | target 侧配置入口过于粗粒度，后续扩展 sanitizer / coverage / warnings 时容易继续膨胀 |
| `CMakePresets.json` | 平台/编译器/构建类型组合入口 | 组合几乎平铺展开，重复多，维护成本高 |
| `scripts/build.sh` | 参数解析、平台探测、preset 自动选择、coverage 入口 | 脚本知道太多 CMake 语义，职责过重 |
| `scripts/coverage.sh` | coverage 收集与 HTML 输出 | 通过 preset 名字推断工具链，边界不清晰 |

---

## 2. 目标架构

### 2.1 目标分层

```text
CMakeLists.txt
  ├── cmake/ProjectOptions.cmake
  ├── cmake/DetectToolchain.cmake
  ├── cmake/CompilerWarnings.cmake
  ├── cmake/Sanitizers.cmake
  ├── cmake/Coverage.cmake
  ├── cmake/ProjectTargets.cmake
  └── cmake/Dependencies.cmake

src/CMakeLists.txt
tests/CMakeLists.txt
scripts/coverage.sh
CMakePresets.json   # 可选便捷入口
scripts/build.sh    # 可选薄封装
```

### 2.2 分工边界

| 层级 | 负责内容 | 不负责内容 |
|---|---|---|
| 顶层 `CMakeLists.txt` | 项目入口、全局 option、模块加载、子目录组织、metadata 输出 | 平铺组合逻辑、coverage 报告采集 |
| `cmake/*.cmake` | 平台/编译器检测、warning/sanitizer/coverage instrumentation 配置、公共 target 搭建 | 构建后 HTML 报告生成 |
| `src/tests` 子目录 | 纯目标声明与少量目标级依赖 | 平台/编译器分支判断 |
| `scripts/coverage.sh` | 读取 CMake 生成的 metadata，按 backend 收集并格式化覆盖率报告 | 猜测编译器 / 猜测 preset 语义 |
| `CMakePresets.json` | 常用入口 | 构建规则本体 |
| `scripts/build.sh` | `configure/build/test` 薄包装 | 平台和 backend 决策 |

---

## 3. 文件级重构映射

## 3.1 顶层 `CMakeLists.txt`

### 当前

```cmake
cmake_minimum_required(VERSION 3.20)
project(QppJS VERSION 0.1.0 LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

include(CompilerFlags)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(QPPJS_BUILD_TESTS "Build QppJS tests" ON)

add_subdirectory(src)

if(QPPJS_BUILD_TESTS)
    enable_testing()
    include(Dependencies)
    qppjs_setup_googletest()
    add_subdirectory(tests)
endif()
```

### 重构后

顶层改为“编排层”：

```cmake
cmake_minimum_required(VERSION 3.20)
project(QppJS VERSION 0.1.0 LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

include(ProjectOptions)
include(DetectToolchain)
include(CompilerWarnings)
include(Sanitizers)
include(Coverage)
include(ProjectTargets)

qppjs_setup_project_options()
qppjs_detect_toolchain()
qppjs_setup_project_targets()

add_subdirectory(src)

if(QPPJS_BUILD_TESTS)
    enable_testing()
    include(Dependencies)
    qppjs_setup_googletest()
    add_subdirectory(tests)
endif()

qppjs_write_build_metadata()
```

### 映射说明

| 当前内容 | 新归属 |
|---|---|
| C++20 标准设置 | `ProjectOptions.cmake` |
| `QPPJS_BUILD_TESTS` | `ProjectOptions.cmake` |
| `CompilerFlags.cmake` 中混合的编译器逻辑 | 拆分到 `DetectToolchain.cmake` / `CompilerWarnings.cmake` / `Sanitizers.cmake` / `Coverage.cmake` |
| 顶层只 include `CompilerFlags` | 顶层按职责 include 多个模块 |

---

## 3.2 `cmake/CompilerFlags.cmake` → 拆分重构

### 当前

单文件承担：

- warning flags
- sanitizer flags
- coverage flags
- `qppjs_apply_compiler_flags(target)` 目标级应用

### 重构后

拆成 5 个文件：

| 新文件 | 职责 |
|---|---|
| `ProjectOptions.cmake` | option 定义、C++ 标准等全局基础设置 |
| `DetectToolchain.cmake` | 平台、编译器、generator、coverage backend 检测 |
| `CompilerWarnings.cmake` | warning / warnings-as-errors |
| `Sanitizers.cmake` | ASAN / UBSAN |
| `Coverage.cmake` | coverage instrumentation 编译/链接选项 |

### 设计动机

- 把“检测”和“应用”分开
- 把“真正全局项”和“项目 target 公共项”分开
- 避免以后在 `qppjs_apply_compiler_flags(target)` 中继续堆条件分支

---

## 3.3 `src/CMakeLists.txt`

### 当前

```cmake
add_library(qppjs_core ...)
target_include_directories(qppjs_core PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_compile_features(qppjs_core PUBLIC cxx_std_20)
qppjs_apply_compiler_flags(qppjs_core)

add_executable(qppjs main/main.cpp)
target_link_libraries(qppjs PRIVATE qppjs_core)
qppjs_apply_compiler_flags(qppjs)
```

### 重构后

```cmake
add_library(qppjs_core ...)
target_include_directories(qppjs_core PUBLIC "${PROJECT_SOURCE_DIR}/include")
qppjs_configure_project_target(qppjs_core)

add_executable(qppjs main/main.cpp)
target_link_libraries(qppjs PRIVATE qppjs_core)
qppjs_configure_project_target(qppjs)
```

### 映射说明

| 当前行为 | 新行为 |
|---|---|
| `target_compile_features(... cxx_std_20)` | 由 `qppjs_project_options` 统一承载 |
| `qppjs_apply_compiler_flags(target)` | 改为单次 `qppjs_configure_project_target(target)` |
| warnings / sanitizer / coverage 逐 target 应用 | 通过公共 INTERFACE target 继承 |

---

## 3.4 `tests/CMakeLists.txt`

### 当前

- 定义 `qppjs_unit_tests`
- 链接 `qppjs_core + GTest::gtest_main`
- 调 `qppjs_apply_compiler_flags(qppjs_unit_tests)`

### 重构后

- 保持测试目标定义不变
- 改为调用 `qppjs_configure_project_target(qppjs_unit_tests)`

### 映射说明

| 当前行为 | 新行为 |
|---|---|
| 测试目标单独套编译器 flag | 统一继承项目公共 options / warnings |
| coverage instrumentation 在脚本阶段隐式依赖 preset 组合 | coverage instrumentation 在 CMake 中显式打开 |

---

## 3.5 `CMakePresets.json`

### 当前

- 平铺展开：
  - `linux-gcc-debug`
  - `linux-gcc-release`
  - `linux-gcc-coverage`
  - `linux-clang-debug`
  - `linux-clang-release`
  - ...
- 同时维护 configure/build/test 三套几乎同名 preset

### 重构后

Presets 降级为便捷入口，有两种推荐保留方式。

#### 方式 A：最小常用入口

只保留：

- `linux-debug`
- `linux-release`
- `linux-coverage`
- `macos-debug`
- `macos-release`
- `macos-coverage`
- `windows-debug`
- `windows-release`

编译器由用户显式通过环境或 `-D CMAKE_CXX_COMPILER=...` 指定。

#### 方式 B：少量推荐组合

保留少数高频组合：

- `linux-clang-debug`
- `linux-gcc-coverage`
- `macos-appleclang-debug`
- `windows-msvc-debug`

但不再追求全矩阵铺开。

### 映射说明

| 当前内容 | 新归属 |
|---|---|
| 平台/编译器/backend 真正逻辑 | 迁回 CMake 模块 |
| Preset 命名驱动脚本分支 | 删除 |
| build/test preset 大量重复 | 只保留少量入口或按需生成 |

---

## 3.6 `scripts/build.sh`

### 当前

负责：

- 自动探测 preset
- 处理 `--test --coverage --open --clean`
- 将 debug/release preset 重写为 coverage preset
- 推导 `LLVM_PREFIX`
- 调用 `coverage.sh`

### 重构后

降级为薄封装，只负责：

- `cmake -S . -B <build-dir>`
- `cmake --build <build-dir>`
- 可选 `ctest --test-dir <build-dir>`

不再负责：

- 猜测编译器
- 决定 coverage backend
- 改写 preset 名称
- 解释平台策略

### 推荐 CLI 形态

```bash
./scripts/build.sh build/linux-debug --test
./scripts/build.sh build/clang-coverage --test
```

或直接鼓励用户调用原生 CMake：

```bash
cmake -S . -B build/linux-debug -G Ninja -D CMAKE_CXX_COMPILER=clang++ -D QPPJS_ENABLE_COVERAGE=ON
cmake --build build/linux-debug
ctest --test-dir build/linux-debug --output-on-failure
```

---

## 3.7 `scripts/coverage.sh`

### 当前

- 从 preset 名称判断 `gcc` / `llvmclang` / `appleclang`
- Linux/macOS 分支中拼接 `gcov` / `llvm-cov` wrapper
- 依赖 `build/${PRESET}` 目录结构

### 重构后

脚本改成只读 **build metadata**，不再读 preset 语义。

#### 输入

- build 目录，例如 `build/linux-clang-coverage`
- metadata 文件，例如 `build/linux-clang-coverage/qppjs-build-meta.json`

#### metadata 示例

```json
{
  "platform": "linux",
  "compiler_family": "clang",
  "generator": "Ninja",
  "is_multi_config": false,
  "coverage_enabled": true,
  "coverage_backend": "llvm-cov",
  "test_binary": "tests/qppjs_unit_tests"
}
```

#### 脚本职责

| backend | 报告链路 |
|---|---|
| `gcov` | `lcov --capture` → `lcov --remove` → `genhtml` |
| `llvm-cov` | `llvm-profdata merge` → `llvm-cov export/show/report`，必要时转换为 lcov 再接 `genhtml` |
| `unsupported` | 直接报错并退出 |

### 映射说明

| 当前行为 | 新行为 |
|---|---|
| 依赖 preset 命名约定 | 依赖 CMake 输出 metadata |
| 脚本二次猜测工具链 | CMake 在 configure 阶段一次性判定 |
| Linux/macOS 混合分支推导 | 只按 `coverage_backend` 分支 |

---

## 4. 新增 CMake 模块 API 草案

## 4.1 `ProjectOptions.cmake`

### 导出内容

```cmake
option(QPPJS_BUILD_TESTS "Build tests" ON)
option(QPPJS_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(QPPJS_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(QPPJS_ENABLE_COVERAGE "Enable coverage instrumentation" OFF)
option(QPPJS_WARNINGS_AS_ERRORS "Treat warnings as errors" ON)

function(qppjs_setup_project_options)
    ...
endfunction()
```

### 负责内容

- `CMAKE_CXX_STANDARD=20`
- `CMAKE_CXX_STANDARD_REQUIRED=ON`
- `CMAKE_CXX_EXTENSIONS=OFF`

---

## 4.2 `DetectToolchain.cmake`

### 输出变量

```cmake
QPPJS_PLATFORM
QPPJS_COMPILER_FAMILY
QPPJS_GENERATOR_NAME
QPPJS_IS_MULTI_CONFIG
QPPJS_COVERAGE_BACKEND
```

### 判定规则

| 编译器 | family | coverage backend |
|---|---|---|
| GNU | `gcc` | `gcov` |
| Clang | `clang` | `llvm-cov` |
| AppleClang | `appleclang` | `llvm-cov` |
| MSVC | `msvc` | `unsupported` |

---

## 4.3 `ProjectTargets.cmake`

### 新增公共 target

```cmake
add_library(qppjs_project_options INTERFACE)
add_library(qppjs_project_warnings INTERFACE)
```

### 新增函数

```cmake
function(qppjs_setup_project_targets)
    ...
endfunction()

function(qppjs_configure_project_target target)
    target_link_libraries(${target}
        PUBLIC qppjs_project_options
        PRIVATE qppjs_project_warnings
    )
endfunction()
```

### 设计目的

- 减少 target 侧样板代码
- 把“项目公共配置”集中在 interface target
- 避免每个 target 调 4~5 个 apply 函数

---

## 5. 迁移步骤建议

## 第 1 步：拆分 `CompilerFlags.cmake`

- 新建：
  - `cmake/ProjectOptions.cmake`
  - `cmake/DetectToolchain.cmake`
  - `cmake/CompilerWarnings.cmake`
  - `cmake/Sanitizers.cmake`
  - `cmake/Coverage.cmake`
  - `cmake/ProjectTargets.cmake`
- 旧 `CompilerFlags.cmake` 暂时保留，逐步下线

## 第 2 步：顶层切到新入口

- 顶层 `CMakeLists.txt` 改为 include 新模块
- 建立 `qppjs_project_options` / `qppjs_project_warnings`

## 第 3 步：子目录切换接入点

- `src/CMakeLists.txt`
- `tests/CMakeLists.txt`

把：

```cmake
qppjs_apply_compiler_flags(target)
```

改为：

```cmake
qppjs_configure_project_target(target)
```

## 第 4 步：coverage metadata 输出

- configure 阶段写出 `qppjs-build-meta.json`
- 至少包含：平台、编译器族、generator、coverage backend、coverage 开关

## 第 5 步：重写 `scripts/coverage.sh`

- 改成读取 metadata
- 去掉 preset 名称判断逻辑

## 第 6 步：简化 `CMakePresets.json` 与 `build.sh`

- Presets 保留少量常用入口
- `build.sh` 降级为 configure/build/test 薄封装

---

## 6. 明确保留与删除项

## 6.1 保留项

| 项目 | 原因 |
|---|---|
| `src/CMakeLists.txt` / `tests/CMakeLists.txt` 分目录结构 | 清晰、稳定，没必要合并 |
| `Dependencies.cmake` | 第三方依赖入口独立合理 |
| `scripts/coverage.sh` | 覆盖率报告仍需脚本编排 |
| `CMakePresets.json` | 可作为便捷入口保留 |

## 6.2 删除/收缩项

| 项目 | 动作 |
|---|---|
| `qppjs_apply_compiler_flags(target)` | 拆分并废弃 |
| preset 全矩阵平铺 | 收缩为最小入口 |
| `build.sh` 中 preset 自动改写 | 删除 |
| `coverage.sh` 中按 preset 名字猜工具链 | 删除 |

---

## 7. 推荐的最终调用方式

## 7.1 原生 CMake（推荐主路径）

```bash
cmake -S . -B build/linux-clang-debug -G Ninja \
  -D CMAKE_CXX_COMPILER=clang++
cmake --build build/linux-clang-debug
ctest --test-dir build/linux-clang-debug --output-on-failure
```

覆盖率：

```bash
cmake -S . -B build/linux-clang-coverage -G Ninja \
  -D CMAKE_CXX_COMPILER=clang++ \
  -D QPPJS_ENABLE_COVERAGE=ON
cmake --build build/linux-clang-coverage
ctest --test-dir build/linux-clang-coverage --output-on-failure
./scripts/coverage.sh build/linux-clang-coverage
```

## 7.2 Preset（可选便捷路径）

```bash
cmake --preset linux-debug
cmake --build --preset linux-debug
ctest --preset linux-debug
```

---

## 8. 决策摘要

本次重构的关键决策如下：

1. **平台 / 编译器 / generator 差异只在 CMake 中建模**
2. **coverage instrumentation 属于 CMake；coverage report 属于脚本**
3. **真正全局的配置直接全局设置**
4. **项目内公共配置通过 INTERFACE target 继承**
5. **target 侧只保留一次 `qppjs_configure_project_target()` 接入**
6. **Preset 和 build.sh 只是入口，不再承载核心语义**

---

## 9. 后续落地顺序

建议按下面顺序实施：

1. 先拆 `CompilerFlags.cmake`
2. 再建立 `qppjs_project_options` / `qppjs_project_warnings`
3. 切换 `src/` 与 `tests/`
4. 输出 metadata
5. 重写 `coverage.sh`
6. 最后收缩 presets 与 build.sh

这样可以保持每一步都可编译、可回归、可验证。
