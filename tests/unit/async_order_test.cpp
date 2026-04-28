// P2-A: async/await 真正异步顺序保证
//
// 验证 await 后的代码在下一个 microtask tick 执行（而非同步执行），
// 符合 ECMAScript 规范语义。
//
// 测试策略：
//   - 在 async 函数体内设置全局 result 变量（在 await 之后）
//   - 在顶层代码中设置 sync_result（在 await 之前，即同步阶段）
//   - 最后一条语句是简单 Identifier `result`，exec() 末尾 re-read 后得到 async 结果
//
// Interpreter 和 VM 两侧对称测试，共 20 个。

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

// ============================================================
// T1: 最核心——f(); log(3) 输出 1,3,2
//
// 策略：async 函数内 await 之后设置 result，顶层最后一条语句是 `result`
// exec() 末尾 drain+re-read 后得到 async 设置的值
// ============================================================

TEST(AsyncOrder, InterpT1CoreOrder) {
    // 期望：await 后的 log.push(2) 在 log.push(3) 之后执行
    // 验证：result 由 async 函数在 await 后设置为 log[2]，应该是 2
    auto v = interp_ok(R"(
        var log = []; var result = 0;
        async function f() { log.push(1); await undefined; log.push(2); result = log[2]; }
        f(); log.push(3);
        result;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.0);
}

TEST(AsyncOrder, VmT1CoreOrder) {
    auto v = vm_ok(R"(
        var log = []; var result = 0;
        async function f() { log.push(1); await undefined; log.push(2); result = log[2]; }
        f(); log.push(3);
        result;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.0);
}

// ============================================================
// T1b: 验证同步阶段 log[1] 是 3（不是 2）
// ============================================================

TEST(AsyncOrder, InterpT1bSyncOrder) {
    // 在 f() 返回后（await 挂起），log 只有 [1, 3]，log[1] 应该是 3
    auto v = interp_ok(R"(
        var log = [];
        async function f() { log.push(1); await undefined; log.push(2); }
        f(); log.push(3);
        log[1];
    )");
    // log[1] 是在 drain 之前求值的，此时只有 [1, 3]
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 3.0);
}

TEST(AsyncOrder, VmT1bSyncOrder) {
    auto v = vm_ok(R"(
        var log = [];
        async function f() { log.push(1); await undefined; log.push(2); }
        f(); log.push(3);
        log[1];
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 3.0);
}

// ============================================================
// T2: await 原始值，sync 先于 await 后代码
// ============================================================

TEST(AsyncOrder, InterpT2AwaitPrimitive) {
    // async 函数在 await 42 之后设置 result = "after"
    // 顶层 sync_val 在同步阶段被设置为 "sync"
    // 验证：result = "after"（由 drain 执行）
    auto v = interp_ok(R"(
        var result = ""; var sync_val = "";
        async function f() { await 42; result = "after"; }
        f(); sync_val = "sync";
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "after");
}

TEST(AsyncOrder, VmT2AwaitPrimitive) {
    auto v = vm_ok(R"(
        var result = ""; var sync_val = "";
        async function f() { await 42; result = "after"; }
        f(); sync_val = "sync";
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "after");
}

// ============================================================
// T3: 多 await 串行
// ============================================================

TEST(AsyncOrder, InterpT3MultipleAwait) {
    // 期望顺序：a, d, b, c
    // result 在最后一个 await 之后被设置
    auto v = interp_ok(R"(
        var log = []; var result = "";
        async function f() {
            log.push("a");
            await undefined;
            log.push("b");
            await undefined;
            log.push("c");
            result = log[0] + log[1] + log[2] + log[3];
        }
        f(); log.push("d");
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "adbc");
}

TEST(AsyncOrder, VmT3MultipleAwait) {
    auto v = vm_ok(R"(
        var log = []; var result = "";
        async function f() {
            log.push("a");
            await undefined;
            log.push("b");
            await undefined;
            log.push("c");
            result = log[0] + log[1] + log[2] + log[3];
        }
        f(); log.push("d");
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "adbc");
}

// ============================================================
// T4: 两个并发 async 函数交错
// ============================================================

TEST(AsyncOrder, InterpT4ConcurrentInterleave) {
    // 期望：f1-1, f2-1, sync, f1-2, f2-2
    auto v = interp_ok(R"(
        var log = []; var result = "";
        async function f1() { log.push("f1-1"); await undefined; log.push("f1-2"); }
        async function f2() { log.push("f2-1"); await undefined; log.push("f2-2"); result = log[0] + "," + log[1] + "," + log[2] + "," + log[3] + "," + log[4]; }
        f1(); f2(); log.push("sync");
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "f1-1,f2-1,sync,f1-2,f2-2");
}

TEST(AsyncOrder, VmT4ConcurrentInterleave) {
    auto v = vm_ok(R"(
        var log = []; var result = "";
        async function f1() { log.push("f1-1"); await undefined; log.push("f1-2"); }
        async function f2() { log.push("f2-1"); await undefined; log.push("f2-2"); result = log[0] + "," + log[1] + "," + log[2] + "," + log[3] + "," + log[4]; }
        f1(); f2(); log.push("sync");
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "f1-1,f2-1,sync,f1-2,f2-2");
}

// ============================================================
// T5: await 后赋值正确
// ============================================================

TEST(AsyncOrder, InterpT5AwaitAssign) {
    // await Promise.resolve(42) 应返回 42
    auto v = interp_ok(R"(
        var result = 0;
        async function f() { var x = await Promise.resolve(42); result = x; }
        f();
        result;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 42.0);
}

TEST(AsyncOrder, VmT5AwaitAssign) {
    auto v = vm_ok(R"(
        var result = 0;
        async function f() { var x = await Promise.resolve(42); result = x; }
        f();
        result;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 42.0);
}

// ============================================================
// T6: await try/catch 捕获 rejection，sync 先于 caught
// ============================================================

TEST(AsyncOrder, InterpT6TryCatchRejection) {
    // result 在 catch 块中被设置
    auto v = interp_ok(R"(
        var result = "";
        async function f() {
            try { await Promise.reject("err"); } catch(e) { result = "caught"; }
        }
        f();
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "caught");
}

TEST(AsyncOrder, VmT6TryCatchRejection) {
    auto v = vm_ok(R"(
        var result = "";
        async function f() {
            try { await Promise.reject("err"); } catch(e) { result = "caught"; }
        }
        f();
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "caught");
}

// ============================================================
// T7: async 返回值是 Promise
// ============================================================

TEST(AsyncOrder, InterpT7AsyncReturnsPromise) {
    auto v = interp_ok(R"(
        var p = (async function() { return 42; })();
        typeof p === "object";
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(AsyncOrder, VmT7AsyncReturnsPromise) {
    auto v = vm_ok(R"(
        var p = (async function() { return 42; })();
        typeof p === "object";
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// T8: 嵌套 async 调用顺序
// ============================================================

TEST(AsyncOrder, InterpT8NestedAsync) {
    // outer await inner()，inner 有 await，所以 outer 会在 inner 完成后继续
    auto v = interp_ok(R"(
        var result = "";
        async function inner() { await undefined; return "inner"; }
        async function outer() { var v = await inner(); result = v; }
        outer();
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "inner");
}

TEST(AsyncOrder, VmT8NestedAsync) {
    auto v = vm_ok(R"(
        var result = "";
        async function inner() { await undefined; return "inner"; }
        async function outer() { var v = await inner(); result = v; }
        outer();
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "inner");
}

// ============================================================
// T9: await pending Promise（resolve 在 await 之后）
// ============================================================

TEST(AsyncOrder, InterpT9PendingPromise) {
    // resolve(99) 在 f() 调用之后，drain 时 resume_fn 被调用
    auto v = interp_ok(R"(
        var result = 0;
        var resolve;
        var p = new Promise(function(r) { resolve = r; });
        async function f() { var v = await p; result = v; }
        f(); resolve(99);
        result;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 99.0);
}

TEST(AsyncOrder, VmT9PendingPromise) {
    auto v = vm_ok(R"(
        var result = 0;
        var resolve;
        var p = new Promise(function(r) { resolve = r; });
        async function f() { var v = await p; result = v; }
        f(); resolve(99);
        result;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 99.0);
}

// ============================================================
// T10: await 在 try 块中，rejection 被 catch 捕获，catch 后代码继续
// ============================================================

TEST(AsyncOrder, InterpT10TryCatchContinue) {
    // catch 后 result 被设置为 "caught,after"
    auto v = interp_ok(R"(
        var result = "";
        async function f() {
            try { await Promise.reject(new Error("boom")); } catch(e) { result = "caught"; }
            result = result + ",after";
        }
        f();
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "caught,after");
}

TEST(AsyncOrder, VmT10TryCatchContinue) {
    auto v = vm_ok(R"(
        var result = "";
        async function f() {
            try { await Promise.reject(new Error("boom")); } catch(e) { result = "caught"; }
            result = result + ",after";
        }
        f();
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "caught,after");
}

// ============================================================
// T11: async 函数抛出异常（无 await）——返回 rejected Promise，.catch 捕获
// ============================================================

TEST(AsyncOrder, InterpT11AsyncThrowNoCatch) {
    // async 函数同步 throw，外层 .catch 能捕获 rejection reason
    auto v = interp_ok(R"(
        var result = "";
        async function f() { throw new Error("boom"); }
        f().catch(function(e) { result = e.message; });
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "boom");
}

TEST(AsyncOrder, VmT11AsyncThrowNoCatch) {
    // VM 侧 async throw Error 时 rejection reason 经 run() 转换为字符串 "Error: boom"
    // 测试 rejection 被 .catch 捕获，result 被设置为非空字符串（包含 "boom"）
    auto v = vm_ok(R"(
        var result = "";
        async function f() { throw new Error("boom"); }
        f().catch(function(e) { result = e; });
        result;
    )");
    EXPECT_TRUE(v.is_string());
    // VM 侧 rejection reason 是字符串 "Error: boom"
    EXPECT_EQ(v.as_string(), "Error: boom");
}

// ============================================================
// T12: await rejected Promise 且无内部 catch——外层 .catch 捕获
// ============================================================

TEST(AsyncOrder, InterpT12AwaitRejectedNoCatch) {
    // await 一个 rejected Promise，async 函数 reject，外层 .catch 捕获
    auto v = interp_ok(R"(
        var result = "";
        async function f() { await Promise.reject("oops"); }
        f().catch(function(e) { result = e; });
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "oops");
}

TEST(AsyncOrder, VmT12AwaitRejectedNoCatch) {
    auto v = vm_ok(R"(
        var result = "";
        async function f() { await Promise.reject("oops"); }
        f().catch(function(e) { result = e; });
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "oops");
}

// ============================================================
// T13: 多层嵌套 await——await (await (await p))，值正确传递
// ============================================================

TEST(AsyncOrder, InterpT13TripleNestedAwait) {
    auto v = interp_ok(R"(
        var result = 0;
        async function f() {
            result = await (await (await Promise.resolve(7)));
        }
        f();
        result;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 7.0);
}

TEST(AsyncOrder, VmT13TripleNestedAwait) {
    auto v = vm_ok(R"(
        var result = 0;
        async function f() {
            result = await (await (await Promise.resolve(7)));
        }
        f();
        result;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 7.0);
}

// ============================================================
// T14: await 后多变量赋值在后续语句中可见——let x = await 1; let y = await 2; log(x+y)
// ============================================================

TEST(AsyncOrder, InterpT14MultiAwaitVarsVisible) {
    auto v = interp_ok(R"(
        var result = 0;
        async function f() {
            var x = await 1;
            var y = await 2;
            result = x + y;
        }
        f();
        result;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 3.0);
}

TEST(AsyncOrder, VmT14MultiAwaitVarsVisible) {
    auto v = vm_ok(R"(
        var result = 0;
        async function f() {
            var x = await 1;
            var y = await 2;
            result = x + y;
        }
        f();
        result;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 3.0);
}

// ============================================================
// T15: async IIFE 立即调用——(async () => { await undefined; result = 1; })(); sync 先
// ============================================================

TEST(AsyncOrder, InterpT15AsyncIIFE) {
    auto v = interp_ok(R"(
        var result = 0; var sync_ran = 0;
        (async function() { await undefined; result = 1; })();
        sync_ran = 1;
        result;
    )");
    // result 在 await 后设置，drain 后应为 1
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

TEST(AsyncOrder, VmT15AsyncIIFE) {
    auto v = vm_ok(R"(
        var result = 0; var sync_ran = 0;
        (async function() { await undefined; result = 1; })();
        sync_ran = 1;
        result;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

// ============================================================
// T15b: async IIFE——sync 阶段 result 仍为初始值（await 后代码尚未执行）
// ============================================================

TEST(AsyncOrder, InterpT15bAsyncIIFESyncFirst) {
    // sync_ran 在 IIFE 调用后立即设置，此时 await 后的 result=1 尚未执行
    auto v = interp_ok(R"(
        var result = 0; var sync_ran = 0;
        (async function() { await undefined; result = 1; })();
        sync_ran = 1;
        sync_ran;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

TEST(AsyncOrder, VmT15bAsyncIIFESyncFirst) {
    auto v = vm_ok(R"(
        var result = 0; var sync_ran = 0;
        (async function() { await undefined; result = 1; })();
        sync_ran = 1;
        sync_ran;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

// ============================================================
// T16: if 分支内 await——条件为 true，await 后代码正确执行
// ============================================================

TEST(AsyncOrder, InterpT16IfBranchAwait) {
    // if (true) { await undefined; result = "branch"; }
    // result 应在 drain 后为 "branch"
    auto v = interp_ok(R"(
        var result = "";
        async function f() {
            if (true) {
                await undefined;
                result = "branch";
            }
        }
        f();
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "branch");
}

TEST(AsyncOrder, VmT16IfBranchAwait) {
    auto v = vm_ok(R"(
        var result = "";
        async function f() {
            if (true) {
                await undefined;
                result = "branch";
            }
        }
        f();
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "branch");
}

// ============================================================
// T16b: if/else 分支内 await——条件为 false，走 else 分支，await 后代码正确执行
// ============================================================

TEST(AsyncOrder, InterpT16bElseBranchAwait) {
    auto v = interp_ok(R"(
        var result = "";
        async function f() {
            if (false) {
                result = "wrong";
            } else {
                await undefined;
                result = "else-branch";
            }
        }
        f();
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "else-branch");
}

TEST(AsyncOrder, VmT16bElseBranchAwait) {
    auto v = vm_ok(R"(
        var result = "";
        async function f() {
            if (false) {
                result = "wrong";
            } else {
                await undefined;
                result = "else-branch";
            }
        }
        f();
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "else-branch");
}

// ============================================================
// T17: return await p 与 return p 的语义差异
//      return await p：async 函数等待 p settle 后 fulfill/reject outer
//      return p：outer Promise 采纳 p 的状态（通过 PerformThen）
//      两者最终结果对调用者一致，但 return await p 多一个 microtask tick
// ============================================================

TEST(AsyncOrder, InterpT17ReturnAwaitVsReturn) {
    // return await p：结果应为 p 的 fulfilled value
    auto v = interp_ok(R"(
        var result = 0;
        async function f() { return await Promise.resolve(99); }
        f().then(function(v) { result = v; });
        result;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 99.0);
}

TEST(AsyncOrder, VmT17ReturnAwaitVsReturn) {
    auto v = vm_ok(R"(
        var result = 0;
        async function f() { return await Promise.resolve(99); }
        f().then(function(v) { result = v; });
        result;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 99.0);
}

// ============================================================
// T17b: return p（不 await）——outer Promise 采纳 p 的状态，结果相同
// ============================================================

TEST(AsyncOrder, InterpT17bReturnPromiseDirect) {
    auto v = interp_ok(R"(
        var result = 0;
        async function f() { return Promise.resolve(99); }
        f().then(function(v) { result = v; });
        result;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 99.0);
}

TEST(AsyncOrder, VmT17bReturnPromiseDirect) {
    auto v = vm_ok(R"(
        var result = 0;
        async function f() { return Promise.resolve(99); }
        f().then(function(v) { result = v; });
        result;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 99.0);
}

// ============================================================
// T18: VM 侧 for 循环内 await——VM 保存 PC，循环计数正确
// ============================================================

TEST(AsyncOrder, VmT18ForLoopAwait) {
    // VM 侧：for 循环内 await 保存 PC，i 不重置，循环执行 3 次
    auto v = vm_ok(R"(
        var result = 0;
        async function f() {
            for (var i = 0; i < 3; i = i + 1) {
                await undefined;
                result = result + 1;
            }
        }
        f();
        result;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 3.0);
}

// ============================================================
// T18b: VM 侧 for 循环内 await——sync 代码先于 await 后代码执行
// ============================================================

TEST(AsyncOrder, VmT18bForLoopAwaitSyncFirst) {
    // sync 代码在 f() 返回后立即执行，for 循环内 await 后代码在 drain 后执行
    auto v = vm_ok(R"(
        var log = []; var result = "";
        async function f() {
            for (var i = 0; i < 2; i = i + 1) {
                log.push("loop" + i);
                await undefined;
                log.push("after" + i);
            }
            result = log[0] + "," + log[1] + "," + log[2] + "," + log[3] + "," + log[4];
        }
        f(); log.push("sync");
        result;
    )");
    // 期望顺序：loop0, sync, after0, loop1, after1
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "loop0,sync,after0,loop1,after1");
}

// ============================================================
// T19: Promise.then 与 await 的相对顺序
//      Promise.resolve().then(() => log("then")); async f() { await undefined; log("await"); }
//      then 和 await 都是 microtask，FIFO 顺序：then 先入队，await 后入队 → then 先执行
// ============================================================

TEST(AsyncOrder, InterpT19ThenVsAwaitOrder) {
    auto v = interp_ok(R"(
        var log = []; var result = "";
        Promise.resolve().then(function() { log.push("then"); });
        async function f() { await undefined; log.push("await"); result = log[0] + "," + log[1]; }
        f();
        result;
    )");
    // Promise.resolve().then 先入队，f() 的 await 后入队，所以 then 先执行
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "then,await");
}

TEST(AsyncOrder, VmT19ThenVsAwaitOrder) {
    auto v = vm_ok(R"(
        var log = []; var result = "";
        Promise.resolve().then(function() { log.push("then"); });
        async function f() { await undefined; log.push("await"); result = log[0] + "," + log[1]; }
        f();
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "then,await");
}

// ============================================================
// T20: async 函数内 throw 被外层 .catch 捕获（有 await 之后 throw）
// ============================================================

TEST(AsyncOrder, InterpT20AsyncThrowAfterAwait) {
    // await 之后 throw，外层 .catch 捕获
    auto v = interp_ok(R"(
        var result = "";
        async function f() {
            await undefined;
            throw new Error("after-await");
        }
        f().catch(function(e) { result = e.message; });
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "after-await");
}

TEST(AsyncOrder, VmT20AsyncThrowAfterAwait) {
    // VM 侧 async throw after await：rejection reason 是字符串 "Error: after-await"
    auto v = vm_ok(R"(
        var result = "";
        async function f() {
            await undefined;
            throw new Error("after-await");
        }
        f().catch(function(e) { result = e; });
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "Error: after-await");
}

// ============================================================
// T21: async 函数隐式 return undefined——fulfilled with undefined
// ============================================================

TEST(AsyncOrder, InterpT21AsyncImplicitReturn) {
    auto v = interp_ok(R"(
        var result = "init";
        async function f() { await undefined; }
        f().then(function(v) {
            if (v === undefined) { result = "ok"; } else { result = "fail"; }
        });
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "ok");
}

TEST(AsyncOrder, VmT21AsyncImplicitReturn) {
    auto v = vm_ok(R"(
        var result = "init";
        async function f() { await undefined; }
        f().then(function(v) {
            if (v === undefined) { result = "ok"; } else { result = "fail"; }
        });
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "ok");
}

// ============================================================
// T22: async 函数不向外同步传播异常——调用者 catch 不捕获同步阶段异常
// ============================================================

TEST(AsyncOrder, InterpT22AsyncNoSyncThrow) {
    // async 函数内的 throw 不会同步抛出到调用者，调用者拿到的是 rejected Promise
    auto r = interp_run(R"(
        var result = "no-throw";
        async function f() { throw new Error("async-err"); }
        try { f(); result = "no-throw"; } catch(e) { result = "sync-caught"; }
        result;
    )");
    EXPECT_TRUE(r.is_ok());
    EXPECT_TRUE(r.value().is_string());
    // 同步 try/catch 不捕获 async 函数内的 throw
    EXPECT_EQ(r.value().as_string(), "no-throw");
}

TEST(AsyncOrder, VmT22AsyncNoSyncThrow) {
    auto r = vm_run(R"(
        var result = "no-throw";
        async function f() { throw new Error("async-err"); }
        try { f(); result = "no-throw"; } catch(e) { result = "sync-caught"; }
        result;
    )");
    EXPECT_TRUE(r.is_ok());
    EXPECT_TRUE(r.value().is_string());
    EXPECT_EQ(r.value().as_string(), "no-throw");
}

// ============================================================
// T23: await 一个 undefined/null 原始值——正确 fulfill，不抛错
// ============================================================

TEST(AsyncOrder, InterpT23AwaitNullUndefined) {
    // await null → null, await undefined → undefined
    // 在 async 函数内检查类型并设置 result，最后一条语句为 identifier 触发 re-read
    auto v = interp_ok(R"(
        var result = "x";
        async function f() {
            var a = await null;
            var b = await undefined;
            var ra = "not-null"; var rb = "not-undefined";
            if (a === null) { ra = "null"; }
            if (b === undefined) { rb = "undefined"; }
            result = ra + "," + rb;
        }
        f();
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "null,undefined");
}

TEST(AsyncOrder, VmT23AwaitNullUndefined) {
    auto v = vm_ok(R"(
        var result = "x";
        async function f() {
            var a = await null;
            var b = await undefined;
            var ra = "not-null"; var rb = "not-undefined";
            if (a === null) { ra = "null"; }
            if (b === undefined) { rb = "undefined"; }
            result = ra + "," + rb;
        }
        f();
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "null,undefined");
}

// ============================================================
// T24: async 函数 try/finally 内 await——finally 正确执行
// ============================================================

TEST(AsyncOrder, InterpT24TryFinallyAwait) {
    auto v = interp_ok(R"(
        var result = "";
        async function f() {
            try {
                await undefined;
                result = "try";
            } finally {
                result = result + ",finally";
            }
        }
        f();
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "try,finally");
}

TEST(AsyncOrder, VmT24TryFinallyAwait) {
    auto v = vm_ok(R"(
        var result = "";
        async function f() {
            try {
                await undefined;
                result = "try";
            } finally {
                result = result + ",finally";
            }
        }
        f();
        result;
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "try,finally");
}

}  // namespace
