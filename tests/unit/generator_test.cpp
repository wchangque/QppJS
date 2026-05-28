#include "qppjs/frontend/ast.h"
#include "qppjs/frontend/ast_dump.h"
#include "qppjs/frontend/parser.h"
#include "qppjs/runtime/interpreter.h"
#include "qppjs/vm/compiler.h"
#include "qppjs/vm/vm.h"

#include <gtest/gtest.h>

#include <string_view>

using namespace qppjs;

namespace {

// ---- helpers ----

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

// ============================================================
// GEN-01: 基础 yield — function* gen() { yield 1; yield 2; }
// ============================================================

TEST(Generator, GEN01_Interp_BasicYield) {
    auto v = interp_ok(R"(
        function* gen() { yield 1; yield 2; }
        var g = gen();
        var r1 = g.next();
        var r2 = g.next();
        var r3 = g.next();
        [r1.value, r1.done, r2.value, r2.done, r3.value, r3.done]
    )");
    ASSERT_TRUE(v.is_object());
    auto* arr = static_cast<JSObject*>(v.as_object_raw());
    EXPECT_DOUBLE_EQ(arr->elements_.at(0).as_number(), 1.0);
    EXPECT_TRUE(arr->elements_.at(1).is_bool() && !arr->elements_.at(1).as_bool());
    EXPECT_DOUBLE_EQ(arr->elements_.at(2).as_number(), 2.0);
    EXPECT_TRUE(arr->elements_.at(3).is_bool() && !arr->elements_.at(3).as_bool());
    EXPECT_TRUE(arr->elements_.at(4).is_undefined());
    EXPECT_TRUE(arr->elements_.at(5).is_bool() && arr->elements_.at(5).as_bool());
}

TEST(Generator, GEN01_VM_BasicYield) {
    auto v = vm_ok(R"(
        function* gen() { yield 1; yield 2; }
        var g = gen();
        var r1 = g.next();
        var r2 = g.next();
        var r3 = g.next();
        [r1.value, r1.done, r2.value, r2.done, r3.value, r3.done]
    )");
    ASSERT_TRUE(v.is_object());
    auto* arr = static_cast<JSObject*>(v.as_object_raw());
    EXPECT_DOUBLE_EQ(arr->elements_.at(0).as_number(), 1.0);
    EXPECT_TRUE(arr->elements_.at(1).is_bool() && !arr->elements_.at(1).as_bool());
    EXPECT_DOUBLE_EQ(arr->elements_.at(2).as_number(), 2.0);
    EXPECT_TRUE(arr->elements_.at(3).is_bool() && !arr->elements_.at(3).as_bool());
    EXPECT_TRUE(arr->elements_.at(4).is_undefined());
    EXPECT_TRUE(arr->elements_.at(5).is_bool() && arr->elements_.at(5).as_bool());
}

// ============================================================
// GEN-02: 调用 generator 函数返回对象，不立即执行函数体
// ============================================================

TEST(Generator, GEN02_Interp_CallReturnsObject) {
    auto v = interp_ok(R"(
        var sideEffect = 0;
        function* gen() { sideEffect = 1; yield 1; }
        var g = gen();
        sideEffect
    )");
    EXPECT_DOUBLE_EQ(v.as_number(), 0.0);
}

TEST(Generator, GEN02_VM_CallReturnsObject) {
    auto v = vm_ok(R"(
        var sideEffect = 0;
        function* gen() { sideEffect = 1; yield 1; }
        var g = gen();
        sideEffect
    )");
    EXPECT_DOUBLE_EQ(v.as_number(), 0.0);
}

// ============================================================
// GEN-03: g.next() 序列
// ============================================================

TEST(Generator, GEN03_Interp_NextSequence) {
    auto v = interp_ok(R"(
        function* gen() { yield 1; yield 2; }
        var g = gen();
        var a = g.next().value === 1;
        var b = g.next().value === 2;
        var c = g.next().done === true;
        [a, b, c]
    )");
    ASSERT_TRUE(v.is_object());
    auto* arr = static_cast<JSObject*>(v.as_object_raw());
    EXPECT_TRUE(arr->elements_.at(0).is_bool() && arr->elements_.at(0).as_bool());
    EXPECT_TRUE(arr->elements_.at(1).is_bool() && arr->elements_.at(1).as_bool());
    EXPECT_TRUE(arr->elements_.at(2).is_bool() && arr->elements_.at(2).as_bool());
}

TEST(Generator, GEN03_VM_NextSequence) {
    auto v = vm_ok(R"(
        function* gen() { yield 1; yield 2; }
        var g = gen();
        var a = g.next().value === 1;
        var b = g.next().value === 2;
        var c = g.next().done === true;
        [a, b, c]
    )");
    ASSERT_TRUE(v.is_object());
    auto* arr = static_cast<JSObject*>(v.as_object_raw());
    EXPECT_TRUE(arr->elements_.at(0).is_bool() && arr->elements_.at(0).as_bool());
    EXPECT_TRUE(arr->elements_.at(1).is_bool() && arr->elements_.at(1).as_bool());
    EXPECT_TRUE(arr->elements_.at(2).is_bool() && arr->elements_.at(2).as_bool());
}

// ============================================================
// GEN-04: 首次 g.next(99) 的参数被丢弃
// ============================================================

TEST(Generator, GEN04_Interp_FirstArgDiscarded) {
    auto v = interp_ok(R"(
        function* gen() { yield 42; }
        var g = gen();
        g.next(99).value
    )");
    EXPECT_DOUBLE_EQ(v.as_number(), 42.0);
}

TEST(Generator, GEN04_VM_FirstArgDiscarded) {
    auto v = vm_ok(R"(
        function* gen() { yield 42; }
        var g = gen();
        g.next(99).value
    )");
    EXPECT_DOUBLE_EQ(v.as_number(), 42.0);
}

// ============================================================
// GEN-05: yield 表达式的返回值 = g.next(val) 的参数（VM only for full semantics）
// ============================================================

// Interpreter: 顺序 yield 语句，yield 表达式值（简化测试）
TEST(Generator, GEN05_Interp_YieldExprValue) {
    // Interpreter 路径：yield 表达式返回 resume 值（用 var 避免 let TDZ 冲突）
    auto v = interp_ok(R"(
        function* gen() {
            var x = yield 1;
            yield x + 10;
        }
        var g = gen();
        g.next();         // yield 1
        g.next(5).value   // yield 5 + 10 = 15
    )");
    EXPECT_DOUBLE_EQ(v.as_number(), 15.0);
}

TEST(Generator, GEN05_VM_YieldExprValue) {
    auto v = vm_ok(R"(
        function* gen() {
            var x = yield 1;
            yield x + 10;
        }
        var g = gen();
        g.next();         // yield 1
        g.next(5).value   // yield 5 + 10 = 15
    )");
    EXPECT_DOUBLE_EQ(v.as_number(), 15.0);
}

// ============================================================
// GEN-06: g[Symbol.iterator]() === g
// ============================================================

TEST(Generator, GEN06_Interp_SymbolIteratorSelf) {
    auto v = interp_ok(R"(
        function* gen() { yield 1; }
        var g = gen();
        g[Symbol.iterator]() === g
    )");
    EXPECT_TRUE(v.is_bool() && v.as_bool());
}

TEST(Generator, GEN06_VM_SymbolIteratorSelf) {
    auto v = vm_ok(R"(
        function* gen() { yield 1; }
        var g = gen();
        g[Symbol.iterator]() === g
    )");
    EXPECT_TRUE(v.is_bool() && v.as_bool());
}

// ============================================================
// GEN-07: for...of 消费 generator
// ============================================================

TEST(Generator, GEN07_Interp_ForOf) {
    auto v = interp_ok(R"(
        function* gen() { yield 1; yield 2; yield 3; }
        var sum = 0;
        for (var x of gen()) { sum += x; }
        sum
    )");
    EXPECT_DOUBLE_EQ(v.as_number(), 6.0);
}

TEST(Generator, GEN07_VM_ForOf) {
    auto v = vm_ok(R"(
        function* gen() { yield 1; yield 2; yield 3; }
        var sum = 0;
        for (var x of gen()) { sum += x; }
        sum
    )");
    EXPECT_DOUBLE_EQ(v.as_number(), 6.0);
}

// ============================================================
// GEN-08: [...gen()] 展开 generator
// ============================================================

TEST(Generator, GEN08_Interp_Spread) {
    auto v = interp_ok(R"(
        function* gen() { yield 10; yield 20; yield 30; }
        var arr = [...gen()];
        arr[0] + arr[1] + arr[2]
    )");
    EXPECT_DOUBLE_EQ(v.as_number(), 60.0);
}

TEST(Generator, GEN08_VM_Spread) {
    auto v = vm_ok(R"(
        function* gen() { yield 10; yield 20; yield 30; }
        var arr = [...gen()];
        arr[0] + arr[1] + arr[2]
    )");
    EXPECT_DOUBLE_EQ(v.as_number(), 60.0);
}

// ============================================================
// GEN-09: g.next() after done 返回 {value: undefined, done: true}
// ============================================================

TEST(Generator, GEN09_Interp_NextAfterDone) {
    auto v = interp_ok(R"(
        function* gen() { yield 1; }
        var g = gen();
        g.next(); // yield 1
        g.next(); // done
        var r = g.next(); // after done
        [r.value === undefined, r.done === true]
    )");
    ASSERT_TRUE(v.is_object());
    auto* arr = static_cast<JSObject*>(v.as_object_raw());
    EXPECT_TRUE(arr->elements_.at(0).is_bool() && arr->elements_.at(0).as_bool());
    EXPECT_TRUE(arr->elements_.at(1).is_bool() && arr->elements_.at(1).as_bool());
}

TEST(Generator, GEN09_VM_NextAfterDone) {
    auto v = vm_ok(R"(
        function* gen() { yield 1; }
        var g = gen();
        g.next(); // yield 1
        g.next(); // done
        var r = g.next(); // after done
        [r.value === undefined, r.done === true]
    )");
    ASSERT_TRUE(v.is_object());
    auto* arr = static_cast<JSObject*>(v.as_object_raw());
    EXPECT_TRUE(arr->elements_.at(0).is_bool() && arr->elements_.at(0).as_bool());
    EXPECT_TRUE(arr->elements_.at(1).is_bool() && arr->elements_.at(1).as_bool());
}

// ============================================================
// GEN-10: g.return(42) in suspendedStart → {value: 42, done: true}
// ============================================================

TEST(Generator, GEN10_Interp_ReturnInSuspendedStart) {
    auto v = interp_ok(R"(
        function* gen() { yield 1; yield 2; }
        var g = gen();
        var r = g.return(42);
        [r.value, r.done]
    )");
    ASSERT_TRUE(v.is_object());
    auto* arr = static_cast<JSObject*>(v.as_object_raw());
    EXPECT_DOUBLE_EQ(arr->elements_.at(0).as_number(), 42.0);
    EXPECT_TRUE(arr->elements_.at(1).is_bool() && arr->elements_.at(1).as_bool());
}

TEST(Generator, GEN10_VM_ReturnInSuspendedStart) {
    auto v = vm_ok(R"(
        function* gen() { yield 1; yield 2; }
        var g = gen();
        var r = g.return(42);
        [r.value, r.done]
    )");
    ASSERT_TRUE(v.is_object());
    auto* arr = static_cast<JSObject*>(v.as_object_raw());
    EXPECT_DOUBLE_EQ(arr->elements_.at(0).as_number(), 42.0);
    EXPECT_TRUE(arr->elements_.at(1).is_bool() && arr->elements_.at(1).as_bool());
}

// ============================================================
// GEN-11: g.return(42) in suspendedYield → {value: 42, done: true}
// ============================================================

TEST(Generator, GEN11_Interp_ReturnInSuspendedYield) {
    auto v = interp_ok(R"(
        function* gen() { yield 1; yield 2; }
        var g = gen();
        g.next();
        var r = g.return(42);
        [r.value, r.done]
    )");
    ASSERT_TRUE(v.is_object());
    auto* arr = static_cast<JSObject*>(v.as_object_raw());
    EXPECT_DOUBLE_EQ(arr->elements_.at(0).as_number(), 42.0);
    EXPECT_TRUE(arr->elements_.at(1).is_bool() && arr->elements_.at(1).as_bool());
}

TEST(Generator, GEN11_VM_ReturnInSuspendedYield) {
    auto v = vm_ok(R"(
        function* gen() { yield 1; yield 2; }
        var g = gen();
        g.next();
        var r = g.return(42);
        [r.value, r.done]
    )");
    ASSERT_TRUE(v.is_object());
    auto* arr = static_cast<JSObject*>(v.as_object_raw());
    EXPECT_DOUBLE_EQ(arr->elements_.at(0).as_number(), 42.0);
    EXPECT_TRUE(arr->elements_.at(1).is_bool() && arr->elements_.at(1).as_bool());
}

// ============================================================
// GEN-12: g.throw(err) in suspendedStart → 抛出 err
// ============================================================

TEST(Generator, GEN12_Interp_ThrowInSuspendedStart) {
    EXPECT_TRUE(interp_throws(R"(
        function* gen() { yield 1; }
        var g = gen();
        g.throw(new Error("oops"));
    )"));
}

TEST(Generator, GEN12_VM_ThrowInSuspendedStart) {
    EXPECT_TRUE(vm_throws(R"(
        function* gen() { yield 1; }
        var g = gen();
        g.throw(new Error("oops"));
    )"));
}

// ============================================================
// GEN-13: g.throw(err) in suspendedYield，有 try/catch → catch 捕获，继续 yield
// ============================================================

TEST(Generator, GEN13_Interp_ThrowCaught) {
    auto v = interp_ok(R"(
        function* gen() {
            var caught = "none";
            try { yield 1; } catch(e) { caught = e.message; }
            yield caught;
        }
        var g = gen();
        g.next();
        var r = g.throw(new TypeError("boom"));
        r.value
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "boom");
}

TEST(Generator, GEN13_VM_ThrowCaught) {
    auto v = vm_ok(R"(
        function* gen() {
            var caught = "none";
            try { yield 1; } catch(e) { caught = e.message; }
            yield caught;
        }
        var g = gen();
        g.next();
        var r = g.throw(new TypeError("boom"));
        r.value
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "boom");
}

// ============================================================
// GEN-14: new gen() → TypeError
// ============================================================

TEST(Generator, GEN14_Interp_NewThrows) {
    EXPECT_TRUE(interp_throws(R"(
        function* gen() { yield 1; }
        new gen();
    )"));
}

TEST(Generator, GEN14_VM_NewThrows) {
    EXPECT_TRUE(vm_throws(R"(
        function* gen() { yield 1; }
        new gen();
    )"));
}

// ============================================================
// GEN-15: generator 函数 .name 和 .length
// ============================================================

TEST(Generator, GEN15_Interp_NameAndLength) {
    auto v = interp_ok(R"(
        function* gen(a, b) { yield 1; }
        [gen.name, gen.length]
    )");
    ASSERT_TRUE(v.is_object());
    auto* arr = static_cast<JSObject*>(v.as_object_raw());
    EXPECT_EQ(arr->elements_.at(0).sv(), "gen");
    EXPECT_DOUBLE_EQ(arr->elements_.at(1).as_number(), 2.0);
}

TEST(Generator, GEN15_VM_NameAndLength) {
    auto v = vm_ok(R"(
        function* gen(a, b) { yield 1; }
        [gen.name, gen.length]
    )");
    ASSERT_TRUE(v.is_object());
    auto* arr = static_cast<JSObject*>(v.as_object_raw());
    EXPECT_EQ(arr->elements_.at(0).sv(), "gen");
    EXPECT_DOUBLE_EQ(arr->elements_.at(1).as_number(), 2.0);
}

// ============================================================
// GEN-16: 对象方法简写 *m() {} 可用
// ============================================================

TEST(Generator, GEN16_Interp_GeneratorMethod) {
    auto v = interp_ok(R"(
        var obj = {
            *values() { yield 1; yield 2; }
        };
        var result = [];
        for (var x of obj.values()) { result.push(x); }
        result[0] + result[1]
    )");
    EXPECT_DOUBLE_EQ(v.as_number(), 3.0);
}

TEST(Generator, GEN16_VM_GeneratorMethod) {
    auto v = vm_ok(R"(
        var obj = {
            *values() { yield 1; yield 2; }
        };
        var result = [];
        for (var x of obj.values()) { result.push(x); }
        result[0] + result[1]
    )");
    EXPECT_DOUBLE_EQ(v.as_number(), 3.0);
}

// ============================================================
// GEN-17: yield* 基础委托（yield* [1,2,3]）
// ============================================================

TEST(Generator, GEN17_Interp_YieldDelegate) {
    auto v = interp_ok(R"(
        function* gen() { yield* [1, 2, 3]; }
        var arr = [...gen()];
        arr[0] + arr[1] + arr[2]
    )");
    EXPECT_DOUBLE_EQ(v.as_number(), 6.0);
}

TEST(Generator, GEN17_VM_YieldDelegate) {
    auto v = vm_ok(R"(
        function* gen() { yield* [1, 2, 3]; }
        var arr = [...gen()];
        arr[0] + arr[1] + arr[2]
    )");
    EXPECT_DOUBLE_EQ(v.as_number(), 6.0);
}

// ============================================================
// GEN-18: generator 体内 throw 未捕获，传播到 .next() 调用方
// ============================================================

TEST(Generator, GEN18_Interp_UncaughtThrowPropagates) {
    EXPECT_TRUE(interp_throws(R"(
        function* gen() { throw new Error("oops"); yield 1; }
        var g = gen();
        g.next();
    )"));
}

TEST(Generator, GEN18_VM_UncaughtThrowPropagates) {
    EXPECT_TRUE(vm_throws(R"(
        function* gen() { throw new Error("oops"); yield 1; }
        var g = gen();
        g.next();
    )"));
}

// ============================================================
// GEN-Extra-01: generator 函数表达式
// ============================================================

TEST(Generator, GENExtra01_Interp_FunctionExpression) {
    auto v = interp_ok(R"(
        var gen = function*() { yield 10; yield 20; };
        var arr = [...gen()];
        arr[0] + arr[1]
    )");
    EXPECT_DOUBLE_EQ(v.as_number(), 30.0);
}

TEST(Generator, GENExtra01_VM_FunctionExpression) {
    auto v = vm_ok(R"(
        var gen = function*() { yield 10; yield 20; };
        var arr = [...gen()];
        arr[0] + arr[1]
    )");
    EXPECT_DOUBLE_EQ(v.as_number(), 30.0);
}

// ============================================================
// GEN-Extra-02: yield 无参数 → yields undefined
// ============================================================

TEST(Generator, GENExtra02_Interp_YieldNoArg) {
    auto v = interp_ok(R"(
        function* gen() { yield; }
        var r = gen().next();
        [r.value === undefined, r.done === false]
    )");
    ASSERT_TRUE(v.is_object());
    auto* arr = static_cast<JSObject*>(v.as_object_raw());
    EXPECT_TRUE(arr->elements_.at(0).as_bool());
    EXPECT_TRUE(arr->elements_.at(1).as_bool());
}

TEST(Generator, GENExtra02_VM_YieldNoArg) {
    auto v = vm_ok(R"(
        function* gen() { yield; }
        var r = gen().next();
        [r.value === undefined, r.done === false]
    )");
    ASSERT_TRUE(v.is_object());
    auto* arr = static_cast<JSObject*>(v.as_object_raw());
    EXPECT_TRUE(arr->elements_.at(0).as_bool());
    EXPECT_TRUE(arr->elements_.at(1).as_bool());
}

// ============================================================
// GEN-Extra-03: return value 被 done 结果携带
// ============================================================

TEST(Generator, GENExtra03_Interp_ReturnValue) {
    auto v = interp_ok(R"(
        function* gen() { yield 1; return 99; }
        var g = gen();
        g.next();
        g.next().value
    )");
    EXPECT_DOUBLE_EQ(v.as_number(), 99.0);
}

TEST(Generator, GENExtra03_VM_ReturnValue) {
    auto v = vm_ok(R"(
        function* gen() { yield 1; return 99; }
        var g = gen();
        g.next();
        g.next().value
    )");
    EXPECT_DOUBLE_EQ(v.as_number(), 99.0);
}

// ============================================================
// GEN-Extra-04: generator 作为参数传递给 Array.from
// ============================================================

TEST(Generator, GENExtra04_VM_ArrayFrom) {
    auto v = vm_ok(R"(
        function* gen() { yield 'a'; yield 'b'; yield 'c'; }
        var arr = Array.from(gen());
        arr.join('')
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "abc");
}

}  // namespace
