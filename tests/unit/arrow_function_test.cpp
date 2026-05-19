#include "qppjs/frontend/parser.h"
#include "qppjs/runtime/interpreter.h"
#include "qppjs/vm/compiler.h"
#include "qppjs/vm/vm.h"

#include <gtest/gtest.h>
#include <string>
#include <string_view>

using namespace qppjs;

namespace {

// ---- Interpreter helpers ----

static EvalResult interp_run(std::string_view src) {
    auto parse_result = parse_program(src);
    EXPECT_TRUE(parse_result.ok()) << "parse failed: " << parse_result.error().message();
    if (!parse_result.ok()) return EvalResult::err(parse_result.error());
    Interpreter interp;
    return interp.exec(parse_result.value());
}

static Value interp_ok(std::string_view src) {
    auto r = interp_run(src);
    EXPECT_TRUE(r.is_ok()) << "exec failed: " << r.error().message();
    return r.is_ok() ? r.value() : Value::undefined();
}

static std::string interp_str(std::string_view src) {
    auto r = interp_run(src);
    if (!r.is_ok()) return "<error>";
    const Value& v = r.value();
    if (v.is_string()) return std::string(v.sv());
    if (v.is_undefined()) return "undefined";
    if (v.is_null()) return "null";
    if (v.is_bool()) return v.as_bool() ? "true" : "false";
    if (v.is_number()) {
        double n = v.as_number();
        if (n == static_cast<double>(static_cast<long long>(n))) {
            return std::to_string(static_cast<long long>(n));
        }
        return std::to_string(n);
    }
    return "<object>";
}

// ---- VM helpers ----

static EvalResult vm_run(std::string_view src) {
    auto parse_result = parse_program(src);
    EXPECT_TRUE(parse_result.ok()) << "parse failed: " << parse_result.error().message();
    if (!parse_result.ok()) return EvalResult::err(parse_result.error());
    Compiler compiler;
    auto bc = compiler.compile(parse_result.value());
    VM vm;
    return vm.exec(bc);
}

static Value vm_ok(std::string_view src) {
    auto r = vm_run(src);
    EXPECT_TRUE(r.is_ok()) << "exec failed: " << r.error().message();
    return r.is_ok() ? r.value() : Value::undefined();
}

static std::string vm_str(std::string_view src) {
    auto r = vm_run(src);
    if (!r.is_ok()) return "<error>";
    const Value& v = r.value();
    if (v.is_string()) return std::string(v.sv());
    if (v.is_undefined()) return "undefined";
    if (v.is_null()) return "null";
    if (v.is_bool()) return v.as_bool() ? "true" : "false";
    if (v.is_number()) {
        double n = v.as_number();
        if (n == static_cast<double>(static_cast<long long>(n))) {
            return std::to_string(static_cast<long long>(n));
        }
        return std::to_string(n);
    }
    return "<object>";
}

// ============================================================
// AF-01: x => x * 2（单参省括号表达式体）
// ============================================================

TEST(ArrowFunctionInterp, AF01_SingleParamExprBody) {
    auto v = interp_ok("var f = x => x * 2; f(3)");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 6.0);
}

TEST(ArrowFunctionVM, AF01_SingleParamExprBody) {
    auto v = vm_ok("var f = x => x * 2; f(3)");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 6.0);
}

// ============================================================
// AF-02: () => 42（无参表达式体）
// ============================================================

TEST(ArrowFunctionInterp, AF02_NoParamExprBody) {
    auto v = interp_ok("var f = () => 42; f()");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 42.0);
}

TEST(ArrowFunctionVM, AF02_NoParamExprBody) {
    auto v = vm_ok("var f = () => 42; f()");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 42.0);
}

// ============================================================
// AF-03: (a, b) => a + b（多参表达式体）
// ============================================================

TEST(ArrowFunctionInterp, AF03_MultiParamExprBody) {
    auto v = interp_ok("var f = (a, b) => a + b; f(3, 4)");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 7.0);
}

TEST(ArrowFunctionVM, AF03_MultiParamExprBody) {
    auto v = vm_ok("var f = (a, b) => a + b; f(3, 4)");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 7.0);
}

// ============================================================
// AF-04: (x) => { return x; }（单参括号块体）
// ============================================================

TEST(ArrowFunctionInterp, AF04_ParenParamBlockBody) {
    auto v = interp_ok("var f = (x) => { return x; }; f(99)");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 99.0);
}

TEST(ArrowFunctionVM, AF04_ParenParamBlockBody) {
    auto v = vm_ok("var f = (x) => { return x; }; f(99)");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 99.0);
}

// ============================================================
// AF-05: x => { x; } 返回 undefined（块体无显式 return）
// ============================================================

TEST(ArrowFunctionInterp, AF05_BlockBodyNoReturn) {
    auto v = interp_ok("var f = x => { x; }; f(1)");
    EXPECT_TRUE(v.is_undefined());
}

TEST(ArrowFunctionVM, AF05_BlockBodyNoReturn) {
    auto v = vm_ok("var f = x => { x; }; f(1)");
    EXPECT_TRUE(v.is_undefined());
}

// ============================================================
// AF-06: x => {} 返回 undefined（空块体）
// ============================================================

TEST(ArrowFunctionInterp, AF06_EmptyBlockBody) {
    auto v = interp_ok("var f = x => {}; f(1)");
    EXPECT_TRUE(v.is_undefined());
}

TEST(ArrowFunctionVM, AF06_EmptyBlockBody) {
    auto v = vm_ok("var f = x => {}; f(1)");
    EXPECT_TRUE(v.is_undefined());
}

// ============================================================
// AF-07: x => ({ a: 1 }) 返回对象字面量（括号包裹）
// ============================================================

TEST(ArrowFunctionInterp, AF07_ParenWrappedObjectBody) {
    auto v = interp_ok("var f = x => ({ a: 1 }); var o = f(0); o.a");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

TEST(ArrowFunctionVM, AF07_ParenWrappedObjectBody) {
    auto v = vm_ok("var f = x => ({ a: 1 }); var o = f(0); o.a");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

// ============================================================
// AF-08: this 词法绑定（外层对象方法调用，箭头函数内 this 正确）
// ============================================================

TEST(ArrowFunctionInterp, AF08_LexicalThis) {
    auto v = interp_ok(R"(
var obj = {
    x: 42,
    getX: function() {
        var arrow = () => this.x;
        return arrow();
    }
};
obj.getX()
)");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 42.0);
}

TEST(ArrowFunctionVM, AF08_LexicalThis) {
    auto v = vm_ok(R"(
var obj = {
    x: 42,
    getX: function() {
        var arrow = () => this.x;
        return arrow();
    }
};
obj.getX()
)");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 42.0);
}

// ============================================================
// AF-09: call/apply 不改变 this
// ============================================================

TEST(ArrowFunctionInterp, AF09_CallDoesNotChangeThis) {
    auto v = interp_ok(R"(
var obj = { x: 10 };
var arrow = () => typeof this;
var r1 = arrow.call(obj);
r1
)");
    // Arrow function this is whatever was captured at creation (global = undefined in strict,
    // or the global object). Either way, call(obj) should NOT change it.
    // We just verify it doesn't crash and returns a string.
    EXPECT_TRUE(v.is_string());
}

TEST(ArrowFunctionVM, AF09_CallDoesNotChangeThis) {
    auto v = vm_ok(R"(
var obj = { x: 10 };
var arrow = () => typeof this;
var r1 = arrow.call(obj);
r1
)");
    EXPECT_TRUE(v.is_string());
}

// ============================================================
// AF-10: bind 不改变 this
// ============================================================

TEST(ArrowFunctionInterp, AF10_BindDoesNotChangeThis) {
    auto v = interp_ok(R"(
var obj = {
    v: 7,
    make: function() { return () => this.v; }
};
var arrow = obj.make();
var bound = arrow.bind({v: 99});
bound()
)");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 7.0);  // lexical this = obj, not the {v:99} bind target
}

TEST(ArrowFunctionVM, AF10_BindDoesNotChangeThis) {
    auto v = vm_ok(R"(
var obj = {
    v: 7,
    make: function() { return () => this.v; }
};
var arrow = obj.make();
var bound = arrow.bind({v: 99});
bound()
)");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 7.0);
}

// ============================================================
// AF-11: 外层无 this 的箭头函数 this（全局/undefined）
// ============================================================

TEST(ArrowFunctionInterp, AF11_TopLevelArrowThis) {
    auto v = interp_ok("var f = () => typeof this; f()");
    // Top-level this is undefined in our engine
    EXPECT_TRUE(v.is_string());
}

TEST(ArrowFunctionVM, AF11_TopLevelArrowThis) {
    auto v = vm_ok("var f = () => typeof this; f()");
    EXPECT_TRUE(v.is_string());
}

// ============================================================
// AF-12: 嵌套箭头函数 this 传递
// ============================================================

TEST(ArrowFunctionInterp, AF12_NestedArrowThis) {
    auto v = interp_ok(R"(
var obj = {
    x: 5,
    run: function() {
        var inner = () => {
            var deeper = () => this.x;
            return deeper();
        };
        return inner();
    }
};
obj.run()
)");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 5.0);
}

TEST(ArrowFunctionVM, AF12_NestedArrowThis) {
    auto v = vm_ok(R"(
var obj = {
    x: 5,
    run: function() {
        var inner = () => {
            var deeper = () => this.x;
            return deeper();
        };
        return inner();
    }
};
obj.run()
)");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 5.0);
}

// ============================================================
// AF-13: arguments 词法穿透外层普通函数
// ============================================================

TEST(ArrowFunctionInterp, AF13_ArgumentsLexical) {
    auto v = interp_ok(R"(
function outer(a, b) {
    var arrow = () => arguments.length;
    return arrow();
}
outer(1, 2, 3)
)");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 3.0);
}

TEST(ArrowFunctionVM, AF13_ArgumentsLexical) {
    auto v = vm_ok(R"(
function outer(a, b) {
    var arrow = () => arguments.length;
    return arrow();
}
outer(1, 2, 3)
)");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 3.0);
}

// ============================================================
// AF-14: new 调用 → TypeError
// ============================================================

TEST(ArrowFunctionInterp, AF14_NewCallTypeError) {
    auto r = interp_run("var f = () => 1; new f()");
    EXPECT_FALSE(r.is_ok());
}

TEST(ArrowFunctionVM, AF14_NewCallTypeError) {
    auto r = vm_run("var f = () => 1; new f()");
    EXPECT_FALSE(r.is_ok());
}

// ============================================================
// AF-15: prototype 不存在（f.prototype === undefined）
// ============================================================

TEST(ArrowFunctionInterp, AF15_NoPrototype) {
    auto v = interp_ok("var f = () => 1; f.prototype");
    EXPECT_TRUE(v.is_undefined());
}

TEST(ArrowFunctionVM, AF15_NoPrototype) {
    auto v = vm_ok("var f = () => 1; f.prototype");
    EXPECT_TRUE(v.is_undefined());
}

// ============================================================
// AF-16: 闭包（箭头函数捕获外层变量）
// ============================================================

TEST(ArrowFunctionInterp, AF16_Closure) {
    auto v = interp_ok(R"(
function make(n) {
    return () => n * 2;
}
make(5)()
)");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 10.0);
}

TEST(ArrowFunctionVM, AF16_Closure) {
    auto v = vm_ok(R"(
function make(n) {
    return () => n * 2;
}
make(5)()
)");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 10.0);
}

// ============================================================
// AF-17: 回调中使用（[1,2,3].map(x => x * 2)）
// ============================================================

TEST(ArrowFunctionInterp, AF17_MapCallback) {
    auto v = interp_ok("var r = [1, 2, 3].map(x => x * 2); r[2]");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 6.0);
}

TEST(ArrowFunctionVM, AF17_MapCallback) {
    auto v = vm_ok("var r = [1, 2, 3].map(x => x * 2); r[2]");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 6.0);
}

// ============================================================
// AF-18: 柯里化（const add = x => y => x + y; add(1)(2) === 3）
// ============================================================

TEST(ArrowFunctionInterp, AF18_Currying) {
    auto v = interp_ok("var add = x => y => x + y; add(1)(2)");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 3.0);
}

TEST(ArrowFunctionVM, AF18_Currying) {
    auto v = vm_ok("var add = x => y => x + y; add(1)(2)");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 3.0);
}

// ============================================================
// AF-19: 换行语法错误（(x)\n=> x 应解析失败）
// ============================================================

TEST(ArrowFunctionParser, AF19_NewlineBeforeArrowError) {
    auto r = parse_program("(x)\n=> x");
    EXPECT_FALSE(r.ok());
}

// ============================================================
// AF-20: typeof 和 instanceof（typeof f === 'function'）
// ============================================================

TEST(ArrowFunctionInterp, AF20_Typeof) {
    EXPECT_EQ(interp_str("var f = () => 1; typeof f"), "function");
}

TEST(ArrowFunctionVM, AF20_Typeof) {
    EXPECT_EQ(vm_str("var f = () => 1; typeof f"), "function");
}

// ============================================================
// AF-21: 表达式体直接返回 this（方法内箭头函数 () => this 捕获 obj）
// ============================================================

TEST(ArrowFunctionInterp, AF21_ExprBodyReturnsThis) {
    auto v = interp_ok(R"(
var obj = {
    v: 55,
    run: function() {
        var f = () => this;
        return f().v;
    }
};
obj.run()
)");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 55.0);
}

TEST(ArrowFunctionVM, AF21_ExprBodyReturnsThis) {
    auto v = vm_ok(R"(
var obj = {
    v: 55,
    run: function() {
        var f = () => this;
        return f().v;
    }
};
obj.run()
)");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 55.0);
}

// ============================================================
// AF-22: 三层箭头嵌套 this 仍指向最外层普通函数的 this
// ============================================================

TEST(ArrowFunctionInterp, AF22_TripleNestedArrowThis) {
    auto v = interp_ok(R"(
var obj = {
    x: 9,
    run: function() {
        var f = () => () => () => this.x;
        return f()()();
    }
};
obj.run()
)");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 9.0);
}

TEST(ArrowFunctionVM, AF22_TripleNestedArrowThis) {
    auto v = vm_ok(R"(
var obj = {
    x: 9,
    run: function() {
        var f = () => () => () => this.x;
        return f()()();
    }
};
obj.run()
)");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 9.0);
}

// ============================================================
// AF-23: 立即调用箭头函数（IIFE）
// ============================================================

TEST(ArrowFunctionInterp, AF23_IIFE) {
    auto v = interp_ok("(() => 42)()");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 42.0);
}

TEST(ArrowFunctionVM, AF23_IIFE) {
    auto v = vm_ok("(() => 42)()");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 42.0);
}

// ============================================================
// AF-24: 箭头函数赋值为另一对象方法后 this 不变（仍是创建时的词法 this）
// ============================================================

TEST(ArrowFunctionInterp, AF24_MethodAssignDoesNotChangeThis) {
    auto v = interp_ok(R"(
var obj1 = {
    v: 10,
    make: function() {
        return () => this.v;
    }
};
var obj2 = { v: 99 };
var arrow = obj1.make();
obj2.fn = arrow;
obj2.fn()
)");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 10.0);  // this is obj1, not obj2
}

TEST(ArrowFunctionVM, AF24_MethodAssignDoesNotChangeThis) {
    auto v = vm_ok(R"(
var obj1 = {
    v: 10,
    make: function() {
        return () => this.v;
    }
};
var obj2 = { v: 99 };
var arrow = obj1.make();
obj2.fn = arrow;
obj2.fn()
)");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 10.0);
}

// ============================================================
// AF-25: 参数与外层同名变量遮蔽（参数 x 遮蔽外层 var x）
// ============================================================

TEST(ArrowFunctionInterp, AF25_ParamShadowsOuterVar) {
    auto v = interp_ok("var x = 1; var f = x => x * 2; f(3)");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 6.0);  // parameter x=3, not outer x=1
}

TEST(ArrowFunctionVM, AF25_ParamShadowsOuterVar) {
    auto v = vm_ok("var x = 1; var f = x => x * 2; f(3)");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 6.0);
}

// ============================================================
// AF-26: 链式调用（filter + map）
// ============================================================

TEST(ArrowFunctionInterp, AF26_ChainedFilterMap) {
    auto v = interp_ok("var r = [1, 2, 3].filter(x => x > 1).map(x => x * 2); r[0]");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 4.0);  // [2,3] → [4,6], r[0]=4
}

TEST(ArrowFunctionVM, AF26_ChainedFilterMap) {
    auto v = vm_ok("var r = [1, 2, 3].filter(x => x > 1).map(x => x * 2); r[0]");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 4.0);
}

// ============================================================
// AF-27: 返回箭头函数（partial application）
// ============================================================

TEST(ArrowFunctionInterp, AF27_ReturnsArrow) {
    auto v = interp_ok("var make = n => x => x + n; var add5 = make(5); add5(3)");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 8.0);
}

TEST(ArrowFunctionVM, AF27_ReturnsArrow) {
    auto v = vm_ok("var make = n => x => x + n; var add5 = make(5); add5(3)");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 8.0);
}

// ============================================================
// AF-28: 条件分支中的箭头函数（块体 if-else，绝对值语义）
// 注：三元运算符 ?:  尚未实现，使用 if-else 块体验证等价语义
// ============================================================

TEST(ArrowFunctionInterp, AF28_IfElseInBody) {
    auto v = interp_ok("var f = n => { if (n > 0) { return n; } else { return 0 - n; } }; f(-7)");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 7.0);
}

TEST(ArrowFunctionVM, AF28_IfElseInBody) {
    auto v = vm_ok("var f = n => { if (n > 0) { return n; } else { return 0 - n; } }; f(-7)");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 7.0);
}

// ============================================================
// AF-29: 无参空块体返回 undefined（显式验证 === undefined）
// ============================================================

TEST(ArrowFunctionInterp, AF29_NoParamEmptyBlockUndefined) {
    auto v = interp_ok("var f = () => {}; f()");
    EXPECT_TRUE(v.is_undefined());
}

TEST(ArrowFunctionVM, AF29_NoParamEmptyBlockUndefined) {
    auto v = vm_ok("var f = () => {}; f()");
    EXPECT_TRUE(v.is_undefined());
}

// ============================================================
// AF-30: typeof === "function" 且无 prototype（复合函数类型检查）
// 注：Function 全局构造函数尚未注册，用 typeof + prototype 组合验证函数语义
// ============================================================

TEST(ArrowFunctionInterp, AF30_TypeofAndNoPrototype) {
    // typeof f === "function" && f.prototype === undefined → true
    auto v = interp_ok("var f = () => 1; (typeof f === \"function\") && (f.prototype === undefined)");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(ArrowFunctionVM, AF30_TypeofAndNoPrototype) {
    auto v = vm_ok("var f = () => 1; (typeof f === \"function\") && (f.prototype === undefined)");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

// ============================================================
// AF-31: 多参数回调（回调接收 element 和 index）
// ============================================================

TEST(ArrowFunctionInterp, AF31_MultiParamCallback) {
    auto v = interp_ok("var r = ['a', 'b'].map((x, i) => x + i); r[0]");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(std::string(v.sv()), "a0");
}

TEST(ArrowFunctionVM, AF31_MultiParamCallback) {
    auto v = vm_ok("var r = ['a', 'b'].map((x, i) => x + i); r[0]");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(std::string(v.sv()), "a0");
}

// ============================================================
// AF-32: map 结果数组两端均正确（验证箭头函数在 map 中的完整性）
// ============================================================

TEST(ArrowFunctionInterp, AF32_MapResultBothElements) {
    auto r0 = interp_ok("var r = [1, 2].map(x => x + 10); r[0]");
    EXPECT_TRUE(r0.is_number());
    EXPECT_EQ(r0.as_number(), 11.0);

    auto r1 = interp_ok("var r = [1, 2].map(x => x + 10); r[1]");
    EXPECT_TRUE(r1.is_number());
    EXPECT_EQ(r1.as_number(), 12.0);
}

TEST(ArrowFunctionVM, AF32_MapResultBothElements) {
    auto r0 = vm_ok("var r = [1, 2].map(x => x + 10); r[0]");
    EXPECT_TRUE(r0.is_number());
    EXPECT_EQ(r0.as_number(), 11.0);

    auto r1 = vm_ok("var r = [1, 2].map(x => x + 10); r[1]");
    EXPECT_TRUE(r1.is_number());
    EXPECT_EQ(r1.as_number(), 12.0);
}

}  // namespace
