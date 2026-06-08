// Tests for do-while, optional catch binding, numeric separators
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
    if (!parse_result.ok()) return Value::undefined();
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

// ---- do-while (Interpreter) ----

TEST(DoWhileInterp, DW01_BasicLoop) {
    auto v = interp_ok("var i=0; do { i++; } while(i<5); i");
    EXPECT_DOUBLE_EQ(v.as_number(), 5.0);
}

TEST(DoWhileInterp, DW02_RunsAtLeastOnce) {
    auto v = interp_ok("var x=0; do { x=1; } while(false); x");
    EXPECT_DOUBLE_EQ(v.as_number(), 1.0);
}

TEST(DoWhileInterp, DW03_Break) {
    auto v = interp_ok("var i=0; do { i++; if(i===3) break; } while(i<10); i");
    EXPECT_DOUBLE_EQ(v.as_number(), 3.0);
}

TEST(DoWhileInterp, DW04_Continue) {
    auto v = interp_ok(R"(
var sum=0, i=0;
do {
  i++;
  if(i%2===0) continue;
  sum += i;
} while(i<6);
sum
)");
    EXPECT_DOUBLE_EQ(v.as_number(), 9.0);  // 1+3+5
}

TEST(DoWhileInterp, DW05_SingleStmt) {
    auto v = interp_ok("var i=0; do i++; while(i<3); i");
    EXPECT_DOUBLE_EQ(v.as_number(), 3.0);
}

TEST(DoWhileInterp, DW06_Nested) {
    auto v = interp_ok(R"(
var i=0, j=0, sum=0;
do {
  i++;
  j=0;
  do {
    j++;
    sum++;
  } while(j<i);
} while(i<3);
sum
)");
    EXPECT_DOUBLE_EQ(v.as_number(), 6.0);  // 1+2+3
}

TEST(DoWhileInterp, DW07_LabeledBreak) {
    auto v = interp_ok(R"(
var i=0;
outer: do {
  i++;
  do {
    if(i>=2) break outer;
    i++;
  } while(i<5);
} while(i<5);
i
)");
    EXPECT_DOUBLE_EQ(v.as_number(), 2.0);
}

TEST(DoWhileInterp, DW08_WithTryCatch) {
    auto v = interp_ok(R"(
var i=0, caught=false;
do {
  try {
    if(i===2) throw 42;
    i++;
  } catch(e) {
    caught=true;
    i++;
  }
} while(i<3);
caught && i===3
)");
    EXPECT_TRUE(v.as_bool());
}

// ---- do-while (VM) ----

TEST(DoWhileVM, DW01_BasicLoop) {
    auto v = vm_ok("var i=0; do { i++; } while(i<5); i");
    EXPECT_DOUBLE_EQ(v.as_number(), 5.0);
}

TEST(DoWhileVM, DW02_RunsAtLeastOnce) {
    auto v = vm_ok("var x=0; do { x=1; } while(false); x");
    EXPECT_DOUBLE_EQ(v.as_number(), 1.0);
}

TEST(DoWhileVM, DW03_Break) {
    auto v = vm_ok("var i=0; do { i++; if(i===3) break; } while(i<10); i");
    EXPECT_DOUBLE_EQ(v.as_number(), 3.0);
}

TEST(DoWhileVM, DW04_Continue) {
    auto v = vm_ok(R"(
var sum=0, i=0;
do {
  i++;
  if(i%2===0) continue;
  sum += i;
} while(i<6);
sum
)");
    EXPECT_DOUBLE_EQ(v.as_number(), 9.0);  // 1+3+5
}

TEST(DoWhileVM, DW05_SingleStmt) {
    auto v = vm_ok("var i=0; do i++; while(i<3); i");
    EXPECT_DOUBLE_EQ(v.as_number(), 3.0);
}

TEST(DoWhileVM, DW06_Nested) {
    auto v = vm_ok(R"(
var i=0, j=0, sum=0;
do {
  i++;
  j=0;
  do {
    j++;
    sum++;
  } while(j<i);
} while(i<3);
sum
)");
    EXPECT_DOUBLE_EQ(v.as_number(), 6.0);
}

TEST(DoWhileVM, DW07_LabeledBreak) {
    auto v = vm_ok(R"(
var i=0;
outer: do {
  i++;
  do {
    if(i>=2) break outer;
    i++;
  } while(i<5);
} while(i<5);
i
)");
    EXPECT_DOUBLE_EQ(v.as_number(), 2.0);
}

TEST(DoWhileVM, DW08_WithTryCatch) {
    auto v = vm_ok(R"(
var i=0, caught=false;
do {
  try {
    if(i===2) throw 42;
    i++;
  } catch(e) {
    caught=true;
    i++;
  }
} while(i<3);
caught && i===3
)");
    EXPECT_TRUE(v.as_bool());
}

// ---- Optional Catch Binding (Interpreter) ----

TEST(OptCatchInterp, OCB01_BasicNoCatch) {
    auto v = interp_ok("var x=0; try { throw 42; } catch { x=1; } x");
    EXPECT_DOUBLE_EQ(v.as_number(), 1.0);
}

TEST(OptCatchInterp, OCB02_NoThrow) {
    auto v = interp_ok("var x=0; try { x=1; } catch { x=99; } x");
    EXPECT_DOUBLE_EQ(v.as_number(), 1.0);
}

TEST(OptCatchInterp, OCB03_WithFinally) {
    auto v = interp_ok(R"(
var x=0;
try { throw "err"; } catch { x=1; } finally { x+=10; }
x
)");
    EXPECT_DOUBLE_EQ(v.as_number(), 11.0);
}

TEST(OptCatchInterp, OCB04_NullError) {
    auto v = interp_ok("var ok=false; try { null.x; } catch { ok=true; } ok");
    EXPECT_TRUE(v.as_bool());
}

// ---- Optional Catch Binding (VM) ----

TEST(OptCatchVM, OCB01_BasicNoCatch) {
    auto v = vm_ok("var x=0; try { throw 42; } catch { x=1; } x");
    EXPECT_DOUBLE_EQ(v.as_number(), 1.0);
}

TEST(OptCatchVM, OCB02_NoThrow) {
    auto v = vm_ok("var x=0; try { x=1; } catch { x=99; } x");
    EXPECT_DOUBLE_EQ(v.as_number(), 1.0);
}

TEST(OptCatchVM, OCB03_WithFinally) {
    auto v = vm_ok(R"(
var x=0;
try { throw "err"; } catch { x=1; } finally { x+=10; }
x
)");
    EXPECT_DOUBLE_EQ(v.as_number(), 11.0);
}

TEST(OptCatchVM, OCB04_NullError) {
    auto v = vm_ok("var ok=false; try { null.x; } catch { ok=true; } ok");
    EXPECT_TRUE(v.as_bool());
}

// ---- Numeric Separators (Interpreter) ----

TEST(NumSepInterp, NS01_Decimal) {
    auto v = interp_ok("1000000");  // no separator - baseline
    EXPECT_DOUBLE_EQ(v.as_number(), 1000000.0);
    auto v2 = interp_ok("1_000_000");
    EXPECT_DOUBLE_EQ(v2.as_number(), 1000000.0);
}

TEST(NumSepInterp, NS02_Hex) {
    auto v = interp_ok("0xFFFF");
    EXPECT_DOUBLE_EQ(v.as_number(), 65535.0);
    auto v2 = interp_ok("0xFF_FF");
    EXPECT_DOUBLE_EQ(v2.as_number(), 65535.0);
}

TEST(NumSepInterp, NS03_Binary) {
    auto v = interp_ok("0b10101010");
    EXPECT_DOUBLE_EQ(v.as_number(), 170.0);
    auto v2 = interp_ok("0b1010_1010");
    EXPECT_DOUBLE_EQ(v2.as_number(), 170.0);
}

TEST(NumSepInterp, NS04_Octal) {
    auto v = interp_ok("0o77");
    EXPECT_DOUBLE_EQ(v.as_number(), 63.0);
    auto v2 = interp_ok("0o7_7");
    EXPECT_DOUBLE_EQ(v2.as_number(), 63.0);
}

TEST(NumSepInterp, NS05_BigDecimal) {
    auto v = interp_ok("1_000_000_000");
    EXPECT_DOUBLE_EQ(v.as_number(), 1000000000.0);
}

TEST(NumSepInterp, NS06_SingleSep) {
    auto v = interp_ok("1_0");
    EXPECT_DOUBLE_EQ(v.as_number(), 10.0);
}

// ---- Numeric Separators (VM) ----

TEST(NumSepVM, NS01_Decimal) {
    auto v = vm_ok("1_000_000");
    EXPECT_DOUBLE_EQ(v.as_number(), 1000000.0);
}

TEST(NumSepVM, NS02_Hex) {
    auto v = vm_ok("0xFF_FF");
    EXPECT_DOUBLE_EQ(v.as_number(), 65535.0);
}

TEST(NumSepVM, NS03_Binary) {
    auto v = vm_ok("0b1010_1010");
    EXPECT_DOUBLE_EQ(v.as_number(), 170.0);
}

TEST(NumSepVM, NS04_Octal) {
    auto v = vm_ok("0o7_7");
    EXPECT_DOUBLE_EQ(v.as_number(), 63.0);
}

TEST(NumSepVM, NS05_BigDecimal) {
    auto v = vm_ok("1_000_000_000");
    EXPECT_DOUBLE_EQ(v.as_number(), 1000000000.0);
}

TEST(NumSepVM, NS06_SingleSep) {
    auto v = vm_ok("1_0");
    EXPECT_DOUBLE_EQ(v.as_number(), 10.0);
}

}  // namespace
