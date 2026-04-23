# QuickJS 引擎设计参考文档

> 面向从零实现 JS 引擎的开发者，基于 QuickJS 源码分析整理。

## 目录

| 模块 | 文档 | 核心概念 |
|------|------|----------|
| 01 | [Value 表示系统](01-value-representation.md) | NaN-boxing、类型标签、引用计数 |
| 02 | [Runtime 与 Context](02-runtime-context.md) | 生命周期、职责划分、内存管理 |
| 03 | [Atom 与 String](03-atom-string.md) | 字符串 interning、双编码、Rope |
| 04 | [对象模型与 Shape](04-object-shape.md) | 隐藏类、属性系统、JSObject 结构 |
| 05 | [垃圾回收](05-garbage-collection.md) | 引用计数 + 循环检测、三步 GC |
| 06 | [字节码与指令集](06-bytecode.md) | 指令格式、常量池、调试信息 |
| 07 | [词法分析器](07-lexer.md) | Token、ASI、UTF-8 处理 |
| 08 | [解析器与编译器](08-parser-compiler.md) | 递归下降、无 AST 直接产出字节码、标签回填 |
| 09 | [执行引擎](09-interpreter.md) | 栈帧、字节码分发、异常处理 |
| 10 | [闭包与作用域](10-closure-scope.md) | Upvalue、开放/关闭、变量捕获 |
| 11 | [Generator 与 Async](11-generator-async.md) | 协程、堆上栈帧、Promise 集成 |
| 12 | [模块系统](12-module-system.md) | ES Module、DFS 链接、TLA |
| 13 | [Promise 与 Job Queue](13-promise-job-queue.md) | Microtask、宿主事件循环 |
| 14 | [内置对象注册](14-builtin-objects.md) | JSClassDef、magic 字段、批量注册 |

## 阅读顺序

**最小可运行引擎路径（推荐顺序）：**

```
01 Value → 02 Runtime/Context → 03 Atom/String → 04 Object/Shape
→ 05 GC → 06 Bytecode → 07 Lexer → 08 Parser → 09 Interpreter
→ 10 Closure → 11 Generator/Async → 12 Module → 13 Promise → 14 Builtin
```

## 源文件对照

| 源文件 | 行数 | 内容 |
|--------|------|------|
| `quickjs.h` | 1168 | 公开 API、Value 编码宏 |
| `quickjs.c` | 56029 | 引擎核心（全部模块） |
| `quickjs-opcode.h` | 370 | 字节码指令表（X-Macro） |
| `quickjs-atom.h` | 270 | 预定义 Atom 表（X-Macro） |
| `libregexp.c` | 3280 | 正则引擎 |
| `libbf.c` | 8473 | BigInt/BigFloat 算术 |
| `libunicode.c` | 2123 | Unicode 分类/规范化 |
| `cutils.c` | 633 | 通用工具（list、dtoa） |
