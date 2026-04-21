#include "qppjs/frontend/ast_dump.h"
#include "qppjs/frontend/parser.h"
#include "qppjs/runtime/interpreter.h"

#include <gtest/gtest.h>
#include <string>

namespace qppjs {
namespace {

EvalResult run(const std::string& src) {
    auto parse_result = parse_program(src);
    if (!parse_result.ok()) {
        return EvalResult::err(parse_result.error());
    }
    Interpreter interp;
    return interp.exec(parse_result.value());
}

// ---- 基础函数声明与调用 ----

TEST(FunctionTest, BasicFunctionDeclarationAndCall) {
    auto r = run("function add(a, b) { return a + b; } add(1, 2)");
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().as_number(), 3.0);
}

TEST(FunctionTest, FunctionReturnUndefinedWhenNoReturn) {
    auto r = run("function f() {} f()");
    ASSERT_TRUE(r.is_ok());
    EXPECT_TRUE(r.value().is_undefined());
}

TEST(FunctionTest, FunctionReturnWithoutValue) {
    auto r = run("function f() { return; } f()");
    ASSERT_TRUE(r.is_ok());
    EXPECT_TRUE(r.value().is_undefined());
}

TEST(FunctionTest, FunctionWithNoArgs) {
    auto r = run("function f() { return 42; } f()");
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().as_number(), 42.0);
}

TEST(FunctionTest, FunctionExtraArgsIgnored) {
    auto r = run("function f(a) { return a; } f(1, 2, 3)");
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().as_number(), 1.0);
}

TEST(FunctionTest, FunctionMissingArgIsUndefined) {
    auto r = run("function f(a, b) { return b; } f(1)");
    ASSERT_TRUE(r.is_ok());
    EXPECT_TRUE(r.value().is_undefined());
}

// ---- 函数表达式 ----

TEST(FunctionTest, FunctionExpressionAssignedToVar) {
    auto r = run("let f = function(x) { return x * 2; }; f(5)");
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().as_number(), 10.0);
}

TEST(FunctionTest, NamedFunctionExpression) {
    auto r = run("let f = function double(x) { return x * 2; }; f(3)");
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().as_number(), 6.0);
}

// ---- typeof ----

TEST(FunctionTest, TypeofFunctionIsFunction) {
    auto r = run("function f() {} typeof f");
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().as_string(), "function");
}

TEST(FunctionTest, TypeofFunctionExpressionIsFunction) {
    auto r = run("let f = function() {}; typeof f");
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().as_string(), "function");
}

// ---- 作用域隔离 ----

TEST(FunctionTest, LocalVarDoesNotLeakToOuter) {
    auto r = run("function f() { let x = 99; } f(); typeof x");
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().as_string(), "undefined");
}

TEST(FunctionTest, FunctionCanReadOuterVar) {
    auto r = run("let x = 10; function f() { return x; } f()");
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().as_number(), 10.0);
}

TEST(FunctionTest, ParamShadowsOuterVar) {
    auto r = run("let x = 10; function f(x) { return x; } f(99)");
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().as_number(), 99.0);
}

// ---- 闭包 ----

TEST(FunctionTest, ClosureCapturesOuterVariable) {
    auto r = run(
        "function makeAdder(n) { return function(x) { return x + n; }; }"
        "let add5 = makeAdder(5);"
        "add5(3)");
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().as_number(), 8.0);
}

TEST(FunctionTest, ClosureSeesUpdatedVar) {
    auto r = run(
        "let count = 0;"
        "function inc() { count = count + 1; }"
        "inc(); inc(); count");
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().as_number(), 2.0);
}

TEST(FunctionTest, ClosureOverLetBinding) {
    auto r = run(
        "function makeCounter() {"
        "  let n = 0;"
        "  return function() { n = n + 1; return n; };"
        "}"
        "let c = makeCounter();"
        "c(); c(); c()");
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().as_number(), 3.0);
}

// ---- 递归 ----

TEST(FunctionTest, RecursiveFactorial) {
    auto r = run(
        "function fact(n) { if (n <= 1) { return 1; } return n * fact(n - 1); }"
        "fact(5)");
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().as_number(), 120.0);
}

TEST(FunctionTest, RecursiveFibonacci) {
    auto r = run(
        "function fib(n) { if (n <= 1) { return n; } return fib(n-1) + fib(n-2); }"
        "fib(10)");
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().as_number(), 55.0);
}

// ---- var 提升 ----

TEST(FunctionTest, VarHoistedToFunctionScope) {
    auto r = run(
        "function f() {"
        "  var x = 1;"
        "  if (true) { var x = 2; }"
        "  return x;"
        "}"
        "f()");
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().as_number(), 2.0);
}

TEST(FunctionTest, VarInFunctionDoesNotLeakToGlobal) {
    auto r = run("function f() { var local = 42; } f(); typeof local");
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().as_string(), "undefined");
}

// ---- 函数作为值传递 ----

TEST(FunctionTest, FunctionPassedAsArgument) {
    auto r = run(
        "function apply(fn, x) { return fn(x); }"
        "function double(x) { return x * 2; }"
        "apply(double, 7)");
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().as_number(), 14.0);
}

// ---- 错误处理 ----

TEST(FunctionTest, CallNonFunctionThrowsTypeError) {
    auto r = run("let x = 42; x()");
    ASSERT_FALSE(r.is_ok());
    EXPECT_NE(r.error().message().find("TypeError"), std::string::npos);
}

TEST(FunctionTest, MaxCallDepthExceeded) {
    auto r = run("function inf() { return inf(); } inf()");
    ASSERT_FALSE(r.is_ok());
    EXPECT_NE(r.error().message().find("RangeError"), std::string::npos);
}

// ---- Parser 测试 ----

TEST(FunctionParserTest, ParseFunctionDeclaration) {
    auto r = parse_program("function f(a, b) { return a + b; }");
    ASSERT_TRUE(r.ok());
    ASSERT_EQ(r.value().body.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<FunctionDeclaration>(r.value().body[0].v));
    const auto& fd = std::get<FunctionDeclaration>(r.value().body[0].v);
    EXPECT_EQ(fd.name, "f");
    ASSERT_EQ(fd.params.size(), 2u);
    EXPECT_EQ(fd.params[0], "a");
    EXPECT_EQ(fd.params[1], "b");
}

TEST(FunctionParserTest, ParseFunctionExpression) {
    auto r = parse_program("let f = function(x) { return x; };");
    ASSERT_TRUE(r.ok());
    ASSERT_EQ(r.value().body.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<VariableDeclaration>(r.value().body[0].v));
    const auto& vd = std::get<VariableDeclaration>(r.value().body[0].v);
    ASSERT_TRUE(vd.init.has_value());
    ASSERT_TRUE(std::holds_alternative<FunctionExpression>(vd.init->v));
}

TEST(FunctionParserTest, ParseCallExpression) {
    auto r = parse_program("f(1, 2)");
    ASSERT_TRUE(r.ok());
    ASSERT_EQ(r.value().body.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<ExpressionStatement>(r.value().body[0].v));
    const auto& es = std::get<ExpressionStatement>(r.value().body[0].v);
    ASSERT_TRUE(std::holds_alternative<CallExpression>(es.expr.v));
    const auto& ce = std::get<CallExpression>(es.expr.v);
    EXPECT_EQ(ce.arguments.size(), 2u);
}

TEST(FunctionParserTest, ParseNoArgCall) {
    auto r = parse_program("f()");
    ASSERT_TRUE(r.ok());
    const auto& es = std::get<ExpressionStatement>(r.value().body[0].v);
    const auto& ce = std::get<CallExpression>(es.expr.v);
    EXPECT_EQ(ce.arguments.size(), 0u);
}

TEST(FunctionParserTest, ParseFunctionNoParams) {
    auto r = parse_program("function f() { return 1; }");
    ASSERT_TRUE(r.ok());
    const auto& fd = std::get<FunctionDeclaration>(r.value().body[0].v);
    EXPECT_EQ(fd.name, "f");
    EXPECT_EQ(fd.params.size(), 0u);
}

// ---- AST dump 测试 ----

TEST(FunctionDumpTest, DumpFunctionDeclaration) {
    auto r = parse_program("function f(a) { return a; }");
    ASSERT_TRUE(r.ok());
    auto dump = dump_program(r.value());
    EXPECT_NE(dump.find("FunctionDeclaration f"), std::string::npos);
    EXPECT_NE(dump.find("params: (a)"), std::string::npos);
}

TEST(FunctionDumpTest, DumpCallExpression) {
    auto r = parse_program("f(1)");
    ASSERT_TRUE(r.ok());
    auto dump = dump_program(r.value());
    EXPECT_NE(dump.find("CallExpression"), std::string::npos);
    EXPECT_NE(dump.find("callee:"), std::string::npos);
}

// ---- 函数声明提升（hoisting）----

// 当前实现：hoist_vars 仅将函数名预绑定为 undefined，
// 函数对象赋值发生在顺序执行到 eval_function_decl 时。
// 因此在声明前调用会遇到 undefined，抛 TypeError。
// 这记录了当前阶段的实际行为，待 Phase N 完整实现提升后再修改预期。
TEST(FunctionTest, FunctionDeclarationHoistingNotYetSupported) {
    auto r = run("f(); function f() { return 1; }");
    // 当前不支持真正的函数声明提升：f 在执行到声明前值为 undefined
    ASSERT_FALSE(r.is_ok());
    EXPECT_NE(r.error().message().find("TypeError"), std::string::npos);
}

// 函数声明在声明后调用可以正常工作（非提升场景的基准）
TEST(FunctionTest, FunctionDeclarationCalledAfterDeclarationWorks) {
    auto r = run("function f() { return 1; } f()");
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().as_number(), 1.0);
}

// ---- 多次调用同一函数 ----

TEST(FunctionTest, MultipleCallsAreIndependent) {
    auto r = run(
        "function f(x) { return x * 2; }"
        "f(1); f(2); f(3)");
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().as_number(), 6.0);
}

// 每次调用的局部状态互不干扰
TEST(FunctionTest, MultipleCallsLocalStateIsolated) {
    auto r = run(
        "function f(x) { var t = x + 1; return t; }"
        "let a = f(10);"
        "let b = f(20);"
        "a + b");
    ASSERT_TRUE(r.is_ok());
    // f(10) = 11, f(20) = 21, sum = 32
    EXPECT_EQ(r.value().as_number(), 32.0);
}

// ---- 嵌套函数调用 ----

TEST(FunctionTest, NestedCallsAsArguments) {
    auto r = run(
        "function add(a, b) { return a + b; }"
        "function mul(a, b) { return a * b; }"
        "add(mul(2, 3), mul(4, 5))");
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().as_number(), 26.0);
}

TEST(FunctionTest, ThreeLevelNestedCalls) {
    auto r = run(
        "function inc(x) { return x + 1; }"
        "inc(inc(inc(0)))");
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().as_number(), 3.0);
}

// ---- 高阶函数：函数返回函数 ----

TEST(FunctionTest, FunctionReturnsFunction) {
    auto r = run(
        "function makeAdder(n) { return function(x) { return x + n; }; }"
        "let add10 = makeAdder(10);"
        "add10(5)");
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().as_number(), 15.0);
}

TEST(FunctionTest, ComposeHigherOrder) {
    auto r = run(
        "function compose(f, g) { return function(x) { return f(g(x)); }; }"
        "function double(x) { return x * 2; }"
        "function inc(x) { return x + 1; }"
        "let doubleAfterInc = compose(double, inc);"
        "doubleAfterInc(3)");
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().as_number(), 8.0);
}

TEST(FunctionTest, ReturnedFunctionIsCallable) {
    auto r = run(
        "function outer() { return function() { return 99; }; }"
        "let f = outer();"
        "f()");
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().as_number(), 99.0);
}

// ---- 参数遮蔽全局变量（补充 const 场景）----

TEST(FunctionTest, ParamShadowsConstGlobal) {
    auto r = run(
        "const x = 100;"
        "function f(x) { return x; }"
        "f(42)");
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().as_number(), 42.0);
}

// const 全局不受函数内操作影响
TEST(FunctionTest, ConstGlobalUnchangedAfterCallWithSameName) {
    auto r = run(
        "const x = 100;"
        "function f(x) { return x * 2; }"
        "f(5);"
        "x");
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().as_number(), 100.0);
}

// ---- var 提升边界 ----

// 函数内 var 使用在声明之前（var 提升到函数作用域，赋值留在原处）
TEST(FunctionTest, VarUsedBeforeDeclarationInFunction) {
    auto r = run(
        "function f() { x = 1; var x; return x; }"
        "f()");
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().as_number(), 1.0);
}

// var 赋值在声明前，声明带初始化在后：赋值结果应被初始化覆盖
TEST(FunctionTest, VarAssignBeforeDeclWithInit) {
    auto r = run(
        "function f() { x = 5; var x = 10; return x; }"
        "f()");
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().as_number(), 10.0);
}

// 函数内 var 与 let 共存，互不干扰
TEST(FunctionTest, VarAndLetCoexistInFunction) {
    auto r = run(
        "function f() { var x = 1; let y = 2; return x + y; }"
        "f()");
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().as_number(), 3.0);
}

// var 在 if 块内声明，应提升到函数作用域（通过 eval_block_stmt -> hoist_vars 路径）
TEST(FunctionTest, VarInIfBlockHoistedToFunction) {
    auto r = run(
        "function f() {"
        "  if (true) { var x = 7; }"
        "  return x;"
        "}"
        "f()");
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().as_number(), 7.0);
}

// ---- 闭包共享同一个 Environment ----

TEST(FunctionTest, TwoClosuresShareSameEnv) {
    auto r = run(
        "function makeCounter() {"
        "  let n = 0;"
        "  function inc() { n = n + 1; }"
        "  function get() { return n; }"
        "  return function() { inc(); return get(); };"
        "}"
        "let step = makeCounter();"
        "step(); step(); step()");
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().as_number(), 3.0);
}

TEST(FunctionTest, TwoClosuresFromSameFactoryShareBinding) {
    auto r = run(
        "function makePair() {"
        "  let v = 0;"
        "  let setter = function(x) { v = x; };"
        "  let getter = function() { return v; };"
        "  setter(42);"
        "  return getter();"
        "}"
        "makePair()");
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().as_number(), 42.0);
}

// ---- 函数作为对象属性（方法调用，无 this 绑定）----

TEST(FunctionTest, FunctionStoredAsObjectProperty) {
    auto r = run(
        "let obj = {};"
        "obj.fn = function(x) { return x + 1; };"
        "obj.fn(5)");
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().as_number(), 6.0);
}

TEST(FunctionTest, MethodCallOnObjectLiteralProperty) {
    auto r = run(
        "let obj = { fn: function(a, b) { return a * b; } };"
        "obj.fn(3, 4)");
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().as_number(), 12.0);
}

// ---- 缺少所有参数时各参数均为 undefined ----

TEST(FunctionTest, AllParamsUndefinedWhenNoArgsGiven) {
    auto r = run(
        "function f(a, b, c) { return a; }"
        "f()");
    ASSERT_TRUE(r.is_ok());
    EXPECT_TRUE(r.value().is_undefined());
}

TEST(FunctionTest, MissingMiddleParamIsUndefined) {
    auto r = run(
        "function f(a, b, c) { return b; }"
        "f(1)");
    ASSERT_TRUE(r.is_ok());
    EXPECT_TRUE(r.value().is_undefined());
}

// ---- 字符串参数与字符串操作 ----

TEST(FunctionTest, StringArgConcatenation) {
    auto r = run(
        "function greet(name) { return \"hello \" + name; }"
        "greet(\"world\")");
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().as_string(), "hello world");
}

TEST(FunctionTest, StringArgPassedThrough) {
    auto r = run(
        "function identity(x) { return x; }"
        "identity(\"test\")");
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().as_string(), "test");
}

// ---- 调用非函数的附加场景 ----

// 调用 null 抛 TypeError
TEST(FunctionTest, CallNullThrowsTypeError) {
    auto r = run("let x = null; x()");
    ASSERT_FALSE(r.is_ok());
    EXPECT_NE(r.error().message().find("TypeError"), std::string::npos);
}

// 调用 undefined 抛 TypeError
TEST(FunctionTest, CallUndefinedThrowsTypeError) {
    auto r = run("let x = undefined; x()");
    ASSERT_FALSE(r.is_ok());
    EXPECT_NE(r.error().message().find("TypeError"), std::string::npos);
}

// 调用字符串抛 TypeError
TEST(FunctionTest, CallStringThrowsTypeError) {
    auto r = run("let x = \"hello\"; x()");
    ASSERT_FALSE(r.is_ok());
    EXPECT_NE(r.error().message().find("TypeError"), std::string::npos);
}

// ---- 递归边界补充 ----

// 互递归（mutual recursion）
TEST(FunctionTest, MutualRecursion) {
    auto r = run(
        "function isEven(n) { if (n === 0) { return true; } return isOdd(n - 1); }"
        "function isOdd(n) { if (n === 0) { return false; } return isEven(n - 1); }"
        "isEven(4)");
    ASSERT_TRUE(r.is_ok());
    EXPECT_TRUE(r.value().as_bool());
}

TEST(FunctionTest, MutualRecursionOddResult) {
    auto r = run(
        "function isEven(n) { if (n === 0) { return true; } return isOdd(n - 1); }"
        "function isOdd(n) { if (n === 0) { return false; } return isEven(n - 1); }"
        "isOdd(3)");
    ASSERT_TRUE(r.is_ok());
    EXPECT_TRUE(r.value().as_bool());
}

}  // namespace
}  // namespace qppjs
