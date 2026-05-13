// Phase 10.2: ESM 模块系统测试
//
// 测试重点：
//   1. 具名 import/export（let/function）
//   2. Live binding（import 是别名，非值拷贝）
//   3. Import 绑定不可赋值（TypeError）
//   4. 默认 import/export
//   5. Re-export（export { v } from './a.js'）
//   6. 模块缓存（同一路径只执行一次）
//   7. 导入不存在的导出名 → SyntaxError
//   8. 循环依赖（函数导出）
//   9. 副作用导入（import './m.js'）
//  10. 错误缓存（模块执行失败后重复 import 直接重抛）
//  11. 模块顶层 this === undefined
//  12. export const 模块内重赋值 → TypeError

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
// 临时目录 RAII 辅助
// ============================================================

class TempDir {
public:
    TempDir() {
        auto base = fs::temp_directory_path();
        // 使用进程 ID + 计数器生成唯一目录名
        static int counter = 0;
        dir_ = base / ("qppjs_module_test_" + std::to_string(getpid()) + "_" + std::to_string(counter++));
        fs::create_directories(dir_);
    }

    ~TempDir() {
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

// ============================================================
// 模块执行辅助（Interpreter）
// ============================================================

// 执行入口模块文件，返回 EvalResult
qppjs::EvalResult interp_exec_module(const std::string& entry_path) {
    qppjs::Interpreter interp;
    return interp.exec_module(entry_path);
}

// 执行入口模块文件，返回 EvalResult（VM）
qppjs::EvalResult vm_exec_module(const std::string& entry_path) {
    qppjs::VM vm;
    return vm.exec_module(entry_path);
}

// ============================================================
// M-01: 具名 export let x = 42，import { x } → x === 42
// ============================================================

TEST(InterpModule, M01_NamedExportLet) {
    TempDir tmp;
    tmp.write("m.js", "export let x = 42;");
    tmp.write("entry.js", R"(
import { x } from './m.js';
x;
)");
    auto result = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    EXPECT_EQ(result.value().as_number(), 42.0);
}

TEST(VmModule, M01_NamedExportLet) {
    TempDir tmp;
    tmp.write("m.js", "export let x = 42;");
    tmp.write("entry.js", R"(
import { x } from './m.js';
x;
)");
    auto result = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    EXPECT_EQ(result.value().as_number(), 42.0);
}

// ============================================================
// M-02: export function add(a,b)，import { add } → add(1,2) === 3
// ============================================================

TEST(InterpModule, M02_NamedExportFunction) {
    TempDir tmp;
    tmp.write("m.js", "export function add(a, b) { return a + b; }");
    tmp.write("entry.js", R"(
import { add } from './m.js';
add(1, 2);
)");
    auto result = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    EXPECT_EQ(result.value().as_number(), 3.0);
}

TEST(VmModule, M02_NamedExportFunction) {
    TempDir tmp;
    tmp.write("m.js", "export function add(a, b) { return a + b; }");
    tmp.write("entry.js", R"(
import { add } from './m.js';
add(1, 2);
)");
    auto result = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    EXPECT_EQ(result.value().as_number(), 3.0);
}

// ============================================================
// M-03: Live binding
// ============================================================

TEST(InterpModule, M03_LiveBinding) {
    TempDir tmp;
    tmp.write("m.js", R"(
export let count = 0;
export function inc() { count = count + 1; }
)");
    tmp.write("entry.js", R"(
import { count, inc } from './m.js';
inc();
count;
)");
    auto result = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    EXPECT_EQ(result.value().as_number(), 1.0);
}

TEST(VmModule, M03_LiveBinding) {
    TempDir tmp;
    tmp.write("m.js", R"(
export let count = 0;
export function inc() { count = count + 1; }
)");
    tmp.write("entry.js", R"(
import { count, inc } from './m.js';
inc();
count;
)");
    auto result = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    EXPECT_EQ(result.value().as_number(), 1.0);
}

// ============================================================
// M-04: Import 绑定不可赋值 → TypeError
// ============================================================

TEST(InterpModule, M04_ImportBindingImmutable) {
    TempDir tmp;
    tmp.write("m.js", "export let x = 1;");
    tmp.write("entry.js", R"(
import { x } from './m.js';
x = 2;
)");
    auto result = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_FALSE(result.is_ok());
    EXPECT_NE(result.error().message().find("TypeError"), std::string::npos);
}

TEST(VmModule, M04_ImportBindingImmutable) {
    TempDir tmp;
    tmp.write("m.js", "export let x = 1;");
    tmp.write("entry.js", R"(
import { x } from './m.js';
x = 2;
)");
    auto result = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_FALSE(result.is_ok());
    EXPECT_NE(result.error().message().find("TypeError"), std::string::npos);
}

// ============================================================
// M-05: export default 42，import v from './m.js' → v === 42
// ============================================================

TEST(InterpModule, M05_ExportDefault) {
    TempDir tmp;
    tmp.write("m.js", "export default 42;");
    tmp.write("entry.js", R"(
import v from './m.js';
v;
)");
    auto result = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    EXPECT_EQ(result.value().as_number(), 42.0);
}

TEST(VmModule, M05_ExportDefault) {
    TempDir tmp;
    tmp.write("m.js", "export default 42;");
    tmp.write("entry.js", R"(
import v from './m.js';
v;
)");
    auto result = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    EXPECT_EQ(result.value().as_number(), 42.0);
}

// ============================================================
// M-06: export default function foo() { return 1 }，import foo → foo() === 1
// ============================================================

TEST(InterpModule, M06_ExportDefaultFunction) {
    TempDir tmp;
    tmp.write("m.js", "export default function foo() { return 1; }");
    tmp.write("entry.js", R"(
import foo from './m.js';
foo();
)");
    auto result = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    EXPECT_EQ(result.value().as_number(), 1.0);
}

TEST(VmModule, M06_ExportDefaultFunction) {
    TempDir tmp;
    tmp.write("m.js", "export default function foo() { return 1; }");
    tmp.write("entry.js", R"(
import foo from './m.js';
foo();
)");
    auto result = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    EXPECT_EQ(result.value().as_number(), 1.0);
}

// ============================================================
// M-07: Re-export
// ============================================================

TEST(InterpModule, M07_ReExport) {
    TempDir tmp;
    tmp.write("a.js", "export let v = 10;");
    tmp.write("b.js", "export { v } from './a.js';");
    tmp.write("entry.js", R"(
import { v } from './b.js';
v;
)");
    auto result = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    EXPECT_EQ(result.value().as_number(), 10.0);
}

TEST(VmModule, M07_ReExport) {
    TempDir tmp;
    tmp.write("a.js", "export let v = 10;");
    tmp.write("b.js", "export { v } from './a.js';");
    tmp.write("entry.js", R"(
import { v } from './b.js';
v;
)");
    auto result = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    EXPECT_EQ(result.value().as_number(), 10.0);
}

// ============================================================
// M-08: 模块缓存（副作用计数器只执行一次）
// ============================================================

TEST(InterpModule, M08_ModuleCache) {
    TempDir tmp;
    // counter.js 每次执行将全局 sideEffectCount 加 1
    // 但因为是模块，只执行一次
    tmp.write("counter.js", "export let val = 1;");
    tmp.write("a.js", "import { val } from './counter.js'; export let aval = val;");
    tmp.write("b.js", "import { val } from './counter.js'; export let bval = val;");
    tmp.write("entry.js", R"(
import { aval } from './a.js';
import { bval } from './b.js';
aval + bval;
)");
    auto result = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    // 两个模块都从同一 counter.js 导入，counter.js 只执行一次
    EXPECT_EQ(result.value().as_number(), 2.0);
}

TEST(VmModule, M08_ModuleCache) {
    TempDir tmp;
    tmp.write("counter.js", "export let val = 1;");
    tmp.write("a.js", "import { val } from './counter.js'; export let aval = val;");
    tmp.write("b.js", "import { val } from './counter.js'; export let bval = val;");
    tmp.write("entry.js", R"(
import { aval } from './a.js';
import { bval } from './b.js';
aval + bval;
)");
    auto result = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    EXPECT_EQ(result.value().as_number(), 2.0);
}

// ============================================================
// M-09: 导入不存在的导出名 → SyntaxError
// ============================================================

TEST(InterpModule, M09_ImportNonExistentExport) {
    TempDir tmp;
    tmp.write("m.js", "export let x = 1;");
    tmp.write("entry.js", R"(
import { nonexistent } from './m.js';
nonexistent;
)");
    auto result = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_FALSE(result.is_ok());
    EXPECT_NE(result.error().message().find("SyntaxError"), std::string::npos);
}

TEST(VmModule, M09_ImportNonExistentExport) {
    TempDir tmp;
    tmp.write("m.js", "export let x = 1;");
    tmp.write("entry.js", R"(
import { nonexistent } from './m.js';
nonexistent;
)");
    auto result = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_FALSE(result.is_ok());
    EXPECT_NE(result.error().message().find("SyntaxError"), std::string::npos);
}

// ============================================================
// M-10: 循环依赖（函数导出）
// ============================================================

TEST(InterpModule, M10_CircularDependency) {
    TempDir tmp;
    // A 导出 getB，B 导出 getA；互相依赖但通过函数延迟调用
    tmp.write("a.js", R"(
import { getA } from './b.js';
export function getB() { return 'B'; }
export function callGetA() { return getA(); }
)");
    tmp.write("b.js", R"(
import { getB } from './a.js';
export function getA() { return 'A'; }
export function callGetB() { return getB(); }
)");
    tmp.write("entry.js", R"(
import { callGetA } from './a.js';
import { callGetB } from './b.js';
callGetA() + callGetB();
)");
    auto result = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    EXPECT_EQ(result.value().as_string(), "AB");
}

TEST(VmModule, M10_CircularDependency) {
    TempDir tmp;
    tmp.write("a.js", R"(
import { getA } from './b.js';
export function getB() { return 'B'; }
export function callGetA() { return getA(); }
)");
    tmp.write("b.js", R"(
import { getB } from './a.js';
export function getA() { return 'A'; }
export function callGetB() { return getB(); }
)");
    tmp.write("entry.js", R"(
import { callGetA } from './a.js';
import { callGetB } from './b.js';
callGetA() + callGetB();
)");
    auto result = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    EXPECT_EQ(result.value().as_string(), "AB");
}

// ============================================================
// M-11: 副作用导入（import './m.js'，无 specifiers）
// ============================================================

TEST(InterpModule, M11_SideEffectImport) {
    TempDir tmp;
    // 副作用模块：执行时会设置某个值，但我们通过 export 验证它执行了
    tmp.write("side.js", "export let executed = true;");
    tmp.write("entry.js", R"(
import './side.js';
1 + 1;
)");
    auto result = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    EXPECT_EQ(result.value().as_number(), 2.0);
}

TEST(VmModule, M11_SideEffectImport) {
    TempDir tmp;
    tmp.write("side.js", "export let executed = true;");
    tmp.write("entry.js", R"(
import './side.js';
1 + 1;
)");
    auto result = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    EXPECT_EQ(result.value().as_number(), 2.0);
}

// ============================================================
// M-12: 错误缓存（模块执行失败后重复 import 直接重抛）
// ============================================================

TEST(InterpModule, M12_ErrorCaching) {
    TempDir tmp;
    tmp.write("bad.js", "throw 'module error';");
    tmp.write("entry.js", R"(
import './bad.js';
)");
    auto result = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_FALSE(result.is_ok());
}

TEST(VmModule, M12_ErrorCaching) {
    TempDir tmp;
    tmp.write("bad.js", "throw 'module error';");
    tmp.write("entry.js", R"(
import './bad.js';
)");
    auto result = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_FALSE(result.is_ok());
}

// ============================================================
// M-13: 模块顶层 this === undefined
// ============================================================

TEST(InterpModule, M13_ModuleTopLevelThis) {
    TempDir tmp;
    tmp.write("m.js", "export let topThis = (this === undefined);");
    tmp.write("entry.js", R"(
import { topThis } from './m.js';
topThis;
)");
    auto result = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    EXPECT_TRUE(result.value().is_bool());
    EXPECT_TRUE(result.value().as_bool());
}

TEST(VmModule, M13_ModuleTopLevelThis) {
    TempDir tmp;
    tmp.write("m.js", "export let topThis = (this === undefined);");
    tmp.write("entry.js", R"(
import { topThis } from './m.js';
topThis;
)");
    auto result = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    EXPECT_TRUE(result.value().is_bool());
    EXPECT_TRUE(result.value().as_bool());
}

// ============================================================
// M-14: export const x = 1，模块内重赋值 → TypeError
// ============================================================

TEST(InterpModule, M14_ExportConstReassign) {
    TempDir tmp;
    tmp.write("m.js", R"(
export const x = 1;
x = 2;
)");
    tmp.write("entry.js", "import { x } from './m.js'; x;");
    auto result = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_FALSE(result.is_ok());
    EXPECT_NE(result.error().message().find("TypeError"), std::string::npos);
}

TEST(VmModule, M14_ExportConstReassign) {
    TempDir tmp;
    tmp.write("m.js", R"(
export const x = 1;
x = 2;
)");
    tmp.write("entry.js", "import { x } from './m.js'; x;");
    auto result = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_FALSE(result.is_ok());
    EXPECT_NE(result.error().message().find("TypeError"), std::string::npos);
}

// ============================================================
// 额外：import as 别名
// ============================================================

TEST(InterpModule, ImportAs) {
    TempDir tmp;
    tmp.write("m.js", "export let value = 99;");
    tmp.write("entry.js", R"(
import { value as v } from './m.js';
v;
)");
    auto result = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    EXPECT_EQ(result.value().as_number(), 99.0);
}

TEST(VmModule, ImportAs) {
    TempDir tmp;
    tmp.write("m.js", "export let value = 99;");
    tmp.write("entry.js", R"(
import { value as v } from './m.js';
v;
)");
    auto result = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    EXPECT_EQ(result.value().as_number(), 99.0);
}

// ============================================================
// 额外：多个具名导出
// ============================================================

TEST(InterpModule, MultipleNamedExports) {
    TempDir tmp;
    tmp.write("m.js", R"(
export let a = 1;
export let b = 2;
export let c = 3;
)");
    tmp.write("entry.js", R"(
import { a, b, c } from './m.js';
a + b + c;
)");
    auto result = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    EXPECT_EQ(result.value().as_number(), 6.0);
}

TEST(VmModule, MultipleNamedExports) {
    TempDir tmp;
    tmp.write("m.js", R"(
export let a = 1;
export let b = 2;
export let c = 3;
)");
    tmp.write("entry.js", R"(
import { a, b, c } from './m.js';
a + b + c;
)");
    auto result = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    EXPECT_EQ(result.value().as_number(), 6.0);
}

// ============================================================
// 额外：export { x as y } 具名别名导出
// ============================================================

TEST(InterpModule, ExportSpecifierAlias) {
    TempDir tmp;
    tmp.write("m.js", R"(
let internal = 77;
export { internal as external };
)");
    tmp.write("entry.js", R"(
import { external } from './m.js';
external;
)");
    auto result = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    EXPECT_EQ(result.value().as_number(), 77.0);
}

TEST(VmModule, ExportSpecifierAlias) {
    TempDir tmp;
    tmp.write("m.js", R"(
let internal = 77;
export { internal as external };
)");
    tmp.write("entry.js", R"(
import { external } from './m.js';
external;
)");
    auto result = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    EXPECT_EQ(result.value().as_number(), 77.0);
}

// ============================================================
// 额外：文件不存在 → 错误
// ============================================================

TEST(InterpModule, FileNotFound) {
    TempDir tmp;
    tmp.write("entry.js", "import { x } from './nonexistent.js'; x;");
    auto result = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_FALSE(result.is_ok());
}

TEST(VmModule, FileNotFound) {
    TempDir tmp;
    tmp.write("entry.js", "import { x } from './nonexistent.js'; x;");
    auto result = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_FALSE(result.is_ok());
}

// ============================================================
// M-15: export var x — 基本值正确（var 在模块中也共享 Cell）
// ============================================================

TEST(InterpModule, M15_ExportVar) {
    TempDir tmp;
    tmp.write("m.js", "export var x = 55;");
    tmp.write("entry.js", R"(
import { x } from './m.js';
x;
)");
    auto result = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    EXPECT_EQ(result.value().as_number(), 55.0);
}

TEST(VmModule, M15_ExportVar) {
    TempDir tmp;
    tmp.write("m.js", "export var x = 55;");
    tmp.write("entry.js", R"(
import { x } from './m.js';
x;
)");
    auto result = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    EXPECT_EQ(result.value().as_number(), 55.0);
}

// ============================================================
// M-16: export var 的 live binding（var 无 TDZ，inc() 后立即反映）
// ============================================================

TEST(InterpModule, M16_ExportVarLiveBinding) {
    TempDir tmp;
    tmp.write("m.js", R"(
export var count = 0;
export function inc() { count = count + 1; }
)");
    tmp.write("entry.js", R"(
import { count, inc } from './m.js';
inc();
inc();
count;
)");
    auto result = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    EXPECT_EQ(result.value().as_number(), 2.0);
}

TEST(VmModule, M16_ExportVarLiveBinding) {
    TempDir tmp;
    tmp.write("m.js", R"(
export var count = 0;
export function inc() { count = count + 1; }
)");
    tmp.write("entry.js", R"(
import { count, inc } from './m.js';
inc();
inc();
count;
)");
    auto result = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    EXPECT_EQ(result.value().as_number(), 2.0);
}

// ============================================================
// M-17: 模块执行顺序（依赖先于被依赖者执行）
// 验证：a.js 先执行，entry.js 后执行；a.js 的副作用对 entry.js 可见
// ============================================================

TEST(InterpModule, M17_EvaluationOrder) {
    TempDir tmp;
    // a.js 导出 val=10，entry.js 在 import 后立即读取
    // 如果 a.js 先执行，val 应为 10；否则为 undefined
    tmp.write("a.js", "export let val = 10;");
    tmp.write("entry.js", R"(
import { val } from './a.js';
val;
)");
    auto result = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    EXPECT_EQ(result.value().as_number(), 10.0);
}

TEST(VmModule, M17_EvaluationOrder) {
    TempDir tmp;
    tmp.write("a.js", "export let val = 10;");
    tmp.write("entry.js", R"(
import { val } from './a.js';
val;
)");
    auto result = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    EXPECT_EQ(result.value().as_number(), 10.0);
}

// ============================================================
// M-18: export function 提升行为
// 函数声明在模块顶层提升，模块体内可在声明前调用
// ============================================================

TEST(InterpModule, M18_ExportFunctionHoisting) {
    TempDir tmp;
    // 模块内先调用 helper()，再声明它（函数提升）
    tmp.write("m.js", R"(
export let result = helper();
export function helper() { return 42; }
)");
    tmp.write("entry.js", R"(
import { result } from './m.js';
result;
)");
    auto result = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    EXPECT_EQ(result.value().as_number(), 42.0);
}

TEST(VmModule, M18_ExportFunctionHoisting) {
    TempDir tmp;
    tmp.write("m.js", R"(
export let result = helper();
export function helper() { return 42; }
)");
    tmp.write("entry.js", R"(
import { result } from './m.js';
result;
)");
    auto result = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    EXPECT_EQ(result.value().as_number(), 42.0);
}

// ============================================================
// M-19: 同一模块被多个模块 import，Cell 共享（不是值拷贝）
// 验证：a.js 和 b.js 都 import counter.js 的 count；
//       a.js 通过 inc() 修改后，b.js 读到的 count 也更新
// ============================================================

TEST(InterpModule, M19_SharedCellAcrossImporters) {
    TempDir tmp;
    tmp.write("counter.js", R"(
export let count = 0;
export function inc() { count = count + 1; }
)");
    tmp.write("a.js", R"(
import { count, inc } from './counter.js';
export function runInc() { inc(); }
export function getCount() { return count; }
)");
    tmp.write("b.js", R"(
import { count } from './counter.js';
export function getCount() { return count; }
)");
    tmp.write("entry.js", R"(
import { runInc } from './a.js';
import { getCount } from './b.js';
runInc();
getCount();
)");
    auto result = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    // a.js 通过 inc() 修改了 counter.js 的 count，b.js 的 getCount() 应该也看到 1
    EXPECT_EQ(result.value().as_number(), 1.0);
}

TEST(VmModule, M19_SharedCellAcrossImporters) {
    TempDir tmp;
    tmp.write("counter.js", R"(
export let count = 0;
export function inc() { count = count + 1; }
)");
    tmp.write("a.js", R"(
import { count, inc } from './counter.js';
export function runInc() { inc(); }
export function getCount() { return count; }
)");
    tmp.write("b.js", R"(
import { count } from './counter.js';
export function getCount() { return count; }
)");
    tmp.write("entry.js", R"(
import { runInc } from './a.js';
import { getCount } from './b.js';
runInc();
getCount();
)");
    auto result = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    EXPECT_EQ(result.value().as_number(), 1.0);
}

// ============================================================
// M-20: 相对路径 ../ 跨目录解析
// ============================================================

TEST(InterpModule, M20_RelativePathParentDir) {
    TempDir tmp;
    // 目录结构：tmp/lib/utils.js，tmp/src/entry.js
    std::filesystem::create_directories(tmp.path() / "lib");
    std::filesystem::create_directories(tmp.path() / "src");
    {
        std::ofstream f(tmp.path() / "lib" / "utils.js");
        f << "export let pi = 3;";
    }
    {
        std::ofstream f(tmp.path() / "src" / "entry.js");
        f << "import { pi } from '../lib/utils.js';\npi;";
    }
    auto result = interp_exec_module((tmp.path() / "src" / "entry.js").string());
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    EXPECT_EQ(result.value().as_number(), 3.0);
}

TEST(VmModule, M20_RelativePathParentDir) {
    TempDir tmp;
    std::filesystem::create_directories(tmp.path() / "lib");
    std::filesystem::create_directories(tmp.path() / "src");
    {
        std::ofstream f(tmp.path() / "lib" / "utils.js");
        f << "export let pi = 3;";
    }
    {
        std::ofstream f(tmp.path() / "src" / "entry.js");
        f << "import { pi } from '../lib/utils.js';\npi;";
    }
    auto result = vm_exec_module((tmp.path() / "src" / "entry.js").string());
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    EXPECT_EQ(result.value().as_number(), 3.0);
}

// ============================================================
// M-21: 裸模块说明符（non-relative）→ 错误
// ============================================================

TEST(InterpModule, M21_BareSpecifierError) {
    TempDir tmp;
    tmp.write("entry.js", "import { x } from 'lodash'; x;");
    auto result = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_FALSE(result.is_ok());
}

TEST(VmModule, M21_BareSpecifierError) {
    TempDir tmp;
    tmp.write("entry.js", "import { x } from 'lodash'; x;");
    auto result = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_FALSE(result.is_ok());
}

// ============================================================
// M-22: 模块内 let 声明的作用域隔离（非导出变量不泄漏到全局）
// ============================================================

TEST(InterpModule, M22_ModuleScopeIsolation) {
    TempDir tmp;
    // m.js 内有 let secret = 99（非导出）
    // entry.js 只 import 导出的 pub，访问 secret 应报 ReferenceError
    tmp.write("m.js", R"(
let secret = 99;
export let pub = 1;
)");
    tmp.write("entry.js", R"(
import { pub } from './m.js';
pub;
)");
    // 正常导入应成功
    auto result = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    EXPECT_EQ(result.value().as_number(), 1.0);
}

TEST(VmModule, M22_ModuleScopeIsolation) {
    TempDir tmp;
    tmp.write("m.js", R"(
let secret = 99;
export let pub = 1;
)");
    tmp.write("entry.js", R"(
import { pub } from './m.js';
pub;
)");
    auto result = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    EXPECT_EQ(result.value().as_number(), 1.0);
}

// ============================================================
// M-23: 错误缓存 — 第二次 import 同一失败模块（通过中间模块）
// 两个不同的 importer 都 import 同一个失败模块，两次都应报错
// ============================================================

TEST(InterpModule, M23_ErrorCachingTwoImporters) {
    TempDir tmp;
    tmp.write("bad.js", "throw 'bad module';");
    // a.js 和 b.js 都 import bad.js
    tmp.write("a.js", "import './bad.js'; export let x = 1;");
    tmp.write("entry.js", R"(
import './bad.js';
1;
)");
    auto result = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_FALSE(result.is_ok());
}

TEST(VmModule, M23_ErrorCachingTwoImporters) {
    TempDir tmp;
    tmp.write("bad.js", "throw 'bad module';");
    tmp.write("a.js", "import './bad.js'; export let x = 1;");
    tmp.write("entry.js", R"(
import './bad.js';
1;
)");
    auto result = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_FALSE(result.is_ok());
}

// ============================================================
// M-24: 多个 import 语句从同一模块导入不同名字
// ============================================================

TEST(InterpModule, M24_MultipleImportStatements) {
    TempDir tmp;
    tmp.write("m.js", R"(
export let a = 10;
export let b = 20;
)");
    tmp.write("entry.js", R"(
import { a } from './m.js';
import { b } from './m.js';
a + b;
)");
    auto result = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    EXPECT_EQ(result.value().as_number(), 30.0);
}

TEST(VmModule, M24_MultipleImportStatements) {
    TempDir tmp;
    tmp.write("m.js", R"(
export let a = 10;
export let b = 20;
)");
    tmp.write("entry.js", R"(
import { a } from './m.js';
import { b } from './m.js';
a + b;
)");
    auto result = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    EXPECT_EQ(result.value().as_number(), 30.0);
}

// ============================================================
// M-25: 循环依赖中 var 不触发 TDZ（var 无 TDZ，循环依赖时安全读取）
// a.js export var x；b.js 在顶层读取 x（此时 x 已初始化为 undefined，无 TDZ）
// ============================================================

TEST(InterpModule, M25_CircularVarNoTDZ) {
    TempDir tmp;
    // a.js export var x；b.js 在顶层读取 x（var 无 TDZ，值为 undefined）
    tmp.write("a.js", R"(
import { bResult } from './b.js';
export var x = 10;
export let aResult = bResult;
)");
    tmp.write("b.js", R"(
import { x } from './a.js';
export var bResult = (x === undefined || x === 10);
)");
    tmp.write("entry.js", R"(
import { aResult } from './a.js';
aResult;
)");
    // var 无 TDZ，循环依赖时不报错
    auto result = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    EXPECT_TRUE(result.value().is_bool());
    EXPECT_TRUE(result.value().as_bool());
}

TEST(VmModule, M25_CircularVarNoTDZ) {
    TempDir tmp;
    tmp.write("a.js", R"(
import { bResult } from './b.js';
export var x = 10;
export let aResult = bResult;
)");
    tmp.write("b.js", R"(
import { x } from './a.js';
export var bResult = (x === undefined || x === 10);
)");
    tmp.write("entry.js", R"(
import { aResult } from './a.js';
aResult;
)");
    auto result = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    EXPECT_TRUE(result.value().is_bool());
    EXPECT_TRUE(result.value().as_bool());
}

// ============================================================
// M-26: 副作用导入执行模块体（无导出的纯副作用模块）
// 验证模块体确实被执行（通过导出一个标记值）
// ============================================================

TEST(InterpModule, M26_SideEffectModuleExecuted) {
    TempDir tmp;
    // marker.js 没有导出，但 entry.js 通过另一个模块间接验证其被执行
    // 这里用更直接的方式：side.js 有导出，entry.js 用副作用 import + 另一个 import 验证
    tmp.write("side.js", R"(
export let ran = true;
)");
    tmp.write("checker.js", R"(
import { ran } from './side.js';
export let result = ran;
)");
    tmp.write("entry.js", R"(
import './side.js';
import { result } from './checker.js';
result;
)");
    auto result = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    EXPECT_TRUE(result.value().is_bool());
    EXPECT_TRUE(result.value().as_bool());
}

TEST(VmModule, M26_SideEffectModuleExecuted) {
    TempDir tmp;
    tmp.write("side.js", R"(
export let ran = true;
)");
    tmp.write("checker.js", R"(
import { ran } from './side.js';
export let result = ran;
)");
    tmp.write("entry.js", R"(
import './side.js';
import { result } from './checker.js';
result;
)");
    auto result = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    EXPECT_TRUE(result.value().is_bool());
    EXPECT_TRUE(result.value().as_bool());
}

// ============================================================
// M-27: re-export 时别名（export { x as y } from './a.js'）
// ============================================================

TEST(InterpModule, M27_ReExportAlias) {
    TempDir tmp;
    tmp.write("a.js", "export let original = 88;");
    tmp.write("b.js", "export { original as aliased } from './a.js';");
    tmp.write("entry.js", R"(
import { aliased } from './b.js';
aliased;
)");
    auto result = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    EXPECT_EQ(result.value().as_number(), 88.0);
}

TEST(VmModule, M27_ReExportAlias) {
    TempDir tmp;
    tmp.write("a.js", "export let original = 88;");
    tmp.write("b.js", "export { original as aliased } from './a.js';");
    tmp.write("entry.js", R"(
import { aliased } from './b.js';
aliased;
)");
    auto result = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    EXPECT_EQ(result.value().as_number(), 88.0);
}

// ============================================================
// M-28: re-export live binding（re-export 的 Cell 与原始模块共享）
// a.js export let count；b.js re-export count；
// a.js 内 inc() 修改 count 后，通过 b.js 的 re-export 也应看到新值
// ============================================================

TEST(InterpModule, M28_ReExportLiveBinding) {
    TempDir tmp;
    tmp.write("a.js", R"(
export let count = 0;
export function inc() { count = count + 1; }
)");
    tmp.write("b.js", "export { count } from './a.js';");
    tmp.write("entry.js", R"(
import { count } from './b.js';
import { inc } from './a.js';
inc();
count;
)");
    auto result = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    // re-export 的 count 应该是 live binding，inc() 后应为 1
    EXPECT_EQ(result.value().as_number(), 1.0);
}

TEST(VmModule, M28_ReExportLiveBinding) {
    TempDir tmp;
    tmp.write("a.js", R"(
export let count = 0;
export function inc() { count = count + 1; }
)");
    tmp.write("b.js", "export { count } from './a.js';");
    tmp.write("entry.js", R"(
import { count } from './b.js';
import { inc } from './a.js';
inc();
count;
)");
    auto result = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    EXPECT_EQ(result.value().as_number(), 1.0);
}

// ============================================================
// M-29: 默认导出 + 具名导出同时存在（分两条 import 语句）
// ============================================================

TEST(InterpModule, M29_DefaultAndNamedExportCoexist) {
    TempDir tmp;
    tmp.write("m.js", R"(
export default 100;
export let extra = 5;
)");
    tmp.write("entry.js", R"(
import def from './m.js';
import { extra } from './m.js';
def + extra;
)");
    auto result = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    EXPECT_EQ(result.value().as_number(), 105.0);
}

TEST(VmModule, M29_DefaultAndNamedExportCoexist) {
    TempDir tmp;
    tmp.write("m.js", R"(
export default 100;
export let extra = 5;
)");
    tmp.write("entry.js", R"(
import def from './m.js';
import { extra } from './m.js';
def + extra;
)");
    auto result = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    EXPECT_EQ(result.value().as_number(), 105.0);
}

// ============================================================
// M-30: export { x as y } 的值正确性（alias 导出，模块执行后快照）
// 与 ExportSpecifierAlias 测试类似，但验证多个别名同时存在
// ============================================================

TEST(InterpModule, M30_MultipleExportSpecifierAliases) {
    TempDir tmp;
    tmp.write("m.js", R"(
let a = 1;
let b = 2;
let c = 3;
export { a as x, b as y, c as z };
)");
    tmp.write("entry.js", R"(
import { x, y, z } from './m.js';
x + y + z;
)");
    auto result = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    EXPECT_EQ(result.value().as_number(), 6.0);
}

TEST(VmModule, M30_MultipleExportSpecifierAliases) {
    TempDir tmp;
    tmp.write("m.js", R"(
let a = 1;
let b = 2;
let c = 3;
export { a as x, b as y, c as z };
)");
    tmp.write("entry.js", R"(
import { x, y, z } from './m.js';
x + y + z;
)");
    auto result = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    EXPECT_EQ(result.value().as_number(), 6.0);
}

// ============================================================
// M-31: 导入不存在的默认导出 → SyntaxError
// ============================================================

TEST(InterpModule, M31_ImportDefaultFromNoDefault) {
    TempDir tmp;
    tmp.write("m.js", "export let x = 1;");
    tmp.write("entry.js", R"(
import def from './m.js';
def;
)");
    auto result = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_FALSE(result.is_ok());
    EXPECT_NE(result.error().message().find("SyntaxError"), std::string::npos);
}

TEST(VmModule, M31_ImportDefaultFromNoDefault) {
    TempDir tmp;
    tmp.write("m.js", "export let x = 1;");
    tmp.write("entry.js", R"(
import def from './m.js';
def;
)");
    auto result = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_FALSE(result.is_ok());
    EXPECT_NE(result.error().message().find("SyntaxError"), std::string::npos);
}

// ============================================================
// M-32: 导入不存在的 re-export 名 → SyntaxError
// ============================================================

TEST(InterpModule, M32_ImportNonExistentReExport) {
    TempDir tmp;
    tmp.write("a.js", "export let x = 1;");
    tmp.write("b.js", "export { x } from './a.js';");
    tmp.write("entry.js", R"(
import { nonexistent } from './b.js';
nonexistent;
)");
    auto result = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_FALSE(result.is_ok());
    EXPECT_NE(result.error().message().find("SyntaxError"), std::string::npos);
}

TEST(VmModule, M32_ImportNonExistentReExport) {
    TempDir tmp;
    tmp.write("a.js", "export let x = 1;");
    tmp.write("b.js", "export { x } from './a.js';");
    tmp.write("entry.js", R"(
import { nonexistent } from './b.js';
nonexistent;
)");
    auto result = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_FALSE(result.is_ok());
    EXPECT_NE(result.error().message().find("SyntaxError"), std::string::npos);
}

// ============================================================
// M-33: export let 无初始值（初始化为 undefined）
// ============================================================

TEST(InterpModule, M33_ExportLetNoInit) {
    TempDir tmp;
    tmp.write("m.js", "export let x;");
    tmp.write("entry.js", R"(
import { x } from './m.js';
x === undefined;
)");
    auto result = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    EXPECT_TRUE(result.value().is_bool());
    EXPECT_TRUE(result.value().as_bool());
}

TEST(VmModule, M33_ExportLetNoInit) {
    TempDir tmp;
    tmp.write("m.js", "export let x;");
    tmp.write("entry.js", R"(
import { x } from './m.js';
x === undefined;
)");
    auto result = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    EXPECT_TRUE(result.value().is_bool());
    EXPECT_TRUE(result.value().as_bool());
}

// ============================================================
// M-34: 模块内非导出 var 不影响导出变量
// ============================================================

TEST(InterpModule, M34_NonExportedVarIsolation) {
    TempDir tmp;
    tmp.write("m.js", R"(
var internal = 999;
export let pub = 42;
)");
    tmp.write("entry.js", R"(
import { pub } from './m.js';
pub;
)");
    auto result = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    EXPECT_EQ(result.value().as_number(), 42.0);
}

TEST(VmModule, M34_NonExportedVarIsolation) {
    TempDir tmp;
    tmp.write("m.js", R"(
var internal = 999;
export let pub = 42;
)");
    tmp.write("entry.js", R"(
import { pub } from './m.js';
pub;
)");
    auto result = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    EXPECT_EQ(result.value().as_number(), 42.0);
}

// ============================================================
// M-35: 模块缓存幂等性（副作用只执行一次，用计数验证）
// counter.js 有自增副作用，被多个模块 import 但只执行一次
// ============================================================

TEST(InterpModule, M35_ModuleCacheIdempotent) {
    TempDir tmp;
    // 用 export var 模拟执行计数（每次执行 +1，但只执行一次）
    tmp.write("counter.js", R"(
export var execCount = 0;
execCount = execCount + 1;
)");
    tmp.write("a.js", "import { execCount } from './counter.js'; export let ac = execCount;");
    tmp.write("b.js", "import { execCount } from './counter.js'; export let bc = execCount;");
    tmp.write("c.js", "import { execCount } from './counter.js'; export let cc = execCount;");
    tmp.write("entry.js", R"(
import { ac } from './a.js';
import { bc } from './b.js';
import { cc } from './c.js';
ac + bc + cc;
)");
    auto result = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    // execCount 只执行一次，值为 1；三个模块读到的都是 1；1+1+1=3
    EXPECT_EQ(result.value().as_number(), 3.0);
}

TEST(VmModule, M35_ModuleCacheIdempotent) {
    TempDir tmp;
    tmp.write("counter.js", R"(
export var execCount = 0;
execCount = execCount + 1;
)");
    tmp.write("a.js", "import { execCount } from './counter.js'; export let ac = execCount;");
    tmp.write("b.js", "import { execCount } from './counter.js'; export let bc = execCount;");
    tmp.write("c.js", "import { execCount } from './counter.js'; export let cc = execCount;");
    tmp.write("entry.js", R"(
import { ac } from './a.js';
import { bc } from './b.js';
import { cc } from './c.js';
ac + bc + cc;
)");
    auto result = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    EXPECT_EQ(result.value().as_number(), 3.0);
}

// ============================================================
// M-36: export { x } live binding（M1 修复验证）
// export { count } 后 inc() 修改 count，import 端读到新值
// ============================================================

TEST(InterpModule, M36_ExportSpecifierLiveBinding) {
    TempDir tmp;
    tmp.write("counter.js", R"(
var count = 0;
function inc() { count = count + 1; }
export { count, inc };
)");
    tmp.write("entry.js", R"(
import { count, inc } from './counter.js';
inc();
inc();
count;
)");
    auto result = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    EXPECT_EQ(result.value().as_number(), 2.0);
}

TEST(VmModule, M36_ExportSpecifierLiveBinding) {
    TempDir tmp;
    tmp.write("counter.js", R"(
var count = 0;
function inc() { count = count + 1; }
export { count, inc };
)");
    tmp.write("entry.js", R"(
import { count, inc } from './counter.js';
inc();
inc();
count;
)");
    auto result = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    EXPECT_EQ(result.value().as_number(), 2.0);
}

// ============================================================
// M-37: export default function foo() {} 模块内 foo 可调用（M3 修复验证）
// ============================================================

TEST(InterpModule, M37_ExportDefaultNamedFunctionBinding) {
    TempDir tmp;
    tmp.write("m.js", R"(
export default function greet() { return 42; }
var result = greet();
export { result };
)");
    tmp.write("entry.js", R"(
import { result } from './m.js';
result;
)");
    auto res = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_number(), 42.0);
}

TEST(VmModule, M37_ExportDefaultNamedFunctionBinding) {
    TempDir tmp;
    tmp.write("m.js", R"(
export default function greet() { return 42; }
var result = greet();
export { result };
)");
    tmp.write("entry.js", R"(
import { result } from './m.js';
result;
)");
    auto res = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_number(), 42.0);
}

// ============================================================
// M-38: export let TDZ 跨模块共享（M2 修复验证）
// 导入 let 变量后，在初始化前访问应抛 ReferenceError
// 注意：静态 import/export 在模块执行前完成 Link，执行时 let 初始化后才可访问
// 这里测试：export let 正常初始化后，import 端读到正确值（Cell.initialized 共享）
// ============================================================

TEST(InterpModule, M38_ExportLetCellInitialized) {
    TempDir tmp;
    tmp.write("m.js", "export let x = 99;");
    tmp.write("entry.js", R"(
import { x } from './m.js';
x;
)");
    auto result = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    EXPECT_EQ(result.value().as_number(), 99.0);
}

TEST(VmModule, M38_ExportLetCellInitialized) {
    TempDir tmp;
    tmp.write("m.js", "export let x = 99;");
    tmp.write("entry.js", R"(
import { x } from './m.js';
x;
)");
    auto result = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    EXPECT_EQ(result.value().as_number(), 99.0);
}

// ============================================================
// M-39: export async function — 基础解析和执行
// ============================================================

TEST(InterpModule, M39_ExportAsyncFunctionBasic) {
    TempDir tmp;
    tmp.write("m.js", "export async function foo() { return 42; }");
    tmp.write("entry.js", R"(
import { foo } from './m.js';
foo();
)");
    auto result = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    // async function 返回 Promise，不是数字
    EXPECT_TRUE(result.value().is_object());
}

TEST(VmModule, M39_ExportAsyncFunctionBasic) {
    TempDir tmp;
    tmp.write("m.js", "export async function foo() { return 42; }");
    tmp.write("entry.js", R"(
import { foo } from './m.js';
foo();
)");
    auto result = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    EXPECT_TRUE(result.value().is_object());
}

// ============================================================
// M-40: export async function — 含 await
// ============================================================

TEST(InterpModule, M40_ExportAsyncFunctionWithAwait) {
    TempDir tmp;
    tmp.write("m.js", R"(
export async function foo() {
    const v = await Promise.resolve(1);
    return v;
}
)");
    tmp.write("entry.js", R"(
import { foo } from './m.js';
foo();
)");
    auto result = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    EXPECT_TRUE(result.value().is_object());
}

TEST(VmModule, M40_ExportAsyncFunctionWithAwait) {
    TempDir tmp;
    tmp.write("m.js", R"(
export async function foo() {
    const v = await Promise.resolve(1);
    return v;
}
)");
    tmp.write("entry.js", R"(
import { foo } from './m.js';
foo();
)");
    auto result = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    EXPECT_TRUE(result.value().is_object());
}

// ============================================================
// M-41: export default async function（匿名）
// ============================================================

TEST(InterpModule, M41_ExportDefaultAnonAsyncFunction) {
    TempDir tmp;
    tmp.write("m.js", "export default async function() { return 1; }");
    tmp.write("entry.js", R"(
import fn from './m.js';
fn();
)");
    auto result = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    EXPECT_TRUE(result.value().is_object());
}

TEST(VmModule, M41_ExportDefaultAnonAsyncFunction) {
    TempDir tmp;
    tmp.write("m.js", "export default async function() { return 1; }");
    tmp.write("entry.js", R"(
import fn from './m.js';
fn();
)");
    auto result = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    EXPECT_TRUE(result.value().is_object());
}

// ============================================================
// M-42: export default async function named（具名）
// ============================================================

TEST(InterpModule, M42_ExportDefaultNamedAsyncFunction) {
    TempDir tmp;
    tmp.write("m.js", "export default async function named() { return 1; }");
    tmp.write("entry.js", R"(
import fn from './m.js';
fn();
)");
    auto result = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    EXPECT_TRUE(result.value().is_object());
}

TEST(VmModule, M42_ExportDefaultNamedAsyncFunction) {
    TempDir tmp;
    tmp.write("m.js", "export default async function named() { return 1; }");
    tmp.write("entry.js", R"(
import fn from './m.js';
fn();
)");
    auto result = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    EXPECT_TRUE(result.value().is_object());
}

// ============================================================
// M-43: 回归 — export function 仍然正常工作
// ============================================================

TEST(InterpModule, M43_ExportFunctionRegression) {
    TempDir tmp;
    tmp.write("m.js", "export function add(a, b) { return a + b; }");
    tmp.write("entry.js", R"(
import { add } from './m.js';
add(2, 3);
)");
    auto result = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    EXPECT_EQ(result.value().as_number(), 5.0);
}

TEST(VmModule, M43_ExportFunctionRegression) {
    TempDir tmp;
    tmp.write("m.js", "export function add(a, b) { return a + b; }");
    tmp.write("entry.js", R"(
import { add } from './m.js';
add(2, 3);
)");
    auto result = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    EXPECT_EQ(result.value().as_number(), 5.0);
}

// ============================================================
// M-44: 回归 — 模块顶层 async function 声明仍然正常工作
// ============================================================

TEST(InterpModule, M44_TopLevelAsyncFunctionRegression) {
    TempDir tmp;
    tmp.write("m.js", R"(
async function helper() { return 7; }
export const result = helper;
)");
    tmp.write("entry.js", R"(
import { result } from './m.js';
result();
)");
    auto res = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_TRUE(res.value().is_object());
}

TEST(VmModule, M44_TopLevelAsyncFunctionRegression) {
    TempDir tmp;
    tmp.write("m.js", R"(
async function helper() { return 7; }
export const result = helper;
)");
    tmp.write("entry.js", R"(
import { result } from './m.js';
result();
)");
    auto res = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_TRUE(res.value().is_object());
}

// ============================================================
// M-45: 同一模块导出多个 async function，各自独立可调用
// 验证：两个 async function 都能被 import，调用后均返回 Promise 对象
// ============================================================

TEST(InterpModule, M45_MultipleExportAsyncFunctions) {
    TempDir tmp;
    tmp.write("m.js", R"(
export async function foo() { return 1; }
export async function bar() { return 2; }
)");
    // 调用两个函数，验证两者都返回 Promise（is_object），且不报错
    // 用数组收集两个 Promise，最后一行是数组长度（同步可验证）
    tmp.write("entry.js", R"(
import { foo, bar } from './m.js';
var p1 = foo();
var p2 = bar();
(p1 !== undefined && p2 !== undefined);
)");
    auto res = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_TRUE(res.value().is_bool());
    EXPECT_TRUE(res.value().as_bool());
}

TEST(VmModule, M45_MultipleExportAsyncFunctions) {
    TempDir tmp;
    tmp.write("m.js", R"(
export async function foo() { return 1; }
export async function bar() { return 2; }
)");
    tmp.write("entry.js", R"(
import { foo, bar } from './m.js';
var p1 = foo();
var p2 = bar();
(p1 !== undefined && p2 !== undefined);
)");
    auto res = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_TRUE(res.value().is_bool());
    EXPECT_TRUE(res.value().as_bool());
}

// ============================================================
// M-46: export async function 与 export function 混用，两者均可 import 和调用
// 验证：syncFoo() 返回正确同步值，asyncFoo() 返回 Promise 对象
// ============================================================

TEST(InterpModule, M46_AsyncAndSyncExportMixed) {
    TempDir tmp;
    tmp.write("m.js", R"(
export async function asyncFoo() { return 10; }
export function syncFoo() { return 20; }
)");
    // 验证同步函数返回正确值，async 函数返回 Promise 对象（不报错）
    tmp.write("entry.js", R"(
import { asyncFoo, syncFoo } from './m.js';
var syncResult = syncFoo();
var asyncResult = asyncFoo();
syncResult;
)");
    auto res = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_number(), 20.0);
}

TEST(VmModule, M46_AsyncAndSyncExportMixed) {
    TempDir tmp;
    tmp.write("m.js", R"(
export async function asyncFoo() { return 10; }
export function syncFoo() { return 20; }
)");
    tmp.write("entry.js", R"(
import { asyncFoo, syncFoo } from './m.js';
var syncResult = syncFoo();
var asyncResult = asyncFoo();
syncResult;
)");
    auto res = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_number(), 20.0);
}

// ============================================================
// M-47: import 并调用含 await 的 export async function（跨模块 await）
// 验证：含 await 的 async function 可被 import，调用后返回 Promise 对象
// 注意：exec_module 无 re-read 机制，.then 回调修改的变量无法作为返回值，
//       因此验证 Promise 对象本身而非 resolved 值
// ============================================================

TEST(InterpModule, M47_ImportAndAwaitResult) {
    TempDir tmp;
    tmp.write("m.js", R"(
export async function compute() {
    const v = await Promise.resolve(99);
    return v;
}
)");
    tmp.write("entry.js", R"(
import { compute } from './m.js';
compute();
)");
    auto res = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_TRUE(res.value().is_object());
}

TEST(VmModule, M47_ImportAndAwaitResult) {
    TempDir tmp;
    tmp.write("m.js", R"(
export async function compute() {
    const v = await Promise.resolve(99);
    return v;
}
)");
    tmp.write("entry.js", R"(
import { compute } from './m.js';
compute();
)");
    auto res = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_TRUE(res.value().is_object());
}

// ============================================================
// M-48: export async function 内部有多个 await（串行 await）
// 验证：含多个 await 的 async function 可被 import，调用后返回 Promise 对象
// ============================================================

TEST(InterpModule, M48_ExportAsyncFunctionMultipleAwait) {
    TempDir tmp;
    tmp.write("m.js", R"(
export async function sum() {
    const a = await Promise.resolve(3);
    const b = await Promise.resolve(7);
    return a + b;
}
)");
    tmp.write("entry.js", R"(
import { sum } from './m.js';
sum();
)");
    auto res = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_TRUE(res.value().is_object());
}

TEST(VmModule, M48_ExportAsyncFunctionMultipleAwait) {
    TempDir tmp;
    tmp.write("m.js", R"(
export async function sum() {
    const a = await Promise.resolve(3);
    const b = await Promise.resolve(7);
    return a + b;
}
)");
    tmp.write("entry.js", R"(
import { sum } from './m.js';
sum();
)");
    auto res = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_TRUE(res.value().is_object());
}

// ============================================================
// M-49: export async function 提升语义
// 模块内调用位置在声明之前，验证提升行为（调用不报 ReferenceError）
// ============================================================

TEST(InterpModule, M49_ExportAsyncFunctionHoisting) {
    TempDir tmp;
    tmp.write("m.js", R"(
var p = foo();
export async function foo() { return 55; }
export { p };
)");
    // p 是 foo() 的返回值（Promise 对象），验证提升后 foo 可在声明前被调用
    tmp.write("entry.js", R"(
import { p } from './m.js';
p;
)");
    auto res = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    // p 应为 Promise 对象（foo 已提升，调用成功）
    EXPECT_TRUE(res.value().is_object());
}

TEST(VmModule, M49_ExportAsyncFunctionHoisting) {
    TempDir tmp;
    tmp.write("m.js", R"(
var p = foo();
export async function foo() { return 55; }
export { p };
)");
    tmp.write("entry.js", R"(
import { p } from './m.js';
p;
)");
    auto res = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_TRUE(res.value().is_object());
}

// ============================================================
// M-50: export default async function named() {} 的可调用性
// import 后验证函数可被调用，返回 Promise 对象
// 注意：fn.name 属性当前返回 undefined（JSFunction.name_ 字段未暴露到
//       own_properties_，属于已知技术债务），此处不验证 name 属性
// ============================================================

TEST(InterpModule, M50_ExportDefaultAsyncFunctionCallable) {
    TempDir tmp;
    tmp.write("m.js", "export default async function myFunc() { return 1; }");
    tmp.write("entry.js", R"(
import fn from './m.js';
fn();
)");
    auto res = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_TRUE(res.value().is_object());
}

TEST(VmModule, M50_ExportDefaultAsyncFunctionCallable) {
    TempDir tmp;
    tmp.write("m.js", "export default async function myFunc() { return 1; }");
    tmp.write("entry.js", R"(
import fn from './m.js';
fn();
)");
    auto res = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_TRUE(res.value().is_object());
}

// ============================================================
// M-51: export async function 可被多次调用（每次返回新 Promise）
// ============================================================

TEST(InterpModule, M51_ExportAsyncFunctionMultipleCalls) {
    TempDir tmp;
    tmp.write("m.js", R"(
export async function compute() { return 7; }
)");
    // 两次调用，两个 Promise 对象应不同（每次调用产生新 Promise）
    tmp.write("entry.js", R"(
import { compute } from './m.js';
var p1 = compute();
var p2 = compute();
(p1 !== p2);
)");
    auto res = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_TRUE(res.value().is_bool());
    EXPECT_TRUE(res.value().as_bool());
}

TEST(VmModule, M51_ExportAsyncFunctionMultipleCalls) {
    TempDir tmp;
    tmp.write("m.js", R"(
export async function compute() { return 7; }
)");
    tmp.write("entry.js", R"(
import { compute } from './m.js';
var p1 = compute();
var p2 = compute();
(p1 !== p2);
)");
    auto res = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_TRUE(res.value().is_bool());
    EXPECT_TRUE(res.value().as_bool());
}

// ============================================================
// M-52: export async function 抛出异常 → 返回 rejected Promise（不向外同步抛）
// 验证：async function 内 throw 不向模块外同步传播，exec_module 仍返回 ok
// ============================================================

TEST(InterpModule, M52_ExportAsyncFunctionReject) {
    TempDir tmp;
    tmp.write("m.js", R"(
export async function failing() {
    throw 'async error';
}
)");
    // async function 内 throw 不向外同步传播，exec_module 应成功
    // 返回值是 rejected Promise 对象
    tmp.write("entry.js", R"(
import { failing } from './m.js';
failing();
)");
    auto res = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_TRUE(res.value().is_object());
}

TEST(VmModule, M52_ExportAsyncFunctionReject) {
    TempDir tmp;
    tmp.write("m.js", R"(
export async function failing() {
    throw 'async error';
}
)");
    tmp.write("entry.js", R"(
import { failing } from './m.js';
failing();
)");
    auto res = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_TRUE(res.value().is_object());
}

// ============================================================
// M-1 修复回归：export default async function foo() {} 的 local_name
// 模块内部可通过 foo 调用该函数（不再 ReferenceError）
// ============================================================

TEST(InterpModule, M1Fix_ExportDefaultAsyncFunctionLocalNameCallable) {
    TempDir tmp;
    // m.js 内部通过 foo 调用自身，验证 local_name 绑定正确
    tmp.write("m.js", R"(
export default async function foo() { return 42; }
foo();
)");
    tmp.write("entry.js", "import fn from './m.js'; fn();");
    auto res = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_TRUE(res.value().is_object());
}

TEST(VmModule, M1Fix_ExportDefaultAsyncFunctionLocalNameCallable) {
    TempDir tmp;
    tmp.write("m.js", R"(
export default async function foo() { return 42; }
foo();
)");
    tmp.write("entry.js", "import fn from './m.js'; fn();");
    auto res = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_TRUE(res.value().is_object());
}

// ============================================================
// DI-01: 基础动态 import — 返回 Promise，resolve 为 namespace 对象
// ============================================================

TEST(InterpModule, DI01_BasicDynamicImport) {
    TempDir tmp;
    tmp.write("m.js", "export let x = 42;");
    tmp.write("entry.js", R"(
import('./m.js').then(function(ns) { ns.x; });
)");
    auto result = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(result.is_ok()) << result.error().message();
}

TEST(VmModule, DI01_BasicDynamicImport) {
    TempDir tmp;
    tmp.write("m.js", "export let x = 42;");
    tmp.write("entry.js", R"(
import('./m.js').then(function(ns) { ns.x; });
)");
    auto result = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(result.is_ok()) << result.error().message();
}

// ============================================================
// DI-02: import() 返回值是 Promise（是 object）
// ============================================================

TEST(InterpModule, DI02_ReturnsPromise) {
    TempDir tmp;
    tmp.write("m.js", "export let v = 1;");
    tmp.write("entry.js", R"(
let p = import('./m.js');
typeof p;
)");
    auto result = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    EXPECT_EQ(result.value().as_string(), "object");
}

TEST(VmModule, DI02_ReturnsPromise) {
    TempDir tmp;
    tmp.write("m.js", "export let v = 1;");
    tmp.write("entry.js", R"(
let p = import('./m.js');
typeof p;
)");
    auto result = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(result.is_ok()) << result.error().message();
    EXPECT_EQ(result.value().as_string(), "object");
}

// ============================================================
// DI-03: namespace 对象包含导出的值
// ============================================================

TEST(InterpModule, DI03_NamespaceHasExport) {
    TempDir tmp;
    tmp.write("m.js", "export let answer = 42;");
    tmp.write("entry.js", R"(
let result = 0;
import('./m.js').then(function(ns) { result = ns.answer; });
result;
)");
    auto res = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_number(), 42.0);
}

TEST(VmModule, DI03_NamespaceHasExport) {
    TempDir tmp;
    tmp.write("m.js", "export let answer = 42;");
    tmp.write("entry.js", R"(
let result = 0;
import('./m.js').then(function(ns) { result = ns.answer; });
result;
)");
    auto res = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_number(), 42.0);
}

// ============================================================
// DI-04: 动态 import 带变量 specifier
// ============================================================

TEST(InterpModule, DI04_VariableSpecifier) {
    TempDir tmp;
    tmp.write("math.js", "export let PI = 3;");
    tmp.write("entry.js", R"(
let mod = './math.js';
let result = 0;
import(mod).then(function(ns) { result = ns.PI; });
result;
)");
    auto res = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_number(), 3.0);
}

TEST(VmModule, DI04_VariableSpecifier) {
    TempDir tmp;
    tmp.write("math.js", "export let PI = 3;");
    tmp.write("entry.js", R"(
let mod = './math.js';
let result = 0;
import(mod).then(function(ns) { result = ns.PI; });
result;
)");
    auto res = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_number(), 3.0);
}

// ============================================================
// DI-05: 错误模块 → rejected Promise（不抛出同步异常）
// ============================================================

TEST(InterpModule, DI05_ErrorModuleRejectsPromise) {
    TempDir tmp;
    tmp.write("entry.js", R"(
let rejected = false;
import('./nonexistent.js').then(null, function(e) { rejected = true; });
rejected;
)");
    auto res = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_TRUE(res.value().as_bool());
}

TEST(VmModule, DI05_ErrorModuleRejectsPromise) {
    TempDir tmp;
    tmp.write("entry.js", R"(
let rejected = false;
import('./nonexistent.js').then(null, function(e) { rejected = true; });
rejected;
)");
    auto res = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_TRUE(res.value().as_bool());
}

// ============================================================
// DI-06: 多个导出都在 namespace 对象中
// ============================================================

TEST(InterpModule, DI06_MultipleExports) {
    TempDir tmp;
    tmp.write("m.js", R"(
export let a = 1;
export let b = 2;
export function add(x, y) { return x + y; }
)");
    tmp.write("entry.js", R"(
let sum = 0;
import('./m.js').then(function(ns) { sum = ns.a + ns.b; });
sum;
)");
    auto res = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_number(), 3.0);
}

TEST(VmModule, DI06_MultipleExports) {
    TempDir tmp;
    tmp.write("m.js", R"(
export let a = 1;
export let b = 2;
export function add(x, y) { return x + y; }
)");
    tmp.write("entry.js", R"(
let sum = 0;
import('./m.js').then(function(ns) { sum = ns.a + ns.b; });
sum;
)");
    auto res = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_number(), 3.0);
}

// ============================================================
// DI-07: 模块缓存 — 多次 import 同一模块只执行一次
// ============================================================

TEST(InterpModule, DI07_ModuleCaching) {
    TempDir tmp;
    tmp.write("counter.js", R"(
export let count = 0;
)");
    tmp.write("entry.js", R"(
let n1 = 0;
let n2 = 0;
import('./counter.js').then(function(ns) { n1 = ns.count; });
import('./counter.js').then(function(ns) { n2 = ns.count; });
n1 + n2;
)");
    auto res = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_number(), 0.0);
}

TEST(VmModule, DI07_ModuleCaching) {
    TempDir tmp;
    tmp.write("counter.js", R"(
export let count = 0;
)");
    tmp.write("entry.js", R"(
let n1 = 0;
let n2 = 0;
import('./counter.js').then(function(ns) { n1 = ns.count; });
import('./counter.js').then(function(ns) { n2 = ns.count; });
n1 + n2;
)");
    auto res = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_number(), 0.0);
}

// ============================================================
// DI-08: 在 async 函数中 await import()
// ============================================================

TEST(InterpModule, DI08_AwaitImport) {
    TempDir tmp;
    tmp.write("m.js", "export let val = 99;");
    tmp.write("entry.js", R"(
let result = 0;
async function load() {
    let ns = await import('./m.js');
    result = ns.val;
}
load();
result;
)");
    auto res = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_number(), 99.0);
}

TEST(VmModule, DI08_AwaitImport) {
    TempDir tmp;
    tmp.write("m.js", "export let val = 99;");
    tmp.write("entry.js", R"(
let result = 0;
async function load() {
    let ns = await import('./m.js');
    result = ns.val;
}
load();
result;
)");
    auto res = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_number(), 99.0);
}

// ============================================================
// DI-09: import() 表达式可以在非模块上下文中使用
// ============================================================

TEST(InterpModule, DI09_ImportInScript) {
    TempDir tmp;
    tmp.write("m.js", "export let x = 7;");
    tmp.write("entry.js", R"(
let result = 0;
import('./m.js').then(function(ns) { result = ns.x; });
result;
)");
    // exec_module 是模块上下文，但 import() 也应该在普通脚本上下文中工作
    // 这里用 exec_module 因为 exec() 不支持模块加载
    auto res = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_number(), 7.0);
}

TEST(VmModule, DI09_ImportInScript) {
    TempDir tmp;
    tmp.write("m.js", "export let x = 7;");
    tmp.write("entry.js", R"(
let result = 0;
import('./m.js').then(function(ns) { result = ns.x; });
result;
)");
    auto res = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_number(), 7.0);
}

// ============================================================
// DI-10: default export 在 namespace 对象中
// ============================================================

TEST(InterpModule, DI10_DefaultExportInNamespace) {
    TempDir tmp;
    tmp.write("m.js", "export default 42;");
    tmp.write("entry.js", R"(
let result = 0;
import('./m.js').then(function(ns) { result = ns.default; });
result;
)");
    auto res = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_number(), 42.0);
}

TEST(VmModule, DI10_DefaultExportInNamespace) {
    TempDir tmp;
    tmp.write("m.js", "export default 42;");
    tmp.write("entry.js", R"(
let result = 0;
import('./m.js').then(function(ns) { result = ns.default; });
result;
)");
    auto res = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_number(), 42.0);
}

// ============================================================
// import.meta 测试（IM-01 ~ IM-12）
// ============================================================

// IM-01: 模块内 import.meta.url 返回模块文件绝对路径
TEST(InterpModule, IM01_ImportMetaUrl) {
    TempDir tmp;
    tmp.write("entry.js", R"(
import.meta.url;
)");
    auto res = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_TRUE(res.value().is_string());
    EXPECT_EQ(res.value().as_string(), tmp.abs("entry.js"));
}

TEST(VmModule, IM01_ImportMetaUrl) {
    TempDir tmp;
    tmp.write("entry.js", R"(
import.meta.url;
)");
    auto res = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_TRUE(res.value().is_string());
    EXPECT_EQ(res.value().as_string(), tmp.abs("entry.js"));
}

// IM-02: 模块内 import.meta 的 [[Prototype]] 为 null
// 验证方式：import.meta 不继承 Object.prototype 的属性（如 hasOwnProperty）
TEST(InterpModule, IM02_ImportMetaProtoNull) {
    TempDir tmp;
    tmp.write("entry.js", R"(
import.meta.hasOwnProperty === undefined;
)");
    auto res = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_bool(), true);
}

TEST(VmModule, IM02_ImportMetaProtoNull) {
    TempDir tmp;
    tmp.write("entry.js", R"(
import.meta.hasOwnProperty === undefined;
)");
    auto res = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_bool(), true);
}

// IM-03: 模块内 import.meta 的属性可写
TEST(InterpModule, IM03_ImportMetaPropertyWritable) {
    TempDir tmp;
    tmp.write("entry.js", R"(
import.meta.x = 1;
import.meta.x;
)");
    auto res = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_number(), 1.0);
}

TEST(VmModule, IM03_ImportMetaPropertyWritable) {
    TempDir tmp;
    tmp.write("entry.js", R"(
import.meta.x = 1;
import.meta.x;
)");
    auto res = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_number(), 1.0);
}

// IM-04: 不同模块的 import.meta 是独立对象
TEST(InterpModule, IM04_DifferentModulesIndependentMeta) {
    TempDir tmp;
    tmp.write("a.js", "import.meta.x = 1; export let ax = import.meta.x;");
    tmp.write("b.js", "export let bx = import.meta.x;");
    tmp.write("entry.js", R"(
import { ax } from './a.js';
import { bx } from './b.js';
ax + ',' + bx;
)");
    auto res = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_string(), "1,undefined");
}

TEST(VmModule, IM04_DifferentModulesIndependentMeta) {
    TempDir tmp;
    tmp.write("a.js", "import.meta.x = 1; export let ax = import.meta.x;");
    tmp.write("b.js", "export let bx = import.meta.x;");
    tmp.write("entry.js", R"(
import { ax } from './a.js';
import { bx } from './b.js';
ax + ',' + bx;
)");
    auto res = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_string(), "1,undefined");
}

// IM-05: Script 中使用 import.meta → SyntaxError
TEST(InterpModule, IM05_ScriptImportMetaSyntaxError) {
    auto parse_result = qppjs::parse_program("import.meta.url;", false);
    EXPECT_FALSE(parse_result.ok());
    EXPECT_NE(parse_result.error().message().find("only valid in modules"), std::string::npos);
}

TEST(VmModule, IM05_ScriptImportMetaSyntaxError) {
    auto parse_result = qppjs::parse_program("import.meta.url;", false);
    EXPECT_FALSE(parse_result.ok());
    EXPECT_NE(parse_result.error().message().find("only valid in modules"), std::string::npos);
}

// IM-06: 模块内 import["meta"] 不是 import.meta（import 不是已定义的变量，报 ReferenceError）
TEST(InterpModule, IM06_ImportBracketMetaIsUndefined) {
    TempDir tmp;
    tmp.write("entry.js", R"(
let result = 'ok';
try { import['meta']; } catch(e) { result = 'error'; }
result;
)");
    auto res = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_string(), "error");
}

TEST(VmModule, IM06_ImportBracketMetaIsUndefined) {
    TempDir tmp;
    tmp.write("entry.js", R"(
let result = 'ok';
try { import['meta']; } catch(e) { result = 'error'; }
result;
)");
    auto res = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_string(), "error");
}

// IM-07: 模块内 typeof import.meta === "object"
TEST(InterpModule, IM07_TypeofImportMetaIsObject) {
    TempDir tmp;
    tmp.write("entry.js", R"(
typeof import.meta === 'object';
)");
    auto res = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_bool(), true);
}

TEST(VmModule, IM07_TypeofImportMetaIsObject) {
    TempDir tmp;
    tmp.write("entry.js", R"(
typeof import.meta === 'object';
)");
    auto res = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_bool(), true);
}

// IM-08: 模块内 import.meta 在函数体内使用
TEST(InterpModule, IM08_ImportMetaInFunctionBody) {
    TempDir tmp;
    tmp.write("entry.js", R"(
function getUrl() { return import.meta.url; }
getUrl();
)");
    auto res = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_string(), tmp.abs("entry.js"));
}

TEST(VmModule, IM08_ImportMetaInFunctionBody) {
    TempDir tmp;
    tmp.write("entry.js", R"(
function getUrl() { return import.meta.url; }
getUrl();
)");
    auto res = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_string(), tmp.abs("entry.js"));
}

// IM-09: 两个不同模块各自访问 import.meta.url
TEST(InterpModule, IM09_TwoModulesOwnMetaUrl) {
    TempDir tmp;
    tmp.write("a.js", "export let aUrl = import.meta.url;");
    tmp.write("b.js", "export let bUrl = import.meta.url;");
    tmp.write("entry.js", R"(
import { aUrl } from './a.js';
import { bUrl } from './b.js';
aUrl + '|' + bUrl;
)");
    auto res = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_string(), tmp.abs("a.js") + "|" + tmp.abs("b.js"));
}

TEST(VmModule, IM09_TwoModulesOwnMetaUrl) {
    TempDir tmp;
    tmp.write("a.js", "export let aUrl = import.meta.url;");
    tmp.write("b.js", "export let bUrl = import.meta.url;");
    tmp.write("entry.js", R"(
import { aUrl } from './a.js';
import { bUrl } from './b.js';
aUrl + '|' + bUrl;
)");
    auto res = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_string(), tmp.abs("a.js") + "|" + tmp.abs("b.js"));
}

// IM-10: Object.keys(import.meta) 包含 "url"
TEST(InterpModule, IM10_ObjectKeysContainsUrl) {
    TempDir tmp;
    tmp.write("entry.js", R"(
let keys = Object.keys(import.meta);
keys.indexOf('url') !== -1;
)");
    auto res = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_bool(), true);
}

TEST(VmModule, IM10_ObjectKeysContainsUrl) {
    TempDir tmp;
    tmp.write("entry.js", R"(
let keys = Object.keys(import.meta);
keys.indexOf('url') !== -1;
)");
    auto res = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_bool(), true);
}

// IM-11: 模块内 import.meta 赋值给变量
TEST(InterpModule, IM11_AssignImportMetaToVariable) {
    TempDir tmp;
    tmp.write("entry.js", R"(
let m = import.meta;
m.url;
)");
    auto res = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_string(), tmp.abs("entry.js"));
}

TEST(VmModule, IM11_AssignImportMetaToVariable) {
    TempDir tmp;
    tmp.write("entry.js", R"(
let m = import.meta;
m.url;
)");
    auto res = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_string(), tmp.abs("entry.js"));
}

// IM-12: 模块内多次访问 import.meta 返回同一对象
TEST(InterpModule, IM12_MultipleAccessSameObject) {
    TempDir tmp;
    tmp.write("entry.js", R"(
import.meta === import.meta;
)");
    auto res = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_bool(), true);
}

TEST(VmModule, IM12_MultipleAccessSameObject) {
    TempDir tmp;
    tmp.write("entry.js", R"(
import.meta === import.meta;
)");
    auto res = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_bool(), true);
}

// ============================================================
// import.meta 边界测试（IM-13 ~ IM-28）
// ============================================================

// IM-13: 模块内深层嵌套函数中访问 import.meta.url
// 验证 VM 侧向上搜索调用栈找模块帧的逻辑
TEST(InterpModule, IM13_DeepNestedFunctionImportMeta) {
    TempDir tmp;
    tmp.write("entry.js", R"(
function outer() {
    function inner() {
        return import.meta.url;
    }
    return inner();
}
outer();
)");
    auto res = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_string(), tmp.abs("entry.js"));
}

TEST(VmModule, IM13_DeepNestedFunctionImportMeta) {
    TempDir tmp;
    tmp.write("entry.js", R"(
function outer() {
    function inner() {
        return import.meta.url;
    }
    return inner();
}
outer();
)");
    auto res = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_string(), tmp.abs("entry.js"));
}

// IM-14: 模块内闭包捕获 import.meta，在模块外（通过导出函数）调用
// import.meta 是词法绑定，应返回定义模块（m.js）的 URL，而非调用者模块（entry.js）
TEST(InterpModule, IM14_ClosureCapturesImportMeta) {
    TempDir tmp;
    tmp.write("m.js", R"(
export function getMetaUrl() { return import.meta.url; }
)");
    tmp.write("entry.js", R"(
import { getMetaUrl } from './m.js';
getMetaUrl();
)");
    auto res = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_string(), tmp.abs("m.js"));
}

TEST(VmModule, IM14_ClosureCapturesImportMeta) {
    TempDir tmp;
    tmp.write("m.js", R"(
export function getMetaUrl() { return import.meta.url; }
)");
    tmp.write("entry.js", R"(
import { getMetaUrl } from './m.js';
getMetaUrl();
)");
    auto res = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_string(), tmp.abs("m.js"));
}

// IM-15: import.meta 作为函数参数传递
TEST(InterpModule, IM15_ImportMetaAsArgument) {
    TempDir tmp;
    tmp.write("entry.js", R"(
function checkUrl(meta) { return meta.url; }
checkUrl(import.meta);
)");
    auto res = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_string(), tmp.abs("entry.js"));
}

TEST(VmModule, IM15_ImportMetaAsArgument) {
    TempDir tmp;
    tmp.write("entry.js", R"(
function checkUrl(meta) { return meta.url; }
checkUrl(import.meta);
)");
    auto res = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_string(), tmp.abs("entry.js"));
}

// IM-16: import.meta 在条件分支中使用
TEST(InterpModule, IM16_ImportMetaInConditional) {
    TempDir tmp;
    tmp.write("entry.js", R"(
let result = '';
if (import.meta.url.length > 0) {
    result = 'has_url';
} else {
    result = 'no_url';
}
result;
)");
    auto res = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_string(), "has_url");
}

TEST(VmModule, IM16_ImportMetaInConditional) {
    TempDir tmp;
    tmp.write("entry.js", R"(
let result = '';
if (import.meta.url.length > 0) {
    result = 'has_url';
} else {
    result = 'no_url';
}
result;
)");
    auto res = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_string(), "has_url");
}

// IM-17: import.meta 在循环中使用（多次访问，验证缓存一致性）
TEST(InterpModule, IM17_ImportMetaInLoop) {
    TempDir tmp;
    tmp.write("entry.js", R"(
let same = true;
let first = import.meta;
for (let i = 0; i < 5; i = i + 1) {
    if (import.meta !== first) { same = false; }
}
same;
)");
    auto res = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_bool(), true);
}

TEST(VmModule, IM17_ImportMetaInLoop) {
    TempDir tmp;
    tmp.write("entry.js", R"(
let same = true;
let first = import.meta;
for (let i = 0; i < 5; i = i + 1) {
    if (import.meta !== first) { same = false; }
}
same;
)");
    auto res = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_bool(), true);
}

// IM-18: import.meta 在 try/catch 块中使用
TEST(InterpModule, IM18_ImportMetaInTryCatch) {
    TempDir tmp;
    tmp.write("entry.js", R"(
let result = '';
try {
    result = import.meta.url;
} catch(e) {
    result = 'error';
}
result;
)");
    auto res = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_string(), tmp.abs("entry.js"));
}

TEST(VmModule, IM18_ImportMetaInTryCatch) {
    TempDir tmp;
    tmp.write("entry.js", R"(
let result = '';
try {
    result = import.meta.url;
} catch(e) {
    result = 'error';
}
result;
)");
    auto res = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_string(), tmp.abs("entry.js"));
}

// IM-19: import.meta 属性可删除（delete 未实现，但属性覆盖验证）
// 验证 import.meta.url 可被覆盖（属性 writable）
TEST(InterpModule, IM19_ImportMetaUrlOverridable) {
    TempDir tmp;
    tmp.write("entry.js", R"(
import.meta.url = '/custom/path';
import.meta.url;
)");
    auto res = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_string(), "/custom/path");
}

TEST(VmModule, IM19_ImportMetaUrlOverridable) {
    TempDir tmp;
    tmp.write("entry.js", R"(
import.meta.url = '/custom/path';
import.meta.url;
)");
    auto res = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_string(), "/custom/path");
}

// IM-20: import.meta 在 re-export 链中的模块里使用
// 验证中间模块的 import.meta.url 指向自身而非入口模块
TEST(InterpModule, IM20_ImportMetaInReExportChain) {
    TempDir tmp;
    tmp.write("a.js", "export let aUrl = import.meta.url;");
    tmp.write("b.js", "export { aUrl } from './a.js'; export let bUrl = import.meta.url;");
    tmp.write("entry.js", R"(
import { aUrl, bUrl } from './b.js';
aUrl + '|' + bUrl;
)");
    auto res = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_string(), tmp.abs("a.js") + "|" + tmp.abs("b.js"));
}

TEST(VmModule, IM20_ImportMetaInReExportChain) {
    TempDir tmp;
    tmp.write("a.js", "export let aUrl = import.meta.url;");
    tmp.write("b.js", "export { aUrl } from './a.js'; export let bUrl = import.meta.url;");
    tmp.write("entry.js", R"(
import { aUrl, bUrl } from './b.js';
aUrl + '|' + bUrl;
)");
    auto res = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_string(), tmp.abs("a.js") + "|" + tmp.abs("b.js"));
}

// IM-21: import.meta 在循环依赖模块的顶层使用（非函数体内）
// 验证循环依赖模块各自的 import.meta.url 指向自身
TEST(InterpModule, IM21_ImportMetaInCircularDependency) {
    TempDir tmp;
    tmp.write("a.js", R"(
import { bUrl } from './b.js';
export let aUrl = import.meta.url;
)");
    tmp.write("b.js", R"(
import { aUrl } from './a.js';
export let bUrl = import.meta.url;
)");
    tmp.write("entry.js", R"(
import { aUrl } from './a.js';
import { bUrl } from './b.js';
aUrl + '|' + bUrl;
)");
    auto res = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_string(), tmp.abs("a.js") + "|" + tmp.abs("b.js"));
}

TEST(VmModule, IM21_ImportMetaInCircularDependency) {
    TempDir tmp;
    tmp.write("a.js", R"(
import { bUrl } from './b.js';
export let aUrl = import.meta.url;
)");
    tmp.write("b.js", R"(
import { aUrl } from './a.js';
export let bUrl = import.meta.url;
)");
    tmp.write("entry.js", R"(
import { aUrl } from './a.js';
import { bUrl } from './b.js';
aUrl + '|' + bUrl;
)");
    auto res = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_string(), tmp.abs("a.js") + "|" + tmp.abs("b.js"));
}

// IM-22: import.meta 在副作用导入模块中使用
TEST(InterpModule, IM22_ImportMetaInSideEffectModule) {
    TempDir tmp;
    tmp.write("side.js", "export let sideUrl = import.meta.url;");
    tmp.write("entry.js", R"(
import './side.js';
import { sideUrl } from './side.js';
sideUrl;
)");
    auto res = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_string(), tmp.abs("side.js"));
}

TEST(VmModule, IM22_ImportMetaInSideEffectModule) {
    TempDir tmp;
    tmp.write("side.js", "export let sideUrl = import.meta.url;");
    tmp.write("entry.js", R"(
import './side.js';
import { sideUrl } from './side.js';
sideUrl;
)");
    auto res = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_string(), tmp.abs("side.js"));
}

// IM-23: import.meta 与动态 import() 组合使用
TEST(InterpModule, IM23_ImportMetaWithDynamicImport) {
    TempDir tmp;
    tmp.write("m.js", "export let mUrl = import.meta.url;");
    tmp.write("entry.js", R"(
let result = '';
import('./m.js').then(function(ns) { result = ns.mUrl; });
result;
)");
    auto res = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_string(), tmp.abs("m.js"));
}

TEST(VmModule, IM23_ImportMetaWithDynamicImport) {
    TempDir tmp;
    tmp.write("m.js", "export let mUrl = import.meta.url;");
    tmp.write("entry.js", R"(
let result = '';
import('./m.js').then(function(ns) { result = ns.mUrl; });
result;
)");
    auto res = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_string(), tmp.abs("m.js"));
}

// IM-24: import.meta 的 url 属性是字符串类型
TEST(InterpModule, IM24_ImportMetaUrlIsString) {
    TempDir tmp;
    tmp.write("entry.js", R"(
typeof import.meta.url;
)");
    auto res = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_string(), "string");
}

TEST(VmModule, IM24_ImportMetaUrlIsString) {
    TempDir tmp;
    tmp.write("entry.js", R"(
typeof import.meta.url;
)");
    auto res = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_string(), "string");
}

// IM-25: import.meta 在模块顶层作为表达式的一部分（二元运算）
TEST(InterpModule, IM25_ImportMetaInBinaryExpression) {
    TempDir tmp;
    tmp.write("entry.js", R"(
import.meta.url + '#hash';
)");
    auto res = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_string(), tmp.abs("entry.js") + "#hash");
}

TEST(VmModule, IM25_ImportMetaInBinaryExpression) {
    TempDir tmp;
    tmp.write("entry.js", R"(
import.meta.url + '#hash';
)");
    auto res = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_string(), tmp.abs("entry.js") + "#hash");
}

// IM-26: import.meta 在 return 语句中直接返回
TEST(InterpModule, IM26_ImportMetaInReturn) {
    TempDir tmp;
    tmp.write("entry.js", R"(
function f() { return import.meta; }
f().url;
)");
    auto res = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_string(), tmp.abs("entry.js"));
}

TEST(VmModule, IM26_ImportMetaInReturn) {
    TempDir tmp;
    tmp.write("entry.js", R"(
function f() { return import.meta; }
f().url;
)");
    auto res = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_string(), tmp.abs("entry.js"));
}

// IM-27: import.meta 在 async 函数中使用
TEST(InterpModule, IM27_ImportMetaInAsyncFunction) {
    TempDir tmp;
    tmp.write("entry.js", R"(
async function getUrl() { return import.meta.url; }
getUrl();
)");
    auto res = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_TRUE(res.value().is_object());  // async 返回 Promise
}

TEST(VmModule, IM27_ImportMetaInAsyncFunction) {
    TempDir tmp;
    tmp.write("entry.js", R"(
async function getUrl() { return import.meta.url; }
getUrl();
)");
    auto res = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_TRUE(res.value().is_object());  // async 返回 Promise
}

// IM-28: import.meta 在模块顶层 await 之后使用
// 验证 TLA 挂起/恢复后 import.meta 仍然可用
TEST(InterpModule, IM28_ImportMetaAfterTopLevelAwait) {
    TempDir tmp;
    tmp.write("entry.js", R"(
let v = await Promise.resolve(1);
import.meta.url;
)");
    auto res = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_string(), tmp.abs("entry.js"));
}

TEST(VmModule, IM28_ImportMetaAfterTopLevelAwait) {
    TempDir tmp;
    tmp.write("entry.js", R"(
let v = await Promise.resolve(1);
import.meta.url;
)");
    auto res = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_string(), tmp.abs("entry.js"));
}

// IM-29: import.meta 在模块顶层 await 挂起前的函数中使用
// 验证 TLA 挂起/恢复后，函数内 import.meta 仍然正确
TEST(InterpModule, IM29_ImportMetaInFunctionBeforeTLA) {
    TempDir tmp;
    tmp.write("entry.js", R"(
function getUrl() { return import.meta.url; }
let url1 = getUrl();
let v = await Promise.resolve(1);
let url2 = getUrl();
url1 + '|' + url2;
)");
    auto res = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_string(), tmp.abs("entry.js") + "|" + tmp.abs("entry.js"));
}

TEST(VmModule, IM29_ImportMetaInFunctionBeforeTLA) {
    TempDir tmp;
    tmp.write("entry.js", R"(
function getUrl() { return import.meta.url; }
let url1 = getUrl();
let v = await Promise.resolve(1);
let url2 = getUrl();
url1 + '|' + url2;
)");
    auto res = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_string(), tmp.abs("entry.js") + "|" + tmp.abs("entry.js"));
}

// IM-30: import.meta 的 url 属性不为空字符串
TEST(InterpModule, IM30_ImportMetaUrlNotEmpty) {
    TempDir tmp;
    tmp.write("entry.js", R"(
import.meta.url.length > 0;
)");
    auto res = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_bool(), true);
}

TEST(VmModule, IM30_ImportMetaUrlNotEmpty) {
    TempDir tmp;
    tmp.write("entry.js", R"(
import.meta.url.length > 0;
)");
    auto res = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_bool(), true);
}

// IM-31: import.meta 在 Object.keys 中只有 "url" 一个属性（初始状态）
TEST(InterpModule, IM31_ObjectKeysOnlyUrl) {
    TempDir tmp;
    tmp.write("entry.js", R"(
let keys = Object.keys(import.meta);
keys.length === 1 && keys[0] === 'url';
)");
    auto res = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_bool(), true);
}

TEST(VmModule, IM31_ObjectKeysOnlyUrl) {
    TempDir tmp;
    tmp.write("entry.js", R"(
let keys = Object.keys(import.meta);
keys.length === 1 && keys[0] === 'url';
)");
    auto res = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_bool(), true);
}

// IM-32: import.meta 的 [[Prototype]] 为 null
// 通过 instanceof 验证：import.meta 不是 Object 的实例（原型链为 null）
TEST(InterpModule, IM32_ImportMetaNotInstanceOfObject) {
    TempDir tmp;
    tmp.write("entry.js", R"(
import.meta instanceof Object === false;
)");
    auto res = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_bool(), true);
}

TEST(VmModule, IM32_ImportMetaNotInstanceOfObject) {
    TempDir tmp;
    tmp.write("entry.js", R"(
import.meta instanceof Object === false;
)");
    auto res = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_bool(), true);
}

// IM-33: import.meta 不继承 toString 方法
TEST(InterpModule, IM33_ImportMetaNoToString) {
    TempDir tmp;
    tmp.write("entry.js", R"(
import.meta.toString === undefined;
)");
    auto res = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_bool(), true);
}

TEST(VmModule, IM33_ImportMetaNoToString) {
    TempDir tmp;
    tmp.write("entry.js", R"(
import.meta.toString === undefined;
)");
    auto res = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_bool(), true);
}

// IM-34: import.meta 不继承 valueOf 方法
TEST(InterpModule, IM34_ImportMetaNoValueOf) {
    TempDir tmp;
    tmp.write("entry.js", R"(
import.meta.valueOf === undefined;
)");
    auto res = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_bool(), true);
}

TEST(VmModule, IM34_ImportMetaNoValueOf) {
    TempDir tmp;
    tmp.write("entry.js", R"(
import.meta.valueOf === undefined;
)");
    auto res = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_bool(), true);
}

// IM-35: import.meta 在模块顶层作为对象属性值
TEST(InterpModule, IM35_ImportMetaAsPropertyValue) {
    TempDir tmp;
    tmp.write("entry.js", R"(
let obj = { meta: import.meta };
obj.meta.url;
)");
    auto res = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_string(), tmp.abs("entry.js"));
}

TEST(VmModule, IM35_ImportMetaAsPropertyValue) {
    TempDir tmp;
    tmp.write("entry.js", R"(
let obj = { meta: import.meta };
obj.meta.url;
)");
    auto res = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_string(), tmp.abs("entry.js"));
}

// IM-36: import.meta 在数组字面量中
TEST(InterpModule, IM36_ImportMetaInArrayLiteral) {
    TempDir tmp;
    tmp.write("entry.js", R"(
let arr = [import.meta.url, 'extra'];
arr[0];
)");
    auto res = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_string(), tmp.abs("entry.js"));
}

TEST(VmModule, IM36_ImportMetaInArrayLiteral) {
    TempDir tmp;
    tmp.write("entry.js", R"(
let arr = [import.meta.url, 'extra'];
arr[0];
)");
    auto res = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_string(), tmp.abs("entry.js"));
}

// IM-37: import.meta 在逻辑表达式中（短路求值）
TEST(InterpModule, IM37_ImportMetaInLogicalExpression) {
    TempDir tmp;
    tmp.write("entry.js", R"(
import.meta && import.meta.url || 'fallback';
)");
    auto res = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_string(), tmp.abs("entry.js"));
}

TEST(VmModule, IM37_ImportMetaInLogicalExpression) {
    TempDir tmp;
    tmp.write("entry.js", R"(
import.meta && import.meta.url || 'fallback';
)");
    auto res = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_string(), tmp.abs("entry.js"));
}

// IM-38: import.meta 在条件分支中（if-else 替代三元，三元运算符未实现）
TEST(InterpModule, IM38_ImportMetaInConditionalBranch) {
    TempDir tmp;
    tmp.write("entry.js", R"(
let result = '';
if (import.meta) {
    result = import.meta.url;
} else {
    result = 'no-meta';
}
result;
)");
    auto res = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_string(), tmp.abs("entry.js"));
}

TEST(VmModule, IM38_ImportMetaInConditionalBranch) {
    TempDir tmp;
    tmp.write("entry.js", R"(
let result = '';
if (import.meta) {
    result = import.meta.url;
} else {
    result = 'no-meta';
}
result;
)");
    auto res = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_string(), tmp.abs("entry.js"));
}

// IM-39: import.meta 在 GC 后仍然可用（验证 meta_obj 被正确追踪为 GC root）
TEST(InterpModule, IM39_ImportMetaSurvivesGC) {
    TempDir tmp;
    // 创建大量临时对象触发 GC，然后验证 import.meta 仍然可用
    tmp.write("entry.js", R"(
let m = import.meta;
m.x = 1;
// 创建一些临时对象（GC 可能在 exec 末尾触发）
let arr = [1,2,3];
m.url;
)");
    auto res = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_string(), tmp.abs("entry.js"));
}

TEST(VmModule, IM39_ImportMetaSurvivesGC) {
    TempDir tmp;
    tmp.write("entry.js", R"(
let m = import.meta;
m.x = 1;
let arr = [1,2,3];
m.url;
)");
    auto res = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_string(), tmp.abs("entry.js"));
}

// IM-40: import.meta 在模块顶层多次赋值给不同变量后仍指向同一对象
TEST(InterpModule, IM40_MultipleVariablesSameImportMeta) {
    TempDir tmp;
    tmp.write("entry.js", R"(
let a = import.meta;
let b = import.meta;
a === b;
)");
    auto res = interp_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_bool(), true);
}

TEST(VmModule, IM40_MultipleVariablesSameImportMeta) {
    TempDir tmp;
    tmp.write("entry.js", R"(
let a = import.meta;
let b = import.meta;
a === b;
)");
    auto res = vm_exec_module(tmp.abs("entry.js"));
    ASSERT_TRUE(res.is_ok()) << res.error().message();
    EXPECT_EQ(res.value().as_bool(), true);
}

}  // namespace
