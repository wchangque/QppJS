# QppJS 下一阶段计划

本文件展开"下一阶段"的可执行内容，是从当前状态进入下一轮开发的直接行动卡。

## 1. 下一阶段

- 下一阶段：Phase 3：Object Model
- 对应路线图：`docs/plans/00-roadmap.md`
- 当前事实源：`docs/plans/01-current-status.md`

## 2. 阶段目标

在已具备完整 AST Interpreter 的基础上，引入 JS 对象模型：对象字面量、属性访问（点号 / 方括号）、属性赋值，为后续原型链和内置对象打基础。

## 3. 进入前提

当前已具备：
- 完整 Lexer（1.1 + 1.2）
- 完整 AST 节点体系（1.3）
- Pratt Parser，支持表达式 + 语句（1.4 + 1.5）
- AST dump（1.6）
- Parser 错误报告，含位置信息（1.7）
- AST Interpreter（Phase 2）：Environment/Scope、表达式求值、语句执行、ToBoolean、Completion 模型
- 321/321 测试全部通过

## 4. 本阶段任务分解

### 3.1 扩展 AST：对象字面量 + 属性访问

目标：
- `ObjectExpression`（`{ key: value, ... }`）
- `MemberExpression`（`obj.prop` / `obj[expr]`，含计算属性）
- `PropertyAssignment`（`obj.prop = val` / `obj[expr] = val`）

### 3.2 扩展 Value + Object 模型

目标：
- `Object` 升级为持有 `std::unordered_map<std::string, Value>` 属性字典
- 属性读取（`GetProperty`）、属性写入（`SetProperty`）
- `typeof obj` → `"object"`

### 3.3 扩展 Interpreter

目标：
- `eval_object_expr`：构造对象，填入属性
- `eval_member_expr`：读取属性（点号 / 方括号）
- 赋值左侧扩展：支持 `obj.prop = val` 和 `obj[expr] = val`

### 3.4 内置属性（最小子集）

目标（可选，按需）：
- `Array` 留待后续
- 此阶段仅确保普通对象属性读写正确

## 5. 建议执行顺序

1. 3.1 AST 扩展（先完成节点定义，后续模块依赖）
2. 3.2 Value/Object 模型升级
3. 3.3 Interpreter 扩展
4. 3.4 内置属性（可选）

## 6. 验证方式

本阶段完成后，应至少能验证：
- `let obj = { x: 1, y: 2 }; obj.x` → `1`
- `obj.x = 10; obj.x` → `10`
- `obj["y"]` → `2`
- `typeof {}` → `"object"`

## 7. 暂不处理内容

- 原型链与 `__proto__`（Phase 5）
- 函数作为属性（需 Phase 4 函数支持）
- `Array` 字面量（Phase 3 后续或单独阶段）
- `delete` 操作符

## 8. 退出条件

满足以下条件即可进入 Phase 4：
- 对象字面量可构造并赋值到变量
- 属性读写（点号 + 方括号）正确
- 属性不存在时返回 `undefined`
- 新增测试覆盖上述场景，全部通过
