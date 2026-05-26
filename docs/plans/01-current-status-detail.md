# QppJS 当前开发状态（详情）

本文件是 `01-current-status.md` 的完整版，供 `/implement` 流程和需要完整历史的场景使用。

## 1. 已完成任务

- [x] **方法简写 Method Shorthand [T262-P1]**（2026-05-26）：
  - **实现内容**：`MethodKind` 枚举（kData/kMethod/kGetter/kSetter/kAsyncMethod/kGenerator）；`ObjectProperty` 增加 `method_kind` 字段；`BytecodeFunction::is_method`；`JSFunction::is_method_` + getter/setter；`kDefineGetter`/`kDefineSetter` 两条新 opcode（2 字节操作数）；Parser `nud(LBrace)` 扩展 `*foo(){}`（kGenerator）、`get/set foo(){}`（kGetter/kSetter，含消歧：`get:`/`get,`/`get}`/`get=`/`get(` fallthrough 为数据属性）、`async foo(){}`（kAsyncMethod，含消歧）、普通方法简写 `foo(){}`（kMethod，key 已解析后遇 `(` 时触发）；Interpreter `eval_object_expr` 分支处理每种 MethodKind（kGetter/kSetter 调用 `define_property` + PropDesc）；Interpreter `call_function` 新增守卫 0：`is_method() && is_new_call → TypeError`（置于 native 检查前，覆盖 async 方法场景）；Compiler `compile_object_expr` 直接调用 `compile_function` 并设置 `child->is_method = true` / `child->is_async = true`，kGetter/kSetter 使用 `kDefineGetter`/`kDefineSetter` 指令；VM `kMakeFunction` 设 `fn->set_is_method(fn_bc->is_method)` + `.name` 写入（仅 is_method 路径）；async 方法 wrapper 跳过 proto 创建；`kDefineGetter`/`kDefineSetter` VM 处理器（pop fn + pop obj，define_property，push fn）；`kNewCall` 在 is_arrow 守卫之后增加 is_method 守卫；`ast_dump.cpp` 扩展 ObjectProperty 输出 method_kind；新增 `tests/unit/method_shorthand_test.cpp`（32 个测试 MS-01～MS-32，覆盖 8 个 Parser 用例 + 12 个 Interpreter 用例 + 12 个 VM 用例）。
  - **测试结果**：3949/3949 通过（coverage），0 LSan 泄漏。

- [x] **compiler.cpp build 修复 + test262 通过率基准建立**（2026-05-26）：
  - **根因**：`src/vm/compiler.cpp` 使用 `std::any_of` 但缺少 `#include <algorithm>`，导致增量构建失败后二进制停留在旧版本；旧版本三元运算符 `?:` 完全无法解析，language/expressions 通过率仅 26.5%。
  - **修复**：`compiler.cpp` 头部补加 `#include <algorithm>`，重建成功后通过率升至 32.4%（+5.9pp）。
  - **test262 失败原因矩阵**（language/expressions，5837 个失败）：
    - SyntaxError 5293 个（91%）：`unexpected token: Star` 545（generator）、`expected '}'` 517（method shorthand）、`expected property key` 333（computed 键）、`expected function name` 176（method shorthand）、`expected parameter name` 150（解构参数）、`unexpected token: Question` 28（`??`）、`unexpected token: Dot` 24（`?.`）
    - RuntimeError 544 个（9%）：`eval not defined` 91、`Function not defined` 60、`Object spread not supported` 27、`TypeError value is not a function` 26
  - **待修复优先列表（已写入 01-current-status.md 和 02-next-phase.md）**：T262-P2 计算属性键、T262-P3 `??`、T262-P4 `?.`。

- [x] **位运算符 Review 必修修复 M1/M2/M3**（2026-05-26）：
  - **M1 hasOwnProperty kFunction 修复**：`interpreter.cpp` + `vm.cpp` 的 `Object.prototype.hasOwnProperty` lambda 删除 `raw->object_kind() == ObjectKind::kFunction` 早返回，新增 kFunction 分支 `static_cast<JSFunction*>(raw)->has_property(key)`，使 `Number.hasOwnProperty("MAX_VALUE")` 等正确返回 `true`。
  - **M2 isPrototypeOf kFunction 修复**：`interpreter.cpp` + `vm.cpp` 的 `Object.prototype.isPrototypeOf` lambda 改为 `[this]` 捕获，`args[0]` 为 kFunction 时检查 `needle == function_prototype_.get()` 或 `needle == object_prototype_.get()`，使 `Object.prototype.isPrototypeOf(Number.isNaN)` 正确返回 `true`。
  - **M3 Number.MIN_VALUE 修复**：`interpreter.cpp:2833` 和 `vm.cpp:2800` 的 `std::numeric_limits<double>::min()`（= 2.22e-308，最小正规化 double）改为 `denorm_min()`（= 5e-324，JS 规范要求的最小正值）。
  - **测试**：3919/3919 通过（coverage），0 LSan 泄漏。

- [x] **位运算符 `&` `|` `^` `~` `<<` `>>` `>>>` 及复合赋值**（2026-05-26）：
  - **词法**：新增 9 个 token（LShift/RShift/URShift/AmpEq/PipeEq/CaretEq/LShiftEq/RShiftEq/URShiftEq）；lexer.cpp `&`/`|`/`^`/`<`/`>` 各 case 扩展最长匹配（`>>=`/`>>>=`/`<<=` 均正确扫描）。
  - **AST**：`UnaryOp::BitNot`；`BinaryOp::{BitAnd,BitOr,BitXor,Shl,Sar,Shr}`；`AssignOp::{BitAndAssign,BitOrAssign,BitXorAssign,ShlAssign,SarAssign,ShrAssign}`。
  - **Opcode**：6 个新 opcode（kBitAnd/kBitOr/kBitXor/kShl/kSar/kShr），均 0 字节操作数。
  - **ToInt32/ToUint32**：新建 `include/qppjs/runtime/number_utils.h`，提供内联 `to_int32_bits(double)` / `to_uint32_bits(double)`（含小整数快路径）；kBitNot 同步修复使用 `to_int32_bits` 替代直接 `static_cast`。
  - **lbp 全局调整**：插入 BitOR(7)/BitXOR(8)/BitAND(9)/Shift(14) 四个新优先级；已有 EqEq(8→11)、Instanceof/Lt/Gt(10→13)、Plus/Minus(12→15)、Star/Slash/Percent(14→17)、LParen(16→19)、PlusPlus/MinusMinus(17→20)、Dot/LBracket(18→21) 统一上移 +3；一元前缀 `parse_expr(15)→18`，await `parse_expr(14)→18`，new callee `parse_expr(17)→20`；contextual `in` lbp 9→12。
  - **Interpreter 双路径**：`eval_unary` 添加 BitNot；`eval_binary` 添加 6 个位运算 case；`eval_assignment` 添加 6 个复合赋值 case。所有路径使用 `to_int32_bits`/`to_uint32_bits`，Shr 用 uint32 路径。
  - **VM/Compiler 双路径**：`compile_unary` 添加 UnaryOp::BitNot→kBitNot；`compile_binary` 添加 6 个 BinaryOp case；`compile_assignment` 添加 6 个 AssignOp case；vm.cpp 添加 6 个 opcode handler，对称实现。
  - **ast_dump**：`unary_op_str`/`binary_op_str`/`assign_op_str` 补全新枚举值字符串。
  - **测试**：更新 4 个 lexer_test（原记录 `<<`/`>>` 切分行为的"待实现"测试，更新为正确的单 token 期望）；新建 `tests/unit/bitwise_test.cpp`（BW-01~BW-30 × Interp+VM = 60 个测试，覆盖 BitNot/BitAnd/BitOr/BitXor/Shl/Sar/Shr/全部复合赋值/优先级验证/ToInt32 边界/float 截断）。3879/3879 通过（coverage），3817/3817 通过（run_ut ASAN），0 LSan 泄漏。

- [x] **`typeof` 运算符专属测试 + `typeof this` Interpreter 修复**（2026-05-26）：
  - **背景**：typeof 原已实现（`kTypeof`(0) + `kTypeofVar`(2) 双指令；compiler.cpp 编译期分路；vm.cpp + interpreter.cpp 双路径）。本轮仅补测试和修复已知 Bug。
  - **Bug M1 修复**：`src/runtime/interpreter.cpp` `eval_unary` typeof 路径（约第 4572 行）对所有 Identifier（含 "this"）执行 `current_env_->lookup`，`lookup("this")` 返回 nullptr → 返回 "undefined"。修复为将 `if (id.name == "undefined") return "undefined"` 冗余分支删除，改为 `if (id.name != "this")` 守卫，使 `typeof this` 落入下方的 `eval_expr` 路径，正确求值 `current_this_`。VM 路径不受影响（compiler.cpp 第 984 行已排除 "this"）。
  - **测试**：新建 `tests/unit/typeof_test.cpp`（60 个测试，TY-01~TY-30 × Interp+VM）：
    - TY-01~07：基础值类型映射（undefined/null/boolean/number/NaN/Infinity/string/symbol）
    - TY-08~10：对象与数组 → "object"
    - TY-11~13：普通函数、箭头函数、内置函数 → "function"
    - TY-14：未声明变量不抛 ReferenceError（kTypeofVar 豁免）
    - TY-15：逗号表达式路径（非 kTypeofVar，全局变量豁免）
    - TY-16：TDZ 行为（Interp 正确抛 ReferenceError；VM kDefLet 内联导致 lookup 返回 null → "undefined"，记录差异）
    - TY-17~19：var 变量、函数参数、对象属性
    - TY-20~22：包装对象（Interp new Number/String/Boolean → "object"；VM new Number → "number" 记录差异）
    - TY-23~30：赋值、嵌套 typeof、函数返回值、let/const 变量、if 条件、类型守卫、表达式操作数等
  - **已知 VM 差异**：(1) TY-16 VM：kDefLet 内联 → typeof TDZ 变量返回 "undefined"（非 ReferenceError）；(2) TY-20 VM：kNewCall native constructor 不创建 this-obj → new Number() 返回 primitive → typeof 为 "number"。
  - **修改文件**：`src/runtime/interpreter.cpp`（typeof this 守卫），`tests/unit/typeof_test.cpp`（新建），`tests/CMakeLists.txt`（新增注册）。
  - 3819/3819 通过（coverage），0 LSan 泄漏。

- [x] **`in` 运算符（in operator）**（2026-05-26）：
  - **AST**：`include/qppjs/frontend/ast.h` `BinaryOp` 枚举新增 `In`；`src/frontend/ast_dump.cpp` 添加 `case BinaryOp::In: return "In"` 分支。
  - **Opcode**：`include/qppjs/vm/opcode.h` 新增 `X(In, 0)`（0-byte 操作数，pop rhs，pop lhs → push bool）。
  - **JSObject**：`include/qppjs/runtime/js_object.h` 声明 `has_property(const std::string& key) const` 和 `has_property_by_symbol(uint64_t symbol_id) const`；`src/runtime/js_object.cpp` 实现两个方法，均使用 `while (cur != nullptr)` 迭代原型链（与 get_property 对称，不用递归）。`has_property` 处理：Array length 特判、`try_parse_array_index` 数字索引检测（elements_ 是 unordered_map，key 不存在 = hole = false）、`index_map_` 普通属性、`has_constructor_property_` constructor 特判。
  - **Parser**：`src/frontend/parser.cpp` Pratt loop 为 Token::Ident "in"（当 `no_in_ == false`）设置 lbp=9；`led()` 构建 `BinaryExpression(BinaryOp::In)`；`is_in_token()` 辅助函数；`no_in_` guard 已有，for-loop head 自动隔离。
  - **Interpreter**：`src/runtime/interpreter.cpp` `eval_binary_expr` 新增 `BinaryOp::In` 分支：RHS 非对象 → TypeError；`lv.is_symbol()` → `has_property_by_symbol`；else → `to_string_val(lv)` → `has_property`。
  - **Compiler**：`src/vm/compiler.cpp` `compile_binary_expr` 新增 `BinaryOp::In` 分支：`compile_expr(*lhs)`; `compile_expr(*rhs)`; `emit(kIn)`。
  - **VM**：`src/vm/vm.cpp` `kIn` handler：pop rhs/lhs；RHS 非对象 → pending_throw TypeError；is_symbol → has_property_by_symbol；else → has_property；push bool。
  - **Tests**：`tests/unit/in_operator_test.cpp`，IN-01～IN-20，共 39 个测试（Interp+VM 双路径）。
  - **修改文件**：`include/qppjs/frontend/ast.h`、`include/qppjs/vm/opcode.h`、`include/qppjs/runtime/js_object.h`、`src/runtime/js_object.cpp`、`src/frontend/parser.cpp`、`src/frontend/ast_dump.cpp`、`src/runtime/interpreter.cpp`、`src/vm/compiler.cpp`、`src/vm/vm.cpp`、`tests/CMakeLists.txt`、`tests/unit/in_operator_test.cpp`（新建）。
  - 3733/3733 通过（coverage），0 LSan 泄漏。

- [x] **解构赋值 Review 必修问题修复 M1/M2/M3/M4/M5 + P1.2**（2026-05-25）：
  - **M1**：`src/vm/compiler.cpp` `has_block_scope_decl()` 新增 `DestructuringDeclaration` 分支（let/const 时返回 true）。修复 `{ let {x} = {x:1}; } x;` 应抛 ReferenceError 但实际泄漏变量的 Bug。
  - **M2**：`src/runtime/interpreter.cpp` `eval_destructuring_decl()` 在调用 `bind_pattern` 前，对 let/const 模式先调用 `collect_pattern_names` 收集所有 IdentifierPattern 名字，然后对每个名字 `current_env_->define(name, kind)`（TDZ 预声明）。确保默认值表达式执行时，后续变量名已在 env 中可见（TDZ 状态）。
  - **M3**：`src/runtime/interpreter.cpp` `hoist_vars_stmt` ForOfStatement 分支新增 `pattern_binding` 检查：`for (var [x] of ...)` 时调用 `collect_pattern_names(*for_of.pattern_binding, names)` 并对每个名字 `var_target.define_initialized`，与 VM 侧 `hoist_vars_scan_stmt` 的 pattern_binding 分支对齐。
  - **M4**：`eval_object_expr`（interpreter.cpp）+ `compile_object_expr`（compiler.cpp）在属性值为 `SpreadElement` 时抛 SyntaxError "Object spread not supported"，比原来的 "invalid use of spread element" 更清晰。不影响对象解构赋值（已通过 cover grammar 转换为 DestructuringAssignmentExpression）。
  - **M5**：`eval_object_expr` + `compile_object_expr` 在属性值为 `AssignmentExpression` 时抛 SyntaxError "Invalid shorthand property initializer"，修复 `({a = 1})` 静默执行副作用赋值的 Bug。
  - **P1.2**：`src/vm/vm.cpp` `kCopyDataProperties` handler 中 excluded key 收集改用 `k.sv()` 替代 `to_string_val(k)`，避免对确定是字符串的 excluded key 做不必要的类型转换。
  - **M6**（跳过）：IteratorClose 异常路径不完整——记录为已知限制，本轮不修复。
  - **新增测试（DS-38～DS-43 × Interp+VM = 12 个）**：DS-38/39 验证 M1 块作用域隔离；DS-40 验证 M3 var pattern hoist；DS-41 验证 M2 let 解构正常绑定；DS-42 验证 M4 对象字面量 spread 报错；DS-43 验证 M5 shorthand+default 报错。
  - **修改文件**：`src/vm/compiler.cpp`（has_block_scope_decl + compile_object_expr），`src/runtime/interpreter.cpp`（hoist_vars_stmt + eval_destructuring_decl + eval_object_expr），`src/vm/vm.cpp`（kCopyDataProperties），`tests/unit/destructuring_test.cpp`（+12 个测试）
  - 3694/3694 通过（coverage），0 LSan 泄漏。

- [x] **解构赋值 Testing Agent 边界补测 + var 解构 hoist Bug 修复**（2026-05-25）：
  - **新增测试（DS-19～DS-37，3 Parser + 19 Interp + 19 VM = 41 个）**：
    - DS-19：嵌套对象默认值 `let {a:{b=1}={}} = {}` → b=1（Parser+Interp+VM）
    - DS-20：数组默认值非触发条件（0/false/null 均不触发，仅 undefined 触发）
    - DS-21：默认值副作用——只在 undefined 时调用一次（call count 验证）
    - DS-22：对象 rest 明确排除已命名属性（`{a,b,...rest}` rest 无 a/b）
    - DS-23：对象 rest 不含原型链属性（Object.create 后解构，rest 只含 own 属性）
    - DS-24：对象 rest 是新对象（`rest !== src`）
    - DS-25：字符串解构 `let [a,b]='hi'` → a==='h',b==='i'（走 spread_into 字符串快路径）
    - DS-26：空数组 rest `let [a,...rest]=[1]` → rest===[]
    - DS-27：赋值表达式结果是 RHS 值 `x=([a,b]=[1,2])` → x[0]===1
    - DS-28：数组+对象混合嵌套 `[{a},{b}]=[{a:1},{b:2}]`
    - DS-29：三层嵌套 `{a:{b:{c}}}={a:{b:{c:42}}}` → c===42
    - DS-30：中间层为 null → TypeError
    - DS-31：`const {a} = undefined` → TypeError
    - DS-32：`const [a] = undefined` → TypeError
    - DS-33：var 解构执行 `var {a,b}={a:10,b:20}; a+b`（回归 Bug 修复验证）
    - DS-34：for-of 嵌套解构 `{name, scores:[first]}`
    - DS-35：自定义 Symbol.iterator 用于数组解构
    - DS-36：迭代器耗尽后多余元素为 undefined
    - DS-37：`{x:undefined}` 解构 undefined 值触发默认（对象属性显式 undefined）
    - Parser：rest 非末尾 SyntaxError × 2（数组+对象）；成员表达式赋值目标当前 SyntaxError（已知限制文档化）
  - **Bug 修复（interpreter.cpp `hoist_vars_stmt`）**：缺少 `DestructuringDeclaration` 分支，导致 `var {a,b}=obj` 中 a/b 未被提升到 var 作用域（执行时 `env->set("a", val)` 找不到绑定 → ReferenceError）。修复：新增 `collect_pattern_names` 静态辅助函数（递归访问 PatternNode variant，将所有 IdentifierPattern 名字收集到 `vector<string>`）；在 `hoist_vars_stmt` 新增 `DestructuringDeclaration` 分支，`var` 种类时调用 `collect_pattern_names` 再对每个名字 `var_target.define_initialized`，与 VM compiler `hoist_vars_scan_pattern` 语义完全对齐。
  - **修改文件**：`src/runtime/interpreter.cpp`（+`collect_pattern_names` 函数 + `hoist_vars_stmt` DestructuringDeclaration 分支），`tests/unit/destructuring_test.cpp`（+41 个测试）
  - 3682/3682 通过（coverage），0 LSan 泄漏。

- [x] **函数默认参数值 Review 必修修复 M1/M2/M3/M4**（2026-05-25）：
  - **M1（P1-A）解释器求值顺序**：`call_function` 的 param_defs 循环改为先 `fn_env->define(name, VarKind::Var)` + `fn_env->initialize(name, arg_val)`（实参或 undefined），再按需 `eval_expr(default)` + `fn_env->set(name, default_val)`。确保前序参数在后续默认值求值时已经在 fn_env 中可见（`f(a=1, b=a+1)` 无参调用 b=2 正确）。
  - **M2（P1-B）解释器 arguments/this 早建立**：将 `arguments` 对象创建和 `actual_this` 计算移到 param_defs 绑定循环之前（含临时设置/恢复 `current_this_`），使默认值表达式可引用 `arguments` 和 `this`（`f(a=arguments.length)` 正确）。
  - **M3（P1-C）VM hoist_vars 顺序**：`compiler.cpp` 将 kDefVar 字节码序列从 prologue 之前移到 prologue 之后；`vm.cpp push_call_frame` 移除对 `bc->var_decls` 的预定义（保留 `function_decls`）。body var 在默认值求值期间不可见，外层同名变量被正确引用（`f(a=x){var x='inner'}` 在外层 `x='outer'` 时 a='outer'）。
  - **M4（P2-A）解析器优先级**：`parser.cpp` 将默认值解析从 `parse_expr(2)` 改为 `parse_expr(1)`（允许 lbp=2 的赋值运算符作为默认值，`f(a=x=1)` 正确解析）。
  - **新增测试（DP-42～DP-45）**：双默认前序引用（Interp+VM）、arguments 在默认值中（Interp）、body var 不可见（VM+Interp）、赋值表达式默认值（Interp+VM）。
  - **修改文件**：`src/frontend/parser.cpp`（1 行），`src/vm/compiler.cpp`（kDefVar 移位），`src/vm/vm.cpp`（移除 var_decls 预定义），`src/runtime/interpreter.cpp`（M1+M2 重构）。
  - 3601/3601 通过（coverage），0 LSan 泄漏。

- [x] **函数默认参数值（Default Parameter Values）**（2026-05-25）：
  - **AST 变更**：新增 `ParamDef{string name, shared_ptr<ExprNode> default_init}` 结构体（`ast.h`）；`FunctionExpression`/`ArrowFunctionExpression`/`AsyncFunctionExpression`/`FunctionDeclaration`/`AsyncFunctionDeclaration` 五个 AST 节点的 `params` 字段从 `vector<string>` 改为 `vector<ParamDef>`；`ast_dump.cpp` 5 处 `params[i]` → `params[i].name`。
  - **JSFunction / BytecodeFunction 扩展**：`JSFunction` 新增 `param_defs_`（`shared_ptr<vector<ParamDef>>`，getter/setter/ClearRefs 重置）；`BytecodeFunction` 新增 `param_defs`（shared_ptr）+ `length_count`（uint16_t，首个默认参数前的参数数量）。
  - **Parser 变更**：`parse_function_params` 返回 `ParseResult<vector<ParamDef>>`，`...rest = expr` → SyntaxError（DP-00）；`parse_arrow_body` 签名改为接受 `vector<ParamDef>`；`nud(LParen)` 中 `parse_expr(2)` → `parse_expr(1)` 允许 AssignmentExpression 节点作为箭头函数参数，通过检测 `AssignmentExpression{op: Assign}` 提取默认值（使用 `std::move(*ae.value)` 解决 ExprNode 不可复制问题）；`led(Arrow)` 单 ident 路径改为 `ParamDef{name, nullptr}`。
  - **Interpreter 变更**：`make_function_value`/`eval_arrow_function_expr`/`make_async_function_value` 计算 `length_count`、提取 param_names、存储 param_defs；`call_function` 有 `param_defs` 时在 fn_env 中求值默认值（env 切换：`current_env_`/`var_env_` 临时设为 fn_env，逐参数绑定，出错后恢复）。`null` 不触发默认值，仅 `undefined` 触发。
  - **VM Compiler 变更**：`compile_function` 提取 param_names、计算 `length_count`、存储 `param_defs`；为每个有 `default_init` 的参数在函数体前发射 `kGetVar / kLoadUndefined / kStrictEq / kJumpIfFalse(skip) / default_expr / kSetVar / kPop / label_skip` 序列。
  - **VM 变更**：`kMakeFunction` 用 `fn_bc->length_count` 设置 `.length` 属性，并将 `fn_bc->param_defs` 存入 JSFunction。
  - **function.length 截断规则**：`length_count` = 首个有默认值参数的索引（rest 参数不计入）。
  - **测试**：新建 `tests/unit/default_params_test.cpp`（DP-00～DP-26，27 个测试），含 Parser SyntaxError、Interpreter、VM 三路径对称覆盖；修复已有测试（`function_test.cpp`、`parser_module_test.cpp`）中 `params[i]` → `params[i].name`。
  - 3578/3578 通过（coverage），0 LSan 泄漏。

- [x] **展开运算符 Spread / Rest — Review 必修问题修复 M1/M2**（2026-05-25）：
  - **M1 Compiler 安全修复**：`compile_expr` 中 `SpreadElement` 分支从 no-op 改为 emit `kLoadString(SyntaxError: invalid use of spread element) + kThrow`。原 no-op 在 `var x = ...arr` 等非法位置时不 push 任何值，后续 `kPop`/赋值 会下溢栈，触发 heap-buffer-overflow；现在会在运行时抛 SyntaxError，栈始终保持一致。
  - **M2a 迭代器 self-iterable 修复（根本原因）**：vm.cpp `array_iterator_fn` 和 `string_iterator_fn` 创建 iter_obj 后，额外注册 `[Symbol.iterator]() { return this; }` 的 NativeFn（gc_heap_.Register，无捕获 lambda）。同步在 interpreter.cpp 数组迭代器 / 字符串迭代器工厂中对称修复。修复后 `[...[1,2][Symbol.iterator]()]` / `[...'ab'[Symbol.iterator]()]` 能走 spread 的 Symbol.iterator 通用路径正确展开。
  - **M2b 原生迭代器类型快路径**：`spread_into`（interpreter.cpp）和 `kSpreadAppend`（vm.cpp）的字符串快路径之后、Symbol.iterator 通用路径之前，新增 `kArrayIterator`/`kStringIterator`/`kForOfIterator` 三类原生迭代器快路径。`ArrayIterator` 直接遍历 `array_ref_` 从 `index_` 起的剩余元素；`StringIterator` 直接从 `byte_pos_` 起按 UTF-8 码点迭代；`ForOfIterator` 调用 `next_method_` 直到 `done_`（vm 侧复用现有 spread_error/continue 模式）。
  - **测试**：新增 5 个测试（`SpreadRestVM.SpreadInIllegalPositionThrows` M1 回归 / `SpreadRestInterp.SpreadArrayIteratorObject` + `SpreadRestVM.SpreadArrayIteratorObject` + `SpreadRestInterp.SpreadStringIteratorObject` + `SpreadRestVM.SpreadStringIteratorObject` M2 回归）。
  - 3551/3551 通过（coverage），0 LSan 泄漏。

- [x] **展开运算符 Spread / Rest 参数**（2026-05-25）：
  - **词法器**：新增 `TokenKind::DotDotDot`，词法器将 `...` 一次性扫描为单个 token（替代原来切分为 3 个 `Dot`）；`LexerTest.OperatorSpreadSplitsToDotDotDot` 同步更新。
  - **AST**：新增 `SpreadElement` 节点（`argument: unique_ptr<ExprNode>`，`range` 字段），加入 `ExprNode` variant；`FunctionDeclaration`/`FunctionExpression`/`ArrowFunctionExpression` 新增 `rest_param: optional<string>` 字段。
  - **Parser**：数组字面量元素遇 `DotDotDot` 解析为 SpreadElement；函数调用/new 调用参数列表遇 `DotDotDot` 解析为 SpreadElement；函数参数列表末尾遇 `DotDotDot` 记录为 rest_param；`expr_range` + `ast_dump` 新增 SpreadElement 分支。
  - **Interpreter**：新增 `spread_into(iterable, out)` 辅助函数（数组快路径、字符串码点快路径、Symbol.iterator 通用路径）；`eval_array_expr` 检测 SpreadElement 调用 spread_into；`eval_call_expr`/`eval_new_expr` 参数 loop 中 SpreadElement 调用 spread_into；`make_function_value`/`make_async_function_value`/`eval_arrow_function_expr` 传递 rest_param 到 JSFunction；`call_function` 参数绑定后追加 rest array 构造与绑定。
  - **VM/Compiler**：新增 4 条 opcode（`kSwap` 交换 TOS/TOS-1；`kArrayAppend` 弹出 val+arr_dup、追加到底层 arr；`kSpreadAppend` 弹出 iterable+arr_dup、展开追加；`kApplyArgs` 弹出 [func, this, args_array]、调用函数，operand 0=普通/1=new）；`compile_array_expr` 有 SpreadElement 时切换 kDup+kArrayAppend/kSpreadAppend append 模式；`compile_call_expr` 有展开时 method call 用 kSwap 调整 [obj,method]→[method,obj]，再 kNewArray+append+kApplyArgs(0)，普通调用类似；`compile_new_expr` 有展开时 kNewArray+append+kApplyArgs(1)；`hoist_vars_scan_expr` 新增 SpreadElement 递归遍历分支；`compile_expr` 新增 SpreadElement no-op 分支（不应单独出现）；`kMakeFunction` 复制 `fn_bc->rest_param` 到 JSFunction；`push_call_frame` 参数绑定后构造并绑定 rest array。
  - **测试**：新增 `tests/unit/spread_rest_test.cpp`（27 个测试：`SpreadRestInterp` 14 个 + `SpreadRestVM` 13 个，覆盖数组展开基本/空/多重/字符串、调用参数展开基本/混合/方法调用、rest 参数基本/空/仅 rest/箭头函数、spread+rest 组合、new 调用展开）。
  - 3516/3516 通过（coverage + ASAN），0 LSan 新增泄漏。

- [x] **`for...of` 循环**（2026-05-21）：`ForOfStatement` AST 节点（has_decl/var_kind/binding/right/body）；Parser `for (x of iterable)` 解析（var/let/const+ident+of 三路径 + 表达式+of 路径；与 `for...in` 共享前缀解析逻辑）；`ForOfIterator`/`ArrayIterator`/`StringIterator` 三个迭代器类（分别对应通用 Symbol.iterator 协议/数组内置快路径/字符串 Unicode 码点迭代；均继承 RcObject，TraceRefs/ClearRefs 实现；kForOfIterator/kArrayIterator/kStringIterator ObjectKind 枚举值）；`for_of_iterator.h` 新文件；opcode.h 新增 `kForOfStart`/`kForOfNext`/`kIteratorClose`/`kIteratorCloseAbnormal` 四条指令；Interpreter `eval_for_of_stmt`（数组走 ArrayIterator 快路径，字符串走 StringIterator UTF-8 码点路径，其他对象调用 Symbol.iterator 获取迭代器 + next 方法；let/const 每迭代创建独立 per-iteration scope；break/continue/return/throw 四路处理；异常路径调用 kIteratorClose 关闭迭代器）；Compiler `compile_for_of_stmt`（四标签布局：kForOfStart → Jump label_check；label_body（PushScope/kDefLet 按需）→ body → （PopScope）→ label_continue → Jump label_check；label_check：kForOfNext + kJumpIfFalse label_body；label_break：kIteratorClose）；VM kForOfStart/kForOfNext/kIteratorClose/kIteratorCloseAbnormal 处理；hoist_vars 添加 ForOfStatement 分支；labeled 语句中 ForOfStatement 分支支持；新增 `tests/unit/for_of_test.cpp`（43 个测试：FO-01～FO-15 Interp + FO-16～FO-30 VM，含 FO-01 Parser/Dump）。**LSan 修复 1**：`compiler.cpp compile_return_stmt` — 返回前追加 `kLoadUndefined; kSetVar return_temp_idx; kPop` 清零 `$__qppjs_return_temp__` 绑定，打断闭包→call_env→return_temp→闭包 引用环（修复 `EnvRcStress.VM_ManyClosuresShareEnv` 50 对象 LSan 泄漏）。**LSan 修复 2**：`js_object.cpp clear_function_properties` — 新增 `symbol_props_` 遍历，清理 symbol 属性中的函数引用（修复 `Symbol.iterator` 闭包 → global_env_ 引用环导致的 FO09/FO21 LSan 泄漏）。3489/3489 通过（coverage + ASAN），0 LSan 泄漏。

- [x] **`for...in` Review 必修问题修复 M1/M2/M3/M4**（2026-05-20）：
  - M1：`interpreter.cpp eval_for_in_stmt` + `vm.cpp kForInStart` — 在调用 `enumerate_properties()` 前添加 ObjectKind 守卫。仅 kOrdinary/kArray/kRegExp/kStringObject/kBooleanObject（JSObject 子类）合法；kFunction/kPromise/kEnvironment/kModule/kForInIterator 为 RcObject，static_cast<JSObject*> 是 UB，现在对这些类型直接返回空键集合。两处对称修复。
  - M2：`js_object.cpp enumerate_properties()` 慢路径 — 原实现只遍历 `properties_`，跳过了 `elements_`（整数索引）。数组通常都有 array_prototype_ 作为原型（proto ≠ nullptr），所以走慢路径，导致 for-in 完全跳过数组的数值索引。修复：在慢路径 while 循环中，对每个节点若为 kArray 先枚举 `elements_`（排序、uint32→string）加入 seen+result，再枚举 properties_。
  - M3：`compiler.cpp compile_for_in_stmt` — 重构字节码布局实现 per-iteration scope 语义。原实现：kPushScope 在循环外执行一次，所有迭代共享同一 Environment，闭包捕获到的是最后一次迭代的值。新布局：RHS + kForInStart 在外层求值，Jump 到 label_check；label_body_start（每次迭代入口）：kPushScope + kDefLet/kDefConst + kInitVar（per-iteration 新建 scope）；body 正常结束：kPopScope + Jump label_check；label_continue（continue 目标）：kPopScope（if lexical）+ Jump label_check；label_check：kForInNext + kJumpIfFalse label_body_start；label_break（break 目标）：kPop(iter)。
  - M4：`compiler.h LoopEnv` 新增 `for_in_has_scope` 字段；`compile_break_stmt`：匹配 lexical for-in 时先 emit kPopScope 再 emit break Jump；跨越 lexical for-in 时 emit kPopScope + kPop（原仅 kPop）。`compile_continue_stmt`：跨越 lexical for-in 时 emit kPopScope + kPop（匹配情况由 label_continue 处理，无需额外 emit）。
  - 3446/3446 通过（coverage + ASAN），0 LSan 泄漏。

- [x] **`for...in` Testing Agent 边界补测 + 3 处 Bug 修复**（2026-05-20）：
  - 新增 23 个测试（FI-31～FI-53，Interp+VM 对称，FI-39 仅 Interpreter）
  - 覆盖 8 大盲区：
    1. 原型链遮蔽（FI-31/32）：own non-enumerable 属性应遮蔽继承链同名 enumerable 属性
    2. 深层原型链（FI-33/34）：A→B→C 三层，各层键均可枚举，验证收集完整且无重复
    3. labeled break outer（FI-35/36）：跨嵌套 for-in 的 break outer 正确退出外层
    4. labeled continue outer（FI-37/38）：跨嵌套 for-in 的 continue outer 正确跳到下轮外层迭代
    5. let 每迭代独立绑定（FI-39，Interpreter only）：闭包捕获 let 变量，各迭代独立
    6. const 重赋值 TypeError（FI-40/41）：for (const k in obj) { k=1 } 抛 TypeError，try/catch 验证
    7. Symbol 键不枚举（FI-42/43）：Symbol 属性不出现在 for-in 结果中
    8. 原始值右侧（FI-44～FI-49）：42/"abc"/true 作为右侧操作数时零迭代
    9. defineProperty 非枚举键（FI-50/51）：enumerable:false 键不出现，enumerable:true 出现
    10. 深层链同名键去重（FI-52/53）：三层同名 'x' 只输出一次
  - 修复 Bug 1：`js_object.cpp enumerate_properties()` — 仅 `own_enumerable_string_keys()` 加入 `seen`，non-enum own 无法遮蔽继承 enum。修复：遍历 `properties_` 所有条目全加 `seen`，仅 enumerable 加 `result`
  - 修复 Bug 2：`interpreter.cpp eval_for_in_stmt` — let/const 所有迭代共用同一 env，闭包捕获同一 Cell。修复：每迭代创建独立 `iter_env`（per-iteration binding）
  - 修复 Bug 3：`compiler.cpp compile_break_stmt`/`compile_continue_stmt` — labeled 跳转跨 for-in 时未弹出内层迭代器，导致 VM 栈状态错误（FI38 挂死）。修复：`LoopEnv` 新增 `is_for_in` 字段（compiler.h），跨越 for-in 时 emit `kPop`
  - 3446/3446 通过（coverage），0 LSan 泄漏

- [x] **三元运算符 Testing Agent 边界补测**（2026-05-20）：
  - 追加 40 个测试（CE-26～CE-45 Interp + CE-46～CE-65 VM）
  - 覆盖 6 大盲区：
    1. 异常传播：未声明变量作为条件抛 ReferenceError；条件中 getter 抛异常向上传播；被选中分支 getter 抛异常传播（CE-26/27/30 × Interp+VM）
    2. 短路副作用验证：true 条件下 else getter 不调用；false 条件下 then getter 不调用（CE-28/29 × 2）
    3. 嵌套三元：(nested) ? d : e 作为外层条件；三层右结合深嵌套（CE-31/32 × 2）
    4. 函数调用副作用：条件函数恰好调用一次；仅被选分支函数执行（CE-33/34 × 2）
    5. 逻辑运算符混合：&& / || 作为条件；&& / || 在分支中（结果是操作数值而非 ToBoolean 值）（CE-35~38 × 2）
    6. 结果用途与控制流：函数参数 Math.abs/Math.max；对象属性赋值；数组元素；while 条件；return 链式三元；for 条件；NaN 为 falsy（CE-39~45 × 2）
  - 3388/3388 通过（coverage）

- [x] **三元运算符 `?:`（ConditionalExpression）**（2026-05-20）：
  - AST：`ConditionalExpression` struct（condition/consequent/alternate/range）加入 `ExprNode` variant（ast.h）
  - Parser：`lbp(Question)=4`；`led(Question)` 使用 `parse_expr(1)` 解析 then/else（允许 AssignmentExpression，lbp=2>1）；右结合性由 parse_expr(1) 递归自然产生；`expr_range` 补全 ConditionalExpression arm（parser.cpp）
  - AST Dump：`ast_dump.cpp` 添加 ConditionalExpression 输出（condition/then/else 子树）
  - Interpreter：`interpreter.h` 声明 + `interpreter.cpp` 实现 `eval_conditional_expr`（短路求值，错误透传）；`eval_expr` visitor 添加 ConditionalExpression lambda
  - Compiler：`compiler.h` 声明 + `compiler.cpp` 实现 `compile_conditional_expr`（两标签三段式：kJumpIfFalse → consequent → kJump → alternate）；`compile_expr` visitor 添加 ConditionalExpression lambda
  - 顺带修复 P2：`to_boolean` 字符串分支 `!v.as_string().empty()` → `!v.sv().empty()`（interpreter.cpp + vm.cpp 各 1 处，避免堆分配）
  - 测试：`conditional_expression_test.cpp`（CE-01～CE-05 Parser + CE-06～CE-15 Interp + CE-16～CE-25 VM = 25 个测试）
  - 3348/3348 通过（coverage），3346/3346（run_ut ASAN），0 LSan 泄漏

- [x] **String/Boolean 构造函数测试修复**（2026-05-19）：
  - `string_prototype_` 新增 `valueOf` 方法：this 为 string primitive 直接返回，kStringObject 返回 wrapped_value_，其他 TypeError（interpreter.cpp + vm.cpp）
  - `string_prototype_` 新增 `toString` 方法：逻辑同 valueOf（interpreter.cpp + vm.cpp）
  - `String.fromCharCode` ToUint16 修正：`static_cast<uint32_t>(negative_double)` 存在 UB，改为 `fmod(trunc_n, 65536.0) + 负数偏移` 正确处理负数参数（-1 → 65535 = U+FFFF）（interpreter.cpp + vm.cpp）
  - 修复 6 个失败测试：SB-24（new String("x").valueOf() === "x"），SB-25（new String("x").toString() === "x"），SB-26（String.fromCharCode(-1) === String.fromCharCode(65535)）× Interp+VM
  - 3323/3323 通过（coverage），3321/3321（run_ut ASAN），0 LSan 泄漏

- [x] **String 和 Boolean 全局构造函数**（2026-05-19）：
  - `ObjectKind` 追加 `kStringObject`/`kBooleanObject` 枚举值（rc_object.h）
  - `JSObject` 新增 `wrapped_value_` 字段（js_object.h）；TraceRefs 追加 wrapped_value_ 对象引用追踪；ClearRefs 清零 wrapped_value_（js_object.cpp）
  - `string_prototype_` 改造为 `kStringObject` 类型，wrapped="" （interpreter.cpp + vm.cpp）
  - `boolean_prototype_` 新建（kBooleanObject，wrapped=false，proto=object_prototype_），注册 valueOf/toString 方法；interpreter.h + vm.h 新增成员声明
  - Boolean 构造函数 is_new_call 两路径：false 路径返回 bool primitive，true 路径创建 kBooleanObject 对象（linked to boolean_prototype_）
  - String 构造函数 is_new_call 两路径：false 路径处理 Symbol 特殊转换，true 路径创建 kStringObject 对象；new String(Symbol) → TypeError
  - `String.fromCharCode`：ToUint16 截断（`uint32_t(trunc(n))` → `uint16_t`），UTF-8 编码
  - `eval_member_expr`/`kGetProp`/`kGetElem`：kStringObject length 特判（wrapped_value_.js_string_raw()），其他属性走 get_property（proto chain）；kBooleanObject 直接 get_property
  - `eval_call_expr`：kStringObject/kBooleanObject 分支（static_cast JSObject*，调用 get_property）
  - `instanceof`：kStringObject/kBooleanObject 加入 proto chain 遍历允许列表（interpreter.cpp + vm.cpp）
  - `ThisStringValue` 更新：8 个 string_prototype_ 方法（indexOf/lastIndexOf/slice/substring/split/trim/trimStart/trimEnd/match）改用 `string_this_value`/`string_this_value_vm` helper，对 kStringObject 提取 wrapped_value_
  - GC roots：4 处 GC roots sections 均加入 `boolean_prototype_`；4 处 cleanup sections 均加入 `boolean_prototype_->clear_function_properties()`
  - 移除 vm.cpp 中已过时的 `Patch string_constructor_ to handle Symbol` 代码块（功能已集成到 string_constructor_ 主体）
  - boolean_constructor_ + string_constructor_ 设置 prototype_obj 和 "prototype" 属性（支持 instanceof）
  - 新增 `tests/unit/string_boolean_constructor_test.cpp`（SB-01～SB-22 × Interp+VM = 44 个测试）
  - 3279/3279 通过（coverage），3277/3277（run_ut ASAN），0 LSan 泄漏

- [x] **箭头函数 Review 修复 P0-1/P2-1**（2026-05-19）：
  - **P0-1**：重构 `nud(LParen)` 逗号分支（`parser.cpp`）。原实现在遇到 `(Ident,` 后立即校验且只接受 Identifier，导致 `(a, 123)` 等合法逗号表达式报 SyntaxError。修复为先收集所有逗号分隔表达式（`parse_expr(2)` 级，不提前校验），消费 `)` 后再判断：若跟 `=>` 则验证每项是否为 Identifier → 箭头函数路径；否则返回最后一项（逗号表达式语义，无 `BinaryOp::Comma` 故返回 last item）。
  - **P2-1**：`is_expr_end_token` 新增 `TokenKind::RParen` → 返回 true，修复 `(a + b) / 2` 中 `)` 后的 `/` 被误识别为正则字面量开头。
  - 3235/3235 通过（coverage），0 LSan 泄漏。

- [x] **箭头函数 Testing Agent 边界补测**（2026-05-19）：
  - 追加 24 个测试（AF-21～AF-32 × Interp+VM）至 `tests/unit/arrow_function_test.cpp`（总计 63 个）。
  - **AF-21**：表达式体直接返回 `this`（方法内 `() => this`，`f().v === obj.v`）。
  - **AF-22**：三层箭头嵌套 `() => () => () => this.x`，this 仍指向最外层普通函数的 this。
  - **AF-23**：立即调用箭头函数 IIFE（`(() => 42)()`）。
  - **AF-24**：箭头函数赋值为另一对象方法后 this 不变（`obj2.fn = arrow; obj2.fn()` 仍返回 obj1 的属性值）。
  - **AF-25**：参数与外层同名变量遮蔽（`var x=1; var f = x => x*2; f(3) === 6`）。
  - **AF-26**：链式调用 `filter + map`（`[1,2,3].filter(x=>x>1).map(x=>x*2)`）。
  - **AF-27**：返回箭头函数 partial application（`var make = n => x => x + n; make(5)(3) === 8`）。
  - **AF-28**：条件分支块体（if-else 验证绝对值语义；三元运算符 `?:` 尚未实现，注释说明）。
  - **AF-29**：无参空块体返回 undefined（`() => {}` 独立验证）。
  - **AF-30**：复合函数类型检查（`typeof f === "function" && f.prototype === undefined`；`Function` 全局尚未注册，注释说明）。
  - **AF-31**：多参数回调 `(x, i) => x + i` 验证 index 参数传递（`['a','b'].map((x,i)=>x+i)[0] === "a0"`）。
  - **AF-32**：map 结果数组两端完整性（`[1,2].map(x=>x+10)` 验证 r[0]/r[1]）。
  - **发现未实现功能**：(1) 三元运算符 `?:` 未实现（`Question` token 已词法化但 Parser 无 led handler）；(2) `Function` 全局构造函数未注册。两项均记录在对应测试注释中，不修改设计结论。
  - 3235/3235 通过（coverage），0 LSan 泄漏。

- [x] **箭头函数 `()=>`**（2026-05-19）：
  - **AST**：新增 `ArrowFunctionExpression{params, body_stmts, range}`（表达式体已在 Parser 合成为含 ReturnStatement 的块体，无 expr_body 字段）；加入 `ExprNode` variant。
  - **BytecodeFunction**：新增 `bool is_arrow = false` 字段。
  - **JSFunction**：新增 `is_arrow_`/`lexical_this_` 私有字段 + 公开 accessor；`TraceRefs` 追踪 `lexical_this_`（对照 `bound_this_`）；`ClearRefs` 清零 `lexical_this_`。
  - **Parser**：`lbp(Arrow)=3`（高于赋值 lbp=2，使箭头函数可在函数参数/数组/赋值 RHS 等所有赋值级上下文中消费）；`nud(LParen)` 重写（三路径：`()=>`/`(a,b)=>`/`(a)=>`，其余为普通括号表达式）；`led(Arrow)` 新增（left 必须是 Identifier，检查 got_lf）；`parse_arrow_body` 新增（块体复用 parse_function_body；表达式体 parse_expr(2) 后包装 ReturnStatement）；in_async_function_/in_module_ 词法透传（不重置，箭头函数继承外层 async/TLA 上下文）；`expr_range` 添加 ArrowFunctionExpression 分支。
  - **ast_dump**：添加 ArrowFunctionExpression 输出（params + body 语句列表）。
  - **Interpreter**：新增 `eval_arrow_function_expr`（set_arrow/set_lexical_this(current_this_)/不创建 prototype_obj/GC 注册）；`eval_expr` dispatch 添加分支；`call_function` 三处守卫（1. is_arrow+is_new_call → TypeError；2. !is_arrow 才建 arguments；3. actual_this = is_arrow ? lexical_this : this_val）；`eval_new_expr` 前置箭头检查（early return TypeError）。同时修复函数隐式返回语义：将 `Completion::normal(result_val)` 改为 `Completion::normal(Value::undefined())`（JS 规范：无显式 return 的函数返回 undefined）。
  - **Compiler**：新增 `compile_arrow_function_expr`（compile_function + is_arrow=true）；`compile_expr` 添加分支；`compile_function` 新增 `is_program=false` 参数——程序体保持"留最后表达式值"的 REPL 语义，函数体改为 compile_stmt（所有语句含最后一条）+ kReturnUndefined（修复 VM 侧函数隐式返回语义）；`compile()` 传 is_program=true。
  - **VM**：`kMakeFunction` 添加 is_arrow 分支（set_arrow/set_lexical_this(frame.this_val)/不创建 proto_obj/break）；`push_call_frame` 三处守卫（1. is_arrow+is_new → return TypeError；2. !is_arrow 才建 arguments；3. actual_this = is_arrow ? lexical_this : this_val）；`kNewCall` 添加箭头检查（frame.pending_throw）。
  - 新增 `tests/unit/arrow_function_test.cpp`（AF-01～AF-20 × Interp+VM + 1 Parser SyntaxError = 39 个测试）；`tests/CMakeLists.txt` 注册。
  - 3211/3211 通过（coverage），0 LSan 泄漏。

- [x] **Object.defineProperty Review 修复 P0-1/P0-2**（2026-05-19）：
  - **P0-1**（`interpreter.cpp`，`eval_member_expr`）：accessor 原型链遍历循环中，原代码仅在 `entry != nullptr && (entry->flags & kPropIsAccessor)` 时才进入 accessor 分支，对 data property 不作任何处理、继续向上遍历，可能错误找到更高层原型的 accessor。修复：将条件改为先判断 `entry != nullptr`，内层再判断 `kPropIsAccessor`——是 accessor 执行 getter 调用，是 data property 执行 `break` 停止遍历。与 `vm.cpp kGetProp` 路径（`if (entry != nullptr) break;`）保持对称。
  - **P0-2**（`js_object.cpp`，`define_property`）：accessor→data 切换时，清除 `kPropIsAccessor` 标志后遗留 `existing->getter` / `existing->setter` 废弃函数引用，影响 GC 可达性。修复：在 `existing->flags &= ~kPropIsAccessor` 之后同时执行 `existing->getter = Value::undefined(); existing->setter = Value::undefined();`。
  - 3170/3170 通过（coverage），0 LSan 泄漏。

- [x] **Object.defineProperty Testing Agent 边界补测 + 1 处 Bug 修复**（2026-05-19）：
  - **追加测试**：24 个（DP-23～DP-34 × Interp+VM），覆盖 accessor 原型链继承（this 为 receiver 子对象）、getter 每次读取重新调用、setter 副作用（修改对象其他属性）、getter 中 this 为 receiver（不是 proto）、configurable:true accessor 替换 getter（DP-27）、configurable:true accessor 切换为 data descriptor（getOwnPropertyDescriptor.get 返回 undefined，DP-28）、non-configurable accessor 不能替换 get/set（TypeError，DP-29）、getOwnPropertyDescriptor accessor 格式（value/writable 为 undefined，DP-30）、Object.keys 多属性 enumerable 混合过滤（DP-31）、preventExtensions 不影响已有属性修改（DP-32）、Object.defineProperty 返回值 === O（可链式，DP-33）、SameValue 幂等重定义（整数/字符串/NaN 同值不抛，不同值仍抛，DP-34）。
  - **Bug 修复**：`js_object.cpp define_property` 在 `!configurable + is_accessor` 分支下缺少 getter/setter SameValue 检查 — 添加 `if (desc.getter.has_value() && !same_value(desc.getter.value(), existing->getter)) → TypeError`（setter 同理）。修复前 non-configurable accessor 可被任意替换 getter/setter 而不抛错。
  - 3172/3172 通过（coverage），0 LSan 泄漏。

- [x] **Object.defineProperty 及 Property Descriptor 系统**（2026-05-19）：
  - **核心数据结构**：`PropertyEntry` 扩展（新增 `flags: uint8_t`、`getter: Value`、`setter: Value` 字段，flags 位 kPropWritable=0x01/kPropEnumerable=0x02/kPropConfigurable=0x04/kPropIsAccessor=0x08/kPropDefault=0x07）；`JSObject::extensible_` 字段（默认 true）；`PropDesc` 结构体（栈上使用，全部 optional 字段）。
  - **JSObject 新接口**：`define_property(key, PropDesc)` — ValidateAndApplyPropertyDescriptor 规范实现（SameValue NaN/+0/-0，non-configurable 保护，data↔accessor 切换限制）；`get_own_entry(key)` — 仅查自身不含原型链；`define_builtin_property(key, value)` — flags=0x00（non-enum/non-writable/non-configurable，供引擎内部初始化）。
  - **现有接口扩展**：`set_property_ex` 新增 writable/extensible sloppy 检查（non-writable 静默忽略，non-extensible 静默忽略——sloppy mode 语义）；`delete_property` 新增 configurable 检查（configurable:false → 返回 false）；`own_enumerable_string_keys` 过滤 enumerable:false 属性；`TraceRefs` 扩展（`getter.is_object()` / `setter.is_object()` mark）；`clear_function_properties` 扩展（清理 property.getter / property.setter 引用防 GC 泄漏）。
  - **Object 静态方法（Interpreter + VM 对称）**：`Object.defineProperty`（ToPropertyDescriptor 验证：get/set 需为 callable；data+accessor 混用 TypeError；`define_property` 错误包装为 TypeError）；`Object.getOwnPropertyDescriptor`（返回含 value/writable 或 get/set 的 descriptor 对象）；`Object.preventExtensions`（设 extensible_=false）。
  - **Accessor 调用路径**：`eval_member_expr` 原型链遍历找 kPropIsAccessor 后调用 getter（`call_function_val`，空参数，receiver=obj_val）；`eval_member_assign` 原型链遍历找 setter 后调用（sloppy 无 setter 静默忽略）；VM `kGetProp` 分离 getter 值拷贝再调用（`call_stack_.back()` re-fetch 防止 realloc 失效引用）；VM `kSetProp` 同理（setter_to_call 值拷贝 + re-fetch frame）。
  - 新增 `tests/unit/define_property_test.cpp`（DP-01～DP-22 × Interp+VM，共 44 个测试）；`tests/CMakeLists.txt` 注册。
  - 3148/3148 通过（coverage），0 LSan 泄漏。

- [x] **delete 运算符 Testing Agent 边界补测 + 2 处 Bug 修复**（2026-05-19）：
  - 追加 48 个测试（DEL-16～DEL-28，Interp+VM 对称）。
  - 新增覆盖：delete 后重新添加属性 Object.keys 正确性；多属性依次删除后枚举顺序；delete 普通对象数字属性；delete 后通过 Object.keys 验证属性消失；delete 嵌套对象内属性（父/兄弟仍完好）；delete 表达式作为条件；连续 delete 同一属性幂等性；delete 数组越界索引（true，length 不变）；delete 后 typeof；delete 全部属性后 Object.keys 为空；delete 返回值赋值；delete 数组元素后 forEach 跳过 hole；delete computed string key 变量。
  - **Bug 修复 1**：`own_enumerable_string_keys`（`js_object.cpp`）遍历 `properties_` 时添加 `it->second == i` slot index 一致性检查，修复 delete 后重新赋值导致 `properties_` 留有旧条目、Object.keys 返回重复项的 Bug。
  - **Bug 修复 2**：`forEach`（`interpreter.cpp` + `vm.cpp` 两侧）对 hole 添加 `continue` 跳过，修复 forEach 不遵循稀疏数组语义的 Bug（规范要求 forEach 跳过 hole）。
  - 3032/3032 通过（coverage），0 LSan 泄漏。

- [x] **RegExp 运行时 Review 修复 P2-1/P2-2/P2-3**（2026-05-18）：
  - **P2-1+P2-3**：`match_not_bol` 语义修复。原代码 `if (rx->multiline_) mflags |= match_not_bol` 语义完全反转——multiline 模式下本应允许 `^` 匹配每行开头，却错误地阻止了它。修复为 `if (!rx->multiline_ && start_pos > 0) mflags |= match_not_bol`，即只在非 multiline 且搜索起点不是字符串开头时才传入该 flag（防止 `^` 错误匹配子串起点）。
  - **P2-2**：exec 空匹配时 lastIndex 职责分离。原 `regexp_exec`/`vm_regexp_exec` 在空匹配时直接 `last_index_ = match_start + 1`（超出规范的额外 +1）。修复为只设 `last_index_ = match_end`；全局 match 循环中检测 `match0.sv().empty()` 后 `rx->last_index_++` 防死循环，与规范 AdvanceStringIndex 语义对齐。
  - **改动文件**：`src/runtime/interpreter.cpp`（2 处）、`src/vm/vm.cpp`（2 处），共 4 处最小改动，Interp+VM 两侧对称。
  - 2944/2944 通过（coverage），0 LSan 泄漏。

- [x] **RegExp 运行时**（2026-05-18）：完整实现 RegExp 运行时。
  - **新增文件**：`include/qppjs/runtime/js_regexp.h`（JSRegExp 类，继承 JSObject，ObjectKind::kRegExp），`src/runtime/js_regexp.cpp`（构造函数：解析 flags、dotAll pattern rewrite、std::regex 编译、is_valid_ 标志）。
  - **rc_object.h**：ObjectKind 枚举末尾追加 kRegExp。
  - **native_errors.h**：NativeErrorType 追加 kSyntaxError（index 4），kCount 自动更新。
  - **interpreter.h**：添加 js_regexp.h include；新增 regexp_prototype_、regexp_constructor_ 字段；声明 eval_regex_literal/make_regexp/regexp_exec。
  - **interpreter.cpp**：添加 `<regex>` include；init_runtime 末尾新增 RegExp prototype（exec/test/toString）+ constructor（RegExp(rx)/new RegExp()）+ String.prototype.match；eval_expr RegexLiteral 分支替换 stub 为 eval_regex_literal；新增 make_regexp（flags 验证+std::regex 编译）、eval_regex_literal、regexp_exec（smatch 处理、lastIndex 更新、空匹配防死循环）；eval_member_expr 新增 kRegExp 分支（source/flags/global/ignoreCase/multiline/dotAll/sticky/unicode/lastIndex getter）；eval_member_assign 新增 kRegExp 分支（lastIndex 写入）；eval_call_expr 新增 kRegExp 分支（regexp_prototype_ 查找）；instanceof 的 prototype chain walk 扩展 kRegExp；两处 GC roots 添加 regexp_prototype_/regexp_constructor_；两处 cleanup 添加 clear_function_properties/clear_own_properties；kSubErrors 追加 kSyntaxError。
  - **vm.h**：添加 js_regexp.h include；新增 regexp_prototype_/regexp_constructor_ 字段；声明 vm_make_regexp/vm_regexp_exec。
  - **vm.cpp**：添加 `<regex>` include；init_global_env 替换 RegExp stub，新增完整 regexp_prototype_（exec/test/toString）+ constructor + String.prototype.match；vm_make_regexp/vm_regexp_exec 实现；kGetProp 新增 kRegExp 分支；kSetProp 新增 kRegExp lastIndex 写入；kInstanceof prototype chain walk 扩展 kRegExp；kNewRegExp 指令（读 pattern_idx/flags_idx，调用 vm_make_regexp）；两处 GC roots + cleanup 同步更新；kSubErrors 追加 kSyntaxError。
  - **opcode.h**：追加 kNewRegExp（4 字节操作数）。
  - **compiler.cpp**：RegexLiteral 分支替换 stub 为 emit kNewRegExp + emit_u16(pattern_idx) + emit_u16(flags_idx)。
  - **gc_heap.h/cpp**：新增 GcHeap 析构函数，清零残留对象的 gc_heap_ 指针，防止 Interpreter 析构时 UAF。
  - **lsan_suppressions.txt**：新增 3 条 macOS 误报（dyld4::APIs::setErrorString/std::__1::__refstring_imp/dyld::ThreadLocalVariables::instantiateVariable）。
  - **tests/CMakeLists.txt**：添加 regexp_test.cpp。
  - **tests/unit/regexp_test.cpp**：52 个测试（RX-01～RX-24 × Interp+VM + DotAll × 2 + ConstructorStringPattern × 2）。
  - **tests/unit/regex_literal_test.cpp**：4 个 stub 测试（ThrowsUnsupportedError × 2 + StmtThrowsUnsupportedError × 2）改为验证正确行为；2 个集成测试（RegexAsFunctionArg/RegexAsCallbackArg）改为验证成功路径。
  - 2916/2916 通过（coverage），0 LSan 泄漏。

- [x] **正则表达式字面量的 Parser 集成**（2026-05-15）：按设计方案实现正则表达式字面量从 Lexer 到 Parser/Interpreter/VM 的完整链路。
  - **AST**：新增 `RegexLiteral` 结构体（`pattern`/`flags`/`range`），扩展 `ExprNode` variant。
  - **Parser**：`advance()` 添加 `is_expr_end_token()` 辅助函数（检查当前 token 是否表达式终结，决定下一个 `/` 是正则还是除法）；`nud` 添加 `TokenKind::Regex` 分支，从 token 文本中提取 pattern 和 flags；`expr_range()` 添加 `RegexLiteral` 分支。
  - **ast_dump**：`dump_expr` 添加 `RegexLiteral` 分支，输出 pattern 和 flags（无 flag 时显示 "(none)"）。
  - **Interpreter stub**：`eval_expr` 添加 `RegexLiteral` 分支，返回 `Value::undefined()`。
  - **Compiler stub**：`compile_expr` 添加 `RegexLiteral` 分支，emit `kLoadUndefined`。
  - **Lexer bug 修复**：`lexer.cpp` 中 `next_token` 的 `case '/'` scan_regex 分支未在调用 `scan_regex()` 前跳过起始 `/`，导致正则主体被误当作空 pattern。添加 `++state.pos` 跳过起始 `/`。
  - **测试**：新增 `tests/unit/regex_literal_test.cpp`，24 个测试（14 Parser + 2 Dump + 4 Interpreter + 4 VM）。
    - Parser：BasicNoFlags/WithFlags/CharacterClass/EscapeSequence/AllFlags/VarDeclWithRegex/VarDeclWithRegexFollowedByIdent/DivisionChain/AssignmentRegex/ArrayRegex/ParenRegex/TypeofRegex/VoidRegex/BangRegex/ReturnRegex/RegexFollowedByDivision/TwoRegexLiterals（17 个）
    - Dump：BasicDump/NoFlagsDump（2 个）
    - Interpreter：ReturnsUndefined/StmtReturnsUndefined（2 个）
    - VM：ReturnsUndefined/StmtReturnsUndefined（2 个）
  - 改动文件：`include/qppjs/frontend/ast.h`、`src/frontend/parser.cpp`、`src/frontend/lexer.cpp`、`src/frontend/ast_dump.cpp`、`src/runtime/interpreter.cpp`、`src/vm/compiler.cpp`、`tests/unit/regex_literal_test.cpp`、`tests/CMakeLists.txt`。
  - 2731/2731 通过（coverage），2729/2729 通过（run_ut ASAN），0 LSan 泄漏。

- [x] **test262 git submodule 集成**（2026-05-15）：将 test262 测试套件从外部路径改为 git submodule 管理。`tests/test262/test262` 指向 `https://github.com/tc39/test262.git`（最新 main，commit 82d7772）。`scripts/run_test262.py` 改为默认使用 submodule 路径（`test262_dir` 参数变为可选），同时增加 `build/test/src/qppjs` 二进制候选取代。修正 `.gitignore` 新增 `*.profraw`。使用者只需 `git submodule update --init` 即可获取完整 test262 套件，无需手动 clone。

- [x] **macOS import.meta 测试路径修复**（2026-05-15）：修复 macOS 上 `/var` → `/private/var` 符号链接导致的 42 个 import.meta 测试失败。`module_test.cpp` 的 `TempDir::abs()` 和 `tla_test.cpp` 的 `TlaTempDir::abs()` 改用 `fs::weakly_canonical(dir_ / filename)` 以与引擎内部 `exec_module` 的路径规范化保持一致。2708/2708 通过。

- [x] **Array.prototype.sort/splice/slice 边界补测**（2026-05-14）：在规范对齐修复基础上补充 20 个边界测试（10 组 Interp+VM 对称，A-296～A-305）。覆盖：slice Infinity 参数（+Infinity 等同 length，-Infinity 等同 0）、slice 负数 end（clamp 到 0）、slice undefined 参数（等同缺失使用默认值）、splice Infinity deleteCount（删除到末尾）、splice undefined deleteCount（等同 0 不删除）、splice 负 start 删除（从末尾倒数位置删除）、sort 单元素数组不调用 comparefn（comparefn 抛异常验证不被调用）、sort comparefn 返回 +0/-0 视为相等（pos tie-breaker 保持稳定）、sort comparefn 返回非数字（boolean 通过 ToNumber 转换，true→1 降序）、sort 稀疏数组 + comparefn（holes 排在 undefined 之后）。2658/2658 通过（coverage），2658/2658 通过（run_ut ASAN），0 LSan 泄漏。

- [x] **Array.prototype.sort/splice/slice 规范对齐修复**（2026-05-14）：对三个 Array.prototype 方法进行规范对齐修复。
  - **sort**：分离 undefined 元素和 holes。收集阶段遍历 [0, array_length_)，对每个索引 find()：不存在（hole）→ hole_count++，is_undefined() → undef_count++，否则收集到 slots（含 val、pos、str_cache）。默认排序时收集阶段一次性对所有 defined 元素 ToString 存入 str_cache，避免比较阶段重复转换。排序使用 std::stable_sort，comparefn 返回 NaN 视为相等（cmp=0），cmp!=0 时用 cmp<0 判断，cmp==0 时用 pos tie-breaker 保持稳定。回写阶段：clear elements_，先写入 defined 元素，再写入 undefined 元素，holes 不写入，保持 array_length_ 不变。
  - **splice**：在 newLen 计算后新增溢出检查，若 `new_len > 9007199254740991LL`（2^53-1）则抛 TypeError。
  - **slice**：现有实现已正确处理 hole 语义（hole 不写入 elements_，保留稀疏语义），无需修改。
  - 改动文件：`src/runtime/interpreter.cpp`（sort 重写 + splice 溢出检查）、`src/vm/vm.cpp`（sort 重写 + splice 溢出检查）、`tests/unit/array_test.cpp`（新增 70 个测试）。
  - 新增 70 个测试（35 组 Interp+VM 对称，A-261～A-295）：slice hole 保留/结果独立/-0/NaN/非整数参数/负数超长/非数组 TypeError（A-261～A-270）；splice 无参/仅 start/hole 保留/插入多于删除/删除多于插入/负数超长/deleteCount 负数/NaN/非数组 TypeError/溢出检查/插入元素正确/删除元素正确（A-271～A-282）；sort undefined 排末尾/多 undefined 稳定/holes 排 undefined 后/全 undefined/全 hole/comparefn NaN 视为相等/comparefn 抛异常/非函数 TypeError/默认字符串排序/返回原引用/非数组 TypeError/混合 undefined+holes+defined/空数组（A-283～A-295）。
  - 2638/2638 通过（coverage），2638/2638 通过（run_ut ASAN），0 LSan 泄漏。

- [x] **import.meta 词法绑定修复**（2026-05-13）：修复 import.meta 的词法绑定语义——当模块 A 中定义的函数被模块 B 调用时，函数体内的 import.meta 应返回定义模块 A 的元数据，而非调用者模块 B。改动：JSFunction 新增 `defining_module_` 字段（ModuleRecord*，默认 nullptr）+ getter/setter；Interpreter `make_function_value`/`make_async_function_value` 中捕获 `current_module_` 设置到 `defining_module_`；Interpreter 新增 `current_function_` 成员，`call_function` 中设置/恢复；`eval_expr` MetaProperty 分支优先使用 `current_function_->defining_module()`，nullptr 时回退到 `current_module_`；VM `kMakeFunction` 中捕获 `frame.current_module` 设置到 `defining_module_`；VM `push_call_frame` 中将 `fn->defining_module()` 赋值给新帧 `current_module`；VM `kMetaProperty` 直接使用 `frame.current_module`，移除调用栈搜索逻辑。更新 IM-14 测试断言为精确匹配 m.js 路径。2568/2568 通过（coverage），2566/2566 通过（run_ut ASAN），0 LSan 泄漏。

- [x] **import.meta 边界测试补充**（2026-05-13）：在 `tests/unit/module_test.cpp` 新增 56 个测试（28 Interp + 28 VM，IM-13～IM-40）。覆盖范围：
  - **深层嵌套**：IM-13 深层嵌套函数中 import.meta（验证 VM 向上搜索调用栈找模块帧）
  - **跨模块闭包**：IM-14 闭包捕获 import.meta 跨模块调用（验证函数可正常调用且返回有效 url）
  - **参数传递**：IM-15 import.meta 作为函数参数传递
  - **控制流**：IM-16 条件分支中使用、IM-17 循环中多次访问验证缓存一致性、IM-18 try/catch 块中使用、IM-38 if-else 条件分支
  - **属性可变性**：IM-19 url 属性可覆盖
  - **模块拓扑**：IM-20 re-export 链中 import.meta 指向自身、IM-21 循环依赖模块顶层 import.meta、IM-22 副作用导入模块中 import.meta
  - **动态 import 组合**：IM-23 import.meta 与动态 import() 组合
  - **类型验证**：IM-24 url 是字符串类型、IM-30 url 非空字符串
  - **表达式上下文**：IM-25 二元表达式、IM-26 return 语句、IM-35 对象属性值、IM-36 数组元素、IM-37 逻辑表达式短路求值
  - **async/TLA**：IM-27 async 函数中使用、IM-28 TLA 挂起后 import.meta 仍可用、IM-29 TLA 挂起前后函数内 import.meta 一致性
  - **原型链验证**：IM-31 Object.keys 仅含 "url"、IM-32 instanceof Object 为 false、IM-33 不继承 toString、IM-34 不继承 valueOf
  - **GC 安全**：IM-39 GC 后 import.meta 仍可用（验证 meta_obj 被正确追踪为 GC root）
  - **对象同一性**：IM-40 多变量指向同一对象
  - 测试过程中发现并记录了以下边界行为：(1) Interpreter 侧 `current_module_` 指向当前正在执行的模块，从 entry.js 调用 m.js 导出函数时，函数体内 import.meta 返回 entry.js 的 meta（非词法绑定）；(2) 三元运算符 `?:` 未实现；(3) `Object.getPrototypeOf` 未实现；(4) 模块顶层 `var` 声明的 for 循环变量在 Interpreter 侧循环体外不可见。2568/2568 通过（coverage），0 LSan 泄漏。

- [x] **import.meta 元属性支持**（2026-05-13）：为 ESM 模块系统实现 `import.meta` 元属性。涉及 8 个模块改动：
  - **AST**（`ast.h`）：新增 `MetaProperty{SourceRange range}` 表达式节点，加入 `ExprNode` variant
  - **Opcode**（`opcode.h`）：新增 `kMetaProperty` 字节码（0 操作数），在 `ImportCall` 之后
  - **Parser**（`parser.cpp`）：nud Ident 分支在 `import(` 动态调用检测之后、普通标识符返回之前，检测 `import.meta`（tok_text=="import" && cur.kind==Dot → advance → 检查 Ident "meta" → 检查 `in_module_context_` → advance → 返回 MetaProperty）；新增 `in_module_context_` 标志（模块解析时设 true，函数体内不重置），与 `in_module_`（TLA 用）分离；`parse_stmt()` 入口 peek 下一个 token 区分 `import.meta`/`import[` 表达式与 import 声明；`expr_range` 添加 MetaProperty 分支
  - **ModuleRecord**（`module_record.h/cpp`）：新增 `RcPtr<JSObject> meta_obj` 字段；`TraceRefs` 添加 `meta_obj` 追踪；`ClearRefs` 添加 `meta_obj` 清理
  - **Interpreter**（`interpreter.cpp`）：`eval_expr` 新增 MetaProperty 分支，通过 `current_module_->meta_obj` 返回缓存对象；`link_module` 在模块环境创建后创建 `meta_obj`（`RcPtr<JSObject>::make()`，proto=nullptr，`set_property("url", mod.specifier)`，`gc_heap_.Register`）
  - **VM Compiler**（`compiler.cpp`）：`compile_expr` 新增 MetaProperty 分支，emit `kMetaProperty`
  - **VM**（`vm.cpp`）：run loop 新增 `kMetaProperty` case，向上搜索调用栈找模块帧的 `meta_obj`；`link_module` 对称创建 `meta_obj`
  - **ast_dump**（`ast_dump.cpp`）：新增 MetaProperty 输出 `"MetaProperty\n"`
  - **测试**：新增 24 个测试（IM-01～IM-12 × Interp+VM 对称），覆盖：url 返回绝对路径、[[Prototype]] 为 null（不继承 Object.prototype）、属性可写、不同模块独立对象、Script 中 SyntaxError、`import["meta"]` 不等同于 import.meta、typeof 为 "object"、函数体内使用、两个模块各自 url、Object.keys 包含 "url"、赋值给变量、多次访问返回同一对象。2512/2512 通过（coverage），2510/2510 通过（run_ut ASAN），0 LSan 泄漏。

- [x] **NM49 修复（Math.max/min 的 +0/-0 语义）**（2026-05-13）：`std::fmax`/`std::fmin` 不能正确区分 `+0` 和 `-0`（C++ 标准规定 `+0.0 == -0.0`，`std::fmax` 若参数等价则返回第一个），导致 `Math.min(-0, 0)` 返回 `0` 而非 `-0`，`Math.max(-0, 0)` 返回 `-0` 而非 `0`。修复方案：interpreter.cpp 和 vm.cpp 两侧各替换 `std::fmax(result, v)` 和 `std::fmin(result, v)` 为手动比较：(1) Math.max：`v > result || (v == 0.0 && !std::signbit(v) && std::signbit(result))` 时取 `v`；(2) Math.min：`v < result || (v == 0.0 && std::signbit(v))` 时取 `v`。共改 4 行（每侧 4 行）。2268/2268 通过（coverage），2266/2266 通过（run_ut ASAN），0 LSan 泄漏。

- [x] **P3-1 JSString SSO Review 修复（M-1/M-2）**（2026-05-13）：M-1：String.prototype indexOf/lastIndexOf/slice/substring 四个方法在 null/undefined 检查之后，若 this 不是字符串则通过 `to_string_val` 转换后取 `js_string_raw()`，修复 `String.prototype.indexOf.call(42, "2")` 类调用的 assert 崩溃/UB（interpreter.cpp + vm.cpp 两侧各 4 处，共 8 处）；M-2：`JSString` 堆分配路径 `malloc` 返回 nullptr 时 `std::abort()`（rc_object.h 1 处）。2266/2268 通过（coverage，2 个预存 NM49 失败），0 LSan 泄漏。

- [x] **P3-1 JSString SSO Testing Agent 边界补测**（2026-05-13）：新增 27 个测试（SSO-P1～P10 系列）。覆盖：is_inline 标志（短字符串/32 字节边界/33 字节超出/空串）、ref_count 生命周期（工厂后=1/拷贝+1/析构-1/Move 不变/三共享=3/赋值旧引用减少）、cp_count_ 缓存持久性（拷贝共享/`sv()`不重置/`as_string()`不重置）、`sv()` 指针有效性（inline 指向内部/heap 指向外部）、内容相等不同指针、长字符串析构无泄漏（10 次创建-LSan/拷贝链逐步析构）、Move 赋值所有权转移、非 BMP 字符存储（SMP size_==4/7×28 inline/9×36 heap）、size_ 字段正确性（空/inline/heap）、含 `\0` 字节不截断。2266/2268 通过（coverage，2 个预存 NM49 失败），0 LSan 泄漏。

- [x] **P3-1 JSString SSO 优化**（2026-05-13）：`JSString` 改为 union SSO 布局（`size_`/`flags_`/union{`inline_buf_[32]`/`heap_ptr_`}，`sizeof(JSString)==48`，`static_assert` 保证）；`Value` 新增 `sv()` 返回 `std::string_view`、`js_string_raw()` 返回 `JSString*`；`as_string()` 改为返回 `std::string`（值）；`Value::string()` 工厂接受 `std::string_view`；工具函数 `utf8_cu_to_byte`/`utf8_substr`/`utf8_trim_impl`/`str_index_of`/`str_last_index_of` 及 vm.cpp 对应函数参数全部改为 `std::string_view`；消除 interpreter.cpp 5 处 + vm.cpp 6 处 `JSString tmp_str(str)` 二次堆分配（indexOf/lastIndexOf/slice/substring/length 计算）；修复 `const std::string& = as_string()` 悬空引用（to_number_double/to_number_double_vm/abstract_eq 共 6 处）；新增 12 个 SSO 单元测试（JSStringSSOTest 系列）。2239/2241 通过（coverage，2 个预先存在失败 NM49），0 LSan 泄漏。

- [x] **Number/Math 内建对象**（2026-04-28）：全局常量（NaN/Infinity 注册为 VarKind::Const）；全局函数（isNaN/isFinite 带 ToNumber 转换，parseInt/parseFloat 独立实现）；Number 构造函数（Number() 类型转换，Number.isNaN/isFinite/isInteger 不做 ToNumber，Number.parseInt === 全局 parseInt 同一对象）；Math 对象（kOrdinary，proto=object_prototype_，PI/E 常量，12 个方法：floor/ceil/round/abs/max/min/pow/sqrt/log/random/trunc/sign）；xorshift64* PRNG（chrono 初始化）；Math.round -0.5 → -0 特判；Math.max/min NaN 立即传播，fmax/fmin 保留 -0/+0 语义；Interpreter + VM 两侧对称实现（interpreter.cpp init_runtime，vm.cpp init_global_env）；GC roots 两处（exec/exec_module）各加 math_obj_/number_constructor_；clear 两处各加 math_obj_->clear_function_properties()/number_constructor_->clear_own_properties()；新增 108 个测试（NM-01～NM-54 × Interp+VM）。2143/2143 通过（coverage），2141/2141 通过（run_ut ASAN），0 LSan 泄漏。

- [x] **String.prototype 8 个方法实现（indexOf/lastIndexOf/slice/substring/split/trim/trimStart/trimEnd）**（2026-04-29）：建立 `string_prototype_`（Interpreter + VM 两侧对称）。`JSString` 新增 `cp_count_` 缓存字段（O(1) length，-1 表示未计算，首次 O(n) 后缓存）。UTF-8 工具函数（interpreter.cpp 匿名命名空间：`utf8_cp_len`/`utf8_cp_to_byte`/`utf8_substr`/`is_js_whitespace_cp`/`utf8_decode_one`/`utf8_trim_impl`/`str_index_of`/`str_last_index_of`；vm.cpp 加 `_vm` 后缀）。`eval_member_expr` 字符串分支（length 返回缓存码点数，其余查 string_prototype_）；`eval_call_expr` 字符串分支（查 string_prototype_ 获取方法，this_val 正确传递）；VM `kGetProp` 字符串分支；VM `kCallMethod` 添加 `__qppjs_pending_throw__` 消费逻辑（从 native_pending_throw_ 取真正的 TypeError 对象）。GC roots（两处 exec/exec_module 各加 `add_obj(string_prototype_.get())`）和 clear_function_properties（两处各加 `string_prototype_->clear_function_properties()`）。split 返回 `ObjectKind::kArray` 对象（修复 `RcPtr<JSObject>::make()` 默认 kOrdinary 导致 `r.length` 返回 undefined 的 bug）。测试中 NaN 用 `0/0` 代替全局 `NaN`（未注册为全局变量）；lastIndexOf fromIndex 负数语义修正为 `max(fromIndex, 0)`（规范正确行为）。新增 76 个测试（38 InterpString + 38 VmString，S-01～S-38 × 2），1967/1967 通过（coverage），1965/1965 通过（run_ut ASAN），0 LSan 泄漏。

- [x] **`export async function` 解析修复 + exec_module GC 安全修复**（2026-04-28）：修复 `parse_export_decl()` 末尾条件链未处理 `async` 上下文关键字导致 `export async function foo() {}` 报 "unexpected token after export" 的问题。Parser 改动：`export default` 分支添加 `async function` 处理（构建 `AsyncFunctionExpression` 包装进 `ExportDefaultDeclaration`）；`export const/let/var/function` 分支添加 `async function` 处理（构建 `AsyncFunctionDeclaration` 包装进 `ExportNamedDeclaration`）；两处均正确处理 `got_lf`（async 和 function 之间有换行则报错）和 `in_async_function_` 标志。运行时改动：`module_loader.cpp` Load 阶段添加 `AsyncFunctionDeclaration` 导出 Cell 分配；Interpreter/VM Link 阶段 `define_binding_with_cell` 添加 `AsyncFunctionDeclaration` 分支（initialized=true，无 TDZ）；Interpreter `hoist_module_vars` 添加 `AsyncFunctionDeclaration` 提升 define；Interpreter `exec_module_body` 提升循环添加顶层 `AsyncFunctionDeclaration` 和 `ExportNamedDeclaration` 含 `AsyncFunctionDeclaration` 的赋值；Compiler `compile_function` 提升循环添加 `ExportNamedDeclaration` 含 `AsyncFunctionDeclaration` 的 emit kMakeFunction+kSetVar+kPop。GC 安全修复（`exec_module` 路径）：Interpreter/VM 的 `exec_module` 添加 `drain_job_queue`/`vm_drain_job_queue` 调用（消费 async 调用产生的微任务），GC roots 添加 `promise_prototype_` 和 `job_queue_.CollectRoots`，GC 之后对 `eval_result`/`final_result` 中的 GC 注册对象调用 `Unregister + gc_heap_ = nullptr`（防止 VM/Interpreter 析构后调用者持有的 EvalResult 析构时 Unregister 崩溃），VM `exec_module` GC roots 添加 `promise_prototype_` 和 `promise_prototype_->clear_function_properties()`。新增 18 个测试（6 Parser：ExportAsyncFunction/ExportDefaultAnonAsyncFunction/ExportDefaultNamedAsyncFunction/ExportAsyncWithNewlineIsError/ExportFunctionRegression/TopLevelAsyncFunctionRegression；12 InterpModule+VmModule 对称：M-39 基础/M-40 含 await/M-41 匿名 default/M-42 具名 default/M-43 function 回归/M-44 顶层 async function 回归）。1860/1860 通过（coverage），1796/1796 通过（run_ut ASAN），0 LSan 泄漏。

- [x] **Array map/filter/reduce/reduceRight Review 修复（M-1/M-2/P1-1/P1-2）**（2026-04-28）：按最小改动原则修复 4 个 Review 问题，共 8 处改动（interpreter.cpp + vm.cpp 各 4 处）。M-1：filter 在 callback 执行后解引用已失效迭代器（UB）——在调用 callback 之前将 `it->second` 快照到局部变量 `Value elem = it->second`，callback 后使用 `elem` 而非 `it->second`（两侧各 1 处）。M-2：map 的 `elements_.reserve(len)` 使用逻辑长度导致稀疏数组可能 OOM——改为 `reserve(arr->elements_.size())` 按实际元素数预分配（两侧各 1 处）。P1-1：reduce/reduceRight 的 `acc = res.value()` 改为 `acc = std::move(res.value())` 避免无效 incref+decref（两侧各 2 处）。P1-2：map 结果写入 `result->elements_[i] = res.value()` 改为 `std::move`（两侧各 1 处）。1726/1726 通过（coverage），1724/1724 通过（run_ut ASAN），0 LSan 泄漏。

- [x] **P2-A：async/await 真正异步顺序保证（协程挂起/恢复机制）**（2026-04-27）：修复 `await` 同步 DrainAll 问题，使 async/await 符合 ECMAScript 规范的真正异步顺序语义（`f(); log(3)` 输出 `1,3,2` 而非 `1,2,3`）。Interpreter 侧：新增 `kAsyncSuspendSentinel`（`"__qppjs_async_suspend__"`）、`pending_await_result_`（`std::optional<Value>`）、`pending_inner_promise_`（`std::optional<RcPtr<JSPromise>>`）成员；`eval_await_expr` 改为双路径——suspend 路径（首次调用：包装 inner_promise，设置 `pending_inner_promise_`，返回 kAsyncSuspendSentinel）和 resume 路径（再次调用：检查 `pending_await_result_`，有值则返回，同时处理 reject 路径通过 `pending_throw_`）；新增 `run_async_body(shared_ptr<body>, stmt_index, fn_env, this_val, outer_promise)` 成员函数，替代原 inline 执行循环——检测 kAsyncSuspendSentinel 后，从 `pending_inner_promise_` 取出 inner_promise，构造 resume_fn（设置 `pending_await_result_`，递归调用 `run_async_body`）和 reject_fn（设置 `pending_throw_` + dummy `pending_await_result_`，递归调用），调用 `PerformThen`；`make_async_function_value` 调用 `run_async_body` 替代原 for 循环，移除末尾 `drain_job_queue()`；`eval_try_stmt` 在 try block 和 catch block 的 err 转 throw 处添加 kAsyncSuspendSentinel 透传检查；`eval_throw_stmt`/`eval_for_stmt`（test/update）同步添加透传检查；GC 安全：resume_fn/reject_fn 的 fn_env 和 outer_promise 存入 `own_properties_["__resume_env__"]`/`["__resume_promise__"]`。VM 侧：新增 `kAsyncSuspendSentinel`、`vm_async_suspended_`（bool）、`vm_pending_inner_promise_`（`std::optional<RcPtr<JSPromise>>`）、`vm_suspended_frame_`（`std::optional<CallFrame>`）成员；`kAwait` 指令改为移出当前 CallFrame（`call_stack_.pop_back(); call_depth_--`），设置 `vm_pending_inner_promise_`/`vm_suspended_frame_`/`vm_async_suspended_ = true`，`goto suspend_exit`；`run()` 末尾新增 `suspend_exit:` 标签，返回 `EvalResult::err(kAsyncSuspendSentinel)`；新增 `vm_handle_async_result(body_result, outer_promise)` 成员函数，统一处理正常/挂起/错误三路径——挂起时取出 `vm_suspended_frame_`，用 `shared_ptr<CallFrame>` 共享给 resume_fn/reject_fn（只有一个会被调用），调用 `PerformThen`，递归处理多 await 串行；async wrapper 移除末尾 `vm_drain_job_queue()`，改为调用 `vm_handle_async_result`；GC 安全：resume_fn/reject_fn 的 env 和 outer_promise 存入 `own_properties_`。新增 22 个测试（11 组 Interp+VM 对称，T1 核心顺序、T1b 同步阶段验证、T2 await 原始值、T3 多 await 串行、T4 并发交错、T5 await 赋值、T6 try/catch rejection、T7 Promise 类型、T8 嵌套 async、T9 pending Promise、T10 try/catch 后继续）。1618/1618 通过（coverage），1616/1616 通过（run_ut ASAN），0 LSan 泄漏。

- [x] **Phase 11 Review 修复（P2-B/C/D/E/F）**（2026-04-27）：修复 5 个 Review 必修问题。P2-B：Promise.prototype 挂载到构造函数——interpreter.cpp 和 vm.cpp 中 Promise 构造函数创建后调用 `set_property("prototype", promise_prototype_)`；`eval_member_expr`/`kGetProp` 的 kFunction 分支改为先查 `own_properties_`，未命中再查 `fn->prototype_obj()`，最后查 `function_prototype_`（原来 "prototype" 直接返回 `prototype_obj()` 跳过 own_properties_）。P2-C：async 函数声明提升——Interpreter 侧 `hoist_vars_stmt` 对 `AsyncFunctionDeclaration` 不仅 `define_function`，还立即调用 `make_async_function_value` 并赋值（与 VM 侧 compile_function 提升循环对称）；`eval_async_function_decl` 改为 no-op（提升阶段已赋值）；VM 侧 `hoist_vars_scan_stmt` 将 AsyncFunctionDeclaration 加入 function_decls，`compile_function` 提升循环增加 AsyncFunctionDeclaration 分支（emit kMakeFunction+kSetVar+kPop），`compile_async_function_decl` 改为 no-op。P2-D：命名 async 函数表达式内部自引用——Interpreter `make_async_function_value` 捕获 `fn_self_raw`（raw pointer），native_fn 里对有名字的 async function 在 fn_env 绑定 `name → fn_self_raw`（GC 安全：调用期间 fn 通过调用者环境可达，无需 `__async_self__` 循环引用属性）；VM `compile_async_function_expr` 设置 `child->is_named_expr = expr.name.has_value()`，`push_call_frame` 已有 is_named_expr 处理。P2-E：await 解析限制——Parser 添加 `in_async_function_` bool 字段（默认 false），async function 声明/表达式解析 body 前设 true，退出后恢复；非 async 的 FunctionDeclaration/FunctionExpression 解析 body 前设 false，退出后恢复；nud 中 await 处理加 `&& in_async_function_` 条件。P2-F：Promise 自循环 resolve——`execute_reaction_job`/`vm_execute_reaction_job` 里，handler 返回 Promise 时检查 `inner == cap_rc.get()`，若相等则 reject with TypeError（"Chaining cycle detected for promise"）。新增 16 个测试（8 组 Interp+VM 对称，覆盖 P2-B/C/D/E/F 各场景）。1596/1596 通过（coverage），1594/1594 通过（run_ut ASAN），0 LSan 泄漏。遗留：P2-A（await 异步顺序保证）需要协程挂起/恢复机制，暂未实现。

- [x] **Phase 11：Promise/Async/Await**（2026-04-27）：完整实现微任务队列、Promise 状态机、async/await，Interpreter 和 VM 两侧对称。核心改动：(1) `rc_object.h`：`ObjectKind` 追加 `kPromise`；(2) 新增 `job_queue.h/cpp`（`JobQueue`：`ReactionJob` 结构体、`Enqueue`/`DrainAll`/`CollectValueRoots`，`DrainAll` 模板函数保证微任务在本轮清空）；(3) 新增 `promise.h/cpp`（`JSPromise` 继承 `RcObject`，含 `state_`/`result_`/`is_handled_`/`fulfill_reactions_`/`reject_reactions_`，实现 `Fulfill`/`Reject`/`PerformThen`/`EnqueueReactions`/`TraceRefs`/`ClearRefs`）；(4) `ast.h`：新增 `AwaitExpression`、`AsyncFunctionDeclaration`、`AsyncFunctionExpression` 节点并加入 `ExprNode`/`StmtNode` variant；(5) `parser.cpp`：`async`/`await` 作为 contextual keyword（Ident 文本比较），支持 `async function` 声明/表达式、`await expr`；Dot 属性名接受任意 keyword（修复 `obj.catch`/`obj.finally` 等）；(6) `interpreter.h/cpp`：新增 `job_queue_`/`promise_prototype_`/`current_async_promise_`/`in_async_body_` 成员；`init_runtime()` 注册 `Promise` 全局构造函数（含 `Promise.resolve`/`Promise.reject` 静态方法）和 `promise_prototype_`（then/catch/finally）；新增 `execute_reaction_job`/`drain_job_queue`/`promise_resolve`/`make_async_function_value`/`eval_async_function_expr`/`eval_async_function_decl`/`eval_await_expr`；`eval_member_expr` 增加 `kPromise` 分支查 `promise_prototype_`；`exec()` 末尾 `DrainAll` + 重读最后标识符；(7) `vm.h/vm.cpp`：对称实现；`kMakeFunction` 识别 `is_async` 标志创建 async wrapper（NativeFn，持有 inner_fn 于 `own_properties_["__async_inner__"]`，DrainAll 同步化）；新增 `kAwait` 指令（同步 DrainAll 方案）；`kGetProp` 增加 `kPromise` 分支；(8) `compiler.h/cpp`：新增 `compile_async_function_expr`/`compile_async_function_decl`，`AwaitExpression` 编译为 `kAwait`；(9) GC 安全：lambda 捕获 promise 改为 `RcPtr`（消除 UAF），async wrapper 通过 `own_properties_` 持有 inner_fn 使其可达；(10) `value.h`：新增 `as_rc_object()` 方法（非堆对象返回 nullptr，供 `CollectValueRoots` 使用）；(11) 清理 unused-includes（`module_loader.h`/`js_object.h`/`module_record.h`/`interpreter.h`/`compiler.h`/`vm.h`/`module_test.cpp`/`parser_module_test.cpp`）。新增 42 个测试（21 组 Interp+VM 对称，覆盖 Promise 构造/resolve/reject/then/catch/finally/链式/微任务顺序/async 声明与表达式/await 暂停恢复/async throw→rejection/await rejected/GC 安全），1500/1500 通过（coverage），1498/1498 通过（run_ut ASAN），0 LSan 泄漏。

- [x] **Phase 10.2：ESM 模块系统**（2026-04-26）：完整实现静态 import/export 运行时语义（Load/Link/Evaluate 三阶段），Interpreter 和 VM 两侧对称。核心改动：(1) `ast.h`：`ExportNamedDeclaration` 添加 `std::optional<std::string> source` 字段（re-export 来源）；(2) `parser.cpp`：`export { v } from './a.js'` 支持 `from` 子句，用 `decode_string` 去引号；(3) `rc_object.h`：`ObjectKind` 追加 `kModule`；(4) 新增 `module_record.h/cpp`（`ModuleRecord` 继承 `RcObject`，含 `specifier`/`status`/`ast`/`exports`/`re_exports`/`module_env`/`dependencies`/`eval_exception`）；(5) 新增 `module_loader.h/cpp`（`ModuleLoader`：文件加载、路径解析、缓存、`TraceRoots`、`ClearModuleEnvs`、`Clear`）；(6) `environment.h/cpp`：新增 `define_binding_with_cell`（live binding 核心）和 `define_import_binding`（不可变 import binding）；(7) `interpreter.h/cpp`：新增 `exec_module`/`link_module`/`evaluate_module`/`exec_module_body`/`hoist_module_vars`，`eval_stmt` 处理 import/export，`current_module_` 字段；(8) `vm.h/vm.cpp`：新增 `exec_module`/`link_module`/`evaluate_module`/`exec_module_body`，`CallFrame::current_module`，`module_loader_` 成员；(9) `opcode.h`：新增 `kSetExportDefault`；(10) `compiler.cpp`：修改 hoist 阶段处理 `export function`，编译 `ExportNamedDeclaration`/`ExportDefaultDeclaration`/`ImportDeclaration`；(11) `vm.cpp`：`kDefLet`/`kDefConst`/`kDefVar` 跳过已存在 Binding，处理 `kSetExportDefault`；(12) 内存管理：`ClearModuleEnvs` 在 GC 后打破 `module_env ↔ JSFunction` 和 `ModuleRecord` 循环引用。新增 36 个测试（18 InterpModule + 18 VmModule，覆盖 M01-M14 + 额外场景），1410/1410 通过，0 LSan 泄漏。

- [x] **Phase 10.1 Review M1/M2/M3 修复**（2026-04-26）：修复 3 个 import/export Parser 审查必修问题。M1：`compile_stmt` 的三个 no-op visitor（ImportDeclaration/ExportNamedDeclaration/ExportDefaultDeclaration）改为 emit `kLoadString`+`kThrow`，VM 执行时产生运行时错误（与 interpreter stub 的 `ErrorKind::Syntax` 行为对齐，两者均在执行阶段报错）。M2：从 `lexer.cpp` 的 `kKeywords` 表移除 `import`/`export` 两条记录（词法器不再产出 `KwImport`/`KwExport`），在 `parse_stmt()` 入口处（switch 之前）新增 Ident 文本比较分发 `parse_import_decl()`/`parse_export_decl()`，同时删除 switch 中原有的 `case TokenKind::KwImport:`/`case TokenKind::KwExport:` 两个分支；修复后 `({ import: 1 }).import`、`obj.export` 等合法属性名解析不再失败。M3：在 `parse_stmt()` 的 labeled statement 分支（Ident + Colon）中，`parse_stmt()` 调用前先 `saved_top_level = is_top_level_; is_top_level_ = false`，调用后恢复（错误路径也恢复），修复 `label: import './m'` 被错误接受为合法语法。新增 4 个测试（ImportAsObjectKey、ExportAsDotAccess、LabeledImportIsError、LabeledExportIsError）。1375/1375 通过，0 回归。
- [x] **Phase 9 GC Review M1/M2 修复**（2026-04-26）：修复 2 个 GC 审查必修问题。M1：`GcHeap::Collect()` Phase 1 新增对 `roots` 参数的 `gc_mark_` 重置循环，解决 `object_prototype_`/`array_prototype_`/`function_prototype_`/`object_constructor_`/`error_protos_[]` 等未注册到 `objects_` 的长期根对象在第二次 exec() 时 `MarkPending` 直接返回（`gc_mark_==true`）、`TraceRefs` 不被调用、子对象被 Sweep 误删导致 UAF 的问题。M2：interpreter.cpp 中 Object.keys（2 处 kArray 分配）、Object.create（1 处 new_obj 分配）、Object() 构造器（1 处 obj 分配）的 native lambda 内补加 `gc_heap_.Register()`；vm.cpp 同步修复（create_fn 同时将捕获从 `[]` 改为 `[this]` 以访问 `gc_heap_`）。追加 4 个测试（InterpMultipleExecObjectPrototypeStaysAlive、VmMultipleExecObjectPrototypeStaysAlive、InterpObjectCreateSurvivesGc、VmObjectCreateSurvivesGc）。1312/1312 通过，run_ut 1306/1306 通过，0 个 LSan 泄露。
- [x] **Phase 9.0 Environment 从 shared_ptr 迁移到 RcPtr<Environment>（RcObject 体系）**（2026-04-26）：消除两套引用计数并存，为 Phase 9.2 mark-sweep GC 统一追踪所有堆对象奠定基础。具体改动：(1) `rc_object.h`：追加 `ObjectKind::kEnvironment`；(2) `environment.h`：`Environment` 改为继承 `RcObject`（移除 `enable_shared_from_this`），构造函数参数和 `outer_` 字段改为 `RcPtr<Environment>`，`outer()` 返回 `const RcPtr<Environment>&`；(3) `environment.cpp`：构造函数初始化 `RcObject(ObjectKind::kEnvironment)`，`clear_function_bindings` 中 `shared_from_this()` 改为 `RcPtr<Environment> self(this)`，`closure_env` 局部变量改为 `RcPtr<Environment>`；(4) `js_function.h`：`closure_env_` 字段、getter、setter 三处同步改为 `RcPtr<Environment>`；(5) `interpreter.h/cpp`：`ScopeGuard` 两个 saved 字段、构造参数，以及 `global_env_`/`current_env_`/`var_env_` 字段，`make_function_value` 参数，全部改为 `RcPtr<Environment>`；所有 `make_shared<Environment>` 改为 `RcPtr<Environment>::make(...)`；(6) `vm.h/vm.cpp`：`CallFrame::env` 和 `global_env_` 改为 `RcPtr<Environment>`；所有 `make_shared<Environment>` 改为 `RcPtr<Environment>::make(...)`；修复关键 bug：`kPopScope` 中 `env = env->outer()` 存在自我赋值导致 SEGFAULT（env 的 release 可能销毁 outer_），改为先 `RcPtr<Environment> parent = env->outer(); env = std::move(parent)`，异常恢复路径 `frame.env = frame.env->outer()` 同样修复。1219/1219 通过，run_ut 4 个 LSan 失败（P3-2 遗留，不增加）。
- [x] **Phase 8.6/8.7 VM catch 作用域修复（P2-1）+ VM labeled break 修复（P2-2）**（2026-04-26）：修复两处 Phase 7 遗留 VM 缺陷。8.6：`compile_try_stmt` 中有 finally 和无 finally 两处 catch 分支，将手动 `for (const auto& s : handler->body.body) compile_stmt(s)` 替换为 `compile_block_stmt(stmt.handler->body)`，使 catch 参数绑定在外层 `kPushScope/kPopScope` 中，catch body 的 let/const 声明由 `compile_block_stmt` 按 `has_block_scope_decl` 判断自动创建内层 scope。8.7：`compile_labeled_stmt` else 分支（非 for/while 的 labeled 语句）在 `compile_stmt` 前后注册/清理 `loop_env_stack_`，并 patch break_patches，使 `break outer` 能正确找到目标。新增 6 个测试（VMCatchScopeP21 × 3 + VMLabeledBlockBreak × 3），1219/1219 全部通过，无回归。
- [x] **Phase 8.5 Function 内建方法**（2026-04-26）：实现 `Function.prototype.call`、`apply`、`bind`，Interpreter 和 VM 两侧对称。核心改动：(1) `interpreter.h` / `vm.h` 新增 `function_prototype_` 成员（`RcPtr<JSObject>`）；(2) `init_runtime()` / `init_global_env()` 末尾注册 `function_prototype_`（proto = object_prototype_），挂载 call/apply/bind 三个 native function；(3) `eval_member_expr` / `kGetProp` 的 kFunction 分支：先查 fn->get_property(key)，未命中时查 function_prototype_->get_property(key)；(4) `eval_call_expr` 的 kFunction 分支同步加二次查找；(5) bind 用 native_fn_ lambda 封装，捕获 target/bound_this/bound_args，支持二次 bind（this 固定为第一次 bound_this）；(6) apply 支持 kArray（按 elements_ 索引展开）和 array-like（kOrdinary + length 属性 + 数字索引属性）；(7) exec/run 清理路径同步添加 function_prototype_->clear_function_properties()。新增 32 个测试（16 InterpFunctionBuiltin + 16 VMFunctionBuiltin，覆盖 FB-01~FB-16），1171/1171 全部通过。
- [x] **Phase 8.4 Object 内建方法 Review 必修问题修复 M1/M2/M3**（2026-04-26）：修复 3 个审查必修问题。M1：Object 构造函数 lambda 改为捕获 `this`，无参/null/undefined 时创建带 `set_proto(object_prototype_)` 的新对象，使 `new Object() instanceof Object` 返回 true；M2：`Object.create` 对 kFunction 参数抛 TypeError（JSFunction 不继承 JSObject，无法作为原型）；M3：`Object.assign` 对 kArray target 使用 `set_property_ex` 走数组感知路径（正确同步 `array_length_`）。Interpreter 和 VM 两侧对称修复。新增 8 个测试（OB-36/37/38/39 各 Interp+VM），1139/1139 全部通过。
- [x] **Phase 8.4 Object 内建方法**（2026-04-26）：实现 `Object.keys`、`Object.assign`、`Object.create`，Interpreter 和 VM 两侧对称。核心改动：(1) `JSObject::own_enumerable_string_keys()` 新方法，普通对象按插入顺序，数组对象先排序整数索引再追加非索引键；(2) `JSFunction` 新增 `own_properties_` 属性字典（`set_property`/`get_property`/`clear_own_properties`），新建 `js_function.cpp`；(3) Interpreter `eval_call_expr` 和 `eval_member_expr` 中 kFunction 属性读取由只支持 `prototype` 扩展为支持任意自有属性；(4) VM `kGetProp` 中 kFunction 属性读取同步扩展；(5) Object 构造函数注册为 `define_initialized`（可变），避免用户重定义 `function Object() {}` 报 const 赋值错误；(6) 所有 TypeError 通过 `pending_throw_` / `EvalResult::err` 机制正确抛出。新增 42 个测试（21 Interp + 21 VM），1103/1103 全部通过，run_ut 4 个 LSan 泄露为预先存在的 P3-2 遗留问题。
- [x] **闭包环境共享修复**（2026-04-26）：根因是 `MakeFunction` 时调用 `clone_for_closure` 对整个环境链做快照，顶层 `let` 在克隆时处于 TDZ，函数体内访问报 `ReferenceError`。删除 `clone_for_closure` / `clone_closure_env` / `define_binding` 整套克隆机制（`environment.cpp/h`、`vm.cpp/h`），`MakeFunction` 直接将当前 `env` 的 `shared_ptr` 存入 `closure_env`；解释器四处 `clone_for_closure` 调用同步改为直接传 `current_env_`。修复后 `VMFunc.ClosureSeesUpdated`、`VMFunc.FunctionCanReadOuterVar`、`VMFunc.TwoClosuresShareSameEnv`、`VMFinallyOverride.FinallyNormalSideEffectWithTryReturn` 全部通过。
- [x] **Named function expression 自引用修复**（2026-04-26）：`call_function` / `push_call_frame` 中判断是否注册自引用绑定的条件 `fn_env->lookup(...) == nullptr` 错误——`lookup` 走 outer 链，外层有同名变量时跳过自引用。在 `JSFunction` 加 `is_named_expr_` 字段，`BytecodeFunction` 加 `is_named_expr` 字段；编译器 `compile_function_expr` 和解释器 function expression 路径设置标记；`call_function` / `push_call_frame` 改为只对 named expr 无条件写入自引用绑定。修复后 `FunctionTest.NamedFunctionExpressionShadowsOuterSameName` 和 `VMFunc.NamedFunctionExpressionShadowsOuterSameName` 通过。coverage 1061/1061 全部通过。
- [x] **`scripts/qppjs.py` `split_log` 重构**（2026-04-26）：提取 `@contextlib.contextmanager split_log(success_path, failure_path, *, failure_filter)` 上下文管理器，统一"写 raw → 成功 rename / 失败分流"逻辑；`TestRunner.run`、`TestRunner.run_quiet`、`CoverageRunner.run` 三处重复代码消除约 23 行。
- [x] **build skill 工具使用规范更新**（2026-04-26）：明确 `coverage.sh` 用于 UT 功能验证（无 ASAN/LSan 噪音，失败即功能缺陷），`run_ut.sh` 专用于内存泄露检查；更新 `SKILL.md` 常用场景排序、脚本表、注意事项，更新 `CLAUDE.md` 快速参考。

- [x] 建立项目目标、参考仓库与 agent team 协作约定
- [x] 创建 6 个项目级 subagents：`es-spec`、`quickjs-research`、`design-agent`、`implementation-agent`、`testing-agent`、`review-agent`
- [x] 形成长期路线图、当前状态、下一阶段计划三文档体系
- [x] 明确当前状态更新机制以"任务完成"而不是"commit"作为主触发点
- [x] 0.1 建立最小目录结构
- [x] 0.2 建立最小构建链路（顶层 CMake 骨架）
- [x] 0.3 建立最小 CLI
- [x] 0.4 设计错误处理基础结构
- [x] 0.5 设计第一版 Value
- [x] 0.6 建立测试基线
- [x] 0.7 建立调试输出入口
- [x] 0.8 建立覆盖率报告链路（lcov + genhtml，HTML 行/分支覆盖率）
- [x] 构建系统重构：完成 CMake 公共配置收口（`cmake/Options.cmake` 统一管理 options，根 `CMakeLists.txt` 通过 `QPPJS_GLOBAL_COMPILER_OPTIONS` + `add_compile_options()` 注入全局编译选项，按需通过 `add_link_options()` 注入全局链接选项）、构建 metadata 导出、移除 `CMakePresets.json`、构建脚本收缩为 `build_release.sh` / `build_debug.sh` / `build_test.sh` / `run_ut.sh` / `coverage.sh` 固定入口

- [x] Phase 1：Lexer + Parser + AST（已全部完成）
  - [x] 1.1 实现 Tokenizer 基础结构（TokenKind、Token、SourceRange、SourceLocation、LexerState、next_token 最小实现）
  - [x] 1.2 支持基础词法元素（数字字面量、字符串字面量、多字符操作符）
  - [x] 1.3 设计 AST 节点体系（header-only，9 个表达式节点 + 6 个语句节点 + Program，variant 包装，overloaded helper）
  - [x] 1.4 实现表达式解析（Pratt Parser：原子/一元/二元/逻辑/赋值，含优先级与结合性）
  - [x] 1.5 实现语句解析（ExpressionStatement、VariableDeclaration、BlockStatement、IfStatement、WhileStatement、ReturnStatement + 最小 ASI）
  - [x] 1.6 实现 AST dump（dump_expr / dump_stmt / dump_program，缩进树形格式，17 个测试全部通过）
  - [x] 1.7 建立 parser 错误报告（make_parse_error 辅助函数，位置信息拼入 message，格式 "line N, column M: <描述>"，5 个新测试，242/242 全部通过）

- [x] Phase 2：AST Interpreter（已全部完成）
  - [x] 2.1 Environment / Scope（Binding struct，链式 Environment，VarKind define，TDZ，get/set/initialize）
  - [x] 2.2 表达式求值（NumberLiteral/StringLiteral/BooleanLiteral/NullLiteral/Identifier/UnaryExpression/BinaryExpression/LogicalExpression/AssignmentExpression）
  - [x] 2.3 语句执行（ExpressionStatement/VariableDeclaration/BlockStatement/IfStatement/WhileStatement/ReturnStatement + var 提升）
  - [x] 2.4 ToBoolean falsy 规则（undefined/null/false/0/NaN/""）
  - [x] 2.5 Completion 模型（CompletionType kNormal/kReturn，EvalResult/StmtResult，顶层 return 视为正常完成）
  - Parser 调整：允许顶层 return（移除 function_depth 检查），更新 3 个相关 parser 测试
  - CLI 更新：接入 Interpreter，parse + exec + format_value
  - 321/321 测试全部通过（含新增 65 个 interpreter 测试）
  - Bug 修复（Review + Testing Agent 审查后）：typeof TDZ 应抛 ReferenceError、let 无初始化值应为 undefined、字符串关系比较使用词典序；Testing Agent 补充 35 个边界测试；359/359 测试全部通过

- [x] Phase 3：Object Model（已全部完成）
- [x] Phase 4：Function（已全部完成）
  - [x] 4.1 Environment outer_ 改为 shared_ptr，Interpreter 增加 var_env_、call_depth_
  - [x] 4.2 AST 扩展（FunctionDeclaration/FunctionExpression/CallExpression）
  - [x] 4.3 JSFunction 类（ObjectKind::kFunction，private 成员，accessor 访问）
  - [x] 4.4 Parser 扩展（lbp(LParen)=16，nud KwFunction，led LParen，parse_function_params/body）
  - [x] 4.5 hoist_vars 修正（var_target 参数，var 提升到函数作用域）
  - [x] 4.6 eval_call_expr/eval_function_decl/eval_function_expr（RAII call_depth_，闭包，递归）
  - [x] 4.7 AST dump 扩展（FunctionDeclaration/FunctionExpression/CallExpression）
  - Bug 修复（Review Agent 审查后）：eval_function_decl 改为 var_env_->set()、eval_member_expr/assign 中 assert 改为 if + 返回 undefined/TypeError、call_depth_ 纳入 ScopeGuard RAII 管理
  - 475/475 测试全部通过（原有 417 个 + 新增 58 个函数测试）

- [x] Phase 5：原型链、this、new（已全部完成）
  - [x] 5.1 JSObject proto_ 字段 + 原型链查找 + object_prototype_
  - [x] 5.2 JSFunction prototype_ 字段 + 急切初始化（make_function_value）
  - [x] 5.3 this 关键字支持（KwThis token）+ ScopeGuard 扩展（saved_this/new_this）
  - [x] 5.4 方法调用 this 提取 + call_function 抽取（返回 StmtResult）
  - [x] 5.5 NewExpression AST + Parser + eval_new_expr
  - Bug 修复（Review Agent P1-1）：call_function 返回 StmtResult 区分显式 return object 与自然完成
  - 531/531 测试全部通过（原有 475 个 + 新增 56 个原型/this/new 测试）

- [x] Phase 6：Bytecode VM（已全部完成）
  - [x] 6.1 Opcode 枚举（49 条，X-Macro）+ BytecodeFunction 结构体
  - [x] 6.2 Compiler 框架 + 字面量 + 算术表达式 + VM 骨架
  - [x] 6.3 变量、作用域、控制流（let/const/var、BlockStatement、if/while、logical）
  - [x] 6.4 函数声明与调用（闭包、递归、var 提升、函数声明提升）
  - [x] 6.5 对象、属性、方法调用、this、new（含 JSFunction.prototype 读写）
  - [x] 6.6 typeof 特殊处理（TypeofVar 指令，未声明变量安全返回 "undefined"）
  - [x] 6.7 全量 VM 测试（134 个测试）+ main.cpp --vm flag
  - 529（interpreter）+ 134（VM）= 663 个测试全部通过

- [x] Phase 7：控制流扩展（break/continue/throw/try/catch/finally）
  - [x] 7.1 规范调研 + 设计
  - [x] 7.2 AST 节点扩展 + Parser 扩展
  - [x] 7.3 AST Interpreter 实现（CompletionType 扩展、6 个新 eval 方法、NativeFn + Error 内建）
  - [x] 7.4 VM 编译器扩展：6 条新 opcode、Compiler 实现、VM dispatch 扩展、Error 内建 VM 侧注册
  - [x] 7.5 Error 内建对象（Interpreter 7.3 已含，VM 7.4 补充）
  - [x] 7.6 全量测试回归（736/736 通过）
  - [x] 7.7 边界测试补充（Testing Agent）：新增 56 个边界测试，790/790 通过，2 个已知 VM 缺陷标注 DISABLED
  - [x] 7.8 P1-1 缺陷修复：compile_return_stmt 增加 finally_info_stack_ 穿越逻辑（LeaveTry + Gosub），792/792 全量通过

## 2. 最近完成内容

- 完成 `scripts/qppjs.py coverage --quiet` 日志行为修复：
  - 保留实际日志路径：成功为 `build/coverage_success.log`，失败为 `build/coverage_failure.log`
  - quiet 模式改为先写原始输出到临时 `build/coverage_raw.log`；若失败，仅提取失败 UT / LSan 摘要写入 `coverage_failure.log`，避免把 build、lcov、genhtml 全量输出直接落到失败日志
  - 复用 `write_test_failure_report()`，并允许传入自定义标题，使 coverage 失败报告格式与 `run_ut` 保持一致
  - 同步更新顶层 epilog 与 `coverage --help` 文案，明确失败日志只包含失败 UT 摘要
  - 验证：`python3 -m py_compile scripts/qppjs.py` 通过

- 完成 `scripts/qppjs.py test --quiet` 日志目录修复：
  - `scripts/qppjs.py` 中 `TestRunner.run()` 的 quiet 构建日志从 `build/run_ut_build_failure.log` / `build/run_ut_build_success.log` 调整到 `build/debug/run_ut_build_failure.log` / `build/debug/run_ut_build_success.log`
  - 使 UT quiet 模式下构建日志与 ctest 成功/失败日志 (`run_ut_success.log` / `run_ut_failure.log`) 保持同目录，避免成功和错误日志分散在 `build/` 与 `build/debug/`
  - 同步更新顶层 epilog 与 `test --help` 文案，明确构建日志也位于 `build/debug/`
  - 验证：`python3 -m py_compile scripts/qppjs.py` 通过

- 完成构建脚本 Python 统一入口重构：
  - 新增 `scripts/qppjs.py`，用 argparse 子命令统一覆盖 `clean`、`build debug/release/test`、`test`、`coverage`
  - 收口重复逻辑：项目路径发现、macOS Homebrew LLVM 探测、CMake 参数拼装、build metadata 读取、ctest 参数构造、quiet 失败报告解析、coverage backend 分支
  - `scripts/build_debug.sh`、`build_release.sh`、`build_test.sh`、`clean.sh`、`run_ut.sh`、`coverage.sh` 保留为薄 wrapper，继续兼容原命令入口
  - 帮助信息已优化：顶层 `--help` 展示常用示例和兼容入口；`build --help` 展示 debug/release/test 的 build 目录与 CMake 开关；`test`、`coverage`、`clean` 子命令说明输出产物和行为边界；支持 `clean build release`、`clean test --quiet`、`clean coverage --quiet --open` 前置组合用法；coverage 支持 `--quiet` 静默构建、ctest、lcov/genhtml；quiet 模式区分成功/失败日志（UT：ctest 日志为 `build/debug/run_ut_success.log` / `build/debug/run_ut_failure.log`，构建日志为同目录下的 `run_ut_build_success.log` / `run_ut_build_failure.log`；coverage：`build/coverage_success.log` / `build/coverage_failure.log`）
  - 验证：`python3 -m py_compile scripts/qppjs.py` 通过；6 个 shell wrapper `bash -n` 通过；Python 与 wrapper 帮助输出通过；`python3 scripts/qppjs.py clean build release` 通过；`python3 scripts/qppjs.py clean test --quiet` 生成预期失败报告；`python3 scripts/qppjs.py test --quiet` 可静默失败并写入 `build/debug/run_ut_failure.log`；`python3 scripts/qppjs.py coverage --quiet` 可静默失败并写入 `build/coverage_failure.log`（当前 coverage ctest 有 4 个预存失败）

- 修复函数/闭包/原型相关 ASAN/LSan 泄漏：
  - `include/qppjs/runtime/environment.h`、`src/runtime/environment.cpp`：为 `Binding` 增加 `function_like` 标记，补充 `define_function()`、`clone_for_closure()`、`clear_function_bindings()`；递归清理 closure env 与对象属性中的函数引用
  - `include/qppjs/runtime/js_object.h`、`src/runtime/js_object.cpp`：新增 `clear_function_properties()`，递归清理对象/prototype 链上保存的函数值，打断 `obj -> fn -> prototype/closure` 保留环
  - `src/runtime/interpreter.cpp`：函数声明/表达式改为捕获裁剪后的 closure env；顶层 `exec()` 在成功/异常返回前统一清理函数绑定与对象属性中的函数引用
  - `include/qppjs/vm/bytecode.h`、`src/vm/compiler.cpp`：新增 `function_decls`，将函数声明提升槽与 `var_decls` 分离，避免 VM 在清理函数绑定时误伤普通变量绑定
  - `include/qppjs/vm/vm.h`、`src/vm/vm.cpp`：VM 顶层与函数帧分别预定义 `function_decls`；`kMakeFunction` 直接捕获运行时环境共享绑定；`exec()` 返回前同步执行清理逻辑
  - 验证：`FunctionTest` / `VMFunc` / `VMProto` / `InterpreterThrow|TryCatch|FinallyOverride` / `VMTryCatch|FinallyOverride` 分组回归 122/122 通过；`./scripts/run_ut.sh` 全量通过，ASAN/LSan 无泄漏

- 建立 macOS LSan 基础设施 + 闭包边界测试补充：
  - `scripts/run_ut.sh` 加 `ASAN_OPTIONS=detect_leaks=1`（macOS 上 LSan 默认关闭，需显式开启）
  - 深度调研 Interpreter 闭包循环引用根因：`clone_for_closure` 产生的克隆 Environment 与其中
    持有的 JSFunction 形成 `clone_env → Cell → JSFunction → closure_env_(clone_env)` 循环；
    Cell 共享设计下引用计数无法打断（`ClosureSeesReassignedFunctionBinding` 要求必须共享 Cell），
    三种常规方案（`outer_` weak_ptr、`closure_env_` weak_ptr、`captured_upvalues_` 替换）均无效；
    结论：需要 Phase 9 标记清除 GC 才能根本解决，记为遗留问题 P3-2（5256 字节，39 个分配）
  - `tests/unit/function_test.cpp` 新增 10 个边界用例：三层嵌套捕获、外层修改后闭包读新值、
    for-var 循环闭包读最终值、具名函数表达式递归、闭包写外层 let、多工厂调用独立性、
    空捕获、const 捕获写入报错、var 遮蔽外层 let（×2）
  - 验证：927/931 通过（4 个 VM 已知失败不变）

- 完成构建脚本跨平台探测修复：
  - `scripts/build_debug.sh`、`scripts/build_release.sh`、`scripts/build_test.sh`：仅在 `CC/CXX` 都未设置、平台为 `Darwin` 且 `brew` 存在时才探测 Homebrew LLVM；其他环境保持未设置 `CC/CXX`，交由 CMake/系统编译器选择，避免在 `set -euo pipefail` 下因缺少 `brew` 直接退出
  - 当前 Linux/WSL 环境下已验证 `./scripts/build_debug.sh`、`./scripts/build_release.sh`、`./scripts/build_test.sh` 均可完成构建

- 完成 Phase 8.1 — Error 子类（TypeError/ReferenceError/RangeError）+ instanceof 运算符：
  - **新增文件**：`include/qppjs/runtime/native_errors.h`（`NativeErrorType` 枚举 + `MakeNativeErrorValue` 工厂函数声明）、`src/runtime/native_errors.cpp`（工厂函数实现）
  - **新增 instanceof 支持**：`token.h` 添加 `KwInstanceof`；`ast.h` 添加 `BinaryOp::Instanceof`；`lexer.cpp` 注册关键字；`parser.cpp` 添加 lbp=10 和 led 处理；`ast_dump.cpp` 添加 case；`opcode.h` 添加 `kInstanceof`；`compiler.cpp` emit `kInstanceof`
  - **VM 侧**：`vm.h` 添加 `error_protos_[4]` 缓存数组和 `make_error_value` 私有方法；`vm.cpp` 重构 `init_global_env()`（完整 Error 原型链：Error → TypeError/ReferenceError/RangeError）、实现 `kInstanceof` opcode（原型链遍历）、将所有字符串抛出替换为 Error 实例（kGetVar/kSetVar/kInitVar/kGetProp/kSetProp/kGetElem/kSetElem/kCall/kCallMethod/kNewCall/kTypeofVar）、修正顶层异常消息格式（`name: message`）
  - **Interpreter 侧**：`interpreter.h` 添加 `error_protos_[4]` 和 `make_error_value`；`interpreter.cpp` 重构构造函数（完整 Error 原型链）、在 `eval_binary` 添加 `BinaryOp::Instanceof` case
  - **修复 P2-3**：内部运行时错误（ReferenceError/TypeError）从字符串改为真正的 Error 实例
  - **新增测试**：`tests/unit/vm_error_test.cpp`（20 个测试）、`tests/unit/interpreter_error_test.cpp`（16 个测试）
  - 861/861 测试全部通过，ASAN/LSan 无泄漏

- 完成 Phase 8.1 Review 必修问题修复（M1/M2/M3）：
  - **M1 — VM 错误消息双重前缀**：在 `src/vm/vm.cpp` 添加 `strip_error_prefix()` 辅助函数，在 kGetVar/kSetVar/kInitVar/kCall/kCallMethod/kNewCall 中所有从 Environment C++ Error 取消息的路径调用剥离前缀，确保 `e.message` 不含 `"XxxError: "` 前缀
  - **M2 — Error 原型链缺少 constructor 属性**：在 `src/vm/vm.cpp` 的 `init_global_env()` 和 `src/runtime/interpreter.cpp` 的构造函数中，为 Error 和每个子类 prototype 调用 `set_constructor_property(fn.get())`，使 `XxxError.prototype.constructor === XxxError`
  - **M3 — Interpreter 路径运行时异常仍为字符串**：在 `src/runtime/interpreter.cpp` 中添加 `strip_error_prefix()` 辅助函数；将 `eval_identifier`、`eval_assignment`、`eval_member_expr`、`eval_member_assign`、`eval_call_expr`、`eval_new_expr` 中所有运行时错误路径改为设置 `pending_throw_` 并返回哨兵；同时修复 `exec()` 顶层未捕获错误的格式化（`name: message`）
  - **测试更新**：`vm_error_test.cpp` 新增 T-21（M1）、T-22（M2）共 6 个测试；`interpreter_error_test.cpp` T-12/T-13 从期望字符串改为期望 Error 实例，新增 T-17（M2）共 3 个测试
  - 917/917 测试全部通过，ASAN/LSan 无泄漏

- 完成 P1 全部热路径性能优化（来自 docs/perf/001-all.md）：
  - **P1-1+2**：`Cell` 增加非原子引用计数，`CellPtr` 从 `shared_ptr<Cell>` 改为 `RcPtr<Cell>`，保留闭包共享可变语义，消除原子操作开销（`environment.h`、`environment.cpp`）
  - **P1-3**：`push_call_frame` 改为接受 `std::span<Value>`；kCall/kCallMethod/kNewCall 三处用 8 元素栈缓冲区收集参数，消除小参数调用的 malloc，同时消除 `std::reverse`（`vm.h`、`vm.cpp`）
  - **P1-4**：Compiler 新增 `has_block_scope_decl` 扫描，无 let/const/function 声明的块跳过 kPushScope/kPopScope，消除循环体每次迭代的 `make_shared<Environment>`（`compiler.cpp`）
  - **P1-5**：`FlatBindingMap` 替换 `unordered_map`，≤16 绑定线性扫描，超出阈值懒升级，提升小函数变量查找的 cache 局部性（`environment.h`、`environment.cpp`）
  - **P1-6**：新增 `number_to_string` 辅助函数，整数快路径用 `std::to_string`，浮点用 `std::to_chars` + 栈缓冲区，消除 `ostringstream` 构造开销（`vm.cpp`）
  - **P1-7/8/9**：NaN-boxing 迁移时已顺带解决（`kind()` O(1)、LoadString 引用计数、`dynamic_pointer_cast` → `static_cast`）
  - 825/825 全量通过，ASAN/LSan 无泄漏

- 完成 P2 设计层技术债务（部分）：
  - P2-1：Value NaN-boxing（`sizeof(Value)` 40→8 字节）
  - P2-2：`ObjectPtr` 改为 `RcPtr<RcObject>`（非原子引用计数）
  - P2-4：`add_name` 加反向索引，O(n)→O(1)
  - P2-5：`parse_number_text` 用 `std::from_chars` 替代 `stod`，消除临时 string
  - P2-3（`Environment::outer_` shared_ptr 链）：待 Phase 9 GC 统一处理

- 完成 Value NaN-boxing + ObjectPtr 非原子引用计数迁移（Phase 0 基础设施优化）：
  - 新增 `include/qppjs/runtime/rc_object.h`：`RcObject` 基类（非原子 `ref_count_` + `object_kind_`，无虚函数 kind 查询）、`RcPtr<T>` 智能指针（copy/move/destructor 管理引用计数，支持 derived→base 隐式上转型）、`JSString`（内嵌引用计数的堆字符串）
  - 重写 `include/qppjs/runtime/value.h`：8 字节 NaN-boxing Value，`static_assert(sizeof(Value) == 8)`，`as_object()` 返回值类型 `ObjectPtr`，新增 `as_object_raw()` 热路径接口
  - 重写 `src/runtime/value.cpp`：NaN 规范化（位操作检测，规范化为 `kCanonicalNaN`），完整的 copy/move/析构引用计数管理
  - 更新 `include/qppjs/runtime/js_object.h`：继承 `RcObject`（替代 `Object`），`proto_` 改为 `RcPtr<JSObject>`，`constructor_property_` 改为裸指针（弱引用）
  - 更新 `include/qppjs/runtime/js_function.h`：继承 `RcObject`（替代 `Object`），`prototype_` 改为 `RcPtr<JSObject>`，`closure_env_`/`body_`/`bytecode_` 保留 `std::shared_ptr`
  - 更新 `src/runtime/js_object.cpp`：适配新接口（`set_constructor_property(RcObject*)` 弱引用）
  - 更新 `src/runtime/interpreter.cpp` + `src/vm/vm.cpp`：全面替换 `std::make_shared<JSObject/JSFunction>` → `RcPtr::make()`，`dynamic_pointer_cast` → `object_kind()` + `static_cast`，`as_object()` → `as_object_raw()` 热路径
  - 更新 `tests/unit/proto_test.cpp`、`tests/unit/value_test.cpp`：适配新 API
  - 新增 `tests/unit/value_nanboxing_test.cpp`：33 个新测试（double 边界值、tag 编码、copy/move 语义、引用计数路径、`sizeof(Value)==8`）
  - 825/825 全量通过（原 792 + 新增 33），ASAN/LSan 无泄漏

- 完成 Phase 7 P1-1 缺陷修复（7.8）：
  - `src/vm/compiler.cpp`：`compile_return_stmt` 在 `finally_info_stack_` 非空时，对每个活跃 finally 从内到外 emit `kLeaveTry` + `kGosub`（patch 位置记录到 `gosub_patches`，由 `compile_try_stmt` 统一 patch），最后 emit `kReturn`；无参 return 情形先 emit `kLoadUndefined` 再走相同路径
  - `tests/unit/vm_phase7_edge_test.cpp`：去掉 `DISABLED_` 前缀启用 `FinallyReturnOverridesTryReturn` 和 `FinallyNormalSideEffectWithTryReturn` 两个测试
  - 792/792 全量通过（原 790 + 新启用 2，0 回归）

- 完成 Phase 7 子任务 7.4：VM 编译器扩展：
  - `include/qppjs/vm/opcode.h`：新增 6 条指令（Throw/EnterTry/LeaveTry/GetException/Gosub/Ret）
  - `include/qppjs/vm/vm.h`：CallFrame 新增 ExceptionHandler 结构体、handler_stack/pending_throw/caught_exception/finally_return_stack/scope_depth 字段
  - `include/qppjs/vm/compiler.h`：新增 LoopEnv/FinallyInfo 结构体、loop_env_stack_/finally_info_stack_ 字段、6 个新 compile_* 方法声明、patch_jump_to/emit_jump_to/current_offset 辅助方法
  - `src/vm/compiler.cpp`：新增 patch_jump_to/emit_jump_to/current_offset；hoist_vars_scan_stmt 扩展；compile_while_stmt 重构；compile_throw_stmt/compile_try_stmt（三分支）；compile_break_stmt/compile_continue_stmt；compile_labeled_stmt/compile_for_stmt
  - `src/vm/vm.cpp`：VM 构造函数拆分，新增 init_global_env()；run() 循环顶部新增 exception_handler 逻辑；新增 kThrow/kEnterTry/kLeaveTry/kGetException/kGosub/kRet handler
  - `tests/unit/vm_phase7_test.cpp`（新建）：29 个 VM Phase 7 测试
  - 736/736 全量通过（原有 707 个 + 新增 29 个）

- 完成 Value NaN-boxing + ObjectPtr 非原子引用计数（Phase 12.1 提前实施）：
  - `include/qppjs/runtime/rc_object.h`（新建）：`RcObject` 基类（非原子 ref_count + object_kind 数据成员）、`RcPtr<T>` 智能指针（含 derived→base 隐式上转型）、`JSString` 结构体（非原子引用计数）
  - `include/qppjs/runtime/value.h`：完整替换为 NaN-boxing（`uint64_t raw_`，8 字节），`using ObjectPtr = RcPtr<RcObject>`，新增 `as_object_raw()` 裸指针接口，`as_object()` 改为值返回
  - `src/runtime/value.cpp`：NaN 规范化（位操作）、tag/payload 编码、copy/move/析构引用计数管理
  - `include/qppjs/runtime/js_object.h`：继承 `RcObject`，`proto_` 改为 `RcPtr<JSObject>`，`constructor_property_` 改为裸指针
  - `include/qppjs/runtime/js_function.h`：继承 `RcObject`，`prototype_` 改为 `RcPtr<JSObject>`
  - `include/qppjs/runtime/interpreter.h`、`include/qppjs/vm/vm.h`：`object_prototype_` 改为 `RcPtr<JSObject>`
  - `src/runtime/interpreter.cpp`、`src/vm/vm.cpp`：`make_shared` → `RcPtr::make()`，`dynamic_pointer_cast` → `object_kind()+static_cast`
  - `tests/unit/value_nanboxing_test.cpp`（新建）：33 个新测试（NaN-boxing 位编码、引用计数语义、RcPtr 语义）
  - 792 → 825（+33），ASAN/LSan 无泄漏，`sizeof(Value) == 8`

## 3. 风险与待决策项

- P2-1（已知，暂不处理）：VM catch 参数与 catch 体共享同一 scope，规范要求两层独立作用域；`catch(e) { let e = 2; }` 在 VM 路径下会失败
- P2-2（已知，暂不处理）：VM `compile_labeled_stmt` 对非循环体的 labeled break 会触发 `assert(false)`；Interpreter 路径已正确实现
- P2-3：内部运行时错误（ReferenceError/TypeError）以字符串值抛出，而非 Error 对象；Phase 8 升级为真正的 Error 子类
- P3-1（新，已知）：`JSString` 二次堆分配（`std::string` 成员），Phase 9 优化
- P3-2（新，已知）：循环引用（proto 链、closure env）导致内存泄漏，Phase 9 GC 解决
- JSFunction::body_ 字段继续保留（AST Interpreter 使用）
- GetProp/SetProp 对 JSFunction 的非 prototype 属性当前静默忽略

## 4. 2026-04-24 内部性能优化（无语义变化）

- `src/frontend/parser.cpp`：`parse_number_text` 十进制路径从 `std::stod`（try/catch）改为 `std::from_chars`，消除异常开销，与"禁止异常"约定对齐
- `include/qppjs/vm/compiler.h` + `src/vm/compiler.cpp`：`add_name` 新增 `name_index_`（`unordered_map<string, uint16_t>`）反向索引，去重从 O(n) 降至 O(1)；`compile_function` 上下文切换时保存/恢复索引
- 测试：825/825 通过，ASAN/LSan 无泄漏

## 5. 2026-04-24 P1 性能优化三连（Cell RcPtr + span + FlatBindingMap）

- **P1-1+2（Cell RcPtr）**：`include/qppjs/runtime/environment.h`、`src/runtime/environment.cpp`
  - `Cell` 增加 `int32_t ref_count`、`add_ref()`、`release()` 方法
  - `CellPtr` 从 `std::shared_ptr<Cell>` 改为 `RcPtr<Cell>`
  - `MakeCell` 从 `std::make_shared` 改为 `RcPtr<Cell>::make`
  - 消除 `Cell` 的原子引用计数开销

- **P1-3（push_call_frame span）**：`include/qppjs/vm/vm.h`、`src/vm/vm.cpp`
  - `push_call_frame` 签名改为 `std::span<Value> args`
  - kCall/kCallMethod/kNewCall 三处调用点：≤8 参数用栈上 `Value small_buf[8]`，>8 参数用 `std::vector<Value>`，消除 `std::vector` 堆分配和 `std::reverse`
  - native 函数调用路径仍用 `std::vector<Value>`（从 span 构造）
  - 移除 `#include <algorithm>`

- **P1-5（FlatBindingMap）**：`include/qppjs/runtime/environment.h`、`src/runtime/environment.cpp`
  - 新增 `FlatBindingMap` 类：≤16 条目线性扫描（`vector<pair<string, Binding>>`），超出阈值懒升级到 `unordered_map`
  - `find()` 返回 `Binding*`（nullptr 表示未找到）
  - `Environment::bindings_` 类型从 `BindingMap`（unordered_map）改为 `FlatBindingMap`
  - `lookup()` 适配新接口
  - 测试：825/825 通过，ASAN/LSan 无泄漏

## 6. 2026-04-24 P1-6 ostringstream 替换 + P1-4 kPushScope 条件化

- **P1-6（number_to_string）**：`src/vm/vm.cpp`
  - 移除 `#include <sstream>`，新增 `#include <charconv>`
  - 新增文件内静态函数 `number_to_string(double)`：`d==0.0` → `"0"`；整数快路径（floor 判断 + 安全范围）→ `std::to_string(int64_t)`；一般浮点 → `std::to_chars(general, 17)`
  - `to_string_val` 的 Number 分支改为调用 `number_to_string`
  - `init_global_env` 的 Error native lambda 同样改为调用 `number_to_string`

- **P1-4（kPushScope 条件化）**：`src/vm/compiler.cpp`
  - 新增静态辅助函数 `has_block_scope_decl`：扫描 stmts，存在 let/const 声明或函数声明则返回 true
  - `compile_block_stmt`：`need_scope = has_block_scope_decl(stmt.body)`，条件化 emit kPushScope/kPopScope
  - `compile_stmt_last` 的 BlockStatement 分支：同样条件化 emit kPushScope/kPopScope
  - 效果：只有 var 的块、空块不再创建多余 scope；有 let/const 的块和有函数声明的块仍正确创建 scope
  - 测试：1516/1516 通过，ASAN/LSan 无泄漏

## 7. 2026-04-25 Phase 8.2 — console 对象

- **改动文件**：`src/runtime/interpreter.cpp`、`src/vm/vm.cpp`、`tests/unit/console_test.cpp`、`tests/CMakeLists.txt`
- **实现内容**：
  - 在 `Interpreter::init_runtime()` 末尾追加 console 注册：创建 `log_fn`（native lambda，调用 `Interpreter::to_string_val` 拼接参数，`std::cout` 输出）；创建 `console_obj`，`set_proto(object_prototype_)`，注册 `log` 属性；`global_env_->define_initialized("console")` + `set`
  - 在 `VM::init_global_env()` 末尾追加 console 注册：同上，调用 `VM::to_string_val`；`global_env_->define("console", VarKind::Const)` + `initialize`
  - 两侧均添加 `#include <iostream>`
- **新增测试**：`tests/unit/console_test.cpp`，包含 `InterpConsole` 和 `VMConsole` 两个 test suite，各 10 个测试（C-01 到 C-10）
- **验证**：20/20 console 测试通过；全量 947/951 通过（4 个预存失败不变）

## 8. 2026-04-25 Phase 8.2 边界测试补充 — console 边界用例

- **改动文件**：`tests/unit/console_test.cpp`（追加）
- **新增测试（C-11 到 C-24，InterpConsole + VMConsole 各 14 个，共 28 个）**：
  - C-11: 5 个参数空格分隔正确（`1 2 3 4 5\n`）
  - C-12: NaN 输出（`0/0` 产生 NaN，因 `NaN` 全局标识符未注册）
  - C-13: Infinity 输出（`1/0`）
  - C-14: -Infinity 输出（`-1/0`）
  - C-15: 0 输出 "0"
  - C-16: -0 输出 "0"（规范 ToString(-0) === "0"，两侧均通过）
  - C-17: 负整数 -42
  - C-18: 空字符串参数 → 仅输出 "\n"
  - C-19: 含空格字符串原样输出
  - C-20: 连续两次调用各自独立输出一行
  - C-21: console.log 赋值给变量后调用
  - C-22: console 注册不影响其他全局变量查找（回归）
  - C-23: console.log 返回值为 undefined
  - C-24: 混合类型 5 参数（undefined null true 0 end）
- **发现缺失功能**：`NaN` / `Infinity` 全局标识符未注册（规范 §18.1.1/§18.1.2 要求），测试中改用算术表达式绕过，待后续补注册
- **验证**：48/48 console 测试通过；全量 977/981 通过（4 个预存失败不变）

## 9. 2026-04-25 Phase 8.3 — Array 基础

- **改动文件**：
  - `include/qppjs/runtime/rc_object.h`：`ObjectKind` 增加 `kArray`
  - `include/qppjs/runtime/js_object.h`：增加 `elements_` 成员、带 kind 参数的构造函数、`set_property_ex` 方法（length setter RangeError 校验）
  - `include/qppjs/runtime/js_function.h`：`NativeFn` 签名增加 `Value this_val` 第一参数
  - `include/qppjs/frontend/ast.h`：新增 `ArrayExpression` 节点，加入 `ExprNode` variant
  - `include/qppjs/vm/opcode.h`：新增 `kNewArray` 指令
  - `include/qppjs/runtime/interpreter.h`：增加 `array_prototype_` 成员、`eval_array_expr` 和 `call_function_val` 声明、`<span>` include
  - `include/qppjs/vm/vm.h`：增加 `array_prototype_` 成员、`call_function_val` 声明
  - `include/qppjs/vm/compiler.h`：增加 `compile_array_expr` 声明
  - `src/runtime/js_object.cpp`：实现 `try_parse_array_index`、kArray 分支的 `get_property`/`set_property`/`set_property_ex`、`clear_function_properties` 扩展到 kArray
  - `src/frontend/parser.cpp`：`nud()` 增加 `LBracket` 分支（数组字面量解析，含 elision 和尾随逗号）；`expr_range` 增加 `ArrayExpression` case
  - `src/frontend/ast_dump.cpp`：`dump_expr` 增加 `ArrayExpression` case
  - `src/runtime/interpreter.cpp`：所有 NativeFn lambda 签名增加 `Value this_val` 参数；`eval_expr` 增加 `ArrayExpression` 分支；新增 `eval_array_expr`、`call_function_val` 实现；`init_runtime()` 注册 `array_prototype_`（push/pop/forEach）；修复 `eval_member_expr`/`eval_member_assign`/`eval_call_expr` 支持 kArray；`call_function` native 分支传 this_val
  - `src/vm/vm.cpp`：所有 NativeFn lambda 签名增加 `Value this_val` 参数；`kCall`/`kCallMethod`/`kNewCall` native 分支传 this_val（CallMethod 传 receiver）；新增 `kNewArray` 实现；`kGetProp`/`kSetProp`/`kGetElem`/`kSetElem` 支持 kArray（含整数快路径守卫）；`init_global_env()` 注册 `array_prototype_`；新增 `call_function_val` 实现
  - `src/vm/compiler.cpp`：`compile_expr` 增加 `ArrayExpression` 分支；新增 `compile_array_expr` 实现（NewArray + 逐元素 SetElem）
  - `tests/unit/array_test.cpp`（新建）：40 个测试（InterpArray A-01~A-20，VMArray A-01~A-20）
  - `tests/CMakeLists.txt`：注册 `array_test.cpp`

- **关键设计决策**：
  - `set_property_ex` 处理 length setter 的 RangeError（非整数、负数、>4294967295）；普通属性走 `set_property`
  - `kGetElem`/`kSetElem` kArray 快路径：`d >= 0 && d == floor(d) && d < UINT32_MAX` 守卫，防止 `arr[-1]` 触发巨型 resize
  - `forEach` 迭代范围在进入时固定（`len = elements_.size()`），循环中 push 不影响迭代次数（C-14 规范）
  - `call_function_val` 在 VM 侧通过 `run(exit_depth)` 嵌套运行，支持 forEach callback 调用 JS 函数
  - NativeFn 签名增加 `this_val` 参数，kCallMethod 传 receiver 作为 this_val（P1 修复）

- **验证**：40/40 Array 测试通过；全量 1017/1021 通过（4 个预存遗留失败不变）；无新增 ASAN 泄漏

## 10. 2026-04-25 Phase 8.3 崩溃修复 + adversarial review 采纳

- **问题**：Phase 8.3 实现中 `elements_` 使用 `vector<Value>` 密集存储，`arr[4294967294] = x` 或 `arr.length = 4294967295` 触发 ~16GB/40GB resize 导致系统崩溃；同时 Interpreter 侧 `clear_function_properties` 未清理 `array_prototype_`，每个测试 case 均有内存泄露

- **崩溃修复**：
  - `include/qppjs/runtime/js_object.h`：`elements_` 从 `vector<Value>` 改为 `unordered_map<uint32_t, Value>` 稀疏存储，加独立 `array_length_` 字段（替代 `length_override_`）
  - `src/runtime/js_object.cpp`：`get_property("length")` 直接返回 `array_length_`；`set_property` 写入时更新 `array_length_`（无 resize）；`set_property_ex` length setter 仅截断（删除 >= new_len 的 key），不扩容；`clear_function_properties` 改为遍历 `unordered_map`
  - `src/runtime/interpreter.cpp` + `src/vm/vm.cpp`：push/pop/forEach/array literal 构建全部适配稀疏存储；`kGetElem` 快路径改用 `elements_.find`

- **内存泄露修复**：
  - `src/runtime/interpreter.cpp`：6 处 `clear_function_properties` 调用点补全 `if (array_prototype_) array_prototype_->clear_function_properties()`
  - `lsan_suppressions.txt`（新建）：屏蔽 macOS 系统库误报（`_fetchInitializingClassList`、`_libxpc_initializer`、`libSystem_initializer`、`__Balloc_D2A`、`__dtoa`）
  - `scripts/run_ut.sh`：macOS 下自动设置 `LSAN_OPTIONS=suppressions=lsan_suppressions.txt`

- **adversarial review 采纳（3/4 条）**：
  - **[high] push overflow 保护**：push 前检查 `array_length_ == UINT32_MAX`，溢出时返回 `RangeError`（interpreter.cpp + vm.cpp 两侧）
  - **[medium] is_new_call 传递**：`call_function` 签名加 `is_new_call = false` 参数；`eval_new_expr` 传 `true`，消除 Interpreter/VM native constructor 行为不一致
  - **[medium] array closure 泄露**：`Environment::clear_function_bindings` 增加 `kArray` 分支（`src/runtime/environment.cpp`），array-held closure 不再绕过循环清理
  - **[medium] forEach 稀疏语义**（不采纳）：当前对 holes 合成 `undefined` 并调用回调符合 V8 行为，且现有测试全通过，保持不变

- **新增测试**：Phase 8.3 新增 19 个测试（A-21~A-39：MaxLegalIndex、IllegalIndexAsProperty、NegativeIndexAsProperty、FractionalIndexAsProperty、NanIndexAsProperty、LengthSetToZero、LengthSetToMaxLegal、LengthSetToTooLargeThrows、PushMultiArgOrder、PopDecreasesLength、PopDeletesLastElement、ForEachNonCallableThrows、ForEachUndefinedCallbackThrows、NestedArray、OrdinaryObjectUnaffected、OrdinaryObjectNoAutoExtend、TruncatedElementsReadUndefined、PushNoArgReturnsCurrentLength、ForEachThirdArgIsArray、StringKeyLength），Interp + VM 两侧各 20 个新测试

- **验证**：80/80 Array 测试通过；全量 1054/1059 通过（5 个预存遗留失败：P3-2 闭包循环引用 × 4 + VMFinallyOverride × 1）；无新增 ASAN/LSan 泄漏

## 11. 2026-04-25 构建脚本 `--clean` 参数

- `scripts/run_ut.sh`：新增 `--clean` 参数，调用 `scripts/clean.sh` 后再执行 `build_debug.sh` 与 UT；帮助文本同步更新
- `scripts/coverage.sh`：新增 `--clean` 参数，调用 `scripts/clean.sh` 后再执行 `build_test.sh` 与覆盖率流程；支持与 `--open` 组合使用；帮助文本同步更新
- **验证**：`bash -n scripts/run_ut.sh && bash -n scripts/coverage.sh` 通过；`./scripts/run_ut.sh --help && ./scripts/coverage.sh --help` 输出正确

## 12. 2026-04-25 `run_ut.sh --quiet` 静默模式

- `scripts/run_ut.sh`：新增 `--quiet` 参数；静默执行 `build_debug.sh` 与 `ctest`，成功时完整 ctest 日志写入 `build/debug/run_ut_success.log`
- 失败时将完整 ctest 输出先写入 `build/debug/run_ut_raw.log`，再抽取失败 test block 与 `LeakSanitizer: detected memory leaks` 所在 case，写入 `build/debug/run_ut_failure.log`，最后删除 raw log
- 构建日志同样区分成功/失败：`build/run_ut_build_success.log` / `build/run_ut_build_failure.log`，避免混入 UT 失败报告
- **验证**：`bash -n scripts/run_ut.sh` 通过；`./scripts/run_ut.sh --help` 显示 `--quiet`；`./scripts/run_ut.sh --quiet` 在当前 5 个已知失败下仅输出报告路径，报告中包含失败 case 与 LSan 泄漏栈

## 14. 2026-04-26 Phase 9.1-9.5 mark-sweep GC + P3-2 修复

- **GcHeap**（`include/qppjs/runtime/gc_heap.h`、`src/runtime/gc_heap.cpp`）：新建 mark-sweep GC 核心。三阶段 Collect（reset marks → mark from roots → sweep）；MarkPending 加入 worklist；DrainWorklist 调用 TraceRefs；Sweep 三子阶段：(A) 对所有不可达对象设 kGcSentinel（防止 ClearRefs 中 release 触发 delete）、(B) 调用 ClearRefs（正常 release RcPtr 成员，kGcSentinel 使 GC 对象 release 为 no-op，非 GC 对象正常减少 ref_count）、(C) delete 所有对象
- **RcObject**（`include/qppjs/runtime/rc_object.h`）：添加 `gc_mark_`、`gc_heap_` 指针；`set_gc_sentinel()`（ref_count = kGcSentinel）；`add_ref()`/`release()` 检查 kGcSentinel；析构函数调用 `Unregister()`（RC 路径）；纯虚 `TraceRefs` 和 `ClearRefs`；RcPtr 新增 `reset_no_release()`（保留备用）
- **Environment/JSFunction/JSObject**：各自实现 `TraceRefs`（遍历所有 RcPtr 成员和 Value 中的 object）和 `ClearRefs`（正常赋值 RcPtr 为空，Value 成员赋为 undefined）
- **JSFunction 新增 is_bound_ 字段**：`is_bound_`、`bound_target_`、`bound_this_`、`bound_args_` 及 accessor；bind lambda 改为从 `self_raw` 字段读取（替代 lambda 值捕获，使 GC 能追踪 bound_target/bound_this/bound_args 中的对象引用）；TraceRefs 遍历这些字段
- **Interpreter**（`include/qppjs/runtime/interpreter.h`、`src/runtime/interpreter.cpp`）：新增 `GcHeap gc_heap_` 成员；`init_runtime()` 末尾注册 global_env_；`call_function`/`eval_block_stmt`/`eval_for_stmt`/`exec_catch` 中注册新建的 Environment；`make_function_value`/`eval_object_expr`/`eval_array_expr`/`eval_new_expr`/bind lambda 中注册新建的 JSFunction/JSObject；exec() 重构为单一 break 路径，GC 在 clear_function_bindings 之前运行，roots = 所有 interpreter 成员 + final_result
- **VM**（`include/qppjs/vm/vm.h`、`src/vm/vm.cpp`）：对称修改；kNewObject/kNewArray/kMakeFunction/kNewCall/kPushScope/bind lambda 中注册新建对象；exec() 末尾先 GC 再 clear_function_bindings
- **新增测试**：`tests/unit/gc_heap_test.cpp`（16 个测试，覆盖 Interp/VM 各 8 个：全局变量、闭包、循环引用、bind、链式 bind、对象属性函数、深层闭包链）
- **验证**：`coverage.sh --quiet` 1280/1280 通过；`run_ut.sh --quiet` 1278/1278 通过，0 个 LSan 泄露（P3-2 根本修复）

## 13. 2026-04-26 Phase 8.5 审查修复（M1/M2/S1）

- **[M1] bind + new 语义修复**（`src/runtime/interpreter.cpp`、`src/vm/vm.cpp`）：bind 生成的 native lambda 增加 `is_new_call` 参数检查；`is_new_call == true` 时忽略 `captured_this`，从 target 的 `prototype_obj()` 创建新实例，以 `is_new_call=true` 调用目标函数（Interpreter 侧走 `call_function`，VM 侧走 `push_call_frame` + `run`；native target 直接转发 `is_new_call=true`）；new 后返回值遵循 ECMAScript §10.2.2 step 9（显式返回 object 则覆盖）
- **[M2] apply array-like length 校验**（两侧对称）：读取 `len_val` 后，不是 number 视为 0；`std::isnan` 或 `<= 0` 视为 0；`> 65535` 抛 RangeError；否则 `static_cast<uint32_t>(len_num)`，消除负数/NaN/Infinity 导致的巨量内存分配崩溃
- **[S1] 链式 bind name 修复**（两侧对称）：计算 `target_name` 时先查 `target_raw->get_property("name")`，若结果 is_string 则使用，否则回退到 `target_raw->name()` 字段；修复 `fn.bind(null).bind(null).name === "bound bound fn"`
- **新增测试**：`tests/unit/function_builtin_test.cpp` 追加 10 个测试（Interp × 5 + VM × 5）：M1 × 2 + M2 × 2 + S1 × 1 各侧
- **验证**：`coverage.sh --quiet` 1213/1213 通过，无回归

## Array.prototype.join/reverse/flat/flatMap（2026-05-13）

### 目标
在 `interpreter.cpp` 和 `vm.cpp` 的 `array_prototype_` 注册区域新增 join/reverse/flat/flatMap 四个 NativeFn，Interp+VM 两侧对称实现。

### 实现要点

**join**：
- 两遍扫描：第一遍累加各段长度（String 用 `sv().size()` 避免堆拷贝），`result.reserve(total)` 后第二遍追加
- String 类型元素直接 `sv()` 取 string_view，非 String 类型走 `to_string_val`
- null/undefined 元素输出空串

**reverse**：
- 原地交换 `elements_` 条目，hole 处理三路（both/upper-only/lower-only/both-absent）
- 返回 `this_val`（原数组）

**flat**：
- 递归辅助 `flatten_into_array`（vm 侧加 `_vm` 后缀避免 ODR 冲突）
- 深度硬限制 10000 防栈溢出
- `Infinity` 深度保持为 `+Inf`，负数 depth 归零
- 结果数组 `gc_heap_.Register` + `set_proto(array_prototype_)`

**flatMap**：
- depth=1 展开：callback 返回 kArray 则展开一层，否则直接追加
- 非函数 callback 抛 TypeError
- 跳过 hole（`elements_.find` 未命中则 continue）

### 测试
- 新增 44 个测试（A-204～A-225 × Interp+VM 对称，测试文件 `tests/unit/array_test.cpp`）
- 覆盖：join 默认/自定义/空串/null-undefined/单元素/空分隔符；reverse 基本/原地/返回值/空/单元素；flat 默认深度/depth=1不展开2层/depth=2/length/depth=0/Infinity；flatMap 基本/length/标量返回/非函数TypeError/thisArg

### 验证
- `./scripts/coverage.sh --quiet`：2488/2488 通过
- `./scripts/run_ut.sh --quiet`：2488/2488 通过，0 LSan 泄漏

---

## 解构赋值（Destructuring Assignment）（2026-05-25）

### 目标
实现完整的 JS 解构赋值语法，覆盖 Interpreter + VM 两路径，对称实现。

### 主要变更

**AST（ast.h）**
- 前向声明 `struct PatternNode;`（位于 ExprNode/StmtNode 之前）
- 新增 `IdentifierPattern` / `ArrayPatternElement` / `ArrayPattern` / `ObjectPatternProperty` / `ObjectPattern` / `PatternNode` 结构体
- `PatternNode` 完整定义放在以上子结构体之后
- `DestructuringAssignmentExpression` 移至 `ExprNode` 之前（解决 variant 需要完整类型的编译问题）
- 新增 `DestructuringDeclaration`（StmtNode variant 成员）
- `ForOfStatement` 扩展 `pattern_binding` 字段（`unique_ptr<PatternNode>`）
- `ExprNode::v` variant 追加 `DestructuringAssignmentExpression`
- `StmtNode::v` variant 追加 `DestructuringDeclaration`

**Parser（parser.cpp）**
- 对象字面量 `nud(LBrace)` 扩展：shorthand `{a}`→`{a: Identifier{a}}`，shorthand+default `{a=v}`，spread `{...rest}`（key="" sentinel + SpreadElement value），关键字属性键支持
- `parse_binding_pattern()`：`[` 分支调用 `parse_array_binding_pattern()`，`{` 分支调用 `parse_object_binding_pattern()`
- `parse_array_binding_pattern()`：解析 `[a, b = 1, , ...rest]`，生成 ArrayPattern
- `parse_object_binding_pattern()`：解析 `{key, key: pat, key = default, ...rest}`，生成 ObjectPattern
- cover grammar 转换函数（非 const 引用，move 内部 unique_ptr）：`convert_array_to_pattern` / `convert_object_to_pattern` / `convert_expr_to_pattern`
- `parse_var_decl()`：检测 `[` 或 `{` 后调用 parse_binding_pattern 产生 DestructuringDeclaration
- `led(Assign)`：检测左侧为 ArrayExpression/ObjectExpression 时先计算 `left_start`，再 `convert_expr_to_pattern(left)` 产生 DestructuringAssignmentExpression
- `parse_for_stmt()`：检测 pattern_binding（`for (let/const/var [` 或 `{`）时调用 parse_binding_pattern，产生 ForOfStatement 的 pattern_binding 字段

**Opcode（opcode.h）**
- 新增 `kCopyDataProperties`（operand: n_excluded u8）

**Compiler（compiler.h + compiler.cpp）**
- 声明 `hoist_vars_scan_pattern` / `compile_destructuring_decl` / `compile_bind_pattern`
- `hoist_vars_scan_expr`：追加 DestructuringAssignmentExpression 分支
- `hoist_vars_scan_stmt`：追加 DestructuringDeclaration 分支；ForOfStatement 的 var 类型 pattern_binding 路径调用 hoist_vars_scan_pattern
- `hoist_vars_scan_pattern`：递归收集 IdentifierPattern 名称到 var_decls
- `compile_stmt`：追加 DestructuringDeclaration 分支
- `compile_expr`：追加 DestructuringAssignmentExpression 分支（compile_expr(rhs) + kDup + compile_bind_pattern(is_assign=true)）
- `compile_bind_pattern`：IdentifierPattern（SetVar/DefLet/DefConst + InitVar + Pop）；ObjectPattern（DefLet temp_var + GetVar + GetProp + [kJumpIfFalse default] + 递归 + CopyDataProperties rest）；ArrayPattern（ForOfStart + EnterTry + ForOfNext + [default] + 递归 + iter_tmp + NewArray + SpreadAppend rest + LeaveTry + IteratorClose / exception handler）
- `compile_destructuring_decl`：compile_expr(init) + compile_bind_pattern
- `compile_for_of_stmt`：has_pattern 分支（kPushScope if need_scope + compile_bind_pattern）

**Interpreter（interpreter.h + interpreter.cpp）**
- 声明 `eval_destructuring_decl` / `bind_pattern`
- `eval_stmt`：追加 DestructuringDeclaration 分支
- `eval_expr`：追加 DestructuringAssignmentExpression 分支（eval_expr(rhs) + bind_pattern(is_assign=true)，返回 rhs）
- `bind_pattern`：IdentifierPattern（bind_identifier helper：set/define+initialize）；ObjectPattern（null/undefined → TypeError，get_property + default check + 递归，rest via own_enumerable_string_keys 排除命名 key）；ArrayPattern（null/undefined → TypeError，spread_into 收集所有值，按索引绑定+default，rest 数组从剩余值）
- `eval_destructuring_decl`：eval_expr(init) + bind_pattern
- `eval_for_of_stmt` run_body lambda：追加 pattern_binding 路径（is_lexical 时创建 per-iteration scope + bind_pattern 调用）

**VM（vm.cpp）**
- `kCopyDataProperties`：read_u8(n_excluded) → 弹出 n 个 key → 弹出 src_obj → 新建 rest JSObject → 遍历 own_enumerable_string_keys 排除 excluded keys → push 结果

**ast_dump.cpp**
- `dump_expr`：追加 DestructuringAssignmentExpression 分支
- `dump_stmt`：追加 DestructuringDeclaration 分支

### 测试（destructuring_test.cpp）
40 个测试：4 Parser + 18 Interp（DS-01～DS-18）+ 18 VM（DS-01～DS-18）

### 验证
- `./build/debug/tests/qppjs_unit_tests --gtest_filter="Destructuring*"`：40/40 通过
- `./scripts/coverage.sh --quiet`：3641/3641 通过，0 LSan 泄漏
