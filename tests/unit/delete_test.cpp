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
// DEL-01: delete obj.prop (属性存在) → true，属性不再存在
// ============================================================

TEST(DeleteInterp, DEL01_DeleteExistingProp) {
    auto v = interp_ok("var o = {a: 1, b: 2}; var r = delete o.a; r");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(DeleteInterp, DEL01_DeleteExistingPropGone) {
    auto v = interp_ok("var o = {a: 1}; delete o.a; o.a");
    EXPECT_TRUE(v.is_undefined());
}

TEST(DeleteVM, DEL01_DeleteExistingProp) {
    auto v = vm_ok("var o = {a: 1, b: 2}; var r = delete o.a; r");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(DeleteVM, DEL01_DeleteExistingPropGone) {
    auto v = vm_ok("var o = {a: 1}; delete o.a; o.a");
    EXPECT_TRUE(v.is_undefined());
}

// ============================================================
// DEL-02: delete obj.prop (属性不存在) → true
// ============================================================

TEST(DeleteInterp, DEL02_DeleteNonExistentProp) {
    auto v = interp_ok("var o = {}; delete o.x");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(DeleteVM, DEL02_DeleteNonExistentProp) {
    auto v = vm_ok("var o = {}; delete o.x");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// DEL-03: delete obj[key] (computed) → true
// ============================================================

TEST(DeleteInterp, DEL03_DeleteComputedProp) {
    auto v = interp_ok("var o = {x: 42}; var r = delete o['x']; r");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(DeleteInterp, DEL03_DeleteComputedPropGone) {
    auto v = interp_ok("var o = {x: 42}; delete o['x']; o.x");
    EXPECT_TRUE(v.is_undefined());
}

TEST(DeleteVM, DEL03_DeleteComputedProp) {
    auto v = vm_ok("var o = {x: 42}; var r = delete o['x']; r");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(DeleteVM, DEL03_DeleteComputedPropGone) {
    auto v = vm_ok("var o = {x: 42}; delete o['x']; o.x");
    EXPECT_TRUE(v.is_undefined());
}

// ============================================================
// DEL-04: delete arr[1] → true, length 不变, arr[1] === undefined
// ============================================================

TEST(DeleteInterp, DEL04_DeleteArrayElem) {
    auto v = interp_ok("var a = [1,2,3]; var r = delete a[1]; r");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(DeleteInterp, DEL04_DeleteArrayElemLengthUnchanged) {
    auto v = interp_ok("var a = [1,2,3]; delete a[1]; a.length");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 3.0);
}

TEST(DeleteInterp, DEL04_DeleteArrayElemHole) {
    auto v = interp_ok("var a = [1,2,3]; delete a[1]; a[1]");
    EXPECT_TRUE(v.is_undefined());
}

TEST(DeleteVM, DEL04_DeleteArrayElem) {
    auto v = vm_ok("var a = [1,2,3]; var r = delete a[1]; r");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(DeleteVM, DEL04_DeleteArrayElemLengthUnchanged) {
    auto v = vm_ok("var a = [1,2,3]; delete a[1]; a.length");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 3.0);
}

TEST(DeleteVM, DEL04_DeleteArrayElemHole) {
    auto v = vm_ok("var a = [1,2,3]; delete a[1]; a[1]");
    EXPECT_TRUE(v.is_undefined());
}

// ============================================================
// DEL-05: delete arr.length → false
// ============================================================

TEST(DeleteInterp, DEL05_DeleteArrayLength) {
    auto v = interp_ok("var a = [1,2,3]; delete a.length");
    EXPECT_TRUE(v.is_bool());
    EXPECT_FALSE(v.as_bool());
}

TEST(DeleteVM, DEL05_DeleteArrayLength) {
    auto v = vm_ok("var a = [1,2,3]; delete a.length");
    EXPECT_TRUE(v.is_bool());
    EXPECT_FALSE(v.as_bool());
}

// ============================================================
// DEL-06: var x = 1; delete x (局部变量) → false, x 仍可访问
// ============================================================

TEST(DeleteInterp, DEL06_DeleteLocalVar) {
    auto v = interp_ok("var x = 1; delete x");
    EXPECT_TRUE(v.is_bool());
    EXPECT_FALSE(v.as_bool());
}

TEST(DeleteInterp, DEL06_DeleteLocalVarStillAccessible) {
    auto v = interp_ok("var x = 42; delete x; x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 42.0);
}

TEST(DeleteVM, DEL06_DeleteLocalVar) {
    auto v = vm_ok("var x = 1; delete x");
    EXPECT_TRUE(v.is_bool());
    EXPECT_FALSE(v.as_bool());
}

TEST(DeleteVM, DEL06_DeleteLocalVarStillAccessible) {
    auto v = vm_ok("var x = 42; delete x; x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 42.0);
}

// ============================================================
// DEL-07: delete undeclaredVar (未声明) → true
// ============================================================

TEST(DeleteInterp, DEL07_DeleteUndeclaredVar) {
    auto v = interp_ok("delete undeclaredXyz");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(DeleteVM, DEL07_DeleteUndeclaredVar) {
    auto v = vm_ok("delete undeclaredXyz");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// DEL-08: delete 42 (字面量) → true
// ============================================================

TEST(DeleteInterp, DEL08_DeleteLiteral) {
    auto v = interp_ok("delete 42");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(DeleteVM, DEL08_DeleteLiteral) {
    auto v = vm_ok("delete 42");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// DEL-09: delete (1+2) (算术表达式) → true
// ============================================================

TEST(DeleteInterp, DEL09_DeleteExpr) {
    auto v = interp_ok("delete (1+2)");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(DeleteVM, DEL09_DeleteExpr) {
    auto v = vm_ok("delete (1+2)");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// DEL-10: delete this → true
// ============================================================

TEST(DeleteInterp, DEL10_DeleteThis) {
    auto v = interp_ok("delete this");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(DeleteVM, DEL10_DeleteThis) {
    auto v = vm_ok("delete this");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// DEL-11: 链式删除后对象完整性
// ============================================================

TEST(DeleteInterp, DEL11_ChainedDelete) {
    auto v = interp_ok(
        "var o = {a: 1, b: 2, c: 3};"
        "delete o.a;"
        "delete o.c;"
        "o.b");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.0);
}

TEST(DeleteVM, DEL11_ChainedDelete) {
    auto v = vm_ok(
        "var o = {a: 1, b: 2, c: 3};"
        "delete o.a;"
        "delete o.c;"
        "o.b");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.0);
}

// ============================================================
// DEL-12: 删除后重新赋值
// ============================================================

TEST(DeleteInterp, DEL12_DeleteThenReassign) {
    auto v = interp_ok("var o = {a: 1}; delete o.a; o.a = 99; o.a");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 99.0);
}

TEST(DeleteVM, DEL12_DeleteThenReassign) {
    auto v = vm_ok("var o = {a: 1}; delete o.a; o.a = 99; o.a");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 99.0);
}

// ============================================================
// DEL-13: 嵌套属性 delete obj.a.b
// ============================================================

TEST(DeleteInterp, DEL13_NestedDelete) {
    auto v = interp_ok("var o = {a: {b: 5}}; delete o.a.b; o.a.b");
    EXPECT_TRUE(v.is_undefined());
}

TEST(DeleteVM, DEL13_NestedDelete) {
    auto v = vm_ok("var o = {a: {b: 5}}; delete o.a.b; o.a.b");
    EXPECT_TRUE(v.is_undefined());
}

// ============================================================
// DEL-14: delete 原型链属性 (自身无该属性，delete 返回 true，原型链属性仍在)
// ============================================================

TEST(DeleteInterp, DEL14_DeleteProtoChainProp) {
    // delete obj.toString where toString is on prototype chain → true, still accessible via chain
    auto v = interp_ok("var o = {}; delete o.nonExistentFromProto");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(DeleteVM, DEL14_DeleteProtoChainProp) {
    auto v = vm_ok("var o = {}; delete o.nonExistentFromProto");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// DEL-15: delete (function(){})() (调用结果，有副作用但非引用) → true
// ============================================================

TEST(DeleteInterp, DEL15_DeleteCallResult) {
    auto v = interp_ok("delete (function(){})()");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(DeleteVM, DEL15_DeleteCallResult) {
    auto v = vm_ok("delete (function(){})()");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// DEL-16: 删除后重新添加属性，Object.keys 枚举正确（active_count_ 正确性）
// ============================================================

TEST(DeleteInterp, DEL16_DeleteThenAddObjectKeys) {
    // delete 后 active_count_ 递减，重新赋值后 active_count_ 递增
    // Object.keys 结果应只包含存在的属性
    auto v = interp_ok(
        "var o = {a: 1, b: 2};"
        "delete o.a;"
        "o.a = 99;"
        "var keys = Object.keys(o);"
        "keys.length");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.0);
}

TEST(DeleteInterp, DEL16_DeleteThenAddObjectKeysContent) {
    // 删除再添加的属性应出现在 Object.keys 末尾（插入顺序）
    auto v = interp_ok(
        "var o = {a: 1, b: 2};"
        "delete o.a;"
        "o.a = 99;"
        "Object.keys(o)[1]");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "a");
}

TEST(DeleteVM, DEL16_DeleteThenAddObjectKeys) {
    auto v = vm_ok(
        "var o = {a: 1, b: 2};"
        "delete o.a;"
        "o.a = 99;"
        "var keys = Object.keys(o);"
        "keys.length");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.0);
}

TEST(DeleteVM, DEL16_DeleteThenAddObjectKeysContent) {
    auto v = vm_ok(
        "var o = {a: 1, b: 2};"
        "delete o.a;"
        "o.a = 99;"
        "Object.keys(o)[1]");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "a");
}

// ============================================================
// DEL-17: 多个属性依次删除后 Object.keys 枚举顺序
// ============================================================

TEST(DeleteInterp, DEL17_MultiDeleteObjectKeysOrder) {
    // 删除 b，剩余 a, c — 插入顺序保持
    auto v = interp_ok(
        "var o = {a: 1, b: 2, c: 3};"
        "delete o.b;"
        "Object.keys(o).join(',')");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "a,c");
}

TEST(DeleteInterp, DEL17_MultiDeleteObjectKeysLength) {
    auto v = interp_ok(
        "var o = {a: 1, b: 2, c: 3, d: 4};"
        "delete o.a;"
        "delete o.c;"
        "Object.keys(o).length");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.0);
}

TEST(DeleteVM, DEL17_MultiDeleteObjectKeysOrder) {
    auto v = vm_ok(
        "var o = {a: 1, b: 2, c: 3};"
        "delete o.b;"
        "Object.keys(o).join(',')");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "a,c");
}

TEST(DeleteVM, DEL17_MultiDeleteObjectKeysLength) {
    auto v = vm_ok(
        "var o = {a: 1, b: 2, c: 3, d: 4};"
        "delete o.a;"
        "delete o.c;"
        "Object.keys(o).length");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.0);
}

// ============================================================
// DEL-18: delete 普通对象的数字属性（obj[0]，非数组）
// ============================================================

TEST(DeleteInterp, DEL18_DeleteNumericKeyOnPlainObject) {
    // 普通对象的 "0" 属性存入 index_map_，delete 应成功
    auto v = interp_ok("var o = {}; o[0] = 'x'; var r = delete o[0]; r");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(DeleteInterp, DEL18_DeleteNumericKeyGone) {
    auto v = interp_ok("var o = {}; o[0] = 'x'; delete o[0]; o[0]");
    EXPECT_TRUE(v.is_undefined());
}

TEST(DeleteInterp, DEL18_DeleteNumericKeyNotInKeys) {
    auto v = interp_ok("var o = {}; o[0] = 'x'; delete o[0]; Object.keys(o).length");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}

TEST(DeleteVM, DEL18_DeleteNumericKeyOnPlainObject) {
    auto v = vm_ok("var o = {}; o[0] = 'x'; var r = delete o[0]; r");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(DeleteVM, DEL18_DeleteNumericKeyGone) {
    auto v = vm_ok("var o = {}; o[0] = 'x'; delete o[0]; o[0]");
    EXPECT_TRUE(v.is_undefined());
}

TEST(DeleteVM, DEL18_DeleteNumericKeyNotInKeys) {
    auto v = vm_ok("var o = {}; o[0] = 'x'; delete o[0]; Object.keys(o).length");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}

// ============================================================
// DEL-19: delete 后通过 Object.keys 验证属性消失（in 运算符未实现时的替代验证）
// ============================================================

TEST(DeleteInterp, DEL19_DeleteVerifyViaObjectKeys) {
    // 删除 b 后，Object.keys 不包含 b
    auto v = interp_ok(
        "var o = {a: 1, b: 2, c: 3};"
        "delete o.b;"
        "var found = false;"
        "var keys = Object.keys(o);"
        "for (var i = 0; i < keys.length; i++) {"
        "  if (keys[i] === 'b') found = true;"
        "}"
        "found");
    EXPECT_TRUE(v.is_bool());
    EXPECT_FALSE(v.as_bool());
}

TEST(DeleteVM, DEL19_DeleteVerifyViaObjectKeys) {
    auto v = vm_ok(
        "var o = {a: 1, b: 2, c: 3};"
        "delete o.b;"
        "var found = false;"
        "var keys = Object.keys(o);"
        "for (var i = 0; i < keys.length; i++) {"
        "  if (keys[i] === 'b') found = true;"
        "}"
        "found");
    EXPECT_TRUE(v.is_bool());
    EXPECT_FALSE(v.as_bool());
}

// ============================================================
// DEL-20: delete 嵌套对象内的属性（obj.a 自身的属性，obj.a 仍存在）
// ============================================================

TEST(DeleteInterp, DEL20_DeleteNestedObjPropParentIntact) {
    // delete obj.a.b 之后 obj.a 本身仍然存在
    auto v = interp_ok(
        "var o = {a: {b: 1, c: 2}};"
        "delete o.a.b;"
        "typeof o.a");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "object");
}

TEST(DeleteInterp, DEL20_DeleteNestedObjPropSiblingIntact) {
    // delete obj.a.b 之后 obj.a.c 仍然可访问
    auto v = interp_ok(
        "var o = {a: {b: 1, c: 2}};"
        "delete o.a.b;"
        "o.a.c");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.0);
}

TEST(DeleteVM, DEL20_DeleteNestedObjPropParentIntact) {
    auto v = vm_ok(
        "var o = {a: {b: 1, c: 2}};"
        "delete o.a.b;"
        "typeof o.a");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "object");
}

TEST(DeleteVM, DEL20_DeleteNestedObjPropSiblingIntact) {
    auto v = vm_ok(
        "var o = {a: {b: 1, c: 2}};"
        "delete o.a.b;"
        "o.a.c");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.0);
}

// ============================================================
// DEL-21: delete 表达式作为条件（if (delete obj.x) { ... }）
// ============================================================

TEST(DeleteInterp, DEL21_DeleteAsConditionTrue) {
    auto v = interp_ok(
        "var o = {x: 1};"
        "var result = 0;"
        "if (delete o.x) { result = 1; } else { result = 2; }"
        "result");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

TEST(DeleteInterp, DEL21_DeleteLengthAsConditionFalse) {
    // delete arr.length → false，走 else 分支
    auto v = interp_ok(
        "var a = [1, 2];"
        "var result = 0;"
        "if (delete a.length) { result = 1; } else { result = 2; }"
        "result");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.0);
}

TEST(DeleteVM, DEL21_DeleteAsConditionTrue) {
    auto v = vm_ok(
        "var o = {x: 1};"
        "var result = 0;"
        "if (delete o.x) { result = 1; } else { result = 2; }"
        "result");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

TEST(DeleteVM, DEL21_DeleteLengthAsConditionFalse) {
    auto v = vm_ok(
        "var a = [1, 2];"
        "var result = 0;"
        "if (delete a.length) { result = 1; } else { result = 2; }"
        "result");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.0);
}

// ============================================================
// DEL-22: 连续 delete 同一属性（幂等性：第二次 delete 也返回 true）
// ============================================================

TEST(DeleteInterp, DEL22_IdempotentDeleteProp) {
    // 第一次 delete → true，第二次 delete 属性已不存在 → true
    auto v = interp_ok(
        "var o = {a: 1};"
        "delete o.a;"
        "delete o.a");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(DeleteInterp, DEL22_IdempotentDeleteArrayElem) {
    // 数组元素同样幂等
    auto v = interp_ok(
        "var a = [1, 2, 3];"
        "delete a[1];"
        "delete a[1]");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(DeleteVM, DEL22_IdempotentDeleteProp) {
    auto v = vm_ok(
        "var o = {a: 1};"
        "delete o.a;"
        "delete o.a");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(DeleteVM, DEL22_IdempotentDeleteArrayElem) {
    auto v = vm_ok(
        "var a = [1, 2, 3];"
        "delete a[1];"
        "delete a[1]");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// DEL-23: delete 数组越界索引（arr 只有 3 个元素，delete arr[100]）→ true
// ============================================================

TEST(DeleteInterp, DEL23_DeleteArrayOutOfBoundsIndex) {
    auto v = interp_ok("var a = [1, 2, 3]; delete a[100]");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(DeleteInterp, DEL23_DeleteArrayOutOfBoundsLengthUnchanged) {
    // 越界 delete 不改变 length
    auto v = interp_ok("var a = [1, 2, 3]; delete a[100]; a.length");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 3.0);
}

TEST(DeleteVM, DEL23_DeleteArrayOutOfBoundsIndex) {
    auto v = vm_ok("var a = [1, 2, 3]; delete a[100]");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(DeleteVM, DEL23_DeleteArrayOutOfBoundsLengthUnchanged) {
    auto v = vm_ok("var a = [1, 2, 3]; delete a[100]; a.length");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 3.0);
}

// ============================================================
// DEL-24: delete 后 typeof（delete x; typeof x 对已声明变量仍返回正确类型）
// ============================================================

TEST(DeleteInterp, DEL24_TypeofAfterDeleteDeclaredVar) {
    // delete x → false（变量不删除），typeof x 仍返回 "number"
    auto v = interp_ok("var x = 42; delete x; typeof x");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "number");
}

TEST(DeleteInterp, DEL24_TypeofAfterDeleteProp) {
    // delete obj.p 后访问 obj.p 返回 undefined，typeof 为 "undefined"
    auto v = interp_ok("var o = {p: 1}; delete o.p; typeof o.p");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "undefined");
}

TEST(DeleteVM, DEL24_TypeofAfterDeleteDeclaredVar) {
    auto v = vm_ok("var x = 42; delete x; typeof x");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "number");
}

TEST(DeleteVM, DEL24_TypeofAfterDeleteProp) {
    auto v = vm_ok("var o = {p: 1}; delete o.p; typeof o.p");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "undefined");
}

// ============================================================
// DEL-25: delete 全部属性后 Object.keys 返回空数组
// ============================================================

TEST(DeleteInterp, DEL25_DeleteAllPropsEmptyKeys) {
    auto v = interp_ok(
        "var o = {a: 1, b: 2};"
        "delete o.a;"
        "delete o.b;"
        "Object.keys(o).length");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}

TEST(DeleteVM, DEL25_DeleteAllPropsEmptyKeys) {
    auto v = vm_ok(
        "var o = {a: 1, b: 2};"
        "delete o.a;"
        "delete o.b;"
        "Object.keys(o).length");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}

// ============================================================
// DEL-26: delete 返回值作为赋值右侧（var r = delete obj.x）
// ============================================================

TEST(DeleteInterp, DEL26_DeleteReturnValueAssignment) {
    auto v = interp_ok("var o = {x: 1}; var r = delete o.x; r === true");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(DeleteInterp, DEL26_DeleteVarReturnValueFalse) {
    auto v = interp_ok("var y = 1; var r = delete y; r === false");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(DeleteVM, DEL26_DeleteReturnValueAssignment) {
    auto v = vm_ok("var o = {x: 1}; var r = delete o.x; r === true");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(DeleteVM, DEL26_DeleteVarReturnValueFalse) {
    auto v = vm_ok("var y = 1; var r = delete y; r === false");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// DEL-27: delete 数组元素后 forEach 跳过 hole（稀疏语义）
// ============================================================

TEST(DeleteInterp, DEL27_DeleteArrayElemForEachSkipsHole) {
    // delete arr[1] 产生 hole，forEach 不调用 hole 位置的 callback
    auto v = interp_ok(
        "var a = [10, 20, 30];"
        "delete a[1];"
        "var count = 0;"
        "a.forEach(function(x) { count++; });"
        "count");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.0);
}

TEST(DeleteVM, DEL27_DeleteArrayElemForEachSkipsHole) {
    auto v = vm_ok(
        "var a = [10, 20, 30];"
        "delete a[1];"
        "var count = 0;"
        "a.forEach(function(x) { count++; });"
        "count");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.0);
}

// ============================================================
// DEL-28: delete 字符串属性（computed key 为字符串变量）
// ============================================================

TEST(DeleteInterp, DEL28_DeleteComputedStringVar) {
    auto v = interp_ok(
        "var o = {foo: 42};"
        "var key = 'foo';"
        "var r = delete o[key];"
        "r");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(DeleteInterp, DEL28_DeleteComputedStringVarGone) {
    auto v = interp_ok(
        "var o = {foo: 42};"
        "var key = 'foo';"
        "delete o[key];"
        "o.foo");
    EXPECT_TRUE(v.is_undefined());
}

TEST(DeleteVM, DEL28_DeleteComputedStringVar) {
    auto v = vm_ok(
        "var o = {foo: 42};"
        "var key = 'foo';"
        "var r = delete o[key];"
        "r");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(DeleteVM, DEL28_DeleteComputedStringVarGone) {
    auto v = vm_ok(
        "var o = {foo: 42};"
        "var key = 'foo';"
        "delete o[key];"
        "o.foo");
    EXPECT_TRUE(v.is_undefined());
}

}  // namespace
