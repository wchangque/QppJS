// Top-Level Await (TLA) 测试
//
// 测试重点：
//   TLA-01: 基础顶层 await（await Promise.resolve(42)）
//   TLA-02: 顶层 await 后导出值正确
//   TLA-03: 多个顶层 await 串行
//   TLA-04: 依赖有 TLA 的模块（静态 import）
//   TLA-05: 顶层 await 错误传播
//   TLA-06: 顶层 await 后的 let 绑定可访问
//   TLA-07: await 非 Promise 值（原始值）
//   TLA-08: 入口模块自身有 TLA，最终结果正确
//   TLA-09: 非模块上下文中 await 仍是普通标识符
//   TLA-10: 模块内 await 语法解析成功

#include "qppjs/frontend/parser.h"
#include "qppjs/runtime/interpreter.h"
#include "qppjs/vm/vm.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace {

namespace fs = std::filesystem;

// ============================================================
// 临时目录 RAII 辅助（复用 module_test.cpp 中的模式）
// ============================================================

class TlaTempDir {
public:
    TlaTempDir() {
        auto base = fs::temp_directory_path();
        static int counter = 0;
        dir_ = base / ("qppjs_tla_test_" + std::to_string(getpid()) + "_" + std::to_string(counter++));
        fs::create_directories(dir_);
    }

    ~TlaTempDir() {
        fs::remove_all(dir_);
    }

    fs::path path() const { return dir_; }

    void write(const std::string& filename, const std::string& content) const {
        std::ofstream f(dir_ / filename);
        f << content;
    }

    std::string abs(const std::string& filename) const {
        return (dir_ / filename).string();
    }

private:
    fs::path dir_;
};

qppjs::EvalResult interp_exec_module(const std::string& entry_path) {
    qppjs::Interpreter interp;
    return interp.exec_module(entry_path);
}

qppjs::EvalResult vm_exec_module(const std::string& entry_path) {
    qppjs::VM vm;
    return vm.exec_module(entry_path);
}

// ============================================================
// TLA-01: 基础顶层 await
// ============================================================

TEST(InterpTla, TLA01_BasicTopLevelAwait) {
    TlaTempDir tmp;
    tmp.write("entry.js", R"(
import './empty.js';
let x = await Promise.resolve(42);
x;
)");
    tmp.write("empty.js", "");
    auto res = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_number(), 42.0);
}

TEST(VmTla, TLA01_BasicTopLevelAwait) {
    TlaTempDir tmp;
    tmp.write("entry.js", R"(
import './empty.js';
let x = await Promise.resolve(42);
x;
)");
    tmp.write("empty.js", "");
    auto res = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_number(), 42.0);
}

// ============================================================
// TLA-02: 顶层 await 后导出值正确
// ============================================================

TEST(InterpTla, TLA02_ExportAfterAwait) {
    TlaTempDir tmp;
    tmp.write("m.js", R"(
export let value = await Promise.resolve(99);
)");
    tmp.write("entry.js", R"(
import { value } from './m.js';
value;
)");
    auto res = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_number(), 99.0);
}

TEST(VmTla, TLA02_ExportAfterAwait) {
    TlaTempDir tmp;
    tmp.write("m.js", R"(
export let value = await Promise.resolve(99);
)");
    tmp.write("entry.js", R"(
import { value } from './m.js';
value;
)");
    auto res = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_number(), 99.0);
}

// ============================================================
// TLA-03: 多个顶层 await 串行
// ============================================================

TEST(InterpTla, TLA03_MultipleTopLevelAwaits) {
    TlaTempDir tmp;
    tmp.write("entry.js", R"(
import './empty.js';
let a = await Promise.resolve(1);
let b = await Promise.resolve(2);
let c = await Promise.resolve(3);
a + b + c;
)");
    tmp.write("empty.js", "");
    auto res = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_number(), 6.0);
}

TEST(VmTla, TLA03_MultipleTopLevelAwaits) {
    TlaTempDir tmp;
    tmp.write("entry.js", R"(
import './empty.js';
let a = await Promise.resolve(1);
let b = await Promise.resolve(2);
let c = await Promise.resolve(3);
a + b + c;
)");
    tmp.write("empty.js", "");
    auto res = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_number(), 6.0);
}

// ============================================================
// TLA-04: 依赖有 TLA 的模块
// ============================================================

TEST(InterpTla, TLA04_DependencyWithTLA) {
    TlaTempDir tmp;
    tmp.write("dep.js", R"(
export let result = await Promise.resolve(10);
)");
    tmp.write("entry.js", R"(
import { result } from './dep.js';
result * 2;
)");
    auto res = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_number(), 20.0);
}

TEST(VmTla, TLA04_DependencyWithTLA) {
    TlaTempDir tmp;
    tmp.write("dep.js", R"(
export let result = await Promise.resolve(10);
)");
    tmp.write("entry.js", R"(
import { result } from './dep.js';
result * 2;
)");
    auto res = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_number(), 20.0);
}

// ============================================================
// TLA-05: 顶层 await 错误传播
// ============================================================

TEST(InterpTla, TLA05_TopLevelAwaitErrorPropagation) {
    TlaTempDir tmp;
    tmp.write("entry.js", R"(
import './empty.js';
let x = await Promise.reject(new Error("tla error"));
x;
)");
    tmp.write("empty.js", "");
    auto res = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_FALSE(res.is_ok());
    EXPECT_NE(res.error().message().find("tla error"), std::string::npos);
}

TEST(VmTla, TLA05_TopLevelAwaitErrorPropagation) {
    TlaTempDir tmp;
    tmp.write("entry.js", R"(
import './empty.js';
let x = await Promise.reject(new Error("tla error"));
x;
)");
    tmp.write("empty.js", "");
    auto res = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_FALSE(res.is_ok());
    EXPECT_NE(res.error().message().find("tla error"), std::string::npos);
}

// ============================================================
// TLA-06: 顶层 await 后的 let 绑定可访问
// ============================================================

TEST(InterpTla, TLA06_BindingAccessibleAfterAwait) {
    TlaTempDir tmp;
    tmp.write("entry.js", R"(
import './empty.js';
let x = 0;
x = await Promise.resolve(7);
x;
)");
    tmp.write("empty.js", "");
    auto res = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_number(), 7.0);
}

TEST(VmTla, TLA06_BindingAccessibleAfterAwait) {
    TlaTempDir tmp;
    tmp.write("entry.js", R"(
import './empty.js';
let x = 0;
x = await Promise.resolve(7);
x;
)");
    tmp.write("empty.js", "");
    auto res = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_number(), 7.0);
}

// ============================================================
// TLA-07: await 非 Promise 值（原始值）
// ============================================================

TEST(InterpTla, TLA07_AwaitNonPromise) {
    TlaTempDir tmp;
    tmp.write("entry.js", R"(
import './empty.js';
let x = await 123;
x;
)");
    tmp.write("empty.js", "");
    auto res = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_number(), 123.0);
}

TEST(VmTla, TLA07_AwaitNonPromise) {
    TlaTempDir tmp;
    tmp.write("entry.js", R"(
import './empty.js';
let x = await 123;
x;
)");
    tmp.write("empty.js", "");
    auto res = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_number(), 123.0);
}

// ============================================================
// TLA-08: 入口模块自身有 TLA，最终结果正确
// ============================================================

TEST(InterpTla, TLA08_EntryModuleWithTLA) {
    TlaTempDir tmp;
    tmp.write("entry.js", R"(
import './empty.js';
let result = await Promise.resolve("hello");
result;
)");
    tmp.write("empty.js", "");
    auto res = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_string(), "hello");
}

TEST(VmTla, TLA08_EntryModuleWithTLA) {
    TlaTempDir tmp;
    tmp.write("entry.js", R"(
import './empty.js';
let result = await Promise.resolve("hello");
result;
)");
    tmp.write("empty.js", "");
    auto res = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_string(), "hello");
}

// ============================================================
// TLA-09: 非模块上下文中 await 是普通标识符
// ============================================================

TEST(InterpTla, TLA09_AwaitIsIdentifierInScript) {
    // 在普通 script 中 await 是普通标识符，不是关键字
    auto r = qppjs::parse_program("let await = 1; await;");
    ASSERT_TRUE(r.ok()) << r.error().message();
}

// ============================================================
// TLA-10: 模块内 await 语法解析成功
// ============================================================

TEST(InterpTla, TLA10_ModuleAwaitParseOk) {
    TlaTempDir tmp;
    tmp.write("entry.js", R"(
import './empty.js';
let x = await Promise.resolve(1);
)");
    tmp.write("empty.js", "");
    auto res = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
}

TEST(VmTla, TLA10_ModuleAwaitParseOk) {
    TlaTempDir tmp;
    tmp.write("entry.js", R"(
import './empty.js';
let x = await Promise.resolve(1);
)");
    tmp.write("empty.js", "");
    auto res = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
}

// ============================================================
// TLA-11: try/catch 捕获顶层 await 错误
// ============================================================

TEST(InterpTla, TLA11_TryCatchTopLevelAwait) {
    TlaTempDir tmp;
    tmp.write("entry.js", R"(
import './empty.js';
let caught = false;
try {
    await Promise.reject(new Error("boom"));
} catch (e) {
    caught = true;
}
caught;
)");
    tmp.write("empty.js", "");
    auto res = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_TRUE(res.value().as_bool());
}

TEST(VmTla, TLA11_TryCatchTopLevelAwait) {
    TlaTempDir tmp;
    tmp.write("entry.js", R"(
import './empty.js';
let caught = false;
try {
    await Promise.reject(new Error("boom"));
} catch (e) {
    caught = true;
}
caught;
)");
    tmp.write("empty.js", "");
    auto res = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_TRUE(res.value().as_bool());
}

// ============================================================
// TLA-12: 多模块依赖链中的顶层 await（链式依赖）
// ============================================================

TEST(InterpTla, TLA12_ChainedDependencyWithTLA) {
    TlaTempDir tmp;
    tmp.write("a.js", R"(
export let a = await Promise.resolve(5);
)");
    tmp.write("b.js", R"(
import { a } from './a.js';
export let b = a + (await Promise.resolve(3));
)");
    tmp.write("entry.js", R"(
import { b } from './b.js';
b;
)");
    auto res = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_number(), 8.0);
}

TEST(VmTla, TLA12_ChainedDependencyWithTLA) {
    TlaTempDir tmp;
    tmp.write("a.js", R"(
export let a = await Promise.resolve(5);
)");
    tmp.write("b.js", R"(
import { a } from './a.js';
export let b = a + (await Promise.resolve(3));
)");
    tmp.write("entry.js", R"(
import { b } from './b.js';
b;
)");
    auto res = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_number(), 8.0);
}

}  // namespace
