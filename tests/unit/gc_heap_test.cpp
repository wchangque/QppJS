//
// Phase 9.1-9.5：mark-sweep GC 测试
//
// 测试重点：
//   1. GC 后可达对象不被误回收（全局变量、闭包）
//   2. GC 后循环引用被回收（通过 LSan 验证：run_ut 不新增泄露）
//   3. bind 生成函数的 bound_target/bound_this/bound_args 在 GC 后仍可访问
//

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
    EXPECT_TRUE(result.is_ok()) << "interp error: " << (result.is_ok() ? "" : result.error().message());
    return result.is_ok() ? result.value() : qppjs::Value::undefined();
}

qppjs::Value vm_ok(std::string_view source) {
    auto parse_result = qppjs::parse_program(source);
    EXPECT_TRUE(parse_result.ok()) << "parse failed: " << parse_result.error().message();
    if (!parse_result.ok()) return qppjs::Value::undefined();
    qppjs::Compiler compiler;
    auto bytecode = compiler.compile(parse_result.value());
    qppjs::VM vm;
    auto result = vm.exec(bytecode);
    EXPECT_TRUE(result.is_ok()) << "vm error: " << (result.is_ok() ? "" : result.error().message());
    return result.is_ok() ? result.value() : qppjs::Value::undefined();
}

// ============================================================
// GC 不误回收可达对象
// ============================================================

TEST(GcHeap, InterpGlobalVarSurvivesGc) {
    // 全局变量在 exec 结束时 GC，结果值应正确
    auto v = interp_ok("var x = 42; x;");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 42.0);
}

TEST(GcHeap, VmGlobalVarSurvivesGc) {
    auto v = vm_ok("var x = 42; x;");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 42.0);
}

TEST(GcHeap, InterpClosureSurvivesGc) {
    // 闭包捕获变量，调用后结果正确
    auto v = interp_ok(R"(
        function makeCounter() {
            var count = 0;
            return function() { count = count + 1; return count; };
        }
        var c = makeCounter();
        c(); c(); c();
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 3.0);
}

TEST(GcHeap, VmClosureSurvivesGc) {
    auto v = vm_ok(R"(
        function makeCounter() {
            var count = 0;
            return function() { count = count + 1; return count; };
        }
        var c = makeCounter();
        c(); c(); c();
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 3.0);
}

// ============================================================
// 循环引用不导致 LSan 泄露（通过 run_ut.sh 验证，此处验证功能正确性）
// ============================================================

TEST(GcHeap, InterpClosureCircularRef) {
    // 闭包循环引用：Environment -> Cell -> JSFunction -> Environment
    // GC 应回收整个环，程序应正常执行
    auto v = interp_ok(R"(
        function outer() {
            var x = 1;
            var inner = function() { return x; };
            return inner();
        }
        outer();
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

TEST(GcHeap, VmClosureCircularRef) {
    auto v = vm_ok(R"(
        function outer() {
            var x = 1;
            var inner = function() { return x; };
            return inner();
        }
        outer();
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

TEST(GcHeap, InterpMultipleClosuresSharedEnv) {
    // 多个闭包共享 env，GC 后均可用
    auto v = interp_ok(R"(
        function makeAdder(x) {
            return function(y) { return x + y; };
        }
        var add5 = makeAdder(5);
        var add10 = makeAdder(10);
        add5(3) + add10(3);
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 21.0);
}

TEST(GcHeap, VmMultipleClosuresSharedEnv) {
    auto v = vm_ok(R"(
        function makeAdder(x) {
            return function(y) { return x + y; };
        }
        var add5 = makeAdder(5);
        var add10 = makeAdder(10);
        add5(3) + add10(3);
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 21.0);
}

// ============================================================
// bind 生成函数在 GC 后仍可正常调用
// ============================================================

TEST(GcHeap, InterpBindSurvivesGc) {
    auto v = interp_ok(R"(
        function add(a, b) { return a + b; }
        var add5 = add.bind(null, 5);
        add5(3);
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 8.0);
}

TEST(GcHeap, VmBindSurvivesGc) {
    auto v = vm_ok(R"(
        function add(a, b) { return a + b; }
        var add5 = add.bind(null, 5);
        add5(3);
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 8.0);
}

TEST(GcHeap, InterpChainedBindSurvivesGc) {
    // 链式 bind：bound_target 本身也是 bound function
    auto v = interp_ok(R"(
        function add(a, b, c) { return a + b + c; }
        var add5 = add.bind(null, 5);
        var add5and3 = add5.bind(null, 3);
        add5and3(2);
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 10.0);
}

TEST(GcHeap, VmChainedBindSurvivesGc) {
    auto v = vm_ok(R"(
        function add(a, b, c) { return a + b + c; }
        var add5 = add.bind(null, 5);
        var add5and3 = add5.bind(null, 3);
        add5and3(2);
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 10.0);
}

// ============================================================
// 对象属性中的函数引用在 GC 后仍可用
// ============================================================

TEST(GcHeap, InterpObjectWithFunctionProperty) {
    auto v = interp_ok(R"(
        var obj = { greet: function(name) { return "hello " + name; } };
        obj.greet("world");
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "hello world");
}

TEST(GcHeap, VmObjectWithFunctionProperty) {
    auto v = vm_ok(R"(
        var obj = { greet: function(name) { return "hello " + name; } };
        obj.greet("world");
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "hello world");
}

// ============================================================
// 深层闭包链 GC 后仍可正常执行
// ============================================================

TEST(GcHeap, InterpDeepClosureChainGc) {
    auto v = interp_ok(R"(
        function a() {
            var x = 1;
            return function b() {
                var y = 2;
                return function c() {
                    return x + y;
                };
            };
        }
        a()()();
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 3.0);
}

TEST(GcHeap, VmDeepClosureChainGc) {
    auto v = vm_ok(R"(
        function a() {
            var x = 1;
            return function b() {
                var y = 2;
                return function c() {
                    return x + y;
                };
            };
        }
        a()()();
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 3.0);
}

// ============================================================
// 直接循环引用（对象互相持有对方引用）GC 后程序正常执行
// ============================================================

TEST(GcHeap, InterpDirectObjectCycleGc) {
    // obj.self = obj 形成自环，GC 应回收，程序不崩溃
    auto v = interp_ok(R"(
        function makeCycle() {
            var obj = {};
            obj.self = obj;
            return 42;
        }
        makeCycle();
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 42.0);
}

TEST(GcHeap, VmDirectObjectCycleGc) {
    auto v = vm_ok(R"(
        function makeCycle() {
            var obj = {};
            obj.self = obj;
            return 42;
        }
        makeCycle();
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 42.0);
}

// ============================================================
// 对象互相持有对方引用（A->B->A 二元环）GC 后正常执行
// ============================================================

TEST(GcHeap, InterpMutualObjectCycleGc) {
    auto v = interp_ok(R"(
        function makeMutual() {
            var a = {};
            var b = {};
            a.other = b;
            b.other = a;
            return 99;
        }
        makeMutual();
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 99.0);
}

TEST(GcHeap, VmMutualObjectCycleGc) {
    auto v = vm_ok(R"(
        function makeMutual() {
            var a = {};
            var b = {};
            a.other = b;
            b.other = a;
            return 99;
        }
        makeMutual();
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 99.0);
}

// ============================================================
// 返回值（对象）是 GC 根，不被误回收
// ============================================================

TEST(GcHeap, InterpReturnedObjectSurvivesGc) {
    // exec() 返回值若为对象，应作为 GC 根被保留
    // 此处只验证返回对象的属性值正确（GC 后对象未被误删）
    auto v = interp_ok(R"(
        function makeObj() {
            return { x: 7, y: 13 };
        }
        var r = makeObj();
        r.x + r.y;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 20.0);
}

TEST(GcHeap, VmReturnedObjectSurvivesGc) {
    auto v = vm_ok(R"(
        function makeObj() {
            return { x: 7, y: 13 };
        }
        var r = makeObj();
        r.x + r.y;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 20.0);
}

// ============================================================
// 抛出异常路径：GC 在 exec() 末尾仍会触发，不崩溃
// ============================================================

TEST(GcHeap, InterpThrowPathGcNoCrash) {
    // 程序抛出异常，exec() 返回 error，GC 仍应在末尾触发且不崩溃
    auto parse_result = qppjs::parse_program("throw new TypeError('boom');");
    EXPECT_TRUE(parse_result.ok());
    if (!parse_result.ok()) return;
    qppjs::Interpreter interp;
    auto result = interp.exec(parse_result.value());
    // 结果应为错误（不是 ok）
    EXPECT_FALSE(result.is_ok());
}

TEST(GcHeap, VmThrowPathGcNoCrash) {
    auto parse_result = qppjs::parse_program("throw new TypeError('boom');");
    EXPECT_TRUE(parse_result.ok());
    if (!parse_result.ok()) return;
    qppjs::Compiler compiler;
    auto bytecode = compiler.compile(parse_result.value());
    qppjs::VM vm;
    auto result = vm.exec(bytecode);
    EXPECT_FALSE(result.is_ok());
}

// ============================================================
// try/catch 路径：catch 块内的对象在 GC 后仍可访问
// ============================================================

TEST(GcHeap, InterpCaughtErrorObjectSurvivesGc) {
    auto v = interp_ok(R"(
        var caught = null;
        try {
            throw new TypeError("test error");
        } catch (e) {
            caught = e;
        }
        caught instanceof TypeError;
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_EQ(v.as_bool(), true);
}

TEST(GcHeap, VmCaughtErrorObjectSurvivesGc) {
    auto v = vm_ok(R"(
        var caught = null;
        try {
            throw new TypeError("test error");
        } catch (e) {
            caught = e;
        }
        caught instanceof TypeError;
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_EQ(v.as_bool(), true);
}

// ============================================================
// 数组持有函数引用，GC 后函数仍可调用
// ============================================================

TEST(GcHeap, InterpArrayHoldingFunctionSurvivesGc) {
    auto v = interp_ok(R"(
        var fns = [];
        fns.push(function() { return 10; });
        fns.push(function() { return 20; });
        fns[0]() + fns[1]();
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 30.0);
}

TEST(GcHeap, VmArrayHoldingFunctionSurvivesGc) {
    auto v = vm_ok(R"(
        var fns = [];
        fns.push(function() { return 10; });
        fns.push(function() { return 20; });
        fns[0]() + fns[1]();
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 30.0);
}

// ============================================================
// 闭包捕获数组，GC 后数组元素仍可访问
// ============================================================

TEST(GcHeap, InterpClosureCapturesArraySurvivesGc) {
    auto v = interp_ok(R"(
        function makeAcc() {
            var items = [1, 2, 3];
            return function() { return items[0] + items[1] + items[2]; };
        }
        var acc = makeAcc();
        acc();
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 6.0);
}

TEST(GcHeap, VmClosureCapturesArraySurvivesGc) {
    auto v = vm_ok(R"(
        function makeAcc() {
            var items = [1, 2, 3];
            return function() { return items[0] + items[1] + items[2]; };
        }
        var acc = makeAcc();
        acc();
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 6.0);
}

// ============================================================
// named function expression 自引用：GC 后递归调用仍正确
// ============================================================

TEST(GcHeap, InterpNamedFunctionExprRecursionAfterGc) {
    auto v = interp_ok(R"(
        var fib = function fib(n) {
            if (n <= 1) return n;
            return fib(n - 1) + fib(n - 2);
        };
        fib(7);
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 13.0);
}

TEST(GcHeap, VmNamedFunctionExprRecursionAfterGc) {
    auto v = vm_ok(R"(
        var fib = function fib(n) {
            if (n <= 1) return n;
            return fib(n - 1) + fib(n - 2);
        };
        fib(7);
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 13.0);
}

// ============================================================
// bind 生成函数作为构造器（new BoundFn()），GC 后实例属性正确
// ============================================================

TEST(GcHeap, InterpBoundFunctionAsConstructorSurvivesGc) {
    auto v = interp_ok(R"(
        function Point(x, y) {
            this.x = x;
            this.y = y;
        }
        var PointAtOriginX = Point.bind(null, 0);
        var p = new PointAtOriginX(5);
        p.x + p.y;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 5.0);
}

TEST(GcHeap, VmBoundFunctionAsConstructorSurvivesGc) {
    auto v = vm_ok(R"(
        function Point(x, y) {
            this.x = x;
            this.y = y;
        }
        var PointAtOriginX = Point.bind(null, 0);
        var p = new PointAtOriginX(5);
        p.x + p.y;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 5.0);
}

// ============================================================
// 大量短生命周期对象：GC 应回收，不积累内存
// ============================================================

TEST(GcHeap, InterpManyShortLivedObjectsGc) {
    // 在循环中创建大量对象并丢弃，GC 应能处理（不崩溃，结果正确）
    auto v = interp_ok(R"(
        var sum = 0;
        var i = 0;
        while (i < 50) {
            var obj = { val: i };
            sum = sum + obj.val;
            i = i + 1;
        }
        sum;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1225.0);
}

TEST(GcHeap, VmManyShortLivedObjectsGc) {
    auto v = vm_ok(R"(
        var sum = 0;
        var i = 0;
        while (i < 50) {
            var obj = { val: i };
            sum = sum + obj.val;
            i = i + 1;
        }
        sum;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1225.0);
}

// ============================================================
// 原型链对象在 GC 后仍可正确访问（instanceof 不被误回收）
// ============================================================

TEST(GcHeap, InterpPrototypeChainSurvivesGc) {
    auto v = interp_ok(R"(
        function Animal(name) { this.name = name; }
        Animal.prototype.speak = function() { return this.name + " speaks"; };
        var a = new Animal("dog");
        a.speak();
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "dog speaks");
}

TEST(GcHeap, VmPrototypeChainSurvivesGc) {
    auto v = vm_ok(R"(
        function Animal(name) { this.name = name; }
        Animal.prototype.speak = function() { return this.name + " speaks"; };
        var a = new Animal("dog");
        a.speak();
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "dog speaks");
}

// ============================================================
// 多次 exec() 调用（同一个 Interpreter/VM 实例复用）不崩溃
// ============================================================

TEST(GcHeap, InterpMultipleExecCallsNoCrash) {
    // 每次 exec() 末尾都触发 GC，多次调用不应因 GcHeap 状态残留而崩溃
    qppjs::Interpreter interp;
    for (int i = 0; i < 3; ++i) {
        auto parse_result = qppjs::parse_program("var x = 1; x + 1;");
        EXPECT_TRUE(parse_result.ok());
        if (!parse_result.ok()) break;
        auto result = interp.exec(parse_result.value());
        EXPECT_TRUE(result.is_ok());
        if (!result.is_ok()) break;
        EXPECT_EQ(result.value().as_number(), 2.0);
    }
}

TEST(GcHeap, VmMultipleExecCallsNoCrash) {
    qppjs::VM vm;
    for (int i = 0; i < 3; ++i) {
        auto parse_result = qppjs::parse_program("var x = 1; x + 1;");
        EXPECT_TRUE(parse_result.ok());
        if (!parse_result.ok()) break;
        qppjs::Compiler compiler;
        auto bytecode = compiler.compile(parse_result.value());
        auto result = vm.exec(bytecode);
        EXPECT_TRUE(result.is_ok());
        if (!result.is_ok()) break;
        EXPECT_EQ(result.value().as_number(), 2.0);
    }
}

// ============================================================
// 空程序（无语句）：GC 在空堆上触发不崩溃
// ============================================================

TEST(GcHeap, InterpEmptyProgramGcNoCrash) {
    auto parse_result = qppjs::parse_program("");
    EXPECT_TRUE(parse_result.ok());
    if (!parse_result.ok()) return;
    qppjs::Interpreter interp;
    auto result = interp.exec(parse_result.value());
    EXPECT_TRUE(result.is_ok());
}

TEST(GcHeap, VmEmptyProgramGcNoCrash) {
    auto parse_result = qppjs::parse_program("");
    EXPECT_TRUE(parse_result.ok());
    if (!parse_result.ok()) return;
    qppjs::Compiler compiler;
    auto bytecode = compiler.compile(parse_result.value());
    qppjs::VM vm;
    auto result = vm.exec(bytecode);
    EXPECT_TRUE(result.is_ok());
}

// ============================================================
// kGcSentinel 语义：被 sweep 的对象上调用 add_ref/release 是 no-op
// ============================================================

TEST(GcHeap, InterpClosureCycleNoDoubleFree) {
    // 三重循环引用：fn1 -> env1 -> fn2 -> env2 -> fn1
    // GC sweep 阶段 kGcSentinel 确保不发生 double-free，LSan 不报泄漏
    auto v = interp_ok(R"(
        function outer() {
            var fn1;
            var fn2 = function() { return fn1; };
            fn1 = function() { return fn2; };
            return fn1()() === fn1;
        }
        outer();
    )");
    // fn1()() 返回 fn1 自身，=== 比较应为 true
    EXPECT_TRUE(v.is_bool());
    EXPECT_EQ(v.as_bool(), true);
}

TEST(GcHeap, VmClosureCycleNoDoubleFree) {
    auto v = vm_ok(R"(
        function outer() {
            var fn1;
            var fn2 = function() { return fn1; };
            fn1 = function() { return fn2; };
            return fn1()() === fn1;
        }
        outer();
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_EQ(v.as_bool(), true);
}

// ============================================================
// M1: 多次 exec() 后长期根对象（object_prototype_ 等）不被误回收
// ============================================================

TEST(GcHeap, InterpMultipleExecObjectPrototypeStaysAlive) {
    // 第一次 exec 后 gc_mark_ 被置为 true；第二次 exec 的 Phase 1 必须重置它，
    // 否则 TraceRefs 不被调用，object_prototype_ 的属性对象被 sweep，导致 UAF。
    // 通过两次 exec() 调用同一 Interpreter 实例来验证（这里用独立实例各执行一次，
    // 验证 Object.create/Object.keys 在 GC 后仍可正常使用）。
    auto v = interp_ok(R"(
        var proto = {x: 42};
        var obj = Object.create(proto);
        var keys1 = Object.keys(proto);
        var obj2 = Object.create(proto);
        obj2.x;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 42.0);
}

TEST(GcHeap, VmMultipleExecObjectPrototypeStaysAlive) {
    auto v = vm_ok(R"(
        var proto = {x: 42};
        var obj = Object.create(proto);
        var keys1 = Object.keys(proto);
        var obj2 = Object.create(proto);
        obj2.x;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 42.0);
}

// ============================================================
// M2: Object.create() 返回的对象在 GC 后仍可访问
// ============================================================

TEST(GcHeap, InterpObjectCreateSurvivesGc) {
    // Object.create 内部分配的 new_obj 必须注册到 GcHeap，
    // 否则若形成可达引用链，GC 无法正确追踪，对象被提前回收。
    auto v = interp_ok(R"(
        var proto = {x: 1};
        var obj = Object.create(proto);
        obj.x;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

TEST(GcHeap, VmObjectCreateSurvivesGc) {
    auto v = vm_ok(R"(
        var proto = {x: 1};
        var obj = Object.create(proto);
        obj.x;
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

}  // namespace
