# QppJS 下一阶段计划

本文件展开"下一阶段"的可执行内容，是从当前状态进入下一轮开发的直接行动卡。

## 1. 下一阶段

- 下一阶段：Phase 5：原型链、this、new
- 对应路线图：`docs/plans/00-roadmap.md`
- 当前事实源：`docs/plans/01-current-status.md`

## 2. 阶段目标

在已具备完整 Function 的基础上，引入面向对象的三个核心机制：原型链属性查找、this 绑定、new 表达式。完成后应能支持最基本的构造函数模式、实例方法、原型继承查找。

## 3. 进入前提

当前已具备：
- 完整 Lexer、Parser、AST（Phase 1）
- AST Interpreter，含 Environment/Scope、表达式求值、语句执行（Phase 2）
- Object Model（Phase 3）：JSObject、属性读写、对象字面量、成员访问
- Function（Phase 4）：JSFunction、函数声明/表达式、调用表达式、闭包、递归、var 提升
- 475/475 测试全部通过

## 4. 本阶段任务分解

### 5.1 JSObject proto_ 字段 + 原型链查找 + object_prototype_

目标：
- `js_object.h`：添加 `proto_` 字段（`shared_ptr<JSObject>`，null 表示链尾）、`set_proto()`、`proto()` accessor
- `js_object.cpp`：`get_property` 改为沿原型链查找；原自身查找提取为内部 helper `get_own_property`
- Interpreter 构造函数：创建全局唯一 `object_prototype_`（proto_ = nullptr）
- `eval_object_expr`：新建 JSObject 时调用 `set_proto(object_prototype_)`
- 测试：属性未命中时能从 proto 上读到值；多级原型链；proto 为 null 时返回 undefined；set_property 只写自身

### 5.2 JSFunction prototype_ 字段 + 急切初始化

目标：
- `js_function.h`：添加 `prototype_` 字段（`shared_ptr<JSObject>`）和 accessor
- Interpreter：抽取 `make_function_value` 方法；`eval_function_decl` / `eval_function_expr` 改调 `make_function_value`
- 急切初始化：创建 JSFunction 时立即创建 prototype 对象（proto = object_prototype_），设 `constructor` 属性
- `eval_member_expr`：修改 kFunction 分支，支持读取 `F.prototype`（其他属性仍返回 undefined）
- 测试：`typeof F.prototype === "object"`；`F.prototype.constructor === F`；现有 475 个测试无回归

### 5.3 this 关键字支持 + ScopeGuard 扩展

目标：
- `token.h`：添加 `KwThis` token（当前 `this` 被识别为 Identifier，需要新增）
- `token.cpp`：关键字查表中注册 `"this"` → `KwThis`
- `parser.cpp`：nud 中为 `KwThis` 添加分支，产生 `Identifier{name: "this"}`
- `interpreter.h`：添加 `current_this_` 成员（初始化为 undefined）；ScopeGuard 扩展 `saved_this` / `new_this` 参数
- `interpreter.cpp`：`eval_identifier` 添加 `if (expr.name == "this")` 分支；ScopeGuard 析构时恢复 this；普通调用时 this = undefined
- 测试：普通调用 `this === undefined`；现有测试无回归

### 5.4 方法调用 this 提取 + call_function 抽取

目标：
- 将 `eval_call_expr` 的执行逻辑抽取为 `call_function(fn, this_val, args)`
- `eval_call_expr`：检测 callee 是否为 MemberExpression；是则提取 this_val = 对象，从对象（走原型链）取方法；否则 this_val = undefined
- 测试：`obj.method()` 中 `this === obj`；链式调用 `obj.a.b()` 中 `this === obj.a`；方法不存在抛 TypeError

### 5.5 NewExpression AST + Parser + eval_new_expr

目标：
- `ast.h`：新增 `NewExpression`（callee + arguments + range）；扩展 ExprNode variant
- `parser.cpp`：`KwNew` 的 nud 分支（带括号形式 `new F(args)`，优先级高于成员访问）
- `interpreter.cpp`：`eval_new_expr`（创建对象、设置 proto、执行构造函数、处理返回值）
- `ast_dump.cpp`：新增 NewExpression dump 分支
- 测试：基础 new 调用；prototype 属性继承；构造函数返回对象覆盖；返回 null 不覆盖；超出调用栈 RangeError；对非函数 new → TypeError

## 5. 建议执行顺序

5.1 → 5.2 → 5.3 → 5.4 → 5.5，每完成一个子任务跑全量测试确认无回归。

## 6. 验证方式

本阶段完成后，应至少能验证：

```js
// 原型链
function Animal(name) { this.name = name; }
Animal.prototype.speak = function() { return this.name; };
let a = new Animal("Cat");
a.speak()  // → "Cat"

// this 绑定
let obj = { x: 42, get: function() { return this.x; } };
obj.get()  // → 42

// new 基础
function Point(x, y) { this.x = x; this.y = y; }
let p = new Point(1, 2);
p.x  // → 1
```

## 7. 暂不处理内容

- `class` 语法
- `Object.create`、`Object.getPrototypeOf` 等内建方法
- `instanceof` 运算符
- getter/setter（property descriptor）
- `arguments` 对象、箭头函数的 this 语义
- 原始值的包装对象属性查找
- `new F`（无括号形式）
- `new.target`

## 8. 退出条件

满足以下条件即可进入 Phase 6：
- 原型链属性查找正确工作（多级链）
- 方法调用时 this 正确绑定到调用对象
- new 表达式能创建实例、关联原型、执行构造函数
- 新增测试覆盖上述场景，全量测试全部通过
