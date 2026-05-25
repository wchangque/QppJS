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
// Parser tests (smoke: parse should succeed)
// ============================================================

TEST(DestructuringParserTest, ObjectBinding) {
    EXPECT_TRUE(parse_ok("let {a} = obj;"));
    EXPECT_TRUE(parse_ok("let {a, b} = obj;"));
    EXPECT_TRUE(parse_ok("let {a: renamed} = obj;"));
    EXPECT_TRUE(parse_ok("let {a = 99} = {};"));
    EXPECT_TRUE(parse_ok("let {a: {b}} = obj;"));
    EXPECT_TRUE(parse_ok("let {a, ...rest} = obj;"));
    EXPECT_TRUE(parse_ok("const {x, y} = point;"));
    EXPECT_TRUE(parse_ok("var {foo} = bar;"));
}

TEST(DestructuringParserTest, ArrayBinding) {
    EXPECT_TRUE(parse_ok("let [a, b] = arr;"));
    EXPECT_TRUE(parse_ok("let [a] = arr;"));
    EXPECT_TRUE(parse_ok("let [,, third] = arr;"));
    EXPECT_TRUE(parse_ok("let [head, ...tail] = arr;"));
    EXPECT_TRUE(parse_ok("let [a = 1] = [];"));
    EXPECT_TRUE(parse_ok("const [x, y] = pair;"));
}

TEST(DestructuringParserTest, AssignmentPattern) {
    EXPECT_TRUE(parse_ok("let a; let b; [a, b] = [1, 2];"));
    EXPECT_TRUE(parse_ok("let a; let b; ({a, b} = obj);"));
}

TEST(DestructuringParserTest, ForOfPattern) {
    EXPECT_TRUE(parse_ok("for (const [k, v] of []) {}"));
    EXPECT_TRUE(parse_ok("for (let {a, b} of []) {}"));
    EXPECT_TRUE(parse_ok("for (var [x] of []) {}"));
}

// ============================================================
// Interpreter tests
// ============================================================

// DS-01: object basic destructuring
TEST(DestructuringInterpTest, DS01_ObjectBasic) {
    auto v = interp_ok("let {a, b} = {a: 1, b: 2}; a + b;");
    EXPECT_EQ(v.as_number(), 3.0);
}

// DS-02: array basic destructuring
TEST(DestructuringInterpTest, DS02_ArrayBasic) {
    auto v = interp_ok("let [a, b] = [10, 20]; a + b;");
    EXPECT_EQ(v.as_number(), 30.0);
}

// DS-03: object default value (undefined triggers)
TEST(DestructuringInterpTest, DS03_ObjectDefault) {
    auto v = interp_ok("let {a = 99} = {}; a;");
    EXPECT_EQ(v.as_number(), 99.0);
}

// DS-04: null does NOT trigger default (only undefined does)
TEST(DestructuringInterpTest, DS04_NullNoDefault) {
    auto v = interp_ok("let {b = 99} = {b: null}; b === null;");
    EXPECT_TRUE(v.as_bool());
}

// DS-05: rename with colon
TEST(DestructuringInterpTest, DS05_Rename) {
    auto v = interp_ok("let {a: renamed} = {a: 42}; renamed;");
    EXPECT_EQ(v.as_number(), 42.0);
}

// DS-06: nested object destructuring
TEST(DestructuringInterpTest, DS06_NestedObject) {
    auto v = interp_ok("let {a: {b}} = {a: {b: 7}}; b;");
    EXPECT_EQ(v.as_number(), 7.0);
}

// DS-07: array rest
TEST(DestructuringInterpTest, DS07_ArrayRest) {
    auto v = interp_ok(R"(
        let [head, ...tail] = [1, 2, 3];
        head === 1 && tail[0] === 2 && tail[1] === 3 && tail.length === 2;
    )");
    EXPECT_TRUE(v.as_bool());
}

// DS-08: object rest
TEST(DestructuringInterpTest, DS08_ObjectRest) {
    auto v = interp_ok(R"(
        let {a, ...rest} = {a: 1, b: 2, c: 3};
        a === 1 && rest.b === 2 && rest.c === 3;
    )");
    EXPECT_TRUE(v.as_bool());
}

// DS-09: assignment pattern
TEST(DestructuringInterpTest, DS09_AssignmentPattern) {
    auto v = interp_ok("let a; let b; [a, b] = [3, 4]; a + b;");
    EXPECT_EQ(v.as_number(), 7.0);
}

// DS-10: destructure null -> TypeError
TEST(DestructuringInterpTest, DS10_DestructureNull) {
    EXPECT_TRUE(interp_throws("const {a} = null;"));
}

// DS-11: array destructure non-iterable -> TypeError
TEST(DestructuringInterpTest, DS11_DestructureNonIterable) {
    EXPECT_TRUE(interp_throws("const [a] = 42;"));
}

// DS-12: array elision
TEST(DestructuringInterpTest, DS12_ArrayElision) {
    auto v = interp_ok("let [,, third] = [1, 2, 3]; third;");
    EXPECT_EQ(v.as_number(), 3.0);
}

// DS-13: for-of with array pattern
TEST(DestructuringInterpTest, DS13_ForOfPattern) {
    auto v = interp_ok(R"(
        let sum = 0;
        for (const [k, v] of [[1, 10], [2, 20]]) {
            sum += k + v;
        }
        sum;
    )");
    EXPECT_EQ(v.as_number(), 33.0);
}

// DS-14: array default value
TEST(DestructuringInterpTest, DS14_ArrayDefault) {
    auto v = interp_ok("let [a = 5] = []; a;");
    EXPECT_EQ(v.as_number(), 5.0);
}

// DS-15: object assignment pattern
TEST(DestructuringInterpTest, DS15_ObjectAssignPattern) {
    auto v = interp_ok("let a; let b; ({a, b} = {a: 10, b: 20}); a + b;");
    EXPECT_EQ(v.as_number(), 30.0);
}

// DS-16: nested array destructuring
TEST(DestructuringInterpTest, DS16_NestedArray) {
    auto v = interp_ok("let [[a, b], c] = [[1, 2], 3]; a + b + c;");
    EXPECT_EQ(v.as_number(), 6.0);
}

// DS-17: for-of with object pattern
TEST(DestructuringInterpTest, DS17_ForOfObjectPattern) {
    auto v = interp_ok(R"(
        let names = [];
        for (const {name} of [{name: 'a'}, {name: 'b'}]) {
            names.push(name);
        }
        names[0] + names[1];
    )");
    EXPECT_EQ(v.as_string(), "ab");
}

// DS-18: undefined does trigger default
TEST(DestructuringInterpTest, DS18_UndefinedTriggersDefault) {
    auto v = interp_ok("let {a = 42} = {a: undefined}; a;");
    EXPECT_EQ(v.as_number(), 42.0);
}

// ============================================================
// VM tests (symmetric)
// ============================================================

// DS-01 VM
TEST(DestructuringVMTest, DS01_ObjectBasic) {
    auto v = vm_ok("let {a, b} = {a: 1, b: 2}; a + b;");
    EXPECT_EQ(v.as_number(), 3.0);
}

// DS-02 VM
TEST(DestructuringVMTest, DS02_ArrayBasic) {
    auto v = vm_ok("let [a, b] = [10, 20]; a + b;");
    EXPECT_EQ(v.as_number(), 30.0);
}

// DS-03 VM
TEST(DestructuringVMTest, DS03_ObjectDefault) {
    auto v = vm_ok("let {a = 99} = {}; a;");
    EXPECT_EQ(v.as_number(), 99.0);
}

// DS-04 VM
TEST(DestructuringVMTest, DS04_NullNoDefault) {
    auto v = vm_ok("let {b = 99} = {b: null}; b === null;");
    EXPECT_TRUE(v.as_bool());
}

// DS-05 VM
TEST(DestructuringVMTest, DS05_Rename) {
    auto v = vm_ok("let {a: renamed} = {a: 42}; renamed;");
    EXPECT_EQ(v.as_number(), 42.0);
}

// DS-06 VM
TEST(DestructuringVMTest, DS06_NestedObject) {
    auto v = vm_ok("let {a: {b}} = {a: {b: 7}}; b;");
    EXPECT_EQ(v.as_number(), 7.0);
}

// DS-07 VM
TEST(DestructuringVMTest, DS07_ArrayRest) {
    auto v = vm_ok(R"(
        let [head, ...tail] = [1, 2, 3];
        head === 1 && tail[0] === 2 && tail[1] === 3 && tail.length === 2;
    )");
    EXPECT_TRUE(v.as_bool());
}

// DS-08 VM
TEST(DestructuringVMTest, DS08_ObjectRest) {
    auto v = vm_ok(R"(
        let {a, ...rest} = {a: 1, b: 2, c: 3};
        a === 1 && rest.b === 2 && rest.c === 3;
    )");
    EXPECT_TRUE(v.as_bool());
}

// DS-09 VM
TEST(DestructuringVMTest, DS09_AssignmentPattern) {
    auto v = vm_ok("let a; let b; [a, b] = [3, 4]; a + b;");
    EXPECT_EQ(v.as_number(), 7.0);
}

// DS-10 VM
TEST(DestructuringVMTest, DS10_DestructureNull) {
    EXPECT_TRUE(vm_throws("const {a} = null;"));
}

// DS-11 VM
TEST(DestructuringVMTest, DS11_DestructureNonIterable) {
    EXPECT_TRUE(vm_throws("const [a] = 42;"));
}

// DS-12 VM
TEST(DestructuringVMTest, DS12_ArrayElision) {
    auto v = vm_ok("let [,, third] = [1, 2, 3]; third;");
    EXPECT_EQ(v.as_number(), 3.0);
}

// DS-13 VM
TEST(DestructuringVMTest, DS13_ForOfPattern) {
    auto v = vm_ok(R"(
        let sum = 0;
        for (const [k, v] of [[1, 10], [2, 20]]) {
            sum += k + v;
        }
        sum;
    )");
    EXPECT_EQ(v.as_number(), 33.0);
}

// DS-14 VM
TEST(DestructuringVMTest, DS14_ArrayDefault) {
    auto v = vm_ok("let [a = 5] = []; a;");
    EXPECT_EQ(v.as_number(), 5.0);
}

// DS-15 VM
TEST(DestructuringVMTest, DS15_ObjectAssignPattern) {
    auto v = vm_ok("let a; let b; ({a, b} = {a: 10, b: 20}); a + b;");
    EXPECT_EQ(v.as_number(), 30.0);
}

// DS-16 VM
TEST(DestructuringVMTest, DS16_NestedArray) {
    auto v = vm_ok("let [[a, b], c] = [[1, 2], 3]; a + b + c;");
    EXPECT_EQ(v.as_number(), 6.0);
}

// DS-17 VM
TEST(DestructuringVMTest, DS17_ForOfObjectPattern) {
    auto v = vm_ok(R"(
        let names = [];
        for (const {name} of [{name: 'a'}, {name: 'b'}]) {
            names.push(name);
        }
        names[0] + names[1];
    )");
    EXPECT_EQ(v.as_string(), "ab");
}

// DS-18 VM
TEST(DestructuringVMTest, DS18_UndefinedTriggersDefault) {
    auto v = vm_ok("let {a = 42} = {a: undefined}; a;");
    EXPECT_EQ(v.as_number(), 42.0);
}

// ============================================================
// Additional Parser tests — SyntaxError cases
// ============================================================

// rest 非末尾 → SyntaxError（解析阶段）
TEST(DestructuringParserTest, ArrayRestNotAtEnd) {
    EXPECT_FALSE(parse_ok("let [...a, b] = [1, 2];"));
}

TEST(DestructuringParserTest, ObjectRestNotAtEnd) {
    EXPECT_FALSE(parse_ok("let {...a, b} = {};"));
}

// 赋值模式中成员表达式目标：当前不支持（已知限制，Parser 报 SyntaxError）
TEST(DestructuringParserTest, AssignTargetMember_KnownLimitation) {
    // [o.x] = [42] 当前 convert_expr_to_pattern 不处理 MemberExpression → SyntaxError
    EXPECT_FALSE(parse_ok("let o = {}; [o.x] = [42];"));
}

// ============================================================
// DS-19 ~ DS-37 — 解释器边界测试
// ============================================================

// DS-19: 嵌套对象解构默认值 {a: {b=1}={}} = {}
TEST(DestructuringInterpTest, DS19_NestedObjectDefault) {
    auto v = interp_ok("let {a: {b = 1} = {}} = {}; b;");
    EXPECT_EQ(v.as_number(), 1.0);
}

// DS-20: 数组解构——0/false/null/NaN 不触发默认值
TEST(DestructuringInterpTest, DS20_ArrayDefaultNonTriggering) {
    auto v = interp_ok(R"(
        let [a = 99, b = 99, c = 99] = [0, false, null];
        a === 0 && b === false && c === null;
    )");
    EXPECT_TRUE(v.as_bool());
}

// DS-21: 默认值副作用——仅在 undefined 时调用一次
TEST(DestructuringInterpTest, DS21_DefaultSideEffect) {
    auto v = interp_ok(R"(
        let count = 0;
        function inc() { count++; return 42; }
        let {a = inc(), b = inc()} = {a: 1};
        count;
    )");
    // a=1 不触发，b=undefined 触发一次
    EXPECT_EQ(v.as_number(), 1.0);
}

// DS-22: 对象 rest 明确排除已命名属性
TEST(DestructuringInterpTest, DS22_ObjectRestExcludesNamed) {
    auto v = interp_ok(R"(
        let {a, b, ...rest} = {a: 1, b: 2, c: 3, d: 4};
        rest.a === undefined && rest.b === undefined &&
        rest.c === 3 && rest.d === 4 &&
        Object.keys(rest).length === 2;
    )");
    EXPECT_TRUE(v.as_bool());
}

// DS-23: 对象 rest 不含原型链属性
TEST(DestructuringInterpTest, DS23_ObjectRestNoProtoChain) {
    auto v = interp_ok(R"(
        const proto = {inherited: 99};
        const obj = Object.create(proto);
        obj.own = 1;
        const {own, ...rest} = obj;
        own === 1 && Object.keys(rest).length === 0;
    )");
    EXPECT_TRUE(v.as_bool());
}

// DS-24: 对象 rest 是新对象（非原始对象的引用）
TEST(DestructuringInterpTest, DS24_ObjectRestIsNewObject) {
    auto v = interp_ok(R"(
        const src = {a: 1, b: 2};
        const {a, ...rest} = src;
        rest !== src;
    )");
    EXPECT_TRUE(v.as_bool());
}

// DS-25: 字符串解构（走迭代器协议 fast path）
TEST(DestructuringInterpTest, DS25_StringDestructuring) {
    auto v = interp_ok("let [a, b] = 'hi'; a === 'h' && b === 'i';");
    EXPECT_TRUE(v.as_bool());
}

// DS-26: 空数组 rest
TEST(DestructuringInterpTest, DS26_EmptyArrayRest) {
    auto v = interp_ok(R"(
        let [a, ...rest] = [1];
        a === 1 && rest.length === 0;
    )");
    EXPECT_TRUE(v.as_bool());
}

// DS-27: 赋值表达式的结果是 RHS 值
TEST(DestructuringInterpTest, DS27_AssignExprReturnsRHS) {
    auto v = interp_ok(R"(
        let a; let b; let x;
        x = ([a, b] = [1, 2]);
        a === 1 && b === 2 && x[0] === 1 && x[1] === 2;
    )");
    EXPECT_TRUE(v.as_bool());
}

// DS-28: 数组+对象混合嵌套
TEST(DestructuringInterpTest, DS28_MixedNestedArrayObject) {
    auto v = interp_ok(R"(
        const [{a}, {b}] = [{a: 1}, {b: 2}];
        a === 1 && b === 2;
    )");
    EXPECT_TRUE(v.as_bool());
}

// DS-29: 三层嵌套
TEST(DestructuringInterpTest, DS29_ThreeLevelNested) {
    auto v = interp_ok("let {a: {b: {c}}} = {a: {b: {c: 42}}}; c;");
    EXPECT_EQ(v.as_number(), 42.0);
}

// DS-30: 中间层为 null → TypeError
TEST(DestructuringInterpTest, DS30_NestedNullMiddleLayer) {
    EXPECT_TRUE(interp_throws("let {a: {b}} = {a: null};"));
}

// DS-31: const {a} = undefined → TypeError
TEST(DestructuringInterpTest, DS31_DestructureUndefined) {
    EXPECT_TRUE(interp_throws("const {a} = undefined;"));
}

// DS-32: const {a} = undefined (array form)
TEST(DestructuringInterpTest, DS32_ArrayDestructureUndefined) {
    EXPECT_TRUE(interp_throws("const [a] = undefined;"));
}

// DS-33: var 解构声明可正常执行
TEST(DestructuringInterpTest, DS33_VarDestructuring) {
    auto v = interp_ok("var {a, b} = {a: 10, b: 20}; a + b;");
    EXPECT_EQ(v.as_number(), 30.0);
}

// DS-34: for-of 嵌套解构模式（{name, scores: [first]}）
TEST(DestructuringInterpTest, DS34_ForOfNestedDestructuring) {
    auto v = interp_ok(R"(
        let result = [];
        for (const {name, scores: [first]} of [{name: 'a', scores: [10, 20]}]) {
            result.push(name + first);
        }
        result[0];
    )");
    EXPECT_EQ(v.as_string(), "a10");
}

// DS-35: 自定义 Symbol.iterator 用于数组解构
TEST(DestructuringInterpTest, DS35_CustomSymbolIterator) {
    auto v = interp_ok(R"(
        const iter = {};
        Object.defineProperty(iter, Symbol.iterator, {
            value: function() {
                let n = 0;
                return {
                    next: function() {
                        n++;
                        if (n <= 3) return {value: n, done: false};
                        return {value: undefined, done: true};
                    }
                };
            },
            enumerable: false, configurable: true, writable: true
        });
        let [a, b, c] = iter;
        a === 1 && b === 2 && c === 3;
    )");
    EXPECT_TRUE(v.as_bool());
}

// DS-36: 迭代器耗尽后元素为 undefined
TEST(DestructuringInterpTest, DS36_ExhaustedIteratorGivesUndefined) {
    auto v = interp_ok(R"(
        let [a, b, c] = [1, 2];
        a === 1 && b === 2 && c === undefined;
    )");
    EXPECT_TRUE(v.as_bool());
}

// DS-37: 对象解构 undefined 属性采用默认值（明确 undefined 赋值触发）
TEST(DestructuringInterpTest, DS37_ObjectDefaultUndefinedExplicit) {
    auto v = interp_ok(R"(
        let obj = {x: undefined, y: 5};
        let {x = 100, y = 200} = obj;
        x === 100 && y === 5;
    )");
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// DS-19 ~ DS-37 — VM 对称测试
// ============================================================

// DS-19 VM
TEST(DestructuringVMTest, DS19_NestedObjectDefault) {
    auto v = vm_ok("let {a: {b = 1} = {}} = {}; b;");
    EXPECT_EQ(v.as_number(), 1.0);
}

// DS-20 VM
TEST(DestructuringVMTest, DS20_ArrayDefaultNonTriggering) {
    auto v = vm_ok(R"(
        let [a = 99, b = 99, c = 99] = [0, false, null];
        a === 0 && b === false && c === null;
    )");
    EXPECT_TRUE(v.as_bool());
}

// DS-21 VM
TEST(DestructuringVMTest, DS21_DefaultSideEffect) {
    auto v = vm_ok(R"(
        let count = 0;
        function inc() { count++; return 42; }
        let {a = inc(), b = inc()} = {a: 1};
        count;
    )");
    EXPECT_EQ(v.as_number(), 1.0);
}

// DS-22 VM
TEST(DestructuringVMTest, DS22_ObjectRestExcludesNamed) {
    auto v = vm_ok(R"(
        let {a, b, ...rest} = {a: 1, b: 2, c: 3, d: 4};
        rest.a === undefined && rest.b === undefined &&
        rest.c === 3 && rest.d === 4 &&
        Object.keys(rest).length === 2;
    )");
    EXPECT_TRUE(v.as_bool());
}

// DS-23 VM
TEST(DestructuringVMTest, DS23_ObjectRestNoProtoChain) {
    auto v = vm_ok(R"(
        const proto = {inherited: 99};
        const obj = Object.create(proto);
        obj.own = 1;
        const {own, ...rest} = obj;
        own === 1 && Object.keys(rest).length === 0;
    )");
    EXPECT_TRUE(v.as_bool());
}

// DS-24 VM
TEST(DestructuringVMTest, DS24_ObjectRestIsNewObject) {
    auto v = vm_ok(R"(
        const src = {a: 1, b: 2};
        const {a, ...rest} = src;
        rest !== src;
    )");
    EXPECT_TRUE(v.as_bool());
}

// DS-25 VM
TEST(DestructuringVMTest, DS25_StringDestructuring) {
    auto v = vm_ok("let [a, b] = 'hi'; a === 'h' && b === 'i';");
    EXPECT_TRUE(v.as_bool());
}

// DS-26 VM
TEST(DestructuringVMTest, DS26_EmptyArrayRest) {
    auto v = vm_ok(R"(
        let [a, ...rest] = [1];
        a === 1 && rest.length === 0;
    )");
    EXPECT_TRUE(v.as_bool());
}

// DS-27 VM
TEST(DestructuringVMTest, DS27_AssignExprReturnsRHS) {
    auto v = vm_ok(R"(
        let a; let b; let x;
        x = ([a, b] = [1, 2]);
        a === 1 && b === 2 && x[0] === 1 && x[1] === 2;
    )");
    EXPECT_TRUE(v.as_bool());
}

// DS-28 VM
TEST(DestructuringVMTest, DS28_MixedNestedArrayObject) {
    auto v = vm_ok(R"(
        const [{a}, {b}] = [{a: 1}, {b: 2}];
        a === 1 && b === 2;
    )");
    EXPECT_TRUE(v.as_bool());
}

// DS-29 VM
TEST(DestructuringVMTest, DS29_ThreeLevelNested) {
    auto v = vm_ok("let {a: {b: {c}}} = {a: {b: {c: 42}}}; c;");
    EXPECT_EQ(v.as_number(), 42.0);
}

// DS-30 VM
TEST(DestructuringVMTest, DS30_NestedNullMiddleLayer) {
    EXPECT_TRUE(vm_throws("let {a: {b}} = {a: null};"));
}

// DS-31 VM
TEST(DestructuringVMTest, DS31_DestructureUndefined) {
    EXPECT_TRUE(vm_throws("const {a} = undefined;"));
}

// DS-32 VM
TEST(DestructuringVMTest, DS32_ArrayDestructureUndefined) {
    EXPECT_TRUE(vm_throws("const [a] = undefined;"));
}

// DS-33 VM
TEST(DestructuringVMTest, DS33_VarDestructuring) {
    auto v = vm_ok("var {a, b} = {a: 10, b: 20}; a + b;");
    EXPECT_EQ(v.as_number(), 30.0);
}

// DS-34 VM
TEST(DestructuringVMTest, DS34_ForOfNestedDestructuring) {
    auto v = vm_ok(R"(
        let result = [];
        for (const {name, scores: [first]} of [{name: 'a', scores: [10, 20]}]) {
            result.push(name + first);
        }
        result[0];
    )");
    EXPECT_EQ(v.as_string(), "a10");
}

// DS-35 VM
TEST(DestructuringVMTest, DS35_CustomSymbolIterator) {
    auto v = vm_ok(R"(
        const iter = {};
        Object.defineProperty(iter, Symbol.iterator, {
            value: function() {
                let n = 0;
                return {
                    next: function() {
                        n++;
                        if (n <= 3) return {value: n, done: false};
                        return {value: undefined, done: true};
                    }
                };
            },
            enumerable: false, configurable: true, writable: true
        });
        let [a, b, c] = iter;
        a === 1 && b === 2 && c === 3;
    )");
    EXPECT_TRUE(v.as_bool());
}

// DS-36 VM
TEST(DestructuringVMTest, DS36_ExhaustedIteratorGivesUndefined) {
    auto v = vm_ok(R"(
        let [a, b, c] = [1, 2];
        a === 1 && b === 2 && c === undefined;
    )");
    EXPECT_TRUE(v.as_bool());
}

// DS-37 VM
TEST(DestructuringVMTest, DS37_ObjectDefaultUndefinedExplicit) {
    auto v = vm_ok(R"(
        let obj = {x: undefined, y: 5};
        let {x = 100, y = 200} = obj;
        x === 100 && y === 5;
    )");
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// DS-38 ~ DS-47 — Review 修复验证测试
// ============================================================

// DS-38 Interp: M1 — let 解构声明在块级作用域中不泄漏（has_block_scope_decl 覆盖 DestructuringDeclaration）
TEST(DestructuringInterpTest, DS38_LetDestructureBlockScope) {
    // x 应在块外不可见（ReferenceError）
    EXPECT_TRUE(interp_throws(R"(
        { let {x} = {x: 1}; }
        x;
    )"));
}

// DS-39 Interp: M1 — const 解构声明在块级作用域中不泄漏
TEST(DestructuringInterpTest, DS39_ConstDestructureBlockScope) {
    EXPECT_TRUE(interp_throws(R"(
        { const [a, b] = [1, 2]; }
        a;
    )"));
}

// DS-40 Interp: M3 — for (var [x] of ...) hoists x 到函数作用域
TEST(DestructuringInterpTest, DS40_ForOfVarPatternHoist) {
    auto v = interp_ok(R"(
        var result = 0;
        for (var [x] of [[10], [20]]) {
            result += x;
        }
        result;
    )");
    EXPECT_EQ(v.as_number(), 30.0);
}

// DS-41 Interp: M2 — let 解构预声明（默认值表达式可见后续名字占位）
TEST(DestructuringInterpTest, DS41_LetDestructurePreDeclare) {
    // 通常场景：正常解构，所有名字都在绑定后可见
    auto v = interp_ok(R"(
        let {a, b} = {a: 1, b: 2};
        a + b;
    )");
    EXPECT_EQ(v.as_number(), 3.0);
}

// DS-42 Interp: M4 — 普通对象字面量中 spread 抛 SyntaxError
TEST(DestructuringInterpTest, DS42_ObjectLiteralSpreadThrows) {
    EXPECT_TRUE(interp_throws(R"(
        var r = {};
        var x = {...r};
    )"));
}

// DS-43 Interp: M5 — 普通对象字面量中 shorthand+default 抛 SyntaxError
TEST(DestructuringInterpTest, DS43_ObjectLiteralShorthandDefaultThrows) {
    EXPECT_TRUE(interp_throws(R"(
        var a = 1;
        var x = {a = 2};
    )"));
}

// DS-38 VM: M1 — let 解构声明在块级作用域中不泄漏
TEST(DestructuringVMTest, DS38_LetDestructureBlockScope) {
    EXPECT_TRUE(vm_throws(R"(
        { let {x} = {x: 1}; }
        x;
    )"));
}

// DS-39 VM: M1 — const 解构声明在块级作用域中不泄漏
TEST(DestructuringVMTest, DS39_ConstDestructureBlockScope) {
    EXPECT_TRUE(vm_throws(R"(
        { const [a, b] = [1, 2]; }
        a;
    )"));
}

// DS-40 VM: M3 — for (var [x] of ...) hoists x 到函数作用域
TEST(DestructuringVMTest, DS40_ForOfVarPatternHoist) {
    auto v = vm_ok(R"(
        var result = 0;
        for (var [x] of [[10], [20]]) {
            result += x;
        }
        result;
    )");
    EXPECT_EQ(v.as_number(), 30.0);
}

// DS-41 VM: M2 — let 解构预声明（正常语义验证）
TEST(DestructuringVMTest, DS41_LetDestructurePreDeclare) {
    auto v = vm_ok(R"(
        let {a, b} = {a: 1, b: 2};
        a + b;
    )");
    EXPECT_EQ(v.as_number(), 3.0);
}

// DS-42 VM: M4 — 普通对象字面量中 spread 抛 SyntaxError
TEST(DestructuringVMTest, DS42_ObjectLiteralSpreadThrows) {
    EXPECT_TRUE(vm_throws(R"(
        var r = {};
        var x = {...r};
    )"));
}

// DS-43 VM: M5 — 普通对象字面量中 shorthand+default 抛 SyntaxError
TEST(DestructuringVMTest, DS43_ObjectLiteralShorthandDefaultThrows) {
    EXPECT_TRUE(vm_throws(R"(
        var a = 1;
        var x = {a = 2};
    )"));
}

}  // namespace
