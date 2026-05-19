#include "qppjs/frontend/parser.h"
#include "qppjs/runtime/interpreter.h"
#include "qppjs/vm/compiler.h"
#include "qppjs/vm/vm.h"

#include <gtest/gtest.h>
#include <string>
#include <string_view>

using namespace qppjs;

namespace {

// ---- Interpreter helpers ----

static EvalResult interp_run(std::string_view src) {
    auto parse_result = parse_program(src);
    EXPECT_TRUE(parse_result.ok()) << "parse failed: " << parse_result.error().message();
    if (!parse_result.ok()) return EvalResult::err(parse_result.error());
    Interpreter interp;
    return interp.exec(parse_result.value());
}

static std::string interp_str(std::string_view src) {
    auto r = interp_run(src);
    if (!r.is_ok()) return "<error:" + r.error().message() + ">";
    const Value& v = r.value();
    if (v.is_string()) return v.as_string();
    if (v.is_undefined()) return "undefined";
    if (v.is_null()) return "null";
    if (v.is_bool()) return v.as_bool() ? "true" : "false";
    if (v.is_number()) {
        double n = v.as_number();
        if (n == static_cast<double>(static_cast<long long>(n))) {
            return std::to_string(static_cast<long long>(n));
        }
        return std::to_string(n);
    }
    return "<object>";
}

static bool interp_throws(std::string_view src) {
    auto r = interp_run(src);
    return !r.is_ok();
}

// ---- VM helpers ----

static EvalResult vm_run(std::string_view src) {
    auto parse_result = parse_program(src);
    EXPECT_TRUE(parse_result.ok()) << "parse failed: " << parse_result.error().message();
    if (!parse_result.ok()) return EvalResult::err(parse_result.error());
    Compiler compiler;
    auto bc = compiler.compile(parse_result.value());
    VM vm;
    return vm.exec(bc);
}

static std::string vm_str(std::string_view src) {
    auto r = vm_run(src);
    if (!r.is_ok()) return "<error:" + r.error().message() + ">";
    const Value& v = r.value();
    if (v.is_string()) return v.as_string();
    if (v.is_undefined()) return "undefined";
    if (v.is_null()) return "null";
    if (v.is_bool()) return v.as_bool() ? "true" : "false";
    if (v.is_number()) {
        double n = v.as_number();
        if (n == static_cast<double>(static_cast<long long>(n))) {
            return std::to_string(static_cast<long long>(n));
        }
        return std::to_string(n);
    }
    return "<object>";
}

static bool vm_throws(std::string_view src) {
    auto r = vm_run(src);
    return !r.is_ok();
}

// ---- DP-01: defineProperty 新属性默认值（writable/enum/config 均为 false）----

TEST(DefinePropInterp, DP01_DefaultDescriptor) {
    // defineProperty with {value:42} — writable/enumerable/configurable all default false
    EXPECT_EQ(interp_str(R"(
        var o = {};
        Object.defineProperty(o, 'x', { value: 42 });
        o.x
    )"), "42");
    // writable: false — write is silently ignored
    EXPECT_EQ(interp_str(R"(
        var o = {};
        Object.defineProperty(o, 'x', { value: 42 });
        o.x = 99;
        o.x
    )"), "42");
    // enumerable: false — not in Object.keys
    EXPECT_EQ(interp_str(R"(
        var o = {};
        Object.defineProperty(o, 'x', { value: 42 });
        Object.keys(o).length
    )"), "0");
}

TEST(DefinePropVM, DP01_DefaultDescriptor) {
    EXPECT_EQ(vm_str(R"(
        var o = {};
        Object.defineProperty(o, 'x', { value: 42 });
        o.x
    )"), "42");
    EXPECT_EQ(vm_str(R"(
        var o = {};
        Object.defineProperty(o, 'x', { value: 42 });
        o.x = 99;
        o.x
    )"), "42");
    EXPECT_EQ(vm_str(R"(
        var o = {};
        Object.defineProperty(o, 'x', { value: 42 });
        Object.keys(o).length
    )"), "0");
}

// ---- DP-02: defineProperty 完整 data descriptor（可读写可配置）----

TEST(DefinePropInterp, DP02_FullDataDescriptor) {
    EXPECT_EQ(interp_str(R"(
        var o = {};
        Object.defineProperty(o, 'x', { value: 1, writable: true, enumerable: true, configurable: true });
        o.x = 2;
        o.x
    )"), "2");
    EXPECT_EQ(interp_str(R"(
        var o = {};
        Object.defineProperty(o, 'x', { value: 1, writable: true, enumerable: true, configurable: true });
        Object.keys(o).length
    )"), "1");
}

TEST(DefinePropVM, DP02_FullDataDescriptor) {
    EXPECT_EQ(vm_str(R"(
        var o = {};
        Object.defineProperty(o, 'x', { value: 1, writable: true, enumerable: true, configurable: true });
        o.x = 2;
        o.x
    )"), "2");
    EXPECT_EQ(vm_str(R"(
        var o = {};
        Object.defineProperty(o, 'x', { value: 1, writable: true, enumerable: true, configurable: true });
        Object.keys(o).length
    )"), "1");
}

// ---- DP-03: non-writable 属性赋值不改变 value（sloppy mode 静默忽略）----

TEST(DefinePropInterp, DP03_NonWritableAssign) {
    // In sloppy mode, assignment to non-writable silently fails
    EXPECT_EQ(interp_str(R"(
        var o = {};
        Object.defineProperty(o, 'x', { value: 10, writable: false, enumerable: true, configurable: true });
        o.x = 99;
        o.x
    )"), "10");
    // Also verify the assignment doesn't throw
    EXPECT_FALSE(interp_throws(R"(
        var o = {};
        Object.defineProperty(o, 'x', { value: 10, writable: false, enumerable: true, configurable: true });
        o.x = 99;
    )"));
}

TEST(DefinePropVM, DP03_NonWritableAssign) {
    EXPECT_EQ(vm_str(R"(
        var o = {};
        Object.defineProperty(o, 'x', { value: 10, writable: false, enumerable: true, configurable: true });
        o.x = 99;
        o.x
    )"), "10");
    EXPECT_FALSE(vm_throws(R"(
        var o = {};
        Object.defineProperty(o, 'x', { value: 10, writable: false, enumerable: true, configurable: true });
        o.x = 99;
    )"));
}

// ---- DP-04: non-enumerable 属性不被 Object.keys 返回 ----

TEST(DefinePropInterp, DP04_NonEnumerable) {
    EXPECT_EQ(interp_str(R"(
        var o = { a: 1 };
        Object.defineProperty(o, 'hidden', { value: 42, writable: true, enumerable: false, configurable: true });
        Object.keys(o).length
    )"), "1");
    EXPECT_EQ(interp_str(R"(
        var o = { a: 1 };
        Object.defineProperty(o, 'hidden', { value: 42, writable: true, enumerable: false, configurable: true });
        o.hidden
    )"), "42");
}

TEST(DefinePropVM, DP04_NonEnumerable) {
    EXPECT_EQ(vm_str(R"(
        var o = { a: 1 };
        Object.defineProperty(o, 'hidden', { value: 42, writable: true, enumerable: false, configurable: true });
        Object.keys(o).length
    )"), "1");
    EXPECT_EQ(vm_str(R"(
        var o = { a: 1 };
        Object.defineProperty(o, 'hidden', { value: 42, writable: true, enumerable: false, configurable: true });
        o.hidden
    )"), "42");
}

// ---- DP-05: non-configurable + delete → false ----

TEST(DefinePropInterp, DP05_NonConfigurableDelete) {
    EXPECT_EQ(interp_str(R"(
        var o = {};
        Object.defineProperty(o, 'x', { value: 1, writable: true, enumerable: true, configurable: false });
        delete o.x
    )"), "false");
    // value still accessible after failed delete
    EXPECT_EQ(interp_str(R"(
        var o = {};
        Object.defineProperty(o, 'x', { value: 1, writable: true, enumerable: true, configurable: false });
        delete o.x;
        o.x
    )"), "1");
}

TEST(DefinePropVM, DP05_NonConfigurableDelete) {
    EXPECT_EQ(vm_str(R"(
        var o = {};
        Object.defineProperty(o, 'x', { value: 1, writable: true, enumerable: true, configurable: false });
        delete o.x
    )"), "false");
    EXPECT_EQ(vm_str(R"(
        var o = {};
        Object.defineProperty(o, 'x', { value: 1, writable: true, enumerable: true, configurable: false });
        delete o.x;
        o.x
    )"), "1");
}

// ---- DP-06: getOwnPropertyDescriptor 返回正确 data descriptor ----

TEST(DefinePropInterp, DP06_GetOwnPropDescData) {
    EXPECT_EQ(interp_str(R"(
        var o = {};
        Object.defineProperty(o, 'x', { value: 7, writable: true, enumerable: false, configurable: true });
        var d = Object.getOwnPropertyDescriptor(o, 'x');
        d.value
    )"), "7");
    EXPECT_EQ(interp_str(R"(
        var o = {};
        Object.defineProperty(o, 'x', { value: 7, writable: true, enumerable: false, configurable: true });
        var d = Object.getOwnPropertyDescriptor(o, 'x');
        d.writable
    )"), "true");
    EXPECT_EQ(interp_str(R"(
        var o = {};
        Object.defineProperty(o, 'x', { value: 7, writable: true, enumerable: false, configurable: true });
        var d = Object.getOwnPropertyDescriptor(o, 'x');
        d.enumerable
    )"), "false");
    EXPECT_EQ(interp_str(R"(
        var o = {};
        Object.defineProperty(o, 'x', { value: 7, writable: true, enumerable: false, configurable: true });
        var d = Object.getOwnPropertyDescriptor(o, 'x');
        d.configurable
    )"), "true");
}

TEST(DefinePropVM, DP06_GetOwnPropDescData) {
    EXPECT_EQ(vm_str(R"(
        var o = {};
        Object.defineProperty(o, 'x', { value: 7, writable: true, enumerable: false, configurable: true });
        var d = Object.getOwnPropertyDescriptor(o, 'x');
        d.value
    )"), "7");
    EXPECT_EQ(vm_str(R"(
        var o = {};
        Object.defineProperty(o, 'x', { value: 7, writable: true, enumerable: false, configurable: true });
        var d = Object.getOwnPropertyDescriptor(o, 'x');
        d.writable
    )"), "true");
    EXPECT_EQ(vm_str(R"(
        var o = {};
        Object.defineProperty(o, 'x', { value: 7, writable: true, enumerable: false, configurable: true });
        var d = Object.getOwnPropertyDescriptor(o, 'x');
        d.enumerable
    )"), "false");
    EXPECT_EQ(vm_str(R"(
        var o = {};
        Object.defineProperty(o, 'x', { value: 7, writable: true, enumerable: false, configurable: true });
        var d = Object.getOwnPropertyDescriptor(o, 'x');
        d.configurable
    )"), "true");
}

// ---- DP-07: getOwnPropertyDescriptor 属性不存在 → undefined ----

TEST(DefinePropInterp, DP07_GetOwnPropDescMissing) {
    EXPECT_EQ(interp_str(R"(
        var o = {};
        Object.getOwnPropertyDescriptor(o, 'missing')
    )"), "undefined");
}

TEST(DefinePropVM, DP07_GetOwnPropDescMissing) {
    EXPECT_EQ(vm_str(R"(
        var o = {};
        Object.getOwnPropertyDescriptor(o, 'missing')
    )"), "undefined");
}

// ---- DP-08: defineProperty accessor（get only）— 读取调用 getter ----
// Note: In sloppy mode, write to get-only accessor silently fails (no throw).

TEST(DefinePropInterp, DP08_AccessorGetOnly) {
    EXPECT_EQ(interp_str(R"(
        var o = {};
        Object.defineProperty(o, 'x', {
            get: function() { return 42; },
            enumerable: true,
            configurable: true
        });
        o.x
    )"), "42");
    // In sloppy mode, writing to get-only accessor silently fails
    EXPECT_FALSE(interp_throws(R"(
        var o = {};
        Object.defineProperty(o, 'x', {
            get: function() { return 42; },
            enumerable: true,
            configurable: true
        });
        o.x = 1;
    )"));
    // Value unchanged after failed write
    EXPECT_EQ(interp_str(R"(
        var o = {};
        Object.defineProperty(o, 'x', {
            get: function() { return 42; },
            enumerable: true,
            configurable: true
        });
        o.x = 1;
        o.x
    )"), "42");
}

TEST(DefinePropVM, DP08_AccessorGetOnly) {
    EXPECT_EQ(vm_str(R"(
        var o = {};
        Object.defineProperty(o, 'x', {
            get: function() { return 42; },
            enumerable: true,
            configurable: true
        });
        o.x
    )"), "42");
    EXPECT_FALSE(vm_throws(R"(
        var o = {};
        Object.defineProperty(o, 'x', {
            get: function() { return 42; },
            enumerable: true,
            configurable: true
        });
        o.x = 1;
    )"));
    EXPECT_EQ(vm_str(R"(
        var o = {};
        Object.defineProperty(o, 'x', {
            get: function() { return 42; },
            enumerable: true,
            configurable: true
        });
        o.x = 1;
        o.x
    )"), "42");
}

// ---- DP-09: defineProperty accessor（get+set）— 读写均正确 ----

TEST(DefinePropInterp, DP09_AccessorGetSet) {
    EXPECT_EQ(interp_str(R"(
        var store = 0;
        var o = {};
        Object.defineProperty(o, 'x', {
            get: function() { return store; },
            set: function(v) { store = v * 2; },
            enumerable: true,
            configurable: true
        });
        o.x = 5;
        o.x
    )"), "10");
}

TEST(DefinePropVM, DP09_AccessorGetSet) {
    EXPECT_EQ(vm_str(R"(
        var store = 0;
        var o = {};
        Object.defineProperty(o, 'x', {
            get: function() { return store; },
            set: function(v) { store = v * 2; },
            enumerable: true,
            configurable: true
        });
        o.x = 5;
        o.x
    )"), "10");
}

// ---- DP-10: preventExtensions 后不能通过 defineProperty 添加新属性（TypeError）----
// Note: In sloppy mode, obj.b = 2 silently fails; only Object.defineProperty throws.

TEST(DefinePropInterp, DP10_PreventExtensions) {
    // Object.defineProperty on non-extensible object throws TypeError
    EXPECT_TRUE(interp_throws(R"(
        var o = { a: 1 };
        Object.preventExtensions(o);
        Object.defineProperty(o, 'b', { value: 2 });
    )"));
    // Existing property can still be modified (writable)
    EXPECT_EQ(interp_str(R"(
        var o = { a: 1 };
        Object.preventExtensions(o);
        o.a = 99;
        o.a
    )"), "99");
    // Sloppy assignment of new property silently ignored (property not added)
    EXPECT_EQ(interp_str(R"(
        var o = { a: 1 };
        Object.preventExtensions(o);
        o.b = 2;
        o.b
    )"), "undefined");
}

TEST(DefinePropVM, DP10_PreventExtensions) {
    EXPECT_TRUE(vm_throws(R"(
        var o = { a: 1 };
        Object.preventExtensions(o);
        Object.defineProperty(o, 'b', { value: 2 });
    )"));
    EXPECT_EQ(vm_str(R"(
        var o = { a: 1 };
        Object.preventExtensions(o);
        o.a = 99;
        o.a
    )"), "99");
    EXPECT_EQ(vm_str(R"(
        var o = { a: 1 };
        Object.preventExtensions(o);
        o.b = 2;
        o.b
    )"), "undefined");
}

// ---- DP-11: non-configurable 不能改 configurable→true → TypeError ----

TEST(DefinePropInterp, DP11_NonConfigurableToTrue) {
    EXPECT_TRUE(interp_throws(R"(
        var o = {};
        Object.defineProperty(o, 'x', { value: 1, configurable: false });
        Object.defineProperty(o, 'x', { configurable: true });
    )"));
}

TEST(DefinePropVM, DP11_NonConfigurableToTrue) {
    EXPECT_TRUE(vm_throws(R"(
        var o = {};
        Object.defineProperty(o, 'x', { value: 1, configurable: false });
        Object.defineProperty(o, 'x', { configurable: true });
    )"));
}

// ---- DP-12: non-configurable 不能改 enumerable → TypeError ----

TEST(DefinePropInterp, DP12_NonConfigurableEnumerableChange) {
    EXPECT_TRUE(interp_throws(R"(
        var o = {};
        Object.defineProperty(o, 'x', { value: 1, enumerable: false, configurable: false });
        Object.defineProperty(o, 'x', { enumerable: true });
    )"));
}

TEST(DefinePropVM, DP12_NonConfigurableEnumerableChange) {
    EXPECT_TRUE(vm_throws(R"(
        var o = {};
        Object.defineProperty(o, 'x', { value: 1, enumerable: false, configurable: false });
        Object.defineProperty(o, 'x', { enumerable: true });
    )"));
}

// ---- DP-13: non-configurable + non-writable 不能改 value → TypeError ----

TEST(DefinePropInterp, DP13_NonWritableValueChange) {
    EXPECT_TRUE(interp_throws(R"(
        var o = {};
        Object.defineProperty(o, 'x', { value: 1, writable: false, configurable: false });
        Object.defineProperty(o, 'x', { value: 2 });
    )"));
}

TEST(DefinePropVM, DP13_NonWritableValueChange) {
    EXPECT_TRUE(vm_throws(R"(
        var o = {};
        Object.defineProperty(o, 'x', { value: 1, writable: false, configurable: false });
        Object.defineProperty(o, 'x', { value: 2 });
    )"));
}

// ---- DP-14: non-configurable + writable:true 可改为 writable:false（单向）----

TEST(DefinePropInterp, DP14_WritableFalseAllowed) {
    // writable: true -> false is allowed even when non-configurable
    EXPECT_EQ(interp_str(R"(
        var o = {};
        Object.defineProperty(o, 'x', { value: 1, writable: true, configurable: false });
        Object.defineProperty(o, 'x', { writable: false });
        var d = Object.getOwnPropertyDescriptor(o, 'x');
        d.writable
    )"), "false");
}

TEST(DefinePropVM, DP14_WritableFalseAllowed) {
    EXPECT_EQ(vm_str(R"(
        var o = {};
        Object.defineProperty(o, 'x', { value: 1, writable: true, configurable: false });
        Object.defineProperty(o, 'x', { writable: false });
        var d = Object.getOwnPropertyDescriptor(o, 'x');
        d.writable
    )"), "false");
}

// ---- DP-15: non-configurable + non-writable 不能 writable false→true → TypeError ----

TEST(DefinePropInterp, DP15_WritableTrueOnNonConfigurable) {
    EXPECT_TRUE(interp_throws(R"(
        var o = {};
        Object.defineProperty(o, 'x', { value: 1, writable: false, configurable: false });
        Object.defineProperty(o, 'x', { writable: true });
    )"));
}

TEST(DefinePropVM, DP15_WritableTrueOnNonConfigurable) {
    EXPECT_TRUE(vm_throws(R"(
        var o = {};
        Object.defineProperty(o, 'x', { value: 1, writable: false, configurable: false });
        Object.defineProperty(o, 'x', { writable: true });
    )"));
}

// ---- DP-16: data 属性不能改为 accessor（non-configurable）→ TypeError ----

TEST(DefinePropInterp, DP16_DataToAccessorNonConfigurable) {
    EXPECT_TRUE(interp_throws(R"(
        var o = {};
        Object.defineProperty(o, 'x', { value: 1, configurable: false });
        Object.defineProperty(o, 'x', { get: function() { return 2; } });
    )"));
}

TEST(DefinePropVM, DP16_DataToAccessorNonConfigurable) {
    EXPECT_TRUE(vm_throws(R"(
        var o = {};
        Object.defineProperty(o, 'x', { value: 1, configurable: false });
        Object.defineProperty(o, 'x', { get: function() { return 2; } });
    )"));
}

// ---- DP-17: 空 descriptor {} → 无操作 ----

TEST(DefinePropInterp, DP17_EmptyDescriptorNoOp) {
    EXPECT_EQ(interp_str(R"(
        var o = {};
        Object.defineProperty(o, 'x', { value: 5, writable: true, enumerable: true, configurable: true });
        Object.defineProperty(o, 'x', {});
        o.x
    )"), "5");
}

TEST(DefinePropVM, DP17_EmptyDescriptorNoOp) {
    EXPECT_EQ(vm_str(R"(
        var o = {};
        Object.defineProperty(o, 'x', { value: 5, writable: true, enumerable: true, configurable: true });
        Object.defineProperty(o, 'x', {});
        o.x
    )"), "5");
}

// ---- DP-18: SameValue NaN（不抛）----

TEST(DefinePropInterp, DP18_SameValueNaN) {
    // Setting NaN to NaN should not throw (SameValue NaN==NaN)
    EXPECT_EQ(interp_str(R"(
        var o = {};
        Object.defineProperty(o, 'x', { value: NaN, writable: false, configurable: false });
        Object.defineProperty(o, 'x', { value: NaN });
        o.x
    )"), "nan");  // NaN displayed as nan (platform dependent)
}

TEST(DefinePropVM, DP18_SameValueNaN) {
    // NaN to NaN should not throw
    EXPECT_FALSE(vm_throws(R"(
        var o = {};
        Object.defineProperty(o, 'x', { value: NaN, writable: false, configurable: false });
        Object.defineProperty(o, 'x', { value: NaN });
    )"));
}

// ---- DP-19: SameValue +0/-0（抛 TypeError）----

TEST(DefinePropInterp, DP19_SameValuePlusZeroMinusZero) {
    EXPECT_TRUE(interp_throws(R"(
        var o = {};
        Object.defineProperty(o, 'x', { value: 0, writable: false, configurable: false });
        Object.defineProperty(o, 'x', { value: -0 });
    )"));
}

TEST(DefinePropVM, DP19_SameValuePlusZeroMinusZero) {
    EXPECT_TRUE(vm_throws(R"(
        var o = {};
        Object.defineProperty(o, 'x', { value: 0, writable: false, configurable: false });
        Object.defineProperty(o, 'x', { value: -0 });
    )"));
}

// ---- DP-20: defineProperty 第一参数非对象 → TypeError ----

TEST(DefinePropInterp, DP20_FirstArgNonObject) {
    EXPECT_TRUE(interp_throws(R"(
        Object.defineProperty(42, 'x', { value: 1 });
    )"));
    EXPECT_TRUE(interp_throws(R"(
        Object.defineProperty(null, 'x', { value: 1 });
    )"));
}

TEST(DefinePropVM, DP20_FirstArgNonObject) {
    EXPECT_TRUE(vm_throws(R"(
        Object.defineProperty(42, 'x', { value: 1 });
    )"));
    EXPECT_TRUE(vm_throws(R"(
        Object.defineProperty(null, 'x', { value: 1 });
    )"));
}

// ---- DP-21: ToPropertyDescriptor get 非 callable → TypeError ----

TEST(DefinePropInterp, DP21_GetterNotCallable) {
    EXPECT_TRUE(interp_throws(R"(
        var o = {};
        Object.defineProperty(o, 'x', { get: 42 });
    )"));
}

TEST(DefinePropVM, DP21_GetterNotCallable) {
    EXPECT_TRUE(vm_throws(R"(
        var o = {};
        Object.defineProperty(o, 'x', { get: 42 });
    )"));
}

// ---- DP-22: ToPropertyDescriptor data+accessor 混用 → TypeError ----

TEST(DefinePropInterp, DP22_DataAccessorMixed) {
    EXPECT_TRUE(interp_throws(R"(
        var o = {};
        Object.defineProperty(o, 'x', { value: 1, get: function() { return 2; } });
    )"));
    EXPECT_TRUE(interp_throws(R"(
        var o = {};
        Object.defineProperty(o, 'x', { writable: true, set: function(v) {} });
    )"));
}

TEST(DefinePropVM, DP22_DataAccessorMixed) {
    EXPECT_TRUE(vm_throws(R"(
        var o = {};
        Object.defineProperty(o, 'x', { value: 1, get: function() { return 2; } });
    )"));
    EXPECT_TRUE(vm_throws(R"(
        var o = {};
        Object.defineProperty(o, 'x', { writable: true, set: function(v) {} });
    )"));
}

// ---- DP-23: accessor 原型链继承 — 子对象读写时 this 为子对象 ----
// 在原型上定义 accessor，子对象读写时 getter/setter 中 this 应为子对象而非原型。

TEST(DefinePropInterp, DP23_AccessorPrototypeInheritance) {
    // getter: this 指向子对象，读取子对象的 _val 属性
    EXPECT_EQ(interp_str(R"(
        var proto = {};
        Object.defineProperty(proto, 'x', {
            get: function() { return this._val; },
            set: function(v) { this._val = v * 3; },
            configurable: true
        });
        var child = Object.create(proto);
        child._val = 0;
        child.x = 4;
        child.x
    )"), "12");
    // 修改 child.x 不影响另一个子对象
    EXPECT_EQ(interp_str(R"(
        var proto = {};
        Object.defineProperty(proto, 'x', {
            get: function() { return this._val; },
            set: function(v) { this._val = v; },
            configurable: true
        });
        var a = Object.create(proto);
        var b = Object.create(proto);
        a._val = 0;
        b._val = 0;
        a.x = 10;
        b.x = 20;
        a.x
    )"), "10");
    // proto 本身不受影响
    EXPECT_EQ(interp_str(R"(
        var proto = {};
        Object.defineProperty(proto, 'x', {
            get: function() { return this._val; },
            set: function(v) { this._val = v; },
            configurable: true
        });
        var child = Object.create(proto);
        child.x = 99;
        proto._val
    )"), "undefined");
}

TEST(DefinePropVM, DP23_AccessorPrototypeInheritance) {
    EXPECT_EQ(vm_str(R"(
        var proto = {};
        Object.defineProperty(proto, 'x', {
            get: function() { return this._val; },
            set: function(v) { this._val = v * 3; },
            configurable: true
        });
        var child = Object.create(proto);
        child._val = 0;
        child.x = 4;
        child.x
    )"), "12");
    EXPECT_EQ(vm_str(R"(
        var proto = {};
        Object.defineProperty(proto, 'x', {
            get: function() { return this._val; },
            set: function(v) { this._val = v; },
            configurable: true
        });
        var a = Object.create(proto);
        var b = Object.create(proto);
        a._val = 0;
        b._val = 0;
        a.x = 10;
        b.x = 20;
        a.x
    )"), "10");
    EXPECT_EQ(vm_str(R"(
        var proto = {};
        Object.defineProperty(proto, 'x', {
            get: function() { return this._val; },
            set: function(v) { this._val = v; },
            configurable: true
        });
        var child = Object.create(proto);
        child.x = 99;
        proto._val
    )"), "undefined");
}

// ---- DP-24: getter 返回复杂表达式 — 每次读取重新调用 ----

TEST(DefinePropInterp, DP24_GetterReinvoked) {
    // getter 每次调用递增计数器，读取两次应得到不同值
    EXPECT_EQ(interp_str(R"(
        var count = 0;
        var o = {};
        Object.defineProperty(o, 'x', {
            get: function() { count = count + 1; return count; },
            configurable: true
        });
        var a = o.x;
        var b = o.x;
        a + ',' + b
    )"), "1,2");
    // getter 返回依赖外部变量的表达式
    EXPECT_EQ(interp_str(R"(
        var base = 100;
        var o = {};
        Object.defineProperty(o, 'x', {
            get: function() { return base * 2; },
            configurable: true
        });
        var first = o.x;
        base = 5;
        var second = o.x;
        first + ',' + second
    )"), "200,10");
}

TEST(DefinePropVM, DP24_GetterReinvoked) {
    EXPECT_EQ(vm_str(R"(
        var count = 0;
        var o = {};
        Object.defineProperty(o, 'x', {
            get: function() { count = count + 1; return count; },
            configurable: true
        });
        var a = o.x;
        var b = o.x;
        a + ',' + b
    )"), "1,2");
    EXPECT_EQ(vm_str(R"(
        var base = 100;
        var o = {};
        Object.defineProperty(o, 'x', {
            get: function() { return base * 2; },
            configurable: true
        });
        var first = o.x;
        base = 5;
        var second = o.x;
        first + ',' + second
    )"), "200,10");
}

// ---- DP-25: setter 副作用 — setter 修改对象其他属性 ----

TEST(DefinePropInterp, DP25_SetterSideEffect) {
    EXPECT_EQ(interp_str(R"(
        var o = { log: '' };
        Object.defineProperty(o, 'x', {
            set: function(v) { this.log = this.log + v; },
            configurable: true
        });
        o.x = 'a';
        o.x = 'b';
        o.log
    )"), "ab");
    // setter 修改同对象另一属性，getter 读取该属性
    EXPECT_EQ(interp_str(R"(
        var o = { _raw: 0, _doubled: 0 };
        Object.defineProperty(o, 'value', {
            get: function() { return this._doubled; },
            set: function(v) { this._raw = v; this._doubled = v * 2; },
            configurable: true
        });
        o.value = 7;
        o.value
    )"), "14");
}

TEST(DefinePropVM, DP25_SetterSideEffect) {
    EXPECT_EQ(vm_str(R"(
        var o = { log: '' };
        Object.defineProperty(o, 'x', {
            set: function(v) { this.log = this.log + v; },
            configurable: true
        });
        o.x = 'a';
        o.x = 'b';
        o.log
    )"), "ab");
    EXPECT_EQ(vm_str(R"(
        var o = { _raw: 0, _doubled: 0 };
        Object.defineProperty(o, 'value', {
            get: function() { return this._doubled; },
            set: function(v) { this._raw = v; this._doubled = v * 2; },
            configurable: true
        });
        o.value = 7;
        o.value
    )"), "14");
}

// ---- DP-26: getter 中 this 指向 — this 为 receiver（子对象）----

TEST(DefinePropInterp, DP26_GetterThisIsReceiver) {
    // getter 中 this 应为实际访问属性的对象（receiver），而非持有 accessor 的对象
    EXPECT_EQ(interp_str(R"(
        var proto = {};
        Object.defineProperty(proto, 'who', {
            get: function() { return this.name; },
            configurable: true
        });
        var child = Object.create(proto);
        child.name = 'child';
        child.who
    )"), "child");
    // 直接在 proto 上读：this 就是 proto
    EXPECT_EQ(interp_str(R"(
        var proto = { name: 'proto' };
        Object.defineProperty(proto, 'who', {
            get: function() { return this.name; },
            configurable: true
        });
        proto.who
    )"), "proto");
}

TEST(DefinePropVM, DP26_GetterThisIsReceiver) {
    EXPECT_EQ(vm_str(R"(
        var proto = {};
        Object.defineProperty(proto, 'who', {
            get: function() { return this.name; },
            configurable: true
        });
        var child = Object.create(proto);
        child.name = 'child';
        child.who
    )"), "child");
    EXPECT_EQ(vm_str(R"(
        var proto = { name: 'proto' };
        Object.defineProperty(proto, 'who', {
            get: function() { return this.name; },
            configurable: true
        });
        proto.who
    )"), "proto");
}

// ---- DP-27: redefine accessor — configurable:true 时可替换 getter ----

TEST(DefinePropInterp, DP27_RedefineAccessor) {
    // 将 getter 从返回 1 替换为返回 2
    EXPECT_EQ(interp_str(R"(
        var o = {};
        Object.defineProperty(o, 'x', {
            get: function() { return 1; },
            configurable: true
        });
        Object.defineProperty(o, 'x', {
            get: function() { return 2; },
            configurable: true
        });
        o.x
    )"), "2");
    // 替换 getter 后 configurable 仍为 true
    EXPECT_EQ(interp_str(R"(
        var o = {};
        Object.defineProperty(o, 'x', {
            get: function() { return 1; },
            configurable: true
        });
        Object.defineProperty(o, 'x', {
            get: function() { return 99; },
            configurable: true
        });
        var d = Object.getOwnPropertyDescriptor(o, 'x');
        d.configurable
    )"), "true");
}

TEST(DefinePropVM, DP27_RedefineAccessor) {
    EXPECT_EQ(vm_str(R"(
        var o = {};
        Object.defineProperty(o, 'x', {
            get: function() { return 1; },
            configurable: true
        });
        Object.defineProperty(o, 'x', {
            get: function() { return 2; },
            configurable: true
        });
        o.x
    )"), "2");
    EXPECT_EQ(vm_str(R"(
        var o = {};
        Object.defineProperty(o, 'x', {
            get: function() { return 1; },
            configurable: true
        });
        Object.defineProperty(o, 'x', {
            get: function() { return 99; },
            configurable: true
        });
        var d = Object.getOwnPropertyDescriptor(o, 'x');
        d.configurable
    )"), "true");
}

// ---- DP-28: configurable accessor 切换为 data descriptor ----

TEST(DefinePropInterp, DP28_AccessorToDataDescriptor) {
    // configurable:true 的 accessor 可以切换为 data descriptor
    EXPECT_EQ(interp_str(R"(
        var o = {};
        Object.defineProperty(o, 'x', {
            get: function() { return 42; },
            configurable: true
        });
        Object.defineProperty(o, 'x', {
            value: 100,
            writable: true,
            configurable: true
        });
        o.x
    )"), "100");
    // 切换后可正常赋值（writable:true）
    EXPECT_EQ(interp_str(R"(
        var o = {};
        Object.defineProperty(o, 'x', {
            get: function() { return 42; },
            configurable: true
        });
        Object.defineProperty(o, 'x', {
            value: 100,
            writable: true,
            configurable: true
        });
        o.x = 200;
        o.x
    )"), "200");
    // 切换后 getOwnPropertyDescriptor 不含 get/set
    EXPECT_EQ(interp_str(R"(
        var o = {};
        Object.defineProperty(o, 'x', {
            get: function() { return 42; },
            configurable: true
        });
        Object.defineProperty(o, 'x', {
            value: 100,
            writable: false,
            configurable: true
        });
        var d = Object.getOwnPropertyDescriptor(o, 'x');
        d.get
    )"), "undefined");
}

TEST(DefinePropVM, DP28_AccessorToDataDescriptor) {
    EXPECT_EQ(vm_str(R"(
        var o = {};
        Object.defineProperty(o, 'x', {
            get: function() { return 42; },
            configurable: true
        });
        Object.defineProperty(o, 'x', {
            value: 100,
            writable: true,
            configurable: true
        });
        o.x
    )"), "100");
    EXPECT_EQ(vm_str(R"(
        var o = {};
        Object.defineProperty(o, 'x', {
            get: function() { return 42; },
            configurable: true
        });
        Object.defineProperty(o, 'x', {
            value: 100,
            writable: true,
            configurable: true
        });
        o.x = 200;
        o.x
    )"), "200");
    EXPECT_EQ(vm_str(R"(
        var o = {};
        Object.defineProperty(o, 'x', {
            get: function() { return 42; },
            configurable: true
        });
        Object.defineProperty(o, 'x', {
            value: 100,
            writable: false,
            configurable: true
        });
        var d = Object.getOwnPropertyDescriptor(o, 'x');
        d.get
    )"), "undefined");
}

// ---- DP-29: non-configurable accessor 不能替换 get → TypeError ----

TEST(DefinePropInterp, DP29_NonConfigurableAccessorReplaceGet) {
    EXPECT_TRUE(interp_throws(R"(
        var o = {};
        Object.defineProperty(o, 'x', {
            get: function() { return 1; },
            configurable: false
        });
        Object.defineProperty(o, 'x', {
            get: function() { return 2; }
        });
    )"));
    // non-configurable accessor 也不能改 set
    EXPECT_TRUE(interp_throws(R"(
        var o = {};
        Object.defineProperty(o, 'x', {
            get: function() { return 1; },
            set: function(v) {},
            configurable: false
        });
        Object.defineProperty(o, 'x', {
            set: function(v) {}
        });
    )"));
}

TEST(DefinePropVM, DP29_NonConfigurableAccessorReplaceGet) {
    EXPECT_TRUE(vm_throws(R"(
        var o = {};
        Object.defineProperty(o, 'x', {
            get: function() { return 1; },
            configurable: false
        });
        Object.defineProperty(o, 'x', {
            get: function() { return 2; }
        });
    )"));
    EXPECT_TRUE(vm_throws(R"(
        var o = {};
        Object.defineProperty(o, 'x', {
            get: function() { return 1; },
            set: function(v) {},
            configurable: false
        });
        Object.defineProperty(o, 'x', {
            set: function(v) {}
        });
    )"));
}

// ---- DP-30: getOwnPropertyDescriptor accessor 格式 — 含 get/set，不含 value/writable ----

TEST(DefinePropInterp, DP30_GetOwnDescAccessorFormat) {
    // accessor descriptor 应包含 get/set/enumerable/configurable，不含 value/writable
    EXPECT_EQ(interp_str(R"(
        var getter = function() { return 7; };
        var o = {};
        Object.defineProperty(o, 'x', {
            get: getter,
            enumerable: true,
            configurable: true
        });
        var d = Object.getOwnPropertyDescriptor(o, 'x');
        d.enumerable
    )"), "true");
    EXPECT_EQ(interp_str(R"(
        var getter = function() { return 7; };
        var o = {};
        Object.defineProperty(o, 'x', {
            get: getter,
            enumerable: true,
            configurable: true
        });
        var d = Object.getOwnPropertyDescriptor(o, 'x');
        d.configurable
    )"), "true");
    // value 字段在 accessor descriptor 中为 undefined
    EXPECT_EQ(interp_str(R"(
        var o = {};
        Object.defineProperty(o, 'x', {
            get: function() { return 7; },
            enumerable: true,
            configurable: true
        });
        var d = Object.getOwnPropertyDescriptor(o, 'x');
        d.value
    )"), "undefined");
    // writable 字段在 accessor descriptor 中为 undefined
    EXPECT_EQ(interp_str(R"(
        var o = {};
        Object.defineProperty(o, 'x', {
            get: function() { return 7; },
            enumerable: true,
            configurable: true
        });
        var d = Object.getOwnPropertyDescriptor(o, 'x');
        d.writable
    )"), "undefined");
    // set-only accessor: get 字段为 undefined
    EXPECT_EQ(interp_str(R"(
        var o = {};
        Object.defineProperty(o, 'x', {
            set: function(v) {},
            configurable: true
        });
        var d = Object.getOwnPropertyDescriptor(o, 'x');
        d.get
    )"), "undefined");
}

TEST(DefinePropVM, DP30_GetOwnDescAccessorFormat) {
    EXPECT_EQ(vm_str(R"(
        var getter = function() { return 7; };
        var o = {};
        Object.defineProperty(o, 'x', {
            get: getter,
            enumerable: true,
            configurable: true
        });
        var d = Object.getOwnPropertyDescriptor(o, 'x');
        d.enumerable
    )"), "true");
    EXPECT_EQ(vm_str(R"(
        var getter = function() { return 7; };
        var o = {};
        Object.defineProperty(o, 'x', {
            get: getter,
            enumerable: true,
            configurable: true
        });
        var d = Object.getOwnPropertyDescriptor(o, 'x');
        d.configurable
    )"), "true");
    EXPECT_EQ(vm_str(R"(
        var o = {};
        Object.defineProperty(o, 'x', {
            get: function() { return 7; },
            enumerable: true,
            configurable: true
        });
        var d = Object.getOwnPropertyDescriptor(o, 'x');
        d.value
    )"), "undefined");
    EXPECT_EQ(vm_str(R"(
        var o = {};
        Object.defineProperty(o, 'x', {
            get: function() { return 7; },
            enumerable: true,
            configurable: true
        });
        var d = Object.getOwnPropertyDescriptor(o, 'x');
        d.writable
    )"), "undefined");
    EXPECT_EQ(vm_str(R"(
        var o = {};
        Object.defineProperty(o, 'x', {
            set: function(v) {},
            configurable: true
        });
        var d = Object.getOwnPropertyDescriptor(o, 'x');
        d.get
    )"), "undefined");
}

// ---- DP-31: Object.keys 区分 enumerable/non-enumerable — 多属性混合 ----

TEST(DefinePropInterp, DP31_ObjectKeysEnumerableFilter) {
    // 混合 enumerable:true 和 enumerable:false 属性，Object.keys 只返回 enumerable 的
    EXPECT_EQ(interp_str(R"(
        var o = {};
        Object.defineProperty(o, 'a', { value: 1, enumerable: true,  configurable: true });
        Object.defineProperty(o, 'b', { value: 2, enumerable: false, configurable: true });
        Object.defineProperty(o, 'c', { value: 3, enumerable: true,  configurable: true });
        Object.defineProperty(o, 'd', { value: 4, enumerable: false, configurable: true });
        Object.keys(o).length
    )"), "2");
    // 确认返回的 key 正确
    EXPECT_EQ(interp_str(R"(
        var o = {};
        Object.defineProperty(o, 'a', { value: 1, enumerable: true,  configurable: true });
        Object.defineProperty(o, 'b', { value: 2, enumerable: false, configurable: true });
        Object.defineProperty(o, 'c', { value: 3, enumerable: true,  configurable: true });
        var keys = Object.keys(o);
        keys[0] + ',' + keys[1]
    )"), "a,c");
    // accessor 属性 enumerable:true 也出现在 Object.keys
    EXPECT_EQ(interp_str(R"(
        var o = {};
        Object.defineProperty(o, 'x', {
            get: function() { return 1; },
            enumerable: true,
            configurable: true
        });
        Object.defineProperty(o, 'y', {
            get: function() { return 2; },
            enumerable: false,
            configurable: true
        });
        Object.keys(o).length
    )"), "1");
}

TEST(DefinePropVM, DP31_ObjectKeysEnumerableFilter) {
    EXPECT_EQ(vm_str(R"(
        var o = {};
        Object.defineProperty(o, 'a', { value: 1, enumerable: true,  configurable: true });
        Object.defineProperty(o, 'b', { value: 2, enumerable: false, configurable: true });
        Object.defineProperty(o, 'c', { value: 3, enumerable: true,  configurable: true });
        Object.defineProperty(o, 'd', { value: 4, enumerable: false, configurable: true });
        Object.keys(o).length
    )"), "2");
    EXPECT_EQ(vm_str(R"(
        var o = {};
        Object.defineProperty(o, 'a', { value: 1, enumerable: true,  configurable: true });
        Object.defineProperty(o, 'b', { value: 2, enumerable: false, configurable: true });
        Object.defineProperty(o, 'c', { value: 3, enumerable: true,  configurable: true });
        var keys = Object.keys(o);
        keys[0] + ',' + keys[1]
    )"), "a,c");
    EXPECT_EQ(vm_str(R"(
        var o = {};
        Object.defineProperty(o, 'x', {
            get: function() { return 1; },
            enumerable: true,
            configurable: true
        });
        Object.defineProperty(o, 'y', {
            get: function() { return 2; },
            enumerable: false,
            configurable: true
        });
        Object.keys(o).length
    )"), "1");
}

// ---- DP-32: preventExtensions 不影响已有属性的修改 ----

TEST(DefinePropInterp, DP32_PreventExtensionsExistingPropStillModifiable) {
    // preventExtensions 只阻止新增属性，不影响已有 writable 属性的赋值
    EXPECT_EQ(interp_str(R"(
        var o = { x: 1, y: 2 };
        Object.preventExtensions(o);
        o.x = 100;
        o.x
    )"), "100");
    // 已有 configurable:true 属性仍可通过 defineProperty 修改
    EXPECT_EQ(interp_str(R"(
        var o = {};
        Object.defineProperty(o, 'x', { value: 1, writable: true, configurable: true });
        Object.preventExtensions(o);
        Object.defineProperty(o, 'x', { value: 99 });
        o.x
    )"), "99");
    // preventExtensions 后仍然可以 delete 已有属性（configurable:true）
    EXPECT_EQ(interp_str(R"(
        var o = {};
        Object.defineProperty(o, 'x', { value: 1, configurable: true });
        Object.preventExtensions(o);
        delete o.x
    )"), "true");
}

TEST(DefinePropVM, DP32_PreventExtensionsExistingPropStillModifiable) {
    EXPECT_EQ(vm_str(R"(
        var o = { x: 1, y: 2 };
        Object.preventExtensions(o);
        o.x = 100;
        o.x
    )"), "100");
    EXPECT_EQ(vm_str(R"(
        var o = {};
        Object.defineProperty(o, 'x', { value: 1, writable: true, configurable: true });
        Object.preventExtensions(o);
        Object.defineProperty(o, 'x', { value: 99 });
        o.x
    )"), "99");
    EXPECT_EQ(vm_str(R"(
        var o = {};
        Object.defineProperty(o, 'x', { value: 1, configurable: true });
        Object.preventExtensions(o);
        delete o.x
    )"), "true");
}

// ---- DP-33: Object.defineProperty 返回值 === 目标对象 ----

TEST(DefinePropInterp, DP33_DefinePropertyReturnsObject) {
    // Object.defineProperty 应返回第一个参数（目标对象 O）
    EXPECT_EQ(interp_str(R"(
        var o = {};
        var ret = Object.defineProperty(o, 'x', { value: 1 });
        ret === o
    )"), "true");
    // 链式调用：返回值可直接再次传入 defineProperty
    EXPECT_EQ(interp_str(R"(
        var o = {};
        Object.defineProperty(
            Object.defineProperty(o, 'x', { value: 1, configurable: true }),
            'y',
            { value: 2, configurable: true }
        );
        o.x + o.y
    )"), "3");
}

TEST(DefinePropVM, DP33_DefinePropertyReturnsObject) {
    EXPECT_EQ(vm_str(R"(
        var o = {};
        var ret = Object.defineProperty(o, 'x', { value: 1 });
        ret === o
    )"), "true");
    EXPECT_EQ(vm_str(R"(
        var o = {};
        Object.defineProperty(
            Object.defineProperty(o, 'x', { value: 1, configurable: true }),
            'y',
            { value: 2, configurable: true }
        );
        o.x + o.y
    )"), "3");
}

// ---- DP-34: non-configurable 属性用 SameValue 重定义幂等 — 不抛异常 ----
// 规范：非 configurable + 非 writable 属性，重定义时若 value 满足 SameValue 则不报错。

TEST(DefinePropInterp, DP34_SameValueRedefineIdempotent) {
    // 同一整数值 SameValue 重定义：不抛异常
    EXPECT_FALSE(interp_throws(R"(
        var o = {};
        Object.defineProperty(o, 'x', { value: 1, writable: false, configurable: false });
        Object.defineProperty(o, 'x', { value: 1 });
    )"));
    // 重定义后值不变
    EXPECT_EQ(interp_str(R"(
        var o = {};
        Object.defineProperty(o, 'x', { value: 42, writable: false, configurable: false });
        Object.defineProperty(o, 'x', { value: 42 });
        o.x
    )"), "42");
    // 字符串 SameValue 重定义：不抛异常
    EXPECT_FALSE(interp_throws(R"(
        var o = {};
        Object.defineProperty(o, 'x', { value: 'hello', writable: false, configurable: false });
        Object.defineProperty(o, 'x', { value: 'hello' });
    )"));
    // NaN SameValue：不抛（DP-18 已测，这里补 writable:false configurable:false 完整组合）
    EXPECT_FALSE(interp_throws(R"(
        var o = {};
        Object.defineProperty(o, 'x', { value: NaN, writable: false, configurable: false });
        Object.defineProperty(o, 'x', { value: NaN });
    )"));
    // 不同值仍然抛 TypeError（回归）
    EXPECT_TRUE(interp_throws(R"(
        var o = {};
        Object.defineProperty(o, 'x', { value: 1, writable: false, configurable: false });
        Object.defineProperty(o, 'x', { value: 2 });
    )"));
}

TEST(DefinePropVM, DP34_SameValueRedefineIdempotent) {
    EXPECT_FALSE(vm_throws(R"(
        var o = {};
        Object.defineProperty(o, 'x', { value: 1, writable: false, configurable: false });
        Object.defineProperty(o, 'x', { value: 1 });
    )"));
    EXPECT_EQ(vm_str(R"(
        var o = {};
        Object.defineProperty(o, 'x', { value: 42, writable: false, configurable: false });
        Object.defineProperty(o, 'x', { value: 42 });
        o.x
    )"), "42");
    EXPECT_FALSE(vm_throws(R"(
        var o = {};
        Object.defineProperty(o, 'x', { value: 'hello', writable: false, configurable: false });
        Object.defineProperty(o, 'x', { value: 'hello' });
    )"));
    EXPECT_FALSE(vm_throws(R"(
        var o = {};
        Object.defineProperty(o, 'x', { value: NaN, writable: false, configurable: false });
        Object.defineProperty(o, 'x', { value: NaN });
    )"));
    EXPECT_TRUE(vm_throws(R"(
        var o = {};
        Object.defineProperty(o, 'x', { value: 1, writable: false, configurable: false });
        Object.defineProperty(o, 'x', { value: 2 });
    )"));
}

}  // namespace
