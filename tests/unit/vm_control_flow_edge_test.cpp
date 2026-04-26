#include "qppjs/frontend/parser.h"
#include "qppjs/vm/compiler.h"
#include "qppjs/vm/vm.h"

#include <gtest/gtest.h>

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
// finally 覆盖规则
// ============================================================

// try{return 1} finally{return 2} → finally return 覆盖 try return
// DISABLED: VM 的 compile_return_stmt 未检查 finally_info_stack_，
// return 直接发 kReturn 跳过 finally 块，行为与规范不符。
// 等 compile_return_stmt 修复后启用。
TEST(VMFinallyOverride, FinallyReturnOverridesTryReturn) {
    auto v = vm_ok(
        "function f() { try { return 1; } finally { return 2; } }"
        "f()");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.0);
}

// try{throw A} finally{return B} → finally return 丢弃 throw
TEST(VMFinallyOverride, FinallyReturnDiscardsThrow) {
    auto v = vm_ok(
        "function f() { try { throw 1; } finally { return 42; } }"
        "f()");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 42.0);
}

// try{throw A} finally{normal} → 抛出 A（finally 正常不替换 throw）
TEST(VMFinallyOverride, FinallyNormalPreservesThrow) {
    auto v = vm_ok(
        "let r = \"none\";"
        "try {"
        "  try { throw \"err\"; } finally { r = \"finally\"; }"
        "} catch(e) { r = e; }"
        "r");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "err");
}

// try{return 1} finally{normal} → 返回 1
TEST(VMFinallyOverride, FinallyNormalPreservesTryReturn) {
    auto v = vm_ok(
        "function f() { try { return 1; } finally { /* no-op */ } }"
        "f()");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

// finally 有副作用但不 return —— 副作用可见，return 值仍来自 try
// DISABLED: 同上，VM 侧 return 直接退出，finally 副作用也未执行。
TEST(VMFinallyOverride, FinallyNormalSideEffectWithTryReturn) {
    auto v = vm_ok(
        "let side = 0;"
        "function f() { try { return 1; } finally { side = 99; } }"
        "f();"
        "side");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 99.0);
}

// try{} catch{} finally{} 三块都有，try 正常完成
TEST(VMFinallyOverride, AllThreeBlocksTryNormal) {
    auto v = vm_ok(
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
TEST(VMNestedTry, InnerCatchRethrowOuterCatchReceives) {
    auto v = vm_ok(
        "let r;"
        "try {"
        "  try { throw 1; } catch(e) { throw 2; }"
        "} catch(e) { r = e; }"
        "r");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.0);
}

// 内层 finally 执行后，外层 catch 接收原始 throw
TEST(VMNestedTry, InnerFinallyNormalOuterCatchReceivesThrow) {
    auto v = vm_ok(
        "let r;"
        "try {"
        "  try { throw 1; } finally { /* normal */ }"
        "} catch(e) { r = e; }"
        "r");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

// 三层嵌套：最内层 throw，中间 finally 正常，最外层 catch 接收
TEST(VMNestedTry, ThreeLevelNestFinallyNormal) {
    auto v = vm_ok(
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
TEST(VMCatchScope, CatchParamShadowsOuter) {
    auto v = vm_ok(
        "let e = 10;"
        "try { throw 99; } catch(e) { /* inner e */ }"
        "e");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 10.0);
}

// catch 内对参数赋值不影响外层同名变量
TEST(VMCatchScope, CatchParamAssignNoEffect) {
    auto v = vm_ok(
        "let e = 10;"
        "try { throw 1; } catch(e) { e = 99; }"
        "e");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 10.0);
}

// ============================================================
// break/continue 边界
// ============================================================

// 多层嵌套循环，labeled break 跳出最外层
TEST(VMLabeledEdge, LabeledBreakFromThreeLevels) {
    auto v = vm_ok(
        "let sum = 0;"
        "outer: for (let i = 0; i < 3; i = i + 1) {"
        "  for (let j = 0; j < 3; j = j + 1) {"
        "    if (j === 1) break outer;"
        "    sum = sum + 1;"
        "  }"
        "}"
        "sum");
    EXPECT_TRUE(v.is_number());
    // i=0, j=0: sum=1, j=1: break outer
    EXPECT_EQ(v.as_number(), 1.0);
}

// labeled continue 继续最外层循环，outer 的 update 执行
TEST(VMLabeledEdge, LabeledContinueOuterUpdateExecutes) {
    auto v = vm_ok(
        "let iters = 0;"
        "outer: for (let i = 0; i < 3; i = i + 1) {"
        "  iters = iters + 1;"
        "  for (let j = 0; j < 3; j = j + 1) {"
        "    if (j === 0) continue outer;"
        "  }"
        "}"
        "iters");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 3.0);
}

// for 循环 continue 后 update 仍执行（计数验证）
TEST(VMLabeledEdge, ForContinueUpdateStillExecutes) {
    auto v = vm_ok(
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

// for 循环的 init 是 var 声明，var 在函数作用域可见
TEST(VMLabeledEdge, ForVarInitVisibleAfterLoop) {
    auto v = vm_ok(
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
TEST(VMErrorEdge, NewErrorNoArgEmptyMessage) {
    auto v = vm_ok("new Error().message");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "");
}

// new Error(42) 数字参数，message 为 "42"
TEST(VMErrorEdge, NewErrorNumberArgMessageIsString) {
    auto v = vm_ok("new Error(42).message");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "42");
}

// new Error("msg").name === "Error"
TEST(VMErrorEdge, NewErrorNameProperty) {
    auto v = vm_ok("new Error(\"msg\").name");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "Error");
}

// ============================================================
// 引擎内部错误被 catch
// ============================================================

// null.x → TypeError，被 catch 捕获
TEST(VMInternalError, NullPropertyAccessCaught) {
    auto v = vm_ok(
        "let r = \"none\";"
        "try { null.x; } catch(e) { r = \"caught\"; }"
        "r");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "caught");
}

// undefined.x → TypeError，被 catch 捕获
TEST(VMInternalError, UndefinedPropertyAccessCaught) {
    auto v = vm_ok(
        "let r = \"none\";"
        "try { undefined.x; } catch(e) { r = \"caught\"; }"
        "r");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "caught");
}

// 未声明变量 → ReferenceError，被 catch 捕获
TEST(VMInternalError, ReferenceErrorCaught) {
    auto v = vm_ok(
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
TEST(VMThrowEdge, ThrowNumberCaught) {
    auto v = vm_ok(
        "let r;"
        "try { throw 42; } catch(e) { r = e; }"
        "r");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 42.0);
}

// throw null catch 到 null
TEST(VMThrowEdge, ThrowNullCaught) {
    auto v = vm_ok(
        "let r = false;"
        "try { throw null; } catch(e) { r = (e === null); }"
        "r");
    EXPECT_TRUE(v.is_bool());
    EXPECT_EQ(v.as_bool(), true);
}

// throw { code: 404 } catch 到对象，可访问 .code
TEST(VMThrowEdge, ThrowObjectCaughtAccessProperty) {
    auto v = vm_ok(
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
TEST(VMForEdge, InfiniteLoopImmediateBreak) {
    auto v = vm_ok(
        "let x = 0;"
        "for (;;) { x = 1; break; }"
        "x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

// for(let i=0; i<0; i++) 循环体不执行
TEST(VMForEdge, ForBodyNotExecutedWhenCondFalse) {
    auto v = vm_ok(
        "let x = 0;"
        "for (let i = 0; i < 0; i = i + 1) { x = 99; }"
        "x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}

// for 循环 continue 后 update 仍执行
TEST(VMForEdge, ContinueDoesNotSkipUpdate) {
    auto v = vm_ok(
        "let sum = 0;"
        "for (let i = 0; i < 5; i = i + 1) {"
        "  if (i % 2 === 0) continue;"
        "  sum = sum + i;"
        "}"
        "sum");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 4.0);
}

// ============================================================
// Phase 8.6 — catch 作用域两层独立（P2-1）
// ============================================================

// catch 参数在 body 中可读
TEST(VMCatchScopeP21, CatchParamReadableInBody) {
    auto v = vm_ok(
        "var r;"
        "try { throw 42; } catch(e) { r = e; }"
        "r");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 42.0);
}

// catch body 中用 let 声明不同名变量，可读 catch 参数
TEST(VMCatchScopeP21, CatchBodyLetCanReadParam) {
    auto v = vm_ok(
        "var r;"
        "try { throw 1; } catch(e) { let x = e + 1; r = x; }"
        "r");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.0);
}

// catch body 中用 let 声明同名变量（遮蔽），外层 catch 参数不受影响
// 修复后 catch 参数在外层 scope，let e 在内层 scope，两者独立
TEST(VMCatchScopeP21, CatchBodyLetSameNameShadows) {
    auto v = vm_ok(
        "var r = 0;"
        "try { throw 10; } catch(e) { let e = 2; r = e; }"
        "r");
    // 内层 let e = 2 遮蔽外层 catch 参数 e，r = 2
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.0);
}

// ============================================================
// Phase 8.7 — labeled block break（P2-2）
// ============================================================

// labeled block break 正常工作
TEST(VMLabeledBlockBreak, BasicLabeledBlockBreak) {
    auto v = vm_ok(
        "var r = 0;"
        "outer: { r = 1; break outer; r = 2; }"
        "r");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

// labeled block 中 var 声明提升，赋值执行
TEST(VMLabeledBlockBreak, VarHoistingInLabeledBlock) {
    auto v = vm_ok(
        "outer: { var x = 1; break outer; }"
        "x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

// labeled block 不影响 return
TEST(VMLabeledBlockBreak, LabeledBlockDoesNotAffectReturn) {
    auto v = vm_ok(
        "function f() { outer: { return 1; break outer; } return 2; }"
        "f()");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

}  // namespace
