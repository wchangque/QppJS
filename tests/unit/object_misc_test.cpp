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

// OM-01: Object.freeze({x:1}).x → 1 (value readable after freeze)
TEST(ObjectMiscInterp, OM01_FreezeValueReadable) {
    EXPECT_EQ(interp_ok("Object.freeze({x:1}).x").as_number(), 1.0);
}
TEST(ObjectMiscVM, OM01_FreezeValueReadable) {
    EXPECT_EQ(vm_ok("Object.freeze({x:1}).x").as_number(), 1.0);
}

// OM-02: frozen object assignment silently fails (sloppy mode)
TEST(ObjectMiscInterp, OM02_FrozenAssignSilent) {
    auto v = interp_ok(R"(
        var o = Object.freeze({x: 1});
        o.x = 99;
        o.x
    )");
    EXPECT_EQ(v.as_number(), 1.0);
}
TEST(ObjectMiscVM, OM02_FrozenAssignSilent) {
    auto v = vm_ok(R"(
        var o = Object.freeze({x: 1});
        o.x = 99;
        o.x
    )");
    EXPECT_EQ(v.as_number(), 1.0);
}

// OM-03: Object.isFrozen({}) → false (fresh empty object is not frozen)
TEST(ObjectMiscInterp, OM03_IsNotFrozenEmpty) {
    auto v = interp_ok("Object.isFrozen({})");
    EXPECT_TRUE(v.is_bool() && !v.as_bool());
}
TEST(ObjectMiscVM, OM03_IsNotFrozenEmpty) {
    auto v = vm_ok("Object.isFrozen({})");
    EXPECT_TRUE(v.is_bool() && !v.as_bool());
}

// OM-04: Object.isFrozen(Object.freeze({})) → true
TEST(ObjectMiscInterp, OM04_IsFrozenAfterFreeze) {
    auto v = interp_ok("Object.isFrozen(Object.freeze({}))");
    EXPECT_TRUE(v.is_bool() && v.as_bool());
}
TEST(ObjectMiscVM, OM04_IsFrozenAfterFreeze) {
    auto v = vm_ok("Object.isFrozen(Object.freeze({}))");
    EXPECT_TRUE(v.is_bool() && v.as_bool());
}

// OM-05: sealed object still allows write to existing writable data property
TEST(ObjectMiscInterp, OM05_SealAllowsWrite) {
    auto v = interp_ok(R"(
        var o = Object.seal({x: 1});
        o.x = 42;
        o.x
    )");
    EXPECT_EQ(v.as_number(), 42.0);
}
TEST(ObjectMiscVM, OM05_SealAllowsWrite) {
    auto v = vm_ok(R"(
        var o = Object.seal({x: 1});
        o.x = 42;
        o.x
    )");
    EXPECT_EQ(v.as_number(), 42.0);
}

// OM-06: Object.isSealed({}) → false
TEST(ObjectMiscInterp, OM06_IsNotSealedEmpty) {
    auto v = interp_ok("Object.isSealed({})");
    EXPECT_TRUE(v.is_bool() && !v.as_bool());
}
TEST(ObjectMiscVM, OM06_IsNotSealedEmpty) {
    auto v = vm_ok("Object.isSealed({})");
    EXPECT_TRUE(v.is_bool() && !v.as_bool());
}

// OM-07: Object.isSealed(Object.seal({})) → true
TEST(ObjectMiscInterp, OM07_IsSealedAfterSeal) {
    auto v = interp_ok("Object.isSealed(Object.seal({}))");
    EXPECT_TRUE(v.is_bool() && v.as_bool());
}
TEST(ObjectMiscVM, OM07_IsSealedAfterSeal) {
    auto v = vm_ok("Object.isSealed(Object.seal({}))");
    EXPECT_TRUE(v.is_bool() && v.as_bool());
}

// OM-08: Object.isFrozen(42) → true (primitives are always frozen per ES2015+)
TEST(ObjectMiscInterp, OM08_PrimitivesAreFrozen) {
    auto v = interp_ok("Object.isFrozen(42)");
    EXPECT_TRUE(v.is_bool() && v.as_bool());
}
TEST(ObjectMiscVM, OM08_PrimitivesAreFrozen) {
    auto v = vm_ok("Object.isFrozen(42)");
    EXPECT_TRUE(v.is_bool() && v.as_bool());
}

// OM-09: freeze returns the same object
TEST(ObjectMiscInterp, OM09_FreezeReturnsSameObject) {
    auto v = interp_ok(R"(
        var o = {x: 1};
        Object.freeze(o) === o
    )");
    EXPECT_TRUE(v.is_bool() && v.as_bool());
}
TEST(ObjectMiscVM, OM09_FreezeReturnsSameObject) {
    auto v = vm_ok(R"(
        var o = {x: 1};
        Object.freeze(o) === o
    )");
    EXPECT_TRUE(v.is_bool() && v.as_bool());
}

// OM-10: seal returns the same object
TEST(ObjectMiscInterp, OM10_SealReturnsSameObject) {
    auto v = interp_ok(R"(
        var o = {x: 1};
        Object.seal(o) === o
    )");
    EXPECT_TRUE(v.is_bool() && v.as_bool());
}
TEST(ObjectMiscVM, OM10_SealReturnsSameObject) {
    auto v = vm_ok(R"(
        var o = {x: 1};
        Object.seal(o) === o
    )");
    EXPECT_TRUE(v.is_bool() && v.as_bool());
}

// OM-11: Object.isSealed(42) → true (primitives are always sealed)
TEST(ObjectMiscInterp, OM11_PrimitivesAreSealed) {
    auto v = interp_ok("Object.isSealed(42)");
    EXPECT_TRUE(v.is_bool() && v.as_bool());
}
TEST(ObjectMiscVM, OM11_PrimitivesAreSealed) {
    auto v = vm_ok("Object.isSealed(42)");
    EXPECT_TRUE(v.is_bool() && v.as_bool());
}

// OM-12: frozen object cannot add new property (sloppy mode: silently ignored)
TEST(ObjectMiscInterp, OM12_FrozenNoNewProp) {
    auto v = interp_ok(R"(
        var o = Object.freeze({});
        o.newProp = 1;
        typeof o.newProp
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "undefined");
}
TEST(ObjectMiscVM, OM12_FrozenNoNewProp) {
    auto v = vm_ok(R"(
        var o = Object.freeze({});
        o.newProp = 1;
        typeof o.newProp
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "undefined");
}

// OM-13: sealed object cannot add new property (sloppy mode: silently ignored)
TEST(ObjectMiscInterp, OM13_SealedNoNewProp) {
    auto v = interp_ok(R"(
        var o = Object.seal({});
        o.newProp = 1;
        typeof o.newProp
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "undefined");
}
TEST(ObjectMiscVM, OM13_SealedNoNewProp) {
    auto v = vm_ok(R"(
        var o = Object.seal({});
        o.newProp = 1;
        typeof o.newProp
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "undefined");
}

// OM-14: isFrozen on non-frozen object with property → false
TEST(ObjectMiscInterp, OM14_IsNotFrozenWithProp) {
    auto v = interp_ok("Object.isFrozen({x:1})");
    EXPECT_TRUE(v.is_bool() && !v.as_bool());
}
TEST(ObjectMiscVM, OM14_IsNotFrozenWithProp) {
    auto v = vm_ok("Object.isFrozen({x:1})");
    EXPECT_TRUE(v.is_bool() && !v.as_bool());
}

// OM-15: isFrozen on frozen object with property → true
TEST(ObjectMiscInterp, OM15_IsFrozenWithProp) {
    auto v = interp_ok("Object.isFrozen(Object.freeze({x:1}))");
    EXPECT_TRUE(v.is_bool() && v.as_bool());
}
TEST(ObjectMiscVM, OM15_IsFrozenWithProp) {
    auto v = vm_ok("Object.isFrozen(Object.freeze({x:1}))");
    EXPECT_TRUE(v.is_bool() && v.as_bool());
}

}  // namespace
