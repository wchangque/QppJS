#include "qppjs/frontend/parser.h"
#include "qppjs/runtime/interpreter.h"
#include "qppjs/vm/compiler.h"
#include "qppjs/vm/vm.h"

#include <gtest/gtest.h>

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
// OC-01: null?.b / undefined?.b → undefined (short-circuit)
// ============================================================

TEST(OptionalChaining, OC01_NullDotPropInterp) {
    EXPECT_TRUE(interp_ok("null?.b").is_undefined());
    EXPECT_TRUE(interp_ok("undefined?.b").is_undefined());
}

TEST(OptionalChaining, OC01_NullDotPropVM) {
    EXPECT_TRUE(vm_ok("null?.b").is_undefined());
    EXPECT_TRUE(vm_ok("undefined?.b").is_undefined());
}

// ============================================================
// OC-02: ({a:1})?.a → 1
// ============================================================

TEST(OptionalChaining, OC02_ObjectPropInterp) {
    EXPECT_EQ(interp_ok("({a:1})?.a").as_number(), 1.0);
}

TEST(OptionalChaining, OC02_ObjectPropVM) {
    EXPECT_EQ(vm_ok("({a:1})?.a").as_number(), 1.0);
}

// ============================================================
// OC-03: false?.b → undefined (false is not null/undefined, no short-circuit)
// ============================================================

TEST(OptionalChaining, OC03_FalseDotPropInterp) {
    EXPECT_TRUE(interp_ok("false?.b").is_undefined());
}

TEST(OptionalChaining, OC03_FalseDotPropVM) {
    EXPECT_TRUE(vm_ok("false?.b").is_undefined());
}

// ============================================================
// OC-04: a?.b.c where a.b is null → TypeError on .c
// ============================================================

TEST(OptionalChaining, OC04_NullIntermediateThrowsInterp) {
    auto v = interp_ok("var a = {b: null}; var r = 'ok'; try { a?.b.c; r = 'bad'; } catch(e) { r = 'caught'; } r;");
    EXPECT_EQ(v.sv(), "caught");
}

TEST(OptionalChaining, OC04_NullIntermediateThrowsVM) {
    auto v = vm_ok("var a = {b: null}; var r = 'ok'; try { a?.b.c; r = 'bad'; } catch(e) { r = 'caught'; } r;");
    EXPECT_EQ(v.sv(), "caught");
}

// ============================================================
// OC-05: a?.b?.c double optional chain
// ============================================================

TEST(OptionalChaining, OC05_DoubleOptionalInterp) {
    EXPECT_TRUE(interp_ok("null?.b?.c").is_undefined());
    EXPECT_TRUE(interp_ok("({b: null})?.b?.c").is_undefined());
    EXPECT_EQ(interp_ok("({b: {c: 42}})?.b?.c").as_number(), 42.0);
}

TEST(OptionalChaining, OC05_DoubleOptionalVM) {
    EXPECT_TRUE(vm_ok("null?.b?.c").is_undefined());
    EXPECT_TRUE(vm_ok("({b: null})?.b?.c").is_undefined());
    EXPECT_EQ(vm_ok("({b: {c: 42}})?.b?.c").as_number(), 42.0);
}

// ============================================================
// OC-06: a?.b?.c?.d triple optional chain
// ============================================================

TEST(OptionalChaining, OC06_TripleOptionalInterp) {
    EXPECT_TRUE(interp_ok("null?.b?.c?.d").is_undefined());
    EXPECT_TRUE(interp_ok("({b: null})?.b?.c?.d").is_undefined());
    EXPECT_TRUE(interp_ok("({b: {c: null}})?.b?.c?.d").is_undefined());
    EXPECT_EQ(interp_ok("({b: {c: {d: 7}}})?.b?.c?.d").as_number(), 7.0);
}

TEST(OptionalChaining, OC06_TripleOptionalVM) {
    EXPECT_TRUE(vm_ok("null?.b?.c?.d").is_undefined());
    EXPECT_TRUE(vm_ok("({b: null})?.b?.c?.d").is_undefined());
    EXPECT_TRUE(vm_ok("({b: {c: null}})?.b?.c?.d").is_undefined());
    EXPECT_EQ(vm_ok("({b: {c: {d: 7}}})?.b?.c?.d").as_number(), 7.0);
}

// ============================================================
// OC-07: obj?.method() — this binding correct
// ============================================================

TEST(OptionalChaining, OC07_MethodCallThisInterp) {
    auto v = interp_ok("var obj = {x: 10, m: function() { return this.x; }}; obj?.m()");
    EXPECT_EQ(v.as_number(), 10.0);
}

TEST(OptionalChaining, OC07_MethodCallThisVM) {
    auto v = vm_ok("var obj = {x: 10, m: function() { return this.x; }}; obj?.m()");
    EXPECT_EQ(v.as_number(), 10.0);
}

// ============================================================
// OC-08: null?.method() → undefined (not called)
// ============================================================

TEST(OptionalChaining, OC08_NullMethodCallInterp) {
    EXPECT_TRUE(interp_ok("null?.method()").is_undefined());
}

TEST(OptionalChaining, OC08_NullMethodCallVM) {
    EXPECT_TRUE(vm_ok("null?.method()").is_undefined());
}

// ============================================================
// OC-09: null?.() → undefined (direct call not invoked)
// ============================================================

TEST(OptionalChaining, OC09_NullDirectCallInterp) {
    EXPECT_TRUE(interp_ok("null?.()").is_undefined());
    EXPECT_TRUE(interp_ok("undefined?.()").is_undefined());
}

TEST(OptionalChaining, OC09_NullDirectCallVM) {
    EXPECT_TRUE(vm_ok("null?.()").is_undefined());
    EXPECT_TRUE(vm_ok("undefined?.()").is_undefined());
}

// ============================================================
// OC-10: RHS not evaluated: null?.[sideEffect()] → undefined
// ============================================================

TEST(OptionalChaining, OC10_RhsNotEvaluatedInterp) {
    auto v = interp_ok("var called = 0; null?.[++called]; called");
    EXPECT_EQ(v.as_number(), 0.0);
}

TEST(OptionalChaining, OC10_RhsNotEvaluatedVM) {
    auto v = vm_ok("var called = 0; null?.[++called]; called");
    EXPECT_EQ(v.as_number(), 0.0);
}

// ============================================================
// OC-11: a?.b ?? "default" combination
// ============================================================

TEST(OptionalChaining, OC11_NullishCoalescingInterp) {
    EXPECT_EQ(interp_ok("null?.b ?? 'def'").sv(), "def");
    EXPECT_EQ(interp_ok("({b: 0})?.b ?? 'def'").as_number(), 0.0);
    EXPECT_EQ(interp_ok("({b: null})?.b ?? 'def'").sv(), "def");
}

TEST(OptionalChaining, OC11_NullishCoalescingVM) {
    EXPECT_EQ(vm_ok("null?.b ?? 'def'").sv(), "def");
    EXPECT_EQ(vm_ok("({b: 0})?.b ?? 'def'").as_number(), 0.0);
    EXPECT_EQ(vm_ok("({b: null})?.b ?? 'def'").sv(), "def");
}

// ============================================================
// OC-12: null?.b = v → SyntaxError (Parser stage)
// ============================================================

TEST(OptionalChaining, OC12_AssignToOptionalChainSyntaxError) {
    EXPECT_TRUE(parse_fails("null?.b = 1"));
    EXPECT_TRUE(parse_fails("a?.b = 1"));
}

// ============================================================
// OC-13: delete null?.b → true
// ============================================================

TEST(OptionalChaining, OC13_DeleteNullChainInterp) {
    auto v1 = interp_ok("delete null?.b");
    EXPECT_TRUE(v1.is_bool());
    EXPECT_TRUE(v1.as_bool());
    auto v2 = interp_ok("delete undefined?.b");
    EXPECT_TRUE(v2.is_bool());
    EXPECT_TRUE(v2.as_bool());
}

TEST(OptionalChaining, OC13_DeleteNullChainVM) {
    auto v1 = vm_ok("delete null?.b");
    EXPECT_TRUE(v1.is_bool());
    EXPECT_TRUE(v1.as_bool());
    auto v2 = vm_ok("delete undefined?.b");
    EXPECT_TRUE(v2.is_bool());
    EXPECT_TRUE(v2.as_bool());
}

// ============================================================
// OC-13b: delete null?.b.c (multi-link) → true (M1 fix regression)
// ============================================================

TEST(OptionalChaining, OC13b_DeleteMultiChainNullInterp) {
    EXPECT_TRUE(interp_ok("delete null?.b.c").as_bool());
}

TEST(OptionalChaining, OC13b_DeleteMultiChainNullVM) {
    EXPECT_TRUE(vm_ok("delete null?.b.c").as_bool());
}

TEST(OptionalChaining, OC13c_DeleteDoubleOptionalNullInterp) {
    EXPECT_TRUE(interp_ok("delete null?.b?.c").as_bool());
}

TEST(OptionalChaining, OC13c_DeleteDoubleOptionalNullVM) {
    EXPECT_TRUE(vm_ok("delete null?.b?.c").as_bool());
}

// ============================================================
// OC-14: delete ({a:1})?.a → true, property deleted
// ============================================================

TEST(OptionalChaining, OC14_DeleteOptionalPropertyInterp) {
    auto v = interp_ok(
        "var obj = {a: 1};"
        "var deleted = delete obj?.a;"
        "deleted + (obj.a === undefined ? 1 : 0)");
    EXPECT_EQ(v.as_number(), 2.0);
}

TEST(OptionalChaining, OC14_DeleteOptionalPropertyVM) {
    auto v = vm_ok(
        "var obj = {a: 1};"
        "var deleted = delete obj?.a;"
        "deleted + (obj.a === undefined ? 1 : 0)");
    EXPECT_EQ(v.as_number(), 2.0);
}

// ============================================================
// OC-15: a?.[key] computed element access
// ============================================================

TEST(OptionalChaining, OC15_ComputedElemInterp) {
    EXPECT_TRUE(interp_ok("null?.['x']").is_undefined());
    EXPECT_EQ(interp_ok("({x: 42})?.['x']").as_number(), 42.0);
    EXPECT_EQ(interp_ok("var k = 'foo'; ({foo: 99})?.[k]").as_number(), 99.0);
}

TEST(OptionalChaining, OC15_ComputedElemVM) {
    EXPECT_TRUE(vm_ok("null?.['x']").is_undefined());
    EXPECT_EQ(vm_ok("({x: 42})?.['x']").as_number(), 42.0);
    EXPECT_EQ(vm_ok("var k = 'foo'; ({foo: 99})?.[k]").as_number(), 99.0);
}

// ============================================================
// OC-16: a?.b()?.c — call result then optional chain
// ============================================================

TEST(OptionalChaining, OC16_CallResultChainInterp) {
    auto v1 = interp_ok(
        "var obj = { m: function() { return {c: 5}; } };"
        "obj?.m()?.c");
    EXPECT_EQ(v1.as_number(), 5.0);

    auto v2 = interp_ok(
        "var obj = { m: function() { return null; } };"
        "obj?.m()?.c");
    EXPECT_TRUE(v2.is_undefined());
}

TEST(OptionalChaining, OC16_CallResultChainVM) {
    auto v1 = vm_ok(
        "var obj = { m: function() { return {c: 5}; } };"
        "obj?.m()?.c");
    EXPECT_EQ(v1.as_number(), 5.0);

    auto v2 = vm_ok(
        "var obj = { m: function() { return null; } };"
        "obj?.m()?.c");
    EXPECT_TRUE(v2.is_undefined());
}

// ============================================================
// Additional: normal property access through ?.
// ============================================================

TEST(OptionalChaining, ExtraPropAccessInterp) {
    EXPECT_EQ(interp_ok("var a = {b: {c: 3}}; a?.b.c").as_number(), 3.0);
    EXPECT_EQ(interp_ok("var a = {b: {c: 3}}; a?.b?.c").as_number(), 3.0);
}

TEST(OptionalChaining, ExtraPropAccessVM) {
    EXPECT_EQ(vm_ok("var a = {b: {c: 3}}; a?.b.c").as_number(), 3.0);
    EXPECT_EQ(vm_ok("var a = {b: {c: 3}}; a?.b?.c").as_number(), 3.0);
}

// ============================================================
// Additional: function call with args via ?.()
// ============================================================

TEST(OptionalChaining, ExtraFuncCallWithArgsInterp) {
    EXPECT_EQ(interp_ok("var f = function(x) { return x * 2; }; f?.(5)").as_number(), 10.0);
    EXPECT_TRUE(interp_ok("null?.(5)").is_undefined());
}

TEST(OptionalChaining, ExtraFuncCallWithArgsVM) {
    EXPECT_EQ(vm_ok("var f = function(x) { return x * 2; }; f?.(5)").as_number(), 10.0);
    EXPECT_TRUE(vm_ok("null?.(5)").is_undefined());
}

// ============================================================
// Additional: method call with args
// ============================================================

TEST(OptionalChaining, ExtraMethodWithArgsInterp) {
    auto v = interp_ok("var obj = {add: function(a,b){return a+b;}}; obj?.add(3,4)");
    EXPECT_EQ(v.as_number(), 7.0);
}

TEST(OptionalChaining, ExtraMethodWithArgsVM) {
    auto v = vm_ok("var obj = {add: function(a,b){return a+b;}}; obj?.add(3,4)");
    EXPECT_EQ(v.as_number(), 7.0);
}

// ============================================================
// OC_Ex1: Falsy non-null/undefined do NOT short-circuit
// Verified via RHS side-effect in ?.[] subscript
// ============================================================

TEST(OptionalChaining, OC_Ex1_FalsyNoShortCircuitInterp) {
    // false: not null/undefined → RHS of ?.[] is evaluated (side effect happens)
    // Interpreter returns undefined for element access on boolean (no throw)
    auto v1 = interp_ok("var c=0; false?.[++c]; c");
    EXPECT_EQ(v1.as_number(), 1.0);

    // 0: not null/undefined → RHS evaluated
    auto v2 = interp_ok("var c=0; 0?.[++c]; c");
    EXPECT_EQ(v2.as_number(), 1.0);

    // "": not null/undefined → RHS evaluated (string path, returns undefined)
    auto v3 = interp_ok("var c=0; ''?.[++c]; c");
    EXPECT_EQ(v3.as_number(), 1.0);

    // contrast: null DOES short-circuit → RHS not evaluated
    auto v4 = interp_ok("var c=0; null?.[++c]; c");
    EXPECT_EQ(v4.as_number(), 0.0);
}

TEST(OptionalChaining, OC_Ex1_FalsyNoShortCircuitVM) {
    // VM kGetElem throws TypeError for non-string/non-object primitives (false, 0),
    // so use try/catch to isolate the short-circuit check: the ++c side effect happens
    // BEFORE kGetElem, proving no short-circuit occurred for false/0.
    auto v1 = vm_ok("var c=0; try{false?.[++c];}catch(e){} c");
    EXPECT_EQ(v1.as_number(), 1.0);

    auto v2 = vm_ok("var c=0; try{0?.[++c];}catch(e){} c");
    EXPECT_EQ(v2.as_number(), 1.0);

    // "" is a string: kGetElem returns undefined (no throw)
    auto v3 = vm_ok("var c=0; ''?.[++c]; c");
    EXPECT_EQ(v3.as_number(), 1.0);

    // contrast: null DOES short-circuit → RHS not evaluated
    auto v4 = vm_ok("var c=0; null?.[++c]; c");
    EXPECT_EQ(v4.as_number(), 0.0);
}

// ============================================================
// OC_Ex2: Empty string length (falsy but non-null, proceeds)
// ============================================================

TEST(OptionalChaining, OC_Ex2_EmptyStringLengthInterp) {
    EXPECT_EQ(interp_ok("''?.length").as_number(), 0.0);
}

TEST(OptionalChaining, OC_Ex2_EmptyStringLengthVM) {
    EXPECT_EQ(vm_ok("''?.length").as_number(), 0.0);
}

// ============================================================
// OC_Ex3: String primitive prototype methods via ?.
// ============================================================

TEST(OptionalChaining, OC_Ex3_StringProtoMethodsInterp) {
    EXPECT_EQ(interp_ok("'hello'?.length").as_number(), 5.0);
    EXPECT_EQ(interp_ok("'hello'?.indexOf('ell')").as_number(), 1.0);
    EXPECT_EQ(interp_ok("'hello'?.slice(1, 3)").sv(), "el");
}

TEST(OptionalChaining, OC_Ex3_StringProtoMethodsVM) {
    EXPECT_EQ(vm_ok("'hello'?.length").as_number(), 5.0);
    EXPECT_EQ(vm_ok("'hello'?.indexOf('ell')").as_number(), 1.0);
    EXPECT_EQ(vm_ok("'hello'?.slice(1, 3)").sv(), "el");
}

// ============================================================
// OC_Ex4: Symbol as LHS — non-null, proceeds to description / toString
// ============================================================

TEST(OptionalChaining, OC_Ex4_SymbolNotShortCircuitInterp) {
    // Symbol("desc")?.description → "desc"
    EXPECT_EQ(interp_ok("Symbol('desc')?.description").sv(), "desc");

    // Symbol("x")?.toString() → "Symbol(x)"
    EXPECT_EQ(interp_ok("Symbol('x')?.toString()").sv(), "Symbol(x)");
}

TEST(OptionalChaining, OC_Ex4_SymbolNotShortCircuitVM) {
    EXPECT_EQ(vm_ok("Symbol('desc')?.description").sv(), "desc");
    EXPECT_EQ(vm_ok("Symbol('x')?.toString()").sv(), "Symbol(x)");
}

// ============================================================
// OC_Ex5: Three-level nested ?. with different null positions
// ============================================================

TEST(OptionalChaining, OC_Ex5_NestedTripleChainInterp) {
    // All non-null: full chain succeeds
    EXPECT_EQ(interp_ok("({a:{b:{c:42}}})?.a?.b?.c").as_number(), 42.0);

    // First ?. short-circuits
    EXPECT_TRUE(interp_ok("null?.a?.b?.c").is_undefined());

    // Second ?. short-circuits (a is null)
    EXPECT_TRUE(interp_ok("({a:null})?.a?.b?.c").is_undefined());

    // Third ?. short-circuits (b is null)
    EXPECT_TRUE(interp_ok("({a:{b:null}})?.a?.b?.c").is_undefined());
}

TEST(OptionalChaining, OC_Ex5_NestedTripleChainVM) {
    EXPECT_EQ(vm_ok("({a:{b:{c:42}}})?.a?.b?.c").as_number(), 42.0);
    EXPECT_TRUE(vm_ok("null?.a?.b?.c").is_undefined());
    EXPECT_TRUE(vm_ok("({a:null})?.a?.b?.c").is_undefined());
    EXPECT_TRUE(vm_ok("({a:{b:null}})?.a?.b?.c").is_undefined());
}

// ============================================================
// OC_Ex6: Mixed ?.prop and ?.[] in same chain
// ============================================================

TEST(OptionalChaining, OC_Ex6_MixedPropAndElemInterp) {
    // Array element access after optional prop
    EXPECT_EQ(interp_ok("({arr:[1,2,3]})?.arr?.[1]").as_number(), 2.0);

    // First base is null → whole chain is undefined
    EXPECT_TRUE(interp_ok("null?.arr?.[1]").is_undefined());

    // arr is null → second ?. short-circuits
    EXPECT_TRUE(interp_ok("({arr:null})?.arr?.[1]").is_undefined());
}

TEST(OptionalChaining, OC_Ex6_MixedPropAndElemVM) {
    EXPECT_EQ(vm_ok("({arr:[1,2,3]})?.arr?.[1]").as_number(), 2.0);
    EXPECT_TRUE(vm_ok("null?.arr?.[1]").is_undefined());
    EXPECT_TRUE(vm_ok("({arr:null})?.arr?.[1]").is_undefined());
}

// ============================================================
// OC_Ex7: ?.() on non-function throws TypeError
// ============================================================

TEST(OptionalChaining, OC_Ex7_NonFunctionCallTypeErrorInterp) {
    auto v = interp_ok("var r='ok'; try{({})?.(); r='bad';}catch(e){r='caught';} r");
    EXPECT_EQ(v.sv(), "caught");
}

TEST(OptionalChaining, OC_Ex7_NonFunctionCallTypeErrorVM) {
    auto v = vm_ok("var r='ok'; try{({})?.(); r='bad';}catch(e){r='caught';} r");
    EXPECT_EQ(v.sv(), "caught");
}

// ============================================================
// OC_Ex8: typeof combination with optional chain
// ============================================================

TEST(OptionalChaining, OC_Ex8_TypeofCombinationInterp) {
    // typeof short-circuits: null?.b → undefined → typeof is "undefined"
    EXPECT_EQ(interp_ok("typeof null?.b").sv(), "undefined");

    // Property exists: typeof is "number"
    EXPECT_EQ(interp_ok("typeof ({a:1})?.a").sv(), "number");

    // undefined base: typeof is "undefined"
    EXPECT_EQ(interp_ok("typeof undefined?.b").sv(), "undefined");
}

TEST(OptionalChaining, OC_Ex8_TypeofCombinationVM) {
    EXPECT_EQ(vm_ok("typeof null?.b").sv(), "undefined");
    EXPECT_EQ(vm_ok("typeof ({a:1})?.a").sv(), "number");
    EXPECT_EQ(vm_ok("typeof undefined?.b").sv(), "undefined");
}

// ============================================================
// OC_Ex9: Side effects executed exactly once
// ============================================================

TEST(OptionalChaining, OC_Ex9_SideEffectsOnceInterp) {
    // f is called once: increments count, returns null → ?.b short-circuits
    auto v = interp_ok(
        "var count=0;"
        "function f(){count++;return null;}"
        "f()?.b;"
        "count");
    EXPECT_EQ(v.as_number(), 1.0);
}

TEST(OptionalChaining, OC_Ex9_SideEffectsOnceVM) {
    auto v = vm_ok(
        "var count=0;"
        "function f(){count++;return null;}"
        "f()?.b;"
        "count");
    EXPECT_EQ(v.as_number(), 1.0);
}

}  // namespace
