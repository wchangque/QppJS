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
// AM-01: entries() 迭代器 - next() 返回 [0,1], [1,2], [2,3]
// ============================================================

TEST(ArrayMethods, AM01_EntriesInterp) {
    auto v = interp_ok(R"(
        let it = [1,2,3].entries();
        let r0 = it.next();
        let r1 = it.next();
        let r2 = it.next();
        let r3 = it.next();
        r0.value[0] === 0 && r0.value[1] === 1 &&
        r1.value[0] === 1 && r1.value[1] === 2 &&
        r2.value[0] === 2 && r2.value[1] === 3 &&
        r3.done === true
    )");
    EXPECT_TRUE(v.is_bool() && v.as_bool());
}

TEST(ArrayMethods, AM01_EntriesVM) {
    auto v = vm_ok(R"(
        let it = [1,2,3].entries();
        let r0 = it.next();
        let r1 = it.next();
        let r2 = it.next();
        let r3 = it.next();
        r0.value[0] === 0 && r0.value[1] === 1 &&
        r1.value[0] === 1 && r1.value[1] === 2 &&
        r2.value[0] === 2 && r2.value[1] === 3 &&
        r3.done === true
    )");
    EXPECT_TRUE(v.is_bool() && v.as_bool());
}

// ============================================================
// AM-02: keys() 迭代器 - next() 返回 0,1,2
// ============================================================

TEST(ArrayMethods, AM02_KeysInterp) {
    auto v = interp_ok(R"(
        let it = [1,2,3].keys();
        let r0 = it.next();
        let r1 = it.next();
        let r2 = it.next();
        let r3 = it.next();
        r0.value === 0 && r1.value === 1 && r2.value === 2 && r3.done === true
    )");
    EXPECT_TRUE(v.is_bool() && v.as_bool());
}

TEST(ArrayMethods, AM02_KeysVM) {
    auto v = vm_ok(R"(
        let it = [1,2,3].keys();
        let r0 = it.next();
        let r1 = it.next();
        let r2 = it.next();
        let r3 = it.next();
        r0.value === 0 && r1.value === 1 && r2.value === 2 && r3.done === true
    )");
    EXPECT_TRUE(v.is_bool() && v.as_bool());
}

// ============================================================
// AM-03: values() 迭代器 - next() 返回 1,2,3
// ============================================================

TEST(ArrayMethods, AM03_ValuesInterp) {
    auto v = interp_ok(R"(
        let it = [1,2,3].values();
        let r0 = it.next();
        let r1 = it.next();
        let r2 = it.next();
        let r3 = it.next();
        r0.value === 1 && r1.value === 2 && r2.value === 3 && r3.done === true
    )");
    EXPECT_TRUE(v.is_bool() && v.as_bool());
}

TEST(ArrayMethods, AM03_ValuesVM) {
    auto v = vm_ok(R"(
        let it = [1,2,3].values();
        let r0 = it.next();
        let r1 = it.next();
        let r2 = it.next();
        let r3 = it.next();
        r0.value === 1 && r1.value === 2 && r2.value === 3 && r3.done === true
    )");
    EXPECT_TRUE(v.is_bool() && v.as_bool());
}

// ============================================================
// AM-04: entries/keys/values for...of 消费
// ============================================================

TEST(ArrayMethods, AM04_ForOfEntriesInterp) {
    auto v = interp_ok(R"(
        let result = [];
        for (let [i, val] of [10,20,30].entries()) {
            result.push(i * 100 + val);
        }
        result[0] === 10 && result[1] === 120 && result[2] === 230
    )");
    EXPECT_TRUE(v.is_bool() && v.as_bool());
}

TEST(ArrayMethods, AM04_ForOfEntriesVM) {
    auto v = vm_ok(R"(
        let result = [];
        for (let [i, val] of [10,20,30].entries()) {
            result.push(i * 100 + val);
        }
        result[0] === 10 && result[1] === 120 && result[2] === 230
    )");
    EXPECT_TRUE(v.is_bool() && v.as_bool());
}

TEST(ArrayMethods, AM04_ForOfKeysInterp) {
    auto v = interp_ok(R"(
        let sum = 0;
        for (let k of ['a','b','c'].keys()) { sum += k; }
        sum
    )");
    EXPECT_EQ(v.as_number(), 3.0);
}

TEST(ArrayMethods, AM04_ForOfKeysVM) {
    auto v = vm_ok(R"(
        let sum = 0;
        for (let k of ['a','b','c'].keys()) { sum += k; }
        sum
    )");
    EXPECT_EQ(v.as_number(), 3.0);
}

TEST(ArrayMethods, AM04_ForOfValuesInterp) {
    auto v = interp_ok(R"(
        let sum = 0;
        for (let val of [1,2,3].values()) { sum += val; }
        sum
    )");
    EXPECT_EQ(v.as_number(), 6.0);
}

TEST(ArrayMethods, AM04_ForOfValuesVM) {
    auto v = vm_ok(R"(
        let sum = 0;
        for (let val of [1,2,3].values()) { sum += val; }
        sum
    )");
    EXPECT_EQ(v.as_number(), 6.0);
}

// ============================================================
// AM-05: findLast(fn) 从末尾找第一个满足条件的元素
// ============================================================

TEST(ArrayMethods, AM05_FindLastInterp) {
    // [1,3,2] 从末尾找 x<3：索引2值2 < 3，返回2
    auto v = interp_ok("[1,3,2].findLast(function(x){ return x < 3; })");
    EXPECT_EQ(v.as_number(), 2.0);
}

TEST(ArrayMethods, AM05_FindLastVM) {
    auto v = vm_ok("[1,3,2].findLast(function(x){ return x < 3; })");
    EXPECT_EQ(v.as_number(), 2.0);
}

TEST(ArrayMethods, AM05_FindLastNotFoundInterp) {
    auto v = interp_ok("[3,2,1].findLast(function(x){ return x > 10; })");
    EXPECT_TRUE(v.is_undefined());
}

TEST(ArrayMethods, AM05_FindLastNotFoundVM) {
    auto v = vm_ok("[3,2,1].findLast(function(x){ return x > 10; })");
    EXPECT_TRUE(v.is_undefined());
}

// ============================================================
// AM-06: findLastIndex(fn) 从末尾找第一个满足条件的元素的索引
// ============================================================

TEST(ArrayMethods, AM06_FindLastIndexInterp) {
    // [1,2,3] 从末尾找 x<3：索引1值2 < 3，返回索引1
    auto v = interp_ok("[1,2,3].findLastIndex(function(x){ return x < 3; })");
    EXPECT_EQ(v.as_number(), 1.0);
}

TEST(ArrayMethods, AM06_FindLastIndexVM) {
    auto v = vm_ok("[1,2,3].findLastIndex(function(x){ return x < 3; })");
    EXPECT_EQ(v.as_number(), 1.0);
}

TEST(ArrayMethods, AM06_FindLastIndexNotFoundInterp) {
    auto v = interp_ok("[1,2,3].findLastIndex(function(x){ return x > 10; })");
    EXPECT_EQ(v.as_number(), -1.0);
}

TEST(ArrayMethods, AM06_FindLastIndexNotFoundVM) {
    auto v = vm_ok("[1,2,3].findLastIndex(function(x){ return x > 10; })");
    EXPECT_EQ(v.as_number(), -1.0);
}

// ============================================================
// AM-07: toSorted(compareFn?) 返回排好序的新数组
// ============================================================

TEST(ArrayMethods, AM07_ToSortedInterp) {
    auto v = interp_ok(R"(
        let a = [3,1,2];
        let b = a.toSorted();
        b[0] === 1 && b[1] === 2 && b[2] === 3 && a[0] === 3
    )");
    EXPECT_TRUE(v.is_bool() && v.as_bool());
}

TEST(ArrayMethods, AM07_ToSortedVM) {
    auto v = vm_ok(R"(
        let a = [3,1,2];
        let b = a.toSorted();
        b[0] === 1 && b[1] === 2 && b[2] === 3 && a[0] === 3
    )");
    EXPECT_TRUE(v.is_bool() && v.as_bool());
}

TEST(ArrayMethods, AM07_ToSortedWithCompareFnInterp) {
    auto v = interp_ok(R"(
        let b = [3,1,2].toSorted(function(a,b){ return b - a; });
        b[0] === 3 && b[1] === 2 && b[2] === 1
    )");
    EXPECT_TRUE(v.is_bool() && v.as_bool());
}

TEST(ArrayMethods, AM07_ToSortedWithCompareFnVM) {
    auto v = vm_ok(R"(
        let b = [3,1,2].toSorted(function(a,b){ return b - a; });
        b[0] === 3 && b[1] === 2 && b[2] === 1
    )");
    EXPECT_TRUE(v.is_bool() && v.as_bool());
}

// ============================================================
// AM-08: toReversed() 返回反转的新数组
// ============================================================

TEST(ArrayMethods, AM08_ToReversedInterp) {
    auto v = interp_ok(R"(
        let a = [1,2,3];
        let b = a.toReversed();
        b[0] === 3 && b[1] === 2 && b[2] === 1 && a[0] === 1
    )");
    EXPECT_TRUE(v.is_bool() && v.as_bool());
}

TEST(ArrayMethods, AM08_ToReversedVM) {
    auto v = vm_ok(R"(
        let a = [1,2,3];
        let b = a.toReversed();
        b[0] === 3 && b[1] === 2 && b[2] === 1 && a[0] === 1
    )");
    EXPECT_TRUE(v.is_bool() && v.as_bool());
}

// ============================================================
// AM-09: toSpliced(start, deleteCount, ...items) 返回拼接后的新数组
// ============================================================

TEST(ArrayMethods, AM09_ToSplicedInterp) {
    auto v = interp_ok(R"(
        let a = [1,2,3];
        let b = a.toSpliced(1, 1, 4);
        b[0] === 1 && b[1] === 4 && b[2] === 3 && a[1] === 2
    )");
    EXPECT_TRUE(v.is_bool() && v.as_bool());
}

TEST(ArrayMethods, AM09_ToSplicedVM) {
    auto v = vm_ok(R"(
        let a = [1,2,3];
        let b = a.toSpliced(1, 1, 4);
        b[0] === 1 && b[1] === 4 && b[2] === 3 && a[1] === 2
    )");
    EXPECT_TRUE(v.is_bool() && v.as_bool());
}

TEST(ArrayMethods, AM09_ToSplicedNoItemsInterp) {
    auto v = interp_ok(R"(
        let b = [1,2,3].toSpliced(1, 1);
        b[0] === 1 && b[1] === 3 && b.length === 2
    )");
    EXPECT_TRUE(v.is_bool() && v.as_bool());
}

TEST(ArrayMethods, AM09_ToSplicedNoItemsVM) {
    auto v = vm_ok(R"(
        let b = [1,2,3].toSpliced(1, 1);
        b[0] === 1 && b[1] === 3 && b.length === 2
    )");
    EXPECT_TRUE(v.is_bool() && v.as_bool());
}

// ============================================================
// AM-10: with(index, value) 返回替换指定索引后的新数组
// ============================================================

TEST(ArrayMethods, AM10_WithInterp) {
    auto v = interp_ok(R"(
        let a = [1,2,3];
        let b = a.with(1, 99);
        b[0] === 1 && b[1] === 99 && b[2] === 3 && a[1] === 2
    )");
    EXPECT_TRUE(v.is_bool() && v.as_bool());
}

TEST(ArrayMethods, AM10_WithVM) {
    auto v = vm_ok(R"(
        let a = [1,2,3];
        let b = a.with(1, 99);
        b[0] === 1 && b[1] === 99 && b[2] === 3 && a[1] === 2
    )");
    EXPECT_TRUE(v.is_bool() && v.as_bool());
}

TEST(ArrayMethods, AM10_WithNegativeIndexInterp) {
    auto v = interp_ok(R"(
        let b = [1,2,3].with(-1, 99);
        b[0] === 1 && b[1] === 2 && b[2] === 99
    )");
    EXPECT_TRUE(v.is_bool() && v.as_bool());
}

TEST(ArrayMethods, AM10_WithNegativeIndexVM) {
    auto v = vm_ok(R"(
        let b = [1,2,3].with(-1, 99);
        b[0] === 1 && b[1] === 2 && b[2] === 99
    )");
    EXPECT_TRUE(v.is_bool() && v.as_bool());
}

}  // namespace
