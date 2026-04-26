#include "qppjs/frontend/parser.h"
#include "qppjs/runtime/interpreter.h"
#include "qppjs/vm/compiler.h"
#include "qppjs/vm/vm.h"

#include <gtest/gtest.h>

#include <string_view>

namespace {

// ============================================================
// Helpers
// ============================================================

qppjs::Value interp_ok(std::string_view source) {
    auto parse_result = qppjs::parse_program(source);
    EXPECT_TRUE(parse_result.ok()) << "parse failed: " << parse_result.error().message();
    if (!parse_result.ok()) return qppjs::Value::undefined();
    qppjs::Interpreter interp;
    auto result = interp.exec(parse_result.value());
    EXPECT_TRUE(result.is_ok()) << "exec failed: " << result.error().message();
    if (!result.is_ok()) return qppjs::Value::undefined();
    return result.value();
}

qppjs::Value vm_ok(std::string_view source) {
    auto parse_result = qppjs::parse_program(source);
    EXPECT_TRUE(parse_result.ok()) << "parse failed: " << parse_result.error().message();
    if (!parse_result.ok()) return qppjs::Value::undefined();
    qppjs::Compiler compiler;
    auto bytecode = compiler.compile(parse_result.value());
    qppjs::VM vm;
    auto result = vm.exec(bytecode);
    EXPECT_TRUE(result.is_ok()) << "exec failed: " << result.error().message();
    if (!result.is_ok()) return qppjs::Value::undefined();
    return result.value();
}

bool interp_err(std::string_view source) {
    auto parse_result = qppjs::parse_program(source);
    if (!parse_result.ok()) return true;
    qppjs::Interpreter interp;
    auto result = interp.exec(parse_result.value());
    return !result.is_ok();
}

bool vm_err(std::string_view source) {
    auto parse_result = qppjs::parse_program(source);
    if (!parse_result.ok()) return true;
    qppjs::Compiler compiler;
    auto bytecode = compiler.compile(parse_result.value());
    qppjs::VM vm;
    auto result = vm.exec(bytecode);
    return !result.is_ok();
}

// ============================================================
// Interpreter: Object builtin tests
// ============================================================

// OB-01: Object.keys({a:1, b:2}) → length=2, contains "a" and "b"
TEST(InterpObjectBuiltin, KeysTwoProps) {
    auto len = interp_ok("Object.keys({a:1, b:2}).length");
    EXPECT_TRUE(len.is_number());
    EXPECT_EQ(len.as_number(), 2.0);

    auto k0 = interp_ok("Object.keys({a:1, b:2})[0]");
    EXPECT_TRUE(k0.is_string());
    EXPECT_EQ(k0.as_string(), "a");

    auto k1 = interp_ok("Object.keys({a:1, b:2})[1]");
    EXPECT_TRUE(k1.is_string());
    EXPECT_EQ(k1.as_string(), "b");
}

// OB-02: Object.keys({}) → empty array length=0
TEST(InterpObjectBuiltin, KeysEmpty) {
    auto v = interp_ok("Object.keys({}).length");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}

// OB-03: Object.keys({b:2, a:1}) → ["b","a"] (insertion order)
TEST(InterpObjectBuiltin, KeysInsertionOrder) {
    auto k0 = interp_ok("Object.keys({b:2, a:1})[0]");
    EXPECT_TRUE(k0.is_string());
    EXPECT_EQ(k0.as_string(), "b");

    auto k1 = interp_ok("Object.keys({b:2, a:1})[1]");
    EXPECT_TRUE(k1.is_string());
    EXPECT_EQ(k1.as_string(), "a");
}

// OB-04: Object.keys(null) → TypeError
TEST(InterpObjectBuiltin, KeysNullThrows) {
    EXPECT_TRUE(interp_err("Object.keys(null)"));
}

// OB-05: Object.keys(undefined) → TypeError
TEST(InterpObjectBuiltin, KeysUndefinedThrows) {
    EXPECT_TRUE(interp_err("Object.keys(undefined)"));
}

// OB-06: Object.keys(42) → TypeError
TEST(InterpObjectBuiltin, KeysNumberThrows) {
    EXPECT_TRUE(interp_err("Object.keys(42)"));
}

// OB-07: inherited properties not included
TEST(InterpObjectBuiltin, KeysNoInherited) {
    auto v = interp_ok("var o = Object.create({x:1}); Object.keys(o).length");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}

// OB-08: Object.assign({}, {a:1}).a === 1
TEST(InterpObjectBuiltin, AssignBasic) {
    auto v = interp_ok("Object.assign({}, {a:1}).a");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

// OB-09: Object.assign({a:1}, {a:2}).a === 2 (overwrite)
TEST(InterpObjectBuiltin, AssignOverwrite) {
    auto v = interp_ok("Object.assign({a:1}, {a:2}).a");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.0);
}

// OB-10: null source skipped
TEST(InterpObjectBuiltin, AssignNullSource) {
    auto v = interp_ok("Object.assign({}, null, {b:2}).b");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.0);
}

// OB-11: undefined source skipped
TEST(InterpObjectBuiltin, AssignUndefinedSource) {
    auto v = interp_ok("Object.assign({}, undefined, {b:2}).b");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.0);
}

// OB-12: multiple sources merged
TEST(InterpObjectBuiltin, AssignMultipleSources) {
    auto a = interp_ok("Object.assign({}, {a:1}, {b:2}).a");
    EXPECT_TRUE(a.is_number());
    EXPECT_EQ(a.as_number(), 1.0);

    auto b = interp_ok("Object.assign({}, {a:1}, {b:2}).b");
    EXPECT_TRUE(b.is_number());
    EXPECT_EQ(b.as_number(), 2.0);
}

// OB-13: Object.assign(null, {}) → TypeError
TEST(InterpObjectBuiltin, AssignNullTargetThrows) {
    EXPECT_TRUE(interp_err("Object.assign(null, {})"));
}

// OB-14: returns target itself
TEST(InterpObjectBuiltin, AssignReturnsTarget) {
    auto v = interp_ok("var t = {}; Object.assign(t, {a:1}); t.a");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

// OB-15: Object.create({x:1}).x === 1 (prototype chain lookup)
TEST(InterpObjectBuiltin, CreateProtoLookup) {
    auto v = interp_ok("Object.create({x:1}).x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

// OB-16: Object.create(null) → object with no prototype
TEST(InterpObjectBuiltin, CreateNullProto) {
    auto v = interp_ok("var o = Object.create(null); o.x");
    EXPECT_TRUE(v.is_undefined());
}

// OB-17: Object.create(undefined) → TypeError
TEST(InterpObjectBuiltin, CreateUndefinedThrows) {
    EXPECT_TRUE(interp_err("Object.create(undefined)"));
}

// OB-18: Object.create(42) → TypeError
TEST(InterpObjectBuiltin, CreateNumberThrows) {
    EXPECT_TRUE(interp_err("Object.create(42)"));
}

// OB-19: Object.create("str") → TypeError
TEST(InterpObjectBuiltin, CreateStringThrows) {
    EXPECT_TRUE(interp_err(R"(Object.create("str"))"));
}

// OB-20: Object.create(null) own keys === 0
TEST(InterpObjectBuiltin, CreateNullProtoOwnKeys) {
    auto v = interp_ok("Object.keys(Object.create(null)).length");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}

// OB-21: Object.create({}) instanceof Object → true
TEST(InterpObjectBuiltin, CreateInstanceof) {
    auto v = interp_ok("Object.create({}) instanceof Object");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// OB-22: Object.keys(false) → TypeError (bool is not an object)
TEST(InterpObjectBuiltin, KeysBoolThrows) {
    EXPECT_TRUE(interp_err("Object.keys(false)"));
}

// OB-23: Object.keys on a function object → empty array (kFunction branch)
TEST(InterpObjectBuiltin, KeysFunctionObject) {
    auto v = interp_ok("Object.keys(function(){}).length");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}

// OB-24: Object.keys on an array → numeric index strings in order
TEST(InterpObjectBuiltin, KeysArray) {
    auto len = interp_ok("Object.keys([10,20,30]).length");
    EXPECT_TRUE(len.is_number());
    EXPECT_EQ(len.as_number(), 3.0);

    auto k0 = interp_ok("Object.keys([10,20,30])[0]");
    EXPECT_TRUE(k0.is_string());
    EXPECT_EQ(k0.as_string(), "0");

    auto k2 = interp_ok("Object.keys([10,20,30])[2]");
    EXPECT_TRUE(k2.is_string());
    EXPECT_EQ(k2.as_string(), "2");
}

// OB-25: Object.keys result elements are strings (type regression)
TEST(InterpObjectBuiltin, KeysResultElementType) {
    auto v = interp_ok("typeof Object.keys({x:1})[0]");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "string");
}

// OB-26: Object.assign({}) with no sources → returns target (empty loop)
TEST(InterpObjectBuiltin, AssignNoSources) {
    auto v = interp_ok("var t = {a:1}; Object.assign(t); t.a");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

// OB-27: Object.assign(undefined, {}) → TypeError
TEST(InterpObjectBuiltin, AssignUndefinedTargetThrows) {
    EXPECT_TRUE(interp_err("Object.assign(undefined, {})"));
}

// OB-28: Object.assign skips non-null/undefined primitive source (number)
TEST(InterpObjectBuiltin, AssignNumberSourceSkipped) {
    auto v = interp_ok("Object.assign({a:1}, 42).a");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

// OB-29: Object.assign returns the exact target reference (identity)
TEST(InterpObjectBuiltin, AssignReturnsSameTarget) {
    auto v = interp_ok("var t = {}; Object.assign(t, {a:1}) === t");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// OB-30: Object.assign does not copy inherited (prototype) properties
TEST(InterpObjectBuiltin, AssignNoInheritedProps) {
    auto v = interp_ok(
        "var proto = {inherited:99};"
        "var src = Object.create(proto);"
        "src.own = 1;"
        "var t = Object.assign({}, src);"
        "t.own === 1 && t.inherited === undefined");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// OB-31: Object.create(null) → instanceof Object is false (no prototype chain)
TEST(InterpObjectBuiltin, CreateNullProtoNotInstanceof) {
    auto v = interp_ok("Object.create(null) instanceof Object");
    EXPECT_TRUE(v.is_bool());
    EXPECT_FALSE(v.as_bool());
}

// OB-32: Object.create(false) → TypeError (bool is not object or null)
TEST(InterpObjectBuiltin, CreateBoolThrows) {
    EXPECT_TRUE(interp_err("Object.create(false)"));
}

// OB-33: Object.create with second argument (propertiesObject) silently ignored
TEST(InterpObjectBuiltin, CreateSecondArgIgnored) {
    auto v = interp_ok("Object.create({x:1}, {y:{value:2}}).x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

// OB-34: multi-level prototype chain via Object.create
TEST(InterpObjectBuiltin, CreateMultiLevelChain) {
    auto v = interp_ok(
        "var a = {x:1};"
        "var b = Object.create(a);"
        "var c = Object.create(b);"
        "c.x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

// OB-35: Object.keys does not include prototype properties of created object
TEST(InterpObjectBuiltin, KeysExcludesProtoAfterCreate) {
    auto v = interp_ok(
        "var o = Object.create({inherited:1});"
        "o.own = 2;"
        "Object.keys(o).length");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

// ============================================================
// VM: Object builtin tests
// ============================================================

// OB-01: Object.keys({a:1, b:2}) → length=2, contains "a" and "b"
TEST(VMObjectBuiltin, KeysTwoProps) {
    auto len = vm_ok("Object.keys({a:1, b:2}).length");
    EXPECT_TRUE(len.is_number());
    EXPECT_EQ(len.as_number(), 2.0);

    auto k0 = vm_ok("Object.keys({a:1, b:2})[0]");
    EXPECT_TRUE(k0.is_string());
    EXPECT_EQ(k0.as_string(), "a");

    auto k1 = vm_ok("Object.keys({a:1, b:2})[1]");
    EXPECT_TRUE(k1.is_string());
    EXPECT_EQ(k1.as_string(), "b");
}

// OB-02: Object.keys({}) → empty array length=0
TEST(VMObjectBuiltin, KeysEmpty) {
    auto v = vm_ok("Object.keys({}).length");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}

// OB-03: Object.keys({b:2, a:1}) → ["b","a"] (insertion order)
TEST(VMObjectBuiltin, KeysInsertionOrder) {
    auto k0 = vm_ok("Object.keys({b:2, a:1})[0]");
    EXPECT_TRUE(k0.is_string());
    EXPECT_EQ(k0.as_string(), "b");

    auto k1 = vm_ok("Object.keys({b:2, a:1})[1]");
    EXPECT_TRUE(k1.is_string());
    EXPECT_EQ(k1.as_string(), "a");
}

// OB-04: Object.keys(null) → TypeError
TEST(VMObjectBuiltin, KeysNullThrows) {
    EXPECT_TRUE(vm_err("Object.keys(null)"));
}

// OB-05: Object.keys(undefined) → TypeError
TEST(VMObjectBuiltin, KeysUndefinedThrows) {
    EXPECT_TRUE(vm_err("Object.keys(undefined)"));
}

// OB-06: Object.keys(42) → TypeError
TEST(VMObjectBuiltin, KeysNumberThrows) {
    EXPECT_TRUE(vm_err("Object.keys(42)"));
}

// OB-07: inherited properties not included
TEST(VMObjectBuiltin, KeysNoInherited) {
    auto v = vm_ok("var o = Object.create({x:1}); Object.keys(o).length");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}

// OB-08: Object.assign({}, {a:1}).a === 1
TEST(VMObjectBuiltin, AssignBasic) {
    auto v = vm_ok("Object.assign({}, {a:1}).a");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

// OB-09: Object.assign({a:1}, {a:2}).a === 2 (overwrite)
TEST(VMObjectBuiltin, AssignOverwrite) {
    auto v = vm_ok("Object.assign({a:1}, {a:2}).a");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.0);
}

// OB-10: null source skipped
TEST(VMObjectBuiltin, AssignNullSource) {
    auto v = vm_ok("Object.assign({}, null, {b:2}).b");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.0);
}

// OB-11: undefined source skipped
TEST(VMObjectBuiltin, AssignUndefinedSource) {
    auto v = vm_ok("Object.assign({}, undefined, {b:2}).b");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.0);
}

// OB-12: multiple sources merged
TEST(VMObjectBuiltin, AssignMultipleSources) {
    auto a = vm_ok("Object.assign({}, {a:1}, {b:2}).a");
    EXPECT_TRUE(a.is_number());
    EXPECT_EQ(a.as_number(), 1.0);

    auto b = vm_ok("Object.assign({}, {a:1}, {b:2}).b");
    EXPECT_TRUE(b.is_number());
    EXPECT_EQ(b.as_number(), 2.0);
}

// OB-13: Object.assign(null, {}) → TypeError
TEST(VMObjectBuiltin, AssignNullTargetThrows) {
    EXPECT_TRUE(vm_err("Object.assign(null, {})"));
}

// OB-14: returns target itself
TEST(VMObjectBuiltin, AssignReturnsTarget) {
    auto v = vm_ok("var t = {}; Object.assign(t, {a:1}); t.a");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

// OB-15: Object.create({x:1}).x === 1 (prototype chain lookup)
TEST(VMObjectBuiltin, CreateProtoLookup) {
    auto v = vm_ok("Object.create({x:1}).x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

// OB-16: Object.create(null) → object with no prototype
TEST(VMObjectBuiltin, CreateNullProto) {
    auto v = vm_ok("var o = Object.create(null); o.x");
    EXPECT_TRUE(v.is_undefined());
}

// OB-17: Object.create(undefined) → TypeError
TEST(VMObjectBuiltin, CreateUndefinedThrows) {
    EXPECT_TRUE(vm_err("Object.create(undefined)"));
}

// OB-18: Object.create(42) → TypeError
TEST(VMObjectBuiltin, CreateNumberThrows) {
    EXPECT_TRUE(vm_err("Object.create(42)"));
}

// OB-19: Object.create("str") → TypeError
TEST(VMObjectBuiltin, CreateStringThrows) {
    EXPECT_TRUE(vm_err(R"(Object.create("str"))"));
}

// OB-20: Object.create(null) own keys === 0
TEST(VMObjectBuiltin, CreateNullProtoOwnKeys) {
    auto v = vm_ok("Object.keys(Object.create(null)).length");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}

// OB-21: Object.create({}) instanceof Object → true
TEST(VMObjectBuiltin, CreateInstanceof) {
    auto v = vm_ok("Object.create({}) instanceof Object");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// OB-22: Object.keys(false) → TypeError (bool is not an object)
TEST(VMObjectBuiltin, KeysBoolThrows) {
    EXPECT_TRUE(vm_err("Object.keys(false)"));
}

// OB-23: Object.keys on a function object → empty array (kFunction branch)
TEST(VMObjectBuiltin, KeysFunctionObject) {
    auto v = vm_ok("Object.keys(function(){}).length");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}

// OB-24: Object.keys on an array → numeric index strings in order
TEST(VMObjectBuiltin, KeysArray) {
    auto len = vm_ok("Object.keys([10,20,30]).length");
    EXPECT_TRUE(len.is_number());
    EXPECT_EQ(len.as_number(), 3.0);

    auto k0 = vm_ok("Object.keys([10,20,30])[0]");
    EXPECT_TRUE(k0.is_string());
    EXPECT_EQ(k0.as_string(), "0");

    auto k2 = vm_ok("Object.keys([10,20,30])[2]");
    EXPECT_TRUE(k2.is_string());
    EXPECT_EQ(k2.as_string(), "2");
}

// OB-25: Object.keys result elements are strings (type regression)
TEST(VMObjectBuiltin, KeysResultElementType) {
    auto v = vm_ok("typeof Object.keys({x:1})[0]");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "string");
}

// OB-26: Object.assign({}) with no sources → returns target (empty loop)
TEST(VMObjectBuiltin, AssignNoSources) {
    auto v = vm_ok("var t = {a:1}; Object.assign(t); t.a");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

// OB-27: Object.assign(undefined, {}) → TypeError
TEST(VMObjectBuiltin, AssignUndefinedTargetThrows) {
    EXPECT_TRUE(vm_err("Object.assign(undefined, {})"));
}

// OB-28: Object.assign skips non-null/undefined primitive source (number)
TEST(VMObjectBuiltin, AssignNumberSourceSkipped) {
    auto v = vm_ok("Object.assign({a:1}, 42).a");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

// OB-29: Object.assign returns the exact target reference (identity)
TEST(VMObjectBuiltin, AssignReturnsSameTarget) {
    auto v = vm_ok("var t = {}; Object.assign(t, {a:1}) === t");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// OB-30: Object.assign does not copy inherited (prototype) properties
TEST(VMObjectBuiltin, AssignNoInheritedProps) {
    auto v = vm_ok(
        "var proto = {inherited:99};"
        "var src = Object.create(proto);"
        "src.own = 1;"
        "var t = Object.assign({}, src);"
        "t.own === 1 && t.inherited === undefined");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// OB-31: Object.create(null) → instanceof Object is false (no prototype chain)
TEST(VMObjectBuiltin, CreateNullProtoNotInstanceof) {
    auto v = vm_ok("Object.create(null) instanceof Object");
    EXPECT_TRUE(v.is_bool());
    EXPECT_FALSE(v.as_bool());
}

// OB-32: Object.create(false) → TypeError (bool is not object or null)
TEST(VMObjectBuiltin, CreateBoolThrows) {
    EXPECT_TRUE(vm_err("Object.create(false)"));
}

// OB-33: Object.create with second argument (propertiesObject) silently ignored
TEST(VMObjectBuiltin, CreateSecondArgIgnored) {
    auto v = vm_ok("Object.create({x:1}, {y:{value:2}}).x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

// OB-34: multi-level prototype chain via Object.create
TEST(VMObjectBuiltin, CreateMultiLevelChain) {
    auto v = vm_ok(
        "var a = {x:1};"
        "var b = Object.create(a);"
        "var c = Object.create(b);"
        "c.x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

// OB-35: Object.keys does not include prototype properties of created object
TEST(VMObjectBuiltin, KeysExcludesProtoAfterCreate) {
    auto v = vm_ok(
        "var o = Object.create({inherited:1});"
        "o.own = 2;"
        "Object.keys(o).length");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

// ============================================================
// M1: Object() / new Object() prototype chain
// ============================================================

// OB-36 (Interp): new Object() instanceof Object → true
TEST(InterpObjectBuiltin, NewObjectInstanceof) {
    auto v = interp_ok("new Object() instanceof Object");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// OB-37 (Interp): Object(null) returns a new object (not null)
TEST(InterpObjectBuiltin, ObjectNullReturnsObject) {
    auto v = interp_ok("typeof Object(null)");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "object");
}

// OB-36 (VM): new Object() instanceof Object → true
TEST(VMObjectBuiltin, NewObjectInstanceof) {
    auto v = vm_ok("new Object() instanceof Object");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// OB-37 (VM): Object(null) returns a new object (not null)
TEST(VMObjectBuiltin, ObjectNullReturnsObject) {
    auto v = vm_ok("typeof Object(null)");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "object");
}

// ============================================================
// M2: Object.create with function as prototype → TypeError
// ============================================================

// OB-38 (Interp): Object.create(function(){}) → TypeError
TEST(InterpObjectBuiltin, CreateFunctionProtoThrows) {
    EXPECT_TRUE(interp_err("Object.create(function(){})"));
}

// OB-38 (VM): Object.create(function(){}) → TypeError
TEST(VMObjectBuiltin, CreateFunctionProtoThrows) {
    EXPECT_TRUE(vm_err("Object.create(function(){})"));
}

// ============================================================
// M3: Object.assign to array target
// ============================================================

// OB-39 (Interp): Object.assign(arr, {0:9}); arr[0] → 9
TEST(InterpObjectBuiltin, AssignToArrayElement) {
    auto v = interp_ok("var arr = [1,2,3]; Object.assign(arr, {0:9}); arr[0]");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 9.0);
}

// OB-39 (VM): Object.assign(arr, {0:9}); arr[0] → 9
TEST(VMObjectBuiltin, AssignToArrayElement) {
    auto v = vm_ok("var arr = [1,2,3]; Object.assign(arr, {0:9}); arr[0]");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 9.0);
}

}  // namespace
