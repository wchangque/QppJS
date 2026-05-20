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
// 辅助函数
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
// FI-01: Parser — for (var x in obj)
// ============================================================

TEST(ForInParser, FI01_ParseVarIn) {
    EXPECT_TRUE(parse_ok("for (var x in {}) {}"));
}

TEST(ForInParser, FI01_ParseLetIn) {
    EXPECT_TRUE(parse_ok("for (let x in {}) {}"));
}

TEST(ForInParser, FI01_ParseConstIn) {
    EXPECT_TRUE(parse_ok("for (const x in {}) {}"));
}

TEST(ForInParser, FI01_ParseNoDecl) {
    EXPECT_TRUE(parse_ok("var x; for (x in {}) {}"));
}

// ============================================================
// FI-02: AST dump ForInStatement
// ============================================================

TEST(ForInParser, FI02_AstDump) {
    auto d = dump("for (var k in obj) {}");
    EXPECT_NE(d.find("ForInStatement"), std::string::npos);
    EXPECT_NE(d.find("var"), std::string::npos);
    EXPECT_NE(d.find("k"), std::string::npos);
}

// ============================================================
// FI-03: Interpreter — basic key enumeration
// ============================================================

TEST(ForInInterp, FI03_BasicEnum) {
    // collect keys into array
    auto v = interp_ok(R"(
        var obj = {a: 1, b: 2, c: 3};
        var keys = [];
        for (var k in obj) { keys.push(k); }
        keys.length
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 3.0);
}

TEST(ForInInterp, FI03_KeysAreStrings) {
    auto v = interp_ok(R"(
        var obj = {x: 10};
        var t = "";
        for (var k in obj) { t = typeof k; }
        t
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "string");
}

// ============================================================
// FI-04: Interpreter — for...in with let
// ============================================================

TEST(ForInInterp, FI04_LetDecl) {
    auto v = interp_ok(R"(
        var obj = {a: 1, b: 2};
        var count = 0;
        for (let k in obj) { count++; }
        count
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.0);
}

// ============================================================
// FI-05: Interpreter — empty object → body never executes
// ============================================================

TEST(ForInInterp, FI05_EmptyObject) {
    auto v = interp_ok(R"(
        var count = 0;
        for (var k in {}) { count++; }
        count
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}

// ============================================================
// FI-06: Interpreter — for...in over null → no iteration
// ============================================================

TEST(ForInInterp, FI06_NullObj) {
    auto v = interp_ok(R"(
        var count = 0;
        for (var k in null) { count++; }
        count
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}

TEST(ForInInterp, FI06_UndefinedObj) {
    auto v = interp_ok(R"(
        var count = 0;
        for (var k in undefined) { count++; }
        count
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}

// ============================================================
// FI-07: Interpreter — prototype chain enumeration
// ============================================================

TEST(ForInInterp, FI07_ProtoChain) {
    auto v = interp_ok(R"(
        var parent = {a: 1, b: 2};
        var child = Object.create(parent);
        child.c = 3;
        var keys = [];
        for (var k in child) { keys.push(k); }
        keys.length
    )");
    EXPECT_TRUE(v.is_number());
    // child has own "c", inherits "a" and "b"
    EXPECT_EQ(v.as_number(), 3.0);
}

// ============================================================
// FI-08: Interpreter — non-enumerable properties skipped
// ============================================================

TEST(ForInInterp, FI08_NonEnumerable) {
    auto v = interp_ok(R"(
        var obj = {};
        Object.defineProperty(obj, 'hidden', {value: 42, enumerable: false});
        obj.visible = 1;
        var keys = [];
        for (var k in obj) { keys.push(k); }
        keys.length
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

// ============================================================
// FI-09: Interpreter — break inside for...in
// ============================================================

TEST(ForInInterp, FI09_Break) {
    auto v = interp_ok(R"(
        var obj = {a: 1, b: 2, c: 3};
        var count = 0;
        for (var k in obj) {
            count++;
            break;
        }
        count
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

// ============================================================
// FI-10: Interpreter — continue inside for...in
// ============================================================

TEST(ForInInterp, FI10_Continue) {
    auto v = interp_ok(R"(
        var obj = {a: 1, b: 2, c: 3};
        var sum = 0;
        for (var k in obj) {
            if (k === "b") continue;
            sum += obj[k];
        }
        sum
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 4.0);
}

// ============================================================
// FI-11: Interpreter — for...in collects correct key values
// ============================================================

TEST(ForInInterp, FI11_KeyValues) {
    auto v = interp_ok(R"(
        var obj = {foo: 1};
        var result = "";
        for (var k in obj) { result = k; }
        result
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "foo");
}

// ============================================================
// FI-12: Interpreter — no_decl (existing variable)
// ============================================================

TEST(ForInInterp, FI12_NoDecl) {
    auto v = interp_ok(R"(
        var obj = {x: 1};
        var k = "init";
        for (k in obj) {}
        k
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "x");
}

// ============================================================
// FI-13: Interpreter — return inside for...in propagates
// ============================================================

TEST(ForInInterp, FI13_Return) {
    auto v = interp_ok(R"(
        function f() {
            var obj = {a: 1, b: 2};
            for (var k in obj) {
                if (k === "a") return 42;
            }
            return 0;
        }
        f()
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 42.0);
}

// ============================================================
// FI-14: Interpreter — throw inside for...in propagates
// ============================================================

TEST(ForInInterp, FI14_Throw) {
    auto v = interp_ok(R"(
        var obj = {a: 1};
        var caught = false;
        try {
            for (var k in obj) {
                throw "err";
            }
        } catch (e) {
            caught = true;
        }
        caught
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// FI-15: Interpreter — const decl in for...in
// ============================================================

TEST(ForInInterp, FI15_ConstDecl) {
    auto v = interp_ok(R"(
        var obj = {a: 1, b: 2};
        var count = 0;
        for (const k in obj) { count++; }
        count
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.0);
}

// ============================================================
// VM tests (FI-16 to FI-30) - parallel to interpreter tests
// ============================================================

TEST(ForInVM, FI16_BasicEnum) {
    auto v = vm_ok(R"(
        var obj = {a: 1, b: 2, c: 3};
        var keys = [];
        for (var k in obj) { keys.push(k); }
        keys.length
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 3.0);
}

TEST(ForInVM, FI16_KeysAreStrings) {
    auto v = vm_ok(R"(
        var obj = {x: 10};
        var t = "";
        for (var k in obj) { t = typeof k; }
        t
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "string");
}

TEST(ForInVM, FI17_LetDecl) {
    auto v = vm_ok(R"(
        var obj = {a: 1, b: 2};
        var count = 0;
        for (let k in obj) { count++; }
        count
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.0);
}

TEST(ForInVM, FI18_EmptyObject) {
    auto v = vm_ok(R"(
        var count = 0;
        for (var k in {}) { count++; }
        count
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}

TEST(ForInVM, FI19_NullObj) {
    auto v = vm_ok(R"(
        var count = 0;
        for (var k in null) { count++; }
        count
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}

TEST(ForInVM, FI19_UndefinedObj) {
    auto v = vm_ok(R"(
        var count = 0;
        for (var k in undefined) { count++; }
        count
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}

TEST(ForInVM, FI20_ProtoChain) {
    auto v = vm_ok(R"(
        var parent = {a: 1, b: 2};
        var child = Object.create(parent);
        child.c = 3;
        var keys = [];
        for (var k in child) { keys.push(k); }
        keys.length
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 3.0);
}

TEST(ForInVM, FI21_NonEnumerable) {
    auto v = vm_ok(R"(
        var obj = {};
        Object.defineProperty(obj, 'hidden', {value: 42, enumerable: false});
        obj.visible = 1;
        var keys = [];
        for (var k in obj) { keys.push(k); }
        keys.length
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

TEST(ForInVM, FI22_Break) {
    auto v = vm_ok(R"(
        var obj = {a: 1, b: 2, c: 3};
        var count = 0;
        for (var k in obj) {
            count++;
            break;
        }
        count
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

TEST(ForInVM, FI23_Continue) {
    auto v = vm_ok(R"(
        var obj = {a: 1, b: 2, c: 3};
        var sum = 0;
        for (var k in obj) {
            if (k === "b") continue;
            sum += obj[k];
        }
        sum
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 4.0);
}

TEST(ForInVM, FI24_KeyValues) {
    auto v = vm_ok(R"(
        var obj = {foo: 1};
        var result = "";
        for (var k in obj) { result = k; }
        result
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "foo");
}

TEST(ForInVM, FI25_NoDecl) {
    auto v = vm_ok(R"(
        var obj = {x: 1};
        var k = "init";
        for (k in obj) {}
        k
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "x");
}

TEST(ForInVM, FI26_Return) {
    auto v = vm_ok(R"(
        function f() {
            var obj = {a: 1, b: 2};
            for (var k in obj) {
                if (k === "a") return 42;
            }
            return 0;
        }
        f()
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 42.0);
}

TEST(ForInVM, FI27_Throw) {
    auto v = vm_ok(R"(
        var obj = {a: 1};
        var caught = false;
        try {
            for (var k in obj) {
                throw "err";
            }
        } catch (e) {
            caught = true;
        }
        caught
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(ForInVM, FI28_ConstDecl) {
    auto v = vm_ok(R"(
        var obj = {a: 1, b: 2};
        var count = 0;
        for (const k in obj) { count++; }
        count
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.0);
}

// ============================================================
// FI-29/30: Nested for...in
// ============================================================

TEST(ForInInterp, FI29_Nested) {
    auto v = interp_ok(R"(
        var a = {x: 1, y: 2};
        var b = {p: 3, q: 4};
        var count = 0;
        for (var k1 in a) {
            for (var k2 in b) {
                count++;
            }
        }
        count
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 4.0);
}

TEST(ForInVM, FI30_Nested) {
    auto v = vm_ok(R"(
        var a = {x: 1, y: 2};
        var b = {p: 3, q: 4};
        var count = 0;
        for (var k1 in a) {
            for (var k2 in b) {
                count++;
            }
        }
        count
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 4.0);
}

// ============================================================
// FI-31/32: Prototype chain shadowing — own non-enumerable suppresses inherited enumerable
// ============================================================

TEST(ForInInterp, FI31_ShadowingNonEnumOwn) {
    // child has own 'x' but non-enumerable; parent has enumerable 'x'.
    // Per spec, child's own property (non-enum) must shadow parent's enumerable 'x'.
    auto v = interp_ok(R"(
        var parent = {};
        Object.defineProperty(parent, 'x', { value: 1, enumerable: true });
        var child = Object.create(parent);
        Object.defineProperty(child, 'x', { value: 2, enumerable: false });
        child.y = 3;
        var keys = [];
        for (var k in child) { keys.push(k); }
        keys.join(',')
    )");
    EXPECT_TRUE(v.is_string());
    // 'x' must NOT appear (shadowed by own non-enumerable); only 'y' appears.
    EXPECT_EQ(v.sv(), "y");
}

TEST(ForInVM, FI32_ShadowingNonEnumOwn) {
    auto v = vm_ok(R"(
        var parent = {};
        Object.defineProperty(parent, 'x', { value: 1, enumerable: true });
        var child = Object.create(parent);
        Object.defineProperty(child, 'x', { value: 2, enumerable: false });
        child.y = 3;
        var keys = [];
        for (var k in child) { keys.push(k); }
        keys.join(',')
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.sv(), "y");
}

// ============================================================
// FI-33/34: Deep prototype chain (A → B → C, three levels)
// ============================================================

TEST(ForInInterp, FI33_DeepProtoChain) {
    auto v = interp_ok(R"(
        var c = {a: 1};
        var b = Object.create(c);
        b.b = 2;
        var a = Object.create(b);
        a.c = 3;
        var keys = [];
        for (var k in a) { keys.push(k); }
        keys.length
    )");
    EXPECT_TRUE(v.is_number());
    // a has own 'c', inherits 'b' from b, inherits 'a' from c — total 3 keys, no duplicates
    EXPECT_EQ(v.as_number(), 3.0);
}

TEST(ForInVM, FI34_DeepProtoChain) {
    auto v = vm_ok(R"(
        var c = {a: 1};
        var b = Object.create(c);
        b.b = 2;
        var a = Object.create(b);
        a.c = 3;
        var keys = [];
        for (var k in a) { keys.push(k); }
        keys.length
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 3.0);
}

// ============================================================
// FI-35/36: Labeled break — break outer exits the outer for...in
// ============================================================

TEST(ForInInterp, FI35_LabeledBreak) {
    auto v = interp_ok(R"(
        var count = 0;
        outer: for (var k1 in {a:1, b:2, c:3}) {
            for (var k2 in {x:1, y:2}) {
                count++;
                break outer;
            }
        }
        count
    )");
    EXPECT_TRUE(v.is_number());
    // break outer fires on first k2 iteration of first k1 iteration
    EXPECT_EQ(v.as_number(), 1.0);
}

TEST(ForInVM, FI36_LabeledBreak) {
    auto v = vm_ok(R"(
        var count = 0;
        outer: for (var k1 in {a:1, b:2, c:3}) {
            for (var k2 in {x:1, y:2}) {
                count++;
                break outer;
            }
        }
        count
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

// ============================================================
// FI-37/38: Labeled continue — continue outer skips to next outer iteration
// ============================================================

TEST(ForInInterp, FI37_LabeledContinue) {
    auto v = interp_ok(R"(
        var outerCount = 0;
        var innerCount = 0;
        outer: for (var k1 in {a:1, b:2}) {
            outerCount++;
            for (var k2 in {x:1, y:2}) {
                innerCount++;
                continue outer;
            }
        }
        outerCount * 10 + innerCount
    )");
    EXPECT_TRUE(v.is_number());
    // outer runs twice (a, b); each time inner runs once then continue outer fires
    EXPECT_EQ(v.as_number(), 22.0);
}

TEST(ForInVM, FI38_LabeledContinue) {
    auto v = vm_ok(R"(
        var outerCount = 0;
        var innerCount = 0;
        outer: for (var k1 in {a:1, b:2}) {
            outerCount++;
            for (var k2 in {x:1, y:2}) {
                innerCount++;
                continue outer;
            }
        }
        outerCount * 10 + innerCount
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 22.0);
}

// ============================================================
// FI-39: let per-iteration independent binding (closures) — Interpreter only
// Note: VM uses a single kPushScope/kPopScope around the whole loop body and does
// not yet emit per-iteration scopes, so closure capture independence is a known
// VM limitation and is not tested here.
// ============================================================

TEST(ForInInterp, FI39_LetPerIterationClosure) {
    auto v = interp_ok(R"(
        var fns = [];
        for (let k in {a:1, b:2, c:3}) {
            fns.push(function() { return k; });
        }
        fns[0]() + "," + fns[1]() + "," + fns[2]()
    )");
    EXPECT_TRUE(v.is_string());
    // Each closure must capture its own independent 'k' value.
    // Iteration order for object literals: a, b, c.
    EXPECT_EQ(v.sv(), "a,b,c");
}

// ============================================================
// FI-40/41: const reassignment inside loop body → TypeError caught via try/catch
// ============================================================

TEST(ForInInterp, FI40_ConstReassignThrows) {
    auto v = interp_ok(R"(
        var caught = false;
        try {
            for (const k in {a: 1}) {
                k = 2;
            }
        } catch (e) {
            caught = true;
        }
        caught
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(ForInVM, FI41_ConstReassignThrows) {
    auto v = vm_ok(R"(
        var caught = false;
        try {
            for (const k in {a: 1}) {
                k = 2;
            }
        } catch (e) {
            caught = true;
        }
        caught
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// FI-42/43: Symbol keys not enumerated by for...in
// ============================================================

TEST(ForInInterp, FI42_SymbolKeysSkipped) {
    auto v = interp_ok(R"(
        var sym = Symbol("s");
        var obj = {a: 1};
        obj[sym] = 2;
        var keys = [];
        for (var k in obj) { keys.push(k); }
        keys.length
    )");
    EXPECT_TRUE(v.is_number());
    // Only 'a' should appear; Symbol key must be excluded
    EXPECT_EQ(v.as_number(), 1.0);
}

TEST(ForInVM, FI43_SymbolKeysSkipped) {
    auto v = vm_ok(R"(
        var sym = Symbol("s");
        var obj = {a: 1};
        obj[sym] = 2;
        var keys = [];
        for (var k in obj) { keys.push(k); }
        keys.length
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

// ============================================================
// FI-44/45: Right side is primitive number → no iteration
// ============================================================

TEST(ForInInterp, FI44_PrimitiveNumberRHS) {
    auto v = interp_ok(R"(
        var count = 0;
        for (var k in 42) { count++; }
        count
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}

TEST(ForInVM, FI45_PrimitiveNumberRHS) {
    auto v = vm_ok(R"(
        var count = 0;
        for (var k in 42) { count++; }
        count
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}

// ============================================================
// FI-46/47: Right side is primitive string → no iteration
// ============================================================

TEST(ForInInterp, FI46_PrimitiveStringRHS) {
    auto v = interp_ok(R"(
        var count = 0;
        for (var k in "abc") { count++; }
        count
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}

TEST(ForInVM, FI47_PrimitiveStringRHS) {
    auto v = vm_ok(R"(
        var count = 0;
        for (var k in "abc") { count++; }
        count
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}

// ============================================================
// FI-48/49: Right side is primitive boolean → no iteration
// ============================================================

TEST(ForInInterp, FI48_PrimitiveBooleanRHS) {
    auto v = interp_ok(R"(
        var count = 0;
        for (var k in true) { count++; }
        count
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}

TEST(ForInVM, FI49_PrimitiveBooleanRHS) {
    auto v = vm_ok(R"(
        var count = 0;
        for (var k in true) { count++; }
        count
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}

// ============================================================
// FI-50/51: Object.defineProperty enumerable:false key not enumerated
// (Explicit defineProperty verification — complements FI-08/FI-21)
// ============================================================

TEST(ForInInterp, FI50_DefinePropertyNonEnumNotInForIn) {
    auto v = interp_ok(R"(
        var obj = {a: 1};
        Object.defineProperty(obj, 'b', { value: 2, enumerable: false });
        Object.defineProperty(obj, 'c', { value: 3, enumerable: true });
        var keys = [];
        for (var k in obj) { keys.push(k); }
        keys.join(',')
    )");
    EXPECT_TRUE(v.is_string());
    // 'b' is non-enumerable, must not appear; 'a' and 'c' must appear
    EXPECT_NE(v.sv().find("a"), std::string::npos);
    EXPECT_NE(v.sv().find("c"), std::string::npos);
    EXPECT_EQ(v.sv().find("b"), std::string::npos);
}

TEST(ForInVM, FI51_DefinePropertyNonEnumNotInForIn) {
    auto v = vm_ok(R"(
        var obj = {a: 1};
        Object.defineProperty(obj, 'b', { value: 2, enumerable: false });
        Object.defineProperty(obj, 'c', { value: 3, enumerable: true });
        var keys = [];
        for (var k in obj) { keys.push(k); }
        keys.join(',')
    )");
    EXPECT_TRUE(v.is_string());
    EXPECT_NE(v.sv().find("a"), std::string::npos);
    EXPECT_NE(v.sv().find("c"), std::string::npos);
    EXPECT_EQ(v.sv().find("b"), std::string::npos);
}

// ============================================================
// FI-52/53: Deep chain deduplication — same key at multiple levels, only once
// ============================================================

TEST(ForInInterp, FI52_DeepChainDeduplicate) {
    auto v = interp_ok(R"(
        var c = {x: 1};
        var b = Object.create(c);
        b.x = 2;
        var a = Object.create(b);
        a.x = 3;
        var keys = [];
        for (var k in a) { keys.push(k); }
        keys.length
    )");
    EXPECT_TRUE(v.is_number());
    // 'x' exists at all three levels but must appear only once
    EXPECT_EQ(v.as_number(), 1.0);
}

TEST(ForInVM, FI53_DeepChainDeduplicate) {
    auto v = vm_ok(R"(
        var c = {x: 1};
        var b = Object.create(c);
        b.x = 2;
        var a = Object.create(b);
        a.x = 3;
        var keys = [];
        for (var k in a) { keys.push(k); }
        keys.length
    )");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

}  // namespace
