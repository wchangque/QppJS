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
// GJ-01: typeof globalThis === "object"
// ============================================================

TEST(GlobalJson, GJ01_Interp_GlobalThis_TypeOf) {
    auto v = interp_ok("typeof globalThis === 'object'");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(GlobalJson, GJ01_VM_GlobalThis_TypeOf) {
    auto v = vm_ok("typeof globalThis === 'object'");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// GJ-02: globalThis === globalThis (identity)
// ============================================================

TEST(GlobalJson, GJ02_Interp_GlobalThis_Identity) {
    auto v = interp_ok("var a = globalThis; var b = globalThis; a === b");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(GlobalJson, GJ02_VM_GlobalThis_Identity) {
    auto v = vm_ok("var a = globalThis; var b = globalThis; a === b");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// GJ-03: globalThis.globalThis === globalThis (self-reference)
// ============================================================

TEST(GlobalJson, GJ03_Interp_GlobalThis_SelfReference) {
    auto v = interp_ok("globalThis.globalThis === globalThis");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(GlobalJson, GJ03_VM_GlobalThis_SelfReference) {
    auto v = vm_ok("globalThis.globalThis === globalThis");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// GJ-04: Object.is(NaN, NaN) === true
// ============================================================

TEST(GlobalJson, GJ04_Interp_ObjectIs_NaNNaN) {
    auto v = interp_ok("Object.is(NaN, NaN)");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(GlobalJson, GJ04_VM_ObjectIs_NaNNaN) {
    auto v = vm_ok("Object.is(NaN, NaN)");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// GJ-05: Object.is(+0, -0) === false
// ============================================================

TEST(GlobalJson, GJ05_Interp_ObjectIs_PlusZeroMinusZero) {
    auto v = interp_ok("Object.is(+0, -0)");
    EXPECT_TRUE(v.is_bool());
    EXPECT_FALSE(v.as_bool());
}

TEST(GlobalJson, GJ05_VM_ObjectIs_PlusZeroMinusZero) {
    auto v = vm_ok("Object.is(+0, -0)");
    EXPECT_TRUE(v.is_bool());
    EXPECT_FALSE(v.as_bool());
}

// ============================================================
// GJ-06: Object.is(1, 1) === true
// ============================================================

TEST(GlobalJson, GJ06_Interp_ObjectIs_SameNumber) {
    auto v = interp_ok("Object.is(1, 1)");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(GlobalJson, GJ06_VM_ObjectIs_SameNumber) {
    auto v = vm_ok("Object.is(1, 1)");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// GJ-07: Object.setPrototypeOf({}, null) sets proto to null
// ============================================================

TEST(GlobalJson, GJ07_Interp_ObjectSetPrototypeOf_Null) {
    auto v = interp_ok(R"(
        var obj = {};
        Object.setPrototypeOf(obj, null);
        Object.getPrototypeOf(obj) === null
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(GlobalJson, GJ07_VM_ObjectSetPrototypeOf_Null) {
    auto v = vm_ok(R"(
        var obj = {};
        Object.setPrototypeOf(obj, null);
        Object.getPrototypeOf(obj) === null
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// GJ-08: Object.hasOwn({x:1}, "x") true / "y" false
// ============================================================

TEST(GlobalJson, GJ08_Interp_ObjectHasOwn) {
    auto v1 = interp_ok("Object.hasOwn({x:1}, 'x')");
    EXPECT_TRUE(v1.is_bool());
    EXPECT_TRUE(v1.as_bool());

    auto v2 = interp_ok("Object.hasOwn({x:1}, 'y')");
    EXPECT_TRUE(v2.is_bool());
    EXPECT_FALSE(v2.as_bool());
}

TEST(GlobalJson, GJ08_VM_ObjectHasOwn) {
    auto v1 = vm_ok("Object.hasOwn({x:1}, 'x')");
    EXPECT_TRUE(v1.is_bool());
    EXPECT_TRUE(v1.as_bool());

    auto v2 = vm_ok("Object.hasOwn({x:1}, 'y')");
    EXPECT_TRUE(v2.is_bool());
    EXPECT_FALSE(v2.as_bool());
}

// ============================================================
// GJ-09: Array.prototype.at
// ============================================================

TEST(GlobalJson, GJ09_Interp_ArrayAt) {
    auto v1 = interp_ok("[1,2,3].at(-1)");
    EXPECT_TRUE(v1.is_number());
    EXPECT_EQ(v1.as_number(), 3.0);

    auto v2 = interp_ok("[1,2,3].at(0)");
    EXPECT_TRUE(v2.is_number());
    EXPECT_EQ(v2.as_number(), 1.0);

    auto v3 = interp_ok("[1,2,3].at(5)");
    EXPECT_TRUE(v3.is_undefined());
}

TEST(GlobalJson, GJ09_VM_ArrayAt) {
    auto v1 = vm_ok("[1,2,3].at(-1)");
    EXPECT_TRUE(v1.is_number());
    EXPECT_EQ(v1.as_number(), 3.0);

    auto v2 = vm_ok("[1,2,3].at(0)");
    EXPECT_TRUE(v2.is_number());
    EXPECT_EQ(v2.as_number(), 1.0);

    auto v3 = vm_ok("[1,2,3].at(5)");
    EXPECT_TRUE(v3.is_undefined());
}

// ============================================================
// GJ-10: JSON.stringify(null) === "null"
// ============================================================

TEST(GlobalJson, GJ10_Interp_JsonStringify_Null) {
    auto v = interp_ok("JSON.stringify(null)");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "null");
}

TEST(GlobalJson, GJ10_VM_JsonStringify_Null) {
    auto v = vm_ok("JSON.stringify(null)");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "null");
}

// ============================================================
// GJ-11: JSON.stringify({a:1,b:"x"})
// ============================================================

TEST(GlobalJson, GJ11_Interp_JsonStringify_Object) {
    auto v = interp_ok(R"(JSON.stringify({a:1,b:"x"}))");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), R"({"a":1,"b":"x"})");
}

TEST(GlobalJson, GJ11_VM_JsonStringify_Object) {
    auto v = vm_ok(R"(JSON.stringify({a:1,b:"x"}))");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), R"({"a":1,"b":"x"})");
}

// ============================================================
// GJ-12: JSON.stringify([1,2,3])
// ============================================================

TEST(GlobalJson, GJ12_Interp_JsonStringify_Array) {
    auto v = interp_ok("JSON.stringify([1,2,3])");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "[1,2,3]");
}

TEST(GlobalJson, GJ12_VM_JsonStringify_Array) {
    auto v = vm_ok("JSON.stringify([1,2,3])");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "[1,2,3]");
}

// ============================================================
// GJ-13: JSON.stringify(NaN) === "null"
// ============================================================

TEST(GlobalJson, GJ13_Interp_JsonStringify_NaN) {
    auto v = interp_ok("JSON.stringify(NaN)");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "null");
}

TEST(GlobalJson, GJ13_VM_JsonStringify_NaN) {
    auto v = vm_ok("JSON.stringify(NaN)");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "null");
}

// ============================================================
// GJ-14: JSON.parse('{"a":1}').a === 1
// ============================================================

TEST(GlobalJson, GJ14_Interp_JsonParse_Object) {
    auto v = interp_ok("JSON.parse('{\"a\":1}').a");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

TEST(GlobalJson, GJ14_VM_JsonParse_Object) {
    auto v = vm_ok("JSON.parse('{\"a\":1}').a");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

// ============================================================
// GJ-15: JSON.parse('[1,2,3]')[1] === 2
// ============================================================

TEST(GlobalJson, GJ15_Interp_JsonParse_Array) {
    auto v = interp_ok("JSON.parse('[1,2,3]')[1]");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.0);
}

TEST(GlobalJson, GJ15_VM_JsonParse_Array) {
    auto v = vm_ok("JSON.parse('[1,2,3]')[1]");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.0);
}

// ============================================================
// GJ-16: queueMicrotask — 微任务在 drain 后被调用
// ============================================================

TEST(GlobalJson, GJ16_Interp_QueueMicrotask) {
    auto v = interp_ok(R"(
        var called = false;
        queueMicrotask(function() { called = true; });
        called
    )");
    // After exec() drains the job queue, called should be true
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(GlobalJson, GJ16_VM_QueueMicrotask) {
    auto v = vm_ok(R"(
        var called = false;
        queueMicrotask(function() { called = true; });
        called
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

}  // namespace
