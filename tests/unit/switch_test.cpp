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

// ============================================================
// SW-01: 基础匹配 case，带 break
// ============================================================

TEST(Switch, SW01_BasicMatchInterp) {
    auto v = interp_ok(R"(
        let x = 2;
        let result = "none";
        switch (x) {
            case 1: result = "one"; break;
            case 2: result = "two"; break;
            case 3: result = "three"; break;
        }
        result
    )");
    EXPECT_EQ(v.sv(), "two");
}

TEST(Switch, SW01_BasicMatchVM) {
    auto v = vm_ok(R"(
        let x = 2;
        let result = "none";
        switch (x) {
            case 1: result = "one"; break;
            case 2: result = "two"; break;
            case 3: result = "three"; break;
        }
        result
    )");
    EXPECT_EQ(v.sv(), "two");
}

// ============================================================
// SW-02: fallthrough（无 break 继续执行下一个 case）
// ============================================================

TEST(Switch, SW02_FallthroughInterp) {
    auto v = interp_ok(R"(
        let x = 1;
        let result = 0;
        switch (x) {
            case 1: result = result + 1;
            case 2: result = result + 2;
            case 3: result = result + 4; break;
        }
        result
    )");
    EXPECT_EQ(v.as_number(), 7.0);
}

TEST(Switch, SW02_FallthroughVM) {
    auto v = vm_ok(R"(
        let x = 1;
        let result = 0;
        switch (x) {
            case 1: result = result + 1;
            case 2: result = result + 2;
            case 3: result = result + 4; break;
        }
        result
    )");
    EXPECT_EQ(v.as_number(), 7.0);
}

// ============================================================
// SW-03: break 阻止 fallthrough
// ============================================================

TEST(Switch, SW03_BreakStopsFallthroughInterp) {
    auto v = interp_ok(R"(
        let x = 1;
        let result = 0;
        switch (x) {
            case 1: result = 10; break;
            case 2: result = 20; break;
        }
        result
    )");
    EXPECT_EQ(v.as_number(), 10.0);
}

TEST(Switch, SW03_BreakStopsFallthroughVM) {
    auto v = vm_ok(R"(
        let x = 1;
        let result = 0;
        switch (x) {
            case 1: result = 10; break;
            case 2: result = 20; break;
        }
        result
    )");
    EXPECT_EQ(v.as_number(), 10.0);
}

// ============================================================
// SW-04: default 执行（无匹配）
// ============================================================

TEST(Switch, SW04_DefaultNoMatchInterp) {
    auto v = interp_ok(R"(
        let x = 99;
        let result = "none";
        switch (x) {
            case 1: result = "one"; break;
            case 2: result = "two"; break;
            default: result = "default"; break;
        }
        result
    )");
    EXPECT_EQ(v.sv(), "default");
}

TEST(Switch, SW04_DefaultNoMatchVM) {
    auto v = vm_ok(R"(
        let x = 99;
        let result = "none";
        switch (x) {
            case 1: result = "one"; break;
            case 2: result = "two"; break;
            default: result = "default"; break;
        }
        result
    )");
    EXPECT_EQ(v.sv(), "default");
}

// ============================================================
// SW-05: default 在中间（case 无匹配时跳到 default）
// ============================================================

TEST(Switch, SW05_DefaultInMiddleInterp) {
    auto v = interp_ok(R"(
        let x = 99;
        let result = "none";
        switch (x) {
            case 1: result = "one"; break;
            default: result = "default"; break;
            case 2: result = "two"; break;
        }
        result
    )");
    EXPECT_EQ(v.sv(), "default");
}

TEST(Switch, SW05_DefaultInMiddleVM) {
    auto v = vm_ok(R"(
        let x = 99;
        let result = "none";
        switch (x) {
            case 1: result = "one"; break;
            default: result = "default"; break;
            case 2: result = "two"; break;
        }
        result
    )");
    EXPECT_EQ(v.sv(), "default");
}

// ============================================================
// SW-06: switch 中 return
// ============================================================

TEST(Switch, SW06_ReturnInSwitchInterp) {
    auto v = interp_ok(R"(
        function f(x) {
            switch (x) {
                case 1: return "one";
                case 2: return "two";
                default: return "other";
            }
        }
        f(2)
    )");
    EXPECT_EQ(v.sv(), "two");
}

TEST(Switch, SW06_ReturnInSwitchVM) {
    auto v = vm_ok(R"(
        function f(x) {
            switch (x) {
                case 1: return "one";
                case 2: return "two";
                default: return "other";
            }
        }
        f(2)
    )");
    EXPECT_EQ(v.sv(), "two");
}

// ============================================================
// SW-07: 嵌套 switch，inner break 不影响 outer
// ============================================================

TEST(Switch, SW07_NestedSwitchInterp) {
    auto v = interp_ok(R"(
        let outer = 1;
        let inner = 2;
        let result = "";
        switch (outer) {
            case 1:
                result += "outer1-";
                switch (inner) {
                    case 2: result += "inner2"; break;
                    case 3: result += "inner3"; break;
                }
                result += "-after";
                break;
            case 2:
                result += "outer2";
                break;
        }
        result
    )");
    EXPECT_EQ(v.sv(), "outer1-inner2-after");
}

TEST(Switch, SW07_NestedSwitchVM) {
    auto v = vm_ok(R"(
        let outer = 1;
        let inner = 2;
        let result = "";
        switch (outer) {
            case 1:
                result += "outer1-";
                switch (inner) {
                    case 2: result += "inner2"; break;
                    case 3: result += "inner3"; break;
                }
                result += "-after";
                break;
            case 2:
                result += "outer2";
                break;
        }
        result
    )");
    EXPECT_EQ(v.sv(), "outer1-inner2-after");
}

// ============================================================
// SW-08: switch 中 continue 穿透到外层 for
// ============================================================

TEST(Switch, SW08_ContinueToOuterForInterp) {
    auto v = interp_ok(R"(
        let result = 0;
        for (let i = 0; i < 3; i++) {
            switch (i) {
                case 1: continue;
            }
            result += 1;
        }
        result
    )");
    EXPECT_EQ(v.as_number(), 2.0);
}

TEST(Switch, SW08_ContinueToOuterForVM) {
    auto v = vm_ok(R"(
        let result = 0;
        for (let i = 0; i < 3; i++) {
            switch (i) {
                case 1: continue;
            }
            result += 1;
        }
        result
    )");
    EXPECT_EQ(v.as_number(), 2.0);
}

// ============================================================
// SW-09: 空 case body（连续 case，共享后续代码）
// ============================================================

TEST(Switch, SW09_EmptyCaseBodyInterp) {
    auto v = interp_ok(R"(
        let x = 1;
        let result = "none";
        switch (x) {
            case 1:
            case 2:
                result = "one-or-two";
                break;
            case 3:
                result = "three";
                break;
        }
        result
    )");
    EXPECT_EQ(v.sv(), "one-or-two");
}

TEST(Switch, SW09_EmptyCaseBodyVM) {
    auto v = vm_ok(R"(
        let x = 1;
        let result = "none";
        switch (x) {
            case 1:
            case 2:
                result = "one-or-two";
                break;
            case 3:
                result = "three";
                break;
        }
        result
    )");
    EXPECT_EQ(v.sv(), "one-or-two");
}

// ============================================================
// SW-10: 字符串 case 值
// ============================================================

TEST(Switch, SW10_StringCaseInterp) {
    auto v = interp_ok(R"(
        let x = "hello";
        let result = "none";
        switch (x) {
            case "world": result = "world"; break;
            case "hello": result = "hello"; break;
            default: result = "other"; break;
        }
        result
    )");
    EXPECT_EQ(v.sv(), "hello");
}

TEST(Switch, SW10_StringCaseVM) {
    auto v = vm_ok(R"(
        let x = "hello";
        let result = "none";
        switch (x) {
            case "world": result = "world"; break;
            case "hello": result = "hello"; break;
            default: result = "other"; break;
        }
        result
    )");
    EXPECT_EQ(v.sv(), "hello");
}

// ============================================================
// SW-11: 无匹配无 default → 正常不执行任何 case
// ============================================================

TEST(Switch, SW11_NoMatchNoDefaultInterp) {
    auto v = interp_ok(R"(
        let x = 99;
        let result = "none";
        switch (x) {
            case 1: result = "one"; break;
            case 2: result = "two"; break;
        }
        result
    )");
    EXPECT_EQ(v.sv(), "none");
}

TEST(Switch, SW11_NoMatchNoDefaultVM) {
    auto v = vm_ok(R"(
        let x = 99;
        let result = "none";
        switch (x) {
            case 1: result = "one"; break;
            case 2: result = "two"; break;
        }
        result
    )");
    EXPECT_EQ(v.sv(), "none");
}

// ============================================================
// SW-12: 严格相等（1 不匹配 "1"）
// ============================================================

TEST(Switch, SW12_StrictEqualityInterp) {
    auto v = interp_ok(R"(
        let x = 1;
        let result = "none";
        switch (x) {
            case "1": result = "string"; break;
            case 1: result = "number"; break;
        }
        result
    )");
    EXPECT_EQ(v.sv(), "number");
}

TEST(Switch, SW12_StrictEqualityVM) {
    auto v = vm_ok(R"(
        let x = 1;
        let result = "none";
        switch (x) {
            case "1": result = "string"; break;
            case 1: result = "number"; break;
        }
        result
    )");
    EXPECT_EQ(v.sv(), "number");
}

}  // namespace
