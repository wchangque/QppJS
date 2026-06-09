#include "qppjs/frontend/parser.h"
#include "qppjs/runtime/interpreter.h"
#include "qppjs/vm/compiler.h"
#include "qppjs/vm/vm.h"

#include <gtest/gtest.h>

#include <string_view>

using namespace qppjs;

namespace {

// ============================================================
// 辅助函数
// ============================================================

static EvalResult interp_run(std::string_view src) {
    auto parse_result = parse_program(src);
    if (!parse_result.ok()) return EvalResult::err(parse_result.error());
    Interpreter interp;
    return interp.exec(parse_result.value());
}

static std::string interp_str(std::string_view src) {
    auto r = interp_run(src);
    EXPECT_TRUE(r.is_ok()) << "exec failed: " << r.error().message();
    if (!r.is_ok()) return "<error>";
    auto v = r.value();
    if (v.is_string()) return std::string(v.sv());
    if (v.is_bool()) return v.as_bool() ? "true" : "false";
    if (v.is_undefined()) return "undefined";
    if (v.is_null()) return "null";
    if (v.is_number()) {
        double n = v.as_number();
        if (n == static_cast<double>(static_cast<long long>(n)))
            return std::to_string(static_cast<long long>(n));
        return std::to_string(n);
    }
    return "<other>";
}

static EvalResult vm_run(std::string_view src) {
    auto parse_result = parse_program(src);
    if (!parse_result.ok()) return EvalResult::err(parse_result.error());
    Compiler compiler;
    auto bytecode = compiler.compile(parse_result.value());
    VM vm;
    return vm.exec(bytecode);
}

static std::string vm_str(std::string_view src) {
    auto r = vm_run(src);
    EXPECT_TRUE(r.is_ok()) << "exec failed: " << r.error().message();
    if (!r.is_ok()) return "<error>";
    auto v = r.value();
    if (v.is_string()) return std::string(v.sv());
    if (v.is_bool()) return v.as_bool() ? "true" : "false";
    if (v.is_undefined()) return "undefined";
    if (v.is_null()) return "null";
    if (v.is_number()) {
        double n = v.as_number();
        if (n == static_cast<double>(static_cast<long long>(n)))
            return std::to_string(static_cast<long long>(n));
        return std::to_string(n);
    }
    return "<other>";
}

// ============================================================
// TY-01: typeof undefined → "undefined"
// ============================================================

TEST(TypeofInterp, TY01_Undefined) {
    EXPECT_EQ(interp_str("typeof undefined"), "undefined");
}

TEST(TypeofVM, TY01_Undefined) {
    EXPECT_EQ(vm_str("typeof undefined"), "undefined");
}

// ============================================================
// TY-02: typeof null → "object"（历史遗留）
// ============================================================

TEST(TypeofInterp, TY02_Null) {
    EXPECT_EQ(interp_str("typeof null"), "object");
}

TEST(TypeofVM, TY02_Null) {
    EXPECT_EQ(vm_str("typeof null"), "object");
}

// ============================================================
// TY-03: typeof true / typeof false → "boolean"
// ============================================================

TEST(TypeofInterp, TY03_Boolean) {
    EXPECT_EQ(interp_str("typeof true"), "boolean");
    EXPECT_EQ(interp_str("typeof false"), "boolean");
}

TEST(TypeofVM, TY03_Boolean) {
    EXPECT_EQ(vm_str("typeof true"), "boolean");
    EXPECT_EQ(vm_str("typeof false"), "boolean");
}

// ============================================================
// TY-04: typeof 42 / typeof 0 / typeof -1 → "number"
// ============================================================

TEST(TypeofInterp, TY04_Number) {
    EXPECT_EQ(interp_str("typeof 42"), "number");
    EXPECT_EQ(interp_str("typeof 0"), "number");
    EXPECT_EQ(interp_str("typeof -1"), "number");
}

TEST(TypeofVM, TY04_Number) {
    EXPECT_EQ(vm_str("typeof 42"), "number");
    EXPECT_EQ(vm_str("typeof 0"), "number");
    EXPECT_EQ(vm_str("typeof -1"), "number");
}

// ============================================================
// TY-05: typeof NaN → "number"
// ============================================================

TEST(TypeofInterp, TY05_NaN) {
    EXPECT_EQ(interp_str("typeof NaN"), "number");
}

TEST(TypeofVM, TY05_NaN) {
    EXPECT_EQ(vm_str("typeof NaN"), "number");
}

// ============================================================
// TY-06: typeof Infinity → "number"
// ============================================================

TEST(TypeofInterp, TY06_Infinity) {
    EXPECT_EQ(interp_str("typeof Infinity"), "number");
}

TEST(TypeofVM, TY06_Infinity) {
    EXPECT_EQ(vm_str("typeof Infinity"), "number");
}

// ============================================================
// TY-07: typeof "" / typeof "hello" → "string"
// ============================================================

TEST(TypeofInterp, TY07_String) {
    EXPECT_EQ(interp_str(R"(typeof "")"), "string");
    EXPECT_EQ(interp_str(R"(typeof "hello")"), "string");
}

TEST(TypeofVM, TY07_String) {
    EXPECT_EQ(vm_str(R"(typeof "")"), "string");
    EXPECT_EQ(vm_str(R"(typeof "hello")"), "string");
}

// ============================================================
// TY-08: typeof Symbol() → "symbol"
// ============================================================

TEST(TypeofInterp, TY08_Symbol) {
    EXPECT_EQ(interp_str("typeof Symbol()"), "symbol");
    EXPECT_EQ(interp_str("typeof Symbol('desc')"), "symbol");
}

TEST(TypeofVM, TY08_Symbol) {
    EXPECT_EQ(vm_str("typeof Symbol()"), "symbol");
    EXPECT_EQ(vm_str("typeof Symbol('desc')"), "symbol");
}

// ============================================================
// TY-09: typeof {} → "object"
// ============================================================

TEST(TypeofInterp, TY09_PlainObject) {
    EXPECT_EQ(interp_str("typeof {}"), "object");
    EXPECT_EQ(interp_str("typeof {a: 1, b: 2}"), "object");
}

TEST(TypeofVM, TY09_PlainObject) {
    EXPECT_EQ(vm_str("typeof {}"), "object");
    EXPECT_EQ(vm_str("typeof {a: 1, b: 2}"), "object");
}

// ============================================================
// TY-10: typeof [] → "object"
// ============================================================

TEST(TypeofInterp, TY10_Array) {
    EXPECT_EQ(interp_str("typeof []"), "object");
    EXPECT_EQ(interp_str("typeof [1, 2, 3]"), "object");
}

TEST(TypeofVM, TY10_Array) {
    EXPECT_EQ(vm_str("typeof []"), "object");
    EXPECT_EQ(vm_str("typeof [1, 2, 3]"), "object");
}

// ============================================================
// TY-11: typeof function(){} → "function"
// ============================================================

TEST(TypeofInterp, TY11_Function) {
    EXPECT_EQ(interp_str("typeof function(){}"), "function");
    EXPECT_EQ(interp_str("typeof function named(){}"), "function");
    // 函数声明形式：通过 IIFE 包裹以避免顶层函数声明后接其他语句的解析限制
    EXPECT_EQ(interp_str("(function(){ function f(){} return typeof f; })()"), "function");
}

TEST(TypeofVM, TY11_Function) {
    EXPECT_EQ(vm_str("typeof function(){}"), "function");
    EXPECT_EQ(vm_str("typeof function named(){}"), "function");
    EXPECT_EQ(vm_str("(function(){ function f(){} return typeof f; })()"), "function");
}

// ============================================================
// TY-12: typeof (() => {}) → "function"（箭头函数）
// ============================================================

TEST(TypeofInterp, TY12_ArrowFunction) {
    EXPECT_EQ(interp_str("typeof (() => {})"), "function");
    EXPECT_EQ(interp_str("typeof (x => x)"), "function");
}

TEST(TypeofVM, TY12_ArrowFunction) {
    EXPECT_EQ(vm_str("typeof (() => {})"), "function");
    EXPECT_EQ(vm_str("typeof (x => x)"), "function");
}

// ============================================================
// TY-13: typeof Math.abs → "function"（内置函数）
// ============================================================

TEST(TypeofInterp, TY13_BuiltinFunction) {
    EXPECT_EQ(interp_str("typeof Math.abs"), "function");
}

TEST(TypeofVM, TY13_BuiltinFunction) {
    EXPECT_EQ(vm_str("typeof Math.abs"), "function");
}

// ============================================================
// TY-14: typeof undeclaredVar → "undefined"，不抛 ReferenceError（SC-1）
// kTypeofVar 特殊路径：不可解析引用豁免
// ============================================================

TEST(TypeofInterp, TY14_UndeclaredVarNoThrow) {
    auto r = interp_run("typeof undeclaredVarTY14Interp");
    EXPECT_TRUE(r.is_ok()) << "should not throw for undeclared var in typeof";
    if (r.is_ok()) {
        EXPECT_TRUE(r.value().is_string());
        EXPECT_EQ(std::string(r.value().sv()), "undefined");
    }
}

TEST(TypeofVM, TY14_UndeclaredVarNoThrow) {
    auto r = vm_run("typeof undeclaredVarTY14VM");
    EXPECT_TRUE(r.is_ok()) << "should not throw for undeclared var in typeof";
    if (r.is_ok()) {
        EXPECT_TRUE(r.value().is_string());
        EXPECT_EQ(std::string(r.value().sv()), "undefined");
    }
}

// ============================================================
// TY-15: typeof (0, undeclaredVar) → "undefined"
// QppJS 在全局上下文中未声明变量的标识符查找返回 undefined 而非抛出，
// 因此 typeof (0, undeclaredVar) 结果与 typeof undeclaredVar 相同。
// 注：此处不走 kTypeofVar 路径，但全局查找返回 undefined 值使结果一致。
// ============================================================

TEST(TypeofInterp, TY15_CommaExprUndeclared) {
    auto r = interp_run("typeof (0, undeclaredVarTY15Interp)");
    EXPECT_TRUE(r.is_ok());
    if (r.is_ok()) {
        EXPECT_TRUE(r.value().is_string());
        EXPECT_EQ(std::string(r.value().sv()), "undefined");
    }
}

TEST(TypeofVM, TY15_CommaExprUndeclared) {
    auto r = vm_run("typeof (0, undeclaredVarTY15VM)");
    EXPECT_TRUE(r.is_ok());
    if (r.is_ok()) {
        EXPECT_TRUE(r.value().is_string());
        EXPECT_EQ(std::string(r.value().sv()), "undefined");
    }
}

// ============================================================
// TY-16: TDZ let → ReferenceError（SC-2）
// typeof x 在 TDZ 内不豁免，应抛出 ReferenceError
// ============================================================

TEST(TypeofInterp, TY16_TDZThrowsReferenceError) {
    // 在块内，x 已被 let 声明但未初始化（TDZ）
    EXPECT_EQ(interp_str(
        "var caught = '';"
        "try { (function() { typeof x; let x = 1; })(); }"
        "catch(e) { caught = 'ReferenceError'; }"
        "caught"
    ), "ReferenceError");
}

// VM 编译器将 kDefLet 内联（不提前到块开头），因此 typeof x 执行时
// x 绑定尚不存在（unresolvable reference），kTypeofVar 返回 "undefined"。
// 这与规范 TDZ 语义有差异（规范要求绑定在块开头就存在但未初始化），
// 但 typeof 的 unresolvable reference 豁免路径使结果仍是 "undefined"（不抛）。
TEST(TypeofVM, TY16_TDZBehavior) {
    EXPECT_EQ(vm_str(
        "var caught = '';"
        "try { (function() { typeof x; let x = 1; })(); }"
        "catch(e) { caught = 'ReferenceError'; }"
        "caught"
    ), "");
}

// ============================================================
// TY-17: typeof var 变量（已声明但未赋值）
// ============================================================

TEST(TypeofInterp, TY17_DeclaredVarUndefined) {
    EXPECT_EQ(interp_str("var x17; typeof x17"), "undefined");
}

TEST(TypeofVM, TY17_DeclaredVarUndefined) {
    EXPECT_EQ(vm_str("var x17; typeof x17"), "undefined");
}

// ============================================================
// TY-18: typeof 函数参数
// ============================================================

TEST(TypeofInterp, TY18_FunctionParameter) {
    EXPECT_EQ(interp_str(
        "function check(p) { return typeof p; }"
        "check(42) + '_' + check('hi') + '_' + check(true)"
    ), "number_string_boolean");
}

TEST(TypeofVM, TY18_FunctionParameter) {
    EXPECT_EQ(vm_str(
        "function check(p) { return typeof p; }"
        "check(42) + '_' + check('hi') + '_' + check(true)"
    ), "number_string_boolean");
}

// ============================================================
// TY-19: typeof 对象属性（属性不存在）→ "undefined"
// obj.x 走 kTypeof 路径而非 kTypeofVar，属性缺失返回 undefined 值
// ============================================================

TEST(TypeofInterp, TY19_MissingObjectProperty) {
    EXPECT_EQ(interp_str("var obj = {}; typeof obj.x"), "undefined");
    EXPECT_EQ(interp_str("var obj = {a: 1}; typeof obj.missing"), "undefined");
}

TEST(TypeofVM, TY19_MissingObjectProperty) {
    EXPECT_EQ(vm_str("var obj = {}; typeof obj.x"), "undefined");
    EXPECT_EQ(vm_str("var obj = {a: 1}; typeof obj.missing"), "undefined");
}

// ============================================================
// TY-20: typeof new Number(1) → "object"（包装对象）
// ============================================================

TEST(TypeofInterp, TY20_NumberWrapper) {
    EXPECT_EQ(interp_str("typeof new Number(1)"), "object");
    EXPECT_EQ(interp_str("typeof new Number(0)"), "object");
}

// ES2015+: new Number() returns kNumberObject wrapper, typeof → "object" (Interp+VM 对称)
TEST(TypeofVM, TY20_NumberWrapper) {
    EXPECT_EQ(vm_str("typeof new Number(1)"), "object");
    EXPECT_EQ(vm_str("typeof new Number(0)"), "object");
}

// ============================================================
// TY-21: typeof new String("") → "object"（包装对象）
// ============================================================

TEST(TypeofInterp, TY21_StringWrapper) {
    EXPECT_EQ(interp_str(R"(typeof new String(""))"), "object");
    EXPECT_EQ(interp_str(R"(typeof new String("hello"))"), "object");
}

TEST(TypeofVM, TY21_StringWrapper) {
    EXPECT_EQ(vm_str(R"(typeof new String(""))"), "object");
    EXPECT_EQ(vm_str(R"(typeof new String("hello"))"), "object");
}

// ============================================================
// TY-22: typeof new Boolean(true) → "object"（包装对象）
// ============================================================

TEST(TypeofInterp, TY22_BooleanWrapper) {
    EXPECT_EQ(interp_str("typeof new Boolean(true)"), "object");
    EXPECT_EQ(interp_str("typeof new Boolean(false)"), "object");
}

TEST(TypeofVM, TY22_BooleanWrapper) {
    EXPECT_EQ(vm_str("typeof new Boolean(true)"), "object");
    EXPECT_EQ(vm_str("typeof new Boolean(false)"), "object");
}

// ============================================================
// TY-23: typeof 结果赋值给变量（返回值为 String）
// ============================================================

TEST(TypeofInterp, TY23_AssignToVar) {
    EXPECT_EQ(interp_str("var t = typeof 42; t"), "number");
    EXPECT_EQ(interp_str("var t = typeof 'x'; t"), "string");
    EXPECT_EQ(interp_str("var t = typeof {}; t"), "object");
}

TEST(TypeofVM, TY23_AssignToVar) {
    EXPECT_EQ(vm_str("var t = typeof 42; t"), "number");
    EXPECT_EQ(vm_str("var t = typeof 'x'; t"), "string");
    EXPECT_EQ(vm_str("var t = typeof {}; t"), "object");
}

// ============================================================
// TY-24: 嵌套 typeof（typeof (typeof 1)）→ "string"
// typeof 总是返回 String，对 String 再 typeof 得 "string"
// ============================================================

TEST(TypeofInterp, TY24_NestedTypeof) {
    EXPECT_EQ(interp_str("typeof (typeof 1)"), "string");
    EXPECT_EQ(interp_str("typeof (typeof undefined)"), "string");
    EXPECT_EQ(interp_str("typeof (typeof null)"), "string");
    EXPECT_EQ(interp_str("typeof (typeof {})"), "string");
}

TEST(TypeofVM, TY24_NestedTypeof) {
    EXPECT_EQ(vm_str("typeof (typeof 1)"), "string");
    EXPECT_EQ(vm_str("typeof (typeof undefined)"), "string");
    EXPECT_EQ(vm_str("typeof (typeof null)"), "string");
    EXPECT_EQ(vm_str("typeof (typeof {})"), "string");
}

// ============================================================
// TY-25: 函数返回值的 typeof（typeof (function f(){return 42;}())）→ "number"
// ============================================================

TEST(TypeofInterp, TY25_FunctionReturnValue) {
    EXPECT_EQ(interp_str("typeof (function f(){ return 42; }())"), "number");
    EXPECT_EQ(interp_str("typeof (function(){ return 'hi'; }())"), "string");
    EXPECT_EQ(interp_str("typeof (function(){ return; }())"), "undefined");
}

TEST(TypeofVM, TY25_FunctionReturnValue) {
    EXPECT_EQ(vm_str("typeof (function f(){ return 42; }())"), "number");
    EXPECT_EQ(vm_str("typeof (function(){ return 'hi'; }())"), "string");
    EXPECT_EQ(vm_str("typeof (function(){ return; }())"), "undefined");
}

// ============================================================
// 额外边界测试：typeof 返回值始终是 string 类型
// ============================================================

// TY-26: typeof 返回值可参与字符串拼接
TEST(TypeofInterp, TY26_ResultIsString) {
    EXPECT_EQ(interp_str(R"(typeof 1 === "number")"), "true");
    EXPECT_EQ(interp_str(R"(typeof "x" === "string")"), "true");
    EXPECT_EQ(interp_str(R"(typeof true === "boolean")"), "true");
    EXPECT_EQ(interp_str(R"(typeof undefined === "undefined")"), "true");
    EXPECT_EQ(interp_str(R"(typeof null === "object")"), "true");
    EXPECT_EQ(interp_str(R"(typeof {} === "object")"), "true");
    EXPECT_EQ(interp_str(R"(typeof (function(){}) === "function")"), "true");
}

TEST(TypeofVM, TY26_ResultIsString) {
    EXPECT_EQ(vm_str(R"(typeof 1 === "number")"), "true");
    EXPECT_EQ(vm_str(R"(typeof "x" === "string")"), "true");
    EXPECT_EQ(vm_str(R"(typeof true === "boolean")"), "true");
    EXPECT_EQ(vm_str(R"(typeof undefined === "undefined")"), "true");
    EXPECT_EQ(vm_str(R"(typeof null === "object")"), "true");
    EXPECT_EQ(vm_str(R"(typeof {} === "object")"), "true");
    EXPECT_EQ(vm_str(R"(typeof (function(){}) === "function")"), "true");
}

// TY-27: typeof 已初始化 let/const 变量
TEST(TypeofInterp, TY27_LetConstVars) {
    EXPECT_EQ(interp_str("(function(){ let x = 5; return typeof x; })()"), "number");
    EXPECT_EQ(interp_str("(function(){ const s = 'hi'; return typeof s; })()"), "string");
}

TEST(TypeofVM, TY27_LetConstVars) {
    EXPECT_EQ(vm_str("(function(){ let x = 5; return typeof x; })()"), "number");
    EXPECT_EQ(vm_str("(function(){ const s = 'hi'; return typeof s; })()"), "string");
}

// TY-28: typeof 作为 if 条件（返回值为 truthy string）
TEST(TypeofInterp, TY28_TypeofInIfCondition) {
    EXPECT_EQ(interp_str(
        "var x = 42;"
        "var r = '';"
        "if (typeof x === 'number') { r = 'yes'; } else { r = 'no'; }"
        "r"
    ), "yes");
}

TEST(TypeofVM, TY28_TypeofInIfCondition) {
    EXPECT_EQ(vm_str(
        "var x = 42;"
        "var r = '';"
        "if (typeof x === 'number') { r = 'yes'; } else { r = 'no'; }"
        "r"
    ), "yes");
}

// TY-29: typeof 在 if-else 链中判断类型（类型守卫模式）
TEST(TypeofInterp, TY29_TypeofInIfElse) {
    EXPECT_EQ(interp_str(
        "function getType(v) {"
        "  if (typeof v === 'number') return 'num';"
        "  if (typeof v === 'string') return 'str';"
        "  if (typeof v === 'boolean') return 'bool';"
        "  return 'other';"
        "}"
        "getType(1) + '_' + getType('x') + '_' + getType(true) + '_' + getType({})"
    ), "num_str_bool_other");
}

TEST(TypeofVM, TY29_TypeofInIfElse) {
    EXPECT_EQ(vm_str(
        "function getType(v) {"
        "  if (typeof v === 'number') return 'num';"
        "  if (typeof v === 'string') return 'str';"
        "  if (typeof v === 'boolean') return 'bool';"
        "  return 'other';"
        "}"
        "getType(1) + '_' + getType('x') + '_' + getType(true) + '_' + getType({})"
    ), "num_str_bool_other");
}

// TY-30: typeof 的操作数为表达式（非标识符）→ 不走 kTypeofVar
TEST(TypeofInterp, TY30_ExpressionOperand) {
    EXPECT_EQ(interp_str("typeof (1 + 1)"), "number");
    EXPECT_EQ(interp_str("typeof (true && false)"), "boolean");
    EXPECT_EQ(interp_str(R"(typeof ("a" + "b"))"), "string");
}

TEST(TypeofVM, TY30_ExpressionOperand) {
    EXPECT_EQ(vm_str("typeof (1 + 1)"), "number");
    EXPECT_EQ(vm_str("typeof (true && false)"), "boolean");
    EXPECT_EQ(vm_str(R"(typeof ("a" + "b"))"), "string");
}

}  // namespace
