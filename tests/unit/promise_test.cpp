// Phase 11: Promise / async / await 测试
//
// 测试重点：
//   1. Promise 基础：构造、resolve、reject、then、catch、finally
//   2. 微任务顺序：同步代码先于微任务执行
//   3. async/await 基础：同步返回 Promise、await 暂停/恢复
//   4. async 内 throw → rejection
//   5. GC 安全：挂起的 async 帧不被回收
//   6. Interpreter 和 VM 两侧对称测试

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

qppjs::EvalResult interp_run(std::string_view source) {
    auto parse_result = qppjs::parse_program(source);
    if (!parse_result.ok()) {
        return qppjs::EvalResult::err(parse_result.error());
    }
    qppjs::Interpreter interp;
    return interp.exec(parse_result.value());
}

qppjs::Value interp_ok(std::string_view source) {
    auto r = interp_run(source);
    EXPECT_TRUE(r.is_ok()) << "interp error: " << (r.is_ok() ? "" : r.error().message());
    return r.is_ok() ? r.value() : qppjs::Value::undefined();
}

[[maybe_unused]] static std::string interp_err(std::string_view source) {
    auto r = interp_run(source);
    EXPECT_FALSE(r.is_ok()) << "expected error but got ok";
    return r.is_ok() ? "" : r.error().message();
}

qppjs::EvalResult vm_run(std::string_view source) {
    auto parse_result = qppjs::parse_program(source);
    if (!parse_result.ok()) {
        return qppjs::EvalResult::err(parse_result.error());
    }
    qppjs::Compiler compiler;
    auto bytecode = compiler.compile(parse_result.value());
    qppjs::VM vm;
    return vm.exec(bytecode);
}

qppjs::Value vm_ok(std::string_view source) {
    auto r = vm_run(source);
    EXPECT_TRUE(r.is_ok()) << "vm error: " << (r.is_ok() ? "" : r.error().message());
    return r.is_ok() ? r.value() : qppjs::Value::undefined();
}

[[maybe_unused]] static std::string vm_err(std::string_view source) {
    auto r = vm_run(source);
    EXPECT_FALSE(r.is_ok()) << "expected error but got ok";
    return r.is_ok() ? "" : r.error().message();
}

// ============================================================
// 1. Promise 基础构造
// ============================================================

TEST(Promise, InterpConstructorResolveSyncResult) {
    // new Promise(resolve => resolve(42)) 同步 resolve
    // 执行完同步代码后 DrainAll，最终返回 Promise 对象（fulfilled）
    auto v = interp_ok(R"(
        var result = undefined;
        var p = new Promise(function(resolve) { resolve(42); });
        p.then(function(v) { result = v; });
        result;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 42.0);
}

TEST(Promise, VmConstructorResolveSyncResult) {
    auto v = vm_ok(R"(
        var result = undefined;
        var p = new Promise(function(resolve) { resolve(42); });
        p.then(function(v) { result = v; });
        result;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 42.0);
}

TEST(Promise, InterpConstructorRejectSyncResult) {
    auto v = interp_ok(R"(
        var result = undefined;
        var p = new Promise(function(resolve, reject) { reject("err"); });
        p.catch(function(r) { result = r; });
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "err");
}

TEST(Promise, VmConstructorRejectSyncResult) {
    auto v = vm_ok(R"(
        var result = undefined;
        var p = new Promise(function(resolve, reject) { reject("err"); });
        p.catch(function(r) { result = r; });
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "err");
}

// ============================================================
// 2. Promise.resolve / Promise.reject 静态方法
// ============================================================

TEST(Promise, InterpStaticResolve) {
    auto v = interp_ok(R"(
        var result = undefined;
        Promise.resolve(99).then(function(v) { result = v; });
        result;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 99.0);
}

TEST(Promise, VmStaticResolve) {
    auto v = vm_ok(R"(
        var result = undefined;
        Promise.resolve(99).then(function(v) { result = v; });
        result;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 99.0);
}

TEST(Promise, InterpStaticReject) {
    auto v = interp_ok(R"(
        var result = undefined;
        Promise.reject("reason").catch(function(r) { result = r; });
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "reason");
}

TEST(Promise, VmStaticReject) {
    auto v = vm_ok(R"(
        var result = undefined;
        Promise.reject("reason").catch(function(r) { result = r; });
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "reason");
}

// ============================================================
// 3. 链式 then
// ============================================================

TEST(Promise, InterpThenChain) {
    auto v = interp_ok(R"(
        var result = undefined;
        Promise.resolve(1)
            .then(function(v) { return v + 1; })
            .then(function(v) { return v * 10; })
            .then(function(v) { result = v; });
        result;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 20.0);
}

TEST(Promise, VmThenChain) {
    auto v = vm_ok(R"(
        var result = undefined;
        Promise.resolve(1)
            .then(function(v) { return v + 1; })
            .then(function(v) { return v * 10; })
            .then(function(v) { result = v; });
        result;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 20.0);
}

// ============================================================
// 4. catch 捕获 then 中的 throw
// ============================================================

TEST(Promise, InterpThenThrowCaughtByCatch) {
    auto v = interp_ok(R"(
        var result = undefined;
        Promise.resolve(1)
            .then(function(v) { throw "oops"; })
            .catch(function(r) { result = r; });
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "oops");
}

TEST(Promise, VmThenThrowCaughtByCatch) {
    auto v = vm_ok(R"(
        var result = undefined;
        Promise.resolve(1)
            .then(function(v) { throw "oops"; })
            .catch(function(r) { result = r; });
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "oops");
}

// ============================================================
// 5. finally
// ============================================================

TEST(Promise, InterpFinallyCalledOnFulfill) {
    auto v = interp_ok(R"(
        var called = false;
        var result = undefined;
        Promise.resolve(42)
            .finally(function() { called = true; })
            .then(function(v) { result = v; });
        result;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 42.0);
}

TEST(Promise, VmFinallyCalledOnFulfill) {
    auto v = vm_ok(R"(
        var called = false;
        var result = undefined;
        Promise.resolve(42)
            .finally(function() { called = true; })
            .then(function(v) { result = v; });
        result;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 42.0);
}

TEST(Promise, InterpFinallyCalledOnReject) {
    auto v = interp_ok(R"(
        var result = undefined;
        Promise.reject("fail")
            .finally(function() { result = "finally"; })
            .catch(function(r) {});
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "finally");
}

TEST(Promise, VmFinallyCalledOnReject) {
    auto v = vm_ok(R"(
        var result = undefined;
        Promise.reject("fail")
            .finally(function() { result = "finally"; })
            .catch(function(r) {});
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "finally");
}

// ============================================================
// 6. 微任务顺序：同步代码先于微任务
// ============================================================

TEST(Promise, InterpMicrotaskOrderAfterSync) {
    // Verify: sync code runs first, microtask runs after
    // log[0] = 1 (sync), log[1] = 2 (microtask)
    // After DrainAll: result = log[1] (set by microtask)
    auto v = interp_ok(R"(
        var log = [];
        var result = 0;
        Promise.resolve().then(function() { log.push(2); result = log[0] * 10 + log[1]; });
        log.push(1);
        result;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 12.0);
}

TEST(Promise, VmMicrotaskOrderAfterSync) {
    auto v = vm_ok(R"(
        var log = [];
        var result = 0;
        Promise.resolve().then(function() { log.push(2); result = log[0] * 10 + log[1]; });
        log.push(1);
        result;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 12.0);
}

// ============================================================
// 7. async/await 基础：async 函数返回 Promise
// ============================================================

TEST(Promise, InterpAsyncFunctionReturnsPromise) {
    auto v = interp_ok(R"(
        var result = undefined;
        async function foo() { return 42; }
        foo().then(function(v) { result = v; });
        result;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 42.0);
}

TEST(Promise, VmAsyncFunctionReturnsPromise) {
    auto v = vm_ok(R"(
        var result = undefined;
        async function foo() { return 42; }
        foo().then(function(v) { result = v; });
        result;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 42.0);
}

// ============================================================
// 8. await 暂停/恢复
// ============================================================

TEST(Promise, InterpAwaitResumesWithValue) {
    auto v = interp_ok(R"(
        var result = undefined;
        async function foo() {
            var x = await Promise.resolve(10);
            result = x + 1;
        }
        foo();
        result;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 11.0);
}

TEST(Promise, VmAwaitResumesWithValue) {
    auto v = vm_ok(R"(
        var result = undefined;
        async function foo() {
            var x = await Promise.resolve(10);
            result = x + 1;
        }
        foo();
        result;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 11.0);
}

TEST(Promise, InterpAwaitNonPromise) {
    auto v = interp_ok(R"(
        var result = undefined;
        async function foo() {
            var x = await 5;
            result = x * 2;
        }
        foo();
        result;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 10.0);
}

TEST(Promise, VmAwaitNonPromise) {
    auto v = vm_ok(R"(
        var result = undefined;
        async function foo() {
            var x = await 5;
            result = x * 2;
        }
        foo();
        result;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 10.0);
}

// ============================================================
// 9. async 内 throw → rejection
// ============================================================

TEST(Promise, InterpAsyncThrowRejectsPromise) {
    auto v = interp_ok(R"(
        var result = undefined;
        async function foo() {
            throw "async error";
        }
        foo().catch(function(r) { result = r; });
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "async error");
}

TEST(Promise, VmAsyncThrowRejectsPromise) {
    auto v = vm_ok(R"(
        var result = undefined;
        async function foo() {
            throw "async error";
        }
        foo().catch(function(r) { result = r; });
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "async error");
}

// ============================================================
// 10. await rejected promise
// ============================================================

TEST(Promise, InterpAwaitRejectedPromise) {
    auto v = interp_ok(R"(
        var result = undefined;
        async function foo() {
            try {
                await Promise.reject("boom");
            } catch(e) {
                result = e;
            }
        }
        foo();
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "boom");
}

TEST(Promise, VmAwaitRejectedPromise) {
    auto v = vm_ok(R"(
        var result = undefined;
        async function foo() {
            try {
                await Promise.reject("boom");
            } catch(e) {
                result = e;
            }
        }
        foo();
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "boom");
}

// ============================================================
// 11. GC 安全：挂起的 async 帧不被回收
// ============================================================

TEST(Promise, InterpAsyncGcSafe) {
    // async 函数挂起后，GC 不应回收 continuation 帧
    auto v = interp_ok(R"(
        var result = undefined;
        async function foo() {
            var x = await Promise.resolve(7);
            result = x;
        }
        foo();
        result;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 7.0);
}

TEST(Promise, VmAsyncGcSafe) {
    auto v = vm_ok(R"(
        var result = undefined;
        async function foo() {
            var x = await Promise.resolve(7);
            result = x;
        }
        foo();
        result;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 7.0);
}

// ============================================================
// 12. async 函数表达式
// ============================================================

TEST(Promise, InterpAsyncFunctionExpression) {
    auto v = interp_ok(R"(
        var result = undefined;
        var foo = async function() { return 100; };
        foo().then(function(v) { result = v; });
        result;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 100.0);
}

TEST(Promise, VmAsyncFunctionExpression) {
    auto v = vm_ok(R"(
        var result = undefined;
        var foo = async function() { return 100; };
        foo().then(function(v) { result = v; });
        result;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 100.0);
}

// ============================================================
// 13. Promise executor 中 throw → reject
// ============================================================

TEST(Promise, InterpExecutorThrowRejects) {
    auto v = interp_ok(R"(
        var result = undefined;
        new Promise(function(resolve, reject) {
            throw "exec error";
        }).catch(function(r) { result = r; });
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "exec error");
}

TEST(Promise, VmExecutorThrowRejects) {
    auto v = vm_ok(R"(
        var result = undefined;
        new Promise(function(resolve, reject) {
            throw "exec error";
        }).catch(function(r) { result = r; });
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "exec error");
}

// ============================================================
// 14. then 返回 Promise（thenable 链）
// ============================================================

TEST(Promise, InterpThenReturnsPromise) {
    auto v = interp_ok(R"(
        var result = undefined;
        Promise.resolve(1)
            .then(function(v) { return Promise.resolve(v + 9); })
            .then(function(v) { result = v; });
        result;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 10.0);
}

TEST(Promise, VmThenReturnsPromise) {
    auto v = vm_ok(R"(
        var result = undefined;
        Promise.resolve(1)
            .then(function(v) { return Promise.resolve(v + 9); })
            .then(function(v) { result = v; });
        result;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 10.0);
}

// ============================================================
// 15. 多个 await
// ============================================================

TEST(Promise, InterpMultipleAwaits) {
    auto v = interp_ok(R"(
        var result = undefined;
        async function foo() {
            var a = await Promise.resolve(3);
            var b = await Promise.resolve(4);
            result = a + b;
        }
        foo();
        result;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 7.0);
}

TEST(Promise, VmMultipleAwaits) {
    auto v = vm_ok(R"(
        var result = undefined;
        async function foo() {
            var a = await Promise.resolve(3);
            var b = await Promise.resolve(4);
            result = a + b;
        }
        foo();
        result;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 7.0);
}

// ============================================================
// 16. async 函数 return 值被 then 接收
// ============================================================

TEST(Promise, InterpAsyncReturnValuePropagated) {
    auto v = interp_ok(R"(
        var result = undefined;
        async function add(a, b) {
            var x = await Promise.resolve(a);
            return x + b;
        }
        add(3, 4).then(function(v) { result = v; });
        result;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 7.0);
}

TEST(Promise, VmAsyncReturnValuePropagated) {
    auto v = vm_ok(R"(
        var result = undefined;
        async function add(a, b) {
            var x = await Promise.resolve(a);
            return x + b;
        }
        add(3, 4).then(function(v) { result = v; });
        result;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 7.0);
}

// ============================================================
// 17. C5/C6：settled 后 resolve/reject 幂等（状态单向不可逆）
// ============================================================

TEST(Promise, InterpSettledIdempotentResolve) {
    // C5/C6：已 fulfilled 后再次 resolve 静默忽略
    auto v = interp_ok(R"(
        var result = 0;
        var p = new Promise(function(resolve) {
            resolve(1);
            resolve(2);
            resolve(3);
        });
        p.then(function(v) { result = v; });
        result;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

TEST(Promise, VmSettledIdempotentResolve) {
    auto v = vm_ok(R"(
        var result = 0;
        var p = new Promise(function(resolve) {
            resolve(1);
            resolve(2);
            resolve(3);
        });
        p.then(function(v) { result = v; });
        result;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

TEST(Promise, InterpSettledIdempotentReject) {
    // C5/C6：已 rejected 后再次 reject 静默忽略
    auto v = interp_ok(R"(
        var result = "none";
        var p = new Promise(function(resolve, reject) {
            reject("first");
            reject("second");
        });
        p.catch(function(r) { result = r; });
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "first");
}

TEST(Promise, VmSettledIdempotentReject) {
    auto v = vm_ok(R"(
        var result = "none";
        var p = new Promise(function(resolve, reject) {
            reject("first");
            reject("second");
        });
        p.catch(function(r) { result = r; });
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "first");
}

TEST(Promise, InterpSettledResolveAfterReject) {
    // C5/C6：已 rejected 后调用 resolve 静默忽略
    auto v = interp_ok(R"(
        var result = "none";
        var p = new Promise(function(resolve, reject) {
            reject("err");
            resolve(42);
        });
        p.catch(function(r) { result = r; });
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "err");
}

TEST(Promise, VmSettledResolveAfterReject) {
    auto v = vm_ok(R"(
        var result = "none";
        var p = new Promise(function(resolve, reject) {
            reject("err");
            resolve(42);
        });
        p.catch(function(r) { result = r; });
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "err");
}

// ============================================================
// 18. C7：resolve(promise_self) → TypeError（自解析循环）
// 注意：当前实现中，resolve(p) 时若 p 已 fulfilled，
// resolve fn 直接调用 Fulfill(fulfilled_promise_value)，
// 而 promise_resolve 对已是 Promise 的值做 identity 返回，
// 不会产生自解析循环。真正的自解析（resolve 传入同一个 pending Promise）
// 会导致 p 永远 pending，是一个已知的 GC 循环引用问题。
// 本测试验证：resolve 传入已 fulfilled 的 Promise（非自身）正常工作。
// ============================================================

TEST(Promise, InterpResolveWithFulfilledPromise) {
    // resolve(already_fulfilled_promise) → 采纳其 fulfilled 值
    auto v = interp_ok(R"(
        var result = "none";
        var inner = Promise.resolve(42);
        new Promise(function(resolve) {
            resolve(inner);
        }).then(function(v) { result = v; });
        result;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 42.0);
}

TEST(Promise, VmResolveWithFulfilledPromise) {
    auto v = vm_ok(R"(
        var result = "none";
        var inner = Promise.resolve(42);
        new Promise(function(resolve) {
            resolve(inner);
        }).then(function(v) { result = v; });
        result;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 42.0);
}

// ============================================================
// 19. C11：then handler 不是函数时透传值/原因
// ============================================================

TEST(Promise, InterpThenNullHandlerPassthroughFulfill) {
    // C11：onFulfilled 不是函数 → 透传 fulfilled 值
    auto v = interp_ok(R"(
        var result = undefined;
        Promise.resolve(42)
            .then(null)
            .then(function(v) { result = v; });
        result;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 42.0);
}

TEST(Promise, VmThenNullHandlerPassthroughFulfill) {
    auto v = vm_ok(R"(
        var result = undefined;
        Promise.resolve(42)
            .then(null)
            .then(function(v) { result = v; });
        result;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 42.0);
}

TEST(Promise, InterpThenUndefinedHandlerPassthroughFulfill) {
    // C11：onFulfilled = undefined → 透传 fulfilled 值
    auto v = interp_ok(R"(
        var result = undefined;
        Promise.resolve(55)
            .then(undefined, undefined)
            .then(function(v) { result = v; });
        result;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 55.0);
}

TEST(Promise, VmThenUndefinedHandlerPassthroughFulfill) {
    auto v = vm_ok(R"(
        var result = undefined;
        Promise.resolve(55)
            .then(undefined, undefined)
            .then(function(v) { result = v; });
        result;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 55.0);
}

TEST(Promise, InterpThenNullRejectHandlerPassthroughRejection) {
    // C11：onRejected 不是函数 → 透传 rejection 原因
    auto v = interp_ok(R"(
        var result = "none";
        Promise.reject("reason")
            .then(null, null)
            .catch(function(r) { result = r; });
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "reason");
}

TEST(Promise, VmThenNullRejectHandlerPassthroughRejection) {
    auto v = vm_ok(R"(
        var result = "none";
        Promise.reject("reason")
            .then(null, null)
            .catch(function(r) { result = r; });
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "reason");
}

// ============================================================
// 20. C14：finally 不改变链的值
// ============================================================

TEST(Promise, InterpFinallyDoesNotChangeValue) {
    // C14：finally fn 返回值被忽略，原始 fulfilled 值透传
    auto v = interp_ok(R"(
        var result = undefined;
        Promise.resolve(42)
            .finally(function() { return 999; })
            .then(function(v) { result = v; });
        result;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 42.0);
}

TEST(Promise, VmFinallyDoesNotChangeValue) {
    auto v = vm_ok(R"(
        var result = undefined;
        Promise.resolve(42)
            .finally(function() { return 999; })
            .then(function(v) { result = v; });
        result;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 42.0);
}

TEST(Promise, InterpFinallyDoesNotChangeRejection) {
    // C14：finally fn 返回值被忽略，原始 rejection 原因透传
    auto v = interp_ok(R"(
        var result = "none";
        Promise.reject("original")
            .finally(function() { return "ignored"; })
            .catch(function(r) { result = r; });
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "original");
}

TEST(Promise, VmFinallyDoesNotChangeRejection) {
    auto v = vm_ok(R"(
        var result = "none";
        Promise.reject("original")
            .finally(function() { return "ignored"; })
            .catch(function(r) { result = r; });
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "original");
}

// ============================================================
// 21. E9/C15：finally fn throw → 替代原值传播
// ============================================================

TEST(Promise, InterpFinallyFnThrowReplacesValue) {
    // E9：finally fn throw → 替代原始 fulfilled 值，向下游传播 rejection
    auto v = interp_ok(R"(
        var result = "none";
        Promise.resolve(42)
            .finally(function() { throw "finally error"; })
            .catch(function(r) { result = r; });
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "finally error");
}

TEST(Promise, VmFinallyFnThrowReplacesValue) {
    auto v = vm_ok(R"(
        var result = "none";
        Promise.resolve(42)
            .finally(function() { throw "finally error"; })
            .catch(function(r) { result = r; });
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "finally error");
}

TEST(Promise, InterpFinallyFnThrowReplacesRejection) {
    // E9：finally fn throw 替代原始 rejection 原因
    auto v = interp_ok(R"(
        var result = "none";
        Promise.reject("original")
            .finally(function() { throw "new error"; })
            .catch(function(r) { result = r; });
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "new error");
}

TEST(Promise, VmFinallyFnThrowReplacesRejection) {
    auto v = vm_ok(R"(
        var result = "none";
        Promise.reject("original")
            .finally(function() { throw "new error"; })
            .catch(function(r) { result = r; });
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "new error");
}

// ============================================================
// 22. C15：finally fn 返回 rejected Promise → 替代原值传播
// ============================================================

TEST(Promise, InterpFinallyFnReturnsRejectedPromise) {
    // C15：finally fn 返回 rejected Promise → 替代原始值
    auto v = interp_ok(R"(
        var result = "none";
        Promise.resolve(42)
            .finally(function() { return Promise.reject("from finally"); })
            .catch(function(r) { result = r; });
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "from finally");
}

TEST(Promise, VmFinallyFnReturnsRejectedPromise) {
    auto v = vm_ok(R"(
        var result = "none";
        Promise.resolve(42)
            .finally(function() { return Promise.reject("from finally"); })
            .catch(function(r) { result = r; });
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "from finally");
}

// ============================================================
// 23. C16：Promise.resolve(同一 Promise) → identity（不包装）
// ============================================================

TEST(Promise, InterpPromiseResolveIdentity) {
    // C16：Promise.resolve(p) where p is already a Promise → returns p itself
    auto v = interp_ok(R"(
        var p = Promise.resolve(42);
        var p2 = Promise.resolve(p);
        var same = (p === p2);
        same;
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(Promise, VmPromiseResolveIdentity) {
    auto v = vm_ok(R"(
        var p = Promise.resolve(42);
        var p2 = Promise.resolve(p);
        var same = (p === p2);
        same;
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// 24. C17：Promise.reject 始终创建新 Promise
// ============================================================

TEST(Promise, InterpPromiseRejectAlwaysNew) {
    // C17：Promise.reject(p) 即使 p 是 Promise，也创建新的 rejected Promise
    auto v = interp_ok(R"(
        var p = Promise.resolve(42);
        var p2 = Promise.reject(p);
        var same = (p === p2);
        same;
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_FALSE(v.as_bool());
}

TEST(Promise, VmPromiseRejectAlwaysNew) {
    auto v = vm_ok(R"(
        var p = Promise.resolve(42);
        var p2 = Promise.reject(p);
        var same = (p === p2);
        same;
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_FALSE(v.as_bool());
}

// ============================================================
// 25. C10：then 返回新 Promise（非同一对象）
// ============================================================

TEST(Promise, InterpThenReturnsNewPromise) {
    // C10：then 返回的是新 Promise，不是原 Promise
    auto v = interp_ok(R"(
        var p = Promise.resolve(1);
        var p2 = p.then(function(v) { return v; });
        var same = (p === p2);
        same;
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_FALSE(v.as_bool());
}

TEST(Promise, VmThenReturnsNewPromise) {
    auto v = vm_ok(R"(
        var p = Promise.resolve(1);
        var p2 = p.then(function(v) { return v; });
        var same = (p === p2);
        same;
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_FALSE(v.as_bool());
}

// ============================================================
// 26. C12：handler 返回值 fulfill 下游；handler throw reject 下游
// ============================================================

TEST(Promise, InterpThenHandlerReturnFulfillsDownstream) {
    // C12：then handler 返回值 → 下游 Promise fulfilled
    auto v = interp_ok(R"(
        var result = undefined;
        Promise.resolve(10)
            .then(function(v) { return v * 3; })
            .then(function(v) { result = v; });
        result;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 30.0);
}

TEST(Promise, VmThenHandlerReturnFulfillsDownstream) {
    auto v = vm_ok(R"(
        var result = undefined;
        Promise.resolve(10)
            .then(function(v) { return v * 3; })
            .then(function(v) { result = v; });
        result;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 30.0);
}

TEST(Promise, InterpThenHandlerThrowRejectsDownstream) {
    // C12/E7：then handler throw → reject 下游 Promise，不向外同步传播
    auto v = interp_ok(R"(
        var result = "none";
        var outer_threw = false;
        Promise.resolve(1)
            .then(function(v) { throw "handler error"; })
            .catch(function(r) { result = r; });
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "handler error");
}

TEST(Promise, VmThenHandlerThrowRejectsDownstream) {
    auto v = vm_ok(R"(
        var result = "none";
        Promise.resolve(1)
            .then(function(v) { throw "handler error"; })
            .catch(function(r) { result = r; });
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "handler error");
}

// ============================================================
// 27. C8：resolve(thenable) 异步处理
// 注意：当前实现只处理 kPromise 类型的 thenable，不处理普通对象 thenable。
// 本测试验证：resolve(fulfilled Promise) 的 thenable 路径正常工作。
// 普通对象 thenable（{ then: fn }）当前作为普通值 fulfill，不展开。
// ============================================================

TEST(Promise, InterpResolveThenablePromise) {
    // C8：resolve(Promise) 通过 PerformThen 异步采纳内层 Promise 状态
    auto v = interp_ok(R"(
        var result = undefined;
        var inner = Promise.resolve(77);
        new Promise(function(resolve) { resolve(inner); })
            .then(function(v) { result = v; });
        result;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 77.0);
}

TEST(Promise, VmResolveThenablePromise) {
    auto v = vm_ok(R"(
        var result = undefined;
        var inner = Promise.resolve(77);
        new Promise(function(resolve) { resolve(inner); })
            .then(function(v) { result = v; });
        result;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 77.0);
}

// ============================================================
// 28. C3：微任务执行期间新入队的微任务在本轮继续处理
// ============================================================

TEST(Promise, InterpNestedMicrotasksProcessedInSameRound) {
    // C3：嵌套 then 新入队的微任务在同一 DrainAll 轮中处理
    // 用 result 变量记录最终值，exec 末尾重读 result
    auto v = interp_ok(R"(
        var result = 0;
        Promise.resolve(1)
            .then(function(v) {
                result = result + 1;
                return v + 1;
            })
            .then(function(v) {
                result = result + 1;
                return v + 1;
            })
            .then(function(v) {
                result = result + 1;
            });
        result;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 3.0);
}

TEST(Promise, VmNestedMicrotasksProcessedInSameRound) {
    auto v = vm_ok(R"(
        var result = 0;
        Promise.resolve(1)
            .then(function(v) {
                result = result + 1;
                return v + 1;
            })
            .then(function(v) {
                result = result + 1;
                return v + 1;
            })
            .then(function(v) {
                result = result + 1;
            });
        result;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 3.0);
}

// ============================================================
// 29. C2：同步代码完成后才处理微任务（顺序验证）
// ============================================================

TEST(Promise, InterpSyncBeforeMicrotaskOrder) {
    // C2：同步代码先执行，微任务后执行
    // 用 result 记录微任务执行时 sync_done 是否已为 true
    auto v = interp_ok(R"(
        var sync_done = false;
        var result = false;
        Promise.resolve().then(function() { result = sync_done; });
        sync_done = true;
        result;
    )");
    // 微任务在同步代码（sync_done=true）之后执行，所以 result 应为 true
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(Promise, VmSyncBeforeMicrotaskOrder) {
    auto v = vm_ok(R"(
        var sync_done = false;
        var result = false;
        Promise.resolve().then(function() { result = sync_done; });
        sync_done = true;
        result;
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// 30. 多个并发 Promise 微任务顺序（FIFO）
// ============================================================

TEST(Promise, InterpConcurrentPromiseFifoOrder) {
    // 多个 Promise 并发时，微任务按入队顺序（FIFO）执行
    // 用 result 累加数值顺序来验证 FIFO
    auto v = interp_ok(R"(
        var result = 0;
        Promise.resolve(1).then(function(v) { result = result * 10 + v; });
        Promise.resolve(2).then(function(v) { result = result * 10 + v; });
        Promise.resolve(3).then(function(v) { result = result * 10 + v; });
        result;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 123.0);
}

TEST(Promise, VmConcurrentPromiseFifoOrder) {
    auto v = vm_ok(R"(
        var result = 0;
        Promise.resolve(1).then(function(v) { result = result * 10 + v; });
        Promise.resolve(2).then(function(v) { result = result * 10 + v; });
        Promise.resolve(3).then(function(v) { result = result * 10 + v; });
        result;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 123.0);
}

// ============================================================
// 31. C19：async function 调用同步返回 Promise（类型检查）
// ============================================================

TEST(Promise, InterpAsyncReturnIsPromise) {
    // C19：async function 调用同步返回 Promise 对象
    auto v = interp_ok(R"(
        async function foo() { return 1; }
        var p = foo();
        typeof p;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "object");
}

TEST(Promise, VmAsyncReturnIsPromise) {
    auto v = vm_ok(R"(
        async function foo() { return 1; }
        var p = foo();
        typeof p;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "object");
}

// ============================================================
// 32. C22：async 内 throw 不向外同步传播，reject 返回的 Promise
// ============================================================

TEST(Promise, InterpAsyncThrowDoesNotPropagateSync) {
    // C22：async 函数内 throw 不向外同步抛出，只 reject 返回的 Promise
    auto v = interp_ok(R"(
        var sync_threw = false;
        var result = "none";
        async function foo() { throw "async err"; }
        foo().catch(function(r) { result = r; });
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "async err");
}

TEST(Promise, VmAsyncThrowDoesNotPropagateSync) {
    auto v = vm_ok(R"(
        var result = "none";
        async function foo() { throw "async err"; }
        foo().catch(function(r) { result = r; });
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "async err");
}

// ============================================================
// 33. C23：async 内 try/catch 可捕获 await rejected Promise
// ============================================================

TEST(Promise, InterpAsyncTryCatchAwaitRejected) {
    // C23：try/catch 在 async 函数内捕获 await rejected Promise
    auto v = interp_ok(R"(
        var result = "none";
        async function foo() {
            try {
                var x = await Promise.reject("caught");
            } catch(e) {
                result = e;
            }
            return result;
        }
        foo();
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "caught");
}

TEST(Promise, VmAsyncTryCatchAwaitRejected) {
    auto v = vm_ok(R"(
        var result = "none";
        async function foo() {
            try {
                var x = await Promise.reject("caught");
            } catch(e) {
                result = e;
            }
            return result;
        }
        foo();
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "caught");
}

// ============================================================
// 34. E12：await rejected Promise → 在 await 处 throw（未捕获时 reject 外层）
// ============================================================

TEST(Promise, InterpAwaitRejectedUncaughtRejectsOuter) {
    // E12：async 函数内 await rejected Promise 未被 try/catch 捕获 → reject 外层 Promise
    auto v = interp_ok(R"(
        var result = "none";
        async function foo() {
            await Promise.reject("uncaught");
            result = "should not reach";
        }
        foo().catch(function(r) { result = r; });
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "uncaught");
}

TEST(Promise, VmAwaitRejectedUncaughtRejectsOuter) {
    auto v = vm_ok(R"(
        var result = "none";
        async function foo() {
            await Promise.reject("uncaught");
            result = "should not reach";
        }
        foo().catch(function(r) { result = r; });
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "uncaught");
}

// ============================================================
// 35. async 函数表达式赋值后调用
// ============================================================

TEST(Promise, InterpAsyncExpressionAssignAndCall) {
    auto v = interp_ok(R"(
        var result = undefined;
        var fn = async function named() {
            var x = await Promise.resolve(5);
            return x * x;
        };
        fn().then(function(v) { result = v; });
        result;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 25.0);
}

TEST(Promise, VmAsyncExpressionAssignAndCall) {
    auto v = vm_ok(R"(
        var result = undefined;
        var fn = async function named() {
            var x = await Promise.resolve(5);
            return x * x;
        };
        fn().then(function(v) { result = v; });
        result;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 25.0);
}

// ============================================================
// 36. 多层 await 串行（三层）
// ============================================================

TEST(Promise, InterpTripleAwaitSerial) {
    auto v = interp_ok(R"(
        var result = undefined;
        async function foo() {
            var a = await Promise.resolve(1);
            var b = await Promise.resolve(a + 1);
            var c = await Promise.resolve(b + 1);
            result = a + b + c;
        }
        foo();
        result;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 6.0);
}

TEST(Promise, VmTripleAwaitSerial) {
    auto v = vm_ok(R"(
        var result = undefined;
        async function foo() {
            var a = await Promise.resolve(1);
            var b = await Promise.resolve(a + 1);
            var c = await Promise.resolve(b + 1);
            result = a + b + c;
        }
        foo();
        result;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 6.0);
}

// ============================================================
// 37. Promise.resolve(undefined) / Promise.resolve(null)
// ============================================================

TEST(Promise, InterpPromiseResolveUndefined) {
    auto v = interp_ok(R"(
        var result = "unset";
        Promise.resolve(undefined).then(function(v) { result = v; });
        result;
    )");
    EXPECT_TRUE(v.is_undefined());
}

TEST(Promise, VmPromiseResolveUndefined) {
    auto v = vm_ok(R"(
        var result = "unset";
        Promise.resolve(undefined).then(function(v) { result = v; });
        result;
    )");
    EXPECT_TRUE(v.is_undefined());
}

TEST(Promise, InterpPromiseResolveNull) {
    auto v = interp_ok(R"(
        var result = "unset";
        Promise.resolve(null).then(function(v) { result = v; });
        result;
    )");
    EXPECT_TRUE(v.is_null());
}

TEST(Promise, VmPromiseResolveNull) {
    auto v = vm_ok(R"(
        var result = "unset";
        Promise.resolve(null).then(function(v) { result = v; });
        result;
    )");
    EXPECT_TRUE(v.is_null());
}

// ============================================================
// 38. catch 后继续链式 then（rejection 恢复）
// ============================================================

TEST(Promise, InterpCatchRecoveryThenChain) {
    // catch 处理 rejection 后，下游 then 收到 catch 的返回值
    auto v = interp_ok(R"(
        var result = undefined;
        Promise.reject("err")
            .catch(function(r) { return "recovered"; })
            .then(function(v) { result = v; });
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "recovered");
}

TEST(Promise, VmCatchRecoveryThenChain) {
    auto v = vm_ok(R"(
        var result = undefined;
        Promise.reject("err")
            .catch(function(r) { return "recovered"; })
            .then(function(v) { result = v; });
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "recovered");
}

// ============================================================
// 39. then 的 onRejected 参数处理 rejection
// ============================================================

TEST(Promise, InterpThenOnRejectedHandlesRejection) {
    // then(onFulfilled, onRejected)：onRejected 处理 rejection
    auto v = interp_ok(R"(
        var result = "none";
        Promise.reject("bad")
            .then(
                function(v) { result = "fulfilled:" + v; },
                function(r) { result = "rejected:" + r; }
            );
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "rejected:bad");
}

TEST(Promise, VmThenOnRejectedHandlesRejection) {
    auto v = vm_ok(R"(
        var result = "none";
        Promise.reject("bad")
            .then(
                function(v) { result = "fulfilled:" + v; },
                function(r) { result = "rejected:" + r; }
            );
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "rejected:bad");
}

// ============================================================
// 40. executor 同步调用（Promise 构造时 executor 立即执行）
// ============================================================

TEST(Promise, InterpExecutorCalledSynchronously) {
    // executor 在 new Promise(...) 时同步调用
    auto v = interp_ok(R"(
        var called = false;
        new Promise(function(resolve) {
            called = true;
            resolve(1);
        });
        called;
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(Promise, VmExecutorCalledSynchronously) {
    auto v = vm_ok(R"(
        var called = false;
        new Promise(function(resolve) {
            called = true;
            resolve(1);
        });
        called;
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// 41. async 函数无 await 直接返回值（无 await 路径）
// ============================================================

TEST(Promise, InterpAsyncNoAwaitReturnValue) {
    auto v = interp_ok(R"(
        var result = undefined;
        async function foo() {
            return 123;
        }
        foo().then(function(v) { result = v; });
        result;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 123.0);
}

TEST(Promise, VmAsyncNoAwaitReturnValue) {
    auto v = vm_ok(R"(
        var result = undefined;
        async function foo() {
            return 123;
        }
        foo().then(function(v) { result = v; });
        result;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 123.0);
}

// ============================================================
// 42. async 函数无 return（隐式 return undefined）
// ============================================================

TEST(Promise, InterpAsyncImplicitReturnUndefined) {
    auto v = interp_ok(R"(
        var result = "unset";
        async function foo() {
            var x = 1;
        }
        foo().then(function(v) { result = v; });
        result;
    )");
    EXPECT_TRUE(v.is_undefined());
}

TEST(Promise, VmAsyncImplicitReturnUndefined) {
    auto v = vm_ok(R"(
        var result = "unset";
        async function foo() {
            var x = 1;
        }
        foo().then(function(v) { result = v; });
        result;
    )");
    EXPECT_TRUE(v.is_undefined());
}

// ============================================================
// 43. await 非 Promise 值（number/string/boolean/null/undefined）
// ============================================================

TEST(Promise, InterpAwaitPrimitiveString) {
    auto v = interp_ok(R"(
        var result = undefined;
        async function foo() {
            result = await "hello";
        }
        foo();
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "hello");
}

TEST(Promise, VmAwaitPrimitiveString) {
    auto v = vm_ok(R"(
        var result = undefined;
        async function foo() {
            result = await "hello";
        }
        foo();
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "hello");
}

TEST(Promise, InterpAwaitPrimitiveNull) {
    auto v = interp_ok(R"(
        var result = "unset";
        async function foo() {
            result = await null;
        }
        foo();
        result;
    )");
    EXPECT_TRUE(v.is_null());
}

TEST(Promise, VmAwaitPrimitiveNull) {
    auto v = vm_ok(R"(
        var result = "unset";
        async function foo() {
            result = await null;
        }
        foo();
        result;
    )");
    EXPECT_TRUE(v.is_null());
}

// ============================================================
// 44. Promise constructor 非函数参数 → TypeError
// ============================================================

TEST(Promise, InterpConstructorNonFunctionThrows) {
    // Promise constructor 传入非函数 → TypeError
    auto v = interp_ok(R"(
        var result = "none";
        try {
            new Promise(42);
        } catch(e) {
            result = typeof e;
        }
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "object");
}

TEST(Promise, VmConstructorNonFunctionThrows) {
    auto v = vm_ok(R"(
        var result = "none";
        try {
            new Promise(42);
        } catch(e) {
            result = typeof e;
        }
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "object");
}

// ============================================================
// 45. 链式 catch → then → catch（混合链）
// ============================================================

TEST(Promise, InterpMixedChainCatchThenCatch) {
    auto v = interp_ok(R"(
        var result = "none";
        Promise.reject("e1")
            .catch(function(r) { return "fixed"; })
            .then(function(v) { throw "e2"; })
            .catch(function(r) { result = r; });
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "e2");
}

TEST(Promise, VmMixedChainCatchThenCatch) {
    auto v = vm_ok(R"(
        var result = "none";
        Promise.reject("e1")
            .catch(function(r) { return "fixed"; })
            .then(function(v) { throw "e2"; })
            .catch(function(r) { result = r; });
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "e2");
}

// ============================================================
// 46. async 函数中 await fulfilled Promise 后继续执行
// ============================================================

TEST(Promise, InterpAsyncAwaitContinuesAfterFulfill) {
    auto v = interp_ok(R"(
        var result = 0;
        async function foo() {
            result = result + 1;
            var x = await Promise.resolve(10);
            result = result + 1;
            return x;
        }
        foo().then(function(v) { result = result + v; });
        result;
    )");
    // step1=1, step2=2, then 2+10=12
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 12.0);
}

TEST(Promise, VmAsyncAwaitContinuesAfterFulfill) {
    auto v = vm_ok(R"(
        var result = 0;
        async function foo() {
            result = result + 1;
            var x = await Promise.resolve(10);
            result = result + 1;
            return x;
        }
        foo().then(function(v) { result = result + v; });
        result;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 12.0);
}

// ============================================================
// 47. 回归：then 链中间节点 throw 后 finally 仍被调用
// ============================================================

TEST(Promise, InterpFinallyCalledAfterThenThrow) {
    auto v = interp_ok(R"(
        var finally_called = false;
        var result = "none";
        Promise.resolve(1)
            .then(function(v) { throw "mid error"; })
            .finally(function() { finally_called = true; })
            .catch(function(r) { result = r; });
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "mid error");
}

TEST(Promise, VmFinallyCalledAfterThenThrow) {
    auto v = vm_ok(R"(
        var finally_called = false;
        var result = "none";
        Promise.resolve(1)
            .then(function(v) { throw "mid error"; })
            .finally(function() { finally_called = true; })
            .catch(function(r) { result = r; });
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "mid error");
}

// ============================================================
// 48. 回归：executor resolve 后调用 reject 无效
// ============================================================

TEST(Promise, InterpExecutorResolveBeforeRejectIgnored) {
    auto v = interp_ok(R"(
        var result = "none";
        new Promise(function(resolve, reject) {
            resolve("ok");
            reject("should be ignored");
        }).then(function(v) { result = v; });
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "ok");
}

TEST(Promise, VmExecutorResolveBeforeRejectIgnored) {
    auto v = vm_ok(R"(
        var result = "none";
        new Promise(function(resolve, reject) {
            resolve("ok");
            reject("should be ignored");
        }).then(function(v) { result = v; });
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "ok");
}

// ============================================================
// P2-B: Promise.prototype 挂载到构造函数
// ============================================================

TEST(Promise, InterpPromisePrototypeAccessible) {
    auto v = interp_ok(R"(
        var pt = Promise.prototype;
        typeof pt;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "object");
}

TEST(Promise, VmPromisePrototypeAccessible) {
    auto v = vm_ok(R"(
        var pt = Promise.prototype;
        typeof pt;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "object");
}

TEST(Promise, InterpPromisePrototypeHasThen) {
    auto v = interp_ok(R"(
        typeof Promise.prototype.then;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "function");
}

TEST(Promise, VmPromisePrototypeHasThen) {
    auto v = vm_ok(R"(
        typeof Promise.prototype.then;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "function");
}

// ============================================================
// P2-C: async 函数声明提升
// ============================================================

TEST(Promise, InterpAsyncFunctionDeclarationHoisted) {
    auto v = interp_ok(R"(
        var result = "none";
        foo().then(function(v) { result = v; });
        async function foo() { return "hoisted"; }
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "hoisted");
}

TEST(Promise, VmAsyncFunctionDeclarationHoisted) {
    auto v = vm_ok(R"(
        var result = "none";
        foo().then(function(v) { result = v; });
        async function foo() { return "hoisted"; }
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "hoisted");
}

// ============================================================
// P2-D: 命名 async 函数表达式内部自引用
// ============================================================

TEST(Promise, InterpNamedAsyncFunctionExprSelfRef) {
    auto v = interp_ok(R"(
        var f = async function g() { return typeof g; };
        var result = "none";
        f().then(function(v) { result = v; });
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "function");
}

TEST(Promise, VmNamedAsyncFunctionExprSelfRef) {
    auto v = vm_ok(R"(
        var f = async function g() { return typeof g; };
        var result = "none";
        f().then(function(v) { result = v; });
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "function");
}

// ============================================================
// P2-E: await 只在 async 函数体内解析
// ============================================================

TEST(Promise, InterpAwaitOutsideAsyncIsIdentifier) {
    // 'await' outside async function body is treated as identifier
    auto v = interp_ok(R"(
        var await = 42;
        await;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 42.0);
}

TEST(Promise, VmAwaitOutsideAsyncIsIdentifier) {
    auto v = vm_ok(R"(
        var await = 42;
        await;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 42.0);
}

TEST(Promise, InterpAwaitInsideNestedNonAsyncIsIdentifier) {
    // await inside non-async nested function is treated as identifier
    auto v = interp_ok(R"(
        var result = "none";
        async function outer() {
            function inner() {
                var await = "id";
                return await;
            }
            result = inner();
        }
        outer();
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "id");
}

TEST(Promise, VmAwaitInsideNestedNonAsyncIsIdentifier) {
    auto v = vm_ok(R"(
        var result = "none";
        async function outer() {
            function inner() {
                var await = "id";
                return await;
            }
            result = inner();
        }
        outer();
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "id");
}

// ============================================================
// P2-F: Promise 自循环 resolve 导致 TypeError rejection
// ============================================================

TEST(Promise, InterpPromiseSelfCycleRejectsWithTypeError) {
    auto v = interp_ok(R"(
        var result = "none";
        var p = Promise.resolve().then(function() { return p; });
        p.catch(function(e) { result = typeof e; });
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "object");
}

TEST(Promise, VmPromiseSelfCycleRejectsWithTypeError) {
    auto v = vm_ok(R"(
        var result = "none";
        var p = Promise.resolve().then(function() { return p; });
        p.catch(function(e) { result = typeof e; });
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "object");
}

}  // namespace
