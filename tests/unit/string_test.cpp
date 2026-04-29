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

bool interp_err(std::string_view source) {
    auto parse_result = qppjs::parse_program(source);
    if (!parse_result.ok()) return true;
    qppjs::Interpreter interp;
    auto result = interp.exec(parse_result.value());
    return !result.is_ok();
}

bool vm_err(std::string_view source) {
    auto parse_result = qppjs::parse_program(source);
    if (!parse_result.ok()) return true;
    qppjs::Compiler compiler;
    auto bytecode = compiler.compile(parse_result.value());
    qppjs::VM vm;
    auto result = vm.exec(bytecode);
    return !result.is_ok();
}

// ============================================================
// Interpreter: length
// ============================================================

// S-01: "abc".length === 3
TEST(InterpString, LengthAscii) {
    auto v = interp_ok(R"("abc".length)");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 3.0);
}

// S-02: "".length === 0
TEST(InterpString, LengthEmpty) {
    auto v = interp_ok(R"("".length)");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}

// ============================================================
// Interpreter: indexOf
// ============================================================

// S-03: basic found
TEST(InterpString, IndexOfFound) {
    auto v = interp_ok(R"("hello".indexOf("ll"))");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.0);
}

// S-04: not found returns -1
TEST(InterpString, IndexOfNotFound) {
    auto v = interp_ok(R"("hello".indexOf("xyz"))");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), -1.0);
}

// S-05: fromIndex positive
TEST(InterpString, IndexOfFromIndex) {
    auto v = interp_ok(R"("abcabc".indexOf("b", 3))");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 4.0);
}

// S-06: fromIndex negative -> treated as 0
TEST(InterpString, IndexOfFromIndexNegative) {
    auto v = interp_ok(R"("hello".indexOf("h", -5))");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}

// S-07: empty search string returns min(fromIndex, len)
TEST(InterpString, IndexOfEmptySearch) {
    auto v = interp_ok(R"("abc".indexOf("", 10))");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 3.0);
}

// S-08: NaN fromIndex treated as 0
TEST(InterpString, IndexOfNaNFromIndex) {
    auto v = interp_ok(R"("hello".indexOf("h", 0/0))");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}

// ============================================================
// Interpreter: lastIndexOf
// ============================================================

// S-09: basic lastIndexOf found
TEST(InterpString, LastIndexOfFound) {
    auto v = interp_ok(R"("abcabc".lastIndexOf("b"))");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 4.0);
}

// S-10: lastIndexOf not found
TEST(InterpString, LastIndexOfNotFound) {
    auto v = interp_ok(R"("hello".lastIndexOf("xyz"))");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), -1.0);
}

// S-11: NaN fromIndex -> search whole string
TEST(InterpString, LastIndexOfNaNFromIndex) {
    auto v = interp_ok(R"("abcabc".lastIndexOf("b", 0/0))");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 4.0);
}

// S-12: fromIndex negative -> max(fromIndex,0)=0, search from 0
TEST(InterpString, LastIndexOfFromIndexNegative) {
    auto v = interp_ok(R"("hello".lastIndexOf("h", -1))");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}

// S-13: fromIndex beyond length -> search whole string
TEST(InterpString, LastIndexOfFromIndexBeyondLen) {
    auto v = interp_ok(R"("abcabc".lastIndexOf("b", 100))");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 4.0);
}

// ============================================================
// Interpreter: slice
// ============================================================

// S-14: basic slice
TEST(InterpString, SliceBasic) {
    auto v = interp_ok(R"("hello".slice(1, 3))");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "el");
}

// S-15: negative start
TEST(InterpString, SliceNegativeStart) {
    auto v = interp_ok(R"("hello".slice(-3))");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "llo");
}

// S-16: negative end
TEST(InterpString, SliceNegativeEnd) {
    auto v = interp_ok(R"("hello".slice(1, -1))");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "ell");
}

// S-17: start >= end returns ""
TEST(InterpString, SliceStartGteEnd) {
    auto v = interp_ok(R"("hello".slice(3, 1))");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "");
}

// S-18: NaN start treated as 0
TEST(InterpString, SliceNaNStart) {
    auto v = interp_ok(R"("hello".slice(0/0, 3))");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "hel");
}

// S-19: omit end -> to end of string
TEST(InterpString, SliceOmitEnd) {
    auto v = interp_ok(R"("hello".slice(2))");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "llo");
}

// ============================================================
// Interpreter: substring
// ============================================================

// S-20: basic substring
TEST(InterpString, SubstringBasic) {
    auto v = interp_ok(R"("hello".substring(1, 3))");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "el");
}

// S-21: start > end -> swap
TEST(InterpString, SubstringSwap) {
    auto v = interp_ok(R"("hello".substring(3, 1))");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "el");
}

// S-22: negative -> 0
TEST(InterpString, SubstringNegative) {
    auto v = interp_ok(R"("hello".substring(-1, 3))");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "hel");
}

// S-23: NaN -> 0
TEST(InterpString, SubstringNaN) {
    auto v = interp_ok(R"("hello".substring(0/0, 3))");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "hel");
}

// S-24: omit end -> to end
TEST(InterpString, SubstringOmitEnd) {
    auto v = interp_ok(R"("hello".substring(2))");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "llo");
}

// ============================================================
// Interpreter: split
// ============================================================

// S-25: undefined separator -> [str]
TEST(InterpString, SplitUndefinedSep) {
    auto v = interp_ok(R"(var r = "abc".split(); r.length)");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

// S-26: empty separator -> split by codepoint
TEST(InterpString, SplitEmptySep) {
    auto v = interp_ok(R"(var r = "abc".split(""); r.length)");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 3.0);
}

// S-27: normal separator
TEST(InterpString, SplitNormalSep) {
    auto v = interp_ok(R"(var r = "a,b,c".split(","); r.length)");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 3.0);
}

// S-28: limit=0 -> []
TEST(InterpString, SplitLimitZero) {
    auto v = interp_ok(R"(var r = "abc".split("", 0); r.length)");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}

// S-29: limit truncates result
TEST(InterpString, SplitLimitTruncate) {
    auto v = interp_ok(R"(var r = "a,b,c".split(",", 2); r.length)");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.0);
}

// S-30: separator not found -> [str]
TEST(InterpString, SplitSepNotFound) {
    auto v = interp_ok(R"(var r = "abc".split("x"); r.length)");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

// ============================================================
// Interpreter: trim / trimStart / trimEnd
// ============================================================

// S-31: trim both ends
TEST(InterpString, TrimBoth) {
    auto v = interp_ok(R"("  hello  ".trim())");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "hello");
}

// S-32: trimStart only left
TEST(InterpString, TrimStart) {
    auto v = interp_ok(R"("  hello  ".trimStart())");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "hello  ");
}

// S-33: trimEnd only right
TEST(InterpString, TrimEnd) {
    auto v = interp_ok(R"("  hello  ".trimEnd())");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "  hello");
}

// S-34: all whitespace -> ""
TEST(InterpString, TrimAllWhitespace) {
    auto v = interp_ok(R"("   ".trim())");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "");
}

// S-35: Unicode whitespace (NBSP U+00A0)
TEST(InterpString, TrimUnicodeWhitespace) {
    // "\xc2\xa0" is UTF-8 for U+00A0 (NO-BREAK SPACE)
    auto v = interp_ok("\"\xc2\xa0hello\xc2\xa0\".trim()");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "hello");
}

// S-36: no whitespace -> unchanged
TEST(InterpString, TrimNoWhitespace) {
    auto v = interp_ok(R"("hello".trim())");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "hello");
}

// ============================================================
// Interpreter: TypeError
// ============================================================

// S-37: null.indexOf throws TypeError
TEST(InterpString, TypeErrorNull) {
    EXPECT_TRUE(interp_err(R"(
        var s = null;
        s.indexOf("x");
    )"));
}

// S-38: undefined.trim throws TypeError
TEST(InterpString, TypeErrorUndefined) {
    EXPECT_TRUE(interp_err(R"(
        var s = undefined;
        s.trim();
    )"));
}

// ============================================================
// VM: length
// ============================================================

// S-01v: "abc".length === 3
TEST(VmString, LengthAscii) {
    auto v = vm_ok(R"("abc".length)");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 3.0);
}

// S-02v: "".length === 0
TEST(VmString, LengthEmpty) {
    auto v = vm_ok(R"("".length)");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}

// ============================================================
// VM: indexOf
// ============================================================

// S-03v: basic found
TEST(VmString, IndexOfFound) {
    auto v = vm_ok(R"("hello".indexOf("ll"))");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.0);
}

// S-04v: not found returns -1
TEST(VmString, IndexOfNotFound) {
    auto v = vm_ok(R"("hello".indexOf("xyz"))");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), -1.0);
}

// S-05v: fromIndex positive
TEST(VmString, IndexOfFromIndex) {
    auto v = vm_ok(R"("abcabc".indexOf("b", 3))");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 4.0);
}

// S-06v: fromIndex negative -> treated as 0
TEST(VmString, IndexOfFromIndexNegative) {
    auto v = vm_ok(R"("hello".indexOf("h", -5))");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}

// S-07v: empty search string returns min(fromIndex, len)
TEST(VmString, IndexOfEmptySearch) {
    auto v = vm_ok(R"("abc".indexOf("", 10))");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 3.0);
}

// S-08v: NaN fromIndex treated as 0
TEST(VmString, IndexOfNaNFromIndex) {
    auto v = vm_ok(R"("hello".indexOf("h", 0/0))");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}

// ============================================================
// VM: lastIndexOf
// ============================================================

// S-09v: basic lastIndexOf found
TEST(VmString, LastIndexOfFound) {
    auto v = vm_ok(R"("abcabc".lastIndexOf("b"))");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 4.0);
}

// S-10v: lastIndexOf not found
TEST(VmString, LastIndexOfNotFound) {
    auto v = vm_ok(R"("hello".lastIndexOf("xyz"))");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), -1.0);
}

// S-11v: NaN fromIndex -> search whole string
TEST(VmString, LastIndexOfNaNFromIndex) {
    auto v = vm_ok(R"("abcabc".lastIndexOf("b", 0/0))");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 4.0);
}

// S-12v: fromIndex negative -> max(fromIndex,0)=0, search from 0
TEST(VmString, LastIndexOfFromIndexNegative) {
    auto v = vm_ok(R"("hello".lastIndexOf("h", -1))");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}

// S-13v: fromIndex beyond length -> search whole string
TEST(VmString, LastIndexOfFromIndexBeyondLen) {
    auto v = vm_ok(R"("abcabc".lastIndexOf("b", 100))");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 4.0);
}

// ============================================================
// VM: slice
// ============================================================

// S-14v: basic slice
TEST(VmString, SliceBasic) {
    auto v = vm_ok(R"("hello".slice(1, 3))");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "el");
}

// S-15v: negative start
TEST(VmString, SliceNegativeStart) {
    auto v = vm_ok(R"("hello".slice(-3))");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "llo");
}

// S-16v: negative end
TEST(VmString, SliceNegativeEnd) {
    auto v = vm_ok(R"("hello".slice(1, -1))");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "ell");
}

// S-17v: start >= end returns ""
TEST(VmString, SliceStartGteEnd) {
    auto v = vm_ok(R"("hello".slice(3, 1))");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "");
}

// S-18v: NaN start treated as 0
TEST(VmString, SliceNaNStart) {
    auto v = vm_ok(R"("hello".slice(0/0, 3))");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "hel");
}

// S-19v: omit end -> to end of string
TEST(VmString, SliceOmitEnd) {
    auto v = vm_ok(R"("hello".slice(2))");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "llo");
}

// ============================================================
// VM: substring
// ============================================================

// S-20v: basic substring
TEST(VmString, SubstringBasic) {
    auto v = vm_ok(R"("hello".substring(1, 3))");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "el");
}

// S-21v: start > end -> swap
TEST(VmString, SubstringSwap) {
    auto v = vm_ok(R"("hello".substring(3, 1))");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "el");
}

// S-22v: negative -> 0
TEST(VmString, SubstringNegative) {
    auto v = vm_ok(R"("hello".substring(-1, 3))");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "hel");
}

// S-23v: NaN -> 0
TEST(VmString, SubstringNaN) {
    auto v = vm_ok(R"("hello".substring(0/0, 3))");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "hel");
}

// S-24v: omit end -> to end
TEST(VmString, SubstringOmitEnd) {
    auto v = vm_ok(R"("hello".substring(2))");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "llo");
}

// ============================================================
// VM: split
// ============================================================

// S-25v: undefined separator -> [str]
TEST(VmString, SplitUndefinedSep) {
    auto v = vm_ok(R"(var r = "abc".split(); r.length)");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

// S-26v: empty separator -> split by codepoint
TEST(VmString, SplitEmptySep) {
    auto v = vm_ok(R"(var r = "abc".split(""); r.length)");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 3.0);
}

// S-27v: normal separator
TEST(VmString, SplitNormalSep) {
    auto v = vm_ok(R"(var r = "a,b,c".split(","); r.length)");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 3.0);
}

// S-28v: limit=0 -> []
TEST(VmString, SplitLimitZero) {
    auto v = vm_ok(R"(var r = "abc".split("", 0); r.length)");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}

// S-29v: limit truncates result
TEST(VmString, SplitLimitTruncate) {
    auto v = vm_ok(R"(var r = "a,b,c".split(",", 2); r.length)");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.0);
}

// S-30v: separator not found -> [str]
TEST(VmString, SplitSepNotFound) {
    auto v = vm_ok(R"(var r = "abc".split("x"); r.length)");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

// ============================================================
// VM: trim / trimStart / trimEnd
// ============================================================

// S-31v: trim both ends
TEST(VmString, TrimBoth) {
    auto v = vm_ok(R"("  hello  ".trim())");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "hello");
}

// S-32v: trimStart only left
TEST(VmString, TrimStart) {
    auto v = vm_ok(R"("  hello  ".trimStart())");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "hello  ");
}

// S-33v: trimEnd only right
TEST(VmString, TrimEnd) {
    auto v = vm_ok(R"("  hello  ".trimEnd())");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "  hello");
}

// S-34v: all whitespace -> ""
TEST(VmString, TrimAllWhitespace) {
    auto v = vm_ok(R"("   ".trim())");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "");
}

// S-35v: Unicode whitespace (NBSP U+00A0)
TEST(VmString, TrimUnicodeWhitespace) {
    auto v = vm_ok("\"\xc2\xa0hello\xc2\xa0\".trim()");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "hello");
}

// S-36v: no whitespace -> unchanged
TEST(VmString, TrimNoWhitespace) {
    auto v = vm_ok(R"("hello".trim())");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "hello");
}

// ============================================================
// VM: TypeError
// ============================================================

// S-37v: null.indexOf throws TypeError
TEST(VmString, TypeErrorNull) {
    EXPECT_TRUE(vm_err(R"(
        var s = null;
        s.indexOf("x");
    )"));
}

// S-38v: undefined.trim throws TypeError
TEST(VmString, TypeErrorUndefined) {
    EXPECT_TRUE(vm_err(R"(
        var s = undefined;
        s.trim();
    )"));
}

// ============================================================
// Interpreter: indexOf additional boundaries (S-39 ~ S-43)
// ============================================================

// S-39: "abc".indexOf("", 0) === 0 (fromIndex=0 on empty search)
TEST(InterpString, IndexOfEmptyAtZero) {
    auto v = interp_ok(R"("abc".indexOf("", 0))");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}

// S-40: "abc".indexOf("", 3) === 3 (fromIndex=len on empty search)
TEST(InterpString, IndexOfEmptyAtLen) {
    auto v = interp_ok(R"("abc".indexOf("", 3))");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 3.0);
}

// S-41: "abc".indexOf("abc") === 0 (whole string match)
TEST(InterpString, IndexOfWholeString) {
    auto v = interp_ok(R"("abc".indexOf("abc"))");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}

// S-42: "".indexOf("") === 0 (both empty)
TEST(InterpString, IndexOfBothEmpty) {
    auto v = interp_ok(R"("".indexOf(""))");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}

// S-43: "abcabc".indexOf("a", 1) === 3 (skip first match via fromIndex)
TEST(InterpString, IndexOfSkipFirst) {
    auto v = interp_ok(R"("abcabc".indexOf("a", 1))");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 3.0);
}

// ============================================================
// Interpreter: lastIndexOf additional boundaries (S-44 ~ S-47)
// ============================================================

// S-44: "abcabc".lastIndexOf("a", 0) === 0 (fromIndex=0, only index 0 checked)
TEST(InterpString, LastIndexOfFromIndexZero) {
    auto v = interp_ok(R"("abcabc".lastIndexOf("a", 0))");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}

// S-45: "abcabc".lastIndexOf("a", 2) === 0 (fromIndex=2, last "a" at or before 2 is index 0)
TEST(InterpString, LastIndexOfFromIndexMid) {
    auto v = interp_ok(R"("abcabc".lastIndexOf("a", 2))");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}

// S-46: "abc".lastIndexOf("") === 3 (empty needle, no fromIndex -> search whole -> returns len)
TEST(InterpString, LastIndexOfEmptyNoFrom) {
    auto v = interp_ok(R"("abc".lastIndexOf(""))");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 3.0);
}

// S-47: "abc".lastIndexOf("", 1) === 1 (empty needle with fromIndex)
TEST(InterpString, LastIndexOfEmptyWithFrom) {
    auto v = interp_ok(R"("abc".lastIndexOf("", 1))");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

// ============================================================
// Interpreter: slice additional boundaries (S-48 ~ S-52)
// ============================================================

// S-48: "abc".slice() === "abc" (no arguments)
TEST(InterpString, SliceNoArgs) {
    auto v = interp_ok(R"("abc".slice())");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "abc");
}

// S-49: "abc".slice(0) === "abc" (start=0)
TEST(InterpString, SliceFromZero) {
    auto v = interp_ok(R"("abc".slice(0))");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "abc");
}

// S-50: "abc".slice(1, 1) === "" (start == end)
TEST(InterpString, SliceStartEqualsEnd) {
    auto v = interp_ok(R"("abc".slice(1, 1))");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "");
}

// S-51: "abc".slice(-100) === "abc" (negative start with abs > len -> clamped to 0)
TEST(InterpString, SliceNegativeStartOverflow) {
    auto v = interp_ok(R"("abc".slice(-100))");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "abc");
}

// S-52: "abc".slice(0, -100) === "" (negative end with abs > len -> clamped to 0, start>=end)
TEST(InterpString, SliceNegativeEndOverflow) {
    auto v = interp_ok(R"("abc".slice(0, -100))");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "");
}

// ============================================================
// Interpreter: substring additional boundaries (S-53 ~ S-55)
// ============================================================

// S-53: "abc".substring(0, 0) === "" (equal arguments)
TEST(InterpString, SubstringEqualArgs) {
    auto v = interp_ok(R"("abc".substring(0, 0))");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "");
}

// S-54: "abc".substring(0, 100) === "abc" (end beyond length clamped)
TEST(InterpString, SubstringEndBeyondLen) {
    auto v = interp_ok(R"("abc".substring(0, 100))");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "abc");
}

// S-55: "abc".substring(2, 0) === "ab" (start > end -> swap, then [0,2))
TEST(InterpString, SubstringSwapFromTwo) {
    auto v = interp_ok(R"("abc".substring(2, 0))");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "ab");
}

// ============================================================
// Interpreter: split additional boundaries (S-56 ~ S-59)
// ============================================================

// S-56: "".split("") === [] (empty string with empty sep -> no codepoints -> empty array)
TEST(InterpString, SplitEmptyStrEmptySep) {
    auto v = interp_ok(R"(var r = "".split(""); r.length)");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}

// S-57: "".split("x") === [""] (empty string with non-empty sep -> one empty element)
TEST(InterpString, SplitEmptyStrNonEmptySep) {
    auto v = interp_ok(R"(var r = "".split("x"); r[0])");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "");
}

// S-57b: "".split("x").length === 1
TEST(InterpString, SplitEmptyStrNonEmptySepLen) {
    auto v = interp_ok(R"(var r = "".split("x"); r.length)");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

// S-58: ",a,".split(",") -> ["","a",""] (leading/trailing sep produces empty strings)
TEST(InterpString, SplitBoundaryEmptyParts) {
    auto v = interp_ok(R"(var r = ",a,".split(","); r.length)");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 3.0);
}

// S-58b: check first and last elements are empty strings
TEST(InterpString, SplitBoundaryEmptyPartsValues) {
    auto v0 = interp_ok(R"(var r = ",a,".split(","); r[0])");
    EXPECT_TRUE(v0.is_string());
    EXPECT_EQ(v0.as_string(), "");
    auto v1 = interp_ok(R"(var r = ",a,".split(","); r[1])");
    EXPECT_TRUE(v1.is_string());
    EXPECT_EQ(v1.as_string(), "a");
    auto v2 = interp_ok(R"(var r = ",a,".split(","); r[2])");
    EXPECT_TRUE(v2.is_string());
    EXPECT_EQ(v2.as_string(), "");
}

// S-59: "aXbXc".split("X", -1) -> 3 elements (ToUint32(-1)=4294967295, no truncation)
TEST(InterpString, SplitLimitNegativeOne) {
    auto v = interp_ok(R"(var r = "aXbXc".split("X", -1); r.length)");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 3.0);
}

// ============================================================
// Interpreter: trim additional boundaries (S-60 ~ S-62)
// ============================================================

// S-60: paragraph separator U+2029 trimmed (UTF-8: \xe2\x80\xa9)
TEST(InterpString, TrimParagraphSeparator) {
    // U+2029 PARAGRAPH SEPARATOR in UTF-8 is \xe2\x80\xa9
    auto v = interp_ok("\"\xe2\x80\xa9hello\xe2\x80\xa9\".trim()");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "hello");
}

// S-61: BOM U+FEFF trimmed (UTF-8: \xef\xbb\xbf)
TEST(InterpString, TrimBOM) {
    // U+FEFF ZERO WIDTH NO-BREAK SPACE / BOM in UTF-8 is \xef\xbb\xbf
    auto v = interp_ok("\"\xef\xbb\xbfhello\".trim()");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "hello");
}

// S-62: internal spaces preserved by trim
TEST(InterpString, TrimInternalSpaces) {
    auto v = interp_ok(R"(" h e l l o ".trim())");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "h e l l o");
}

// ============================================================
// Interpreter: chained calls (S-63 ~ S-64)
// ============================================================

// S-63: "  hello  ".trim().split("") -> 5 chars
TEST(InterpString, ChainTrimSplit) {
    auto v = interp_ok(R"(var r = "  hello  ".trim().split(""); r.length)");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 5.0);
}

// S-64: "a,b,c".split(",").indexOf("b") === 1 (split returns real array with indexOf)
TEST(InterpString, ChainSplitIndexOf) {
    auto v = interp_ok(R"("a,b,c".split(",").indexOf("b"))");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

// ============================================================
// VM: indexOf additional boundaries (S-39v ~ S-43v)
// ============================================================

// S-39v: "abc".indexOf("", 0) === 0
TEST(VmString, IndexOfEmptyAtZero) {
    auto v = vm_ok(R"("abc".indexOf("", 0))");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}

// S-40v: "abc".indexOf("", 3) === 3
TEST(VmString, IndexOfEmptyAtLen) {
    auto v = vm_ok(R"("abc".indexOf("", 3))");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 3.0);
}

// S-41v: "abc".indexOf("abc") === 0
TEST(VmString, IndexOfWholeString) {
    auto v = vm_ok(R"("abc".indexOf("abc"))");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}

// S-42v: "".indexOf("") === 0
TEST(VmString, IndexOfBothEmpty) {
    auto v = vm_ok(R"("".indexOf(""))");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}

// S-43v: "abcabc".indexOf("a", 1) === 3
TEST(VmString, IndexOfSkipFirst) {
    auto v = vm_ok(R"("abcabc".indexOf("a", 1))");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 3.0);
}

// ============================================================
// VM: lastIndexOf additional boundaries (S-44v ~ S-47v)
// ============================================================

// S-44v: "abcabc".lastIndexOf("a", 0) === 0
TEST(VmString, LastIndexOfFromIndexZero) {
    auto v = vm_ok(R"("abcabc".lastIndexOf("a", 0))");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}

// S-45v: "abcabc".lastIndexOf("a", 2) === 0
TEST(VmString, LastIndexOfFromIndexMid) {
    auto v = vm_ok(R"("abcabc".lastIndexOf("a", 2))");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}

// S-46v: "abc".lastIndexOf("") === 3
TEST(VmString, LastIndexOfEmptyNoFrom) {
    auto v = vm_ok(R"("abc".lastIndexOf(""))");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 3.0);
}

// S-47v: "abc".lastIndexOf("", 1) === 1
TEST(VmString, LastIndexOfEmptyWithFrom) {
    auto v = vm_ok(R"("abc".lastIndexOf("", 1))");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

// ============================================================
// VM: slice additional boundaries (S-48v ~ S-52v)
// ============================================================

// S-48v: "abc".slice() === "abc"
TEST(VmString, SliceNoArgs) {
    auto v = vm_ok(R"("abc".slice())");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "abc");
}

// S-49v: "abc".slice(0) === "abc"
TEST(VmString, SliceFromZero) {
    auto v = vm_ok(R"("abc".slice(0))");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "abc");
}

// S-50v: "abc".slice(1, 1) === ""
TEST(VmString, SliceStartEqualsEnd) {
    auto v = vm_ok(R"("abc".slice(1, 1))");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "");
}

// S-51v: "abc".slice(-100) === "abc"
TEST(VmString, SliceNegativeStartOverflow) {
    auto v = vm_ok(R"("abc".slice(-100))");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "abc");
}

// S-52v: "abc".slice(0, -100) === ""
TEST(VmString, SliceNegativeEndOverflow) {
    auto v = vm_ok(R"("abc".slice(0, -100))");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "");
}

// ============================================================
// VM: substring additional boundaries (S-53v ~ S-55v)
// ============================================================

// S-53v: "abc".substring(0, 0) === ""
TEST(VmString, SubstringEqualArgs) {
    auto v = vm_ok(R"("abc".substring(0, 0))");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "");
}

// S-54v: "abc".substring(0, 100) === "abc"
TEST(VmString, SubstringEndBeyondLen) {
    auto v = vm_ok(R"("abc".substring(0, 100))");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "abc");
}

// S-55v: "abc".substring(2, 0) === "ab"
TEST(VmString, SubstringSwapFromTwo) {
    auto v = vm_ok(R"("abc".substring(2, 0))");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "ab");
}

// ============================================================
// VM: split additional boundaries (S-56v ~ S-59v)
// ============================================================

// S-56v: "".split("").length === 0
TEST(VmString, SplitEmptyStrEmptySep) {
    auto v = vm_ok(R"(var r = "".split(""); r.length)");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}

// S-57v: "".split("x")[0] === ""
TEST(VmString, SplitEmptyStrNonEmptySep) {
    auto v = vm_ok(R"(var r = "".split("x"); r[0])");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "");
}

// S-57bv: "".split("x").length === 1
TEST(VmString, SplitEmptyStrNonEmptySepLen) {
    auto v = vm_ok(R"(var r = "".split("x"); r.length)");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

// S-58v: ",a,".split(",").length === 3
TEST(VmString, SplitBoundaryEmptyParts) {
    auto v = vm_ok(R"(var r = ",a,".split(","); r.length)");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 3.0);
}

// S-58bv: check first, middle, last elements
TEST(VmString, SplitBoundaryEmptyPartsValues) {
    auto v0 = vm_ok(R"(var r = ",a,".split(","); r[0])");
    EXPECT_TRUE(v0.is_string());
    EXPECT_EQ(v0.as_string(), "");
    auto v1 = vm_ok(R"(var r = ",a,".split(","); r[1])");
    EXPECT_TRUE(v1.is_string());
    EXPECT_EQ(v1.as_string(), "a");
    auto v2 = vm_ok(R"(var r = ",a,".split(","); r[2])");
    EXPECT_TRUE(v2.is_string());
    EXPECT_EQ(v2.as_string(), "");
}

// S-59v: "aXbXc".split("X", -1).length === 3 (ToUint32(-1)=4294967295)
TEST(VmString, SplitLimitNegativeOne) {
    auto v = vm_ok(R"(var r = "aXbXc".split("X", -1); r.length)");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 3.0);
}

// ============================================================
// VM: trim additional boundaries (S-60v ~ S-62v)
// ============================================================

// S-60v: paragraph separator U+2029 trimmed
TEST(VmString, TrimParagraphSeparator) {
    auto v = vm_ok("\"\xe2\x80\xa9hello\xe2\x80\xa9\".trim()");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "hello");
}

// S-61v: BOM U+FEFF trimmed
TEST(VmString, TrimBOM) {
    auto v = vm_ok("\"\xef\xbb\xbfhello\".trim()");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "hello");
}

// S-62v: internal spaces preserved by trim
TEST(VmString, TrimInternalSpaces) {
    auto v = vm_ok(R"(" h e l l o ".trim())");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "h e l l o");
}

// ============================================================
// VM: chained calls (S-63v ~ S-64v)
// ============================================================

// S-63v: "  hello  ".trim().split("").length === 5
TEST(VmString, ChainTrimSplit) {
    auto v = vm_ok(R"(var r = "  hello  ".trim().split(""); r.length)");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 5.0);
}

// S-64v: "a,b,c".split(",").indexOf("b") === 1
TEST(VmString, ChainSplitIndexOf) {
    auto v = vm_ok(R"("a,b,c".split(",").indexOf("b"))");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 1.0);
}

// ============================================================
// Interp: M-1 non-BMP (SMP) character UTF-16 code unit semantics
// ============================================================

// S-65i: "😀".length === 2 (SMP char = 2 UTF-16 code units)
TEST(InterpString, SmpLengthIsTwo) {
    // U+1F600 GRINNING FACE, UTF-8: \xF0\x9F\x98\x80
    auto v = interp_ok("\"\xF0\x9F\x98\x80\".length");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.0);
}

// S-66i: "😀a".indexOf("a") === 2
TEST(InterpString, SmpIndexOfAfterSmp) {
    auto v = interp_ok("\"\xF0\x9F\x98\x80\x61\".indexOf(\"a\")");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.0);
}

// S-67i: "abc😀".slice(0, 4) === "abc😀"  (SMP occupies 2 code units)
TEST(InterpString, SmpSliceIncludesSmp) {
    auto v = interp_ok("\"\x61\x62\x63\xF0\x9F\x98\x80\".slice(0, 4)");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "abc\xF0\x9F\x98\x80");
}

// S-68i: "a😀b".length === 4 (a=1, 😀=2, b=1)
TEST(InterpString, SmpMixedLength) {
    auto v = interp_ok("\"\x61\xF0\x9F\x98\x80\x62\".length");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 4.0);
}

// S-69i: M-2 split(undefined, 0) returns []
TEST(InterpString, SplitUndefinedLimitZero) {
    auto v = interp_ok(R"(var r = "abc".split(undefined, 0); r.length)");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}

// ============================================================
// VM: M-1 non-BMP (SMP) character UTF-16 code unit semantics
// ============================================================

// S-65v: "😀".length === 2
TEST(VmString, SmpLengthIsTwo) {
    auto v = vm_ok("\"\xF0\x9F\x98\x80\".length");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.0);
}

// S-66v: "😀a".indexOf("a") === 2
TEST(VmString, SmpIndexOfAfterSmp) {
    auto v = vm_ok("\"\xF0\x9F\x98\x80\x61\".indexOf(\"a\")");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.0);
}

// S-67v: "abc😀".slice(0, 4) === "abc😀"
TEST(VmString, SmpSliceIncludesSmp) {
    auto v = vm_ok("\"\x61\x62\x63\xF0\x9F\x98\x80\".slice(0, 4)");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "abc\xF0\x9F\x98\x80");
}

// S-68v: "a😀b".length === 4
TEST(VmString, SmpMixedLength) {
    auto v = vm_ok("\"\x61\xF0\x9F\x98\x80\x62\".length");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 4.0);
}

// S-69v: M-2 split(undefined, 0) returns []
TEST(VmString, SplitUndefinedLimitZero) {
    auto v = vm_ok(R"(var r = "abc".split(undefined, 0); r.length)");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}

// S-70v: M-3 kGetElem string method access via bracket notation
TEST(VmString, GetElemStringMethod) {
    auto v = vm_ok(R"(var fn = "hello"["indexOf"]; fn.call("hello", "l"))");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.0);
}

// S-70i: M-3 bracket notation on string (interpreter side)
TEST(InterpString, GetElemStringMethod) {
    auto v = interp_ok(R"(var fn = "hello"["indexOf"]; fn.call("hello", "l"))");
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 2.0);
}

}  // namespace
