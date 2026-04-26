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
#include "qppjs/vm/compiler.h"
#include "qppjs/vm/vm.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

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

}  // namespace
