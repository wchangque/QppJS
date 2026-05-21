#include "qppjs/frontend/ast_dump.h"
#include "qppjs/frontend/parser.h"
#include "qppjs/runtime/interpreter.h"
#include "qppjs/vm/compiler.h"
#include "qppjs/vm/vm.h"

#include <gtest/gtest.h>

#include <string_view>

using namespace qppjs;

namespace {

// ============================================================
// Helper functions
// ============================================================

static bool parse_ok(std::string_view source) {
    auto result = parse_program(source);
    return result.ok();
}

static std::string dump(std::string_view source) {
    auto result = parse_program(source);
    if (!result.ok()) return "parse error: " + std::string(result.error().message());
    return dump_program(result.value());
}

static Value interp_ok(std::string_view source) {
    auto parse_result = parse_program(source);
    EXPECT_TRUE(parse_result.ok()) << "parse failed: " << parse_result.error().message();
    Interpreter interp;
    auto result = interp.exec(parse_result.value());
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
// FO-01: Parser — for (var x of [])
// ============================================================

TEST(ForOfParser, FO01_ParseVarOf) {
    EXPECT_TRUE(parse_ok("for (var x of []) {}"));
}

TEST(ForOfParser, FO01_ParseLetOf) {
    EXPECT_TRUE(parse_ok("for (let x of []) {}"));
}

TEST(ForOfParser, FO01_ParseConstOf) {
    EXPECT_TRUE(parse_ok("for (const x of []) {}"));
}

TEST(ForOfParser, FO01_ParseNoDecl) {
    EXPECT_TRUE(parse_ok("var x; for (x of []) {}"));
}

// ============================================================
// FO-02: Parser dump
// ============================================================

TEST(ForOfParser, FO02_DumpVarOf) {
    std::string d = dump("for (var x of [1,2]) {}");
    EXPECT_NE(d.find("ForOfStatement"), std::string::npos);
    EXPECT_NE(d.find("var"), std::string::npos);
    EXPECT_NE(d.find("x"), std::string::npos);
}

TEST(ForOfParser, FO02_DumpLetOf) {
    std::string d = dump("for (let item of []) {}");
    EXPECT_NE(d.find("ForOfStatement"), std::string::npos);
    EXPECT_NE(d.find("let"), std::string::npos);
    EXPECT_NE(d.find("item"), std::string::npos);
}

// ============================================================
// FO-03: Interpreter — array iteration (basic)
// ============================================================

TEST(ForOfInterp, FO03_ArrayBasic) {
    Value v = interp_ok(R"(
        let sum = 0;
        for (const x of [1, 2, 3]) { sum += x; }
        sum
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_number(), 6.0);
}

TEST(ForOfInterp, FO03_ArrayEmpty) {
    Value v = interp_ok(R"(
        let count = 0;
        for (const x of []) { count++; }
        count
    )");
    EXPECT_DOUBLE_EQ(v.as_number(), 0.0);
}

TEST(ForOfInterp, FO03_ArrayVar) {
    Value v = interp_ok(R"(
        var result = 0;
        for (var x of [10, 20, 30]) { result += x; }
        result
    )");
    EXPECT_DOUBLE_EQ(v.as_number(), 60.0);
}

// ============================================================
// FO-04: Interpreter — string iteration
// ============================================================

TEST(ForOfInterp, FO04_StringBasic) {
    Value v = interp_ok(R"(
        let result = "";
        for (const ch of "abc") { result += ch; }
        result
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "abc");
}

TEST(ForOfInterp, FO04_StringEmpty) {
    Value v = interp_ok(R"(
        let count = 0;
        for (const ch of "") { count++; }
        count
    )");
    EXPECT_DOUBLE_EQ(v.as_number(), 0.0);
}

// ============================================================
// FO-05: Interpreter — break in for...of
// ============================================================

TEST(ForOfInterp, FO05_Break) {
    Value v = interp_ok(R"(
        let result = 0;
        for (const x of [1, 2, 3, 4, 5]) {
            if (x === 3) break;
            result += x;
        }
        result
    )");
    EXPECT_DOUBLE_EQ(v.as_number(), 3.0);
}

// ============================================================
// FO-06: Interpreter — continue in for...of
// ============================================================

TEST(ForOfInterp, FO06_Continue) {
    Value v = interp_ok(R"(
        let result = 0;
        for (const x of [1, 2, 3, 4, 5]) {
            if (x % 2 === 0) continue;
            result += x;
        }
        result
    )");
    EXPECT_DOUBLE_EQ(v.as_number(), 9.0);
}

// ============================================================
// FO-07: Interpreter — let per-iteration binding (closure)
// ============================================================

TEST(ForOfInterp, FO07_LetPerIterationBinding) {
    Value v = interp_ok(R"(
        const fns = [];
        for (let x of [1, 2, 3]) {
            fns.push(function() { return x; });
        }
        fns[0]() + fns[1]() + fns[2]()
    )");
    EXPECT_DOUBLE_EQ(v.as_number(), 6.0);
}

// ============================================================
// FO-08: Interpreter — const loop variable TypeError on reassign
// ============================================================

TEST(ForOfInterp, FO08_ConstReassignThrows) {
    Value v = interp_ok(R"(
        let caught = false;
        for (const x of [1, 2]) {
            try { (function() { x = 99; })(); } catch(e) { caught = true; break; }
        }
        caught
    )");
    EXPECT_TRUE(v.is_bool() && v.as_bool());
}

// ============================================================
// FO-09: Interpreter — custom iterator via Symbol.iterator
// ============================================================

TEST(ForOfInterp, FO09_CustomIterator) {
    Value v = interp_ok(R"(
        const obj = {};
        Object.defineProperty(obj, Symbol.iterator, {
            value: function() {
                let i = 0;
                return {
                    next: function() {
                        if (i < 3) return { value: i++, done: false };
                        return { value: undefined, done: true };
                    }
                };
            },
            enumerable: true,
            configurable: true,
            writable: true
        });
        let sum = 0;
        for (const x of obj) { sum += x; }
        sum
    )");
    EXPECT_DOUBLE_EQ(v.as_number(), 3.0);
}

// ============================================================
// FO-10: Interpreter — non-iterable throws TypeError
// ============================================================

TEST(ForOfInterp, FO10_NonIterableThrows) {
    EXPECT_TRUE(interp_throws("for (const x of 42) {}"));
    EXPECT_TRUE(interp_throws("for (const x of null) {}"));
    EXPECT_TRUE(interp_throws("for (const x of undefined) {}"));
}

// ============================================================
// FO-11: Interpreter — labeled break
// ============================================================

TEST(ForOfInterp, FO11_LabeledBreak) {
    Value v = interp_ok(R"(
        let result = 0;
        outer: for (const x of [1, 2, 3]) {
            for (const y of [10, 20]) {
                if (x === 2) break outer;
                result += y;
            }
            result += x;
        }
        result
    )");
    EXPECT_DOUBLE_EQ(v.as_number(), 31.0);
}

// ============================================================
// FO-12: Interpreter — nested for...of
// ============================================================

TEST(ForOfInterp, FO12_Nested) {
    Value v = interp_ok(R"(
        let result = [];
        for (const x of [1, 2]) {
            for (const y of ["a", "b"]) {
                result.push(x + y);
            }
        }
        result.join(",")
    )");
    EXPECT_EQ(v.as_string(), "1a,1b,2a,2b");
}

// ============================================================
// FO-13: Interpreter — array with holes
// ============================================================

TEST(ForOfInterp, FO13_ArrayHoles) {
    Value v = interp_ok(R"(
        let count = 0;
        let undefs = 0;
        for (const x of [1,,3]) {
            count++;
            if (x === undefined) undefs++;
        }
        count * 10 + undefs
    )");
    // Array [1,,3] has length 3; hole at index 1 → undefined
    EXPECT_DOUBLE_EQ(v.as_number(), 31.0);
}

// ============================================================
// FO-14: Interpreter — for...of in try/catch (exception from body)
// ============================================================

TEST(ForOfInterp, FO14_ExceptionInBody) {
    Value v = interp_ok(R"(
        let result = 0;
        try {
            for (const x of [1, 2, 3]) {
                if (x === 2) throw "stop";
                result += x;
            }
        } catch(e) {
            result += 100;
        }
        result
    )");
    EXPECT_DOUBLE_EQ(v.as_number(), 101.0);
}

// ============================================================
// FO-15: Interpreter — no_decl form
// ============================================================

TEST(ForOfInterp, FO15_NoDecl) {
    Value v = interp_ok(R"(
        var x;
        var sum = 0;
        for (x of [5, 6, 7]) { sum += x; }
        sum
    )");
    EXPECT_DOUBLE_EQ(v.as_number(), 18.0);
}

// ============================================================
// FO-16 to FO-30: VM tests (symmetric to Interp tests)
// ============================================================

TEST(ForOfVM, FO16_ArrayBasic) {
    Value v = vm_ok(R"(
        let sum = 0;
        for (const x of [1, 2, 3]) { sum += x; }
        sum
    )");
    EXPECT_DOUBLE_EQ(v.as_number(), 6.0);
}

TEST(ForOfVM, FO16_ArrayEmpty) {
    Value v = vm_ok(R"(
        let count = 0;
        for (const x of []) { count++; }
        count
    )");
    EXPECT_DOUBLE_EQ(v.as_number(), 0.0);
}

TEST(ForOfVM, FO16_ArrayVar) {
    Value v = vm_ok(R"(
        var result = 0;
        for (var x of [10, 20, 30]) { result += x; }
        result
    )");
    EXPECT_DOUBLE_EQ(v.as_number(), 60.0);
}

TEST(ForOfVM, FO17_StringBasic) {
    Value v = vm_ok(R"(
        let result = "";
        for (const ch of "abc") { result += ch; }
        result
    )");
    EXPECT_EQ(v.as_string(), "abc");
}

TEST(ForOfVM, FO17_StringEmpty) {
    Value v = vm_ok(R"(
        let count = 0;
        for (const ch of "") { count++; }
        count
    )");
    EXPECT_DOUBLE_EQ(v.as_number(), 0.0);
}

TEST(ForOfVM, FO18_Break) {
    Value v = vm_ok(R"(
        let result = 0;
        for (const x of [1, 2, 3, 4, 5]) {
            if (x === 3) break;
            result += x;
        }
        result
    )");
    EXPECT_DOUBLE_EQ(v.as_number(), 3.0);
}

TEST(ForOfVM, FO19_Continue) {
    Value v = vm_ok(R"(
        let result = 0;
        for (const x of [1, 2, 3, 4, 5]) {
            if (x % 2 === 0) continue;
            result += x;
        }
        result
    )");
    EXPECT_DOUBLE_EQ(v.as_number(), 9.0);
}

TEST(ForOfVM, FO20_LetPerIterationBinding) {
    Value v = vm_ok(R"(
        const fns = [];
        for (let x of [1, 2, 3]) {
            fns.push(function() { return x; });
        }
        fns[0]() + fns[1]() + fns[2]()
    )");
    EXPECT_DOUBLE_EQ(v.as_number(), 6.0);
}

TEST(ForOfVM, FO21_CustomIterator) {
    Value v = vm_ok(R"(
        const obj = {};
        Object.defineProperty(obj, Symbol.iterator, {
            value: function() {
                let i = 0;
                return {
                    next: function() {
                        if (i < 3) return { value: i++, done: false };
                        return { value: undefined, done: true };
                    }
                };
            },
            enumerable: true,
            configurable: true,
            writable: true
        });
        let sum = 0;
        for (const x of obj) { sum += x; }
        sum
    )");
    EXPECT_DOUBLE_EQ(v.as_number(), 3.0);
}

TEST(ForOfVM, FO22_NonIterableThrows) {
    EXPECT_TRUE(vm_throws("for (const x of 42) {}"));
    EXPECT_TRUE(vm_throws("for (const x of null) {}"));
    EXPECT_TRUE(vm_throws("for (const x of undefined) {}"));
}

TEST(ForOfVM, FO23_LabeledBreak) {
    Value v = vm_ok(R"(
        let result = 0;
        outer: for (const x of [1, 2, 3]) {
            for (const y of [10, 20]) {
                if (x === 2) break outer;
                result += y;
            }
            result += x;
        }
        result
    )");
    EXPECT_DOUBLE_EQ(v.as_number(), 31.0);
}

TEST(ForOfVM, FO24_Nested) {
    Value v = vm_ok(R"(
        let result = [];
        for (const x of [1, 2]) {
            for (const y of ["a", "b"]) {
                result.push(x + y);
            }
        }
        result.join(",")
    )");
    EXPECT_EQ(v.as_string(), "1a,1b,2a,2b");
}

TEST(ForOfVM, FO25_ArrayHoles) {
    Value v = vm_ok(R"(
        let count = 0;
        let undefs = 0;
        for (const x of [1,,3]) {
            count++;
            if (x === undefined) undefs++;
        }
        count * 10 + undefs
    )");
    EXPECT_DOUBLE_EQ(v.as_number(), 31.0);
}

TEST(ForOfVM, FO26_ExceptionInBody) {
    Value v = vm_ok(R"(
        let result = 0;
        try {
            for (const x of [1, 2, 3]) {
                if (x === 2) throw "stop";
                result += x;
            }
        } catch(e) {
            result += 100;
        }
        result
    )");
    EXPECT_DOUBLE_EQ(v.as_number(), 101.0);
}

TEST(ForOfVM, FO27_NoDecl) {
    Value v = vm_ok(R"(
        var x;
        var sum = 0;
        for (x of [5, 6, 7]) { sum += x; }
        sum
    )");
    EXPECT_DOUBLE_EQ(v.as_number(), 18.0);
}

// ============================================================
// FO-28: Array prototype Symbol.iterator works via for...of
// ============================================================

TEST(ForOfInterp, FO28_ArrayProtoIterator) {
    // Test that arrays have Symbol.iterator defined
    Value v = interp_ok(R"(
        let arr = [10, 20, 30];
        let iter = arr[Symbol.iterator]();
        let r1 = iter.next();
        let r2 = iter.next();
        let r3 = iter.next();
        let r4 = iter.next();
        (r1.done === false && r1.value === 10 &&
         r2.done === false && r2.value === 20 &&
         r3.done === false && r3.value === 30 &&
         r4.done === true)
    )");
    EXPECT_TRUE(v.is_bool() && v.as_bool());
}

TEST(ForOfVM, FO28_ArrayProtoIterator) {
    Value v = vm_ok(R"(
        let arr = [10, 20, 30];
        let iter = arr[Symbol.iterator]();
        let r1 = iter.next();
        let r2 = iter.next();
        let r3 = iter.next();
        let r4 = iter.next();
        (r1.done === false && r1.value === 10 &&
         r2.done === false && r2.value === 20 &&
         r3.done === false && r3.value === 30 &&
         r4.done === true)
    )");
    EXPECT_TRUE(v.is_bool() && v.as_bool());
}

// ============================================================
// FO-29: String prototype Symbol.iterator works via for...of
// ============================================================

TEST(ForOfInterp, FO29_StringProtoIterator) {
    Value v = interp_ok(R"(
        let str = "hi";
        let iter = str[Symbol.iterator]();
        let r1 = iter.next();
        let r2 = iter.next();
        let r3 = iter.next();
        (r1.done === false && r1.value === "h" &&
         r2.done === false && r2.value === "i" &&
         r3.done === true)
    )");
    EXPECT_TRUE(v.is_bool() && v.as_bool());
}

TEST(ForOfVM, FO29_StringProtoIterator) {
    Value v = vm_ok(R"(
        let str = "hi";
        let iter = str[Symbol.iterator]();
        let r1 = iter.next();
        let r2 = iter.next();
        let r3 = iter.next();
        (r1.done === false && r1.value === "h" &&
         r2.done === false && r2.value === "i" &&
         r3.done === true)
    )");
    EXPECT_TRUE(v.is_bool() && v.as_bool());
}

// ============================================================
// FO-30: return value of for...of is undefined
// ============================================================

TEST(ForOfInterp, FO30_ReturnUndefined) {
    Value v = interp_ok("for (const x of [1]) {} undefined");
    EXPECT_TRUE(v.is_undefined());
}

TEST(ForOfVM, FO30_ReturnUndefined) {
    Value v = vm_ok("for (const x of [1]) {}");
    EXPECT_TRUE(v.is_undefined());
}

}  // namespace
