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
// PAO-01: Promise.all 全部 resolve -> 结果数组
// ============================================================

TEST(PromiseArrayObject, PAO01_Interp_PromiseAll_AllResolve) {
    auto v = interp_ok(R"(
        var result;
        Promise.all([
            Promise.resolve(1),
            Promise.resolve(2),
            Promise.resolve(3)
        ]).then(function(vals) { result = vals; });
        result
    )");
    ASSERT_TRUE(v.is_object());
    auto* arr = static_cast<JSObject*>(v.as_object_raw());
    EXPECT_EQ(arr->array_length_, 3u);
    EXPECT_EQ(arr->elements_.at(0).as_number(), 1.0);
    EXPECT_EQ(arr->elements_.at(1).as_number(), 2.0);
    EXPECT_EQ(arr->elements_.at(2).as_number(), 3.0);
}

TEST(PromiseArrayObject, PAO01_VM_PromiseAll_AllResolve) {
    auto v = vm_ok(R"(
        var result;
        Promise.all([
            Promise.resolve(1),
            Promise.resolve(2),
            Promise.resolve(3)
        ]).then(function(vals) { result = vals; });
        result
    )");
    ASSERT_TRUE(v.is_object());
    auto* arr = static_cast<JSObject*>(v.as_object_raw());
    EXPECT_EQ(arr->array_length_, 3u);
    EXPECT_EQ(arr->elements_.at(0).as_number(), 1.0);
    EXPECT_EQ(arr->elements_.at(1).as_number(), 2.0);
    EXPECT_EQ(arr->elements_.at(2).as_number(), 3.0);
}

// ============================================================
// PAO-02: Promise.all 任一 reject -> 整体 reject
// ============================================================

TEST(PromiseArrayObject, PAO02_Interp_PromiseAll_AnyReject) {
    auto v = interp_ok(R"(
        var result = "none";
        Promise.all([
            Promise.resolve(1),
            Promise.reject("boom"),
            Promise.resolve(3)
        ]).catch(function(r) { result = r; });
        result
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "boom");
}

TEST(PromiseArrayObject, PAO02_VM_PromiseAll_AnyReject) {
    auto v = vm_ok(R"(
        var result = "none";
        Promise.all([
            Promise.resolve(1),
            Promise.reject("boom"),
            Promise.resolve(3)
        ]).catch(function(r) { result = r; });
        result
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "boom");
}

// ============================================================
// PAO-03: Promise.all 空数组 -> resolve []
// ============================================================

TEST(PromiseArrayObject, PAO03_Interp_PromiseAll_Empty) {
    auto v = interp_ok(R"(
        var result = null;
        Promise.all([]).then(function(vals) { result = vals; });
        result
    )");
    ASSERT_TRUE(v.is_object());
    auto* arr = static_cast<JSObject*>(v.as_object_raw());
    EXPECT_EQ(arr->array_length_, 0u);
}

TEST(PromiseArrayObject, PAO03_VM_PromiseAll_Empty) {
    auto v = vm_ok(R"(
        var result = null;
        Promise.all([]).then(function(vals) { result = vals; });
        result
    )");
    ASSERT_TRUE(v.is_object());
    auto* arr = static_cast<JSObject*>(v.as_object_raw());
    EXPECT_EQ(arr->array_length_, 0u);
}

// ============================================================
// PAO-04: Promise.all 保序（各 resolve 时间不影响顺序）
// ============================================================

TEST(PromiseArrayObject, PAO04_Interp_PromiseAll_Order) {
    auto v = interp_ok(R"(
        var result;
        Promise.all([
            Promise.resolve("a"),
            Promise.resolve("b"),
            Promise.resolve("c")
        ]).then(function(vals) { result = vals[0] + vals[1] + vals[2]; });
        result
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "abc");
}

TEST(PromiseArrayObject, PAO04_VM_PromiseAll_Order) {
    auto v = vm_ok(R"(
        var result;
        Promise.all([
            Promise.resolve("a"),
            Promise.resolve("b"),
            Promise.resolve("c")
        ]).then(function(vals) { result = vals[0] + vals[1] + vals[2]; });
        result
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "abc");
}

// ============================================================
// PAO-05: Promise.race 第一个 resolve 赢
// ============================================================

TEST(PromiseArrayObject, PAO05_Interp_PromiseRace_FirstResolve) {
    auto v = interp_ok(R"(
        var result = "none";
        Promise.race([
            Promise.resolve("first"),
            Promise.resolve("second")
        ]).then(function(v) { result = v; });
        result
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "first");
}

TEST(PromiseArrayObject, PAO05_VM_PromiseRace_FirstResolve) {
    auto v = vm_ok(R"(
        var result = "none";
        Promise.race([
            Promise.resolve("first"),
            Promise.resolve("second")
        ]).then(function(v) { result = v; });
        result
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "first");
}

// ============================================================
// PAO-06: Promise.race 第一个 reject 赢
// ============================================================

TEST(PromiseArrayObject, PAO06_Interp_PromiseRace_FirstReject) {
    auto v = interp_ok(R"(
        var result = "none";
        Promise.race([
            Promise.reject("err"),
            Promise.resolve("ok")
        ]).catch(function(r) { result = r; });
        result
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "err");
}

TEST(PromiseArrayObject, PAO06_VM_PromiseRace_FirstReject) {
    auto v = vm_ok(R"(
        var result = "none";
        Promise.race([
            Promise.reject("err"),
            Promise.resolve("ok")
        ]).catch(function(r) { result = r; });
        result
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "err");
}

// ============================================================
// PAO-07: Promise.allSettled 全部 resolve -> [{status:"fulfilled",value}]
// ============================================================

TEST(PromiseArrayObject, PAO07_Interp_PromiseAllSettled_AllResolve) {
    auto v = interp_ok(R"(
        var result;
        Promise.allSettled([
            Promise.resolve(42),
            Promise.resolve("hello")
        ]).then(function(arr) { result = arr; });
        result
    )");
    ASSERT_TRUE(v.is_object());
    auto* arr = static_cast<JSObject*>(v.as_object_raw());
    ASSERT_EQ(arr->array_length_, 2u);
    auto* e0 = static_cast<JSObject*>(arr->elements_.at(0).as_object_raw());
    EXPECT_EQ(e0->get_property("status").sv(), "fulfilled");
    EXPECT_EQ(e0->get_property("value").as_number(), 42.0);
    auto* e1 = static_cast<JSObject*>(arr->elements_.at(1).as_object_raw());
    EXPECT_EQ(e1->get_property("status").sv(), "fulfilled");
    EXPECT_EQ(e1->get_property("value").sv(), "hello");
}

TEST(PromiseArrayObject, PAO07_VM_PromiseAllSettled_AllResolve) {
    auto v = vm_ok(R"(
        var result;
        Promise.allSettled([
            Promise.resolve(42),
            Promise.resolve("hello")
        ]).then(function(arr) { result = arr; });
        result
    )");
    ASSERT_TRUE(v.is_object());
    auto* arr = static_cast<JSObject*>(v.as_object_raw());
    ASSERT_EQ(arr->array_length_, 2u);
    auto* e0 = static_cast<JSObject*>(arr->elements_.at(0).as_object_raw());
    EXPECT_EQ(e0->get_property("status").sv(), "fulfilled");
    EXPECT_EQ(e0->get_property("value").as_number(), 42.0);
    auto* e1 = static_cast<JSObject*>(arr->elements_.at(1).as_object_raw());
    EXPECT_EQ(e1->get_property("status").sv(), "fulfilled");
    EXPECT_EQ(e1->get_property("value").sv(), "hello");
}

// ============================================================
// PAO-08: Promise.allSettled 混合 -> [{status:"fulfilled"},{status:"rejected"}]
// ============================================================

TEST(PromiseArrayObject, PAO08_Interp_PromiseAllSettled_Mixed) {
    auto v = interp_ok(R"(
        var result;
        Promise.allSettled([
            Promise.resolve(1),
            Promise.reject("err")
        ]).then(function(arr) { result = arr; });
        result
    )");
    ASSERT_TRUE(v.is_object());
    auto* arr = static_cast<JSObject*>(v.as_object_raw());
    ASSERT_EQ(arr->array_length_, 2u);
    auto* e0 = static_cast<JSObject*>(arr->elements_.at(0).as_object_raw());
    EXPECT_EQ(e0->get_property("status").sv(), "fulfilled");
    EXPECT_EQ(e0->get_property("value").as_number(), 1.0);
    auto* e1 = static_cast<JSObject*>(arr->elements_.at(1).as_object_raw());
    EXPECT_EQ(e1->get_property("status").sv(), "rejected");
    EXPECT_EQ(e1->get_property("reason").sv(), "err");
}

TEST(PromiseArrayObject, PAO08_VM_PromiseAllSettled_Mixed) {
    auto v = vm_ok(R"(
        var result;
        Promise.allSettled([
            Promise.resolve(1),
            Promise.reject("err")
        ]).then(function(arr) { result = arr; });
        result
    )");
    ASSERT_TRUE(v.is_object());
    auto* arr = static_cast<JSObject*>(v.as_object_raw());
    ASSERT_EQ(arr->array_length_, 2u);
    auto* e0 = static_cast<JSObject*>(arr->elements_.at(0).as_object_raw());
    EXPECT_EQ(e0->get_property("status").sv(), "fulfilled");
    EXPECT_EQ(e0->get_property("value").as_number(), 1.0);
    auto* e1 = static_cast<JSObject*>(arr->elements_.at(1).as_object_raw());
    EXPECT_EQ(e1->get_property("status").sv(), "rejected");
    EXPECT_EQ(e1->get_property("reason").sv(), "err");
}

// ============================================================
// PAO-09: Promise.any 有 resolve -> 第一个 resolve 的值
// ============================================================

TEST(PromiseArrayObject, PAO09_Interp_PromiseAny_FirstResolve) {
    auto v = interp_ok(R"(
        var result = "none";
        Promise.any([
            Promise.reject("e1"),
            Promise.resolve("ok"),
            Promise.resolve("ok2")
        ]).then(function(v) { result = v; });
        result
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "ok");
}

TEST(PromiseArrayObject, PAO09_VM_PromiseAny_FirstResolve) {
    auto v = vm_ok(R"(
        var result = "none";
        Promise.any([
            Promise.reject("e1"),
            Promise.resolve("ok"),
            Promise.resolve("ok2")
        ]).then(function(v) { result = v; });
        result
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "ok");
}

// ============================================================
// PAO-10: Promise.any 全部 reject -> AggregateError (errors 数组)
// ============================================================

TEST(PromiseArrayObject, PAO10_Interp_PromiseAny_AllReject) {
    auto v = interp_ok(R"(
        var result = "none";
        Promise.any([
            Promise.reject("e1"),
            Promise.reject("e2")
        ]).catch(function(e) {
            result = e.message + ":" + e.errors.length;
        });
        result
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "All promises were rejected:2");
}

TEST(PromiseArrayObject, PAO10_VM_PromiseAny_AllReject) {
    auto v = vm_ok(R"(
        var result = "none";
        Promise.any([
            Promise.reject("e1"),
            Promise.reject("e2")
        ]).catch(function(e) {
            result = e.message + ":" + e.errors.length;
        });
        result
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "All promises were rejected:2");
}

// ============================================================
// PAO-11: Array.from([1,2,3]) -> [1,2,3]
// ============================================================

TEST(PromiseArrayObject, PAO11_Interp_ArrayFrom_Array) {
    auto v = interp_ok("Array.from([1,2,3])");
    ASSERT_TRUE(v.is_object());
    auto* arr = static_cast<JSObject*>(v.as_object_raw());
    EXPECT_EQ(arr->array_length_, 3u);
    EXPECT_EQ(arr->elements_.at(0).as_number(), 1.0);
    EXPECT_EQ(arr->elements_.at(1).as_number(), 2.0);
    EXPECT_EQ(arr->elements_.at(2).as_number(), 3.0);
}

TEST(PromiseArrayObject, PAO11_VM_ArrayFrom_Array) {
    auto v = vm_ok("Array.from([1,2,3])");
    ASSERT_TRUE(v.is_object());
    auto* arr = static_cast<JSObject*>(v.as_object_raw());
    EXPECT_EQ(arr->array_length_, 3u);
    EXPECT_EQ(arr->elements_.at(0).as_number(), 1.0);
    EXPECT_EQ(arr->elements_.at(1).as_number(), 2.0);
    EXPECT_EQ(arr->elements_.at(2).as_number(), 3.0);
}

// ============================================================
// PAO-12: Array.from("abc") -> ["a","b","c"]
// ============================================================

TEST(PromiseArrayObject, PAO12_Interp_ArrayFrom_String) {
    auto v = interp_ok(R"(Array.from("abc"))");
    ASSERT_TRUE(v.is_object());
    auto* arr = static_cast<JSObject*>(v.as_object_raw());
    EXPECT_EQ(arr->array_length_, 3u);
    EXPECT_EQ(arr->elements_.at(0).sv(), "a");
    EXPECT_EQ(arr->elements_.at(1).sv(), "b");
    EXPECT_EQ(arr->elements_.at(2).sv(), "c");
}

TEST(PromiseArrayObject, PAO12_VM_ArrayFrom_String) {
    auto v = vm_ok(R"(Array.from("abc"))");
    ASSERT_TRUE(v.is_object());
    auto* arr = static_cast<JSObject*>(v.as_object_raw());
    EXPECT_EQ(arr->array_length_, 3u);
    EXPECT_EQ(arr->elements_.at(0).sv(), "a");
    EXPECT_EQ(arr->elements_.at(1).sv(), "b");
    EXPECT_EQ(arr->elements_.at(2).sv(), "c");
}

// ============================================================
// PAO-13: Array.from({length:2,0:"a",1:"b"}) -> array-like
// ============================================================

TEST(PromiseArrayObject, PAO13_Interp_ArrayFrom_ArrayLike) {
    auto v = interp_ok(R"(Array.from({length:2, 0:"a", 1:"b"}))");
    ASSERT_TRUE(v.is_object());
    auto* arr = static_cast<JSObject*>(v.as_object_raw());
    EXPECT_EQ(arr->array_length_, 2u);
    EXPECT_EQ(arr->elements_.at(0).sv(), "a");
    EXPECT_EQ(arr->elements_.at(1).sv(), "b");
}

TEST(PromiseArrayObject, PAO13_VM_ArrayFrom_ArrayLike) {
    auto v = vm_ok(R"(Array.from({length:2, 0:"a", 1:"b"}))");
    ASSERT_TRUE(v.is_object());
    auto* arr = static_cast<JSObject*>(v.as_object_raw());
    EXPECT_EQ(arr->array_length_, 2u);
    EXPECT_EQ(arr->elements_.at(0).sv(), "a");
    EXPECT_EQ(arr->elements_.at(1).sv(), "b");
}

// ============================================================
// PAO-14: Array.from([1,2,3], x => x*2) -> [2,4,6]
// ============================================================

TEST(PromiseArrayObject, PAO14_Interp_ArrayFrom_MapFn) {
    auto v = interp_ok("Array.from([1,2,3], function(x) { return x * 2; })");
    ASSERT_TRUE(v.is_object());
    auto* arr = static_cast<JSObject*>(v.as_object_raw());
    EXPECT_EQ(arr->array_length_, 3u);
    EXPECT_EQ(arr->elements_.at(0).as_number(), 2.0);
    EXPECT_EQ(arr->elements_.at(1).as_number(), 4.0);
    EXPECT_EQ(arr->elements_.at(2).as_number(), 6.0);
}

TEST(PromiseArrayObject, PAO14_VM_ArrayFrom_MapFn) {
    auto v = vm_ok("Array.from([1,2,3], function(x) { return x * 2; })");
    ASSERT_TRUE(v.is_object());
    auto* arr = static_cast<JSObject*>(v.as_object_raw());
    EXPECT_EQ(arr->array_length_, 3u);
    EXPECT_EQ(arr->elements_.at(0).as_number(), 2.0);
    EXPECT_EQ(arr->elements_.at(1).as_number(), 4.0);
    EXPECT_EQ(arr->elements_.at(2).as_number(), 6.0);
}

// ============================================================
// PAO-15: Array.of(1,2,3) -> [1,2,3]
// ============================================================

TEST(PromiseArrayObject, PAO15_Interp_ArrayOf) {
    auto v = interp_ok("Array.of(1, 2, 3)");
    ASSERT_TRUE(v.is_object());
    auto* arr = static_cast<JSObject*>(v.as_object_raw());
    EXPECT_EQ(arr->array_length_, 3u);
    EXPECT_EQ(arr->elements_.at(0).as_number(), 1.0);
    EXPECT_EQ(arr->elements_.at(1).as_number(), 2.0);
    EXPECT_EQ(arr->elements_.at(2).as_number(), 3.0);
}

TEST(PromiseArrayObject, PAO15_VM_ArrayOf) {
    auto v = vm_ok("Array.of(1, 2, 3)");
    ASSERT_TRUE(v.is_object());
    auto* arr = static_cast<JSObject*>(v.as_object_raw());
    EXPECT_EQ(arr->array_length_, 3u);
    EXPECT_EQ(arr->elements_.at(0).as_number(), 1.0);
    EXPECT_EQ(arr->elements_.at(1).as_number(), 2.0);
    EXPECT_EQ(arr->elements_.at(2).as_number(), 3.0);
}

// ============================================================
// PAO-16: Object.entries({a:1,b:2}) -> [["a",1],["b",2]]
// ============================================================

TEST(PromiseArrayObject, PAO16_Interp_ObjectEntries) {
    auto v = interp_ok(R"(
        var e = Object.entries({a:1, b:2});
        e[0][0] + "=" + e[0][1] + "," + e[1][0] + "=" + e[1][1]
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "a=1,b=2");
}

TEST(PromiseArrayObject, PAO16_VM_ObjectEntries) {
    auto v = vm_ok(R"(
        var e = Object.entries({a:1, b:2});
        e[0][0] + "=" + e[0][1] + "," + e[1][0] + "=" + e[1][1]
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "a=1,b=2");
}

// ============================================================
// PAO-17: Object.values({a:1,b:2}) -> [1,2]
// ============================================================

TEST(PromiseArrayObject, PAO17_Interp_ObjectValues) {
    auto v = interp_ok("Object.values({a:1, b:2})");
    ASSERT_TRUE(v.is_object());
    auto* arr = static_cast<JSObject*>(v.as_object_raw());
    EXPECT_EQ(arr->array_length_, 2u);
    EXPECT_EQ(arr->elements_.at(0).as_number(), 1.0);
    EXPECT_EQ(arr->elements_.at(1).as_number(), 2.0);
}

TEST(PromiseArrayObject, PAO17_VM_ObjectValues) {
    auto v = vm_ok("Object.values({a:1, b:2})");
    ASSERT_TRUE(v.is_object());
    auto* arr = static_cast<JSObject*>(v.as_object_raw());
    EXPECT_EQ(arr->array_length_, 2u);
    EXPECT_EQ(arr->elements_.at(0).as_number(), 1.0);
    EXPECT_EQ(arr->elements_.at(1).as_number(), 2.0);
}

// ============================================================
// PAO-18: Object.fromEntries([["a",1],["b",2]]) -> {a:1,b:2}
// ============================================================

TEST(PromiseArrayObject, PAO18_Interp_ObjectFromEntries_Array) {
    auto v = interp_ok(R"(
        var o = Object.fromEntries([["a",1],["b",2]]);
        o.a + o.b
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 3.0);
}

TEST(PromiseArrayObject, PAO18_VM_ObjectFromEntries_Array) {
    auto v = vm_ok(R"(
        var o = Object.fromEntries([["a",1],["b",2]]);
        o.a + o.b
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 3.0);
}

// ============================================================
// PAO-19: Object.fromEntries(new Map([["a",1]])) -> {a:1}
// ============================================================

TEST(PromiseArrayObject, PAO19_Interp_ObjectFromEntries_Map) {
    auto v = interp_ok(R"(
        var m = new Map([["a", 1], ["b", 2]]);
        var o = Object.fromEntries(m);
        o.a + o.b
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 3.0);
}

TEST(PromiseArrayObject, PAO19_VM_ObjectFromEntries_Map) {
    auto v = vm_ok(R"(
        var m = new Map([["a", 1], ["b", 2]]);
        var o = Object.fromEntries(m);
        o.a + o.b
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 3.0);
}

// ============================================================
// PAO-20: Object.getOwnPropertyNames 含 non-enumerable
// ============================================================

TEST(PromiseArrayObject, PAO20_Interp_GetOwnPropertyNames) {
    auto v = interp_ok(R"(
        var o = {};
        Object.defineProperty(o, "hidden", {value: 1, enumerable: false, configurable: true, writable: true});
        o.visible = 2;
        var names = Object.getOwnPropertyNames(o);
        names.indexOf("hidden") >= 0 && names.indexOf("visible") >= 0
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(PromiseArrayObject, PAO20_VM_GetOwnPropertyNames) {
    auto v = vm_ok(R"(
        var o = {};
        Object.defineProperty(o, "hidden", {value: 1, enumerable: false, configurable: true, writable: true});
        o.visible = 2;
        var names = Object.getOwnPropertyNames(o);
        names.indexOf("hidden") >= 0 && names.indexOf("visible") >= 0
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

}  // namespace
