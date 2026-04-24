#include "qppjs/frontend/parser.h"
#include "qppjs/vm/compiler.h"
#include "qppjs/vm/vm.h"

#include <gtest/gtest.h>

#include <string_view>

namespace {

qppjs::Value vm_ok(std::string_view source) {
    auto parse_result = qppjs::parse_program(source);
    EXPECT_TRUE(parse_result.ok()) << "parse failed: " << parse_result.error().message();
    qppjs::Compiler compiler;
    auto bytecode = compiler.compile(parse_result.value());
    qppjs::VM vm;
    auto result = vm.exec(bytecode);
    EXPECT_TRUE(result.is_ok()) << "exec failed: " << result.error().message();
    return result.value();
}

// ============================================================
// T-01: Basic construction
// ============================================================

TEST(VMErrorSubclass, TypeErrorInstanceofTypeError) {
    auto v = vm_ok("new TypeError('bad type') instanceof TypeError");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(VMErrorSubclass, TypeErrorInstanceofError) {
    auto v = vm_ok("new TypeError('bad type') instanceof Error");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(VMErrorSubclass, TypeErrorNotInstanceofRangeError) {
    auto v = vm_ok("new TypeError('bad type') instanceof RangeError");
    EXPECT_TRUE(v.is_bool());
    EXPECT_FALSE(v.as_bool());
}

TEST(VMErrorSubclass, TypeErrorName) {
    auto v = vm_ok("new TypeError('bad type').name");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "TypeError");
}

TEST(VMErrorSubclass, TypeErrorMessage) {
    auto v = vm_ok("new TypeError('bad type').message");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "bad type");
}

// ============================================================
// T-02: Runtime errors trigger proper Error instances
// ============================================================

TEST(VMErrorSubclass, NullPropertyAccessIsTypeError) {
    auto v = vm_ok("let e; try { null.x; } catch(err) { e = err instanceof TypeError; } e");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(VMErrorSubclass, CallNonFunctionIsTypeError) {
    auto v = vm_ok("let e; try { (42)(); } catch(err) { e = err instanceof TypeError; } e");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(VMErrorSubclass, UndeclaredVarIsReferenceError) {
    auto v = vm_ok("let e; try { notDefined; } catch(err) { e = err instanceof ReferenceError; } e");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// T-03: RangeError
// ============================================================

TEST(VMErrorSubclass, RangeErrorInstanceofRangeError) {
    auto v = vm_ok("new RangeError('out of range') instanceof RangeError");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(VMErrorSubclass, RangeErrorInstanceofError) {
    auto v = vm_ok("new RangeError('out of range') instanceof Error");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// T-04: No-argument construction
// ============================================================

TEST(VMErrorSubclass, TypeErrorNoArgMessage) {
    auto v = vm_ok("new TypeError().message");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "");
}

TEST(VMErrorSubclass, TypeErrorNoArgName) {
    auto v = vm_ok("new TypeError().name");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "TypeError");
}

// ============================================================
// T-05: instanceof edge cases
// ============================================================

TEST(VMInstanceof, NullInstanceofError) {
    auto v = vm_ok("null instanceof Error");
    EXPECT_TRUE(v.is_bool());
    EXPECT_FALSE(v.as_bool());
}

TEST(VMInstanceof, NumberInstanceofError) {
    auto v = vm_ok("42 instanceof Error");
    EXPECT_TRUE(v.is_bool());
    EXPECT_FALSE(v.as_bool());
}

TEST(VMInstanceof, PlainObjectNotInstanceofError) {
    auto v = vm_ok("({}) instanceof Error");
    EXPECT_TRUE(v.is_bool());
    EXPECT_FALSE(v.as_bool());
}

TEST(VMInstanceof, UserDefinedClass) {
    auto v = vm_ok(R"(
        function Foo() {}
        let obj = new Foo();
        obj instanceof Foo
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(VMInstanceof, UserDefinedClassNegative) {
    auto v = vm_ok(R"(
        function Foo() {}
        function Bar() {}
        let obj = new Foo();
        obj instanceof Bar
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_FALSE(v.as_bool());
}

// ============================================================
// T-06: ReferenceError subclass
// ============================================================

TEST(VMErrorSubclass, ReferenceErrorInstanceofReferenceError) {
    auto v = vm_ok("new ReferenceError('x is not defined') instanceof ReferenceError");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(VMErrorSubclass, ReferenceErrorInstanceofError) {
    auto v = vm_ok("new ReferenceError('x is not defined') instanceof Error");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(VMErrorSubclass, ReferenceErrorMessage) {
    auto v = vm_ok("new ReferenceError('x is not defined').message");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "x is not defined");
}

// ============================================================
// T-07: Caught runtime errors have correct name via inheritance
// ============================================================

TEST(VMErrorSubclass, CaughtTypeErrorHasNameFromProto) {
    auto v = vm_ok("let n; try { null.x; } catch(e) { n = e.name; } n");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "TypeError");
}

TEST(VMErrorSubclass, CaughtReferenceErrorHasNameFromProto) {
    auto v = vm_ok("let n; try { notDefined; } catch(e) { n = e.name; } n");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "ReferenceError");
}

// ============================================================
// T-08: Error base class
// ============================================================

TEST(VMErrorSubclass, ErrorBaseInstanceofError) {
    auto v = vm_ok("new Error('msg') instanceof Error");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(VMErrorSubclass, ErrorBaseName) {
    auto v = vm_ok("new Error('msg').name");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "Error");
}

TEST(VMErrorSubclass, ErrorBaseMessage) {
    auto v = vm_ok("new Error('msg').message");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "msg");
}

// ============================================================
// T-09: undefined.x triggers TypeError (same as null.x)
// ============================================================

TEST(VMErrorSubclass, UndefinedPropertyAccessIsTypeError) {
    auto v = vm_ok("let e; try { undefined.x; } catch(err) { e = err instanceof TypeError; } e");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// T-10: RangeError manual throw and catch
// ============================================================

TEST(VMErrorSubclass, RangeErrorThrowCatchInstanceof) {
    auto v = vm_ok(R"(
        let r;
        try { throw new RangeError('r'); } catch(e) { r = e instanceof RangeError; }
        r
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(VMErrorSubclass, RangeErrorThrowCatchMessage) {
    auto v = vm_ok(R"(
        let m;
        try { throw new RangeError('out'); } catch(e) { m = e.message; }
        m
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "out");
}

// ============================================================
// T-11: Runtime error message is non-empty
// ============================================================

TEST(VMErrorSubclass, NullPropertyAccessMessageNonEmpty) {
    // e.message is a non-empty string; verify it is not the empty string
    auto v = vm_ok("let ok; try { null.x; } catch(e) { ok = e.message !== ''; } ok");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(VMErrorSubclass, UndeclaredVarMessageNonEmpty) {
    auto v = vm_ok("let ok; try { notDefined; } catch(e) { ok = e.message !== ''; } ok");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// T-12: instanceof right-hand side non-function throws TypeError
// ============================================================

TEST(VMInstanceof, RhsNumberThrowsTypeError) {
    // Left side must be an object to reach the right-side callable check
    auto v = vm_ok("let e; try { ({}) instanceof 42; } catch(err) { e = err instanceof TypeError; } e");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(VMInstanceof, RhsStringThrowsTypeError) {
    auto v = vm_ok("let e; try { ({}) instanceof 'str'; } catch(err) { e = err instanceof TypeError; } e");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// T-13: TypeError called as ordinary function (no new)
// ============================================================

TEST(VMErrorSubclass, TypeErrorCallWithoutNewInstanceofTypeError) {
    auto v = vm_ok("TypeError('x') instanceof TypeError");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(VMErrorSubclass, TypeErrorCallWithoutNewMessage) {
    auto v = vm_ok("TypeError('hello').message");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "hello");
}

// ============================================================
// T-14: Cross-subclass instanceof isolation
// ============================================================

TEST(VMErrorSubclass, TypeErrorNotInstanceofReferenceError) {
    auto v = vm_ok("new TypeError('t') instanceof ReferenceError");
    EXPECT_TRUE(v.is_bool());
    EXPECT_FALSE(v.as_bool());
}

TEST(VMErrorSubclass, ReferenceErrorNotInstanceofTypeError) {
    auto v = vm_ok("new ReferenceError('r') instanceof TypeError");
    EXPECT_TRUE(v.is_bool());
    EXPECT_FALSE(v.as_bool());
}

TEST(VMErrorSubclass, RangeErrorNotInstanceofTypeError) {
    auto v = vm_ok("new RangeError('r') instanceof TypeError");
    EXPECT_TRUE(v.is_bool());
    EXPECT_FALSE(v.as_bool());
}

// ============================================================
// T-15: TypeError.prototype instanceof Error
// ============================================================

TEST(VMInstanceof, TypeErrorPrototypeInstanceofError) {
    auto v = vm_ok("TypeError.prototype instanceof Error");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(VMInstanceof, RangeErrorPrototypeInstanceofError) {
    auto v = vm_ok("RangeError.prototype instanceof Error");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// T-16: new non-constructor throws TypeError (VM only)
// ============================================================

TEST(VMErrorSubclass, NewNumberThrowsTypeError) {
    auto v = vm_ok("let e; try { new 42(); } catch(err) { e = err instanceof TypeError; } e");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// T-17: TDZ access throws ReferenceError object
// ============================================================

TEST(VMErrorSubclass, TDZAccessIsReferenceError) {
    // let x = x: rhs reads x before initialization — TDZ violation
    auto v = vm_ok("let e; try { let x = x; } catch(err) { e = err instanceof ReferenceError; } e");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(VMErrorSubclass, TDZAccessErrorName) {
    auto v = vm_ok("let e; try { let x = x; } catch(err) { e = err.name; } e");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "ReferenceError");
}

TEST(VMErrorSubclass, TDZAccessErrorMessageNonEmpty) {
    auto v = vm_ok("let e; try { let x = x; } catch(err) { e = err.message; } e");
    EXPECT_TRUE(v.is_string());
    EXPECT_FALSE(v.as_string().empty());
}

// ============================================================
// T-18: Nested try/catch preserves instanceof across rethrow
// ============================================================

TEST(VMErrorSubclass, NestedTryCatchRethrowPreservesType) {
    auto v = vm_ok(R"(
        let r1 = false;
        let r2 = false;
        try {
            try {
                null.x;
            } catch(e1) {
                r1 = e1 instanceof TypeError;
                throw e1;
            }
        } catch(e2) {
            r2 = e2 instanceof TypeError;
        }
        r1 && r2
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(VMErrorSubclass, NestedTryCatchDifferentErrorTypes) {
    auto v = vm_ok(R"(
        let typeErr = false;
        let refErr = false;
        try {
            null.x;
        } catch(e) {
            typeErr = e instanceof TypeError;
        }
        try {
            notDeclared;
        } catch(e) {
            refErr = e instanceof ReferenceError;
        }
        typeErr && refErr
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// T-19: Error.prototype is not an instance of Error
// ============================================================

TEST(VMInstanceof, ErrorPrototypeNotInstanceofError) {
    // Error.prototype's proto is object_prototype_, not Error.prototype itself
    auto v = vm_ok("Error.prototype instanceof Error");
    EXPECT_TRUE(v.is_bool());
    EXPECT_FALSE(v.as_bool());
}

// ============================================================
// T-20: instanceof right-side plain object throws TypeError
// ============================================================

TEST(VMInstanceof, RhsPlainObjectThrowsTypeError) {
    auto v = vm_ok("let e; try { ({}) instanceof {}; } catch(err) { e = err instanceof TypeError; } e");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// T-21: M1 — runtime error message must NOT contain "XxxError: " prefix
// Verified by checking message !== the full "ReferenceError: ..." string
// ============================================================

TEST(VMErrorSubclass, UndeclaredVarMessageNoPrefix) {
    // e.message should be "notDefined is not defined", not "ReferenceError: notDefined is not defined"
    auto v = vm_ok(R"(
        let ok;
        try { notDefined; } catch(e) {
            ok = e.message !== 'ReferenceError: notDefined is not defined';
        }
        ok
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(VMErrorSubclass, TDZMessageNoPrefix) {
    // e.message should not start with "ReferenceError: "
    // Verify by checking it equals the bare message (without prefix)
    auto v = vm_ok(R"(
        let ok;
        try { let x = x; } catch(e) {
            ok = e.message === "Cannot access 'x' before initialization";
        }
        ok
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// T-22: M2 — Error.prototype.constructor should be Error
// ============================================================

TEST(VMErrorSubclass, ErrorPrototypeConstructorIsError) {
    auto v = vm_ok("Error.prototype.constructor === Error");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(VMErrorSubclass, TypeErrorPrototypeConstructorIsTypeError) {
    auto v = vm_ok("TypeError.prototype.constructor === TypeError");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(VMErrorSubclass, ReferenceErrorPrototypeConstructorIsReferenceError) {
    auto v = vm_ok("ReferenceError.prototype.constructor === ReferenceError");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(VMErrorSubclass, NewTypeErrorConstructorIsTypeError) {
    auto v = vm_ok("new TypeError('x').constructor === TypeError");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

}  // namespace
