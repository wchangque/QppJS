# 代码风格

完整规范见 `docs/google-cpp-style-guide.md`，以下是 agent 必须遵守的核心规则。

## 改动原则

- 只修改完成任务所必需的代码，保持改动最小。
- 优先复用现有模式，不为假设性的未来需求添加抽象。

## 命名

| 实体 | 风格 | 示例 |
|------|------|------|
| 文件名 | `snake_case.cpp` / `.h` | `my_class.cpp` |
| 类 / 结构体 / 枚举 / 类型别名 | `PascalCase` | `MyClass` |
| 普通变量、函数参数 | `snake_case` | `table_name` |
| 类数据成员 | `snake_case_`（尾部下划线） | `table_name_` |
| 结构体数据成员 | `snake_case`（无尾部下划线） | `table_name` |
| 常量（`constexpr` / `const` 静态） | `kPascalCase` | `kDaysInAWeek` |
| 普通函数 | `PascalCase` | `AddTableEntry()` |
| getter / setter | `snake_case` | `count()`, `set_count()` |
| 命名空间 | `snake_case` | `qppjs` |
| 枚举值 | `kPascalCase` | `kOutOfMemory` |
| 宏 | `ALL_CAPS_WITH_PREFIX` | `QPPJS_ROUND` |

## 格式

- 缩进：**4 个空格**，不使用 Tab。
- 行长：最多 **120 字符**（URL、字符串字面量、`#include` 除外）。
- 左花括号不另起一行（K&R 风格）。
- 函数名与左括号之间无空格；括号内侧无空格。
- 二元运算符两侧各一个空格；逗号后一个空格。
- `public:` / `protected:` / `private:` 不缩进（与 `class` 对齐）。

## 注释

- 默认不写注释；只在原因不明显时补充简短说明（解释"为什么"，不描述"做了什么"）。
- 全局变量必须注释说明用途。
- 类数据成员若有非显而易见的不变量（如哨兵值），需加注释。

## 头文件与作用域

- 头文件使用 `#pragma once`。
- include 顺序：关联头文件 → C 系统头 → C++ 标准库 → 第三方库 → 项目内头文件，各组空一行。
- 禁止 `using namespace xxx`。
- 文件内部实现用匿名命名空间或 `static`。

## 类与函数

- 单参数构造函数和转换运算符加 `explicit`。
- 虚函数重写标注 `override`。
- `struct` 仅用于无行为的数据载体；有方法的用 `class`。
- 优先返回值而非输出参数。
- 禁止使用异常；禁止 RTTI（`dynamic_cast` / `typeid`）。
- 使用 `static_cast` 等 C++ 风格转型，不用 C 风格 `(Type)` 转型。
- 独占所有权用 `std::unique_ptr`；共享所有权才用 `std::shared_ptr`。
