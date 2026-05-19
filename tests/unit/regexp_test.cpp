#include "qppjs/frontend/parser.h"
#include "qppjs/runtime/interpreter.h"
#include "qppjs/vm/compiler.h"
#include "qppjs/vm/vm.h"

#include <gtest/gtest.h>
#include <string>

using namespace qppjs;

// ============================================================
// 辅助宏
// ============================================================

#define INTERP_OK(src, var)                                         \
    auto parse_##var = parse_program(src);                          \
    ASSERT_TRUE(parse_##var.ok()) << parse_##var.error().message(); \
    Interpreter interp_##var;                                       \
    auto var = interp_##var.exec(parse_##var.value());              \
    ASSERT_TRUE(var.is_ok()) << var.error().message()

#define INTERP_ERR(src, var)                                        \
    auto parse_##var = parse_program(src);                          \
    ASSERT_TRUE(parse_##var.ok()) << parse_##var.error().message(); \
    Interpreter interp_##var;                                       \
    auto var = interp_##var.exec(parse_##var.value());              \
    ASSERT_FALSE(var.is_ok())

#define VM_OK(src, var)                                             \
    auto parse_##var = parse_program(src);                          \
    ASSERT_TRUE(parse_##var.ok()) << parse_##var.error().message(); \
    VM vm_##var;                                                    \
    Compiler compiler_##var;                                        \
    auto bc_##var = compiler_##var.compile(parse_##var.value());    \
    ASSERT_NE(bc_##var, nullptr) << "compile failed";               \
    auto var = vm_##var.exec(bc_##var);                             \
    ASSERT_TRUE(var.is_ok()) << var.error().message()

#define VM_ERR(src, var)                                            \
    auto parse_##var = parse_program(src);                          \
    ASSERT_TRUE(parse_##var.ok()) << parse_##var.error().message(); \
    VM vm_##var;                                                    \
    Compiler compiler_##var;                                        \
    auto bc_##var = compiler_##var.compile(parse_##var.value());    \
    ASSERT_NE(bc_##var, nullptr) << "compile failed";               \
    auto var = vm_##var.exec(bc_##var);                             \
    ASSERT_FALSE(var.is_ok())

// ============================================================
// RX-01: 正则字面量创建，typeof object
// ============================================================

TEST(RegExpInterp, RX01_TypeofObject) {
    INTERP_OK("typeof /abc/gi;", r);
    EXPECT_EQ(r.value().as_string(), "object");
}

TEST(RegExpVM, RX01_TypeofObject) {
    VM_OK("typeof /abc/gi;", r);
    EXPECT_EQ(r.value().as_string(), "object");
}

// ============================================================
// RX-02: /a/ !== /a/ — 每次求值新对象
// ============================================================

TEST(RegExpInterp, RX02_NewObjectEachEval) {
    INTERP_OK("var a = /a/; var b = /a/; a === b;", r);
    EXPECT_EQ(r.value().as_bool(), false);
}

TEST(RegExpVM, RX02_NewObjectEachEval) {
    VM_OK("var a = /a/; var b = /a/; a === b;", r);
    EXPECT_EQ(r.value().as_bool(), false);
}

// ============================================================
// RX-03: source/flags/global/ignoreCase/multiline getter
// ============================================================

TEST(RegExpInterp, RX03_Getters) {
    {
        INTERP_OK("/abc/gi.source;", r);
        EXPECT_EQ(r.value().as_string(), "abc");
    }
    {
        INTERP_OK("/abc/gi.flags;", r);
        EXPECT_EQ(r.value().as_string(), "gi");
    }
    {
        INTERP_OK("/abc/g.global;", r);
        EXPECT_EQ(r.value().as_bool(), true);
    }
    {
        INTERP_OK("/abc/i.ignoreCase;", r);
        EXPECT_EQ(r.value().as_bool(), true);
    }
    {
        INTERP_OK("/abc/m.multiline;", r);
        EXPECT_EQ(r.value().as_bool(), true);
    }
}

TEST(RegExpVM, RX03_Getters) {
    {
        VM_OK("/abc/gi.source;", r);
        EXPECT_EQ(r.value().as_string(), "abc");
    }
    {
        VM_OK("/abc/gi.flags;", r);
        EXPECT_EQ(r.value().as_string(), "gi");
    }
    {
        VM_OK("/abc/g.global;", r);
        EXPECT_EQ(r.value().as_bool(), true);
    }
    {
        VM_OK("/abc/i.ignoreCase;", r);
        EXPECT_EQ(r.value().as_bool(), true);
    }
    {
        VM_OK("/abc/m.multiline;", r);
        EXPECT_EQ(r.value().as_bool(), true);
    }
}

// ============================================================
// RX-04: 空 pattern，source === "(?:)"
// ============================================================

TEST(RegExpInterp, RX04_EmptyPatternSource) {
    INTERP_OK("new RegExp('').source;", r);
    EXPECT_EQ(r.value().as_string(), "(?:)");
}

TEST(RegExpVM, RX04_EmptyPatternSource) {
    VM_OK("new RegExp('').source;", r);
    EXPECT_EQ(r.value().as_string(), "(?:)");
}

// ============================================================
// RX-05: exec 无匹配返回 null
// ============================================================

TEST(RegExpInterp, RX05_ExecNoMatch) {
    INTERP_OK("/xyz/.exec('hello') === null;", r);
    EXPECT_EQ(r.value().as_bool(), true);
}

TEST(RegExpVM, RX05_ExecNoMatch) {
    VM_OK("/xyz/.exec('hello') === null;", r);
    EXPECT_EQ(r.value().as_bool(), true);
}

// ============================================================
// RX-06: exec 有匹配，返回数组含 index/input
// ============================================================

TEST(RegExpInterp, RX06_ExecMatch) {
    {
        INTERP_OK("var m = /hello/.exec('say hello world'); m[0];", r);
        EXPECT_EQ(r.value().as_string(), "hello");
    }
    {
        INTERP_OK("var m = /hello/.exec('say hello world'); m.index;", r);
        EXPECT_DOUBLE_EQ(r.value().as_number(), 4.0);
    }
    {
        INTERP_OK("var m = /hello/.exec('say hello world'); m.input;", r);
        EXPECT_EQ(r.value().as_string(), "say hello world");
    }
}

TEST(RegExpVM, RX06_ExecMatch) {
    {
        VM_OK("var m = /hello/.exec('say hello world'); m[0];", r);
        EXPECT_EQ(r.value().as_string(), "hello");
    }
    {
        VM_OK("var m = /hello/.exec('say hello world'); m.index;", r);
        EXPECT_DOUBLE_EQ(r.value().as_number(), 4.0);
    }
    {
        VM_OK("var m = /hello/.exec('say hello world'); m.input;", r);
        EXPECT_EQ(r.value().as_string(), "say hello world");
    }
}

// ============================================================
// RX-07: exec 捕获组（未匹配组为 undefined）
// ============================================================

TEST(RegExpInterp, RX07_CaptureGroups) {
    {
        INTERP_OK("var m = /(a)(b)?/.exec('a'); m[1];", r);
        EXPECT_EQ(r.value().as_string(), "a");
    }
    {
        INTERP_OK("var m = /(a)(b)?/.exec('a'); m[2] === undefined;", r);
        EXPECT_EQ(r.value().as_bool(), true);
    }
}

TEST(RegExpVM, RX07_CaptureGroups) {
    {
        VM_OK("var m = /(a)(b)?/.exec('a'); m[1];", r);
        EXPECT_EQ(r.value().as_string(), "a");
    }
    {
        VM_OK("var m = /(a)(b)?/.exec('a'); m[2] === undefined;", r);
        EXPECT_EQ(r.value().as_bool(), true);
    }
}

// ============================================================
// RX-08: exec global 模式 lastIndex 更新
// ============================================================

TEST(RegExpInterp, RX08_GlobalLastIndex) {
    INTERP_OK("var rx = /a/g; rx.exec('abab'); rx.lastIndex;", r);
    EXPECT_DOUBLE_EQ(r.value().as_number(), 1.0);
}

TEST(RegExpVM, RX08_GlobalLastIndex) {
    VM_OK("var rx = /a/g; rx.exec('abab'); rx.lastIndex;", r);
    EXPECT_DOUBLE_EQ(r.value().as_number(), 1.0);
}

// ============================================================
// RX-09: exec 失败后 lastIndex 置 0
// ============================================================

TEST(RegExpInterp, RX09_FailedExecResetsLastIndex) {
    INTERP_OK("var rx = /a/g; rx.exec('b'); rx.lastIndex;", r);
    EXPECT_DOUBLE_EQ(r.value().as_number(), 0.0);
}

TEST(RegExpVM, RX09_FailedExecResetsLastIndex) {
    VM_OK("var rx = /a/g; rx.exec('b'); rx.lastIndex;", r);
    EXPECT_DOUBLE_EQ(r.value().as_number(), 0.0);
}

// ============================================================
// RX-10: test 返回 boolean
// ============================================================

TEST(RegExpInterp, RX10_TestReturnsBoolean) {
    {
        INTERP_OK("/hello/.test('say hello');", r);
        EXPECT_EQ(r.value().as_bool(), true);
    }
    {
        INTERP_OK("/world/.test('say hello');", r);
        EXPECT_EQ(r.value().as_bool(), false);
    }
}

TEST(RegExpVM, RX10_TestReturnsBoolean) {
    {
        VM_OK("/hello/.test('say hello');", r);
        EXPECT_EQ(r.value().as_bool(), true);
    }
    {
        VM_OK("/world/.test('say hello');", r);
        EXPECT_EQ(r.value().as_bool(), false);
    }
}

// ============================================================
// RX-11: match 非全局（含 index/input）
// ============================================================

TEST(RegExpInterp, RX11_MatchNonGlobal) {
    {
        INTERP_OK("var m = 'hello world'.match(/world/); m[0];", r);
        EXPECT_EQ(r.value().as_string(), "world");
    }
    {
        INTERP_OK("var m = 'hello world'.match(/world/); m.index;", r);
        EXPECT_DOUBLE_EQ(r.value().as_number(), 6.0);
    }
}

TEST(RegExpVM, RX11_MatchNonGlobal) {
    {
        VM_OK("var m = 'hello world'.match(/world/); m[0];", r);
        EXPECT_EQ(r.value().as_string(), "world");
    }
    {
        VM_OK("var m = 'hello world'.match(/world/); m.index;", r);
        EXPECT_DOUBLE_EQ(r.value().as_number(), 6.0);
    }
}

// ============================================================
// RX-12: match 全局（返回字符串数组）
// ============================================================

TEST(RegExpInterp, RX12_MatchGlobal) {
    {
        INTERP_OK("var m = 'abab'.match(/a/g); m.length;", r);
        EXPECT_DOUBLE_EQ(r.value().as_number(), 2.0);
    }
    {
        INTERP_OK("var m = 'abab'.match(/a/g); m[0];", r);
        EXPECT_EQ(r.value().as_string(), "a");
    }
}

TEST(RegExpVM, RX12_MatchGlobal) {
    {
        VM_OK("var m = 'abab'.match(/a/g); m.length;", r);
        EXPECT_DOUBLE_EQ(r.value().as_number(), 2.0);
    }
    {
        VM_OK("var m = 'abab'.match(/a/g); m[0];", r);
        EXPECT_EQ(r.value().as_string(), "a");
    }
}

// ============================================================
// RX-13: match 无匹配返回 null
// ============================================================

TEST(RegExpInterp, RX13_MatchNoMatch) {
    INTERP_OK("'hello'.match(/xyz/) === null;", r);
    EXPECT_EQ(r.value().as_bool(), true);
}

TEST(RegExpVM, RX13_MatchNoMatch) {
    VM_OK("'hello'.match(/xyz/) === null;", r);
    EXPECT_EQ(r.value().as_bool(), true);
}

// ============================================================
// RX-14: toString 输出 /pattern/flags
// ============================================================

TEST(RegExpInterp, RX14_ToString) {
    INTERP_OK("/abc/gi.toString();", r);
    EXPECT_EQ(r.value().as_string(), "/abc/gi");
}

TEST(RegExpVM, RX14_ToString) {
    VM_OK("/abc/gi.toString();", r);
    EXPECT_EQ(r.value().as_string(), "/abc/gi");
}

// ============================================================
// RX-15: RegExp(rx) 无 flags 返回 rx 本身
// ============================================================

TEST(RegExpInterp, RX15_RegExpCallWithRx) {
    INTERP_OK("var rx = /abc/g; RegExp(rx) === rx;", r);
    EXPECT_EQ(r.value().as_bool(), true);
}

TEST(RegExpVM, RX15_RegExpCallWithRx) {
    VM_OK("var rx = /abc/g; RegExp(rx) === rx;", r);
    EXPECT_EQ(r.value().as_bool(), true);
}

// ============================================================
// RX-16: new RegExp(rx) 创建新对象
// ============================================================

TEST(RegExpInterp, RX16_NewRegExpCreatesNew) {
    INTERP_OK("var rx = /abc/; var rx2 = new RegExp(rx); rx2 !== rx;", r);
    EXPECT_EQ(r.value().as_bool(), true);
}

TEST(RegExpVM, RX16_NewRegExpCreatesNew) {
    VM_OK("var rx = /abc/; var rx2 = new RegExp(rx); rx2 !== rx;", r);
    EXPECT_EQ(r.value().as_bool(), true);
}

// ============================================================
// RX-17: flags 含重复字符 → SyntaxError
// ============================================================

TEST(RegExpInterp, RX17_DuplicateFlags) {
    INTERP_ERR("new RegExp('a', 'gg');", r);
}

TEST(RegExpVM, RX17_DuplicateFlags) {
    VM_ERR("new RegExp('a', 'gg');", r);
}

// ============================================================
// RX-18: flags 含非法字符 → SyntaxError
// ============================================================

TEST(RegExpInterp, RX18_InvalidFlags) {
    INTERP_ERR("new RegExp('a', 'x');", r);
}

TEST(RegExpVM, RX18_InvalidFlags) {
    VM_ERR("new RegExp('a', 'x');", r);
}

// ============================================================
// RX-19: pattern 无效 → SyntaxError
// ============================================================

TEST(RegExpInterp, RX19_InvalidPattern) {
    INTERP_ERR("new RegExp('[invalid');", r);
}

TEST(RegExpVM, RX19_InvalidPattern) {
    VM_ERR("new RegExp('[invalid');", r);
}

// ============================================================
// RX-20: i flag（ignoreCase）
// ============================================================

TEST(RegExpInterp, RX20_IgnoreCase) {
    INTERP_OK("/hello/i.test('HELLO');", r);
    EXPECT_EQ(r.value().as_bool(), true);
}

TEST(RegExpVM, RX20_IgnoreCase) {
    VM_OK("/hello/i.test('HELLO');", r);
    EXPECT_EQ(r.value().as_bool(), true);
}

// ============================================================
// RX-21: m flag（multiline ^ $）
// ============================================================

TEST(RegExpInterp, RX21_Multiline) {
    INTERP_OK("/^world/m.test('hello\\nworld');", r);
    EXPECT_EQ(r.value().as_bool(), true);
}

TEST(RegExpVM, RX21_Multiline) {
    VM_OK("/^world/m.test('hello\\nworld');", r);
    EXPECT_EQ(r.value().as_bool(), true);
}

// ============================================================
// RX-22: g flag 全局 exec 循环
// ============================================================

TEST(RegExpInterp, RX22_GlobalExecLoop) {
    INTERP_OK(
        "var rx = /a/g; var s = 'abab'; var count = 0;"
        "while (rx.exec(s) !== null) { count++; }"
        "count;",
        r);
    EXPECT_DOUBLE_EQ(r.value().as_number(), 2.0);
}

TEST(RegExpVM, RX22_GlobalExecLoop) {
    VM_OK(
        "var rx = /a/g; var s = 'abab'; var count = 0;"
        "while (rx.exec(s) !== null) { count++; }"
        "count;",
        r);
    EXPECT_DOUBLE_EQ(r.value().as_number(), 2.0);
}

// ============================================================
// RX-23: lastIndex 可写（手动设置）
// ============================================================

TEST(RegExpInterp, RX23_LastIndexWritable) {
    INTERP_OK("var rx = /a/g; rx.exec('abab'); rx.lastIndex = 0; rx.exec('abab'); rx.lastIndex;", r);
    EXPECT_DOUBLE_EQ(r.value().as_number(), 1.0);
}

TEST(RegExpVM, RX23_LastIndexWritable) {
    VM_OK("var rx = /a/g; rx.exec('abab'); rx.lastIndex = 0; rx.exec('abab'); rx.lastIndex;", r);
    EXPECT_DOUBLE_EQ(r.value().as_number(), 1.0);
}

// ============================================================
// RX-24: instanceof RegExp
// ============================================================

TEST(RegExpInterp, RX24_Instanceof) {
    INTERP_OK("/abc/ instanceof RegExp;", r);
    EXPECT_EQ(r.value().as_bool(), true);
}

TEST(RegExpVM, RX24_Instanceof) {
    VM_OK("/abc/ instanceof RegExp;", r);
    EXPECT_EQ(r.value().as_bool(), true);
}

// ============================================================
// 额外测试：dotAll (s flag)
// ============================================================

TEST(RegExpInterp, DotAll) {
    INTERP_OK("/a.b/s.test('a\\nb');", r);
    EXPECT_EQ(r.value().as_bool(), true);
}

TEST(RegExpVM, DotAll) {
    VM_OK("/a.b/s.test('a\\nb');", r);
    EXPECT_EQ(r.value().as_bool(), true);
}

// ============================================================
// 额外测试：RegExp constructor with string pattern
// ============================================================

TEST(RegExpInterp, ConstructorStringPattern) {
    INTERP_OK("new RegExp('hello').test('say hello');", r);
    EXPECT_EQ(r.value().as_bool(), true);
}

TEST(RegExpVM, ConstructorStringPattern) {
    VM_OK("new RegExp('hello').test('say hello');", r);
    EXPECT_EQ(r.value().as_bool(), true);
}

// ============================================================
// RX-25: exec 结果属性验证 — groups === undefined，input 与参数一致，index 精确值
// ============================================================

TEST(RegExpInterp, RX25_ExecResultProperties) {
    {
        // groups === undefined（无命名捕获组时）
        INTERP_OK("var m = /hello/.exec('say hello world'); m.groups === undefined;", r);
        EXPECT_EQ(r.value().as_bool(), true);
    }
    {
        // input 与传入字符串完全一致
        INTERP_OK("var m = /hello/.exec('say hello world'); m.input === 'say hello world';", r);
        EXPECT_EQ(r.value().as_bool(), true);
    }
    {
        // index 精确值：'say hello' 中 'hello' 从 4 开始
        INTERP_OK("var m = /hello/.exec('say hello world'); m.index === 4;", r);
        EXPECT_EQ(r.value().as_bool(), true);
    }
    {
        // index 为 0 时（匹配在字符串起始）
        INTERP_OK("var m = /abc/.exec('abcdef'); m.index === 0;", r);
        EXPECT_EQ(r.value().as_bool(), true);
    }
}

TEST(RegExpVM, RX25_ExecResultProperties) {
    {
        VM_OK("var m = /hello/.exec('say hello world'); m.groups === undefined;", r);
        EXPECT_EQ(r.value().as_bool(), true);
    }
    {
        VM_OK("var m = /hello/.exec('say hello world'); m.input === 'say hello world';", r);
        EXPECT_EQ(r.value().as_bool(), true);
    }
    {
        VM_OK("var m = /hello/.exec('say hello world'); m.index === 4;", r);
        EXPECT_EQ(r.value().as_bool(), true);
    }
    {
        VM_OK("var m = /abc/.exec('abcdef'); m.index === 0;", r);
        EXPECT_EQ(r.value().as_bool(), true);
    }
}

// ============================================================
// RX-26: lastIndex 边界 — NaN/负数/超出字符串长度
// ============================================================

TEST(RegExpInterp, RX26_LastIndexBoundary) {
    {
        // lastIndex = NaN → 视为 0，从头匹配
        INTERP_OK("var rx = /a/g; rx.lastIndex = NaN; rx.exec('abc'); rx.lastIndex;", r);
        EXPECT_DOUBLE_EQ(r.value().as_number(), 1.0);
    }
    {
        // lastIndex = -5 → 视为 0，从头匹配
        INTERP_OK("var rx = /a/g; rx.lastIndex = -5; rx.exec('abc'); rx.lastIndex;", r);
        EXPECT_DOUBLE_EQ(r.value().as_number(), 1.0);
    }
    {
        // lastIndex 超出字符串长度 → 立即返回 null，lastIndex 置 0
        INTERP_OK("var rx = /a/g; rx.lastIndex = 100; var r = rx.exec('abc'); r === null;", r);
        EXPECT_EQ(r.value().as_bool(), true);
    }
    {
        // lastIndex 超出后 lastIndex 应置 0
        INTERP_OK("var rx = /a/g; rx.lastIndex = 100; rx.exec('abc'); rx.lastIndex;", r);
        EXPECT_DOUBLE_EQ(r.value().as_number(), 0.0);
    }
}

TEST(RegExpVM, RX26_LastIndexBoundary) {
    {
        VM_OK("var rx = /a/g; rx.lastIndex = NaN; rx.exec('abc'); rx.lastIndex;", r);
        EXPECT_DOUBLE_EQ(r.value().as_number(), 1.0);
    }
    {
        VM_OK("var rx = /a/g; rx.lastIndex = -5; rx.exec('abc'); rx.lastIndex;", r);
        EXPECT_DOUBLE_EQ(r.value().as_number(), 1.0);
    }
    {
        VM_OK("var rx = /a/g; rx.lastIndex = 100; var r = rx.exec('abc'); r === null;", r);
        EXPECT_EQ(r.value().as_bool(), true);
    }
    {
        VM_OK("var rx = /a/g; rx.lastIndex = 100; rx.exec('abc'); rx.lastIndex;", r);
        EXPECT_DOUBLE_EQ(r.value().as_number(), 0.0);
    }
}

// ============================================================
// RX-27: exec this 检查 — 非 RegExp 对象调用 exec → TypeError
// ============================================================

TEST(RegExpInterp, RX27_ExecThisCheck) {
    {
        // 普通对象调用 exec → TypeError
        INTERP_ERR(
            "var obj = {}; RegExp.prototype.exec.call(obj, 'test');",
            r);
    }
    {
        // 捕获异常类型为 TypeError
        INTERP_OK(
            "var obj = {};"
            "try { RegExp.prototype.exec.call(obj, 'test'); } catch(e) { e instanceof TypeError; }",
            r);
        EXPECT_EQ(r.value().as_bool(), true);
    }
}

TEST(RegExpVM, RX27_ExecThisCheck) {
    {
        VM_ERR(
            "var obj = {}; RegExp.prototype.exec.call(obj, 'test');",
            r);
    }
    {
        // VM 侧需要将结果赋值给变量，last_expr_name 机制才能正确重读
        VM_OK(
            "var obj = {}; var result = false;"
            "try { RegExp.prototype.exec.call(obj, 'test'); } catch(e) { result = e instanceof TypeError; }"
            "result",
            r);
        EXPECT_EQ(r.value().as_bool(), true);
    }
}

// ============================================================
// RX-28: 多次 exec 循环终止 — g 模式连续三次 exec（第三次返回 null，lastIndex 回 0）
// ============================================================

TEST(RegExpInterp, RX28_GlobalExecThreeTimes) {
    {
        // 第一次匹配 'a' at index 0
        INTERP_OK("var rx = /a/g; var s = 'abab'; var m1 = rx.exec(s); m1[0];", r);
        EXPECT_EQ(r.value().as_string(), "a");
    }
    {
        // 第二次匹配 'a' at index 2
        INTERP_OK(
            "var rx = /a/g; var s = 'abab';"
            "rx.exec(s);"
            "var m2 = rx.exec(s); m2[0];",
            r);
        EXPECT_EQ(r.value().as_string(), "a");
    }
    {
        // 第三次无匹配，返回 null
        INTERP_OK(
            "var rx = /a/g; var s = 'abab';"
            "rx.exec(s); rx.exec(s);"
            "var m3 = rx.exec(s); m3 === null;",
            r);
        EXPECT_EQ(r.value().as_bool(), true);
    }
    {
        // 第三次失败后 lastIndex 回 0
        INTERP_OK(
            "var rx = /a/g; var s = 'abab';"
            "rx.exec(s); rx.exec(s); rx.exec(s);"
            "rx.lastIndex;",
            r);
        EXPECT_DOUBLE_EQ(r.value().as_number(), 0.0);
    }
}

TEST(RegExpVM, RX28_GlobalExecThreeTimes) {
    {
        VM_OK("var rx = /a/g; var s = 'abab'; var m1 = rx.exec(s); m1[0];", r);
        EXPECT_EQ(r.value().as_string(), "a");
    }
    {
        VM_OK(
            "var rx = /a/g; var s = 'abab';"
            "rx.exec(s);"
            "var m2 = rx.exec(s); m2[0];",
            r);
        EXPECT_EQ(r.value().as_string(), "a");
    }
    {
        VM_OK(
            "var rx = /a/g; var s = 'abab';"
            "rx.exec(s); rx.exec(s);"
            "var m3 = rx.exec(s); m3 === null;",
            r);
        EXPECT_EQ(r.value().as_bool(), true);
    }
    {
        VM_OK(
            "var rx = /a/g; var s = 'abab';"
            "rx.exec(s); rx.exec(s); rx.exec(s);"
            "rx.lastIndex;",
            r);
        EXPECT_DOUBLE_EQ(r.value().as_number(), 0.0);
    }
}

// ============================================================
// RX-29: toString 边界 — 斜杠需要转义
// ============================================================

TEST(RegExpInterp, RX29_ToStringSlashEscape) {
    {
        // new RegExp('/') → "/\\//"
        INTERP_OK("new RegExp('/').toString();", r);
        EXPECT_EQ(r.value().as_string(), "/\\//");
    }
    {
        // 已有转义的斜杠不重复转义：/\// → "/\\//"
        INTERP_OK("/\\//.toString();", r);
        EXPECT_EQ(r.value().as_string(), "/\\//");
    }
    {
        // 空 pattern → "/(?:)/"
        INTERP_OK("new RegExp('').toString();", r);
        EXPECT_EQ(r.value().as_string(), "/(?:)/");
    }
}

TEST(RegExpVM, RX29_ToStringSlashEscape) {
    {
        VM_OK("new RegExp('/').toString();", r);
        EXPECT_EQ(r.value().as_string(), "/\\//");
    }
    {
        VM_OK("/\\//.toString();", r);
        EXPECT_EQ(r.value().as_string(), "/\\//");
    }
    {
        VM_OK("new RegExp('').toString();", r);
        EXPECT_EQ(r.value().as_string(), "/(?:)/");
    }
}

// ============================================================
// RX-30: 全局 match 空匹配防死循环 — "abc".match(/(?:)/g) 返回有限数组
// ============================================================

TEST(RegExpInterp, RX30_GlobalMatchEmptyPattern) {
    {
        // 返回非 null（有匹配）
        INTERP_OK("'abc'.match(/(?:)/g) !== null;", r);
        EXPECT_EQ(r.value().as_bool(), true);
    }
    {
        // 长度有限（不死循环），"abc" 有 4 个位置（0,1,2,3），每次空匹配后 lastIndex++
        INTERP_OK("var m = 'abc'.match(/(?:)/g); m.length;", r);
        EXPECT_DOUBLE_EQ(r.value().as_number(), 4.0);
    }
}

TEST(RegExpVM, RX30_GlobalMatchEmptyPattern) {
    {
        VM_OK("'abc'.match(/(?:)/g) !== null;", r);
        EXPECT_EQ(r.value().as_bool(), true);
    }
    {
        VM_OK("var m = 'abc'.match(/(?:)/g); m.length;", r);
        EXPECT_DOUBLE_EQ(r.value().as_number(), 4.0);
    }
}

// ============================================================
// RX-31: new RegExp(pattern, flags) 覆盖原 flags
// ============================================================

TEST(RegExpInterp, RX31_NewRegExpOverrideFlags) {
    {
        // new RegExp(/abc/i, "m") → flags 为 "m"，不含 "i"
        INTERP_OK("new RegExp(/abc/i, 'm').flags;", r);
        EXPECT_EQ(r.value().as_string(), "m");
    }
    {
        // ignoreCase 应为 false
        INTERP_OK("new RegExp(/abc/i, 'm').ignoreCase;", r);
        EXPECT_EQ(r.value().as_bool(), false);
    }
    {
        // multiline 应为 true
        INTERP_OK("new RegExp(/abc/i, 'm').multiline;", r);
        EXPECT_EQ(r.value().as_bool(), true);
    }
}

TEST(RegExpVM, RX31_NewRegExpOverrideFlags) {
    {
        VM_OK("new RegExp(/abc/i, 'm').flags;", r);
        EXPECT_EQ(r.value().as_string(), "m");
    }
    {
        VM_OK("new RegExp(/abc/i, 'm').ignoreCase;", r);
        EXPECT_EQ(r.value().as_bool(), false);
    }
    {
        VM_OK("new RegExp(/abc/i, 'm').multiline;", r);
        EXPECT_EQ(r.value().as_bool(), true);
    }
}

// ============================================================
// RX-32: new RegExp(undefined) → source === "(?:)"
// ============================================================

TEST(RegExpInterp, RX32_NewRegExpUndefined) {
    {
        INTERP_OK("new RegExp(undefined).source;", r);
        EXPECT_EQ(r.value().as_string(), "(?:)");
    }
    {
        // 能正常匹配空字符串
        INTERP_OK("new RegExp(undefined).test('');", r);
        EXPECT_EQ(r.value().as_bool(), true);
    }
}

TEST(RegExpVM, RX32_NewRegExpUndefined) {
    {
        VM_OK("new RegExp(undefined).source;", r);
        EXPECT_EQ(r.value().as_string(), "(?:)");
    }
    {
        VM_OK("new RegExp(undefined).test('');", r);
        EXPECT_EQ(r.value().as_bool(), true);
    }
}

// ============================================================
// RX-33: flags 访问器顺序 — 混乱输入 "mig" → flags 按 g/i/m 顺序输出
// ============================================================

TEST(RegExpInterp, RX33_FlagsOrder) {
    {
        // 字面量 /abc/mig → flags 应为 "gim"
        INTERP_OK("/abc/mig.flags;", r);
        EXPECT_EQ(r.value().as_string(), "gim");
    }
    {
        // 构造函数 new RegExp('abc', 'mig') → flags 应为 "gim"
        INTERP_OK("new RegExp('abc', 'mig').flags;", r);
        EXPECT_EQ(r.value().as_string(), "gim");
    }
    {
        // 全部 flags "yusimg" → 规范化为 "gimsuy"
        INTERP_OK("new RegExp('a', 'yusimg').flags;", r);
        EXPECT_EQ(r.value().as_string(), "gimsuy");
    }
}

TEST(RegExpVM, RX33_FlagsOrder) {
    {
        VM_OK("/abc/mig.flags;", r);
        EXPECT_EQ(r.value().as_string(), "gim");
    }
    {
        VM_OK("new RegExp('abc', 'mig').flags;", r);
        EXPECT_EQ(r.value().as_string(), "gim");
    }
    {
        VM_OK("new RegExp('a', 'yusimg').flags;", r);
        EXPECT_EQ(r.value().as_string(), "gimsuy");
    }
}

// ============================================================
// RX-34: exec 返回数组 length == 捕获组数 + 1
// ============================================================

TEST(RegExpInterp, RX34_ExecArrayLength) {
    {
        // 无捕获组 → length = 1
        INTERP_OK("var m = /abc/.exec('abc'); m.length;", r);
        EXPECT_DOUBLE_EQ(r.value().as_number(), 1.0);
    }
    {
        // 1 个捕获组 → length = 2
        INTERP_OK("var m = /(a)/.exec('a'); m.length;", r);
        EXPECT_DOUBLE_EQ(r.value().as_number(), 2.0);
    }
    {
        // 3 个捕获组 → length = 4
        INTERP_OK("var m = /(a)(b)(c)/.exec('abc'); m.length;", r);
        EXPECT_DOUBLE_EQ(r.value().as_number(), 4.0);
    }
    {
        // 含未匹配的可选捕获组 → length 仍包含未匹配组
        INTERP_OK("var m = /(a)(b)?/.exec('a'); m.length;", r);
        EXPECT_DOUBLE_EQ(r.value().as_number(), 3.0);
    }
}

TEST(RegExpVM, RX34_ExecArrayLength) {
    {
        VM_OK("var m = /abc/.exec('abc'); m.length;", r);
        EXPECT_DOUBLE_EQ(r.value().as_number(), 1.0);
    }
    {
        VM_OK("var m = /(a)/.exec('a'); m.length;", r);
        EXPECT_DOUBLE_EQ(r.value().as_number(), 2.0);
    }
    {
        VM_OK("var m = /(a)(b)(c)/.exec('abc'); m.length;", r);
        EXPECT_DOUBLE_EQ(r.value().as_number(), 4.0);
    }
    {
        VM_OK("var m = /(a)(b)?/.exec('a'); m.length;", r);
        EXPECT_DOUBLE_EQ(r.value().as_number(), 3.0);
    }
}

// ============================================================
// RX-35: sticky flag 基础行为 — y flag exec 在非 lastIndex 位置无法匹配
// ============================================================

TEST(RegExpInterp, RX35_StickyFlag) {
    {
        // lastIndex = 0，匹配起始的 'a'
        INTERP_OK("var rx = /a/y; rx.exec('abc'); rx.lastIndex;", r);
        EXPECT_DOUBLE_EQ(r.value().as_number(), 1.0);
    }
    {
        // lastIndex = 1，位置 1 是 'b'，无法匹配 'a'，返回 null
        INTERP_OK("var rx = /a/y; rx.lastIndex = 1; var m = rx.exec('abc'); m === null;", r);
        EXPECT_EQ(r.value().as_bool(), true);
    }
    {
        // sticky 失败后 lastIndex 置 0
        INTERP_OK("var rx = /a/y; rx.lastIndex = 1; rx.exec('abc'); rx.lastIndex;", r);
        EXPECT_DOUBLE_EQ(r.value().as_number(), 0.0);
    }
    {
        // lastIndex = 2，位置 2 是 'c'，无法匹配 'a'
        INTERP_OK("var rx = /a/y; rx.lastIndex = 2; var m = rx.exec('abc'); m === null;", r);
        EXPECT_EQ(r.value().as_bool(), true);
    }
}

TEST(RegExpVM, RX35_StickyFlag) {
    {
        VM_OK("var rx = /a/y; rx.exec('abc'); rx.lastIndex;", r);
        EXPECT_DOUBLE_EQ(r.value().as_number(), 1.0);
    }
    {
        VM_OK("var rx = /a/y; rx.lastIndex = 1; var m = rx.exec('abc'); m === null;", r);
        EXPECT_EQ(r.value().as_bool(), true);
    }
    {
        VM_OK("var rx = /a/y; rx.lastIndex = 1; rx.exec('abc'); rx.lastIndex;", r);
        EXPECT_DOUBLE_EQ(r.value().as_number(), 0.0);
    }
    {
        VM_OK("var rx = /a/y; rx.lastIndex = 2; var m = rx.exec('abc'); m === null;", r);
        EXPECT_EQ(r.value().as_bool(), true);
    }
}

// ============================================================
// RX-36: 非全局 match 的 groups 属性 — 返回数组含 groups === undefined
// ============================================================

TEST(RegExpInterp, RX36_MatchNonGlobalGroups) {
    {
        INTERP_OK("var m = 'hello world'.match(/world/); m.groups === undefined;", r);
        EXPECT_EQ(r.value().as_bool(), true);
    }
    {
        // 含捕获组的非全局 match，groups 仍为 undefined（无命名捕获组）
        INTERP_OK("var m = 'hello world'.match(/(world)/); m.groups === undefined;", r);
        EXPECT_EQ(r.value().as_bool(), true);
    }
}

TEST(RegExpVM, RX36_MatchNonGlobalGroups) {
    {
        VM_OK("var m = 'hello world'.match(/world/); m.groups === undefined;", r);
        EXPECT_EQ(r.value().as_bool(), true);
    }
    {
        VM_OK("var m = 'hello world'.match(/(world)/); m.groups === undefined;", r);
        EXPECT_EQ(r.value().as_bool(), true);
    }
}

// ============================================================
// RX-37: String.prototype.match 非 RegExp 参数 — 字符串参数用 new RegExp 包装
// ============================================================

TEST(RegExpInterp, RX37_MatchStringArg) {
    {
        // 'abc'.match('b') 等同于 'abc'.match(new RegExp('b'))
        INTERP_OK("var m = 'abc'.match('b'); m[0];", r);
        EXPECT_EQ(r.value().as_string(), "b");
    }
    {
        // 匹配 index
        INTERP_OK("var m = 'abc'.match('b'); m.index;", r);
        EXPECT_DOUBLE_EQ(r.value().as_number(), 1.0);
    }
    {
        // 无匹配返回 null
        INTERP_OK("'abc'.match('x') === null;", r);
        EXPECT_EQ(r.value().as_bool(), true);
    }
    {
        // 数字参数被转为字符串
        INTERP_OK("var m = 'abc123'.match(1); m[0];", r);
        EXPECT_EQ(r.value().as_string(), "1");
    }
}

TEST(RegExpVM, RX37_MatchStringArg) {
    {
        VM_OK("var m = 'abc'.match('b'); m[0];", r);
        EXPECT_EQ(r.value().as_string(), "b");
    }
    {
        VM_OK("var m = 'abc'.match('b'); m.index;", r);
        EXPECT_DOUBLE_EQ(r.value().as_number(), 1.0);
    }
    {
        VM_OK("'abc'.match('x') === null;", r);
        EXPECT_EQ(r.value().as_bool(), true);
    }
    {
        VM_OK("var m = 'abc123'.match(1); m[0];", r);
        EXPECT_EQ(r.value().as_string(), "1");
    }
}
