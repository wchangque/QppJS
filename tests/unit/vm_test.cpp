#include "qppjs/frontend/parser.h"
#include "qppjs/vm/compiler.h"
#include "qppjs/vm/vm.h"

#include <gtest/gtest.h>

#include <cmath>
#include <string_view>

namespace {

// Helper: parse → compile → exec via VM, expect success, return Value
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
// Basic literals
// ============================================================

TEST(VMLiterals, NumberLiteral) {
    auto v = vm_ok("42");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 42.0);
}

TEST(VMLiterals, StringLiteral) {
    auto v = vm_ok("\"hello\"");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "hello");
}

TEST(VMLiterals, BooleanTrue) {
    auto v = vm_ok("true");
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(VMLiterals, BooleanFalse) {
    auto v = vm_ok("false");
    EXPECT_TRUE(v.is_bool());
    EXPECT_FALSE(v.as_bool());
}

TEST(VMLiterals, NullLiteral) {
    auto v = vm_ok("null");
    EXPECT_TRUE(v.is_null());
}

// ============================================================
// Arithmetic
// ============================================================

TEST(VMArith, Add) {
    auto v = vm_ok("1 + 2");
    EXPECT_EQ(v.as_number(), 3.0);
}

TEST(VMArith, Sub) {
    auto v = vm_ok("10 - 3");
    EXPECT_EQ(v.as_number(), 7.0);
}

TEST(VMArith, Mul) {
    auto v = vm_ok("4 * 5");
    EXPECT_EQ(v.as_number(), 20.0);
}

TEST(VMArith, Div) {
    auto v = vm_ok("10 / 2");
    EXPECT_EQ(v.as_number(), 5.0);
}

TEST(VMArith, Mod) {
    auto v = vm_ok("7 % 3");
    EXPECT_EQ(v.as_number(), 1.0);
}

TEST(VMArith, Neg) {
    auto v = vm_ok("-5");
    EXPECT_EQ(v.as_number(), -5.0);
}

TEST(VMArith, Pos) {
    auto v = vm_ok("+\"42\"");
    EXPECT_EQ(v.as_number(), 42.0);
}

TEST(VMArith, Not) {
    auto v = vm_ok("!true");
    EXPECT_FALSE(v.as_bool());
}

TEST(VMArith, StringConcat) {
    auto v = vm_ok("\"hello\" + \" world\"");
    EXPECT_EQ(v.as_string(), "hello world");
}

// ============================================================
// Comparison
// ============================================================

TEST(VMCompare, Lt) {
    EXPECT_TRUE(vm_ok("1 < 2").as_bool());
    EXPECT_FALSE(vm_ok("2 < 1").as_bool());
}

TEST(VMCompare, Gt) {
    EXPECT_TRUE(vm_ok("2 > 1").as_bool());
    EXPECT_FALSE(vm_ok("1 > 2").as_bool());
}

TEST(VMCompare, StrictEq) {
    EXPECT_TRUE(vm_ok("1 === 1").as_bool());
    EXPECT_FALSE(vm_ok("1 === 2").as_bool());
}

TEST(VMCompare, StrictNEq) {
    EXPECT_TRUE(vm_ok("1 !== 2").as_bool());
    EXPECT_FALSE(vm_ok("1 !== 1").as_bool());
}

TEST(VMCompare, Eq) {
    EXPECT_TRUE(vm_ok("null == undefined").as_bool());
    EXPECT_FALSE(vm_ok("null == 0").as_bool());
}

// ============================================================
// Variables
// ============================================================

TEST(VMVar, LetDecl) {
    auto v = vm_ok("let x = 1; x");
    EXPECT_EQ(v.as_number(), 1.0);
}

TEST(VMVar, ConstDecl) {
    auto v = vm_ok("const x = 2; x");
    EXPECT_EQ(v.as_number(), 2.0);
}

TEST(VMVar, VarDecl) {
    auto v = vm_ok("var x = 3; x");
    EXPECT_EQ(v.as_number(), 3.0);
}

TEST(VMVar, Assign) {
    auto v = vm_ok("let x = 1; x = 2; x");
    EXPECT_EQ(v.as_number(), 2.0);
}

TEST(VMVar, AssignResult) {
    auto v = vm_ok("let x; x = 5");
    EXPECT_EQ(v.as_number(), 5.0);
}

TEST(VMVar, CompoundAdd) {
    auto v = vm_ok("let x = 3; x += 2; x");
    EXPECT_EQ(v.as_number(), 5.0);
}

TEST(VMVar, UndefinedIdentifier) {
    auto v = vm_ok("undefined");
    EXPECT_TRUE(v.is_undefined());
}

// ============================================================
// Block scope
// ============================================================

TEST(VMScope, BlockScope) {
    auto v = vm_ok("let x = 1; { let x = 2; } x");
    EXPECT_EQ(v.as_number(), 1.0);
}

TEST(VMScope, BlockScopeInner) {
    auto v = vm_ok("{ let x = 5; x }");
    EXPECT_EQ(v.as_number(), 5.0);
}

// ============================================================
// Control flow: if
// ============================================================

TEST(VMIf, IfTrue) {
    auto v = vm_ok("let x = 0; if (true) { x = 1; } x");
    EXPECT_EQ(v.as_number(), 1.0);
}

TEST(VMIf, IfFalse) {
    auto v = vm_ok("let x = 0; if (false) { x = 1; } x");
    EXPECT_EQ(v.as_number(), 0.0);
}

TEST(VMIf, IfElse) {
    auto v = vm_ok("let x = 0; if (false) { x = 1; } else { x = 2; } x");
    EXPECT_EQ(v.as_number(), 2.0);
}

// ============================================================
// Control flow: while
// ============================================================

TEST(VMWhile, Sum) {
    auto v = vm_ok("let sum = 0; let i = 1; while (i <= 10) { sum = sum + i; i = i + 1; } sum");
    EXPECT_EQ(v.as_number(), 55.0);
}

TEST(VMWhile, NotEntered) {
    auto v = vm_ok("let x = 0; while (false) { x = 1; } x");
    EXPECT_EQ(v.as_number(), 0.0);
}

// ============================================================
// Logical expressions
// ============================================================

TEST(VMLogical, AndTrue) {
    auto v = vm_ok("true && 42");
    EXPECT_EQ(v.as_number(), 42.0);
}

TEST(VMLogical, AndFalse) {
    auto v = vm_ok("false && 42");
    EXPECT_FALSE(v.as_bool());
}

TEST(VMLogical, OrTrue) {
    auto v = vm_ok("1 || 2");
    EXPECT_EQ(v.as_number(), 1.0);
}

TEST(VMLogical, OrFalse) {
    auto v = vm_ok("0 || 2");
    EXPECT_EQ(v.as_number(), 2.0);
}

// ============================================================
// typeof
// ============================================================

TEST(VMTypeof, Number) {
    auto v = vm_ok("typeof 42");
    EXPECT_EQ(v.as_string(), "number");
}

TEST(VMTypeof, String) {
    auto v = vm_ok("typeof \"hello\"");
    EXPECT_EQ(v.as_string(), "string");
}

TEST(VMTypeof, Undeclared) {
    // typeof undeclaredVar must NOT throw ReferenceError
    auto v = vm_ok("typeof undeclaredVar");
    EXPECT_EQ(v.as_string(), "undefined");
}

TEST(VMTypeof, Boolean) {
    auto v = vm_ok("typeof true");
    EXPECT_EQ(v.as_string(), "boolean");
}

// ============================================================
// Functions: declaration and call
// ============================================================

TEST(VMFunc, BasicDeclarationAndCall) {
    auto v = vm_ok("function add(a, b) { return a + b; } add(1, 2)");
    EXPECT_EQ(v.as_number(), 3.0);
}

TEST(VMFunc, ReturnUndefinedWhenNoReturn) {
    auto v = vm_ok("function f() {} f()");
    EXPECT_TRUE(v.is_undefined());
}

TEST(VMFunc, ReturnWithoutValue) {
    auto v = vm_ok("function f() { return; } f()");
    EXPECT_TRUE(v.is_undefined());
}

TEST(VMFunc, ExtraArgsIgnored) {
    auto v = vm_ok("function f(a) { return a; } f(1, 2, 3)");
    EXPECT_EQ(v.as_number(), 1.0);
}

TEST(VMFunc, MissingArgIsUndefined) {
    auto v = vm_ok("function f(a, b) { return b; } f(1)");
    EXPECT_TRUE(v.is_undefined());
}

// ============================================================
// Function expressions
// ============================================================

TEST(VMFunc, FunctionExpression) {
    auto v = vm_ok("let f = function(x) { return x * 2; }; f(5)");
    EXPECT_EQ(v.as_number(), 10.0);
}

TEST(VMFunc, NamedFunctionExpression) {
    auto v = vm_ok("let f = function double(x) { return x * 2; }; f(3)");
    EXPECT_EQ(v.as_number(), 6.0);
}

TEST(VMFunc, NamedFunctionExpressionShadowsOuterSameName) {
    auto v = vm_ok("let g = 1; let f = function g() { return g; }; f() === f");
    EXPECT_TRUE(v.as_bool());
}

TEST(VMFunc, ClosureSeesReassignedFunctionBinding) {
    auto v = vm_ok(
        "let g = function() { return 1; };"
        "let f = function() { return g(); };"
        "g = function() { return 2; };"
        "f()");
    EXPECT_EQ(v.as_number(), 2.0);
}

// ============================================================
// Closures
// ============================================================

TEST(VMFunc, ClosureCapturesOuter) {
    auto v = vm_ok(
        "function makeAdder(n) { return function(x) { return x + n; }; }"
        "let add5 = makeAdder(5);"
        "add5(3)");
    EXPECT_EQ(v.as_number(), 8.0);
}

TEST(VMFunc, ClosureOverLet) {
    auto v = vm_ok(
        "function makeCounter() {"
        "  let n = 0;"
        "  return function() { n = n + 1; return n; };"
        "}"
        "let c = makeCounter();"
        "c(); c(); c()");
    EXPECT_EQ(v.as_number(), 3.0);
}

TEST(VMFunc, ClosureSeesUpdated) {
    auto v = vm_ok(
        "let count = 0;"
        "function inc() { count = count + 1; }"
        "inc(); inc(); count");
    EXPECT_EQ(v.as_number(), 2.0);
}

// ============================================================
// Recursion
// ============================================================

TEST(VMFunc, RecursiveFactorial) {
    auto v = vm_ok(
        "function fact(n) { if (n <= 1) { return 1; } return n * fact(n - 1); }"
        "fact(5)");
    EXPECT_EQ(v.as_number(), 120.0);
}

TEST(VMFunc, RecursiveFibonacci) {
    auto v = vm_ok(
        "function fib(n) { if (n <= 1) { return n; } return fib(n-1) + fib(n-2); }"
        "fib(10)");
    EXPECT_EQ(v.as_number(), 55.0);
}

// ============================================================
// Var hoisting
// ============================================================

TEST(VMFunc, VarHoistedToFunctionScope) {
    auto v = vm_ok(
        "function f() {"
        "  var x = 1;"
        "  if (true) { var x = 2; }"
        "  return x;"
        "}"
        "f()");
    EXPECT_EQ(v.as_number(), 2.0);
}

// VM hoists function declarations at function entry, so f() can be called before its declaration
TEST(VMFunc, FunctionDeclarationHoisted) {
    // The last statement is a FunctionDeclaration (returns undefined as completion value).
    // But the call to f() succeeds (returns 1) because hoisting works.
    // To verify hoisting works, check the result of f():
    auto v = vm_ok("let r = f(); function f() { return 1; } r");
    EXPECT_EQ(v.as_number(), 1.0);
}

// ============================================================
// Error cases
// ============================================================

TEST(VMFunc, CallNonFunctionError) {
    auto parse_result = qppjs::parse_program("let x = 42; x()");
    ASSERT_TRUE(parse_result.ok());
    qppjs::Compiler compiler;
    auto bytecode = compiler.compile(parse_result.value());
    qppjs::VM vm;
    auto result = vm.exec(bytecode);
    EXPECT_FALSE(result.is_ok());
    EXPECT_NE(result.error().message().find("TypeError"), std::string::npos);
}

TEST(VMFunc, MaxCallDepthExceeded) {
    auto parse_result = qppjs::parse_program("function inf() { return inf(); } inf()");
    ASSERT_TRUE(parse_result.ok());
    qppjs::Compiler compiler;
    auto bytecode = compiler.compile(parse_result.value());
    qppjs::VM vm;
    auto result = vm.exec(bytecode);
    EXPECT_FALSE(result.is_ok());
    EXPECT_NE(result.error().message().find("RangeError"), std::string::npos);
}

// ============================================================
// Objects
// ============================================================

TEST(VMObject, EmptyObjectIsObject) {
    auto v = vm_ok("let obj = {}; typeof obj");
    EXPECT_EQ(v.as_string(), "object");
}

TEST(VMObject, DotAccessProperty) {
    auto v = vm_ok("let obj = { x: 1 }; obj.x");
    EXPECT_EQ(v.as_number(), 1.0);
}

TEST(VMObject, DotAccessMissing) {
    auto v = vm_ok("let obj = {}; obj.x");
    EXPECT_TRUE(v.is_undefined());
}

TEST(VMObject, BracketAccess) {
    auto v = vm_ok("let obj = { x: 1 }; obj[\"x\"]");
    EXPECT_EQ(v.as_number(), 1.0);
}

TEST(VMObject, SetProperty) {
    auto v = vm_ok("let obj = {}; obj.x = 42; obj.x");
    EXPECT_EQ(v.as_number(), 42.0);
}

TEST(VMObject, NestedAccess) {
    auto v = vm_ok("let obj = { nested: { a: 1 } }; obj.nested.a");
    EXPECT_EQ(v.as_number(), 1.0);
}

TEST(VMObject, BracketSetDotGet) {
    auto v = vm_ok("let obj = {}; obj[\"k\"] = 99; obj.k");
    EXPECT_EQ(v.as_number(), 99.0);
}

TEST(VMObject, MultipleProperties) {
    auto v = vm_ok("let obj = { a: 1, b: 2 }; obj.a + obj.b");
    EXPECT_EQ(v.as_number(), 3.0);
}

// ============================================================
// Method calls and this
// ============================================================

TEST(VMThis, MethodCallBindsThis) {
    auto v = vm_ok("let obj = { val: 42, get: function() { return this.val; } }; obj.get()");
    EXPECT_EQ(v.as_number(), 42.0);
}

TEST(VMThis, ThisInConstructor) {
    auto v = vm_ok(
        "function Point(x, y) { this.x = x; this.y = y; }"
        "let p = new Point(3, 4);"
        "p.x + p.y");
    EXPECT_EQ(v.as_number(), 7.0);
}

// ============================================================
// Prototype chain
// ============================================================

TEST(VMProto, PrototypeMethodInheritance) {
    auto v = vm_ok(
        "function Animal(name) { this.name = name; }"
        "Animal.prototype.speak = function() { return this.name; };"
        "let a = new Animal(\"Cat\");"
        "a.speak()");
    EXPECT_EQ(v.as_string(), "Cat");
}

TEST(VMProto, PrototypeChainLookup) {
    auto v = vm_ok(
        "function Foo() {}"
        "Foo.prototype.bar = 42;"
        "let f = new Foo();"
        "f.bar");
    EXPECT_EQ(v.as_number(), 42.0);
}

TEST(VMProto, OwnPropertyShadows) {
    auto v = vm_ok(
        "function Foo() { this.x = 1; }"
        "Foo.prototype.x = 99;"
        "let f = new Foo();"
        "f.x");
    EXPECT_EQ(v.as_number(), 1.0);
}

// ============================================================
// new expression
// ============================================================

TEST(VMNew, NewReturnsInstance) {
    auto v = vm_ok(
        "function Ctor() { this.val = 10; }"
        "let obj = new Ctor();"
        "obj.val");
    EXPECT_EQ(v.as_number(), 10.0);
}

TEST(VMNew, ConstructorExplicitReturnObjectOverrides) {
    auto v = vm_ok(
        "function Ctor() { return { x: 99 }; }"
        "let obj = new Ctor();"
        "obj.x");
    EXPECT_EQ(v.as_number(), 99.0);
}

TEST(VMNew, ConstructorReturnPrimitiveIgnored) {
    auto v = vm_ok(
        "function Ctor() { this.x = 5; return 42; }"
        "let obj = new Ctor();"
        "obj.x");
    EXPECT_EQ(v.as_number(), 5.0);
}

// ============================================================
// Additional coverage: interpreter_test.cpp semantics
// ============================================================

TEST(VMVar, UndeclaredRefError) {
    auto parse_result = qppjs::parse_program("x");
    ASSERT_TRUE(parse_result.ok());
    qppjs::Compiler compiler;
    auto bytecode = compiler.compile(parse_result.value());
    qppjs::VM vm;
    auto result = vm.exec(bytecode);
    EXPECT_FALSE(result.is_ok());
    EXPECT_NE(result.error().message().find("ReferenceError"), std::string::npos);
}

TEST(VMVar, VarHoisting) {
    auto v = vm_ok("x; var x = 1; x");
    EXPECT_EQ(v.as_number(), 1.0);
}

TEST(VMVar, VarHoistingFirstUndefined) {
    auto v = vm_ok("var y; if (y === undefined) { y = 99; } y");
    EXPECT_EQ(v.as_number(), 99.0);
}

TEST(VMVar, VarPenetratesBlock) {
    auto v = vm_ok("{ var x = 1; } x");
    EXPECT_EQ(v.as_number(), 1.0);
}

TEST(VMVar, VarDuplicateDeclNoError) {
    auto v = vm_ok("var x = 1; var x = 2; x");
    EXPECT_EQ(v.as_number(), 2.0);
}

TEST(VMScope, InnerLetNoLeak) {
    auto msg_result = []{
        auto parse_result = qppjs::parse_program("{ let x = 1; } x");
        EXPECT_TRUE(parse_result.ok());
        qppjs::Compiler compiler;
        auto bytecode = compiler.compile(parse_result.value());
        qppjs::VM vm;
        return vm.exec(bytecode);
    }();
    EXPECT_FALSE(msg_result.is_ok());
    EXPECT_NE(msg_result.error().message().find("ReferenceError"), std::string::npos);
}

TEST(VMScope, VarSameBinding) {
    auto v = vm_ok("var x = 1; { var x = 2; } x");
    EXPECT_EQ(v.as_number(), 2.0);
}

TEST(VMScope, InnerModifiesOuter) {
    auto v = vm_ok("let x = 1; { x = 2; } x");
    EXPECT_EQ(v.as_number(), 2.0);
}

TEST(VMToBoolean, ZeroFalsy) {
    auto v = vm_ok("let r = 0; if (0) { r = 1; } else { r = 2; } r");
    EXPECT_EQ(v.as_number(), 2.0);
}

TEST(VMToBoolean, EmptyStringFalsy) {
    auto v = vm_ok("let r = 0; if (\"\") { r = 1; } else { r = 2; } r");
    EXPECT_EQ(v.as_number(), 2.0);
}

TEST(VMToBoolean, NullFalsy) {
    auto v = vm_ok("let r = 0; if (null) { r = 1; } else { r = 2; } r");
    EXPECT_EQ(v.as_number(), 2.0);
}

TEST(VMToBoolean, UndefinedFalsy) {
    auto v = vm_ok("let r = 0; if (undefined) { r = 1; } else { r = 2; } r");
    EXPECT_EQ(v.as_number(), 2.0);
}

TEST(VMToBoolean, NanFalsy) {
    auto v = vm_ok("let r = 0; if (0 / 0) { r = 1; } else { r = 2; } r");
    EXPECT_EQ(v.as_number(), 2.0);
}

TEST(VMTypeof, Null) {
    EXPECT_EQ(vm_ok("typeof null").as_string(), "object");
}

TEST(VMTypeof, Undefined2) {
    EXPECT_EQ(vm_ok("typeof undefined").as_string(), "undefined");
}

TEST(VMTypeof, Function) {
    EXPECT_EQ(vm_ok("function f() {} typeof f").as_string(), "function");
}

TEST(VMArith, AddNumberString) {
    EXPECT_EQ(vm_ok("1 + \"2\"").as_string(), "12");
}

TEST(VMArith, SubStringNumber) {
    EXPECT_EQ(vm_ok("\"3\" - 1").as_number(), 2.0);
}

TEST(VMArith, DivByZero) {
    auto v = vm_ok("1 / 0");
    EXPECT_TRUE(v.is_number());
    EXPECT_TRUE(std::isinf(v.as_number()));
}

TEST(VMArith, ZeroDivZero) {
    auto v = vm_ok("0 / 0");
    EXPECT_TRUE(v.is_number());
    EXPECT_TRUE(std::isnan(v.as_number()));
}

TEST(VMArith, NullPlusNumber) {
    EXPECT_EQ(vm_ok("null + 1").as_number(), 1.0);
}

TEST(VMArith, TruePlusTrue) {
    EXPECT_EQ(vm_ok("true + true").as_number(), 2.0);
}

TEST(VMArith, NumberPlusNumberPlusString) {
    EXPECT_EQ(vm_ok("1 + 2 + \"3\"").as_string(), "33");
}

TEST(VMCompare, LtEq) {
    EXPECT_TRUE(vm_ok("1 <= 1").as_bool());
}

TEST(VMCompare, GtEq) {
    EXPECT_TRUE(vm_ok("2 >= 2").as_bool());
}

TEST(VMCompare, StringLexicographicOrder) {
    EXPECT_TRUE(vm_ok("\"10\" < \"9\"").as_bool());
}

TEST(VMCompare, NullLessThanOne) {
    EXPECT_TRUE(vm_ok("null < 1").as_bool());
}

TEST(VMCompare, NanNotLtNumber) {
    EXPECT_FALSE(vm_ok("0 / 0 < 1").as_bool());
}

TEST(VMEq, StrictEqTypeMismatch) {
    EXPECT_FALSE(vm_ok("1 === \"1\"").as_bool());
}

TEST(VMEq, NanNotEqNan) {
    EXPECT_FALSE(vm_ok("0 / 0 === 0 / 0").as_bool());
}

TEST(VMEq, ZeroEqFalse) {
    EXPECT_TRUE(vm_ok("0 == false").as_bool());
}

TEST(VMEq, StringOneEqNumberOne) {
    EXPECT_TRUE(vm_ok("\"1\" == 1").as_bool());
}

TEST(VMAssign, SubAssign) {
    EXPECT_EQ(vm_ok("let x = 10; x -= 3; x").as_number(), 7.0);
}

TEST(VMAssign, MulAssign) {
    EXPECT_EQ(vm_ok("let x = 3; x *= 2; x").as_number(), 6.0);
}

TEST(VMAssign, DivAssign) {
    EXPECT_EQ(vm_ok("let x = 10; x /= 4; x").as_number(), 2.5);
}

TEST(VMAssign, ModAssign) {
    EXPECT_EQ(vm_ok("let x = 10; x %= 3; x").as_number(), 1.0);
}

TEST(VMControl, ReturnInsideWhile) {
    auto v = vm_ok("let i = 0; while (i < 10) { i = i + 1; if (i === 3) { return i; } } i");
    EXPECT_EQ(v.as_number(), 3.0);
}

// ============================================================
// Additional function tests
// ============================================================

TEST(VMFunc, TypeofFunctionIsFunction) {
    EXPECT_EQ(vm_ok("function f() {} typeof f").as_string(), "function");
}

TEST(VMFunc, LocalVarDoesNotLeakToOuter) {
    EXPECT_EQ(vm_ok("function f() { let x = 99; } f(); typeof x").as_string(), "undefined");
}

TEST(VMFunc, FunctionCanReadOuterVar) {
    EXPECT_EQ(vm_ok("let x = 10; function f() { return x; } f()").as_number(), 10.0);
}

TEST(VMFunc, ParamShadowsOuterVar) {
    EXPECT_EQ(vm_ok("let x = 10; function f(x) { return x; } f(99)").as_number(), 99.0);
}

TEST(VMFunc, FunctionPassedAsArgument) {
    auto v = vm_ok(
        "function apply(fn, x) { return fn(x); }"
        "function double(x) { return x * 2; }"
        "apply(double, 7)");
    EXPECT_EQ(v.as_number(), 14.0);
}

TEST(VMFunc, MultipleCallsAreIndependent) {
    EXPECT_EQ(vm_ok("function f(x) { return x * 2; } f(1); f(2); f(3)").as_number(), 6.0);
}

TEST(VMFunc, NestedCallsAsArguments) {
    auto v = vm_ok(
        "function add(a, b) { return a + b; }"
        "function mul(a, b) { return a * b; }"
        "add(mul(2, 3), mul(4, 5))");
    EXPECT_EQ(v.as_number(), 26.0);
}

TEST(VMFunc, MutualRecursion) {
    auto v = vm_ok(
        "function isEven(n) { if (n === 0) { return true; } return isOdd(n - 1); }"
        "function isOdd(n) { if (n === 0) { return false; } return isEven(n - 1); }"
        "isEven(4)");
    EXPECT_TRUE(v.as_bool());
}

TEST(VMFunc, TwoClosuresShareSameEnv) {
    auto v = vm_ok(
        "function makeCounter() {"
        "  let n = 0;"
        "  function inc() { n = n + 1; }"
        "  function get() { return n; }"
        "  return function() { inc(); return get(); };"
        "}"
        "let step = makeCounter();"
        "step(); step(); step()");
    EXPECT_EQ(v.as_number(), 3.0);
}

TEST(VMFunc, CallNullThrowsTypeError) {
    auto parse_result = qppjs::parse_program("let x = null; x()");
    ASSERT_TRUE(parse_result.ok());
    qppjs::Compiler compiler;
    auto bytecode = compiler.compile(parse_result.value());
    qppjs::VM vm;
    auto result = vm.exec(bytecode);
    EXPECT_FALSE(result.is_ok());
    EXPECT_NE(result.error().message().find("TypeError"), std::string::npos);
}

// ============================================================
// Additional object tests
// ============================================================

TEST(VMObject, DuplicateKeyLastWins) {
    EXPECT_EQ(vm_ok("let obj = { a: 1, a: 2 }; obj.a").as_number(), 2.0);
}

TEST(VMObject, ComputedKeyFromVariable) {
    EXPECT_EQ(vm_ok("let key = \"x\"; let obj = { x: 1 }; obj[key]").as_number(), 1.0);
}

TEST(VMObject, NullDotAccessThrowsTypeError) {
    auto parse_result = qppjs::parse_program("null.x");
    ASSERT_TRUE(parse_result.ok());
    qppjs::Compiler compiler;
    auto bytecode = compiler.compile(parse_result.value());
    qppjs::VM vm;
    auto result = vm.exec(bytecode);
    EXPECT_FALSE(result.is_ok());
    EXPECT_NE(result.error().message().find("TypeError"), std::string::npos);
}

TEST(VMObject, UndefinedDotAccessThrowsTypeError) {
    auto parse_result = qppjs::parse_program("undefined.x");
    ASSERT_TRUE(parse_result.ok());
    qppjs::Compiler compiler;
    auto bytecode = compiler.compile(parse_result.value());
    qppjs::VM vm;
    auto result = vm.exec(bytecode);
    EXPECT_FALSE(result.is_ok());
    EXPECT_NE(result.error().message().find("TypeError"), std::string::npos);
}

TEST(VMObject, SameObjectReferenceStrictEqual) {
    EXPECT_TRUE(vm_ok("let a = {}; a === a").as_bool());
}

TEST(VMObject, TwoDifferentObjectLiteralsNotStrictEqual) {
    EXPECT_FALSE(vm_ok("let a = {}; let b = {}; a === b").as_bool());
}

TEST(VMObject, SharedReferenceWriteThroughAlias) {
    EXPECT_EQ(vm_ok("let a = {}; let b = a; b.x = 1; a.x").as_number(), 1.0);
}

TEST(VMObject, DeepNestedAccess) {
    EXPECT_EQ(vm_ok("let obj = { a: { b: { c: 42 } } }; obj.a.b.c").as_number(), 42.0);
}

TEST(VMObject, NestedObjectWriteThenRead) {
    EXPECT_EQ(vm_ok("let obj = { a: {} }; obj.a.b = 3; obj.a.b").as_number(), 3.0);
}

TEST(VMObject, NumberKeyEqualsStringKey) {
    EXPECT_EQ(vm_ok("let obj = {}; obj[\"0\"] = 99; obj[0]").as_number(), 99.0);
}

TEST(VMObject, PropertyValueNull) {
    EXPECT_TRUE(vm_ok("let obj = {}; obj.k = null; obj.k").is_null());
}

TEST(VMObject, TypeofNullIsObject) {
    EXPECT_EQ(vm_ok("typeof null").as_string(), "object");
}

// ============================================================
// Additional proto tests
// ============================================================

TEST(VMProto, PrototypeChainThreeLevels) {
    auto v = vm_ok(
        "function A() {}"
        "A.prototype.x = 1;"
        "function B() {}"
        "B.prototype = new A();"
        "let b = new B();"
        "b.x");
    EXPECT_EQ(v.as_number(), 1.0);
}

TEST(VMProto, InstanceModifyDoesNotAffectProto) {
    auto v = vm_ok(
        "function Foo() { this.x = 1; }"
        "Foo.prototype.x = 99;"
        "let a = new Foo();"
        "let b = new Foo();"
        "a.x = 42;"
        "b.x");
    EXPECT_EQ(v.as_number(), 1.0);
}

}  // namespace
