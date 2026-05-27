#include "qppjs/frontend/parser.h"
#include "qppjs/runtime/interpreter.h"
#include "qppjs/vm/compiler.h"
#include "qppjs/vm/vm.h"

#include <gtest/gtest.h>

#include <cmath>
#include <string_view>

using namespace qppjs;

namespace {

static EvalResult interp_run(std::string_view src) {
    auto pr = parse_program(src);
    if (!pr.ok()) return EvalResult::err(pr.error());
    Interpreter interp;
    return interp.exec(pr.value());
}

static Value interp_ok(std::string_view src) {
    auto r = interp_run(src);
    EXPECT_TRUE(r.is_ok()) << "interp failed: " << r.error().message();
    return r.is_ok() ? r.value() : Value::undefined();
}

static EvalResult vm_run(std::string_view src) {
    auto pr = parse_program(src);
    if (!pr.ok()) return EvalResult::err(pr.error());
    Compiler compiler;
    auto bc = compiler.compile(pr.value());
    VM vm;
    return vm.exec(bc);
}

static Value vm_ok(std::string_view src) {
    auto r = vm_run(src);
    EXPECT_TRUE(r.is_ok()) << "vm failed: " << r.error().message();
    return r.is_ok() ? r.value() : Value::undefined();
}

static bool parse_fails(std::string_view src) {
    return !parse_program(src).ok();
}

// ============================================================
// NC-01: null ?? "default" → "default"
// ============================================================

TEST(NullishCoalescing, NC01_NullLhsInterp) {
    auto v = interp_ok(R"(null ?? "default")");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "default");
}

TEST(NullishCoalescing, NC01_NullLhsVM) {
    auto v = vm_ok(R"(null ?? "default")");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "default");
}

// ============================================================
// NC-02: undefined ?? "default" → "default"
// ============================================================

TEST(NullishCoalescing, NC02_UndefinedLhsInterp) {
    auto v = interp_ok(R"(undefined ?? "default")");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "default");
}

TEST(NullishCoalescing, NC02_UndefinedLhsVM) {
    auto v = vm_ok(R"(undefined ?? "default")");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "default");
}

// ============================================================
// NC-03: 0 ?? "default" → 0（0 不是 null/undefined）
// ============================================================

TEST(NullishCoalescing, NC03_ZeroLhsInterp) {
    auto v = interp_ok(R"(0 ?? "default")");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}

TEST(NullishCoalescing, NC03_ZeroLhsVM) {
    auto v = vm_ok(R"(0 ?? "default")");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}

// ============================================================
// NC-04: false ?? "default" → false
// ============================================================

TEST(NullishCoalescing, NC04_FalseLhsInterp) {
    auto v = interp_ok(R"(false ?? "default")");
    EXPECT_TRUE(v.is_bool());
    EXPECT_EQ(v.as_bool(), false);
}

TEST(NullishCoalescing, NC04_FalseLhsVM) {
    auto v = vm_ok(R"(false ?? "default")");
    EXPECT_TRUE(v.is_bool());
    EXPECT_EQ(v.as_bool(), false);
}

// ============================================================
// NC-05: "" ?? "default" → "" (空字符串不是 nullish)
// ============================================================

TEST(NullishCoalescing, NC05_EmptyStringLhsInterp) {
    auto v = interp_ok(R"("" ?? "default")");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "");
}

TEST(NullishCoalescing, NC05_EmptyStringLhsVM) {
    auto v = vm_ok(R"("" ?? "default")");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "");
}

// ============================================================
// NC-06: NaN ?? "default" → NaN（NaN 是数字，不是 nullish）
// ============================================================

TEST(NullishCoalescing, NC06_NaNLhsInterp) {
    auto v = interp_ok(R"((0/0) ?? "default")");
    EXPECT_TRUE(v.is_number());
    EXPECT_TRUE(std::isnan(v.as_number()));
}

TEST(NullishCoalescing, NC06_NaNLhsVM) {
    auto v = vm_ok(R"((0/0) ?? "default")");
    EXPECT_TRUE(v.is_number());
    EXPECT_TRUE(std::isnan(v.as_number()));
}

// ============================================================
// NC-07: 1 ?? "default" → 1
// ============================================================

TEST(NullishCoalescing, NC07_NumberLhsInterp) {
    auto v = interp_ok(R"(1 ?? "default")");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

TEST(NullishCoalescing, NC07_NumberLhsVM) {
    auto v = vm_ok(R"(1 ?? "default")");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

// ============================================================
// NC-08: "hello" ?? "default" → "hello"
// ============================================================

TEST(NullishCoalescing, NC08_StringLhsInterp) {
    auto v = interp_ok(R"("hello" ?? "default")");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "hello");
}

TEST(NullishCoalescing, NC08_StringLhsVM) {
    auto v = vm_ok(R"("hello" ?? "default")");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "hello");
}

// ============================================================
// NC-09: LHS 非 nullish 时 RHS 不求值（短路）
// ============================================================

TEST(NullishCoalescing, NC09_RhsNotEvaledInterp) {
    auto v = interp_ok(R"(
        let count = 0;
        function inc() { count++; return "side"; }
        let r = "hello" ?? inc();
        count === 0 && r === "hello"
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(NullishCoalescing, NC09_RhsNotEvaledVM) {
    auto v = vm_ok(R"(
        let count = 0;
        function inc() { count++; return "side"; }
        let r = "hello" ?? inc();
        count === 0 && r === "hello"
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// NC-10: LHS 为 null 时 RHS 求值一次
// ============================================================

TEST(NullishCoalescing, NC10_RhsEvaledOnceInterp) {
    auto v = interp_ok(R"(
        let count = 0;
        function inc() { count++; return "side"; }
        let r = null ?? inc();
        count === 1 && r === "side"
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(NullishCoalescing, NC10_RhsEvaledOnceVM) {
    auto v = vm_ok(R"(
        let count = 0;
        function inc() { count++; return "side"; }
        let r = null ?? inc();
        count === 1 && r === "side"
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// NC-11: null ?? undefined → undefined
// ============================================================

TEST(NullishCoalescing, NC11_NullAndUndefinedInterp) {
    auto v = interp_ok("null ?? undefined");
    EXPECT_TRUE(v.is_undefined());
}

TEST(NullishCoalescing, NC11_NullAndUndefinedVM) {
    auto v = vm_ok("null ?? undefined");
    EXPECT_TRUE(v.is_undefined());
}

// ============================================================
// NC-12: undefined ?? null → null
// ============================================================

TEST(NullishCoalescing, NC12_UndefinedAndNullInterp) {
    auto v = interp_ok("undefined ?? null");
    EXPECT_TRUE(v.is_null());
}

TEST(NullishCoalescing, NC12_UndefinedAndNullVM) {
    auto v = vm_ok("undefined ?? null");
    EXPECT_TRUE(v.is_null());
}

// ============================================================
// NC-13: null ?? null → null
// ============================================================

TEST(NullishCoalescing, NC13_NullAndNullInterp) {
    auto v = interp_ok("null ?? null");
    EXPECT_TRUE(v.is_null());
}

TEST(NullishCoalescing, NC13_NullAndNullVM) {
    auto v = vm_ok("null ?? null");
    EXPECT_TRUE(v.is_null());
}

// ============================================================
// NC-14: 左结合 null ?? "a" ?? "b" → "a"
// ============================================================

TEST(NullishCoalescing, NC14_LeftAssocChainInterp) {
    auto v = interp_ok(R"(null ?? "a" ?? "b")");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "a");
}

TEST(NullishCoalescing, NC14_LeftAssocChainVM) {
    auto v = vm_ok(R"(null ?? "a" ?? "b")");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "a");
}

// ============================================================
// NC-15: "x" ?? "a" ?? "b" → "x"
// ============================================================

TEST(NullishCoalescing, NC15_NonNullishFirstInterp) {
    auto v = interp_ok(R"("x" ?? "a" ?? "b")");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "x");
}

TEST(NullishCoalescing, NC15_NonNullishFirstVM) {
    auto v = vm_ok(R"("x" ?? "a" ?? "b")");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "x");
}

// ============================================================
// NC-16: (false && null) ?? "yes" → false（false 非 nullish）
// ============================================================

TEST(NullishCoalescing, NC16_FalseAndNullInterp) {
    auto v = interp_ok(R"((false && null) ?? "yes")");
    EXPECT_TRUE(v.is_bool());
    EXPECT_EQ(v.as_bool(), false);
}

TEST(NullishCoalescing, NC16_FalseAndNullVM) {
    auto v = vm_ok(R"((false && null) ?? "yes")");
    EXPECT_TRUE(v.is_bool());
    EXPECT_EQ(v.as_bool(), false);
}

// ============================================================
// NC-17: (false || null) ?? "yes" → "yes"（null 是 nullish）
// ============================================================

TEST(NullishCoalescing, NC17_FalseOrNullInterp) {
    auto v = interp_ok(R"((false || null) ?? "yes")");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "yes");
}

TEST(NullishCoalescing, NC17_FalseOrNullVM) {
    auto v = vm_ok(R"((false || null) ?? "yes")");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "yes");
}

// ============================================================
// NC-18: (false || 0) ?? "yes" → 0（0 非 nullish）
// ============================================================

TEST(NullishCoalescing, NC18_FalseOrZeroInterp) {
    auto v = interp_ok(R"((false || 0) ?? "yes")");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}

TEST(NullishCoalescing, NC18_FalseOrZeroVM) {
    auto v = vm_ok(R"((false || 0) ?? "yes")");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}

// ============================================================
// NC-19: null ?? "a" ? "b" : "c" → "b"
// ============================================================

TEST(NullishCoalescing, NC19_NullishBeforeTernaryInterp) {
    auto v = interp_ok(R"(null ?? "a" ? "b" : "c")");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "b");
}

TEST(NullishCoalescing, NC19_NullishBeforeTernaryVM) {
    auto v = vm_ok(R"(null ?? "a" ? "b" : "c")");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "b");
}

// ============================================================
// NC-20: SyntaxError: a && b ?? c（无括号混用）
// ============================================================

TEST(NullishCoalescing, NC20_MixAndNullishSyntaxError) {
    EXPECT_TRUE(parse_fails("a && b ?? c"));
}

// ============================================================
// NC-21: SyntaxError: a ?? b || c
// ============================================================

TEST(NullishCoalescing, NC21_MixNullishOrSyntaxError) {
    EXPECT_TRUE(parse_fails("a ?? b || c"));
}

// ============================================================
// NC-22: SyntaxError: a || b ?? c
// ============================================================

TEST(NullishCoalescing, NC22_MixOrNullishSyntaxError) {
    EXPECT_TRUE(parse_fails("a || b ?? c"));
}

// ============================================================
// NC-23: SyntaxError: a ?? b && c
// ============================================================

TEST(NullishCoalescing, NC23_MixNullishAndSyntaxError) {
    EXPECT_TRUE(parse_fails("a ?? b && c"));
}

// ============================================================
// NC-24: (a && b) ?? c — 合法（括号隔离）
// ============================================================

TEST(NullishCoalescing, NC24_ParenAndNullishInterp) {
    auto v = interp_ok(R"((false && true) ?? "c")");
    EXPECT_TRUE(v.is_bool());
    EXPECT_EQ(v.as_bool(), false);
}

TEST(NullishCoalescing, NC24_ParenAndNullishVM) {
    auto v = vm_ok(R"((false && true) ?? "c")");
    EXPECT_TRUE(v.is_bool());
    EXPECT_EQ(v.as_bool(), false);
}

// ============================================================
// NC-25: a ?? (b || c) — 合法（括号隔离）
// ============================================================

TEST(NullishCoalescing, NC25_NullishParenOrInterp) {
    auto v = interp_ok(R"(1 ?? (false || 2))");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

TEST(NullishCoalescing, NC25_NullishParenOrVM) {
    auto v = vm_ok(R"(1 ?? (false || 2))");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

// ============================================================
// NC-26: null ?? (null ?? "deep") → "deep"
// ============================================================

TEST(NullishCoalescing, NC26_NestedNullishInterp) {
    auto v = interp_ok(R"(null ?? (null ?? "deep"))");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "deep");
}

TEST(NullishCoalescing, NC26_NestedNullishVM) {
    auto v = vm_ok(R"(null ?? (null ?? "deep"))");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "deep");
}

// ============================================================
// NC-27: null ?? null ?? null ?? "end" → "end"
// ============================================================

TEST(NullishCoalescing, NC27_LongChainInterp) {
    auto v = interp_ok(R"(null ?? null ?? null ?? "end")");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "end");
}

TEST(NullishCoalescing, NC27_LongChainVM) {
    auto v = vm_ok(R"(null ?? null ?? null ?? "end")");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "end");
}

// ============================================================
// NC-28: 对象属性为 null 时回退
// ============================================================

TEST(NullishCoalescing, NC28_ObjectPropNullInterp) {
    auto v = interp_ok(R"(
        let obj = {x: null};
        obj.x ?? "default"
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "default");
}

TEST(NullishCoalescing, NC28_ObjectPropNullVM) {
    auto v = vm_ok(R"(
        let obj = {x: null};
        obj.x ?? "default"
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "default");
}

// ============================================================
// NC-29: 函数返回值作为 LHS
// ============================================================

TEST(NullishCoalescing, NC29_FunctionReturnLhsInterp) {
    auto v = interp_ok(R"(
        function f() { return null; }
        f() ?? "fallback"
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "fallback");
}

TEST(NullishCoalescing, NC29_FunctionReturnLhsVM) {
    auto v = vm_ok(R"(
        function f() { return null; }
        f() ?? "fallback"
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "fallback");
}

// ============================================================
// NC-30: ?? 与赋值组合
// ============================================================

TEST(NullishCoalescing, NC30_AssignmentInterp) {
    auto v = interp_ok(R"(
        let x = null ?? "val";
        x
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "val");
}

TEST(NullishCoalescing, NC30_AssignmentVM) {
    auto v = vm_ok(R"(
        let x = null ?? "val";
        x
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "val");
}

// ============================================================
// NC-31: getter 副作用 — getter 只调用一次，LHS 非 nullish 时 RHS 不求值
// ============================================================

TEST(NullishCoalescing, NC31_GetterCalledOnceNonNullishInterp) {
    auto v = interp_ok(R"(
        let getCount = 0;
        let rhsCount = 0;
        let obj = {};
        Object.defineProperty(obj, 'x', {
            get: function() { getCount++; return 42; }
        });
        function getDefault() { rhsCount++; return "default"; }
        let r = obj.x ?? getDefault();
        getCount === 1 && rhsCount === 0 && r === 42
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(NullishCoalescing, NC31_GetterCalledOnceNonNullishVM) {
    auto v = vm_ok(R"(
        let getCount = 0;
        let rhsCount = 0;
        let obj = {};
        Object.defineProperty(obj, 'x', {
            get: function() { getCount++; return 42; }
        });
        function getDefault() { rhsCount++; return "default"; }
        let r = obj.x ?? getDefault();
        getCount === 1 && rhsCount === 0 && r === 42
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// getter 返回 null 时 RHS 求值一次，getter 仍只调用一次
TEST(NullishCoalescing, NC31b_GetterCalledOnceNullishInterp) {
    auto v = interp_ok(R"(
        let getCount = 0;
        let rhsCount = 0;
        let obj = {};
        Object.defineProperty(obj, 'x', {
            get: function() { getCount++; return null; }
        });
        function getDefault() { rhsCount++; return "default"; }
        let r = obj.x ?? getDefault();
        getCount === 1 && rhsCount === 1 && r === "default"
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(NullishCoalescing, NC31b_GetterCalledOnceNullishVM) {
    auto v = vm_ok(R"(
        let getCount = 0;
        let rhsCount = 0;
        let obj = {};
        Object.defineProperty(obj, 'x', {
            get: function() { getCount++; return null; }
        });
        function getDefault() { rhsCount++; return "default"; }
        let r = obj.x ?? getDefault();
        getCount === 1 && rhsCount === 1 && r === "default"
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// NC-32: LHS 为对象 {} → 返回对象本身（非 nullish）
// ============================================================

TEST(NullishCoalescing, NC32_ObjectLhsInterp) {
    auto v = interp_ok(R"(
        let obj = {};
        let r = obj ?? "default";
        typeof r === "object" && r !== null
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(NullishCoalescing, NC32_ObjectLhsVM) {
    auto v = vm_ok(R"(
        let obj = {};
        let r = obj ?? "default";
        typeof r === "object" && r !== null
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// NC-33: LHS 为空数组 [] → 返回数组本身（非 nullish）
// ============================================================

TEST(NullishCoalescing, NC33_ArrayLhsInterp) {
    auto v = interp_ok(R"(
        let arr = [];
        let r = arr ?? "default";
        typeof r === "object" && r !== null
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(NullishCoalescing, NC33_ArrayLhsVM) {
    auto v = vm_ok(R"(
        let arr = [];
        let r = arr ?? "default";
        typeof r === "object" && r !== null
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// NC-34: ?? 作为函数参数
// ============================================================

TEST(NullishCoalescing, NC34_AsArgumentInterp) {
    auto v = interp_ok(R"(
        function foo(x) { return x; }
        foo(null ?? "val") === "val"
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(NullishCoalescing, NC34_AsArgumentVM) {
    auto v = vm_ok(R"(
        function foo(x) { return x; }
        foo(null ?? "val") === "val"
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// NC-35: ?? 在 return 语句中
// ============================================================

TEST(NullishCoalescing, NC35_InReturnInterp) {
    auto v = interp_ok(R"(
        function f() { return null ?? 42; }
        f() === 42
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(NullishCoalescing, NC35_InReturnVM) {
    auto v = vm_ok(R"(
        function f() { return null ?? 42; }
        f() === 42
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// NC-36: 嵌套三元中 ?? — (null ?? "a") === "a" ? "yes" : "no" → "yes"
// ============================================================

TEST(NullishCoalescing, NC36_InTernaryConditionInterp) {
    auto v = interp_ok(R"((null ?? "a") === "a" ? "yes" : "no")");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "yes");
}

TEST(NullishCoalescing, NC36_InTernaryConditionVM) {
    auto v = vm_ok(R"((null ?? "a") === "a" ? "yes" : "no")");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "yes");
}

// ============================================================
// NC-37: ?? 与 + 运算符优先级 — null ?? 1 + 2 → 3
// ============================================================

TEST(NullishCoalescing, NC37_PlusHigherPriorityInterp) {
    auto v = interp_ok(R"(null ?? 1 + 2)");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 3.0);
}

TEST(NullishCoalescing, NC37_PlusHigherPriorityVM) {
    auto v = vm_ok(R"(null ?? 1 + 2)");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 3.0);
}

// ============================================================
// NC-38: ?? 链接不同类型 — null ?? 0 ?? "final" → 0
// ============================================================

TEST(NullishCoalescing, NC38_ChainWithZeroInterp) {
    auto v = interp_ok(R"(null ?? 0 ?? "final")");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}

TEST(NullishCoalescing, NC38_ChainWithZeroVM) {
    auto v = vm_ok(R"(null ?? 0 ?? "final")");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}

// ============================================================
// NC-39: LHS 为 Symbol → Symbol 非 nullish，返回 Symbol 本身
// ============================================================

TEST(NullishCoalescing, NC39_SymbolLhsInterp) {
    auto v = interp_ok(R"(
        let s = Symbol("x");
        let r = s ?? "default";
        typeof r === "symbol"
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(NullishCoalescing, NC39_SymbolLhsVM) {
    auto v = vm_ok(R"(
        let s = Symbol("x");
        let r = s ?? "default";
        typeof r === "symbol"
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// NC-40: 对象解构默认值 vs ?? — {x = undefined} = {} → x=undefined → ?? "fallback"
// ============================================================

TEST(NullishCoalescing, NC40_DestructuringVsNullishInterp) {
    auto v = interp_ok(R"(
        const { x = undefined } = {};
        x ?? "fallback"
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "fallback");
}

TEST(NullishCoalescing, NC40_DestructuringVsNullishVM) {
    auto v = vm_ok(R"(
        const { x = undefined } = {};
        x ?? "fallback"
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "fallback");
}

// ============================================================
// NC-41: ??= 占位测试 — 当前 QuestionQuestionEq 报 parse error（已知限制）
// ============================================================

TEST(NullishCoalescing, NC41_NullishAssignCurrentlyParseError) {
    // ??= (QuestionQuestionEq) 尚未在 Parser 中实现赋值语义
    // 预期当前阶段报 parse error（占位，待 ??= 实现后改为语义验证）
    // 拆分字符串避免编译器 trigraph 警告（??= 在 C++ 源码中是三字符序列）
    EXPECT_TRUE(parse_fails("let a = null; a ??" "= 42"));
}

}  // namespace
