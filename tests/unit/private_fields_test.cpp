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

// ============================================================
// PF-01: 基础私有字段声明和读写
// ============================================================

TEST(PrivateFields, PF01_Interp_BasicReadWrite) {
    auto v = interp_ok(R"(
        class Counter {
            #count = 0;
            increment() { this.#count += 1; }
            get() { return this.#count; }
        }
        var c = new Counter();
        c.increment();
        c.increment();
        c.get()
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.0);
}

TEST(PrivateFields, PF01_VM_BasicReadWrite) {
    auto v = vm_ok(R"(
        class Counter {
            #count = 0;
            increment() { this.#count += 1; }
            get() { return this.#count; }
        }
        var c = new Counter();
        c.increment();
        c.increment();
        c.get()
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.0);
}

// ============================================================
// PF-02: 私有字段初始化器
// ============================================================

TEST(PrivateFields, PF02_Interp_Initializer) {
    auto v = interp_ok(R"(
        class Point {
            #x = 10;
            #y = 20;
            sum() { return this.#x + this.#y; }
        }
        new Point().sum()
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 30.0);
}

TEST(PrivateFields, PF02_VM_Initializer) {
    auto v = vm_ok(R"(
        class Point {
            #x = 10;
            #y = 20;
            sum() { return this.#x + this.#y; }
        }
        new Point().sum()
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 30.0);
}

// ============================================================
// PF-03: 私有字段无初始化器默认为 undefined
// ============================================================

TEST(PrivateFields, PF03_Interp_NoInitializer) {
    auto v = interp_ok(R"(
        class C {
            #x;
            get() { return this.#x; }
        }
        new C().get()
    )");
    EXPECT_TRUE(v.is_undefined());
}

TEST(PrivateFields, PF03_VM_NoInitializer) {
    auto v = vm_ok(R"(
        class C {
            #x;
            get() { return this.#x; }
        }
        new C().get()
    )");
    EXPECT_TRUE(v.is_undefined());
}

// ============================================================
// PF-04: 私有字段不被外部访问（TypeError）
// ============================================================

TEST(PrivateFields, PF04_Interp_NotAccessibleFromOutside) {
    // Accessing #x from outside the class via PrivateMemberExpression
    // Parser parses obj.#x only in valid class body context?
    // Actually the parser allows it anywhere. The interpreter throws when sym_id not found.
    // We test that accessing an undeclared private field throws TypeError.
    EXPECT_TRUE(interp_throws(R"(
        class C { #x = 1; }
        var c = new C();
        class D {
            test(obj) { return obj.#x; }  // #x not in D's scope
        }
        new D().test(c)
    )"));
}

TEST(PrivateFields, PF04_VM_NotAccessibleFromOutside) {
    EXPECT_TRUE(vm_throws(R"(
        class C { #x = 1; }
        var c = new C();
        class D {
            test(obj) { return obj.#x; }  // #x not in D's scope
        }
        new D().test(c)
    )"));
}

// ============================================================
// PF-05: 私有字段 brand check (#x in obj)
// ============================================================

TEST(PrivateFields, PF05_Interp_BrandCheck) {
    auto v = interp_ok(R"(
        class C {
            #x = 1;
            static isC(obj) { return #x in obj; }
        }
        var c = new C();
        var notC = {};
        C.isC(c) + "," + C.isC(notC)
    )");
    EXPECT_EQ(v.sv(), "true,false");
}

TEST(PrivateFields, PF05_VM_BrandCheck) {
    auto v = vm_ok(R"(
        class C {
            #x = 1;
            static isC(obj) { return #x in obj; }
        }
        var c = new C();
        var notC = {};
        C.isC(c) + "," + C.isC(notC)
    )");
    EXPECT_EQ(v.sv(), "true,false");
}

// ============================================================
// PF-06: 私有字段在 constructor 中初始化
// ============================================================

TEST(PrivateFields, PF06_Interp_ConstructorInit) {
    auto v = interp_ok(R"(
        class C {
            #value;
            constructor(v) { this.#value = v; }
            get() { return this.#value; }
        }
        new C(42).get()
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 42.0);
}

TEST(PrivateFields, PF06_VM_ConstructorInit) {
    auto v = vm_ok(R"(
        class C {
            #value;
            constructor(v) { this.#value = v; }
            get() { return this.#value; }
        }
        new C(42).get()
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 42.0);
}

// ============================================================
// PF-07: 多个实例各自独立的私有字段
// ============================================================

TEST(PrivateFields, PF07_Interp_PerInstanceFields) {
    auto v = interp_ok(R"(
        class C {
            #x;
            constructor(v) { this.#x = v; }
            get() { return this.#x; }
        }
        var a = new C(1);
        var b = new C(2);
        a.get() + "," + b.get()
    )");
    EXPECT_EQ(v.sv(), "1,2");
}

TEST(PrivateFields, PF07_VM_PerInstanceFields) {
    auto v = vm_ok(R"(
        class C {
            #x;
            constructor(v) { this.#x = v; }
            get() { return this.#x; }
        }
        var a = new C(1);
        var b = new C(2);
        a.get() + "," + b.get()
    )");
    EXPECT_EQ(v.sv(), "1,2");
}

// ============================================================
// PF-08: 私有字段不出现在 Object.keys 中
// ============================================================

TEST(PrivateFields, PF08_Interp_NotInObjectKeys) {
    auto v = interp_ok(R"(
        class C {
            #secret = 42;
            pub = 1;
        }
        var c = new C();
        Object.keys(c).length
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);  // only "pub"
}

TEST(PrivateFields, PF08_VM_NotInObjectKeys) {
    auto v = vm_ok(R"(
        class C {
            #secret = 42;
            pub = 1;
        }
        var c = new C();
        Object.keys(c).length
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

// ============================================================
// PF-09: 继承类中各自独立的私有字段
// ============================================================

TEST(PrivateFields, PF09_Interp_InheritanceIsolation) {
    auto v = interp_ok(R"(
        class A {
            #x = 10;
            getX() { return this.#x; }
        }
        class B extends A {
            #y = 20;
            getY() { return this.#y; }
        }
        var b = new B();
        b.getX() + "," + b.getY()
    )");
    EXPECT_EQ(v.sv(), "10,20");
}

TEST(PrivateFields, PF09_VM_InheritanceIsolation) {
    auto v = vm_ok(R"(
        class A {
            #x = 10;
            getX() { return this.#x; }
        }
        class B extends A {
            #y = 20;
            getY() { return this.#y; }
        }
        var b = new B();
        b.getX() + "," + b.getY()
    )");
    EXPECT_EQ(v.sv(), "10,20");
}

// ============================================================
// PF-10: 静态私有字段（不在 instance fields 中）
// ============================================================

TEST(PrivateFields, PF10_Interp_StaticPrivateField) {
    auto v = interp_ok(R"(
        class C {
            static #count = 0;
            static increment() { C.#count++; }
            static get() { return C.#count; }
        }
        C.increment();
        C.increment();
        C.get()
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.0);
}

TEST(PrivateFields, PF10_VM_StaticPrivateField) {
    auto v = vm_ok(R"(
        class C {
            static #count = 0;
            static increment() { C.#count++; }
            static get() { return C.#count; }
        }
        C.increment();
        C.increment();
        C.get()
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.0);
}

}  // namespace
