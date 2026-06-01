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
// CF-01: 基础实例字段 x = 1 在 new 后可访问
// ============================================================

TEST(ClassFields, CF01_Interp_BasicInstanceField) {
    auto v = interp_ok(R"(
        class C { x = 1; }
        var c = new C();
        c.x
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

TEST(ClassFields, CF01_VM_BasicInstanceField) {
    auto v = vm_ok(R"(
        class C { x = 1; }
        var c = new C();
        c.x
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

// ============================================================
// CF-02: 多字段按顺序初始化
// ============================================================

TEST(ClassFields, CF02_Interp_MultipleFields) {
    auto v = interp_ok(R"(
        class C {
            x = 1;
            y = 2;
            z = 3;
        }
        var c = new C();
        c.x + c.y + c.z
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 6.0);
}

TEST(ClassFields, CF02_VM_MultipleFields) {
    auto v = vm_ok(R"(
        class C {
            x = 1;
            y = 2;
            z = 3;
        }
        var c = new C();
        c.x + c.y + c.z
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 6.0);
}

// ============================================================
// CF-03: 无初始化器的字段值为 undefined
// ============================================================

TEST(ClassFields, CF03_Interp_FieldWithoutInitializer) {
    auto v = interp_ok(R"(
        class C { x; }
        var c = new C();
        typeof c.x
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "undefined");
}

TEST(ClassFields, CF03_VM_FieldWithoutInitializer) {
    auto v = vm_ok(R"(
        class C { x; }
        var c = new C();
        typeof c.x
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "undefined");
}

// ============================================================
// CF-04: 字段初始化器可引用 this
// ============================================================

TEST(ClassFields, CF04_Interp_FieldInitRefThis) {
    auto v = interp_ok(R"(
        class C {
            x = 42;
            y = this.x + 1;
        }
        var c = new C();
        c.y
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 43.0);
}

TEST(ClassFields, CF04_VM_FieldInitRefThis) {
    auto v = vm_ok(R"(
        class C {
            x = 42;
            y = this.x + 1;
        }
        var c = new C();
        c.y
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 43.0);
}

// ============================================================
// CF-05: 字段初始化器可引用前面已设置的字段（通过 this）
// ============================================================

TEST(ClassFields, CF05_Interp_FieldInitRefPrevField) {
    auto v = interp_ok(R"(
        class C {
            a = 10;
            b = this.a * 2;
        }
        var c = new C();
        c.b
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 20.0);
}

TEST(ClassFields, CF05_VM_FieldInitRefPrevField) {
    auto v = vm_ok(R"(
        class C {
            a = 10;
            b = this.a * 2;
        }
        var c = new C();
        c.b
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 20.0);
}

// ============================================================
// CF-06: 字段是 own property（遮蔽 prototype 属性）
// ============================================================

TEST(ClassFields, CF06_Interp_FieldIsOwnProperty) {
    auto v = interp_ok(R"(
        class C {
            x = 99;
        }
        C.prototype.x = 1;
        var c = new C();
        c.hasOwnProperty('x') ? c.x : -1
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 99.0);
}

TEST(ClassFields, CF06_VM_FieldIsOwnProperty) {
    auto v = vm_ok(R"(
        class C {
            x = 99;
        }
        C.prototype.x = 1;
        var c = new C();
        c.hasOwnProperty('x') ? c.x : -1
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 99.0);
}

// ============================================================
// CF-07: 每次 new 独立初始化（互不干扰）
// ============================================================

TEST(ClassFields, CF07_Interp_IndependentInstances) {
    auto v = interp_ok(R"(
        class C { x = 0; }
        var a = new C();
        var b = new C();
        a.x = 10;
        b.x
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}

TEST(ClassFields, CF07_VM_IndependentInstances) {
    auto v = vm_ok(R"(
        class C { x = 0; }
        var a = new C();
        var b = new C();
        a.x = 10;
        b.x
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}

// ============================================================
// CF-08: 字段 enumerable=true（Object.keys 可见）
// ============================================================

TEST(ClassFields, CF08_Interp_FieldEnumerable) {
    auto v = interp_ok(R"(
        class C { x = 1; y = 2; }
        var c = new C();
        Object.keys(c).length
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.0);
}

TEST(ClassFields, CF08_VM_FieldEnumerable) {
    auto v = vm_ok(R"(
        class C { x = 1; y = 2; }
        var c = new C();
        Object.keys(c).length
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.0);
}

// ============================================================
// CF-09: 字段初始化在 constructor body 之前
// ============================================================

TEST(ClassFields, CF09_Interp_FieldBeforeConstructor) {
    auto v = interp_ok(R"(
        var order = [];
        class C {
            x = order.push('field');
            constructor() {
                order.push('ctor');
            }
        }
        new C();
        order[0]
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "field");
}

TEST(ClassFields, CF09_VM_FieldBeforeConstructor) {
    auto v = vm_ok(R"(
        var order = [];
        class C {
            x = order.push('field');
            constructor() {
                order.push('ctor');
            }
        }
        new C();
        order[0]
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "field");
}

// ============================================================
// CF-10: static 字段在 class 本身（不在 prototype 上）
// ============================================================

TEST(ClassFields, CF10_Interp_StaticFieldOnClass) {
    auto v = interp_ok(R"(
        class C { static x = 42; }
        C.x
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 42.0);
}

TEST(ClassFields, CF10_VM_StaticFieldOnClass) {
    auto v = vm_ok(R"(
        class C { static x = 42; }
        C.x
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 42.0);
}

// ============================================================
// CF-11: static 字段无初始化器 → undefined
// ============================================================

TEST(ClassFields, CF11_Interp_StaticFieldNoInit) {
    auto v = interp_ok(R"(
        class C { static x; }
        typeof C.x
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "undefined");
}

TEST(ClassFields, CF11_VM_StaticFieldNoInit) {
    auto v = vm_ok(R"(
        class C { static x; }
        typeof C.x
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "undefined");
}

// ============================================================
// CF-12: static 字段在 new 之前就存在
// ============================================================

TEST(ClassFields, CF12_Interp_StaticFieldBeforeNew) {
    auto v = interp_ok(R"(
        class C { static count = 0; }
        C.count
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}

TEST(ClassFields, CF12_VM_StaticFieldBeforeNew) {
    auto v = vm_ok(R"(
        class C { static count = 0; }
        C.count
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}

// ============================================================
// CF-13: static 字段初始化可引用外部变量
// ============================================================

TEST(ClassFields, CF13_Interp_StaticFieldRefOuter) {
    auto v = interp_ok(R"(
        var base = 100;
        class C { static x = base + 1; }
        C.x
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 101.0);
}

TEST(ClassFields, CF13_VM_StaticFieldRefOuter) {
    auto v = vm_ok(R"(
        var base = 100;
        class C { static x = base + 1; }
        C.x
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 101.0);
}

// ============================================================
// CF-14: 子类有独立的 static 字段（不共享）
// ============================================================

TEST(ClassFields, CF14_Interp_SubclassIndependentStatic) {
    auto v = interp_ok(R"(
        class A { static x = 1; }
        class B extends A { static x = 2; }
        A.x + B.x
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 3.0);
}

TEST(ClassFields, CF14_VM_SubclassIndependentStatic) {
    auto v = vm_ok(R"(
        class A { static x = 1; }
        class B extends A { static x = 2; }
        A.x + B.x
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 3.0);
}

// ============================================================
// CF-15: 子类字段在父类 super() 之后初始化
// ============================================================

TEST(ClassFields, CF15_Interp_DerivedFieldAfterSuper) {
    auto v = interp_ok(R"(
        class A {
            constructor() { this.base = 10; }
        }
        class B extends A {
            extra = 20;
        }
        var b = new B();
        b.base + b.extra
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 30.0);
}

TEST(ClassFields, CF15_VM_DerivedFieldAfterSuper) {
    auto v = vm_ok(R"(
        class A {
            constructor() { this.base = 10; }
        }
        class B extends A {
            extra = 20;
        }
        var b = new B();
        b.base + b.extra
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 30.0);
}

// ============================================================
// CF-16: 子类字段覆盖父类同名字段（子类后初始化）
// ============================================================

TEST(ClassFields, CF16_Interp_DerivedFieldOverridesBase) {
    auto v = interp_ok(R"(
        class A { x = 1; }
        class B extends A { x = 2; }
        var b = new B();
        b.x
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.0);
}

TEST(ClassFields, CF16_VM_DerivedFieldOverridesBase) {
    auto v = vm_ok(R"(
        class A { x = 1; }
        class B extends A { x = 2; }
        var b = new B();
        b.x
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.0);
}

// ============================================================
// CF-17: derived class 的字段初始化在 constructor 中 super() 之后
// ============================================================

TEST(ClassFields, CF17_Interp_DerivedFieldInitAfterSuperInCtor) {
    auto v = interp_ok(R"(
        var order = [];
        class A {
            constructor() { order.push('A'); }
        }
        class B extends A {
            y = order.push('field');
            constructor() {
                super();
                order.push('B');
            }
        }
        new B();
        order.join(',')
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "A,field,B");
}

TEST(ClassFields, CF17_VM_DerivedFieldInitAfterSuperInCtor) {
    auto v = vm_ok(R"(
        var order = [];
        class A {
            constructor() { order.push('A'); }
        }
        class B extends A {
            y = order.push('field');
            constructor() {
                super();
                order.push('B');
            }
        }
        new B();
        order.join(',')
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "A,field,B");
}

// ============================================================
// Extra: 字段初始化器可引用外部变量（closure）
// ============================================================

TEST(ClassFields, CFExtra01_Interp_FieldRefClosure) {
    auto v = interp_ok(R"(
        var n = 7;
        class C { x = n * 2; }
        new C().x
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 14.0);
}

TEST(ClassFields, CFExtra01_VM_FieldRefClosure) {
    auto v = vm_ok(R"(
        var n = 7;
        class C { x = n * 2; }
        new C().x
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 14.0);
}

// ============================================================
// Extra: 字段与 constructor 共存
// ============================================================

TEST(ClassFields, CFExtra02_Interp_FieldAndCtor) {
    auto v = interp_ok(R"(
        class C {
            x = 1;
            constructor(v) { this.y = v; }
        }
        var c = new C(5);
        c.x + c.y
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 6.0);
}

TEST(ClassFields, CFExtra02_VM_FieldAndCtor) {
    auto v = vm_ok(R"(
        class C {
            x = 1;
            constructor(v) { this.y = v; }
        }
        var c = new C(5);
        c.x + c.y
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 6.0);
}

}  // namespace
