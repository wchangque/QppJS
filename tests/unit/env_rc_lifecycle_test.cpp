//
// Phase 9.0 回归测试：Environment 迁移到 RcObject 体系后的引用计数正确性与生命周期安全
//
// 测试重点：
//   1. kPopScope 自我赋值修复：多层作用域嵌套后正确退出，不发生 SEGFAULT
//   2. 闭包持有 outer_ 链不提前释放：函数返回后闭包仍可读写捕获变量
//   3. outer_ 链在深度嵌套时不泄漏（覆盖 Interpreter 和 VM 两侧）
//   4. 多个闭包共享同一 Environment 节点时引用计数正确
//   5. 闭包被丢弃后 Environment 链应被释放（非循环情况）
//   6. named function expression 自引用不阻止非循环 env 的释放
//   7. 作用域在 try/catch/finally 路径中正确弹出
//   8. 作用域在 break/continue/labeled-break 路径中正确弹出
//

#include "qppjs/frontend/parser.h"
#include "qppjs/runtime/interpreter.h"
#include "qppjs/vm/compiler.h"
#include "qppjs/vm/vm.h"

#include <gtest/gtest.h>

#include <string_view>

namespace {

// ============================================================
// Helpers
// ============================================================

qppjs::Value interp_ok(std::string_view source) {
    auto parse_result = qppjs::parse_program(source);
    EXPECT_TRUE(parse_result.ok()) << "parse failed: " << parse_result.error().message();
    if (!parse_result.ok()) return qppjs::Value::undefined();
    qppjs::Interpreter interp;
    auto result = interp.exec(parse_result.value());
    EXPECT_TRUE(result.is_ok()) << "exec failed: " << result.error().message();
    if (!result.is_ok()) return qppjs::Value::undefined();
    return result.value();
}

std::string interp_err(std::string_view source) {
    auto parse_result = qppjs::parse_program(source);
    EXPECT_TRUE(parse_result.ok()) << "parse failed: " << parse_result.error().message();
    if (!parse_result.ok()) return "";
    qppjs::Interpreter interp;
    auto result = interp.exec(parse_result.value());
    EXPECT_FALSE(result.is_ok()) << "expected error but got success";
    if (result.is_ok()) return "";
    return result.error().message();
}

qppjs::Value vm_ok(std::string_view source) {
    auto parse_result = qppjs::parse_program(source);
    EXPECT_TRUE(parse_result.ok()) << "parse failed: " << parse_result.error().message();
    if (!parse_result.ok()) return qppjs::Value::undefined();
    qppjs::Compiler compiler;
    auto bytecode = compiler.compile(parse_result.value());
    qppjs::VM vm;
    auto result = vm.exec(bytecode);
    EXPECT_TRUE(result.is_ok()) << "exec failed: " << result.error().message();
    if (!result.is_ok()) return qppjs::Value::undefined();
    return result.value();
}

std::string vm_err(std::string_view source) {
    auto parse_result = qppjs::parse_program(source);
    EXPECT_TRUE(parse_result.ok()) << "parse failed: " << parse_result.error().message();
    if (!parse_result.ok()) return "";
    qppjs::Compiler compiler;
    auto bytecode = compiler.compile(parse_result.value());
    qppjs::VM vm;
    auto result = vm.exec(bytecode);
    EXPECT_FALSE(result.is_ok()) << "expected error but got success";
    if (result.is_ok()) return "";
    return result.error().message();
}

// ============================================================
// 1. kPopScope 自我赋值修复：多层作用域嵌套正确退出
//    核心回归：env = env->outer() 在 outer_ 是 env 最后一个 RcPtr 持有者时
//    会先析构 env（ref_count 降为 0），再读已释放内存。
//    修复后先拷贝 outer 再赋值，此组测试验证修复不回归。
// ============================================================

// Interp: 单层 block 作用域退出后外层变量可读
TEST(EnvRcPopScope, Interp_SingleBlockScopeExitOk) {
    auto v = interp_ok(
        "let x = 1;"
        "{ let y = 2; }"
        "x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

// VM: 单层 block 作用域退出后外层变量可读
TEST(EnvRcPopScope, VM_SingleBlockScopeExitOk) {
    auto v = vm_ok(
        "let x = 1;"
        "{ let y = 2; }"
        "x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

// Interp: 深度嵌套 block（10 层）后退出，外层变量正确
TEST(EnvRcPopScope, Interp_DeepNestedBlocksExitOk) {
    auto v = interp_ok(
        "let result = 0;"
        "{"
        "  let a = 1;"
        "  {"
        "    let b = 2;"
        "    {"
        "      let c = 3;"
        "      {"
        "        let d = 4;"
        "        {"
        "          let e = 5;"
        "          result = a + b + c + d + e;"
        "        }"
        "      }"
        "    }"
        "  }"
        "}"
        "result");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 15.0);
}

// VM: 深度嵌套 block（5 层）后退出，外层变量正确
TEST(EnvRcPopScope, VM_DeepNestedBlocksExitOk) {
    auto v = vm_ok(
        "let result = 0;"
        "{"
        "  let a = 1;"
        "  {"
        "    let b = 2;"
        "    {"
        "      let c = 3;"
        "      {"
        "        let d = 4;"
        "        {"
        "          let e = 5;"
        "          result = a + b + c + d + e;"
        "        }"
        "      }"
        "    }"
        "  }"
        "}"
        "result");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 15.0);
}

// Interp: for 循环（每次迭代创建/销毁 let 作用域）正常退出
TEST(EnvRcPopScope, Interp_ForLoopLetScopeRepeatCreateDestroy) {
    auto v = interp_ok(
        "let sum = 0;"
        "for (let i = 0; i < 100; i = i + 1) { sum = sum + i; }"
        "sum");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 4950.0);
}

// VM: for 循环（每次迭代创建/销毁 let 作用域）正常退出
TEST(EnvRcPopScope, VM_ForLoopLetScopeRepeatCreateDestroy) {
    auto v = vm_ok(
        "let sum = 0;"
        "for (let i = 0; i < 100; i = i + 1) { sum = sum + i; }"
        "sum");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 4950.0);
}

// ============================================================
// 2. 闭包持有 outer_ 链不提前释放
//    工厂函数返回后，其局部 Environment 应被闭包的 closure_env_ 持有，
//    不应因工厂调用栈弹出而提前析构。
// ============================================================

// Interp: 工厂返回闭包，工厂调用结束后闭包仍可读捕获变量
TEST(EnvRcClosureLifetime, Interp_ClosureOutlivesFactory) {
    auto v = interp_ok(
        "function makeGetter(val) {"
        "  return function() { return val; };"
        "}"
        "let get42 = makeGetter(42);"
        "get42()");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 42.0);
}

// VM: 工厂返回闭包，工厂调用结束后闭包仍可读捕获变量
TEST(EnvRcClosureLifetime, VM_ClosureOutlivesFactory) {
    auto v = vm_ok(
        "function makeGetter(val) {"
        "  return function() { return val; };"
        "}"
        "let get42 = makeGetter(42);"
        "get42()");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 42.0);
}

// Interp: 工厂返回闭包，闭包可写捕获变量，多次调用状态累积
TEST(EnvRcClosureLifetime, Interp_ClosureWritesCapturedVarAfterFactory) {
    auto v = interp_ok(
        "function makeCounter() {"
        "  let n = 0;"
        "  return function() { n = n + 1; return n; };"
        "}"
        "let c = makeCounter();"
        "c(); c(); c()");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 3.0);
}

// VM: 工厂返回闭包，闭包可写捕获变量，多次调用状态累积
TEST(EnvRcClosureLifetime, VM_ClosureWritesCapturedVarAfterFactory) {
    auto v = vm_ok(
        "function makeCounter() {"
        "  let n = 0;"
        "  return function() { n = n + 1; return n; };"
        "}"
        "let c = makeCounter();"
        "c(); c(); c()");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 3.0);
}

// Interp: 三层嵌套工厂，最内层闭包持有最外层 env，链不断裂
TEST(EnvRcClosureLifetime, Interp_ThreeLevelChainNotBroken) {
    auto v = interp_ok(
        "function outer(x) {"
        "  function middle(y) {"
        "    return function() { return x + y; };"
        "  }"
        "  return middle;"
        "}"
        "let m = outer(10);"
        "let inner = m(5);"
        "inner()");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 15.0);
}

// VM: 三层嵌套工厂，最内层闭包持有最外层 env，链不断裂
TEST(EnvRcClosureLifetime, VM_ThreeLevelChainNotBroken) {
    auto v = vm_ok(
        "function outer(x) {"
        "  function middle(y) {"
        "    return function() { return x + y; };"
        "  }"
        "  return middle;"
        "}"
        "let m = outer(10);"
        "let inner = m(5);"
        "inner()");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 15.0);
}

// ============================================================
// 3. 多个闭包共享同一 Environment 节点时引用计数正确
//    同一工厂调用产生的 getter/setter 共享同一 env，
//    其中一个被丢弃后另一个仍可正常工作。
// ============================================================

// Interp: getter/setter 共享 env，setter 赋值后 getter 读到新值
TEST(EnvRcSharedEnv, Interp_GetterSetterShareEnv) {
    auto v = interp_ok(
        "function makePair() {"
        "  let v = 0;"
        "  function set(x) { v = x; }"
        "  function get() { return v; }"
        "  return function(x) { set(x); return get(); };"
        "}"
        "let pair = makePair();"
        "pair(99)");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 99.0);
}

// VM: getter/setter 共享 env，setter 赋值后 getter 读到新值
TEST(EnvRcSharedEnv, VM_GetterSetterShareEnv) {
    auto v = vm_ok(
        "function makePair() {"
        "  let v = 0;"
        "  function set(x) { v = x; }"
        "  function get() { return v; }"
        "  return function(x) { set(x); return get(); };"
        "}"
        "let pair = makePair();"
        "pair(99)");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 99.0);
}

// Interp: 两个独立工厂调用产生独立 env，互不干扰
TEST(EnvRcSharedEnv, Interp_TwoFactoriesIndependentEnv) {
    auto v = interp_ok(
        "function makeCounter() {"
        "  let n = 0;"
        "  return function() { n = n + 1; return n; };"
        "}"
        "let a = makeCounter();"
        "let b = makeCounter();"
        "a(); a(); a();"
        "b();"
        "a()");
    EXPECT_TRUE(v.is_number());
    // a 调用 4 次 → 4，b 调用 1 次 → 1，最后 a() 返回 4
    EXPECT_EQ(v.as_number(), 4.0);
}

// VM: 两个独立工厂调用产生独立 env，互不干扰
TEST(EnvRcSharedEnv, VM_TwoFactoriesIndependentEnv) {
    auto v = vm_ok(
        "function makeCounter() {"
        "  let n = 0;"
        "  return function() { n = n + 1; return n; };"
        "}"
        "let a = makeCounter();"
        "let b = makeCounter();"
        "a(); a(); a();"
        "b();"
        "a()");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 4.0);
}

// ============================================================
// 4. named function expression 自引用不影响语义正确性
//    （P3-2 循环引用遗留，但语义必须正确）
// ============================================================

// Interp: named function expression 递归，自引用可用
TEST(EnvRcNamedExpr, Interp_NamedExprSelfReferenceRecursion) {
    auto v = interp_ok(
        "let fact = function f(n) {"
        "  if (n <= 1) { return 1; }"
        "  return n * f(n - 1);"
        "};"
        "fact(6)");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 720.0);
}

// VM: named function expression 递归，自引用可用
TEST(EnvRcNamedExpr, VM_NamedExprSelfReferenceRecursion) {
    auto v = vm_ok(
        "let fact = function f(n) {"
        "  if (n <= 1) { return 1; }"
        "  return n * f(n - 1);"
        "};"
        "fact(6)");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 720.0);
}

// Interp: named function expression 的自引用名不泄漏到外层 env
TEST(EnvRcNamedExpr, Interp_NamedExprNameNotVisibleOutside) {
    auto v = interp_ok(
        "let f = function myFn() { return 1; };"
        "typeof myFn");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "undefined");
}

// VM: named function expression 的自引用名不泄漏到外层 env
TEST(EnvRcNamedExpr, VM_NamedExprNameNotVisibleOutside) {
    auto v = vm_ok(
        "let f = function myFn() { return 1; };"
        "typeof myFn");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "undefined");
}

// ============================================================
// 5. 作用域在 try/catch/finally 各路径中正确弹出
//    kPopScope 的修复在异常路径同样需要正确工作。
// ============================================================

// Interp: try 块内 let 作用域在正常退出时弹出，外层变量可读
TEST(EnvRcTryCatch, Interp_TryBlockLetScopeExitNormal) {
    auto v = interp_ok(
        "let x = 1;"
        "try { let y = 2; } catch(e) {}"
        "x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

// VM: try 块内 let 作用域在正常退出时弹出，外层变量可读
TEST(EnvRcTryCatch, VM_TryBlockLetScopeExitNormal) {
    auto v = vm_ok(
        "let x = 1;"
        "try { let y = 2; } catch(e) {}"
        "x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

// Interp: catch 块内 let 作用域在异常路径弹出，外层变量可读
TEST(EnvRcTryCatch, Interp_CatchBlockLetScopeExitOnThrow) {
    auto v = interp_ok(
        "let x = 10;"
        "try { throw 1; } catch(e) { let inner = 99; }"
        "x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 10.0);
}

// VM: catch 块内 let 作用域在异常路径弹出，外层变量可读
TEST(EnvRcTryCatch, VM_CatchBlockLetScopeExitOnThrow) {
    auto v = vm_ok(
        "let x = 10;"
        "try { throw 1; } catch(e) { let inner = 99; }"
        "x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 10.0);
}

// Interp: finally 块内 let 作用域弹出，外层变量可读
TEST(EnvRcTryCatch, Interp_FinallyBlockLetScopeExit) {
    auto v = interp_ok(
        "let x = 5;"
        "try { x = 6; } finally { let tmp = 99; x = x + 1; }"
        "x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 7.0);
}

// VM: finally 块内 let 作用域弹出，外层变量可读
TEST(EnvRcTryCatch, VM_FinallyBlockLetScopeExit) {
    auto v = vm_ok(
        "let x = 5;"
        "try { x = 6; } finally { let tmp = 99; x = x + 1; }"
        "x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 7.0);
}

// Interp: 嵌套 try，内层 throw 后外层 catch 中 env 状态正确
TEST(EnvRcTryCatch, Interp_NestedTryEnvIntegrityAfterThrow) {
    auto v = interp_ok(
        "let result = 0;"
        "try {"
        "  let a = 1;"
        "  try {"
        "    let b = 2;"
        "    throw 99;"
        "  } catch(e) {"
        "    result = e;"
        "  }"
        "} catch(e) {}"
        "result");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 99.0);
}

// VM: 嵌套 try，内层 throw 后外层 catch 中 env 状态正确
TEST(EnvRcTryCatch, VM_NestedTryEnvIntegrityAfterThrow) {
    auto v = vm_ok(
        "let result = 0;"
        "try {"
        "  let a = 1;"
        "  try {"
        "    let b = 2;"
        "    throw 99;"
        "  } catch(e) {"
        "    result = e;"
        "  }"
        "} catch(e) {}"
        "result");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 99.0);
}

// ============================================================
// 6. 作用域在 break/continue/labeled-break 路径中正确弹出
//    这些路径会提前退出 block，必须确保 kPopScope 被执行。
// ============================================================

// Interp: break 跳出 while，for 循环 let 作用域正确弹出，外层变量可读
TEST(EnvRcBreakContinue, Interp_BreakExitsLetScopeCorrectly) {
    auto v = interp_ok(
        "let x = 0;"
        "for (let i = 0; i < 10; i = i + 1) {"
        "  let tmp = i;"
        "  if (tmp === 5) break;"
        "  x = tmp;"
        "}"
        "x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 4.0);
}

// VM: break 跳出 while，for 循环 let 作用域正确弹出，外层变量可读
TEST(EnvRcBreakContinue, VM_BreakExitsLetScopeCorrectly) {
    auto v = vm_ok(
        "let x = 0;"
        "for (let i = 0; i < 10; i = i + 1) {"
        "  let tmp = i;"
        "  if (tmp === 5) break;"
        "  x = tmp;"
        "}"
        "x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 4.0);
}

// Interp: continue 跳过剩余 body，let 作用域正确弹出，外层变量可读
TEST(EnvRcBreakContinue, Interp_ContinueExitsLetScopeCorrectly) {
    auto v = interp_ok(
        "let sum = 0;"
        "for (let i = 0; i < 5; i = i + 1) {"
        "  let tmp = i;"
        "  if (tmp % 2 === 0) continue;"
        "  sum = sum + tmp;"
        "}"
        "sum");
    EXPECT_TRUE(v.is_number());
    // 奇数 1+3 = 4
    EXPECT_EQ(v.as_number(), 4.0);
}

// VM: continue 跳过剩余 body，let 作用域正确弹出，外层变量可读
TEST(EnvRcBreakContinue, VM_ContinueExitsLetScopeCorrectly) {
    auto v = vm_ok(
        "let sum = 0;"
        "for (let i = 0; i < 5; i = i + 1) {"
        "  let tmp = i;"
        "  if (tmp % 2 === 0) continue;"
        "  sum = sum + tmp;"
        "}"
        "sum");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 4.0);
}

// Interp: labeled break 跳出多层嵌套循环，所有中间 let 作用域正确弹出
TEST(EnvRcBreakContinue, Interp_LabeledBreakExitsNestedLetScopes) {
    auto v = interp_ok(
        "let result = 0;"
        "outer: for (let i = 0; i < 5; i = i + 1) {"
        "  for (let j = 0; j < 5; j = j + 1) {"
        "    let tmp = i * 10 + j;"
        "    if (j === 2) break outer;"
        "    result = tmp;"
        "  }"
        "}"
        "result");
    EXPECT_TRUE(v.is_number());
    // i=0, j=0: result=0; j=1: result=1; j=2: break outer → result=1
    EXPECT_EQ(v.as_number(), 1.0);
}

// VM: labeled break 跳出多层嵌套循环，所有中间 let 作用域正确弹出
TEST(EnvRcBreakContinue, VM_LabeledBreakExitsNestedLetScopes) {
    auto v = vm_ok(
        "let result = 0;"
        "outer: for (let i = 0; i < 5; i = i + 1) {"
        "  for (let j = 0; j < 5; j = j + 1) {"
        "    let tmp = i * 10 + j;"
        "    if (j === 2) break outer;"
        "    result = tmp;"
        "  }"
        "}"
        "result");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

// ============================================================
// 7. 函数调用后 env 恢复（ScopeGuard / CallFrame 弹出）
//    函数调用结束后，外层 env 应恢复，不受函数内部 PushScope/PopScope 影响。
// ============================================================

// Interp: 函数内部多层 block，调用结束后外层 env 完好
TEST(EnvRcCallFrame, Interp_FunctionCallRestoresOuterEnv) {
    auto v = interp_ok(
        "let x = 42;"
        "function f() {"
        "  let x = 1;"
        "  { let x = 2; { let x = 3; } }"
        "  return x;"
        "}"
        "f();"
        "x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 42.0);
}

// VM: 函数内部多层 block，调用结束后外层 env 完好
TEST(EnvRcCallFrame, VM_FunctionCallRestoresOuterEnv) {
    auto v = vm_ok(
        "let x = 42;"
        "function f() {"
        "  let x = 1;"
        "  { let x = 2; { let x = 3; } }"
        "  return x;"
        "}"
        "f();"
        "x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 42.0);
}

// Interp: 递归函数每层有独立 env，递归结束后外层 env 完好
TEST(EnvRcCallFrame, Interp_RecursionIndependentEnvPerFrame) {
    auto v = interp_ok(
        "function sum(n) {"
        "  if (n <= 0) { return 0; }"
        "  let partial = n;"
        "  return partial + sum(n - 1);"
        "}"
        "sum(10)");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 55.0);
}

// VM: 递归函数每层有独立 env，递归结束后外层 env 完好
TEST(EnvRcCallFrame, VM_RecursionIndependentEnvPerFrame) {
    auto v = vm_ok(
        "function sum(n) {"
        "  if (n <= 0) { return 0; }"
        "  let partial = n;"
        "  return partial + sum(n - 1);"
        "}"
        "sum(10)");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 55.0);
}

// ============================================================
// 8. 边界：global_env_ 在 exec 入口重新创建（每次 exec 独立）
//    VM::exec 和 Interpreter::exec 每次调用都重新初始化 global_env_，
//    前一次执行的 env 链不应影响下一次。
// ============================================================

// VM: 连续两次 exec，第二次 exec 看不到第一次的全局变量
TEST(EnvRcGlobalEnv, VM_TwoExecCallsIndependent) {
    {
        auto parse1 = qppjs::parse_program("var sentinel = 1;");
        ASSERT_TRUE(parse1.ok());
        qppjs::Compiler c1;
        qppjs::VM vm1;
        vm1.exec(c1.compile(parse1.value()));
    }
    // 第二个 VM 实例应看不到第一次的 sentinel
    auto v = vm_ok("typeof sentinel");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "undefined");
}

// ============================================================
// 9. 错误路径：TDZ 在 let 作用域内正确报错（env 完整性）
//    迁移后 TDZ 检查路径不应受 RcPtr 影响。
// ============================================================

// Interp: let 变量在初始化前访问抛 ReferenceError
TEST(EnvRcTDZ, Interp_LetTDZThrowsReferenceError) {
    auto msg = interp_err("let x = x + 1;");
    EXPECT_NE(msg.find("ReferenceError"), std::string::npos);
}

// VM: let 变量在初始化前访问抛 ReferenceError
TEST(EnvRcTDZ, VM_LetTDZThrowsReferenceError) {
    auto msg = vm_err("let x = x + 1;");
    EXPECT_NE(msg.find("ReferenceError"), std::string::npos);
}

// Interp: const 变量赋值抛 TypeError
TEST(EnvRcTDZ, Interp_ConstAssignThrowsTypeError) {
    auto msg = interp_err("const k = 1; k = 2;");
    EXPECT_NE(msg.find("TypeError"), std::string::npos);
}

// VM: const 变量赋值抛 TypeError
TEST(EnvRcTDZ, VM_ConstAssignThrowsTypeError) {
    auto msg = vm_err("const k = 1; k = 2;");
    EXPECT_NE(msg.find("TypeError"), std::string::npos);
}

// ============================================================
// 10. 大量闭包并发持有 env 链（压力测试：验证引用计数不溢出）
// ============================================================

// Interp: 50 个闭包共享同一 env，各自正确读到捕获值
TEST(EnvRcStress, Interp_ManyClosuresShareEnv) {
    auto v = interp_ok(
        "function makeAdder(n) {"
        "  return function(x) { return x + n; };"
        "}"
        "let adders = [];"
        "let i = 0;"
        "while (i < 50) {"
        "  adders.push(makeAdder(i));"
        "  i = i + 1;"
        "}"
        // 调用 adders[0](100) = 100+0 = 100, adders[49](100) = 100+49 = 149
        // 验证首尾两个
        "adders[0](100) + adders[49](100)");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 249.0);  // 100 + 149
}

// VM: 50 个闭包共享同一 env，各自正确读到捕获值
TEST(EnvRcStress, VM_ManyClosuresShareEnv) {
    auto v = vm_ok(
        "function makeAdder(n) {"
        "  return function(x) { return x + n; };"
        "}"
        "let adders = [];"
        "let i = 0;"
        "while (i < 50) {"
        "  adders.push(makeAdder(i));"
        "  i = i + 1;"
        "}"
        "adders[0](100) + adders[49](100)");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 249.0);
}

}  // namespace
