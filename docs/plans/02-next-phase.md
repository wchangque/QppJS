# QppJS 下一阶段计划

本文件展开"下一阶段"的可执行内容，是从当前状态进入下一轮开发的直接行动卡。

## 1. 下一阶段

- 下一阶段：Phase 7：控制流扩展（break/continue/throw/try/catch/finally）
- 对应路线图：`docs/plans/00-roadmap.md`
- 当前事实源：`docs/plans/01-current-status.md`

## 2. 阶段目标

在 AST Interpreter 和 Bytecode VM 上同步支持以下特性：
- break/continue 语句（含 labeled）
- throw 语句
- try/catch/finally 语句
- Error 对象（new Error(msg)）

## 3. 进入前提

当前已具备：
- 完整 Lexer、Parser、AST（Phase 1）
- AST Interpreter，含 Environment/Scope、表达式求值、语句执行（Phase 2）
- Object Model（Phase 3）：JSObject、属性读写、对象字面量、成员访问
- Function（Phase 4）：JSFunction、函数声明/表达式、调用表达式、闭包、递归、var 提升
- 原型链、this、new（Phase 5）：proto_ 链、ScopeGuard this 绑定、NewExpression
- 531/531 测试全部通过

## 4. 设计方案摘要

### 4.1 指令集（49 条）

**值加载（7）**：LoadUndefined, LoadNull, LoadTrue, LoadFalse, LoadNumber(u16), LoadString(u16), LoadThis

**变量（6）**：GetVar(u16), SetVar(u16), DefVar(u16), DefLet(u16), DefConst(u16), InitVar(u16)

**作用域（2）**：PushScope, PopScope

**对象属性（5）**：NewObject, GetProp(u16), SetProp(u16), GetElem, SetElem

**函数与调用（6）**：MakeFunction(u16), Call(u8), CallMethod(u8), NewCall(u8), Return, ReturnUndefined

**算术（5）**：Add, Sub, Mul, Div, Mod

**一元（4）**：Neg, Pos, BitNot, Not

**比较（8）**：Lt, LtEq, Gt, GtEq, Eq, NEq, StrictEq, StrictNEq

**类型（2）**：Typeof, TypeofVar(u16)

**控制流（3）**：Jump(i32), JumpIfFalse(i32), JumpIfTrue(i32)

**栈操作（2）**：Pop, Dup

操作数格式：u8=1字节无符号、u16=2字节大端无符号、i32=4字节大端有符号（跳转偏移，相对跳转指令末尾）

### 4.2 BytecodeFunction 结构

```cpp
struct BytecodeFunction {
    std::vector<uint8_t> code;                                 // 指令流
    std::vector<Value> constants;                              // 常量池（number/string）
    std::vector<std::string> names;                            // 名称池（变量名/属性名）
    std::vector<std::shared_ptr<BytecodeFunction>> functions;  // 嵌套函数池
    std::vector<std::string> params;                           // 参数名列表
    std::optional<std::string> name;                           // 函数名（调试用）
    std::vector<uint16_t> var_decls;                           // var 声明的 names 索引
};
```

### 4.3 CallFrame 结构

```cpp
struct CallFrame {
    const BytecodeFunction* bytecode;   // 当前字节码（非拥有）
    size_t pc;                          // 程序计数器（字节偏移）
    std::vector<Value> stack;           // 操作数栈
    std::shared_ptr<Environment> env;   // 当前词法环境
    Value this_val;                     // 当前 this 绑定
};
```

### 4.4 新增文件

| 文件 | 职责 |
|---|---|
| `include/qppjs/vm/opcode.h` | Opcode 枚举（X-Macro）|
| `include/qppjs/vm/bytecode.h` | BytecodeFunction 结构体 |
| `include/qppjs/vm/compiler.h` | Compiler 类声明 |
| `src/vm/compiler.cpp` | Compiler 完整实现 |
| `include/qppjs/vm/vm.h` | VM 类声明 + CallFrame 结构体 |
| `src/vm/vm.cpp` | VM 完整实现（dispatch loop）|
| `tests/unit/vm_test.cpp` | VM 路径全量行为测试 |

**修改现有文件**：
- `include/qppjs/runtime/js_function.h`：增加 `bytecode_` 字段
- `include/qppjs/runtime/environment.h`：增加 `outer()` accessor
- `src/main/main.cpp`：增加 `--vm` 执行路径
- `src/CMakeLists.txt`、`tests/CMakeLists.txt`：注册新文件

## 5. 本阶段任务分解

### 6.1 指令集定义 + BytecodeFunction 骨架

目标：
- 新建 `include/qppjs/vm/opcode.h`：Opcode 枚举（X-Macro 形式，49 条指令）
- 新建 `include/qppjs/vm/bytecode.h`：BytecodeFunction 结构体（code/constants/names/functions/params/name/var_decls）
- 项目可编译通过
- 不需要任何运行时测试

### 6.2 Compiler 框架 + 字面量 + 算术表达式

目标：
- 新建 `include/qppjs/vm/compiler.h` + `src/vm/compiler.cpp`：Compiler 类骨架（emit/add_constant/add_name/emit_jump/patch_jump 等辅助函数）
- 编译 NumberLiteral/StringLiteral/BooleanLiteral/NullLiteral → LoadNumber/LoadString/LoadTrue/LoadFalse/LoadNull/LoadUndefined
- 编译 BinaryExpression（算术 + 比较 + 相等）
- 编译 UnaryExpression（Neg/Pos/BitNot/Not）
- 编译 ExpressionStatement（追加 Pop）
- 新建 `include/qppjs/vm/vm.h` + `src/vm/vm.cpp`：VM 骨架（dispatch loop，处理上述指令）
- 测试：`1 + 2` 编译并在 VM 中执行返回 3

### 6.3 变量、作用域、控制流

目标：
- 编译 VariableDeclaration（let/const/var）→ DefLet/DefConst/DefVar + InitVar
- 编译 Identifier → GetVar；赋值 → SetVar/InitVar
- 编译 BlockStatement → PushScope/PopScope（Environment::outer() accessor 需同步开放）
- 编译 IfStatement → JumpIfFalse + patch
- 编译 WhileStatement → 回跳 + patch
- 编译 LogicalExpression（&& / ||）→ Dup + JumpIfFalse/JumpIfTrue + Pop
- VM 实现对应指令
- 完成后：interpreter_test.cpp 中的变量/控制流用例在 VM 上全部通过

### 6.4 函数声明与调用

目标：
- compile_function_body（参数绑定、var 提升预扫描 hoist_vars_scan、发射 var_decls、编译函数体）
- 编译 FunctionDeclaration → 子函数编译 + MakeFunction + SetVar（提升到函数体入口）
- 编译 FunctionExpression → 子函数编译 + MakeFunction
- 编译 CallExpression → Call 指令
- 编译 ReturnStatement → Return 指令
- VM do_call（建立新 CallFrame、参数绑定、执行子函数、弹帧）
- VM Return/ReturnUndefined 处理
- 完成后：function_test.cpp 的函数调用/闭包/递归/var 提升用例在 VM 上通过

### 6.5 对象、属性、方法调用、this、new

目标：
- 编译 ObjectExpression → NewObject + SetProp × N
- 编译 MemberExpression → GetProp / GetElem
- 编译 MemberAssignmentExpression → SetProp / SetElem
- 编译方法调用（CallMethod：obj + callee + args 的栈布局）
- 编译 NewExpression → NewCall
- 编译 LoadThis（this 标识符 → LoadThis 指令）
- VM 实现 CallMethod（receiver 绑定 this）、NewCall（do_new）、GetProp/SetProp/GetElem/SetElem
- 完成后：object_test.cpp + proto_test.cpp 中的端到端用例在 VM 上通过

### 6.6 typeof 特殊处理

目标：
- 增加 TypeofVar(u16) 指令：安全 lookup，未声明返回 "undefined"
- compile_unary 中，typeof identifier → TypeofVar，其他 typeof expr → compile_expr + Typeof
- VM 实现 TypeofVar
- 完成后：typeof 系列测试在 VM 上全部通过（包括 typeof undeclaredVar）

### 6.7 全量 VM 测试 + main.cpp 集成

目标：
- 新建 `tests/unit/vm_test.cpp`：将现有 531 个测试用例复制并适配 VM 执行路径（替换 run 函数）
- `main.cpp` 增加 `--vm` flag
- 全量 VM 测试全部通过（531 个用例）
- 原有 531 个 Interpreter 测试继续通过（无回归）
- 总测试数：531（Interpreter）+ 531（VM）= 1062 个全部通过

## 6. 建议执行顺序

6.1 → 6.2 → 6.3 → 6.4 → 6.5 → 6.6 → 6.7，每完成一个子任务跑全量测试确认无回归。

## 7. 验证方式

Phase 6 完成后，应至少能验证：

```js
// 原型链 + this + new
function Animal(name) { this.name = name; }
Animal.prototype.speak = function() { return this.name; };
let a = new Animal("Cat");
a.speak()  // → "Cat"（通过 VM 执行）

// 闭包
function make_counter() {
    let n = 0;
    return function() { n = n + 1; return n; };
}
let c = make_counter();
c(); c(); c()  // → 3

// 控制流
let sum = 0;
let i = 1;
while (i <= 10) { sum = sum + i; i = i + 1; }
sum  // → 55
```

## 8. 暂不处理内容

- break/continue 语句（Phase 7）
- throw/try/catch/finally（Phase 7）
- 字节码序列化/反序列化
- QuickJS 字节码兼容
- upvalue 优化（继续用 Environment 链）
- NaN boxing
- atom 系统

## 9. 退出条件

满足以下条件即可进入 Phase 7：
- vm_test.cpp 中 531 个用例全部通过
- 原有 531 个 Interpreter 测试无回归
- main.cpp 支持 `--vm` flag 切换执行路径
- Phase 6 设计方案中的所有风险点均已处理或明确推迟
