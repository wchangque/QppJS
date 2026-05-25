#include "qppjs/frontend/parser.h"
#include "qppjs/runtime/interpreter.h"
#include "qppjs/runtime/js_object.h"
#include "qppjs/vm/compiler.h"
#include "qppjs/vm/vm.h"

#include <gtest/gtest.h>

#include <string_view>

using namespace qppjs;

namespace {

static bool parse_ok(std::string_view source) {
    return parse_program(source).ok();
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
// Array spread tests — Interpreter
// ============================================================

TEST(SpreadRestInterp, ArraySpreadBasic) {
    // [1, ...arr, 2] should flatten
    auto v = interp_ok("var arr = [3, 4]; [1, ...arr, 2]");
    ASSERT_TRUE(v.is_object());
    auto* obj = static_cast<JSObject*>(v.as_object_raw());
    EXPECT_EQ(obj->array_length_, 4u);
    EXPECT_EQ(obj->elements_[0].as_number(), 1.0);
    EXPECT_EQ(obj->elements_[1].as_number(), 3.0);
    EXPECT_EQ(obj->elements_[2].as_number(), 4.0);
    EXPECT_EQ(obj->elements_[3].as_number(), 2.0);
}

TEST(SpreadRestInterp, ArraySpreadEmpty) {
    auto v = interp_ok("var arr = []; [1, ...arr, 2]");
    ASSERT_TRUE(v.is_object());
    auto* obj = static_cast<JSObject*>(v.as_object_raw());
    EXPECT_EQ(obj->array_length_, 2u);
}

TEST(SpreadRestInterp, ArraySpreadMultiple) {
    auto v = interp_ok("[...[1, 2], ...[3, 4]]");
    ASSERT_TRUE(v.is_object());
    auto* obj = static_cast<JSObject*>(v.as_object_raw());
    EXPECT_EQ(obj->array_length_, 4u);
    EXPECT_EQ(obj->elements_[0].as_number(), 1.0);
    EXPECT_EQ(obj->elements_[3].as_number(), 4.0);
}

TEST(SpreadRestInterp, ArraySpreadString) {
    auto v = interp_ok("[...'abc']");
    ASSERT_TRUE(v.is_object());
    auto* obj = static_cast<JSObject*>(v.as_object_raw());
    EXPECT_EQ(obj->array_length_, 3u);
    EXPECT_EQ(obj->elements_[0].as_string(), "a");
    EXPECT_EQ(obj->elements_[1].as_string(), "b");
    EXPECT_EQ(obj->elements_[2].as_string(), "c");
}

// ============================================================
// Function call argument spread — Interpreter
// ============================================================

TEST(SpreadRestInterp, CallArgSpread) {
    auto v = interp_ok(R"(
        function sum(a, b, c) { return a + b + c; }
        var args = [1, 2, 3];
        sum(...args)
    )");
    EXPECT_EQ(v.as_number(), 6.0);
}

TEST(SpreadRestInterp, CallArgSpreadMixed) {
    auto v = interp_ok(R"(
        function sum(a, b, c) { return a + b + c; }
        sum(1, ...[2, 3])
    )");
    EXPECT_EQ(v.as_number(), 6.0);
}

TEST(SpreadRestInterp, MethodCallSpread) {
    auto v = interp_ok(R"(
        var obj = {
            sum: function(a, b) { return a + b; }
        };
        var args = [3, 4];
        obj.sum(...args)
    )");
    EXPECT_EQ(v.as_number(), 7.0);
}

// ============================================================
// Rest parameters — Interpreter
// ============================================================

TEST(SpreadRestInterp, RestParamBasic) {
    auto v = interp_ok(R"(
        function f(a, ...rest) { return rest; }
        f(1, 2, 3)
    )");
    ASSERT_TRUE(v.is_object());
    auto* obj = static_cast<JSObject*>(v.as_object_raw());
    EXPECT_EQ(obj->array_length_, 2u);
    EXPECT_EQ(obj->elements_[0].as_number(), 2.0);
    EXPECT_EQ(obj->elements_[1].as_number(), 3.0);
}

TEST(SpreadRestInterp, RestParamEmpty) {
    auto v = interp_ok(R"(
        function f(a, ...rest) { return rest; }
        f(1)
    )");
    ASSERT_TRUE(v.is_object());
    auto* obj = static_cast<JSObject*>(v.as_object_raw());
    EXPECT_EQ(obj->array_length_, 0u);
}

TEST(SpreadRestInterp, RestParamOnly) {
    auto v = interp_ok(R"(
        function f(...args) { return args; }
        f(1, 2, 3)
    )");
    ASSERT_TRUE(v.is_object());
    auto* obj = static_cast<JSObject*>(v.as_object_raw());
    EXPECT_EQ(obj->array_length_, 3u);
}

TEST(SpreadRestInterp, ArrowRestParam) {
    auto v = interp_ok(R"(
        var f = (...args) => args;
        f(1, 2, 3)
    )");
    ASSERT_TRUE(v.is_object());
    auto* obj = static_cast<JSObject*>(v.as_object_raw());
    EXPECT_EQ(obj->array_length_, 3u);
}

TEST(SpreadRestInterp, ArrowSingleRestParam) {
    auto v = interp_ok(R"(
        var f = (...rest) => rest.length;
        f(1, 2, 3)
    )");
    EXPECT_EQ(v.as_number(), 3.0);
}

// ============================================================
// Array spread tests — VM
// ============================================================

TEST(SpreadRestVM, ArraySpreadBasic) {
    auto v = vm_ok("var arr = [3, 4]; [1, ...arr, 2]");
    ASSERT_TRUE(v.is_object());
    auto* obj = static_cast<JSObject*>(v.as_object_raw());
    EXPECT_EQ(obj->array_length_, 4u);
    EXPECT_EQ(obj->elements_[0].as_number(), 1.0);
    EXPECT_EQ(obj->elements_[1].as_number(), 3.0);
    EXPECT_EQ(obj->elements_[2].as_number(), 4.0);
    EXPECT_EQ(obj->elements_[3].as_number(), 2.0);
}

TEST(SpreadRestVM, ArraySpreadEmpty) {
    auto v = vm_ok("var arr = []; [1, ...arr, 2]");
    ASSERT_TRUE(v.is_object());
    auto* obj = static_cast<JSObject*>(v.as_object_raw());
    EXPECT_EQ(obj->array_length_, 2u);
}

TEST(SpreadRestVM, ArraySpreadMultiple) {
    auto v = vm_ok("[...[1, 2], ...[3, 4]]");
    ASSERT_TRUE(v.is_object());
    auto* obj = static_cast<JSObject*>(v.as_object_raw());
    EXPECT_EQ(obj->array_length_, 4u);
    EXPECT_EQ(obj->elements_[0].as_number(), 1.0);
    EXPECT_EQ(obj->elements_[3].as_number(), 4.0);
}

TEST(SpreadRestVM, ArraySpreadString) {
    auto v = vm_ok("[...'abc']");
    ASSERT_TRUE(v.is_object());
    auto* obj = static_cast<JSObject*>(v.as_object_raw());
    EXPECT_EQ(obj->array_length_, 3u);
    EXPECT_EQ(obj->elements_[0].as_string(), "a");
}

// ============================================================
// Function call argument spread — VM
// ============================================================

TEST(SpreadRestVM, CallArgSpread) {
    auto v = vm_ok(R"(
        function sum(a, b, c) { return a + b + c; }
        var args = [1, 2, 3];
        sum(...args)
    )");
    EXPECT_EQ(v.as_number(), 6.0);
}

TEST(SpreadRestVM, CallArgSpreadMixed) {
    auto v = vm_ok(R"(
        function sum(a, b, c) { return a + b + c; }
        sum(1, ...[2, 3])
    )");
    EXPECT_EQ(v.as_number(), 6.0);
}

TEST(SpreadRestVM, MethodCallSpread) {
    auto v = vm_ok(R"(
        var obj = {
            sum: function(a, b) { return a + b; }
        };
        var args = [3, 4];
        obj.sum(...args)
    )");
    EXPECT_EQ(v.as_number(), 7.0);
}

// ============================================================
// Rest parameters — VM
// ============================================================

TEST(SpreadRestVM, RestParamBasic) {
    auto v = vm_ok(R"(
        function f(a, ...rest) { return rest; }
        f(1, 2, 3)
    )");
    ASSERT_TRUE(v.is_object());
    auto* obj = static_cast<JSObject*>(v.as_object_raw());
    EXPECT_EQ(obj->array_length_, 2u);
    EXPECT_EQ(obj->elements_[0].as_number(), 2.0);
    EXPECT_EQ(obj->elements_[1].as_number(), 3.0);
}

TEST(SpreadRestVM, RestParamEmpty) {
    auto v = vm_ok(R"(
        function f(a, ...rest) { return rest; }
        f(1)
    )");
    ASSERT_TRUE(v.is_object());
    auto* obj = static_cast<JSObject*>(v.as_object_raw());
    EXPECT_EQ(obj->array_length_, 0u);
}

TEST(SpreadRestVM, RestParamOnly) {
    auto v = vm_ok(R"(
        function f(...args) { return args; }
        f(1, 2, 3)
    )");
    ASSERT_TRUE(v.is_object());
    auto* obj = static_cast<JSObject*>(v.as_object_raw());
    EXPECT_EQ(obj->array_length_, 3u);
}

TEST(SpreadRestVM, ArrowRestParam) {
    auto v = vm_ok(R"(
        var f = (...args) => args;
        f(1, 2, 3)
    )");
    ASSERT_TRUE(v.is_object());
    auto* obj = static_cast<JSObject*>(v.as_object_raw());
    EXPECT_EQ(obj->array_length_, 3u);
}

// ============================================================
// Spread + Rest combined
// ============================================================

TEST(SpreadRestInterp, SpreadAndRest) {
    auto v = interp_ok(R"(
        function f(a, ...rest) { return rest; }
        var extra = [3, 4, 5];
        f(1, 2, ...extra)
    )");
    ASSERT_TRUE(v.is_object());
    auto* obj = static_cast<JSObject*>(v.as_object_raw());
    EXPECT_EQ(obj->array_length_, 4u);
    EXPECT_EQ(obj->elements_[0].as_number(), 2.0);
    EXPECT_EQ(obj->elements_[1].as_number(), 3.0);
    EXPECT_EQ(obj->elements_[3].as_number(), 5.0);
}

TEST(SpreadRestVM, SpreadAndRest) {
    auto v = vm_ok(R"(
        function f(a, ...rest) { return rest; }
        var extra = [3, 4, 5];
        f(1, 2, ...extra)
    )");
    ASSERT_TRUE(v.is_object());
    auto* obj = static_cast<JSObject*>(v.as_object_raw());
    EXPECT_EQ(obj->array_length_, 4u);
    EXPECT_EQ(obj->elements_[0].as_number(), 2.0);
    EXPECT_EQ(obj->elements_[3].as_number(), 5.0);
}

// ============================================================
// new call with spread — Interpreter
// ============================================================

TEST(SpreadRestInterp, NewCallSpread) {
    auto v = interp_ok(R"(
        function Point(x, y) { this.x = x; this.y = y; }
        var args = [3, 4];
        new Point(...args)
    )");
    ASSERT_TRUE(v.is_object());
    auto* obj = static_cast<JSObject*>(v.as_object_raw());
    EXPECT_EQ(obj->get_property("x").as_number(), 3.0);
    EXPECT_EQ(obj->get_property("y").as_number(), 4.0);
}

TEST(SpreadRestVM, NewCallSpread) {
    auto v = vm_ok(R"(
        function Point(x, y) { this.x = x; this.y = y; }
        var args = [3, 4];
        new Point(...args)
    )");
    ASSERT_TRUE(v.is_object());
    auto* obj = static_cast<JSObject*>(v.as_object_raw());
    EXPECT_EQ(obj->get_property("x").as_number(), 3.0);
    EXPECT_EQ(obj->get_property("y").as_number(), 4.0);
}

// ============================================================
// E-01 ~ E-06: TypeError — non-iterable spread
// ============================================================

TEST(SpreadRestInterp, SpreadUndefinedInArrayThrows) {
    EXPECT_TRUE(interp_throws("[...undefined]"));
}

TEST(SpreadRestVM, SpreadUndefinedInArrayThrows) {
    EXPECT_TRUE(vm_throws("[...undefined]"));
}

TEST(SpreadRestInterp, SpreadNullInArrayThrows) {
    EXPECT_TRUE(interp_throws("[...null]"));
}

TEST(SpreadRestVM, SpreadNullInArrayThrows) {
    EXPECT_TRUE(vm_throws("[...null]"));
}

TEST(SpreadRestInterp, SpreadNumberInArrayThrows) {
    EXPECT_TRUE(interp_throws("[...123]"));
}

TEST(SpreadRestVM, SpreadNumberInArrayThrows) {
    EXPECT_TRUE(vm_throws("[...123]"));
}

TEST(SpreadRestInterp, SpreadBooleanInArrayThrows) {
    EXPECT_TRUE(interp_throws("[...true]"));
}

TEST(SpreadRestVM, SpreadBooleanInArrayThrows) {
    EXPECT_TRUE(vm_throws("[...true]"));
}

TEST(SpreadRestInterp, SpreadNullInCallArgsThrows) {
    EXPECT_TRUE(interp_throws("function f(a){} f(...null)"));
}

TEST(SpreadRestVM, SpreadNullInCallArgsThrows) {
    EXPECT_TRUE(vm_throws("function f(a){} f(...null)"));
}

TEST(SpreadRestInterp, SpreadUndefinedInCallArgsThrows) {
    EXPECT_TRUE(interp_throws("function f(a){} f(...undefined)"));
}

TEST(SpreadRestVM, SpreadUndefinedInCallArgsThrows) {
    EXPECT_TRUE(vm_throws("function f(a){} f(...undefined)"));
}

// ============================================================
// S-01 ~ S-02: SyntaxError — illegal rest param placement
// ============================================================

TEST(SpreadRestParser, RestNotLastParamIsSyntaxError) {
    // rest 不能是非最后参数
    EXPECT_FALSE(parse_ok("function f(...a, b){}"));
}

TEST(SpreadRestParser, RestWithDefaultIsSyntaxError) {
    // rest 参数不能有默认值
    EXPECT_FALSE(parse_ok("function f(...a=1){}"));
}

// ============================================================
// U-01: 字符串 Unicode 码点展开 — emoji 算 1 个元素
// ============================================================

TEST(SpreadRestInterp, SpreadEmojiIsOneElement) {
    // 4-byte UTF-8 emoji "😀" 应展开为 1 个元素，不是 2 个
    auto v = interp_ok("[...'😀']");
    ASSERT_TRUE(v.is_object());
    auto* obj = static_cast<JSObject*>(v.as_object_raw());
    EXPECT_EQ(obj->array_length_, 1u);
}

TEST(SpreadRestVM, SpreadEmojiIsOneElement) {
    auto v = vm_ok("[...'😀']");
    ASSERT_TRUE(v.is_object());
    auto* obj = static_cast<JSObject*>(v.as_object_raw());
    EXPECT_EQ(obj->array_length_, 1u);
}

// ============================================================
// H-01: 数组 hole 展开 — hole 读为 undefined
// ============================================================

TEST(SpreadRestInterp, SpreadHoleBecomesUndefined) {
    // [1,,3] 有一个 hole，展开后应得到 [1, undefined, 3]
    auto v = interp_ok("[...[1,,3]]");
    ASSERT_TRUE(v.is_object());
    auto* obj = static_cast<JSObject*>(v.as_object_raw());
    ASSERT_EQ(obj->array_length_, 3u);
    EXPECT_EQ(obj->elements_[0].as_number(), 1.0);
    EXPECT_TRUE(obj->elements_.find(1) == obj->elements_.end() ||
                obj->elements_[1].is_undefined());
    EXPECT_EQ(obj->elements_[2].as_number(), 3.0);
}

TEST(SpreadRestVM, SpreadHoleBecomesUndefined) {
    auto v = vm_ok("[...[1,,3]]");
    ASSERT_TRUE(v.is_object());
    auto* obj = static_cast<JSObject*>(v.as_object_raw());
    ASSERT_EQ(obj->array_length_, 3u);
    EXPECT_EQ(obj->elements_[0].as_number(), 1.0);
    EXPECT_TRUE(obj->elements_.find(1) == obj->elements_.end() ||
                obj->elements_[1].is_undefined());
    EXPECT_EQ(obj->elements_[2].as_number(), 3.0);
}

// ============================================================
// I-01: 自定义 Symbol.iterator 被展开
// ============================================================

TEST(SpreadRestInterp, CustomSymbolIteratorSpread) {
    auto v = interp_ok(R"(
        const obj = {};
        Object.defineProperty(obj, Symbol.iterator, {
            value: function() {
                let i = 0;
                return {
                    next: function() {
                        if (i < 3) return { value: i++ * 10, done: false };
                        return { value: undefined, done: true };
                    }
                };
            },
            enumerable: true,
            configurable: true,
            writable: true
        });
        [...obj]
    )");
    ASSERT_TRUE(v.is_object());
    auto* obj = static_cast<JSObject*>(v.as_object_raw());
    ASSERT_EQ(obj->array_length_, 3u);
    EXPECT_EQ(obj->elements_[0].as_number(), 0.0);
    EXPECT_EQ(obj->elements_[1].as_number(), 10.0);
    EXPECT_EQ(obj->elements_[2].as_number(), 20.0);
}

TEST(SpreadRestVM, CustomSymbolIteratorSpread) {
    auto v = vm_ok(R"(
        const obj = {};
        Object.defineProperty(obj, Symbol.iterator, {
            value: function() {
                let i = 0;
                return {
                    next: function() {
                        if (i < 3) return { value: i++ * 10, done: false };
                        return { value: undefined, done: true };
                    }
                };
            },
            enumerable: true,
            configurable: true,
            writable: true
        });
        [...obj]
    )");
    ASSERT_TRUE(v.is_object());
    auto* obj = static_cast<JSObject*>(v.as_object_raw());
    ASSERT_EQ(obj->array_length_, 3u);
    EXPECT_EQ(obj->elements_[0].as_number(), 0.0);
    EXPECT_EQ(obj->elements_[1].as_number(), 10.0);
    EXPECT_EQ(obj->elements_[2].as_number(), 20.0);
}

// ============================================================
// L-01 ~ L-02: function.length 不包含 rest 参数
// ============================================================

TEST(SpreadRestInterp, FunctionLengthExcludesRestOnly) {
    // function f(...rest){} → f.length === 0
    auto v = interp_ok("function f(...rest){} f.length === 0");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(SpreadRestVM, FunctionLengthExcludesRestOnly) {
    auto v = vm_ok("function f(...rest){} f.length === 0");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(SpreadRestInterp, FunctionLengthExcludesRestWithParams) {
    // function f(a, b, ...rest){} → f.length === 2
    auto v = interp_ok("function f(a, b, ...rest){} f.length === 2");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(SpreadRestVM, FunctionLengthExcludesRestWithParams) {
    auto v = vm_ok("function f(a, b, ...rest){} f.length === 2");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// A-01: rest 是真正的 Array（Array.isArray）
// ============================================================

TEST(SpreadRestInterp, RestParamIsRealArray) {
    auto v = interp_ok(R"(
        function f(...rest) { return Array.isArray(rest); }
        f(1, 2, 3)
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(SpreadRestVM, RestParamIsRealArray) {
    auto v = vm_ok(R"(
        function f(...rest) { return Array.isArray(rest); }
        f(1, 2, 3)
    )");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// G-01: rest 与 arguments 独立——arguments 含全部实参
// ============================================================

TEST(SpreadRestInterp, RestAndArgumentsAreIndependent) {
    // arguments 应包含所有实参，与 rest 独立
    auto v = interp_ok(R"(
        function f(a, ...rest) { return arguments.length; }
        f(1, 2, 3)
    )");
    EXPECT_EQ(v.as_number(), 3.0);
}

TEST(SpreadRestVM, RestAndArgumentsAreIndependent) {
    auto v = vm_ok(R"(
        function f(a, ...rest) { return arguments.length; }
        f(1, 2, 3)
    )");
    EXPECT_EQ(v.as_number(), 3.0);
}

// ============================================================
// C-01: 连续展开两个字符串——独立迭代
// ============================================================

TEST(SpreadRestInterp, ConsecutiveStringSpread) {
    auto v = interp_ok("[...'ab', ...'cd']");
    ASSERT_TRUE(v.is_object());
    auto* obj = static_cast<JSObject*>(v.as_object_raw());
    ASSERT_EQ(obj->array_length_, 4u);
    EXPECT_EQ(obj->elements_[0].as_string(), "a");
    EXPECT_EQ(obj->elements_[1].as_string(), "b");
    EXPECT_EQ(obj->elements_[2].as_string(), "c");
    EXPECT_EQ(obj->elements_[3].as_string(), "d");
}

TEST(SpreadRestVM, ConsecutiveStringSpread) {
    auto v = vm_ok("[...'ab', ...'cd']");
    ASSERT_TRUE(v.is_object());
    auto* obj = static_cast<JSObject*>(v.as_object_raw());
    ASSERT_EQ(obj->array_length_, 4u);
    EXPECT_EQ(obj->elements_[0].as_string(), "a");
    EXPECT_EQ(obj->elements_[1].as_string(), "b");
    EXPECT_EQ(obj->elements_[2].as_string(), "c");
    EXPECT_EQ(obj->elements_[3].as_string(), "d");
}

// ============================================================
// M1: Compiler SpreadElement at illegal position throws at runtime
// ============================================================

TEST(SpreadRestVM, SpreadInIllegalPositionThrows) {
    // var x = ...arr 语法上合法（parser 接受），但 compiler 应 emit throw
    EXPECT_TRUE(vm_throws("var arr = [1,2]; var x = (...arr)"));
}

// ============================================================
// M2: Spread array/string Symbol.iterator result (iterable iterator)
// ============================================================

TEST(SpreadRestInterp, SpreadArrayIteratorObject) {
    // [1,2][Symbol.iterator]() 返回迭代器，迭代器本身也是 iterable
    auto v = interp_ok("var iter = [1, 2][Symbol.iterator](); [...iter]");
    ASSERT_TRUE(v.is_object());
    auto* obj = static_cast<JSObject*>(v.as_object_raw());
    ASSERT_EQ(obj->array_length_, 2u);
    EXPECT_EQ(obj->elements_[0].as_number(), 1.0);
    EXPECT_EQ(obj->elements_[1].as_number(), 2.0);
}

TEST(SpreadRestVM, SpreadArrayIteratorObject) {
    auto v = vm_ok("var iter = [1, 2][Symbol.iterator](); [...iter]");
    ASSERT_TRUE(v.is_object());
    auto* obj = static_cast<JSObject*>(v.as_object_raw());
    ASSERT_EQ(obj->array_length_, 2u);
    EXPECT_EQ(obj->elements_[0].as_number(), 1.0);
    EXPECT_EQ(obj->elements_[1].as_number(), 2.0);
}

TEST(SpreadRestInterp, SpreadStringIteratorObject) {
    // 'ab'[Symbol.iterator]() 返回迭代器，迭代器本身也是 iterable
    auto v = interp_ok("var iter = 'ab'[Symbol.iterator](); [...iter]");
    ASSERT_TRUE(v.is_object());
    auto* obj = static_cast<JSObject*>(v.as_object_raw());
    ASSERT_EQ(obj->array_length_, 2u);
    EXPECT_EQ(obj->elements_[0].as_string(), "a");
    EXPECT_EQ(obj->elements_[1].as_string(), "b");
}

TEST(SpreadRestVM, SpreadStringIteratorObject) {
    auto v = vm_ok("var iter = 'ab'[Symbol.iterator](); [...iter]");
    ASSERT_TRUE(v.is_object());
    auto* obj = static_cast<JSObject*>(v.as_object_raw());
    ASSERT_EQ(obj->array_length_, 2u);
    EXPECT_EQ(obj->elements_[0].as_string(), "a");
    EXPECT_EQ(obj->elements_[1].as_string(), "b");
}

}  // namespace
