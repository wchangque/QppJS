#include "qppjs/frontend/parser.h"
#include "qppjs/runtime/interpreter.h"

#include <gtest/gtest.h>

#include <string_view>

namespace {

qppjs::Value interp_ok(std::string_view source) {
    auto parse_result = qppjs::parse_program(source);
    EXPECT_TRUE(parse_result.ok()) << "parse failed: " << parse_result.error().message();
    qppjs::Interpreter interp;
    auto result = interp.exec(parse_result.value());
    EXPECT_TRUE(result.is_ok()) << "exec failed: " << result.error().message();
    return result.value();
}

// ============================================================
// T-01: Basic construction
// ============================================================

TEST(InterpErrorSubclass, TypeErrorInstanceofTypeError) {
    auto v = interp_ok("new TypeError('bad type') instanceof TypeError");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(InterpErrorSubclass, TypeErrorInstanceofError) {
    auto v = interp_ok("new TypeError('bad type') instanceof Error");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(InterpErrorSubclass, TypeErrorNotInstanceofRangeError) {
    auto v = interp_ok("new TypeError('bad type') instanceof RangeError");
    EXPECT_TRUE(v.is_bool());
    EXPECT_FALSE(v.as_bool());
}

TEST(InterpErrorSubclass, TypeErrorName) {
    auto v = interp_ok("new TypeError('bad type').name");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "TypeError");
}

TEST(InterpErrorSubclass, TypeErrorMessage) {
    auto v = interp_ok("new TypeError('bad type').message");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "bad type");
}

// ============================================================
// T-02: No-argument construction
// ============================================================

TEST(InterpErrorSubclass, TypeErrorNoArgMessage) {
    auto v = interp_ok("new TypeError().message");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "");
}

TEST(InterpErrorSubclass, TypeErrorNoArgName) {
    auto v = interp_ok("new TypeError().name");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "TypeError");
}

// ============================================================
// T-03: RangeError
// ============================================================

TEST(InterpErrorSubclass, RangeErrorInstanceofRangeError) {
    auto v = interp_ok("new RangeError('out of range') instanceof RangeError");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(InterpErrorSubclass, RangeErrorInstanceofError) {
    auto v = interp_ok("new RangeError('out of range') instanceof Error");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// T-04: instanceof edge cases
// ============================================================

TEST(InterpInstanceof, NullInstanceofError) {
    auto v = interp_ok("null instanceof Error");
    EXPECT_TRUE(v.is_bool());
    EXPECT_FALSE(v.as_bool());
}

TEST(InterpInstanceof, NumberInstanceofError) {
    auto v = interp_ok("42 instanceof Error");
    EXPECT_TRUE(v.is_bool());
    EXPECT_FALSE(v.as_bool());
}

TEST(InterpInstanceof, UserDefinedClass) {
    auto v = interp_ok(R"(
        function Foo() {}
        let obj = new Foo();
        obj instanceof Foo
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// T-05: ReferenceError subclass
// ============================================================

TEST(InterpErrorSubclass, ReferenceErrorInstanceofReferenceError) {
    auto v = interp_ok("new ReferenceError('x is not defined') instanceof ReferenceError");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(InterpErrorSubclass, ReferenceErrorInstanceofError) {
    auto v = interp_ok("new ReferenceError('x is not defined') instanceof Error");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// T-06: Error base class
// ============================================================

TEST(InterpErrorSubclass, ErrorBaseInstanceofError) {
    auto v = interp_ok("new Error('msg') instanceof Error");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(InterpErrorSubclass, ErrorBaseName) {
    auto v = interp_ok("new Error('msg').name");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "Error");
}

TEST(InterpErrorSubclass, ErrorBaseMessage) {
    auto v = interp_ok("new Error('msg').message");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "msg");
}

// ============================================================
// T-07: RangeError manual throw and catch
// ============================================================

TEST(InterpErrorSubclass, RangeErrorThrowCatchInstanceof) {
    auto v = interp_ok(R"(
        let r;
        try { throw new RangeError('r'); } catch(e) { r = e instanceof RangeError; }
        r
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(InterpErrorSubclass, RangeErrorThrowCatchMessage) {
    auto v = interp_ok(R"(
        let m;
        try { throw new RangeError('out'); } catch(e) { m = e.message; }
        m
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "out");
}

// ============================================================
// T-08: TypeError called as ordinary function (no new)
// ============================================================

TEST(InterpErrorSubclass, TypeErrorCallWithoutNewInstanceofTypeError) {
    auto v = interp_ok("TypeError('x') instanceof TypeError");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(InterpErrorSubclass, TypeErrorCallWithoutNewMessage) {
    auto v = interp_ok("TypeError('hello').message");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "hello");
}

// ============================================================
// T-09: Cross-subclass instanceof isolation
// ============================================================

TEST(InterpErrorSubclass, TypeErrorNotInstanceofReferenceError) {
    auto v = interp_ok("new TypeError('t') instanceof ReferenceError");
    EXPECT_TRUE(v.is_bool());
    EXPECT_FALSE(v.as_bool());
}

TEST(InterpErrorSubclass, ReferenceErrorNotInstanceofTypeError) {
    auto v = interp_ok("new ReferenceError('r') instanceof TypeError");
    EXPECT_TRUE(v.is_bool());
    EXPECT_FALSE(v.as_bool());
}

TEST(InterpErrorSubclass, RangeErrorNotInstanceofTypeError) {
    auto v = interp_ok("new RangeError('r') instanceof TypeError");
    EXPECT_TRUE(v.is_bool());
    EXPECT_FALSE(v.as_bool());
}

// ============================================================
// T-10: TypeError.prototype instanceof Error
// ============================================================

TEST(InterpInstanceof, TypeErrorPrototypeInstanceofError) {
    auto v = interp_ok("TypeError.prototype instanceof Error");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(InterpInstanceof, RangeErrorPrototypeInstanceofError) {
    auto v = interp_ok("RangeError.prototype instanceof Error");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// T-11: instanceof right-hand side non-function throws TypeError
// ============================================================

TEST(InterpInstanceof, RhsNumberThrowsTypeError) {
    // Left side must be an object to reach the right-side callable check
    auto v = interp_ok("let e; try { ({}) instanceof 42; } catch(err) { e = err instanceof TypeError; } e");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(InterpInstanceof, RhsStringThrowsTypeError) {
    auto v = interp_ok("let e; try { ({}) instanceof 'str'; } catch(err) { e = err instanceof TypeError; } e");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// T-12: undefined.x / null.x triggers TypeError instance
// ============================================================

TEST(InterpErrorSubclass, UndefinedPropertyAccessIsTypeError) {
    auto v = interp_ok(R"(
        let ok;
        try { undefined.x; } catch(e) { ok = e instanceof TypeError; }
        ok
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(InterpErrorSubclass, NullPropertyAccessIsTypeError) {
    auto v = interp_ok(R"(
        let ok;
        try { null.x; } catch(e) { ok = e instanceof TypeError; }
        ok
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// T-13: Runtime ReferenceError / TypeError are Error instances
// ============================================================

TEST(InterpErrorSubclass, UndeclaredVarIsReferenceError) {
    auto v = interp_ok(R"(
        let ok;
        try { notDeclared; } catch(e) { ok = e instanceof ReferenceError; }
        ok
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(InterpErrorSubclass, CallNonFunctionIsTypeError) {
    auto v = interp_ok(R"(
        let ok;
        try { (42)(); } catch(e) { ok = e instanceof TypeError; }
        ok
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(InterpErrorSubclass, TDZAccessIsReferenceError) {
    auto v = interp_ok(R"(
        let ok;
        try { let x = x; } catch(e) { ok = e instanceof ReferenceError; }
        ok
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// T-14: Error.prototype is not an instance of Error
// ============================================================

TEST(InterpInstanceof, ErrorPrototypeNotInstanceofError) {
    auto v = interp_ok("Error.prototype instanceof Error");
    EXPECT_TRUE(v.is_bool());
    EXPECT_FALSE(v.as_bool());
}

// ============================================================
// T-15: instanceof right-side plain object throws TypeError
// ============================================================

TEST(InterpInstanceof, RhsPlainObjectThrowsTypeError) {
    auto v = interp_ok("let e; try { ({}) instanceof {}; } catch(err) { e = err instanceof TypeError; } e");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// T-16: Nested try/catch with manually thrown Error objects
// ============================================================

TEST(InterpErrorSubclass, NestedTryCatchRethrowPreservesType) {
    // Manually thrown Error objects (not runtime errors) use pending_throw_ path
    // and preserve their type through rethrow
    auto v = interp_ok(R"(
        let r1 = false;
        let r2 = false;
        try {
            try {
                throw new TypeError('inner');
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

// ============================================================
// T-17: M2 — Error.prototype.constructor should be the constructor function
// ============================================================

TEST(InterpErrorSubclass, ErrorPrototypeConstructorIsError) {
    auto v = interp_ok("Error.prototype.constructor === Error");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(InterpErrorSubclass, TypeErrorPrototypeConstructorIsTypeError) {
    auto v = interp_ok("TypeError.prototype.constructor === TypeError");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(InterpErrorSubclass, NewTypeErrorConstructorIsTypeError) {
    auto v = interp_ok("new TypeError('x').constructor === TypeError");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

}  // namespace
