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
// AG-01: 基础 async generator，调用返回一个对象（AsyncGenerator）
// ============================================================

TEST(AsyncGenerator, AG01_ReturnObjectInterp) {
    auto v = interp_ok(R"(
        async function* gen() { yield 1; }
        let g = gen();
        typeof g
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "object");
}

TEST(AsyncGenerator, AG01_ReturnObjectVM) {
    auto v = vm_ok(R"(
        async function* gen() { yield 1; }
        let g = gen();
        typeof g
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "object");
}

// ============================================================
// AG-02: .next() 返回 Promise (typeof === "object")
// ============================================================

TEST(AsyncGenerator, AG02_NextReturnsPromiseInterp) {
    auto v = interp_ok(R"(
        async function* gen() { yield 1; }
        let g = gen();
        let p = g.next();
        typeof p
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "object");
}

TEST(AsyncGenerator, AG02_NextReturnsPromiseVM) {
    auto v = vm_ok(R"(
        async function* gen() { yield 1; }
        let g = gen();
        let p = g.next();
        typeof p
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "object");
}

// ============================================================
// AG-03: .next() resolve 到 {value: 42}（用简单标识符 re-read 机制）
// ============================================================

TEST(AsyncGenerator, AG03_NextResolvesValueInterp) {
    auto v = interp_ok(R"(
        async function* gen() { yield 42; }
        let g = gen();
        let result_val;
        g.next().then(function(r) { result_val = r.value; });
        result_val
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 42.0);
}

TEST(AsyncGenerator, AG03_NextResolvesValueVM) {
    auto v = vm_ok(R"(
        async function* gen() { yield 42; }
        let g = gen();
        let result_val;
        g.next().then(function(r) { result_val = r.value; });
        result_val
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 42.0);
}

// ============================================================
// AG-04: done:false for first yield (capture to simple var)
// ============================================================

TEST(AsyncGenerator, AG04_DoneFalseInterp) {
    auto v = interp_ok(R"(
        async function* gen() { yield 1; }
        let g = gen();
        let result_done;
        g.next().then(function(r) { result_done = r.done; });
        result_done
    )");
    // done is a boolean false
    EXPECT_TRUE(v.is_bool());
    EXPECT_FALSE(v.as_bool());
}

TEST(AsyncGenerator, AG04_DoneFalseVM) {
    auto v = vm_ok(R"(
        async function* gen() { yield 1; }
        let g = gen();
        let result_done;
        g.next().then(function(r) { result_done = r.done; });
        result_done
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_FALSE(v.as_bool());
}

// ============================================================
// AG-05: 函数结束后 done:true
// ============================================================

TEST(AsyncGenerator, AG05_DoneTrueWhenFinishedInterp) {
    auto v = interp_ok(R"(
        async function* gen() { yield 1; }
        let g = gen();
        let done2;
        g.next().then(function(r) {});
        g.next().then(function(r) { done2 = r.done; });
        done2
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(AsyncGenerator, AG05_DoneTrueWhenFinishedVM) {
    auto v = vm_ok(R"(
        async function* gen() { yield 1; }
        let g = gen();
        let done2;
        g.next().then(function(r) {});
        g.next().then(function(r) { done2 = r.done; });
        done2
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// AG-06: 多次 yield 依次消费
// ============================================================

TEST(AsyncGenerator, AG06_MultipleYieldsInterp) {
    auto v = interp_ok(R"(
        async function* gen() { yield 1; yield 2; yield 3; }
        let g = gen();
        let sum = 0;
        g.next().then(function(r) { sum = sum + r.value; });
        g.next().then(function(r) { sum = sum + r.value; });
        g.next().then(function(r) { sum = sum + r.value; });
        sum
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 6.0);
}

TEST(AsyncGenerator, AG06_MultipleYieldsVM) {
    auto v = vm_ok(R"(
        async function* gen() { yield 1; yield 2; yield 3; }
        let g = gen();
        let sum = 0;
        g.next().then(function(r) { sum = sum + r.value; });
        g.next().then(function(r) { sum = sum + r.value; });
        g.next().then(function(r) { sum = sum + r.value; });
        sum
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 6.0);
}

// ============================================================
// AG-07: async generator 中的 await
// ============================================================

TEST(AsyncGenerator, AG07_AwaitInsideInterp) {
    auto v = interp_ok(R"(
        async function* gen() {
            let x = await Promise.resolve(10);
            yield x;
        }
        let g = gen();
        let resolved;
        g.next().then(function(r) { resolved = r.value; });
        resolved
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 10.0);
}

TEST(AsyncGenerator, AG07_AwaitInsideVM) {
    auto v = vm_ok(R"(
        async function* gen() {
            let x = await Promise.resolve(10);
            yield x;
        }
        let g = gen();
        let resolved;
        g.next().then(function(r) { resolved = r.value; });
        resolved
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 10.0);
}

// ============================================================
// AG-08: return 提前结束，done:true, value returned
// ============================================================

TEST(AsyncGenerator, AG08_ReturnValueInterp) {
    auto v = interp_ok(R"(
        async function* gen() { return 99; }
        let g = gen();
        let ret_done;
        g.next().then(function(r) { ret_done = r.done; });
        ret_done
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(AsyncGenerator, AG08_ReturnValueVM) {
    auto v = vm_ok(R"(
        async function* gen() { return 99; }
        let g = gen();
        let ret_done;
        g.next().then(function(r) { ret_done = r.done; });
        ret_done
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

}  // namespace
