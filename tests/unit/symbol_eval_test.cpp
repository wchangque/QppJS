#include "qppjs/frontend/parser.h"
#include "qppjs/runtime/interpreter.h"
#include "qppjs/vm/compiler.h"
#include "qppjs/vm/vm.h"

#include <gtest/gtest.h>

#include <string_view>

using namespace qppjs;

namespace {

static Value interp_ok(std::string_view source) {
    auto parse_result = parse_program(source);
    EXPECT_TRUE(parse_result.ok()) << "parse failed: " << parse_result.error().message();
    if (!parse_result.ok()) return Value::undefined();
    Interpreter interp;
    auto result = interp.exec(parse_result.value());
    EXPECT_TRUE(result.is_ok()) << "exec failed: " << result.error().message();
    return result.value();
}

static bool interp_throws(std::string_view source) {
    auto parse_result = parse_program(source);
    if (!parse_result.ok()) return true;
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

// ============================================================
// SE-01: Symbol.toPrimitive !== undefined
// ============================================================

TEST(SymbolEval, SE01_Interp_ToPrimitive_Registered) {
    auto v = interp_ok("Symbol.toPrimitive !== undefined");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(SymbolEval, SE01_VM_ToPrimitive_Registered) {
    auto v = vm_ok("Symbol.toPrimitive !== undefined");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// SE-02: Symbol.hasInstance !== undefined
// ============================================================

TEST(SymbolEval, SE02_Interp_HasInstance_Registered) {
    auto v = interp_ok("Symbol.hasInstance !== undefined");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(SymbolEval, SE02_VM_HasInstance_Registered) {
    auto v = vm_ok("Symbol.hasInstance !== undefined");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// SE-03: Symbol.toStringTag !== undefined
// ============================================================

TEST(SymbolEval, SE03_Interp_ToStringTag_Registered) {
    auto v = interp_ok("Symbol.toStringTag !== undefined");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(SymbolEval, SE03_VM_ToStringTag_Registered) {
    auto v = vm_ok("Symbol.toStringTag !== undefined");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// SE-04: Symbol.asyncIterator !== undefined
// ============================================================

TEST(SymbolEval, SE04_Interp_AsyncIterator_Registered) {
    auto v = interp_ok("Symbol.asyncIterator !== undefined");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(SymbolEval, SE04_VM_AsyncIterator_Registered) {
    auto v = vm_ok("Symbol.asyncIterator !== undefined");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// SE-04b: Symbol.species !== undefined
// ============================================================

TEST(SymbolEval, SE04b_Interp_Species_Registered) {
    auto v = interp_ok("Symbol.species !== undefined");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(SymbolEval, SE04b_VM_Species_Registered) {
    auto v = vm_ok("Symbol.species !== undefined");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// SE-05: [Symbol.toPrimitive] in +obj triggers hint="number"
// ============================================================

TEST(SymbolEval, SE05_Interp_ToPrimitive_NumberHint) {
    auto v = interp_ok(R"(
        var obj = {};
        obj[Symbol.toPrimitive] = function(hint) { return hint === "number" ? 42 : 0; };
        +obj
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 42.0);
}

TEST(SymbolEval, SE05_VM_ToPrimitive_NumberHint) {
    auto v = vm_ok(R"(
        var obj = {};
        obj[Symbol.toPrimitive] = function(hint) { return hint === "number" ? 42 : 0; };
        +obj
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 42.0);
}

// ============================================================
// SE-06: [Symbol.toPrimitive] in template string triggers hint="string"
// ============================================================

TEST(SymbolEval, SE06_Interp_ToPrimitive_StringHint) {
    auto v = interp_ok(R"(
        var obj = {};
        obj[Symbol.toPrimitive] = function(hint) { return hint === "string" ? "hello" : "other"; };
        `${obj}`
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "hello");
}

TEST(SymbolEval, SE06_VM_ToPrimitive_StringHint) {
    auto v = vm_ok(R"(
        var obj = {};
        obj[Symbol.toPrimitive] = function(hint) { return hint === "string" ? "hello" : "other"; };
        `${obj}`
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "hello");
}

// ============================================================
// SE-07: new Function("return 1+2")() → 3
// ============================================================

TEST(SymbolEval, SE07_Interp_FunctionConstructor_NoParams) {
    auto v = interp_ok("new Function('return 1+2')()");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 3.0);
}

TEST(SymbolEval, SE07_VM_FunctionConstructor_NoParams) {
    auto v = vm_ok("new Function('return 1+2')()");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 3.0);
}

// ============================================================
// SE-08: new Function("x", "return x*2")(5) → 10
// ============================================================

TEST(SymbolEval, SE08_Interp_FunctionConstructor_WithParams) {
    auto v = interp_ok("new Function('x', 'return x*2')(5)");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 10.0);
}

TEST(SymbolEval, SE08_VM_FunctionConstructor_WithParams) {
    auto v = vm_ok("new Function('x', 'return x*2')(5)");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 10.0);
}

// ============================================================
// SE-09: Function("return 42")() → 42 (without new)
// ============================================================

TEST(SymbolEval, SE09_Interp_FunctionConstructor_WithoutNew) {
    auto v = interp_ok("Function('return 42')()");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 42.0);
}

TEST(SymbolEval, SE09_VM_FunctionConstructor_WithoutNew) {
    auto v = vm_ok("Function('return 42')()");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 42.0);
}

// ============================================================
// SE-10: Function constructor with syntax error → SyntaxError
// ============================================================

TEST(SymbolEval, SE10_Interp_FunctionConstructor_SyntaxError) {
    EXPECT_TRUE(interp_throws(R"(
        try { new Function('%%%invalid%%%'); } catch(e) { false }
        true
    )") == false);
    // More direct: it should throw when called
    auto v = interp_ok(R"(
        var threw = false;
        try { new Function('%%%invalid%%%'); } catch(e) { threw = true; }
        threw
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(SymbolEval, SE10_VM_FunctionConstructor_SyntaxError) {
    auto v = vm_ok(R"(
        var threw = false;
        try { new Function('%%%invalid%%%'); } catch(e) { threw = true; }
        threw
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// SE-11: eval("1+2") → 3
// ============================================================

TEST(SymbolEval, SE11_Interp_Eval_BasicExpr) {
    auto v = interp_ok("eval('1+2')");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 3.0);
}

TEST(SymbolEval, SE11_VM_Eval_BasicExpr) {
    auto v = vm_ok("eval('1+2')");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 3.0);
}

// ============================================================
// SE-12: eval("var x = 10; x*2") → 20
// ============================================================

TEST(SymbolEval, SE12_Interp_Eval_VarDecl) {
    auto v = interp_ok("eval('var x = 10; x*2')");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 20.0);
}

TEST(SymbolEval, SE12_VM_Eval_VarDecl) {
    auto v = vm_ok("eval('var x = 10; x*2')");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 20.0);
}

// ============================================================
// SE-13: eval("[1,2,3].length") → 3
// ============================================================

TEST(SymbolEval, SE13_Interp_Eval_ArrayLength) {
    auto v = interp_ok("eval('[1,2,3].length')");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 3.0);
}

TEST(SymbolEval, SE13_VM_Eval_ArrayLength) {
    auto v = vm_ok("eval('[1,2,3].length')");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 3.0);
}

// ============================================================
// SE-14: eval syntax error → SyntaxError thrown
// ============================================================

TEST(SymbolEval, SE14_Interp_Eval_SyntaxError) {
    auto v = interp_ok(R"(
        var threw = false;
        try { eval('%%%invalid%%%'); } catch(e) { threw = true; }
        threw
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(SymbolEval, SE14_VM_Eval_SyntaxError) {
    auto v = vm_ok(R"(
        var threw = false;
        try { eval('%%%invalid%%%'); } catch(e) { threw = true; }
        threw
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// SE-15: eval non-string → original value returned
// ============================================================

TEST(SymbolEval, SE15_Interp_Eval_NonString_Number) {
    auto v = interp_ok("eval(42)");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 42.0);
}

TEST(SymbolEval, SE15_VM_Eval_NonString_Number) {
    auto v = vm_ok("eval(42)");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 42.0);
}

TEST(SymbolEval, SE15_Interp_Eval_NonString_Undefined) {
    auto v = interp_ok("eval(undefined)");
    EXPECT_TRUE(v.is_undefined());
}

TEST(SymbolEval, SE15_VM_Eval_NonString_Undefined) {
    auto v = vm_ok("eval(undefined)");
    EXPECT_TRUE(v.is_undefined());
}

}  // namespace
