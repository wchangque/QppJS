#include "qppjs/frontend/ast.h"
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
    EXPECT_TRUE(parse_result.ok()) << "parse failed: " << parse_result.error().message();
    if (!parse_result.ok()) return false;
    Interpreter interp;
    auto result = interp.exec(parse_result.value());
    return !result.is_ok();
}

static bool vm_throws(std::string_view source) {
    auto parse_result = parse_program(source);
    EXPECT_TRUE(parse_result.ok()) << "parse failed: " << parse_result.error().message();
    if (!parse_result.ok()) return false;
    Compiler compiler;
    auto bytecode = compiler.compile(parse_result.value());
    VM vm;
    auto result = vm.exec(bytecode);
    return !result.is_ok();
}

// ============================================================
// CP-01: 基础字符串表达式键 { [expr]: value }
// ============================================================

TEST(ComputedProperty, CP01_Interp_BasicStringExprKey) {
    auto v = interp_ok("var k = 'foo'; var o = {[k]: 42}; o.foo");
    EXPECT_EQ(v.as_number(), 42.0);
}

TEST(ComputedProperty, CP01_VM_BasicStringExprKey) {
    auto v = vm_ok("var k = 'foo'; var o = {[k]: 42}; o.foo");
    EXPECT_EQ(v.as_number(), 42.0);
}

// ============================================================
// CP-02: 字符串字面量计算键 { ["key"]: 1 }
// ============================================================

TEST(ComputedProperty, CP02_Interp_StringLiteralComputedKey) {
    auto v = interp_ok("var o = {['hello']: 99}; o.hello");
    EXPECT_EQ(v.as_number(), 99.0);
}

TEST(ComputedProperty, CP02_VM_StringLiteralComputedKey) {
    auto v = vm_ok("var o = {['hello']: 99}; o.hello");
    EXPECT_EQ(v.as_number(), 99.0);
}

// ============================================================
// CP-03: 数值运算键 { [1+2]: "three" } → key "3"
// ============================================================

TEST(ComputedProperty, CP03_Interp_NumericArithmeticKey) {
    auto v = interp_ok("var o = {[1+2]: 'three'}; o[3]");
    EXPECT_EQ(v.as_string(), "three");
}

TEST(ComputedProperty, CP03_VM_NumericArithmeticKey) {
    auto v = vm_ok("var o = {[1+2]: 'three'}; o[3]");
    EXPECT_EQ(v.as_string(), "three");
}

// ============================================================
// CP-04: null 键 → "null"
// ============================================================

TEST(ComputedProperty, CP04_Interp_NullKey) {
    auto v = interp_ok("var o = {[null]: 1}; o['null']");
    EXPECT_EQ(v.as_number(), 1.0);
}

TEST(ComputedProperty, CP04_VM_NullKey) {
    auto v = vm_ok("var o = {[null]: 1}; o['null']");
    EXPECT_EQ(v.as_number(), 1.0);
}

// ============================================================
// CP-05: undefined 键 → "undefined"
// ============================================================

TEST(ComputedProperty, CP05_Interp_UndefinedKey) {
    auto v = interp_ok("var o = {[undefined]: 2}; o['undefined']");
    EXPECT_EQ(v.as_number(), 2.0);
}

TEST(ComputedProperty, CP05_VM_UndefinedKey) {
    auto v = vm_ok("var o = {[undefined]: 2}; o['undefined']");
    EXPECT_EQ(v.as_number(), 2.0);
}

// ============================================================
// CP-06: NaN 键 → "NaN"
// ============================================================

TEST(ComputedProperty, CP06_Interp_NaNKey) {
    auto v = interp_ok("var o = {[NaN]: 3}; o['NaN']");
    EXPECT_EQ(v.as_number(), 3.0);
}

TEST(ComputedProperty, CP06_VM_NaNKey) {
    auto v = vm_ok("var o = {[NaN]: 3}; o['NaN']");
    EXPECT_EQ(v.as_number(), 3.0);
}

// ============================================================
// CP-07: 对象键 → "[object Object]"
// ============================================================

TEST(ComputedProperty, CP07_Interp_ObjectKey) {
    auto v = interp_ok("var o = {[{}]: 'val'}; o['[object Object]']");
    EXPECT_EQ(v.as_string(), "val");
}

TEST(ComputedProperty, CP07_VM_ObjectKey) {
    auto v = vm_ok("var o = {[{}]: 'val'}; o['[object Object]']");
    EXPECT_EQ(v.as_string(), "val");
}

// ============================================================
// CP-08: Symbol 作为键：不被 Object.keys 枚举
// ============================================================

TEST(ComputedProperty, CP08_Interp_SymbolKeyNotEnumerated) {
    auto v = interp_ok(
        "var sym = Symbol('s');"
        "var o = {[sym]: 1, a: 2};"
        "Object.keys(o).length");
    EXPECT_EQ(v.as_number(), 1.0);
}

TEST(ComputedProperty, CP08_VM_SymbolKeyNotEnumerated) {
    auto v = vm_ok(
        "var sym = Symbol('s');"
        "var o = {[sym]: 1, a: 2};"
        "Object.keys(o).length");
    EXPECT_EQ(v.as_number(), 1.0);
}

// ============================================================
// CP-09: Symbol 键可通过 obj[sym] 访问
// ============================================================

TEST(ComputedProperty, CP09_Interp_SymbolKeyAccess) {
    auto v = interp_ok(
        "var sym = Symbol('s');"
        "var o = {[sym]: 'symval'};"
        "o[sym]");
    EXPECT_EQ(v.as_string(), "symval");
}

TEST(ComputedProperty, CP09_VM_SymbolKeyAccess) {
    auto v = vm_ok(
        "var sym = Symbol('s');"
        "var o = {[sym]: 'symval'};"
        "o[sym]");
    EXPECT_EQ(v.as_string(), "symval");
}

// ============================================================
// CP-10: 键先于值求值（副作用计数验证）
// ============================================================

TEST(ComputedProperty, CP10_Interp_KeyEvaluatedBeforeValue) {
    // 通过函数调用记录求值顺序，验证 key 先于 value 求值
    auto v = interp_ok(
        "var steps = '';"
        "function getKey() { steps += 'k'; return 'myKey'; }"
        "function getVal() { steps += 'v'; return 'myVal'; }"
        "var o = {[getKey()]: getVal()};"
        "steps");
    EXPECT_EQ(v.as_string(), "kv");
}

TEST(ComputedProperty, CP10_VM_KeyEvaluatedBeforeValue) {
    auto v = vm_ok(
        "var steps = '';"
        "function getKey() { steps += 'k'; return 'myKey'; }"
        "function getVal() { steps += 'v'; return 'myVal'; }"
        "var o = {[getKey()]: getVal()};"
        "steps");
    EXPECT_EQ(v.as_string(), "kv");
}

// ============================================================
// CP-11: 计算方法名 { [expr]() {} }
// ============================================================

TEST(ComputedProperty, CP11_Interp_ComputedMethodName) {
    auto v = interp_ok(
        "var k = 'greet';"
        "var o = {[k]() { return 'hello'; }};"
        "o.greet()");
    EXPECT_EQ(v.as_string(), "hello");
}

TEST(ComputedProperty, CP11_VM_ComputedMethodName) {
    auto v = vm_ok(
        "var k = 'greet';"
        "var o = {[k]() { return 'hello'; }};"
        "o.greet()");
    EXPECT_EQ(v.as_string(), "hello");
}

// ============================================================
// CP-12: 计算方法 .name 属性（SetFunctionName）
// ============================================================

TEST(ComputedProperty, CP12_Interp_ComputedMethodName_Attr) {
    auto v = interp_ok(
        "var k = 'myMethod';"
        "var o = {[k]() {}};"
        "o.myMethod.name");
    EXPECT_EQ(v.as_string(), "myMethod");
}

TEST(ComputedProperty, CP12_VM_ComputedMethodName_Attr) {
    auto v = vm_ok(
        "var k = 'myMethod';"
        "var o = {[k]() {}};"
        "o.myMethod.name");
    EXPECT_EQ(v.as_string(), "myMethod");
}

// ============================================================
// CP-13: 计算 getter { get [expr]() {} }
// ============================================================

TEST(ComputedProperty, CP13_Interp_ComputedGetter) {
    auto v = interp_ok(
        "var k = 'x';"
        "var o = {get [k]() { return 99; }};"
        "o.x");
    EXPECT_EQ(v.as_number(), 99.0);
}

TEST(ComputedProperty, CP13_VM_ComputedGetter) {
    auto v = vm_ok(
        "var k = 'x';"
        "var o = {get [k]() { return 99; }};"
        "o.x");
    EXPECT_EQ(v.as_number(), 99.0);
}

// ============================================================
// CP-14: 计算 setter { set [expr](v) {} }
// ============================================================

TEST(ComputedProperty, CP14_Interp_ComputedSetter) {
    auto v = interp_ok(
        "var k = 'x';"
        "var received = null;"
        "var o = {set [k](v) { received = v; }};"
        "o.x = 77;"
        "received");
    EXPECT_EQ(v.as_number(), 77.0);
}

TEST(ComputedProperty, CP14_VM_ComputedSetter) {
    auto v = vm_ok(
        "var k = 'x';"
        "var received = null;"
        "var o = {set [k](v) { received = v; }};"
        "o.x = 77;"
        "received");
    EXPECT_EQ(v.as_number(), 77.0);
}

// ============================================================
// CP-15: 同名计算 getter + setter 合并
// ============================================================

TEST(ComputedProperty, CP15_Interp_ComputedGetterSetterMerge) {
    auto v = interp_ok(
        "var k = 'val';"
        "var storage = 0;"
        "var o = {"
        "  get [k]() { return storage; },"
        "  set [k](v) { storage = v; }"
        "};"
        "o.val = 55;"
        "o.val");
    EXPECT_EQ(v.as_number(), 55.0);
}

TEST(ComputedProperty, CP15_VM_ComputedGetterSetterMerge) {
    auto v = vm_ok(
        "var k = 'val';"
        "var storage = 0;"
        "var o = {"
        "  get [k]() { return storage; },"
        "  set [k](v) { storage = v; }"
        "};"
        "o.val = 55;"
        "o.val");
    EXPECT_EQ(v.as_number(), 55.0);
}

// ============================================================
// CP-16: ["__proto__"] 不触发原型设置
// ============================================================

TEST(ComputedProperty, CP16_Interp_ComputedProtoNotProtoSet) {
    auto v = interp_ok(
        "var other = {x: 42};"
        "var o = {['__proto__']: other};"
        "o['__proto__'] === other");
    EXPECT_TRUE(v.as_bool());
}

TEST(ComputedProperty, CP16_VM_ComputedProtoNotProtoSet) {
    auto v = vm_ok(
        "var other = {x: 42};"
        "var o = {['__proto__']: other};"
        "o['__proto__'] === other");
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// CP-17: 计算键与静态键共存
// ============================================================

TEST(ComputedProperty, CP17_Interp_ComputedAndStaticKeys) {
    auto v = interp_ok(
        "var k = 'dynamic';"
        "var o = {static: 1, [k]: 2};"
        "o.static + o.dynamic");
    EXPECT_EQ(v.as_number(), 3.0);
}

TEST(ComputedProperty, CP17_VM_ComputedAndStaticKeys) {
    auto v = vm_ok(
        "var k = 'dynamic';"
        "var o = {static: 1, [k]: 2};"
        "o.static + o.dynamic");
    EXPECT_EQ(v.as_number(), 3.0);
}

// ============================================================
// CP-18: 多个计算键对象
// ============================================================

TEST(ComputedProperty, CP18_Interp_MultipleComputedKeys) {
    auto v = interp_ok(
        "var a = 'x'; var b = 'y'; var c = 'z';"
        "var o = {[a]: 1, [b]: 2, [c]: 3};"
        "o.x + o.y + o.z");
    EXPECT_EQ(v.as_number(), 6.0);
}

TEST(ComputedProperty, CP18_VM_MultipleComputedKeys) {
    auto v = vm_ok(
        "var a = 'x'; var b = 'y'; var c = 'z';"
        "var o = {[a]: 1, [b]: 2, [c]: 3};"
        "o.x + o.y + o.z");
    EXPECT_EQ(v.as_number(), 6.0);
}

// ============================================================
// CP-19: Well-Known Symbol key { [Symbol.iterator]() {} }
// ============================================================

TEST(ComputedProperty, CP19_Interp_WellKnownSymbolKey) {
    auto v = interp_ok(
        "var o = {[Symbol.iterator]() { return 'iter'; }};"
        "o[Symbol.iterator]()");
    EXPECT_EQ(v.as_string(), "iter");
}

TEST(ComputedProperty, CP19_VM_WellKnownSymbolKey) {
    auto v = vm_ok(
        "var o = {[Symbol.iterator]() { return 'iter'; }};"
        "o[Symbol.iterator]()");
    EXPECT_EQ(v.as_string(), "iter");
}

// ============================================================
// CP-20: 键表达式抛异常时整个对象字面量报错
// ============================================================

TEST(ComputedProperty, CP20_Interp_KeyExprThrows) {
    EXPECT_TRUE(interp_throws(
        "function throws() { throw new TypeError('oops'); }"
        "var o = {[throws()]: 1};"));
}

TEST(ComputedProperty, CP20_VM_KeyExprThrows) {
    EXPECT_TRUE(vm_throws(
        "function throws() { throw new TypeError('oops'); }"
        "var o = {[throws()]: 1};"));
}

// ============================================================
// CP-21: 计算异步方法 { async [expr]() {} }
// ============================================================

TEST(ComputedProperty, CP21_Interp_ComputedAsyncMethod) {
    auto v = interp_ok(
        "var k = 'doAsync';"
        "var o = {async [k]() { return 10; }};"
        "var result = null;"
        "o.doAsync().then(function(v) { result = v; });"
        "result");
    EXPECT_EQ(v.as_number(), 10.0);
}

TEST(ComputedProperty, CP21_VM_ComputedAsyncMethod) {
    auto v = vm_ok(
        "var k = 'doAsync';"
        "var o = {async [k]() { return 10; }};"
        "var result = null;"
        "o.doAsync().then(function(v) { result = v; });"
        "result");
    EXPECT_EQ(v.as_number(), 10.0);
}

// ============================================================
// CP-22: 计算 generator 方法 { *[expr]() {} }（降级为普通方法）
// ============================================================

TEST(ComputedProperty, CP22_Interp_ComputedGeneratorMethod) {
    auto v = interp_ok(
        "var k = 'gen';"
        "var o = {*[k]() { return 7; }};"
        "typeof o.gen");
    EXPECT_EQ(v.as_string(), "function");
}

TEST(ComputedProperty, CP22_VM_ComputedGeneratorMethod) {
    auto v = vm_ok(
        "var k = 'gen';"
        "var o = {*[k]() { return 7; }};"
        "typeof o.gen");
    EXPECT_EQ(v.as_string(), "function");
}

// ============================================================
// CP-23: 数字键 ToPropertyKey 转 string：{ [0]: 1 } → obj["0"] === 1
// ============================================================

TEST(ComputedProperty, CP23_Interp_NumericKeyToString) {
    auto v = interp_ok("var o = {[0]: 'zero'}; o['0']");
    EXPECT_EQ(v.as_string(), "zero");
}

TEST(ComputedProperty, CP23_VM_NumericKeyToString) {
    auto v = vm_ok("var o = {[0]: 'zero'}; o['0']");
    EXPECT_EQ(v.as_string(), "zero");
}

// ============================================================
// CP-24: 对象 toPrimitive 转换（toString 返回自定义字符串）
// ============================================================

TEST(ComputedProperty, CP24_Interp_ObjectToPrimitive) {
    // 对象作为计算键时，ToPropertyKey 将其转换为 "[object Object]"
    auto v = interp_ok(
        "var key = {name: 'custom'};"
        "var o = {[key]: 'found'};"
        "o['[object Object]']");
    EXPECT_EQ(v.as_string(), "found");
}

TEST(ComputedProperty, CP24_VM_ObjectToPrimitive) {
    auto v = vm_ok(
        "var key = {name: 'custom'};"
        "var o = {[key]: 'found'};"
        "o['[object Object]']");
    EXPECT_EQ(v.as_string(), "found");
}

// ============================================================
// CP-25: 计算 getter .name 带 "get " 前缀
// ============================================================

TEST(ComputedProperty, CP25_Interp_ComputedGetterName) {
    auto v = interp_ok(
        "var k = 'x';"
        "var o = {get [k]() { return 1; }};"
        "Object.getOwnPropertyDescriptor(o, 'x').get.name");
    EXPECT_EQ(v.as_string(), "get x");
}

TEST(ComputedProperty, CP25_VM_ComputedGetterName) {
    auto v = vm_ok(
        "var k = 'x';"
        "var o = {get [k]() { return 1; }};"
        "Object.getOwnPropertyDescriptor(o, 'x').get.name");
    EXPECT_EQ(v.as_string(), "get x");
}

// ============================================================
// CP-26: 计算 setter .name 带 "set " 前缀
// ============================================================

TEST(ComputedProperty, CP26_Interp_ComputedSetterName) {
    auto v = interp_ok(
        "var k = 'x';"
        "var o = {set [k](v) {}};"
        "Object.getOwnPropertyDescriptor(o, 'x').set.name");
    EXPECT_EQ(v.as_string(), "set x");
}

TEST(ComputedProperty, CP26_VM_ComputedSetterName) {
    auto v = vm_ok(
        "var k = 'x';"
        "var o = {set [k](v) {}};"
        "Object.getOwnPropertyDescriptor(o, 'x').set.name");
    EXPECT_EQ(v.as_string(), "set x");
}

// ============================================================
// CP-27: 嵌套对象字面量中的计算键
// ============================================================

TEST(ComputedProperty, CP27_Interp_NestedComputedKey) {
    auto v = interp_ok(
        "var k = 'inner';"
        "var o = {outer: {[k]: 99}};"
        "o.outer.inner");
    EXPECT_EQ(v.as_number(), 99.0);
}

TEST(ComputedProperty, CP27_VM_NestedComputedKey) {
    auto v = vm_ok(
        "var k = 'inner';"
        "var o = {outer: {[k]: 99}};"
        "o.outer.inner");
    EXPECT_EQ(v.as_number(), 99.0);
}

// ============================================================
// CP-28: 解构赋值计算键 const { [key]: alias } = obj
// ============================================================

TEST(ComputedProperty, CP28_Interp_DestructuringComputedKey) {
    auto v = interp_ok(
        "var key = 'foo';"
        "var obj = {foo: 123};"
        "var { [key]: alias } = obj;"
        "alias");
    EXPECT_EQ(v.as_number(), 123.0);
}

TEST(ComputedProperty, CP28_VM_DestructuringComputedKey) {
    auto v = vm_ok(
        "var key = 'foo';"
        "var obj = {foo: 123};"
        "var { [key]: alias } = obj;"
        "alias");
    EXPECT_EQ(v.as_number(), 123.0);
}

// ============================================================
// CP-29: 值为 undefined 的计算键
// ============================================================

TEST(ComputedProperty, CP29_Interp_UndefinedValue) {
    auto v = interp_ok(
        "var k = 'key';"
        "var o = {[k]: undefined};"
        "o.key === undefined");
    EXPECT_TRUE(v.as_bool());
}

TEST(ComputedProperty, CP29_VM_UndefinedValue) {
    auto v = vm_ok(
        "var k = 'key';"
        "var o = {[k]: undefined};"
        "o.key === undefined");
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// CP-30: 多次构造同一对象字面量键结果一致
// ============================================================

TEST(ComputedProperty, CP30_Interp_ConsistentAcrossCalls) {
    auto v = interp_ok(
        "function makeObj(k, v) { return {[k]: v}; }"
        "var a = makeObj('x', 1);"
        "var b = makeObj('x', 2);"
        "a.x + b.x");
    EXPECT_EQ(v.as_number(), 3.0);
}

TEST(ComputedProperty, CP30_VM_ConsistentAcrossCalls) {
    auto v = vm_ok(
        "function makeObj(k, v) { return {[k]: v}; }"
        "var a = makeObj('x', 1);"
        "var b = makeObj('x', 2);"
        "a.x + b.x");
    EXPECT_EQ(v.as_number(), 3.0);
}

// ============================================================
// CP-31: 计算键+解构默认值 — 键存在时不触发默认值
// ============================================================

TEST(ComputedProperty, CP31_Interp_DestructuringDefaultNotTriggered) {
    auto v = interp_ok(
        "var key = 'a';"
        "var {[key]: alias = 99} = {a: 42};"
        "alias");
    EXPECT_EQ(v.as_number(), 42.0);
}

TEST(ComputedProperty, CP31_VM_DestructuringDefaultNotTriggered) {
    auto v = vm_ok(
        "var key = 'a';"
        "var {[key]: alias = 99} = {a: 42};"
        "alias");
    EXPECT_EQ(v.as_number(), 42.0);
}

// ============================================================
// CP-32: 计算键+解构默认值 — 键缺失时触发默认值
// ============================================================

TEST(ComputedProperty, CP32_Interp_DestructuringDefaultTriggeredWhenMissing) {
    auto v = interp_ok(
        "var key = 'a';"
        "var {[key]: alias = 99} = {};"
        "alias");
    EXPECT_EQ(v.as_number(), 99.0);
}

TEST(ComputedProperty, CP32_VM_DestructuringDefaultTriggeredWhenMissing) {
    auto v = vm_ok(
        "var key = 'a';"
        "var {[key]: alias = 99} = {};"
        "alias");
    EXPECT_EQ(v.as_number(), 99.0);
}

// ============================================================
// CP-33: 计算键+解构默认值 — null 值不触发默认值（只有 undefined 触发）
// ============================================================

TEST(ComputedProperty, CP33_Interp_DestructuringDefaultNullNotTriggered) {
    auto v = interp_ok(
        "var key = 'a';"
        "var {[key]: alias = 99} = {a: null};"
        "alias === null");
    EXPECT_TRUE(v.as_bool());
}

TEST(ComputedProperty, CP33_VM_DestructuringDefaultNullNotTriggered) {
    auto v = vm_ok(
        "var key = 'a';"
        "var {[key]: alias = 99} = {a: null};"
        "alias === null");
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// CP-34: 计算键在嵌套解构中 { [key]: { b, c } } = obj
// ============================================================

TEST(ComputedProperty, CP34_Interp_NestedObjectDestructuring) {
    auto v = interp_ok(
        "var key = 'inner';"
        "var {[key]: {b, c}} = {inner: {b: 10, c: 20}};"
        "b + c");
    EXPECT_EQ(v.as_number(), 30.0);
}

TEST(ComputedProperty, CP34_VM_NestedObjectDestructuring) {
    auto v = vm_ok(
        "var key = 'inner';"
        "var {[key]: {b, c}} = {inner: {b: 10, c: 20}};"
        "b + c");
    EXPECT_EQ(v.as_number(), 30.0);
}

// ============================================================
// CP-35: 计算键在嵌套解构中 — 带内层默认值 { [key]: { b = 10 } = {} } = {}
// ============================================================

TEST(ComputedProperty, CP35_Interp_NestedDestructuringWithInnerDefault) {
    auto v = interp_ok(
        "var key = 'a';"
        "var {[key]: {b = 10} = {}} = {};"
        "b");
    EXPECT_EQ(v.as_number(), 10.0);
}

TEST(ComputedProperty, CP35_VM_NestedDestructuringWithInnerDefault) {
    auto v = vm_ok(
        "var key = 'a';"
        "var {[key]: {b = 10} = {}} = {};"
        "b");
    EXPECT_EQ(v.as_number(), 10.0);
}

// ============================================================
// CP-36: 同名双计算键 — 后者覆盖前者
// ============================================================

TEST(ComputedProperty, CP36_Interp_DuplicateComputedKeyLastWins) {
    auto v = interp_ok(
        "var k = 'x';"
        "var o = {[k]: 1, [k]: 2};"
        "o.x");
    EXPECT_EQ(v.as_number(), 2.0);
}

TEST(ComputedProperty, CP36_VM_DuplicateComputedKeyLastWins) {
    auto v = vm_ok(
        "var k = 'x';"
        "var o = {[k]: 1, [k]: 2};"
        "o.x");
    EXPECT_EQ(v.as_number(), 2.0);
}

// ============================================================
// CP-37: 计算键后接同名静态键 — 静态键后写覆盖
// ============================================================

TEST(ComputedProperty, CP37_Interp_ComputedThenStaticKeyLastWins) {
    auto v = interp_ok(
        "var o = {['x']: 1, x: 2};"
        "o.x");
    EXPECT_EQ(v.as_number(), 2.0);
}

TEST(ComputedProperty, CP37_VM_ComputedThenStaticKeyLastWins) {
    auto v = vm_ok(
        "var o = {['x']: 1, x: 2};"
        "o.x");
    EXPECT_EQ(v.as_number(), 2.0);
}

// ============================================================
// CP-38: 静态键后接同名计算键 — 计算键后写覆盖
// ============================================================

TEST(ComputedProperty, CP38_Interp_StaticThenComputedKeyLastWins) {
    auto v = interp_ok(
        "var o = {x: 1, ['x']: 2};"
        "o.x");
    EXPECT_EQ(v.as_number(), 2.0);
}

TEST(ComputedProperty, CP38_VM_StaticThenComputedKeyLastWins) {
    auto v = vm_ok(
        "var o = {x: 1, ['x']: 2};"
        "o.x");
    EXPECT_EQ(v.as_number(), 2.0);
}

// ============================================================
// CP-39: 计算键为 false（falsy）→ ToPropertyKey → "false"
// ============================================================

TEST(ComputedProperty, CP39_Interp_FalsyKeyFalse) {
    auto v = interp_ok(
        "var o = {[false]: 'f'};"
        "o['false']");
    EXPECT_EQ(v.as_string(), "f");
}

TEST(ComputedProperty, CP39_VM_FalsyKeyFalse) {
    auto v = vm_ok(
        "var o = {[false]: 'f'};"
        "o['false']");
    EXPECT_EQ(v.as_string(), "f");
}

// ============================================================
// CP-40: 计算键为 0（falsy）→ ToPropertyKey → "0"
// ============================================================

TEST(ComputedProperty, CP40_Interp_FalsyKeyZero) {
    auto v = interp_ok(
        "var o = {[0]: 'zero'};"
        "o['0']");
    EXPECT_EQ(v.as_string(), "zero");
}

TEST(ComputedProperty, CP40_VM_FalsyKeyZero) {
    auto v = vm_ok(
        "var o = {[0]: 'zero'};"
        "o['0']");
    EXPECT_EQ(v.as_string(), "zero");
}

// ============================================================
// CP-41: 计算键为 "" 空字符串（falsy 且合法键）
// ============================================================

TEST(ComputedProperty, CP41_Interp_FalsyKeyEmptyString) {
    auto v = interp_ok(
        "var o = {['']: 'empty'};"
        "o['']");
    EXPECT_EQ(v.as_string(), "empty");
}

TEST(ComputedProperty, CP41_VM_FalsyKeyEmptyString) {
    auto v = vm_ok(
        "var o = {['']: 'empty'};"
        "o['']");
    EXPECT_EQ(v.as_string(), "empty");
}

// ============================================================
// CP-42: Object.getOwnPropertyDescriptor 验证计算数据属性描述符
// ============================================================

TEST(ComputedProperty, CP42_Interp_GetOwnPropertyDescriptorDataKey) {
    // 计算键数据属性：value/writable/enumerable/configurable 均存在
    auto v = interp_ok(
        "var k = 'foo';"
        "var o = {[k]: 42};"
        "var d = Object.getOwnPropertyDescriptor(o, 'foo');"
        "d.value + '|' + d.writable + '|' + d.enumerable + '|' + d.configurable");
    EXPECT_EQ(v.as_string(), "42|true|true|true");
}

TEST(ComputedProperty, CP42_VM_GetOwnPropertyDescriptorDataKey) {
    auto v = vm_ok(
        "var k = 'foo';"
        "var o = {[k]: 42};"
        "var d = Object.getOwnPropertyDescriptor(o, 'foo');"
        "d.value + '|' + d.writable + '|' + d.enumerable + '|' + d.configurable");
    EXPECT_EQ(v.as_string(), "42|true|true|true");
}

// ============================================================
// CP-43: Object.getOwnPropertyDescriptor 验证计算访问器属性描述符
// ============================================================

TEST(ComputedProperty, CP43_Interp_GetOwnPropertyDescriptorAccessorKey) {
    // 计算键 accessor 属性：get/set 为 function，无 value/writable
    auto v = interp_ok(
        "var k = 'y';"
        "var o = {get [k]() { return 1; }, set [k](v) {}};"
        "var d = Object.getOwnPropertyDescriptor(o, 'y');"
        "typeof d.get + '|' + typeof d.set + '|' + (d.value === undefined) + '|' + (d.writable === undefined)");
    EXPECT_EQ(v.as_string(), "function|function|true|true");
}

TEST(ComputedProperty, CP43_VM_GetOwnPropertyDescriptorAccessorKey) {
    auto v = vm_ok(
        "var k = 'y';"
        "var o = {get [k]() { return 1; }, set [k](v) {}};"
        "var d = Object.getOwnPropertyDescriptor(o, 'y');"
        "typeof d.get + '|' + typeof d.set + '|' + (d.value === undefined) + '|' + (d.writable === undefined)");
    EXPECT_EQ(v.as_string(), "function|function|true|true");
}

// ============================================================
// CP-44: 计算键方法调用时 this 绑定正确
// ============================================================

TEST(ComputedProperty, CP44_Interp_ThisBindingInComputedMethod) {
    auto v = interp_ok(
        "var k = 'getVal';"
        "var o = {val: 100, [k]() { return this.val; }};"
        "o.getVal()");
    EXPECT_EQ(v.as_number(), 100.0);
}

TEST(ComputedProperty, CP44_VM_ThisBindingInComputedMethod) {
    auto v = vm_ok(
        "var k = 'getVal';"
        "var o = {val: 100, [k]() { return this.val; }};"
        "o.getVal()");
    EXPECT_EQ(v.as_number(), 100.0);
}

// ============================================================
// CP-45: 计算方法通过 this 修改对象属性
// ============================================================

TEST(ComputedProperty, CP45_Interp_ComputedMethodMutatesViaThis) {
    auto v = interp_ok(
        "var k = 'bump';"
        "var o = {count: 0, [k]() { this.count = this.count + 1; return this.count; }};"
        "o.bump();"
        "o.bump();"
        "o.bump()");
    EXPECT_EQ(v.as_number(), 3.0);
}

TEST(ComputedProperty, CP45_VM_ComputedMethodMutatesViaThis) {
    auto v = vm_ok(
        "var k = 'bump';"
        "var o = {count: 0, [k]() { this.count = this.count + 1; return this.count; }};"
        "o.bump();"
        "o.bump();"
        "o.bump()");
    EXPECT_EQ(v.as_number(), 3.0);
}

// ============================================================
// CP-46: Symbol 计算键对象仅有一个字符串键时 Object.keys 只返回字符串键
// ============================================================

TEST(ComputedProperty, CP46_Interp_SymbolKeyExcludedFromObjectKeys) {
    auto v = interp_ok(
        "var sym1 = Symbol('a');"
        "var sym2 = Symbol('b');"
        "var o = {[sym1]: 1, [sym2]: 2, str: 3};"
        "var keys = Object.keys(o);"
        "keys.length + '|' + keys[0]");
    EXPECT_EQ(v.as_string(), "1|str");
}

TEST(ComputedProperty, CP46_VM_SymbolKeyExcludedFromObjectKeys) {
    auto v = vm_ok(
        "var sym1 = Symbol('a');"
        "var sym2 = Symbol('b');"
        "var o = {[sym1]: 1, [sym2]: 2, str: 3};"
        "var keys = Object.keys(o);"
        "keys.length + '|' + keys[0]");
    EXPECT_EQ(v.as_string(), "1|str");
}

// ============================================================
// CP-47: 多个 Symbol 计算键各自独立可访问
// ============================================================

TEST(ComputedProperty, CP47_Interp_MultipleSymbolKeysIndependent) {
    auto v = interp_ok(
        "var s1 = Symbol('x');"
        "var s2 = Symbol('x');"
        "var o = {[s1]: 10, [s2]: 20};"
        "o[s1] + o[s2]");
    EXPECT_EQ(v.as_number(), 30.0);
}

TEST(ComputedProperty, CP47_VM_MultipleSymbolKeysIndependent) {
    auto v = vm_ok(
        "var s1 = Symbol('x');"
        "var s2 = Symbol('x');"
        "var o = {[s1]: 10, [s2]: 20};"
        "o[s1] + o[s2]");
    EXPECT_EQ(v.as_number(), 30.0);
}

// ============================================================
// CP-48: Object.getOwnPropertySymbols 目前未实现（已知限制记录）
// ============================================================

TEST(ComputedProperty, CP48_Interp_GetOwnPropertySymbolsNotImplemented) {
    // Object.getOwnPropertySymbols 已实现（misc_fixes_test MF-07）
    auto v = interp_ok("typeof Object.getOwnPropertySymbols");
    EXPECT_EQ(v.as_string(), "function");
}

TEST(ComputedProperty, CP48_VM_GetOwnPropertySymbolsNotImplemented) {
    auto v = vm_ok("typeof Object.getOwnPropertySymbols");
    EXPECT_EQ(v.as_string(), "function");
}

// ============================================================
// CP-49: Symbol 计算 accessor getter 被正确调用（M1 修复验证）
// ============================================================

TEST(ComputedProperty, CP49_Interp_SymbolAccessorGetterCalled) {
    auto v = interp_ok(
        "var sym = Symbol('x');"
        "var callCount = 0;"
        "var o = { get [sym]() { callCount = callCount + 1; return 99; } };"
        "var r1 = o[sym];"
        "var r2 = o[sym];"
        "callCount + '|' + r1 + '|' + r2");
    EXPECT_EQ(v.as_string(), "2|99|99");
}

TEST(ComputedProperty, CP49_VM_SymbolAccessorGetterCalled) {
    auto v = vm_ok(
        "var sym = Symbol('x');"
        "var callCount = 0;"
        "var o = { get [sym]() { callCount = callCount + 1; return 99; } };"
        "var r1 = o[sym];"
        "var r2 = o[sym];"
        "callCount + '|' + r1 + '|' + r2");
    EXPECT_EQ(v.as_string(), "2|99|99");
}

// ============================================================
// CP-50: Symbol 计算 accessor setter 被正确调用（M1 修复验证）
// ============================================================

TEST(ComputedProperty, CP50_Interp_SymbolAccessorSetterCalled) {
    auto v = interp_ok(
        "var sym = Symbol('y');"
        "var captured = 0;"
        "var o = { set [sym](v) { captured = v; } };"
        "o[sym] = 42;"
        "captured");
    EXPECT_EQ(v.as_number(), 42.0);
}

TEST(ComputedProperty, CP50_VM_SymbolAccessorSetterCalled) {
    auto v = vm_ok(
        "var sym = Symbol('y');"
        "var captured = 0;"
        "var o = { set [sym](v) { captured = v; } };"
        "o[sym] = 42;"
        "captured");
    EXPECT_EQ(v.as_number(), 42.0);
}

// ============================================================
// CP-51: VM 解构 rest 排除计算键（M2 修复验证）
// ============================================================

TEST(ComputedProperty, CP51_Interp_RestExcludesComputedKey) {
    auto v = interp_ok(
        "var key = 'a';"
        "var obj = {a: 1, b: 2, c: 3};"
        "var { [key]: x, ...rest } = obj;"
        "x + '|' + rest.b + '|' + rest.c + '|' + (rest.a === undefined)");
    EXPECT_EQ(v.as_string(), "1|2|3|true");
}

TEST(ComputedProperty, CP51_VM_RestExcludesComputedKey) {
    auto v = vm_ok(
        "var key = 'a';"
        "var obj = {a: 1, b: 2, c: 3};"
        "var { [key]: x, ...rest } = obj;"
        "x + '|' + rest.b + '|' + rest.c + '|' + (rest.a === undefined)");
    EXPECT_EQ(v.as_string(), "1|2|3|true");
}

}  // namespace
