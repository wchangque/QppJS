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
// BW-01: ~ (BitNot) 基础
// ============================================================

TEST(Bitwise, BW01_BitNotInterp) {
    auto v = interp_ok("~5");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), -6.0);
}

TEST(Bitwise, BW01_BitNotVM) {
    auto v = vm_ok("~5");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), -6.0);
}

// ============================================================
// BW-02: ~NaN = -1（ToInt32(NaN) = 0，~0 = -1）
// ============================================================

TEST(Bitwise, BW02_BitNotNaNInterp) {
    auto v = interp_ok("~NaN");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), -1.0);
}

TEST(Bitwise, BW02_BitNotNaNVM) {
    auto v = vm_ok("~NaN");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), -1.0);
}

// ============================================================
// BW-03: ~Infinity = -1（ToInt32(Infinity) = 0）
// ============================================================

TEST(Bitwise, BW03_BitNotInfinityInterp) {
    auto v = interp_ok("~(1/0)");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), -1.0);
}

TEST(Bitwise, BW03_BitNotInfinityVM) {
    auto v = vm_ok("~(1/0)");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), -1.0);
}

// ============================================================
// BW-04: & (BitAnd) 基础
// ============================================================

TEST(Bitwise, BW04_BitAndInterp) {
    auto v = interp_ok("12 & 10");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 8.0);
}

TEST(Bitwise, BW04_BitAndVM) {
    auto v = vm_ok("12 & 10");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 8.0);
}

// ============================================================
// BW-05: | (BitOr) 基础
// ============================================================

TEST(Bitwise, BW05_BitOrInterp) {
    auto v = interp_ok("12 | 10");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 14.0);
}

TEST(Bitwise, BW05_BitOrVM) {
    auto v = vm_ok("12 | 10");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 14.0);
}

// ============================================================
// BW-06: ^ (BitXor) 基础
// ============================================================

TEST(Bitwise, BW06_BitXorInterp) {
    auto v = interp_ok("12 ^ 10");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 6.0);
}

TEST(Bitwise, BW06_BitXorVM) {
    auto v = vm_ok("12 ^ 10");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 6.0);
}

// ============================================================
// BW-07: << (Shl) 基础
// ============================================================

TEST(Bitwise, BW07_ShlInterp) {
    auto v = interp_ok("1 << 3");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 8.0);
}

TEST(Bitwise, BW07_ShlVM) {
    auto v = vm_ok("1 << 3");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 8.0);
}

// ============================================================
// BW-08: >> (Sar) 有符号右移
// ============================================================

TEST(Bitwise, BW08_SarInterp) {
    auto v = interp_ok("-8 >> 2");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), -2.0);
}

TEST(Bitwise, BW08_SarVM) {
    auto v = vm_ok("-8 >> 2");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), -2.0);
}

// ============================================================
// BW-09: >>> (Shr) 无符号右移
// ============================================================

TEST(Bitwise, BW09_ShrInterp) {
    auto v = interp_ok("-1 >>> 0");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 4294967295.0);
}

TEST(Bitwise, BW09_ShrVM) {
    auto v = vm_ok("-1 >>> 0");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 4294967295.0);
}

// ============================================================
// BW-10: >>> 右移非零位
// ============================================================

TEST(Bitwise, BW10_ShrNonZeroInterp) {
    auto v = interp_ok("-8 >>> 1");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2147483644.0);
}

TEST(Bitwise, BW10_ShrNonZeroVM) {
    auto v = vm_ok("-8 >>> 1");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2147483644.0);
}

// ============================================================
// BW-11: 浮点截断（ToInt32 先 trunc）
// ============================================================

TEST(Bitwise, BW11_FloatTruncInterp) {
    // ToInt32(1.7) = 1, ToInt32(2.9) = 2, 1 & 2 = 0
    auto v = interp_ok("1.7 & 2.9");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}

TEST(Bitwise, BW11_FloatTruncVM) {
    auto v = vm_ok("1.7 & 2.9");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}

// ============================================================
// BW-12: 1 << 31 = -2147483648（有符号溢出）
// ============================================================

TEST(Bitwise, BW12_ShlOverflowInterp) {
    auto v = interp_ok("1 << 31");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), -2147483648.0);
}

TEST(Bitwise, BW12_ShlOverflowVM) {
    auto v = vm_ok("1 << 31");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), -2147483648.0);
}

// ============================================================
// BW-13: 复合赋值 &=
// ============================================================

TEST(Bitwise, BW13_BitAndAssignInterp) {
    auto v = interp_ok("var x = 15; x &= 9; x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 9.0);
}

TEST(Bitwise, BW13_BitAndAssignVM) {
    auto v = vm_ok("var x = 15; x &= 9; x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 9.0);
}

// ============================================================
// BW-14: 复合赋值 |=
// ============================================================

TEST(Bitwise, BW14_BitOrAssignInterp) {
    auto v = interp_ok("var x = 5; x |= 10; x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 15.0);
}

TEST(Bitwise, BW14_BitOrAssignVM) {
    auto v = vm_ok("var x = 5; x |= 10; x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 15.0);
}

// ============================================================
// BW-15: 复合赋值 ^=
// ============================================================

TEST(Bitwise, BW15_BitXorAssignInterp) {
    auto v = interp_ok("var x = 0xFF; x ^= 0x0F; x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 240.0);
}

TEST(Bitwise, BW15_BitXorAssignVM) {
    auto v = vm_ok("var x = 0xFF; x ^= 0x0F; x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 240.0);
}

// ============================================================
// BW-16: 复合赋值 <<=
// ============================================================

TEST(Bitwise, BW16_ShlAssignInterp) {
    auto v = interp_ok("var x = 3; x <<= 2; x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 12.0);
}

TEST(Bitwise, BW16_ShlAssignVM) {
    auto v = vm_ok("var x = 3; x <<= 2; x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 12.0);
}

// ============================================================
// BW-17: 复合赋值 >>=
// ============================================================

TEST(Bitwise, BW17_SarAssignInterp) {
    auto v = interp_ok("var x = -16; x >>= 2; x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), -4.0);
}

TEST(Bitwise, BW17_SarAssignVM) {
    auto v = vm_ok("var x = -16; x >>= 2; x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), -4.0);
}

// ============================================================
// BW-18: 复合赋值 >>>=
// ============================================================

TEST(Bitwise, BW18_ShrAssignInterp) {
    auto v = interp_ok("var x = -1; x >>>= 1; x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2147483647.0);
}

TEST(Bitwise, BW18_ShrAssignVM) {
    auto v = vm_ok("var x = -1; x >>>= 1; x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2147483647.0);
}

// ============================================================
// BW-19: 优先级：& 高于 |（a | b & c = a | (b & c)）
// ============================================================

TEST(Bitwise, BW19_PrecedenceBitAndOverBitOrInterp) {
    // 6 | 5 & 3 = 6 | (5 & 3) = 6 | 1 = 7
    auto v = interp_ok("6 | 5 & 3");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 7.0);
}

TEST(Bitwise, BW19_PrecedenceBitAndOverBitOrVM) {
    auto v = vm_ok("6 | 5 & 3");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 7.0);
}

// ============================================================
// BW-20: 优先级：+ 高于 &（a + b & c = (a + b) & c）
// ============================================================

TEST(Bitwise, BW20_PrecedenceAddOverBitAndInterp) {
    // 1 + 2 & 3 = (1 + 2) & 3 = 3 & 3 = 3
    auto v = interp_ok("1 + 2 & 3");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 3.0);
}

TEST(Bitwise, BW20_PrecedenceAddOverBitAndVM) {
    auto v = vm_ok("1 + 2 & 3");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 3.0);
}

// ============================================================
// BW-21: 优先级：^ 高于 |
// ============================================================

TEST(Bitwise, BW21_PrecedenceBitXorOverBitOrInterp) {
    // 5 ^ 3 | 2 = (5 ^ 3) | 2 = 6 | 2 = 6
    auto v = interp_ok("5 ^ 3 | 2");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 6.0);
}

TEST(Bitwise, BW21_PrecedenceBitXorOverBitOrVM) {
    auto v = vm_ok("5 ^ 3 | 2");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 6.0);
}

// ============================================================
// BW-22: 优先级：<< 高于 +（a << b + c = a << (b + c)）
// Wait: + has lbp=15, << has lbp=14 → + is higher → a + b << c = (a + b) << c
// ============================================================

TEST(Bitwise, BW22_PrecedenceAddOverShlInterp) {
    // 1 + 1 << 2 = (1 + 1) << 2 = 2 << 2 = 8
    auto v = interp_ok("1 + 1 << 2");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 8.0);
}

TEST(Bitwise, BW22_PrecedenceAddOverShlVM) {
    auto v = vm_ok("1 + 1 << 2");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 8.0);
}

// ============================================================
// BW-23: | 低于 &&（a | b && c = (a | b) && c）
// ============================================================

TEST(Bitwise, BW23_PrecedenceBitOrOverAndInterp) {
    // 3 | 4 && 5 = (3 | 4) && 5 = 7 && 5 = 5
    auto v = interp_ok("3 | 4 && 5");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 5.0);
}

TEST(Bitwise, BW23_PrecedenceBitOrOverAndVM) {
    auto v = vm_ok("3 | 4 && 5");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 5.0);
}

// ============================================================
// BW-24: ~ 对负数（~(-1) = 0）
// ============================================================

TEST(Bitwise, BW24_BitNotNegInterp) {
    auto v = interp_ok("~(-1)");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}

TEST(Bitwise, BW24_BitNotNegVM) {
    auto v = vm_ok("~(-1)");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}

// ============================================================
// BW-25: >> 保持符号位
// ============================================================

TEST(Bitwise, BW25_SarSignExtendInterp) {
    // -2147483648 >> 1 = -1073741824
    auto v = interp_ok("-2147483648 >> 1");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), -1073741824.0);
}

TEST(Bitwise, BW25_SarSignExtendVM) {
    auto v = vm_ok("-2147483648 >> 1");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), -1073741824.0);
}

// ============================================================
// BW-26: >>> 不保持符号位（-2147483648 >>> 1 = 1073741824）
// ============================================================

TEST(Bitwise, BW26_ShrNoSignExtendInterp) {
    auto v = interp_ok("-2147483648 >>> 1");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1073741824.0);
}

TEST(Bitwise, BW26_ShrNoSignExtendVM) {
    auto v = vm_ok("-2147483648 >>> 1");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1073741824.0);
}

// ============================================================
// BW-27: 复合赋值返回新值
// ============================================================

TEST(Bitwise, BW27_AssignReturnsNewValInterp) {
    // 赋值表达式本身的值是运算后的新值
    auto v = interp_ok("var x = 15; var y = (x &= 6); y");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 6.0);
}

TEST(Bitwise, BW27_AssignReturnsNewValVM) {
    auto v = vm_ok("var x = 15; var y = (x &= 6); y");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 6.0);
}

// ============================================================
// BW-28: 移位量取 mod 32（>> 32 = >> 0，无移位）
// ============================================================

TEST(Bitwise, BW28_ShiftMod32Interp) {
    // 8 >> 32 = 8 >> (32 & 31) = 8 >> 0 = 8
    auto v = interp_ok("8 >> 32");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 8.0);
}

TEST(Bitwise, BW28_ShiftMod32VM) {
    auto v = vm_ok("8 >> 32");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 8.0);
}

// ============================================================
// BW-29: 大浮点数的 ToInt32 换算（2^32 + 1 → ToInt32 = 1）
// ============================================================

TEST(Bitwise, BW29_ToInt32LargeFloatInterp) {
    auto v = interp_ok("(4294967297) | 0");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

TEST(Bitwise, BW29_ToInt32LargeFloatVM) {
    auto v = vm_ok("(4294967297) | 0");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

// ============================================================
// BW-30: 链式位运算
// ============================================================

TEST(Bitwise, BW30_ChainBitwiseInterp) {
    // 0xFF & 0x0F | 0xF0 = (0xFF & 0x0F) | 0xF0 = 0x0F | 0xF0 = 0xFF = 255
    auto v = interp_ok("0xFF & 0x0F | 0xF0");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 255.0);
}

TEST(Bitwise, BW30_ChainBitwiseVM) {
    auto v = vm_ok("0xFF & 0x0F | 0xF0");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 255.0);
}

// ============================================================
// BW-31: ~(-0) = -1（ToInt32(-0) = 0，~0 = -1）
// ============================================================

TEST(Bitwise, BW31_BitNotNegZeroInterp) {
    auto v = interp_ok("~(-0)");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), -1.0);
}

TEST(Bitwise, BW31_BitNotNegZeroVM) {
    auto v = vm_ok("~(-0)");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), -1.0);
}

// ============================================================
// BW-32: NaN 作为二元操作数（ToInt32(NaN) = 0）
// NaN & 1 = 0,  NaN | 1 = 1,  NaN ^ 1 = 1
// ============================================================

TEST(Bitwise, BW32_NanBinaryOperandInterp) {
    EXPECT_EQ(interp_ok("NaN & 1").as_number(), 0.0);
    EXPECT_EQ(interp_ok("NaN | 1").as_number(), 1.0);
    EXPECT_EQ(interp_ok("NaN ^ 1").as_number(), 1.0);
}

TEST(Bitwise, BW32_NanBinaryOperandVM) {
    EXPECT_EQ(vm_ok("NaN & 1").as_number(), 0.0);
    EXPECT_EQ(vm_ok("NaN | 1").as_number(), 1.0);
    EXPECT_EQ(vm_ok("NaN ^ 1").as_number(), 1.0);
}

// ============================================================
// BW-33: null 作为操作数（ToInt32(null) = 0）
// null | 5 = 5,  null & 7 = 0
// ============================================================

TEST(Bitwise, BW33_NullOperandInterp) {
    EXPECT_EQ(interp_ok("null | 5").as_number(), 5.0);
    EXPECT_EQ(interp_ok("null & 7").as_number(), 0.0);
}

TEST(Bitwise, BW33_NullOperandVM) {
    EXPECT_EQ(vm_ok("null | 5").as_number(), 5.0);
    EXPECT_EQ(vm_ok("null & 7").as_number(), 0.0);
}

// ============================================================
// BW-34: undefined 作为操作数（ToInt32(undefined) = ToInt32(NaN) = 0）
// undefined ^ 3 = 3,  undefined & 7 = 0
// ============================================================

TEST(Bitwise, BW34_UndefinedOperandInterp) {
    EXPECT_EQ(interp_ok("undefined ^ 3").as_number(), 3.0);
    EXPECT_EQ(interp_ok("undefined & 7").as_number(), 0.0);
}

TEST(Bitwise, BW34_UndefinedOperandVM) {
    EXPECT_EQ(vm_ok("undefined ^ 3").as_number(), 3.0);
    EXPECT_EQ(vm_ok("undefined & 7").as_number(), 0.0);
}

// ============================================================
// BW-35: boolean 作为操作数（ToInt32(true)=1, ToInt32(false)=0）
// true | 2 = 3,  false & 7 = 0,  true ^ true = 0
// ============================================================

TEST(Bitwise, BW35_BooleanOperandInterp) {
    EXPECT_EQ(interp_ok("true | 2").as_number(), 3.0);
    EXPECT_EQ(interp_ok("false & 7").as_number(), 0.0);
    EXPECT_EQ(interp_ok("true ^ true").as_number(), 0.0);
}

TEST(Bitwise, BW35_BooleanOperandVM) {
    EXPECT_EQ(vm_ok("true | 2").as_number(), 3.0);
    EXPECT_EQ(vm_ok("false & 7").as_number(), 0.0);
    EXPECT_EQ(vm_ok("true ^ true").as_number(), 0.0);
}

// ============================================================
// BW-36: 移位量为 0（恒等操作）
// 7 << 0 = 7,  -3 >> 0 = -3,  -3 >>> 0 = 4294967293
// ============================================================

TEST(Bitwise, BW36_ShiftByZeroInterp) {
    EXPECT_EQ(interp_ok("7 << 0").as_number(), 7.0);
    EXPECT_EQ(interp_ok("-3 >> 0").as_number(), -3.0);
    // ToUint32(-3) = 4294967293，>>> 0 保持不变
    EXPECT_EQ(interp_ok("-3 >>> 0").as_number(), 4294967293.0);
}

TEST(Bitwise, BW36_ShiftByZeroVM) {
    EXPECT_EQ(vm_ok("7 << 0").as_number(), 7.0);
    EXPECT_EQ(vm_ok("-3 >> 0").as_number(), -3.0);
    EXPECT_EQ(vm_ok("-3 >>> 0").as_number(), 4294967293.0);
}

// ============================================================
// BW-37: 负数移位量（ToUint32(-1) & 0x1F = 31）
// 1 << -1 = 1 << 31 = -2147483648
// ============================================================

TEST(Bitwise, BW37_NegShiftAmountInterp) {
    auto v = interp_ok("1 << -1");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), -2147483648.0);
}

TEST(Bitwise, BW37_NegShiftAmountVM) {
    auto v = vm_ok("1 << -1");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), -2147483648.0);
}

// ============================================================
// BW-38: NaN 作为移位量（ToUint32(NaN) = 0，shift & 0x1F = 0）
// 5 >> NaN = 5,  -8 >>> NaN = 4294967288
// ============================================================

TEST(Bitwise, BW38_NanShiftAmountInterp) {
    EXPECT_EQ(interp_ok("5 >> NaN").as_number(), 5.0);
    // ToUint32(-8) = 4294967288，>>> 0 = 4294967288
    EXPECT_EQ(interp_ok("-8 >>> NaN").as_number(), 4294967288.0);
}

TEST(Bitwise, BW38_NanShiftAmountVM) {
    EXPECT_EQ(vm_ok("5 >> NaN").as_number(), 5.0);
    EXPECT_EQ(vm_ok("-8 >>> NaN").as_number(), 4294967288.0);
}

// ============================================================
// BW-39: 双重 ~~ 恒等（ToInt32 截断效果）
// ~~5 = 5,  ~~NaN = 0,  ~~1.9 = 1,  ~~(-3.5) = -3
// ============================================================

TEST(Bitwise, BW39_DoubleNotIdentityInterp) {
    EXPECT_EQ(interp_ok("~~5").as_number(), 5.0);
    EXPECT_EQ(interp_ok("~~NaN").as_number(), 0.0);
    EXPECT_EQ(interp_ok("~~1.9").as_number(), 1.0);
    EXPECT_EQ(interp_ok("~~(-3.5)").as_number(), -3.0);
}

TEST(Bitwise, BW39_DoubleNotIdentityVM) {
    EXPECT_EQ(vm_ok("~~5").as_number(), 5.0);
    EXPECT_EQ(vm_ok("~~NaN").as_number(), 0.0);
    EXPECT_EQ(vm_ok("~~1.9").as_number(), 1.0);
    EXPECT_EQ(vm_ok("~~(-3.5)").as_number(), -3.0);
}

// ============================================================
// BW-40: XOR 幂等性（x ^ x = 0，x ^ 0 = x）
// ============================================================

TEST(Bitwise, BW40_XorIdentityInterp) {
    EXPECT_EQ(interp_ok("7 ^ 7").as_number(), 0.0);
    EXPECT_EQ(interp_ok("7 ^ 0").as_number(), 7.0);
    EXPECT_EQ(interp_ok("-1 ^ -1").as_number(), 0.0);
}

TEST(Bitwise, BW40_XorIdentityVM) {
    EXPECT_EQ(vm_ok("7 ^ 7").as_number(), 0.0);
    EXPECT_EQ(vm_ok("7 ^ 0").as_number(), 7.0);
    EXPECT_EQ(vm_ok("-1 ^ -1").as_number(), 0.0);
}

// ============================================================
// BW-41: -1 作为 AND 掩码（全 1 位，-1 & x = x）
// ============================================================

TEST(Bitwise, BW41_AllOnesMaskInterp) {
    EXPECT_EQ(interp_ok("-1 & 255").as_number(), 255.0);
    EXPECT_EQ(interp_ok("-1 & 0").as_number(), 0.0);
    EXPECT_EQ(interp_ok("-1 & -1").as_number(), -1.0);
}

TEST(Bitwise, BW41_AllOnesMaskVM) {
    EXPECT_EQ(vm_ok("-1 & 255").as_number(), 255.0);
    EXPECT_EQ(vm_ok("-1 & 0").as_number(), 0.0);
    EXPECT_EQ(vm_ok("-1 & -1").as_number(), -1.0);
}

// ============================================================
// BW-42: 0 作为 OR 操作数（0 | x = x）
// ============================================================

TEST(Bitwise, BW42_ZeroOrIdentityInterp) {
    EXPECT_EQ(interp_ok("0 | 255").as_number(), 255.0);
    EXPECT_EQ(interp_ok("0 | -1").as_number(), -1.0);
    EXPECT_EQ(interp_ok("0 | 0").as_number(), 0.0);
}

TEST(Bitwise, BW42_ZeroOrIdentityVM) {
    EXPECT_EQ(vm_ok("0 | 255").as_number(), 255.0);
    EXPECT_EQ(vm_ok("0 | -1").as_number(), -1.0);
    EXPECT_EQ(vm_ok("0 | 0").as_number(), 0.0);
}

// ============================================================
// BW-43: ToInt32(2^32) = 0（环绕至 0）
// 4294967296 | 0 = 0
// ============================================================

TEST(Bitwise, BW43_ToInt32TwoTo32Interp) {
    auto v = interp_ok("4294967296 | 0");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}

TEST(Bitwise, BW43_ToInt32TwoTo32VM) {
    auto v = vm_ok("4294967296 | 0");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}

// ============================================================
// BW-44: ToInt32(2^31) = -2147483648（最高位置 1 变负数）
// 2147483648 | 0 = -2147483648
// ============================================================

TEST(Bitwise, BW44_ToInt32TwoTo31Interp) {
    auto v = interp_ok("2147483648 | 0");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), -2147483648.0);
}

TEST(Bitwise, BW44_ToInt32TwoTo31VM) {
    auto v = vm_ok("2147483648 | 0");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), -2147483648.0);
}

// ============================================================
// BW-45: 左结合性验证
// 8 >> 1 >> 1 = (8>>1)>>1 = 4>>1 = 2
// 1 ^ 2 ^ 3   = (1^2)^3   = 3^3  = 0
// ============================================================

TEST(Bitwise, BW45_LeftAssociativityInterp) {
    EXPECT_EQ(interp_ok("8 >> 1 >> 1").as_number(), 2.0);
    EXPECT_EQ(interp_ok("1 ^ 2 ^ 3").as_number(), 0.0);
    // 16 >>> 1 >>> 1 = (16>>>1)>>>1 = 8>>>1 = 4
    EXPECT_EQ(interp_ok("16 >>> 1 >>> 1").as_number(), 4.0);
}

TEST(Bitwise, BW45_LeftAssociativityVM) {
    EXPECT_EQ(vm_ok("8 >> 1 >> 1").as_number(), 2.0);
    EXPECT_EQ(vm_ok("1 ^ 2 ^ 3").as_number(), 0.0);
    EXPECT_EQ(vm_ok("16 >>> 1 >>> 1").as_number(), 4.0);
}

// ============================================================
// BW-46: 字符串操作数数值转换（ToInt32("4") = 4）
// "4" | 2 = 6,  "3" & 7 = 3,  "15" ^ 5 = 10
// ============================================================

TEST(Bitwise, BW46_StringCoercionInterp) {
    EXPECT_EQ(interp_ok("\"4\" | 2").as_number(), 6.0);
    EXPECT_EQ(interp_ok("\"3\" & 7").as_number(), 3.0);
    EXPECT_EQ(interp_ok("\"15\" ^ 5").as_number(), 10.0);
}

TEST(Bitwise, BW46_StringCoercionVM) {
    EXPECT_EQ(vm_ok("\"4\" | 2").as_number(), 6.0);
    EXPECT_EQ(vm_ok("\"3\" & 7").as_number(), 3.0);
    EXPECT_EQ(vm_ok("\"15\" ^ 5").as_number(), 10.0);
}

// ============================================================
// BW-47: 复合赋值 RHS 为表达式
// x <<= 1+1 即 x = x << 2；x >>= 1+1 即 x = x >> 2
// ============================================================

TEST(Bitwise, BW47_CompoundAssignExprRhsInterp) {
    EXPECT_EQ(interp_ok("var x = 3; x <<= 1+1; x").as_number(), 12.0);
    EXPECT_EQ(interp_ok("var x = 16; x >>= 1+1; x").as_number(), 4.0);
    EXPECT_EQ(interp_ok("var x = -1; x >>>= 1+1; x").as_number(), 1073741823.0);
}

TEST(Bitwise, BW47_CompoundAssignExprRhsVM) {
    EXPECT_EQ(vm_ok("var x = 3; x <<= 1+1; x").as_number(), 12.0);
    EXPECT_EQ(vm_ok("var x = 16; x >>= 1+1; x").as_number(), 4.0);
    EXPECT_EQ(vm_ok("var x = -1; x >>>= 1+1; x").as_number(), 1073741823.0);
}

// ============================================================
// BW-48: -Infinity 作为操作数（ToInt32(-Infinity) = 0）
// -Infinity | 0 = 0,  -Infinity & 1 = 0
// ============================================================

TEST(Bitwise, BW48_NegInfinityOperandInterp) {
    EXPECT_EQ(interp_ok("(-1/0) | 0").as_number(), 0.0);
    EXPECT_EQ(interp_ok("(-1/0) & 1").as_number(), 0.0);
    EXPECT_EQ(interp_ok("(-1/0) ^ 3").as_number(), 3.0);
}

TEST(Bitwise, BW48_NegInfinityOperandVM) {
    EXPECT_EQ(vm_ok("(-1/0) | 0").as_number(), 0.0);
    EXPECT_EQ(vm_ok("(-1/0) & 1").as_number(), 0.0);
    EXPECT_EQ(vm_ok("(-1/0) ^ 3").as_number(), 3.0);
}

// ============================================================
// BW-49: 大负数 ToInt32（环绕到 -1）
// -4294967297 | 0 = -1（fmod(-4294967297, 2^32) + 2^32 → uint32=0xFFFFFFFF → int32=-1）
// ============================================================

TEST(Bitwise, BW49_ToInt32LargeNegInterp) {
    auto v = interp_ok("-4294967297 | 0");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), -1.0);
}

TEST(Bitwise, BW49_ToInt32LargeNegVM) {
    auto v = vm_ok("-4294967297 | 0");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), -1.0);
}

// ============================================================
// BW-50: 多步复合赋值顺序正确
// var x = 255; x &= 0xF0; x >>= 4 → 240 >> 4 = 15
// ============================================================

TEST(Bitwise, BW50_MultiStepCompoundAssignInterp) {
    auto v = interp_ok("var x = 255; x &= 0xF0; x >>= 4; x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 15.0);
}

TEST(Bitwise, BW50_MultiStepCompoundAssignVM) {
    auto v = vm_ok("var x = 255; x &= 0xF0; x >>= 4; x");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 15.0);
}

}  // namespace
