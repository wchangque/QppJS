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

static bool interp_throws(std::string_view source) {
    auto parse_result = parse_program(source);
    if (!parse_result.ok()) return false;
    Interpreter interp;
    auto result = interp.exec(parse_result.value());
    return !result.is_ok();
}

static bool vm_throws(std::string_view source) {
    auto parse_result = parse_program(source);
    if (!parse_result.ok()) return false;
    Compiler compiler;
    auto bytecode = compiler.compile(parse_result.value());
    VM vm;
    auto result = vm.exec(bytecode);
    return !result.is_ok();
}

// ---- Map tests (MS-01 ~ MS-20) ----

TEST(MapSetInterp, MS01_MapTypeofObject) {
    auto v = interp_ok("var m = new Map(); typeof m");
    EXPECT_EQ(v.sv(), "object");
}

TEST(MapSetVM, MS01_MapTypeofObject) {
    auto v = vm_ok("var m = new Map(); typeof m");
    EXPECT_EQ(v.sv(), "object");
}

TEST(MapSetInterp, MS02_MapSetChainable) {
    auto v = interp_ok("var m = new Map(); m.set('a', 1) === m");
    EXPECT_TRUE(v.is_bool() && v.as_bool() == true);
}

TEST(MapSetVM, MS02_MapSetChainable) {
    auto v = vm_ok("var m = new Map(); m.set('a', 1) === m");
    EXPECT_TRUE(v.is_bool() && v.as_bool() == true);
}

TEST(MapSetInterp, MS03_MapGet) {
    auto v = interp_ok("var m = new Map(); m.set('k', 42); m.get('k')");
    EXPECT_EQ(v.as_number(), 42.0);
}

TEST(MapSetVM, MS03_MapGet) {
    auto v = vm_ok("var m = new Map(); m.set('k', 42); m.get('k')");
    EXPECT_EQ(v.as_number(), 42.0);
}

TEST(MapSetInterp, MS04_MapHas) {
    auto v = interp_ok("var m = new Map(); m.set('x', 1); m.has('x')");
    EXPECT_TRUE(v.is_bool() && v.as_bool() == true);
    auto v2 = interp_ok("var m = new Map(); m.has('y')");
    EXPECT_TRUE(v2.is_bool() && v2.as_bool() == false);
}

TEST(MapSetVM, MS04_MapHas) {
    auto v = vm_ok("var m = new Map(); m.set('x', 1); m.has('x')");
    EXPECT_TRUE(v.is_bool() && v.as_bool() == true);
    auto v2 = vm_ok("var m = new Map(); m.has('y')");
    EXPECT_TRUE(v2.is_bool() && v2.as_bool() == false);
}

TEST(MapSetInterp, MS05_MapDelete) {
    auto v = interp_ok(
        "var m = new Map(); m.set('a', 1); var r = m.delete('a'); r && !m.has('a')");
    EXPECT_TRUE(v.is_bool() && v.as_bool() == true);
}

TEST(MapSetVM, MS05_MapDelete) {
    auto v = vm_ok(
        "var m = new Map(); m.set('a', 1); var r = m.delete('a'); r && !m.has('a')");
    EXPECT_TRUE(v.is_bool() && v.as_bool() == true);
}

TEST(MapSetInterp, MS06_MapSize) {
    auto v = interp_ok(
        "var m = new Map(); m.set('a', 1); m.set('b', 2); m.size");
    EXPECT_EQ(v.as_number(), 2.0);
}

TEST(MapSetVM, MS06_MapSize) {
    auto v = vm_ok(
        "var m = new Map(); m.set('a', 1); m.set('b', 2); m.size");
    EXPECT_EQ(v.as_number(), 2.0);
}

TEST(MapSetInterp, MS07_MapObjectKey) {
    auto v = interp_ok(
        "var m = new Map(); var k = {}; m.set(k, 99); m.get(k)");
    EXPECT_EQ(v.as_number(), 99.0);
}

TEST(MapSetVM, MS07_MapObjectKey) {
    auto v = vm_ok(
        "var m = new Map(); var k = {}; m.set(k, 99); m.get(k)");
    EXPECT_EQ(v.as_number(), 99.0);
}

TEST(MapSetInterp, MS08_MapNaNKey) {
    auto v = interp_ok(
        "var m = new Map(); m.set(NaN, 'nan'); m.get(NaN)");
    EXPECT_EQ(v.sv(), "nan");
}

TEST(MapSetVM, MS08_MapNaNKey) {
    auto v = vm_ok(
        "var m = new Map(); m.set(NaN, 'nan'); m.get(NaN)");
    EXPECT_EQ(v.sv(), "nan");
}

TEST(MapSetInterp, MS09_MapPlusZeroMinusZeroSameKey) {
    // +0 and -0 are the same key in SameValueZero
    auto v = interp_ok(
        "var m = new Map(); m.set(+0, 'plus'); m.set(-0, 'minus'); m.size");
    EXPECT_EQ(v.as_number(), 1.0);
    auto v2 = interp_ok(
        "var m = new Map(); m.set(+0, 'plus'); m.set(-0, 'minus'); m.get(+0)");
    EXPECT_EQ(v2.sv(), "minus");
}

TEST(MapSetVM, MS09_MapPlusZeroMinusZeroSameKey) {
    auto v = vm_ok(
        "var m = new Map(); m.set(+0, 'plus'); m.set(-0, 'minus'); m.size");
    EXPECT_EQ(v.as_number(), 1.0);
    auto v2 = vm_ok(
        "var m = new Map(); m.set(+0, 'plus'); m.set(-0, 'minus'); m.get(+0)");
    EXPECT_EQ(v2.sv(), "minus");
}

TEST(MapSetInterp, MS10_MapClear) {
    auto v = interp_ok(
        "var m = new Map(); m.set('a', 1); m.set('b', 2); m.clear(); m.size");
    EXPECT_EQ(v.as_number(), 0.0);
}

TEST(MapSetVM, MS10_MapClear) {
    auto v = vm_ok(
        "var m = new Map(); m.set('a', 1); m.set('b', 2); m.clear(); m.size");
    EXPECT_EQ(v.as_number(), 0.0);
}

TEST(MapSetInterp, MS11_MapIterableConstructor) {
    auto v = interp_ok(
        "var m = new Map([['a', 1], ['b', 2]]); m.size");
    EXPECT_EQ(v.as_number(), 2.0);
    auto v2 = interp_ok(
        "var m = new Map([['a', 1], ['b', 2]]); m.get('b')");
    EXPECT_EQ(v2.as_number(), 2.0);
}

TEST(MapSetVM, MS11_MapIterableConstructor) {
    auto v = vm_ok(
        "var m = new Map([['a', 1], ['b', 2]]); m.size");
    EXPECT_EQ(v.as_number(), 2.0);
    auto v2 = vm_ok(
        "var m = new Map([['a', 1], ['b', 2]]); m.get('b')");
    EXPECT_EQ(v2.as_number(), 2.0);
}

TEST(MapSetInterp, MS12_MapForEach) {
    auto v = interp_ok(R"(
        var m = new Map(); m.set('a', 1); m.set('b', 2);
        var sum = 0;
        m.forEach(function(v, k) { sum += v; });
        sum
    )");
    EXPECT_EQ(v.as_number(), 3.0);
}

TEST(MapSetVM, MS12_MapForEach) {
    auto v = vm_ok(R"(
        var m = new Map(); m.set('a', 1); m.set('b', 2);
        var sum = 0;
        m.forEach(function(v, k) { sum += v; });
        sum
    )");
    EXPECT_EQ(v.as_number(), 3.0);
}

TEST(MapSetInterp, MS13_MapKeys) {
    auto v = interp_ok(R"(
        var m = new Map(); m.set('a', 1); m.set('b', 2);
        var keys = [];
        var it = m.keys();
        var r;
        while (!(r = it.next()).done) keys.push(r.value);
        keys.join(',')
    )");
    EXPECT_EQ(v.sv(), "a,b");
}

TEST(MapSetVM, MS13_MapKeys) {
    auto v = vm_ok(R"(
        var m = new Map(); m.set('a', 1); m.set('b', 2);
        var keys = [];
        var it = m.keys();
        var r;
        while (!(r = it.next()).done) keys.push(r.value);
        keys.join(',')
    )");
    EXPECT_EQ(v.sv(), "a,b");
}

TEST(MapSetInterp, MS14_MapValues) {
    auto v = interp_ok(R"(
        var m = new Map(); m.set('a', 10); m.set('b', 20);
        var vals = [];
        var it = m.values();
        var r;
        while (!(r = it.next()).done) vals.push(r.value);
        vals.join(',')
    )");
    EXPECT_EQ(v.sv(), "10,20");
}

TEST(MapSetVM, MS14_MapValues) {
    auto v = vm_ok(R"(
        var m = new Map(); m.set('a', 10); m.set('b', 20);
        var vals = [];
        var it = m.values();
        var r;
        while (!(r = it.next()).done) vals.push(r.value);
        vals.join(',')
    )");
    EXPECT_EQ(v.sv(), "10,20");
}

TEST(MapSetInterp, MS15_MapEntries) {
    auto v = interp_ok(R"(
        var m = new Map(); m.set('a', 1); m.set('b', 2);
        var it = m.entries();
        var first = it.next();
        first.value[0] + ':' + first.value[1]
    )");
    EXPECT_EQ(v.sv(), "a:1");
}

TEST(MapSetVM, MS15_MapEntries) {
    auto v = vm_ok(R"(
        var m = new Map(); m.set('a', 1); m.set('b', 2);
        var it = m.entries();
        var first = it.next();
        first.value[0] + ':' + first.value[1]
    )");
    EXPECT_EQ(v.sv(), "a:1");
}

TEST(MapSetInterp, MS16_MapForOf) {
    auto v = interp_ok(R"(
        var m = new Map(); m.set('x', 10); m.set('y', 20);
        var sum = 0;
        for (var entry of m) { sum += entry[1]; }
        sum
    )");
    EXPECT_EQ(v.as_number(), 30.0);
}

TEST(MapSetVM, MS16_MapForOf) {
    auto v = vm_ok(R"(
        var m = new Map(); m.set('x', 10); m.set('y', 20);
        var sum = 0;
        for (var entry of m) { sum += entry[1]; }
        sum
    )");
    EXPECT_EQ(v.as_number(), 30.0);
}

TEST(MapSetInterp, MS17_MapOverwriteKey) {
    auto v = interp_ok(
        "var m = new Map(); m.set('k', 1); m.set('k', 99); m.get('k')");
    EXPECT_EQ(v.as_number(), 99.0);
}

TEST(MapSetVM, MS17_MapOverwriteKey) {
    auto v = vm_ok(
        "var m = new Map(); m.set('k', 1); m.set('k', 99); m.get('k')");
    EXPECT_EQ(v.as_number(), 99.0);
}

TEST(MapSetInterp, MS18_MapGetMissing) {
    auto v = interp_ok("var m = new Map(); m.get('missing')");
    EXPECT_TRUE(v.is_undefined());
}

TEST(MapSetVM, MS18_MapGetMissing) {
    auto v = vm_ok("var m = new Map(); m.get('missing')");
    EXPECT_TRUE(v.is_undefined());
}

TEST(MapSetInterp, MS19_MapDifferentKeyTypes) {
    auto v = interp_ok(R"(
        var m = new Map();
        m.set('s', 1);
        m.set(42, 2);
        m.set(true, 3);
        m.size
    )");
    EXPECT_EQ(v.as_number(), 3.0);
}

TEST(MapSetVM, MS19_MapDifferentKeyTypes) {
    auto v = vm_ok(R"(
        var m = new Map();
        m.set('s', 1);
        m.set(42, 2);
        m.set(true, 3);
        m.size
    )");
    EXPECT_EQ(v.as_number(), 3.0);
}

TEST(MapSetInterp, MS20_MapNoIterable) {
    auto v = interp_ok("var m = new Map(); m.size");
    EXPECT_EQ(v.as_number(), 0.0);
}

TEST(MapSetVM, MS20_MapNoIterable) {
    auto v = vm_ok("var m = new Map(); m.size");
    EXPECT_EQ(v.as_number(), 0.0);
}

// ---- Set tests (MS-21 ~ MS-35) ----

TEST(MapSetInterp, MS21_SetTypeofObject) {
    auto v = interp_ok("var s = new Set(); typeof s");
    EXPECT_EQ(v.sv(), "object");
}

TEST(MapSetVM, MS21_SetTypeofObject) {
    auto v = vm_ok("var s = new Set(); typeof s");
    EXPECT_EQ(v.sv(), "object");
}

TEST(MapSetInterp, MS22_SetAddChainable) {
    auto v = interp_ok("var s = new Set(); s.add(1) === s");
    EXPECT_TRUE(v.is_bool() && v.as_bool() == true);
}

TEST(MapSetVM, MS22_SetAddChainable) {
    auto v = vm_ok("var s = new Set(); s.add(1) === s");
    EXPECT_TRUE(v.is_bool() && v.as_bool() == true);
}

TEST(MapSetInterp, MS23_SetHas) {
    auto v = interp_ok("var s = new Set(); s.add(1); s.has(1)");
    EXPECT_TRUE(v.is_bool() && v.as_bool() == true);
    auto v2 = interp_ok("var s = new Set(); s.has(99)");
    EXPECT_TRUE(v2.is_bool() && v2.as_bool() == false);
}

TEST(MapSetVM, MS23_SetHas) {
    auto v = vm_ok("var s = new Set(); s.add(1); s.has(1)");
    EXPECT_TRUE(v.is_bool() && v.as_bool() == true);
    auto v2 = vm_ok("var s = new Set(); s.has(99)");
    EXPECT_TRUE(v2.is_bool() && v2.as_bool() == false);
}

TEST(MapSetInterp, MS24_SetDelete) {
    auto v = interp_ok(
        "var s = new Set(); s.add(5); var r = s.delete(5); r && !s.has(5)");
    EXPECT_TRUE(v.is_bool() && v.as_bool() == true);
}

TEST(MapSetVM, MS24_SetDelete) {
    auto v = vm_ok(
        "var s = new Set(); s.add(5); var r = s.delete(5); r && !s.has(5)");
    EXPECT_TRUE(v.is_bool() && v.as_bool() == true);
}

TEST(MapSetInterp, MS25_SetSize) {
    auto v = interp_ok("var s = new Set(); s.add(1); s.add(2); s.add(3); s.size");
    EXPECT_EQ(v.as_number(), 3.0);
}

TEST(MapSetVM, MS25_SetSize) {
    auto v = vm_ok("var s = new Set(); s.add(1); s.add(2); s.add(3); s.size");
    EXPECT_EQ(v.as_number(), 3.0);
}

TEST(MapSetInterp, MS26_SetNaNDedup) {
    auto v = interp_ok("var s = new Set(); s.add(NaN); s.add(NaN); s.size");
    EXPECT_EQ(v.as_number(), 1.0);
}

TEST(MapSetVM, MS26_SetNaNDedup) {
    auto v = vm_ok("var s = new Set(); s.add(NaN); s.add(NaN); s.size");
    EXPECT_EQ(v.as_number(), 1.0);
}

TEST(MapSetInterp, MS27_SetPlusZeroMinusZeroDedup) {
    auto v = interp_ok("var s = new Set(); s.add(+0); s.add(-0); s.size");
    EXPECT_EQ(v.as_number(), 1.0);
}

TEST(MapSetVM, MS27_SetPlusZeroMinusZeroDedup) {
    auto v = vm_ok("var s = new Set(); s.add(+0); s.add(-0); s.size");
    EXPECT_EQ(v.as_number(), 1.0);
}

TEST(MapSetInterp, MS28_SetObjectNoDedup) {
    auto v = interp_ok("var s = new Set(); s.add({}); s.add({}); s.size");
    EXPECT_EQ(v.as_number(), 2.0);
}

TEST(MapSetVM, MS28_SetObjectNoDedup) {
    auto v = vm_ok("var s = new Set(); s.add({}); s.add({}); s.size");
    EXPECT_EQ(v.as_number(), 2.0);
}

TEST(MapSetInterp, MS29_SetClear) {
    auto v = interp_ok(
        "var s = new Set(); s.add(1); s.add(2); s.clear(); s.size");
    EXPECT_EQ(v.as_number(), 0.0);
}

TEST(MapSetVM, MS29_SetClear) {
    auto v = vm_ok(
        "var s = new Set(); s.add(1); s.add(2); s.clear(); s.size");
    EXPECT_EQ(v.as_number(), 0.0);
}

TEST(MapSetInterp, MS30_SetFromArray) {
    auto v = interp_ok("var s = new Set([1, 2, 3]); s.size");
    EXPECT_EQ(v.as_number(), 3.0);
}

TEST(MapSetVM, MS30_SetFromArray) {
    auto v = vm_ok("var s = new Set([1, 2, 3]); s.size");
    EXPECT_EQ(v.as_number(), 3.0);
}

TEST(MapSetInterp, MS31_SetForEach) {
    auto v = interp_ok(R"(
        var s = new Set([10, 20, 30]);
        var sum = 0;
        s.forEach(function(v) { sum += v; });
        sum
    )");
    EXPECT_EQ(v.as_number(), 60.0);
}

TEST(MapSetVM, MS31_SetForEach) {
    auto v = vm_ok(R"(
        var s = new Set([10, 20, 30]);
        var sum = 0;
        s.forEach(function(v) { sum += v; });
        sum
    )");
    EXPECT_EQ(v.as_number(), 60.0);
}

TEST(MapSetInterp, MS32_SetValues) {
    auto v = interp_ok(R"(
        var s = new Set(['a', 'b', 'c']);
        var arr = [];
        var it = s.values();
        var r;
        while (!(r = it.next()).done) arr.push(r.value);
        arr.join(',')
    )");
    EXPECT_EQ(v.sv(), "a,b,c");
}

TEST(MapSetVM, MS32_SetValues) {
    auto v = vm_ok(R"(
        var s = new Set(['a', 'b', 'c']);
        var arr = [];
        var it = s.values();
        var r;
        while (!(r = it.next()).done) arr.push(r.value);
        arr.join(',')
    )");
    EXPECT_EQ(v.sv(), "a,b,c");
}

TEST(MapSetInterp, MS33_SetForOf) {
    auto v = interp_ok(R"(
        var s = new Set([1, 2, 3]);
        var sum = 0;
        for (var v of s) sum += v;
        sum
    )");
    EXPECT_EQ(v.as_number(), 6.0);
}

TEST(MapSetVM, MS33_SetForOf) {
    auto v = vm_ok(R"(
        var s = new Set([1, 2, 3]);
        var sum = 0;
        for (var v of s) sum += v;
        sum
    )");
    EXPECT_EQ(v.as_number(), 6.0);
}

TEST(MapSetInterp, MS34_SetAddDuplicate) {
    auto v = interp_ok("var s = new Set(); s.add(1); s.add(1); s.add(1); s.size");
    EXPECT_EQ(v.as_number(), 1.0);
}

TEST(MapSetVM, MS34_SetAddDuplicate) {
    auto v = vm_ok("var s = new Set(); s.add(1); s.add(1); s.add(1); s.size");
    EXPECT_EQ(v.as_number(), 1.0);
}

TEST(MapSetInterp, MS35_SetEntries) {
    auto v = interp_ok(R"(
        var s = new Set([42]);
        var it = s.entries();
        var r = it.next();
        r.value[0] === r.value[1] && r.value[0] === 42
    )");
    EXPECT_TRUE(v.is_bool() && v.as_bool() == true);
}

TEST(MapSetVM, MS35_SetEntries) {
    auto v = vm_ok(R"(
        var s = new Set([42]);
        var it = s.entries();
        var r = it.next();
        r.value[0] === r.value[1] && r.value[0] === 42
    )");
    EXPECT_TRUE(v.is_bool() && v.as_bool() == true);
}

// ---- WeakMap tests (MS-36 ~ MS-40) ----

TEST(MapSetInterp, MS36_WeakMapGetSetHas) {
    auto v = interp_ok(R"(
        var wm = new WeakMap();
        var k = {};
        wm.set(k, 'val');
        wm.get(k) + ',' + wm.has(k)
    )");
    EXPECT_EQ(v.sv(), "val,true");
}

TEST(MapSetVM, MS36_WeakMapGetSetHas) {
    auto v = vm_ok(R"(
        var wm = new WeakMap();
        var k = {};
        wm.set(k, 'val');
        wm.get(k) + ',' + wm.has(k)
    )");
    EXPECT_EQ(v.sv(), "val,true");
}

TEST(MapSetInterp, MS37_WeakMapNonObjectKeyThrows) {
    EXPECT_TRUE(interp_throws(R"(
        var wm = new WeakMap();
        wm.set('notobj', 1);
    )"));
}

TEST(MapSetVM, MS37_WeakMapNonObjectKeyThrows) {
    EXPECT_TRUE(vm_throws(R"(
        var wm = new WeakMap();
        wm.set('notobj', 1);
    )"));
}

TEST(MapSetInterp, MS38_WeakMapDelete) {
    auto v = interp_ok(R"(
        var wm = new WeakMap();
        var k = {};
        wm.set(k, 1);
        wm.delete(k);
        wm.has(k)
    )");
    EXPECT_TRUE(v.is_bool() && v.as_bool() == false);
}

TEST(MapSetVM, MS38_WeakMapDelete) {
    auto v = vm_ok(R"(
        var wm = new WeakMap();
        var k = {};
        wm.set(k, 1);
        wm.delete(k);
        wm.has(k)
    )");
    EXPECT_TRUE(v.is_bool() && v.as_bool() == false);
}

TEST(MapSetInterp, MS39_WeakMapNoSize) {
    auto v = interp_ok("var wm = new WeakMap(); typeof wm.size");
    EXPECT_EQ(v.sv(), "undefined");
}

TEST(MapSetVM, MS39_WeakMapNoSize) {
    auto v = vm_ok("var wm = new WeakMap(); typeof wm.size");
    EXPECT_EQ(v.sv(), "undefined");
}

TEST(MapSetInterp, MS40_WeakMapFromIterable) {
    auto v = interp_ok(R"(
        var k1 = {}; var k2 = {};
        var wm = new WeakMap([[k1, 10], [k2, 20]]);
        wm.get(k1) + wm.get(k2)
    )");
    EXPECT_EQ(v.as_number(), 30.0);
}

TEST(MapSetVM, MS40_WeakMapFromIterable) {
    auto v = vm_ok(R"(
        var k1 = {}; var k2 = {};
        var wm = new WeakMap([[k1, 10], [k2, 20]]);
        wm.get(k1) + wm.get(k2)
    )");
    EXPECT_EQ(v.as_number(), 30.0);
}

// ---- WeakSet tests (MS-41 ~ MS-45) ----

TEST(MapSetInterp, MS41_WeakSetAddHas) {
    auto v = interp_ok(R"(
        var ws = new WeakSet();
        var o = {};
        ws.add(o);
        ws.has(o)
    )");
    EXPECT_TRUE(v.is_bool() && v.as_bool() == true);
}

TEST(MapSetVM, MS41_WeakSetAddHas) {
    auto v = vm_ok(R"(
        var ws = new WeakSet();
        var o = {};
        ws.add(o);
        ws.has(o)
    )");
    EXPECT_TRUE(v.is_bool() && v.as_bool() == true);
}

TEST(MapSetInterp, MS42_WeakSetNonObjectThrows) {
    EXPECT_TRUE(interp_throws(R"(
        var ws = new WeakSet();
        ws.add(42);
    )"));
}

TEST(MapSetVM, MS42_WeakSetNonObjectThrows) {
    EXPECT_TRUE(vm_throws(R"(
        var ws = new WeakSet();
        ws.add(42);
    )"));
}

TEST(MapSetInterp, MS43_WeakSetDeleteHas) {
    auto v = interp_ok(R"(
        var ws = new WeakSet();
        var o = {};
        ws.add(o);
        ws.delete(o);
        ws.has(o)
    )");
    EXPECT_TRUE(v.is_bool() && v.as_bool() == false);
}

TEST(MapSetVM, MS43_WeakSetDeleteHas) {
    auto v = vm_ok(R"(
        var ws = new WeakSet();
        var o = {};
        ws.add(o);
        ws.delete(o);
        ws.has(o)
    )");
    EXPECT_TRUE(v.is_bool() && v.as_bool() == false);
}

TEST(MapSetInterp, MS44_WeakSetNoSize) {
    auto v = interp_ok("var ws = new WeakSet(); typeof ws.size");
    EXPECT_EQ(v.sv(), "undefined");
}

TEST(MapSetVM, MS44_WeakSetNoSize) {
    auto v = vm_ok("var ws = new WeakSet(); typeof ws.size");
    EXPECT_EQ(v.sv(), "undefined");
}

TEST(MapSetInterp, MS45_WeakSetFromIterable) {
    auto v = interp_ok(R"(
        var o1 = {}; var o2 = {};
        var ws = new WeakSet([o1, o2]);
        ws.has(o1) && ws.has(o2)
    )");
    EXPECT_TRUE(v.is_bool() && v.as_bool() == true);
}

TEST(MapSetVM, MS45_WeakSetFromIterable) {
    auto v = vm_ok(R"(
        var o1 = {}; var o2 = {};
        var ws = new WeakSet([o1, o2]);
        ws.has(o1) && ws.has(o2)
    )");
    EXPECT_TRUE(v.is_bool() && v.as_bool() == true);
}

}  // namespace
