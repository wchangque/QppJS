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

static std::string interp_str(std::string_view src) {
    auto r = interp_run(src);
    if (!r.is_ok()) return "<error:" + r.error().message() + ">";
    const Value& v = r.value();
    if (v.is_string()) return v.as_string();
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

static bool interp_ok(std::string_view src) {
    auto r = interp_run(src);
    return r.is_ok();
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

static std::string vm_str(std::string_view src) {
    auto r = vm_run(src);
    if (!r.is_ok()) return "<error:" + r.error().message() + ">";
    const Value& v = r.value();
    if (v.is_string()) return v.as_string();
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

static bool vm_ok(std::string_view src) {
    auto r = vm_run(src);
    return r.is_ok();
}

// ============================================================
// SB-01: String(42) === "42"
// ============================================================

TEST(StringBooleanConstructorInterp, SB01_StringCoerceNumber) {
    EXPECT_EQ(interp_str("String(42)"), "42");
}
TEST(StringBooleanConstructorVM, SB01_StringCoerceNumber) {
    EXPECT_EQ(vm_str("String(42)"), "42");
}

// ============================================================
// SB-02: String() === ""
// ============================================================

TEST(StringBooleanConstructorInterp, SB02_StringNoArg) {
    EXPECT_EQ(interp_str("String()"), "");
}
TEST(StringBooleanConstructorVM, SB02_StringNoArg) {
    EXPECT_EQ(vm_str("String()"), "");
}

// ============================================================
// SB-03: String(null) === "null"
// ============================================================

TEST(StringBooleanConstructorInterp, SB03_StringNull) {
    EXPECT_EQ(interp_str("String(null)"), "null");
}
TEST(StringBooleanConstructorVM, SB03_StringNull) {
    EXPECT_EQ(vm_str("String(null)"), "null");
}

// ============================================================
// SB-04: String(undefined) === "undefined"
// ============================================================

TEST(StringBooleanConstructorInterp, SB04_StringUndefined) {
    EXPECT_EQ(interp_str("String(undefined)"), "undefined");
}
TEST(StringBooleanConstructorVM, SB04_StringUndefined) {
    EXPECT_EQ(vm_str("String(undefined)"), "undefined");
}

// ============================================================
// SB-05: String(Symbol("x")) === "Symbol(x)"
// ============================================================

TEST(StringBooleanConstructorInterp, SB05_StringSymbol) {
    EXPECT_EQ(interp_str("String(Symbol(\"x\"))"), "Symbol(x)");
}
TEST(StringBooleanConstructorVM, SB05_StringSymbol) {
    EXPECT_EQ(vm_str("String(Symbol(\"x\"))"), "Symbol(x)");
}

// ============================================================
// SB-06: typeof new String("x") === "object"
// ============================================================

TEST(StringBooleanConstructorInterp, SB06_NewStringTypeof) {
    EXPECT_EQ(interp_str("typeof new String(\"x\")"), "object");
}
TEST(StringBooleanConstructorVM, SB06_NewStringTypeof) {
    EXPECT_EQ(vm_str("typeof new String(\"x\")"), "object");
}

// ============================================================
// SB-07: typeof String("x") === "string"
// ============================================================

TEST(StringBooleanConstructorInterp, SB07_StringCallTypeof) {
    EXPECT_EQ(interp_str("typeof String(\"x\")"), "string");
}
TEST(StringBooleanConstructorVM, SB07_StringCallTypeof) {
    EXPECT_EQ(vm_str("typeof String(\"x\")"), "string");
}

// ============================================================
// SB-08: new String("ab") instanceof String
// ============================================================

TEST(StringBooleanConstructorInterp, SB08_NewStringInstanceof) {
    EXPECT_EQ(interp_str("new String(\"ab\") instanceof String"), "true");
}
TEST(StringBooleanConstructorVM, SB08_NewStringInstanceof) {
    EXPECT_EQ(vm_str("new String(\"ab\") instanceof String"), "true");
}

// ============================================================
// SB-09: new String("ab").length === 2
// ============================================================

TEST(StringBooleanConstructorInterp, SB09_NewStringLength) {
    EXPECT_EQ(interp_str("new String(\"ab\").length"), "2");
}
TEST(StringBooleanConstructorVM, SB09_NewStringLength) {
    EXPECT_EQ(vm_str("new String(\"ab\").length"), "2");
}

// ============================================================
// SB-10: new String("hello").indexOf("ll") === 2
// ============================================================

TEST(StringBooleanConstructorInterp, SB10_NewStringInheritedMethod) {
    EXPECT_EQ(interp_str("new String(\"hello\").indexOf(\"ll\")"), "2");
}
TEST(StringBooleanConstructorVM, SB10_NewStringInheritedMethod) {
    EXPECT_EQ(vm_str("new String(\"hello\").indexOf(\"ll\")"), "2");
}

// ============================================================
// SB-11: new String(Symbol("x")) → TypeError
// ============================================================

TEST(StringBooleanConstructorInterp, SB11_NewStringSymbolTypeError) {
    EXPECT_FALSE(interp_ok("new String(Symbol(\"x\"))"));
}
TEST(StringBooleanConstructorVM, SB11_NewStringSymbolTypeError) {
    EXPECT_FALSE(vm_ok("new String(Symbol(\"x\"))"));
}

// ============================================================
// SB-12: String.fromCharCode(65, 66, 67) === "ABC"
// ============================================================

TEST(StringBooleanConstructorInterp, SB12_FromCharCode) {
    EXPECT_EQ(interp_str("String.fromCharCode(65, 66, 67)"), "ABC");
}
TEST(StringBooleanConstructorVM, SB12_FromCharCode) {
    EXPECT_EQ(vm_str("String.fromCharCode(65, 66, 67)"), "ABC");
}

// ============================================================
// SB-13: String.fromCharCode() === ""
// ============================================================

TEST(StringBooleanConstructorInterp, SB13_FromCharCodeEmpty) {
    EXPECT_EQ(interp_str("String.fromCharCode()"), "");
}
TEST(StringBooleanConstructorVM, SB13_FromCharCodeEmpty) {
    EXPECT_EQ(vm_str("String.fromCharCode()"), "");
}

// ============================================================
// SB-14: String.fromCharCode(65 + 65536) === "A" (ToUint16 truncation)
// ============================================================

TEST(StringBooleanConstructorInterp, SB14_FromCharCodeTruncation) {
    EXPECT_EQ(interp_str("String.fromCharCode(65 + 65536) === \"A\""), "true");
}
TEST(StringBooleanConstructorVM, SB14_FromCharCodeTruncation) {
    EXPECT_EQ(vm_str("String.fromCharCode(65 + 65536) === \"A\""), "true");
}

// ============================================================
// SB-15: Boolean(0) === false
// ============================================================

TEST(StringBooleanConstructorInterp, SB15_BooleanFalsy) {
    EXPECT_EQ(interp_str("Boolean(0)"), "false");
}
TEST(StringBooleanConstructorVM, SB15_BooleanFalsy) {
    EXPECT_EQ(vm_str("Boolean(0)"), "false");
}

// ============================================================
// SB-16: Boolean(1) === true
// ============================================================

TEST(StringBooleanConstructorInterp, SB16_BooleanTruthy) {
    EXPECT_EQ(interp_str("Boolean(1)"), "true");
}
TEST(StringBooleanConstructorVM, SB16_BooleanTruthy) {
    EXPECT_EQ(vm_str("Boolean(1)"), "true");
}

// ============================================================
// SB-17: Boolean(new Boolean(false)) === true (object is truthy)
// ============================================================

TEST(StringBooleanConstructorInterp, SB17_BooleanObjectIsTruthy) {
    EXPECT_EQ(interp_str("Boolean(new Boolean(false))"), "true");
}
TEST(StringBooleanConstructorVM, SB17_BooleanObjectIsTruthy) {
    EXPECT_EQ(vm_str("Boolean(new Boolean(false))"), "true");
}

// ============================================================
// SB-18: typeof new Boolean(false) === "object"
// ============================================================

TEST(StringBooleanConstructorInterp, SB18_NewBooleanTypeof) {
    EXPECT_EQ(interp_str("typeof new Boolean(false)"), "object");
}
TEST(StringBooleanConstructorVM, SB18_NewBooleanTypeof) {
    EXPECT_EQ(vm_str("typeof new Boolean(false)"), "object");
}

// ============================================================
// SB-19: new Boolean(false) instanceof Boolean
// ============================================================

TEST(StringBooleanConstructorInterp, SB19_NewBooleanInstanceof) {
    EXPECT_EQ(interp_str("new Boolean(false) instanceof Boolean"), "true");
}
TEST(StringBooleanConstructorVM, SB19_NewBooleanInstanceof) {
    EXPECT_EQ(vm_str("new Boolean(false) instanceof Boolean"), "true");
}

// ============================================================
// SB-20: new Boolean(false).valueOf() === false
// ============================================================

TEST(StringBooleanConstructorInterp, SB20_NewBooleanValueOf) {
    EXPECT_EQ(interp_str("new Boolean(false).valueOf()"), "false");
}
TEST(StringBooleanConstructorVM, SB20_NewBooleanValueOf) {
    EXPECT_EQ(vm_str("new Boolean(false).valueOf()"), "false");
}

// ============================================================
// SB-21: new Boolean(true).toString() === "true"
// ============================================================

TEST(StringBooleanConstructorInterp, SB21_NewBooleanToString) {
    EXPECT_EQ(interp_str("new Boolean(true).toString()"), "true");
}
TEST(StringBooleanConstructorVM, SB21_NewBooleanToString) {
    EXPECT_EQ(vm_str("new Boolean(true).toString()"), "true");
}

// ============================================================
// SB-22: Boolean.prototype.valueOf.call("x") → TypeError
// ============================================================

TEST(StringBooleanConstructorInterp, SB22_BooleanValueOfCallStringTypeError) {
    EXPECT_FALSE(interp_ok("Boolean.prototype.valueOf.call(\"x\")"));
}
TEST(StringBooleanConstructorVM, SB22_BooleanValueOfCallStringTypeError) {
    EXPECT_FALSE(vm_ok("Boolean.prototype.valueOf.call(\"x\")"));
}

// ============================================================
// SB-23: new String("hello world").split(" ")[0] === "hello"
//        String 包装对象继承 split 方法，返回真正数组
// ============================================================

TEST(StringBooleanConstructorInterp, SB23_NewStringSplit) {
    EXPECT_EQ(interp_str("new String(\"hello world\").split(\" \")[0]"), "hello");
    EXPECT_EQ(interp_str("new String(\"hello world\").split(\" \")[1]"), "world");
}
TEST(StringBooleanConstructorVM, SB23_NewStringSplit) {
    EXPECT_EQ(vm_str("new String(\"hello world\").split(\" \")[0]"), "hello");
    EXPECT_EQ(vm_str("new String(\"hello world\").split(\" \")[1]"), "world");
}

// ============================================================
// SB-24: new String("x").valueOf() === "x"
//        String 包装对象 valueOf 从 wrapped_value_ 提取原始值
// ============================================================

TEST(StringBooleanConstructorInterp, SB24_NewStringValueOf) {
    EXPECT_EQ(interp_str("new String(\"x\").valueOf() === \"x\""), "true");
}
TEST(StringBooleanConstructorVM, SB24_NewStringValueOf) {
    EXPECT_EQ(vm_str("new String(\"x\").valueOf() === \"x\""), "true");
}

// ============================================================
// SB-25: new String("x").toString() === "x"
//        String 包装对象 toString 从 wrapped_value_ 提取
// ============================================================

TEST(StringBooleanConstructorInterp, SB25_NewStringToString) {
    EXPECT_EQ(interp_str("new String(\"x\").toString() === \"x\""), "true");
}
TEST(StringBooleanConstructorVM, SB25_NewStringToString) {
    EXPECT_EQ(vm_str("new String(\"x\").toString() === \"x\""), "true");
}

// ============================================================
// SB-26: String.fromCharCode(-1) === String.fromCharCode(65535)
//        ToUint16(-1) = 65535，-1 mod 65536 = 65535 (U+FFFF)
// ============================================================

TEST(StringBooleanConstructorInterp, SB26_FromCharCodeNegOne) {
    EXPECT_EQ(interp_str("String.fromCharCode(-1) === String.fromCharCode(65535)"), "true");
}
TEST(StringBooleanConstructorVM, SB26_FromCharCodeNegOne) {
    EXPECT_EQ(vm_str("String.fromCharCode(-1) === String.fromCharCode(65535)"), "true");
}

// ============================================================
// SB-27: String.fromCharCode(65.9) === "A"
//        ToUint16 先 ToNumber 再取整截断，65.9 → 65 → "A"
// ============================================================

TEST(StringBooleanConstructorInterp, SB27_FromCharCodeFloat) {
    EXPECT_EQ(interp_str("String.fromCharCode(65.9) === \"A\""), "true");
}
TEST(StringBooleanConstructorVM, SB27_FromCharCodeFloat) {
    EXPECT_EQ(vm_str("String.fromCharCode(65.9) === \"A\""), "true");
}

// ============================================================
// SB-28: new Boolean().valueOf() === false
//        无参数构造，ToBoolean(undefined) = false
// ============================================================

TEST(StringBooleanConstructorInterp, SB28_NewBooleanNoArg) {
    EXPECT_EQ(interp_str("new Boolean().valueOf()"), "false");
}
TEST(StringBooleanConstructorVM, SB28_NewBooleanNoArg) {
    EXPECT_EQ(vm_str("new Boolean().valueOf()"), "false");
}

// ============================================================
// SB-29: Boolean("") === false
//        空字符串为 falsy
// ============================================================

TEST(StringBooleanConstructorInterp, SB29_BooleanEmptyString) {
    EXPECT_EQ(interp_str("Boolean(\"\")"), "false");
}
TEST(StringBooleanConstructorVM, SB29_BooleanEmptyString) {
    EXPECT_EQ(vm_str("Boolean(\"\")"), "false");
}

// ============================================================
// SB-30: Boolean(null) === false
// ============================================================

TEST(StringBooleanConstructorInterp, SB30_BooleanNull) {
    EXPECT_EQ(interp_str("Boolean(null)"), "false");
}
TEST(StringBooleanConstructorVM, SB30_BooleanNull) {
    EXPECT_EQ(vm_str("Boolean(null)"), "false");
}

// ============================================================
// SB-31: Boolean(undefined) === false
// ============================================================

TEST(StringBooleanConstructorInterp, SB31_BooleanUndefined) {
    EXPECT_EQ(interp_str("Boolean(undefined)"), "false");
}
TEST(StringBooleanConstructorVM, SB31_BooleanUndefined) {
    EXPECT_EQ(vm_str("Boolean(undefined)"), "false");
}

// ============================================================
// SB-32: Boolean(NaN) === false
//        NaN 为 falsy，独立验证
// ============================================================

TEST(StringBooleanConstructorInterp, SB32_BooleanNaN) {
    EXPECT_EQ(interp_str("Boolean(NaN)"), "false");
}
TEST(StringBooleanConstructorVM, SB32_BooleanNaN) {
    EXPECT_EQ(vm_str("Boolean(NaN)"), "false");
}

// ============================================================
// SB-33: Boolean(-0) === false
//        -0 为 falsy（与 +0 同样处理）
// ============================================================

TEST(StringBooleanConstructorInterp, SB33_BooleanNegZero) {
    EXPECT_EQ(interp_str("Boolean(-0)"), "false");
}
TEST(StringBooleanConstructorVM, SB33_BooleanNegZero) {
    EXPECT_EQ(vm_str("Boolean(-0)"), "false");
}

// ============================================================
// SB-34: Boolean({}) === true
//        普通对象为 truthy
// ============================================================

TEST(StringBooleanConstructorInterp, SB34_BooleanObject) {
    EXPECT_EQ(interp_str("Boolean({})"), "true");
}
TEST(StringBooleanConstructorVM, SB34_BooleanObject) {
    EXPECT_EQ(vm_str("Boolean({})"), "true");
}

// ============================================================
// SB-35: Boolean([]) === true
//        空数组为 truthy
// ============================================================

TEST(StringBooleanConstructorInterp, SB35_BooleanEmptyArray) {
    EXPECT_EQ(interp_str("Boolean([])"), "true");
}
TEST(StringBooleanConstructorVM, SB35_BooleanEmptyArray) {
    EXPECT_EQ(vm_str("Boolean([])"), "true");
}

// ============================================================
// SB-36: String.fromCharCode(72) === "H"
//        单参数基础行为验证
// ============================================================

TEST(StringBooleanConstructorInterp, SB36_FromCharCodeSingle) {
    EXPECT_EQ(interp_str("String.fromCharCode(72) === \"H\""), "true");
}
TEST(StringBooleanConstructorVM, SB36_FromCharCodeSingle) {
    EXPECT_EQ(vm_str("String.fromCharCode(72) === \"H\""), "true");
}

// ============================================================
// SB-37: typeof String.prototype === "object"
//        String.prototype 本身是对象（kStringObject，wrapped=""）
// ============================================================

TEST(StringBooleanConstructorInterp, SB37_StringPrototypeIsObject) {
    EXPECT_EQ(interp_str("typeof String.prototype"), "object");
}
TEST(StringBooleanConstructorVM, SB37_StringPrototypeIsObject) {
    EXPECT_EQ(vm_str("typeof String.prototype"), "object");
}

// ============================================================
// SB-38: typeof Boolean.prototype === "object"
//        Boolean.prototype 本身是对象（kBooleanObject，wrapped=false）
// ============================================================

TEST(StringBooleanConstructorInterp, SB38_BooleanPrototypeIsObject) {
    EXPECT_EQ(interp_str("typeof Boolean.prototype"), "object");
}
TEST(StringBooleanConstructorVM, SB38_BooleanPrototypeIsObject) {
    EXPECT_EQ(vm_str("typeof Boolean.prototype"), "object");
}

// ============================================================
// SB-39: String(true) === "true"  /  String(false) === "false"
// ============================================================

TEST(StringBooleanConstructorInterp, SB39_StringBooleanPrimitive) {
    EXPECT_EQ(interp_str("String(true) === \"true\""), "true");
    EXPECT_EQ(interp_str("String(false) === \"false\""), "true");
}
TEST(StringBooleanConstructorVM, SB39_StringBooleanPrimitive) {
    EXPECT_EQ(vm_str("String(true) === \"true\""), "true");
    EXPECT_EQ(vm_str("String(false) === \"false\""), "true");
}

// ============================================================
// SB-40: String(NaN) === "NaN"
// ============================================================

TEST(StringBooleanConstructorInterp, SB40_StringNaN) {
    EXPECT_EQ(interp_str("String(NaN) === \"NaN\""), "true");
}
TEST(StringBooleanConstructorVM, SB40_StringNaN) {
    EXPECT_EQ(vm_str("String(NaN) === \"NaN\""), "true");
}

// ============================================================
// SB-41: String(Infinity) === "Infinity"
// ============================================================

TEST(StringBooleanConstructorInterp, SB41_StringInfinity) {
    EXPECT_EQ(interp_str("String(Infinity) === \"Infinity\""), "true");
}
TEST(StringBooleanConstructorVM, SB41_StringInfinity) {
    EXPECT_EQ(vm_str("String(Infinity) === \"Infinity\""), "true");
}

// ============================================================
// SB-42: String(-Infinity) === "-Infinity"
// ============================================================

TEST(StringBooleanConstructorInterp, SB42_StringNegInfinity) {
    EXPECT_EQ(interp_str("String(-Infinity) === \"-Infinity\""), "true");
}
TEST(StringBooleanConstructorVM, SB42_StringNegInfinity) {
    EXPECT_EQ(vm_str("String(-Infinity) === \"-Infinity\""), "true");
}

// ============================================================
// SB-43: new Boolean(false).toString.call({}) → TypeError
//        Boolean.prototype.toString 在非 Boolean 对象上抛 TypeError
// ============================================================

TEST(StringBooleanConstructorInterp, SB43_BooleanToStringCallNonBoolean) {
    EXPECT_FALSE(interp_ok("new Boolean(false).toString.call({})"));
}
TEST(StringBooleanConstructorVM, SB43_BooleanToStringCallNonBoolean) {
    EXPECT_FALSE(vm_ok("new Boolean(false).toString.call({})"));
}

// ============================================================
// SB-44: new String("ab").slice(1) === "b"
//        String 包装对象继承 slice，验证更多 prototype 方法
// ============================================================

TEST(StringBooleanConstructorInterp, SB44_NewStringSlice) {
    EXPECT_EQ(interp_str("new String(\"ab\").slice(1) === \"b\""), "true");
}
TEST(StringBooleanConstructorVM, SB44_NewStringSlice) {
    EXPECT_EQ(vm_str("new String(\"ab\").slice(1) === \"b\""), "true");
}

}  // namespace
