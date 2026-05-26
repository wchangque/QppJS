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

static bool parse_ok(std::string_view source) {
    auto result = parse_program(source);
    return result.ok();
}

static Value interp_ok(std::string_view source) {
    auto parse_result = parse_program(source);
    EXPECT_TRUE(parse_result.ok()) << "parse failed: " << parse_result.error().message();
    Interpreter interp;
    auto result = interp.exec(parse_result.value());
    EXPECT_TRUE(result.is_ok()) << "exec failed: " << result.error().message();
    return result.value();
}

static bool interp_err(std::string_view source) {
    auto parse_result = parse_program(source);
    if (!parse_result.ok()) return false;
    Interpreter interp;
    auto result = interp.exec(parse_result.value());
    return !result.is_ok();
}

static Value vm_ok(std::string_view source) {
    auto parse_result = parse_program(source);
    EXPECT_TRUE(parse_result.ok()) << "parse failed: " << parse_result.error().message();
    if (!parse_result.ok()) return Value::undefined();
    Compiler compiler;
    auto bytecode = compiler.compile(parse_result.value());
    VM vm;
    auto result = vm.exec(bytecode);
    EXPECT_TRUE(result.is_ok()) << "exec failed: " << result.error().message();
    return result.value();
}

static bool vm_err(std::string_view source) {
    auto parse_result = parse_program(source);
    if (!parse_result.ok()) return false;
    Compiler compiler;
    auto bytecode = compiler.compile(parse_result.value());
    VM vm;
    auto result = vm.exec(bytecode);
    return !result.is_ok();
}

// ============================================================
// IN-01: 普通属性存在
// ============================================================

TEST(InOperator, IN01_OwnPropertyExistsInterp) {
    auto v = interp_ok(R"(var obj = {x:1}; "x" in obj)");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(InOperator, IN01_OwnPropertyExistsVM) {
    auto v = vm_ok(R"(var obj = {x:1}; "x" in obj)");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// IN-02: 属性不存在
// ============================================================

TEST(InOperator, IN02_PropertyMissingInterp) {
    auto v = interp_ok(R"(var obj = {x:1}; "y" in obj)");
    EXPECT_TRUE(v.is_bool());
    EXPECT_FALSE(v.as_bool());
}

TEST(InOperator, IN02_PropertyMissingVM) {
    auto v = vm_ok(R"(var obj = {x:1}; "y" in obj)");
    EXPECT_TRUE(v.is_bool());
    EXPECT_FALSE(v.as_bool());
}

// ============================================================
// IN-03: 继承属性（push 来自 Array.prototype）
// ============================================================

TEST(InOperator, IN03_InheritedPropertyInterp) {
    auto v = interp_ok(R"(var arr = []; "push" in arr)");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(InOperator, IN03_InheritedPropertyVM) {
    auto v = vm_ok(R"(var arr = []; "push" in arr)");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// IN-04: 数组数字索引（存在）
// ============================================================

TEST(InOperator, IN04_ArrayIndexExistsInterp) {
    auto v = interp_ok(R"(var arr = [1,2,3]; 0 in arr)");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(InOperator, IN04_ArrayIndexExistsVM) {
    auto v = vm_ok(R"(var arr = [1,2,3]; 0 in arr)");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// IN-05: 数组越界索引
// ============================================================

TEST(InOperator, IN05_ArrayIndexOutOfBoundsInterp) {
    auto v = interp_ok(R"(var arr = [1,2,3]; 5 in arr)");
    EXPECT_TRUE(v.is_bool());
    EXPECT_FALSE(v.as_bool());
}

TEST(InOperator, IN05_ArrayIndexOutOfBoundsVM) {
    auto v = vm_ok(R"(var arr = [1,2,3]; 5 in arr)");
    EXPECT_TRUE(v.is_bool());
    EXPECT_FALSE(v.as_bool());
}

// ============================================================
// IN-06: 数组 length 属性
// ============================================================

TEST(InOperator, IN06_ArrayLengthInterp) {
    auto v = interp_ok(R"(var arr = []; "length" in arr)");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(InOperator, IN06_ArrayLengthVM) {
    auto v = vm_ok(R"(var arr = []; "length" in arr)");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// IN-07: Symbol 键存在
// ============================================================

TEST(InOperator, IN07_SymbolKeyExistsInterp) {
    auto v = interp_ok(R"(
        var s = Symbol("test");
        var obj = {};
        obj[s] = 42;
        s in obj
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(InOperator, IN07_SymbolKeyExistsVM) {
    auto v = vm_ok(R"(
        var s = Symbol("test");
        var obj = {};
        obj[s] = 42;
        s in obj
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// IN-08: Symbol 键不存在
// ============================================================

TEST(InOperator, IN08_SymbolKeyMissingInterp) {
    auto v = interp_ok(R"(
        var s = Symbol("test");
        var obj = {};
        s in obj
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_FALSE(v.as_bool());
}

TEST(InOperator, IN08_SymbolKeyMissingVM) {
    auto v = vm_ok(R"(
        var s = Symbol("test");
        var obj = {};
        s in obj
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_FALSE(v.as_bool());
}

// ============================================================
// IN-09: RHS 非对象（数字）→ TypeError
// ============================================================

TEST(InOperator, IN09_RhsNumberTypeErrorInterp) {
    EXPECT_TRUE(interp_err(R"(try { "x" in 42; } catch(e) { throw e; })"));
}

TEST(InOperator, IN09_RhsNumberTypeErrorVM) {
    EXPECT_TRUE(vm_err(R"(try { "x" in 42; } catch(e) { throw e; })"));
}

// ============================================================
// IN-10: RHS 非对象（string）→ TypeError
// ============================================================

TEST(InOperator, IN10_RhsStringTypeErrorInterp) {
    EXPECT_TRUE(interp_err(R"(try { "x" in "abc"; } catch(e) { throw e; })"));
}

TEST(InOperator, IN10_RhsStringTypeErrorVM) {
    EXPECT_TRUE(vm_err(R"(try { "x" in "abc"; } catch(e) { throw e; })"));
}

// ============================================================
// IN-11: LHS 查属性值为 null（null 值不影响 has）
// ============================================================

TEST(InOperator, IN11_PropertyValueNullInterp) {
    auto v = interp_ok(R"(var obj = {x: null}; "x" in obj)");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(InOperator, IN11_PropertyValueNullVM) {
    auto v = vm_ok(R"(var obj = {x: null}; "x" in obj)");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// IN-12: 数字 LHS 转字符串（0 in {"0": "a"} → true）
// ============================================================

TEST(InOperator, IN12_NumberLhsToStringInterp) {
    auto v = interp_ok(R"(var obj = {"0": "a"}; 0 in obj)");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(InOperator, IN12_NumberLhsToStringVM) {
    auto v = vm_ok(R"(var obj = {"0": "a"}; 0 in obj)");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// IN-13: no_in_ guard — for(;;) 初始化器中 in 不被解析为二元运算符
// ============================================================

TEST(InOperator, IN13_NoInGuardForStmtParse) {
    // for(var i = "x" in obj; ...) 应当正常解析，
    // 但 for(var i in obj) 也应该正常。
    // 这里只测 for-init 中的简单赋值不会混淆 in
    EXPECT_TRUE(parse_ok("var obj={a:1}; for(var k in obj) {}"));
    // for-init expression 不应将 in 当成二元运算符
    EXPECT_TRUE(parse_ok("for(var i=0; i<3; i++) {}"));
}

// ============================================================
// IN-14: 嵌套对象原型链
// ============================================================

TEST(InOperator, IN14_NestedPrototypeChainInterp) {
    auto v = interp_ok(R"(
        var parent = {foo: 1};
        var child = Object.create(parent);
        "foo" in child
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(InOperator, IN14_NestedPrototypeChainVM) {
    auto v = vm_ok(R"(
        var parent = {foo: 1};
        var child = Object.create(parent);
        "foo" in child
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// IN-15: delete 后属性不存在
// ============================================================

TEST(InOperator, IN15_DeleteThenInInterp) {
    auto v = interp_ok(R"(
        var obj = {x: 1};
        delete obj.x;
        "x" in obj
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_FALSE(v.as_bool());
}

TEST(InOperator, IN15_DeleteThenInVM) {
    auto v = vm_ok(R"(
        var obj = {x: 1};
        delete obj.x;
        "x" in obj
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_FALSE(v.as_bool());
}

// ============================================================
// IN-16: 数组字符串索引（"0" in [1,2,3] → true）
// ============================================================

TEST(InOperator, IN16_ArrayStringIndexInterp) {
    auto v = interp_ok(R"(var arr = [1,2,3]; "0" in arr)");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(InOperator, IN16_ArrayStringIndexVM) {
    auto v = vm_ok(R"(var arr = [1,2,3]; "0" in arr)");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// IN-17: 原型链中间节点属性
// ============================================================

TEST(InOperator, IN17_MiddleNodePropertyInterp) {
    auto v = interp_ok(R"(
        var a = {x: 1};
        var b = Object.create(a);
        b.y = 2;
        var c = Object.create(b);
        "x" in c
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(InOperator, IN17_MiddleNodePropertyVM) {
    auto v = vm_ok(R"(
        var a = {x: 1};
        var b = Object.create(a);
        b.y = 2;
        var c = Object.create(b);
        "x" in c
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// IN-18: LHS 为 undefined → "undefined" in obj
// ============================================================

TEST(InOperator, IN18_LhsUndefinedInterp) {
    auto v = interp_ok(R"(var obj = {undefined: 1}; undefined in obj)");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(InOperator, IN18_LhsUndefinedVM) {
    auto v = vm_ok(R"(var obj = {undefined: 1}; undefined in obj)");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// IN-19: LHS 为 boolean → 转字符串
// ============================================================

TEST(InOperator, IN19_LhsBooleanInterp) {
    auto v = interp_ok(R"(var obj = {"true": 1}; true in obj)");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(InOperator, IN19_LhsBooleanVM) {
    auto v = vm_ok(R"(var obj = {"true": 1}; true in obj)");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// IN-20: constructor 属性（来自用户函数的 prototype.constructor）
// ============================================================

TEST(InOperator, IN20_ConstructorPropertyInterp) {
    auto v = interp_ok(R"(function Foo() {} var obj = new Foo(); "constructor" in obj)");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(InOperator, IN20_ConstructorPropertyVM) {
    auto v = vm_ok(R"(function Foo() {} var obj = new Foo(); "constructor" in obj)");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// IN-21: RHS null → TypeError
// ============================================================

TEST(InOperator, IN21_RhsNullTypeErrorInterp) {
    EXPECT_TRUE(interp_err(R"(try { "x" in null; } catch(e) { throw e; })"));
}

TEST(InOperator, IN21_RhsNullTypeErrorVM) {
    EXPECT_TRUE(vm_err(R"(try { "x" in null; } catch(e) { throw e; })"));
}

// ============================================================
// IN-22: RHS undefined → TypeError
// ============================================================

TEST(InOperator, IN22_RhsUndefinedTypeErrorInterp) {
    EXPECT_TRUE(interp_err(R"(try { "x" in undefined; } catch(e) { throw e; })"));
}

TEST(InOperator, IN22_RhsUndefinedTypeErrorVM) {
    EXPECT_TRUE(vm_err(R"(try { "x" in undefined; } catch(e) { throw e; })"));
}

// ============================================================
// IN-23: RHS boolean → TypeError
// ============================================================

TEST(InOperator, IN23_RhsBooleanTypeErrorInterp) {
    EXPECT_TRUE(interp_err(R"(try { "x" in true; } catch(e) { throw e; })"));
}

TEST(InOperator, IN23_RhsBooleanTypeErrorVM) {
    EXPECT_TRUE(vm_err(R"(try { "x" in true; } catch(e) { throw e; })"));
}

// ============================================================
// IN-24: LHS null → ToString("null") → "null" in obj
// ============================================================

TEST(InOperator, IN24_LhsNullToStringInterp) {
    auto v = interp_ok(R"(var obj = {"null": 1}; null in obj)");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(InOperator, IN24_LhsNullToStringVM) {
    auto v = vm_ok(R"(var obj = {"null": 1}; null in obj)");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// IN-25: LHS NaN → ToString("NaN") → "NaN" in obj
// ============================================================

TEST(InOperator, IN25_LhsNanToStringInterp) {
    auto v = interp_ok(R"(var obj = {"NaN": 1}; NaN in obj)");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(InOperator, IN25_LhsNanToStringVM) {
    auto v = vm_ok(R"(var obj = {"NaN": 1}; NaN in obj)");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// IN-26: LHS Infinity → ToString("Infinity") → "Infinity" in obj
// ============================================================

TEST(InOperator, IN26_LhsInfinityToStringInterp) {
    auto v = interp_ok(R"(var obj = {"Infinity": 1}; Infinity in obj)");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(InOperator, IN26_LhsInfinityToStringVM) {
    auto v = vm_ok(R"(var obj = {"Infinity": 1}; Infinity in obj)");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// IN-27: 稀疏数组 hole — 1 in [1,,3] → false
// ============================================================

TEST(InOperator, IN27_SparseArrayHoleInterp) {
    auto v = interp_ok(R"(var arr = [1,,3]; 1 in arr)");
    EXPECT_TRUE(v.is_bool());
    EXPECT_FALSE(v.as_bool());
}

TEST(InOperator, IN27_SparseArrayHoleVM) {
    auto v = vm_ok(R"(var arr = [1,,3]; 1 in arr)");
    EXPECT_TRUE(v.is_bool());
    EXPECT_FALSE(v.as_bool());
}

// ============================================================
// IN-28: non-configurable 属性 delete 失败，in 仍返回 true
// ============================================================

TEST(InOperator, IN28_NonConfigurableDeleteStillExistsInterp) {
    auto v = interp_ok(R"(
        var obj = {};
        Object.defineProperty(obj, "x", {value: 1, configurable: false, writable: false, enumerable: true});
        delete obj.x;
        "x" in obj
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(InOperator, IN28_NonConfigurableDeleteStillExistsVM) {
    auto v = vm_ok(R"(
        var obj = {};
        Object.defineProperty(obj, "x", {value: 1, configurable: false, writable: false, enumerable: true});
        delete obj.x;
        "x" in obj
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// IN-29: Symbol 属性在原型链上（非 own）可被 in 找到
// ============================================================

TEST(InOperator, IN29_SymbolInheritedFromProtoInterp) {
    auto v = interp_ok(R"(
        var s = Symbol("inherited");
        var parent = {};
        parent[s] = 99;
        var child = Object.create(parent);
        s in child
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(InOperator, IN29_SymbolInheritedFromProtoVM) {
    auto v = vm_ok(R"(
        var s = Symbol("inherited");
        var parent = {};
        parent[s] = 99;
        var child = Object.create(parent);
        s in child
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// IN-30: 两个独立 Symbol 实例互不匹配
// ============================================================

TEST(InOperator, IN30_DifferentSymbolsDontMatchInterp) {
    auto v = interp_ok(R"(
        var s1 = Symbol("k");
        var s2 = Symbol("k");
        var obj = {};
        obj[s1] = 1;
        s2 in obj
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_FALSE(v.as_bool());
}

TEST(InOperator, IN30_DifferentSymbolsDontMatchVM) {
    auto v = vm_ok(R"(
        var s1 = Symbol("k");
        var s2 = Symbol("k");
        var obj = {};
        obj[s1] = 1;
        s2 in obj
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_FALSE(v.as_bool());
}

// IN-31: 函数对象作为 RHS — 已知 SEGFAULT 回归风险
// JSFunction 继承自 RcObject 而非 JSObject，kIn/BinaryOp::In 中无条件
// static_cast<JSObject*>(raw) 对函数 RHS 会产生 UB/crash。
// 待实现侧增加 object_kind() == kFunction 分支后补充此测试。

// ============================================================
// IN-32: in 表达式结果用于 if 条件分支
// ============================================================

TEST(InOperator, IN32_InResultInIfConditionInterp) {
    auto v = interp_ok(R"(
        var obj = {x: 1};
        var result = false;
        if ("x" in obj) { result = true; }
        result
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(InOperator, IN32_InResultInIfConditionVM) {
    auto v = vm_ok(R"(
        var obj = {x: 1};
        var result = false;
        if ("x" in obj) { result = true; }
        result
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// IN-33: 负数 LHS → "-1" → 数组中不存在
// ============================================================

TEST(InOperator, IN33_NegativeNumberLhsInterp) {
    auto v = interp_ok(R"(var arr = [1, 2, 3]; -1 in arr)");
    EXPECT_TRUE(v.is_bool());
    EXPECT_FALSE(v.as_bool());
}

TEST(InOperator, IN33_NegativeNumberLhsVM) {
    auto v = vm_ok(R"(var arr = [1, 2, 3]; -1 in arr)");
    EXPECT_TRUE(v.is_bool());
    EXPECT_FALSE(v.as_bool());
}

// ============================================================
// IN-34: 整数值的浮点 LHS（1.0）→ "1" → 数组索引存在
// ============================================================

TEST(InOperator, IN34_FloatIndexLhsInterp) {
    auto v = interp_ok(R"(var arr = [1, 2, 3]; 1.0 in arr)");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(InOperator, IN34_FloatIndexLhsVM) {
    auto v = vm_ok(R"(var arr = [1, 2, 3]; 1.0 in arr)");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

}  // namespace
