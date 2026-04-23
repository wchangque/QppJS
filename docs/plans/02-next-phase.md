# QppJS 下一阶段计划

本文件展开"下一阶段"的可执行内容，是从当前状态进入下一轮开发的直接行动卡。

## 1. 下一阶段

- 下一阶段：Phase 8（基础内建对象）
- Phase 7 已全部完成（792/792 测试通过）
- 对应路线图：`docs/plans/00-roadmap.md`
- 当前事实源：`docs/plans/01-current-status.md`

## 2. 阶段目标

引入最小可用的内建对象集合，修复 Phase 7 遗留的规范偏差，在 AST Interpreter 和 Bytecode VM 两侧保持行为一致。

## 3. 进入前提

当前已具备：
- 完整 Lexer、Parser、AST（Phase 1）
- AST Interpreter，含 Environment/Scope、表达式求值、语句执行（Phase 2）
- Object Model（Phase 3）
- Function（Phase 4）：闭包、递归、var 提升
- 原型链、this、new（Phase 5）
- Bytecode VM（Phase 6）：49 条指令，含函数调用、对象属性、原型链
- 控制流扩展（Phase 7）：throw/try/catch/finally/break/continue/for/labeled/Error 基础内建
- 功能基线：792/792 测试通过，ASAN/LSan 全量回归无泄露

## 4. 已知遗留问题（Phase 7 P2）

以下问题在 Phase 8 中处理：

- **P2-1**：VM catch 参数与 catch 体共享同一 scope，规范要求两层独立作用域；`catch(e) { let e = 2; }` 在 VM 路径下会失败
- **P2-2**：VM `compile_labeled_stmt` 对非循环体的 labeled break 触发 `assert(false)`；Interpreter 路径已正确实现
- **P2-3**：内部运行时错误（ReferenceError/TypeError）以字符串值抛出，而非 Error 对象；影响 `instanceof` 等常见模式

## 5. 本阶段任务分解

### 8.1 Error 子类（优先）

目标：
- 将内部运行时错误从字符串改为真正的 Error 子类实例（TypeError、ReferenceError、RangeError）
- 支持 `instanceof Error`、`instanceof TypeError` 等判断
- Interpreter 和 VM 两侧均升级
- 修复 P2-3

### 8.2 console 对象

目标：
- 注册全局 `console` 对象
- 支持 `console.log(...args)`，多参数空格分隔输出到 stdout
- Interpreter 和 VM 两侧均可用

### 8.3 Array 基础

目标：
- 数组字面量 `[1, 2, 3]`（Parser + AST + Interpreter + VM）
- 下标读写 `arr[0]`、`arr[0] = x`
- `arr.length` 属性
- 最小方法：`push`、`pop`、`forEach`（或 `map`）

### 8.4 Object 内建方法

目标：
- `Object.keys(obj)` 返回自有可枚举属性名数组
- `Object.assign(target, source)` 浅拷贝属性
- `Object.create(proto)` 以指定原型创建对象

### 8.5 Function 内建方法

目标：
- `fn.call(thisArg, ...args)`
- `fn.apply(thisArg, argsArray)`
- `fn.bind(thisArg, ...args)` 返回绑定函数

### 8.6 VM catch 作用域修复（P2-1）

目标：
- catch 参数绑定与 catch 体使用两层独立作用域
- 使 `catch(e) { let e = 2; }` 在 VM 路径下正确报错（或正确隔离）
- 与 Interpreter 行为对齐

### 8.7 VM labeled break 修复（P2-2）

目标：
- `compile_labeled_stmt` 支持非循环体的 labeled break
- 将 `assert(false)` 替换为正确实现或明确错误返回
- 与 Interpreter 行为对齐

## 6. 建议执行顺序

8.1 → 8.2 → 8.3 → 8.4 → 8.5 → 8.6 → 8.7

每完成一个子任务跑全量测试确认无回归。

8.1 优先级最高（规范偏差，影响 catch 中对错误类型的判断）。
8.6、8.7 可在 8.1 完成后穿插进行，不阻塞 8.2～8.5。

## 7. 验证样例

Phase 8 完成后，应至少能验证：

```js
// Error 子类
try {
    null.x;
} catch (e) {
    e instanceof TypeError  // → true
    e.message               // → 非空字符串
}

// console.log
console.log("hello", 42);  // 输出: hello 42

// Array
let arr = [1, 2, 3];
arr.push(4);
arr.length  // → 4
arr[0]      // → 1

// Object 内建
let keys = Object.keys({a: 1, b: 2});
keys[0]  // → "a"

// Function 内建
function greet(name) { return "hi " + name; }
greet.call(null, "world")  // → "hi world"
```

## 8. 暂不处理内容

- `for...in` / `for...of`（Phase 9 前考虑）
- generator / iterator
- Promise / async
- 完整 property descriptor（configurable/enumerable/writable）
- Array 稀疏表示优化
- 完整 String 内建方法

## 9. 退出条件

满足以下条件即可进入 Phase 9：
- Error 子类（TypeError/ReferenceError）可用，`instanceof` 判断正确
- `console.log` 可用
- Array 基础可用（字面量、下标、length、push/pop）
- Object.keys / Object.assign / Object.create 可用
- Function.prototype.call / apply / bind 可用
- VM catch 作用域与 Interpreter 行为对齐（P2-1）
- VM labeled break 非循环体场景不再 assert 崩溃（P2-2）
- 原有 792 个测试无回归
- 状态文档已同步
