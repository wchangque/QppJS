#include "qppjs/frontend/parser.h"
#include "qppjs/runtime/interpreter.h"

#include <gtest/gtest.h>

#include <string_view>

namespace {

// Helper: parse + exec, expect success, return Value
qppjs::Value exec_ok(std::string_view source) {
    auto parse_result = qppjs::parse_program(source);
    EXPECT_TRUE(parse_result.ok()) << "parse failed: " << parse_result.error().message();
    qppjs::Interpreter interp;
    auto result = interp.exec(parse_result.value());
    EXPECT_TRUE(result.is_ok()) << "exec failed: " << result.error().message();
    return result.value();
}

// ============================================================
// finally 覆盖规则
// ============================================================

// try{return 1} finally{return 2} → finally return 覆盖 try return
TEST(InterpreterFinallyOverride, FinallyReturnOverridesTryReturn) {
    auto v = exec_ok(
        "function f() { try { return 1; } finally { return 2; } }"
        "f()");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.0);
}

// try{throw A} finally{return B} → finally return 丢弃 throw
TEST(InterpreterFinallyOverride, FinallyReturnDiscardsThrow) {
    auto v = exec_ok(
        "function f() { try { throw 1; } finally { return 42; } }"
        "f()");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 42.0);
}

// try{throw A} finally{normal} → 抛出 A（finally 正常不替换 throw）
TEST(InterpreterFinallyOverride, FinallyNormalPreservesThrow) {
    auto v = exec_ok(
        "let r = \"none\";"
        "try {"
        "  try { throw \"err\"; } finally { r = \"finally\"; }"
        "} catch(e) { r = e; }"
        "r");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "err");
}

// try{return 1} finally{normal} → 返回 1（finally 正常不替换 return）
TEST(InterpreterFinallyOverride, FinallyNormalPreservesTryReturn) {
    auto v = exec_ok(
        "function f() { try { return 1; } finally { /* no-op */ } }"
        "f()");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

// finally 有副作用但不 return —— 副作用可见，return 值仍来自 try
TEST(InterpreterFinallyOverride, FinallyNormalSideEffectWithTryReturn) {
    auto v = exec_ok(
        "let side = 0;"
        "function f() { try { return 1; } finally { side = 99; } }"
        "f();"
        "side");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 99.0);
}

// try{} catch{} finally{} 三块都有，try 正常完成
TEST(InterpreterFinallyOverride, AllThreeBlocksTryNormal) {
    auto v = exec_ok(
        "let r = 0;"
        "try { r = 1; } catch(e) { r = 99; } finally { r = r + 10; }"
        "r");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 11.0);
}

// ============================================================
// 嵌套 try 场景
// ============================================================

// 内层 catch 不处理（重新 throw），外层 catch 接收
TEST(InterpreterNestedTry, InnerCatchRethrowOuterCatchReceives) {
    auto v = exec_ok(
        "let r;"
        "try {"
        "  try { throw 1; } catch(e) { throw 2; }"
        "} catch(e) { r = e; }"
        "r");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.0);
}

// 内层 finally 执行后，外层 catch 接收原始 throw
TEST(InterpreterNestedTry, InnerFinallyNormalOuterCatchReceivesThrow) {
    auto v = exec_ok(
        "let r;"
        "try {"
        "  try { throw 1; } finally { /* normal */ }"
        "} catch(e) { r = e; }"
        "r");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

// 三层嵌套：最内层 throw，中间 finally 正常，最外层 catch 接收
TEST(InterpreterNestedTry, ThreeLevelNestFinallyNormal) {
    auto v = exec_ok(
        "let r;"
        "try {"
        "  try {"
        "    try { throw \"deep\"; } finally {}"
        "  } finally {}"
        "} catch(e) { r = e; }"
        "r");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "deep");
}

// ============================================================
// catch 参数作用域
// ============================================================

// catch 参数遮蔽外层同名变量，catch 结束后外层恢复
TEST(InterpreterCatchScope, CatchParamShadowsOuter) {
    auto v = exec_ok(
        "let e = 10;"
        "try { throw 99; } catch(e) { /* inner e = 99 */ }"
        "e");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 10.0);
}

// catch 内对参数赋值不影响外层同名变量
TEST(InterpreterCatchScope, CatchParamAssignNoEffect) {
    auto v = exec_ok(
        "let e = 10;"
        "try { throw 1; } catch(e) { e = 99; }"
        "e");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 10.0);
}

// catch 块内用 let 声明的变量不在 catch 外可见
TEST(InterpreterCatchScope, CatchBlockLetNotVisible) {
    auto v = exec_ok(
        "let result = \"outside\";"
        "try { throw 1; } catch(e) { let inner = \"inside\"; result = inner; }"
        "result");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "inside");
}

// ============================================================
// break/continue 边界
// ============================================================

// 多层嵌套循环，labeled break 跳出最外层
TEST(InterpreterLabeledEdge, LabeledBreakFromThreeLevels) {
    auto v = exec_ok(
        "let sum = 0;"
        "outer: for (let i = 0; i < 3; i = i + 1) {"
        "  for (let j = 0; j < 3; j = j + 1) {"
        "    if (j === 1) break outer;"
        "    sum = sum + 1;"
        "  }"
        "}"
        "sum");
    EXPECT_TRUE(v.is_number());
    // i=0, j=0: sum=1, j=1: break outer → total sum=1
    EXPECT_EQ(v.as_number(), 1.0);
}

// labeled continue 继续最外层循环（内层循环退出），outer 的 update 执行
TEST(InterpreterLabeledEdge, LabeledContinueOuterUpdateExecutes) {
    auto v = exec_ok(
        "let iters = 0;"
        "outer: for (let i = 0; i < 3; i = i + 1) {"
        "  iters = iters + 1;"
        "  for (let j = 0; j < 3; j = j + 1) {"
        "    if (j === 0) continue outer;"
        "  }"
        "}"
        "iters");
    EXPECT_TRUE(v.is_number());
    // 每次 continue outer 后 i 会 +1，最终执行 3 次
    EXPECT_EQ(v.as_number(), 3.0);
}

// continue 在 for 循环中，update 表达式仍然执行
TEST(InterpreterLabeledEdge, ForContinueUpdateStillExecutes) {
    auto v = exec_ok(
        "let update_count = 0;"
        "for (let i = 0; i < 3; update_count = update_count + 1) {"
        "  if (i === 0) { i = i + 1; continue; }"
        "  i = i + 1;"
        "}"
        "update_count");
    EXPECT_TRUE(v.is_number());
    // i: 0→skip body but i+1+update, then check i=1→body i+1=2+update, check i=2<3→i+1=3+update, done
    // update 执行 3 次
    EXPECT_EQ(v.as_number(), 3.0);
}

// for 循环的 init 是 var 声明，var 在函数作用域可见
TEST(InterpreterLabeledEdge, ForVarInitVisibleAfterLoop) {
    auto v = exec_ok(
        "var result = 0;"
        "for (var i = 0; i < 3; i = i + 1) {}"
        "i");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 3.0);
}

// ============================================================
// Error 对象边界
// ============================================================

// new Error() 无参数，message 为空字符串
TEST(InterpreterErrorEdge, NewErrorNoArgEmptyMessage) {
    auto v = exec_ok("new Error().message");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "");
}

// new Error(42) 数字参数，message 为 "42"
TEST(InterpreterErrorEdge, NewErrorNumberArgMessageIsString) {
    auto v = exec_ok("new Error(42).message");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "42");
}

// new Error("msg").name === "Error"
TEST(InterpreterErrorEdge, NewErrorNameProperty) {
    auto v = exec_ok("new Error(\"msg\").name");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "Error");
}

// ============================================================
// 引擎内部错误被 catch
// ============================================================

// null.x → TypeError，被 catch 捕获
TEST(InterpreterInternalError, NullPropertyAccessCaught) {
    auto v = exec_ok(
        "let r = \"none\";"
        "try { null.x; } catch(e) { r = \"caught\"; }"
        "r");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "caught");
}

// undefined.x → TypeError，被 catch 捕获
TEST(InterpreterInternalError, UndefinedPropertyAccessCaught) {
    auto v = exec_ok(
        "let r = \"none\";"
        "try { undefined.x; } catch(e) { r = \"caught\"; }"
        "r");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "caught");
}

// 未声明变量 → ReferenceError，被 catch 捕获
TEST(InterpreterInternalError, ReferenceErrorCaught) {
    auto v = exec_ok(
        "let r = \"none\";"
        "try { let x = undeclaredVar; } catch(e) { r = \"caught\"; }"
        "r");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "caught");
}

// ============================================================
// throw 非 Error 对象
// ============================================================

// throw 42 catch 到数字
TEST(InterpreterThrowEdge, ThrowNumberCaught) {
    auto v = exec_ok(
        "let r;"
        "try { throw 42; } catch(e) { r = e; }"
        "r");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 42.0);
}

// throw null catch 到 null
TEST(InterpreterThrowEdge, ThrowNullCaught) {
    auto v = exec_ok(
        "let r = false;"
        "try { throw null; } catch(e) { r = (e === null); }"
        "r");
    EXPECT_TRUE(v.is_bool());
    EXPECT_EQ(v.as_bool(), true);
}

// throw { code: 404 } catch 到对象，可访问 .code
TEST(InterpreterThrowEdge, ThrowObjectCaughtAccessProperty) {
    auto v = exec_ok(
        "let r;"
        "try { throw { code: 404 }; } catch(e) { r = e.code; }"
        "r");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 404.0);
}

// ============================================================
// for 循环边界
// ============================================================

// for(;;) { break; } 无限循环立即 break
TEST(InterpreterForEdge, InfiniteLoopImmediateBreak) {
    auto v = exec_ok(
        "let x = 0;"
        "for (;;) { x = 1; break; }"
        "x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

// for(let i=0; i<0; i++) 循环体不执行
TEST(InterpreterForEdge, ForBodyNotExecutedWhenCondFalse) {
    auto v = exec_ok(
        "let x = 0;"
        "for (let i = 0; i < 0; i = i + 1) { x = 99; }"
        "x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}

// for 循环 continue 后 update 仍执行（计数验证）
TEST(InterpreterForEdge, ContinueDoesNotSkipUpdate) {
    auto v = exec_ok(
        "let sum = 0;"
        "for (let i = 0; i < 5; i = i + 1) {"
        "  if (i % 2 === 0) continue;"
        "  sum = sum + i;"
        "}"
        "sum");
    EXPECT_TRUE(v.is_number());
    // 奇数 1+3 = 4
    EXPECT_EQ(v.as_number(), 4.0);
}

// for 循环无条件（只有 init 和 update 缺失）立即退出
TEST(InterpreterForEdge, ForEmptyBodyBreak) {
    auto v = exec_ok(
        "let cnt = 0;"
        "for (let i = 0; i < 3; i = i + 1) { cnt = cnt + 1; }"
        "cnt");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 3.0);
}

}  // namespace
