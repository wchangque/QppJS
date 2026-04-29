#include "qppjs/frontend/parser.h"

#include "qppjs/frontend/ast.h"

#include <gtest/gtest.h>

using namespace qppjs;

// ---- 辅助宏 ----

#define ASSERT_PARSE_OK(src, prog_var)                    \
    auto _res_##prog_var = parse_program(src);            \
    ASSERT_TRUE(_res_##prog_var.ok()) << _res_##prog_var.error().message(); \
    const Program& prog_var = _res_##prog_var.value()

#define ASSERT_PARSE_ERR(src)                  \
    do {                                       \
        auto _r = parse_program(src);          \
        ASSERT_FALSE(_r.ok());                 \
    } while (0)

// ---- import 合法形式 ----

TEST(ParserModule, ImportSideEffect) {
    ASSERT_PARSE_OK("import './m';", prog);
    ASSERT_EQ(prog.body.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<ImportDeclaration>(prog.body[0].v));
    const auto& decl = std::get<ImportDeclaration>(prog.body[0].v);
    EXPECT_EQ(decl.specifier, "./m");
    EXPECT_TRUE(decl.specifiers.empty());
}

TEST(ParserModule, ImportNamed) {
    ASSERT_PARSE_OK("import { x } from './m';", prog);
    ASSERT_EQ(prog.body.size(), 1u);
    const auto& decl = std::get<ImportDeclaration>(prog.body[0].v);
    EXPECT_EQ(decl.specifier, "./m");
    ASSERT_EQ(decl.specifiers.size(), 1u);
    EXPECT_EQ(decl.specifiers[0].imported_name, "x");
    EXPECT_EQ(decl.specifiers[0].local_name, "x");
    EXPECT_FALSE(decl.specifiers[0].is_namespace);
}

TEST(ParserModule, ImportNamedAs) {
    ASSERT_PARSE_OK("import { x as y } from './m';", prog);
    const auto& decl = std::get<ImportDeclaration>(prog.body[0].v);
    ASSERT_EQ(decl.specifiers.size(), 1u);
    EXPECT_EQ(decl.specifiers[0].imported_name, "x");
    EXPECT_EQ(decl.specifiers[0].local_name, "y");
}

TEST(ParserModule, ImportDefault) {
    ASSERT_PARSE_OK("import defaultExport from './m';", prog);
    const auto& decl = std::get<ImportDeclaration>(prog.body[0].v);
    ASSERT_EQ(decl.specifiers.size(), 1u);
    EXPECT_EQ(decl.specifiers[0].imported_name, "default");
    EXPECT_EQ(decl.specifiers[0].local_name, "defaultExport");
    EXPECT_FALSE(decl.specifiers[0].is_namespace);
}

TEST(ParserModule, ImportNamespace) {
    ASSERT_PARSE_OK("import * as ns from './m';", prog);
    const auto& decl = std::get<ImportDeclaration>(prog.body[0].v);
    ASSERT_EQ(decl.specifiers.size(), 1u);
    EXPECT_EQ(decl.specifiers[0].imported_name, "*");
    EXPECT_EQ(decl.specifiers[0].local_name, "ns");
    EXPECT_TRUE(decl.specifiers[0].is_namespace);
}

TEST(ParserModule, ImportMultipleNamed) {
    ASSERT_PARSE_OK("import { x, y } from './m';", prog);
    const auto& decl = std::get<ImportDeclaration>(prog.body[0].v);
    ASSERT_EQ(decl.specifiers.size(), 2u);
    EXPECT_EQ(decl.specifiers[0].local_name, "x");
    EXPECT_EQ(decl.specifiers[1].local_name, "y");
}

// ---- export 合法形式 ----

TEST(ParserModule, ExportNamedConst) {
    ASSERT_PARSE_OK("export const x = 1;", prog);
    ASSERT_EQ(prog.body.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<ExportNamedDeclaration>(prog.body[0].v));
    const auto& decl = std::get<ExportNamedDeclaration>(prog.body[0].v);
    ASSERT_NE(decl.declaration, nullptr);
    ASSERT_TRUE(std::holds_alternative<VariableDeclaration>(decl.declaration->v));
    EXPECT_TRUE(decl.specifiers.empty());
}

TEST(ParserModule, ExportNamedFunction) {
    ASSERT_PARSE_OK("export function foo() {}", prog);
    const auto& decl = std::get<ExportNamedDeclaration>(prog.body[0].v);
    ASSERT_NE(decl.declaration, nullptr);
    ASSERT_TRUE(std::holds_alternative<FunctionDeclaration>(decl.declaration->v));
}

TEST(ParserModule, ExportSpecifiers) {
    ASSERT_PARSE_OK("export { x, y as z };", prog);
    const auto& decl = std::get<ExportNamedDeclaration>(prog.body[0].v);
    EXPECT_EQ(decl.declaration, nullptr);
    ASSERT_EQ(decl.specifiers.size(), 2u);
    EXPECT_EQ(decl.specifiers[0].local_name, "x");
    EXPECT_EQ(decl.specifiers[0].export_name, "x");
    EXPECT_EQ(decl.specifiers[1].local_name, "y");
    EXPECT_EQ(decl.specifiers[1].export_name, "z");
}

TEST(ParserModule, ExportDefaultExpr) {
    ASSERT_PARSE_OK("export default 42;", prog);
    ASSERT_TRUE(std::holds_alternative<ExportDefaultDeclaration>(prog.body[0].v));
    const auto& decl = std::get<ExportDefaultDeclaration>(prog.body[0].v);
    ASSERT_NE(decl.expression, nullptr);
    ASSERT_TRUE(std::holds_alternative<NumberLiteral>(decl.expression->v));
}

TEST(ParserModule, ExportDefaultAnonFunction) {
    ASSERT_PARSE_OK("export default function() {}", prog);
    const auto& decl = std::get<ExportDefaultDeclaration>(prog.body[0].v);
    ASSERT_NE(decl.expression, nullptr);
    ASSERT_TRUE(std::holds_alternative<FunctionExpression>(decl.expression->v));
    const auto& fe = std::get<FunctionExpression>(decl.expression->v);
    EXPECT_FALSE(fe.name.has_value());
}

TEST(ParserModule, ExportDefaultNamedFunction) {
    ASSERT_PARSE_OK("export default function foo() {}", prog);
    const auto& decl = std::get<ExportDefaultDeclaration>(prog.body[0].v);
    ASSERT_TRUE(std::holds_alternative<FunctionExpression>(decl.expression->v));
    const auto& fe = std::get<FunctionExpression>(decl.expression->v);
    ASSERT_TRUE(fe.name.has_value());
    EXPECT_EQ(*fe.name, "foo");
}

// ---- from/as 作为普通变量名不报错 ----

TEST(ParserModule, FromAsOrdinaryIdent) {
    // from 和 as 不是关键字，可作为普通标识符
    auto r1 = parse_program("let from = 1;");
    EXPECT_TRUE(r1.ok());
    auto r2 = parse_program("let as = 2;");
    EXPECT_TRUE(r2.ok());
}

// ---- import/export 在函数体内报 SyntaxError ----

TEST(ParserModule, ImportInFunctionBody) {
    ASSERT_PARSE_ERR("function f() { import './m'; }");
}

TEST(ParserModule, ExportInFunctionBody) {
    ASSERT_PARSE_ERR("function f() { export const x = 1; }");
}

TEST(ParserModule, ImportInBlockStmt) {
    ASSERT_PARSE_ERR("{ import './m'; }");
}

TEST(ParserModule, ExportInBlockStmt) {
    ASSERT_PARSE_ERR("{ export const x = 1; }");
}

// ---- 重复导出名报 SyntaxError ----

TEST(ParserModule, DuplicateExportName) {
    ASSERT_PARSE_ERR("export const x = 1; export const x = 2;");
}

TEST(ParserModule, DuplicateExportSpecifier) {
    ASSERT_PARSE_ERR("export { x }; export { x };");
}

TEST(ParserModule, DuplicateExportDefault) {
    ASSERT_PARSE_ERR("export default 1; export default 2;");
}

// ---- import 边界：语法错误路径 ----

// import { 后缺少 } 应报错
TEST(ParserModule, ImportNamedMissingCloseBrace) {
    ASSERT_PARSE_ERR("import { x from './m';");
}

// import { x as } 缺少 local name 应报错
TEST(ParserModule, ImportNamedAsMissingLocalName) {
    ASSERT_PARSE_ERR("import { x as } from './m';");
}

// import * 后缺少 as 应报错
TEST(ParserModule, ImportNamespaceNoAs) {
    ASSERT_PARSE_ERR("import * from './m';");
}

// import * as 后缺少标识符应报错
TEST(ParserModule, ImportNamespaceAsMissingIdent) {
    ASSERT_PARSE_ERR("import * as from './m';");
}

// import defaultExport 后缺少 from 应报错
TEST(ParserModule, ImportDefaultMissingFrom) {
    ASSERT_PARSE_ERR("import defaultExport './m';");
}

// import { x } 后缺少 from 应报错
TEST(ParserModule, ImportNamedMissingFrom) {
    ASSERT_PARSE_ERR("import { x } './m';");
}

// import { x } from 后缺少字符串 specifier 应报错
TEST(ParserModule, ImportFromMissingSpecifier) {
    ASSERT_PARSE_ERR("import { x } from ;");
}

// import 后跟非法 token 应报错
TEST(ParserModule, ImportUnexpectedToken) {
    ASSERT_PARSE_ERR("import 42 from './m';");
}

// ---- import 边界：合法形式细节 ----

// 空具名列表 import {} from '...' 应合法
TEST(ParserModule, ImportEmptyNamedList) {
    ASSERT_PARSE_OK("import {} from './m';", prog);
    const auto& decl = std::get<ImportDeclaration>(prog.body[0].v);
    EXPECT_EQ(decl.specifier, "./m");
    EXPECT_TRUE(decl.specifiers.empty());
}

// import 多个具名，含重命名混合
TEST(ParserModule, ImportMultipleNamedWithAlias) {
    ASSERT_PARSE_OK("import { a, b as c, d } from './m';", prog);
    const auto& decl = std::get<ImportDeclaration>(prog.body[0].v);
    ASSERT_EQ(decl.specifiers.size(), 3u);
    EXPECT_EQ(decl.specifiers[0].imported_name, "a");
    EXPECT_EQ(decl.specifiers[0].local_name, "a");
    EXPECT_EQ(decl.specifiers[1].imported_name, "b");
    EXPECT_EQ(decl.specifiers[1].local_name, "c");
    EXPECT_EQ(decl.specifiers[2].imported_name, "d");
    EXPECT_EQ(decl.specifiers[2].local_name, "d");
}

// import 尾随逗号应合法：import { x, } from '...'
TEST(ParserModule, ImportNamedTrailingComma) {
    ASSERT_PARSE_OK("import { x, } from './m';", prog);
    const auto& decl = std::get<ImportDeclaration>(prog.body[0].v);
    ASSERT_EQ(decl.specifiers.size(), 1u);
    EXPECT_EQ(decl.specifiers[0].local_name, "x");
}

// from 作为 import 的 local name（contextual keyword，可作本地绑定名）
TEST(ParserModule, ImportLocalNameFrom) {
    ASSERT_PARSE_OK("import { x as from } from './m';", prog);
    const auto& decl = std::get<ImportDeclaration>(prog.body[0].v);
    ASSERT_EQ(decl.specifiers.size(), 1u);
    EXPECT_EQ(decl.specifiers[0].imported_name, "x");
    EXPECT_EQ(decl.specifiers[0].local_name, "from");
}

// import 副作用导入，specifier 含路径分隔符
TEST(ParserModule, ImportSideEffectPathSpecifier) {
    ASSERT_PARSE_OK("import '../lib/util.js';", prog);
    const auto& decl = std::get<ImportDeclaration>(prog.body[0].v);
    EXPECT_EQ(decl.specifier, "../lib/util.js");
    EXPECT_TRUE(decl.specifiers.empty());
}

// ---- export 边界：语法错误路径 ----

// export { 后缺少 } 应报错
TEST(ParserModule, ExportSpecifierMissingCloseBrace) {
    ASSERT_PARSE_ERR("export { x ;");
}

// export { x as } 缺少 export name 应报错
TEST(ParserModule, ExportSpecifierAsMissingName) {
    ASSERT_PARSE_ERR("export { x as };");
}

// export 后跟非法 token 应报错
TEST(ParserModule, ExportUnexpectedToken) {
    ASSERT_PARSE_ERR("export 42;");
}

// export let 无初始化器（const 必须有，let 可以没有）
TEST(ParserModule, ExportNamedLet) {
    ASSERT_PARSE_OK("export let x;", prog);
    const auto& decl = std::get<ExportNamedDeclaration>(prog.body[0].v);
    ASSERT_NE(decl.declaration, nullptr);
    const auto& vd = std::get<VariableDeclaration>(decl.declaration->v);
    EXPECT_EQ(vd.name, "x");
    EXPECT_EQ(vd.kind, VarKind::Let);
    EXPECT_FALSE(vd.init.has_value());
}

// export var 形式
TEST(ParserModule, ExportNamedVar) {
    ASSERT_PARSE_OK("export var y = 2;", prog);
    const auto& decl = std::get<ExportNamedDeclaration>(prog.body[0].v);
    ASSERT_NE(decl.declaration, nullptr);
    const auto& vd = std::get<VariableDeclaration>(decl.declaration->v);
    EXPECT_EQ(vd.name, "y");
    EXPECT_EQ(vd.kind, VarKind::Var);
}

// export {} 空列表应合法
TEST(ParserModule, ExportEmptySpecifiers) {
    ASSERT_PARSE_OK("export {};", prog);
    const auto& decl = std::get<ExportNamedDeclaration>(prog.body[0].v);
    EXPECT_EQ(decl.declaration, nullptr);
    EXPECT_TRUE(decl.specifiers.empty());
}

// export { x as y }：export_name 与 local_name 均正确
TEST(ParserModule, ExportSpecifierAlias) {
    ASSERT_PARSE_OK("export { foo as bar };", prog);
    const auto& decl = std::get<ExportNamedDeclaration>(prog.body[0].v);
    ASSERT_EQ(decl.specifiers.size(), 1u);
    EXPECT_EQ(decl.specifiers[0].local_name, "foo");
    EXPECT_EQ(decl.specifiers[0].export_name, "bar");
}

// export default 对象字面量（括号包裹，因对象字面量在表达式位置需要括号）
TEST(ParserModule, ExportDefaultObject) {
    ASSERT_PARSE_OK("export default ({ a: 1 });", prog);
    const auto& decl = std::get<ExportDefaultDeclaration>(prog.body[0].v);
    ASSERT_NE(decl.expression, nullptr);
    ASSERT_TRUE(std::holds_alternative<ObjectExpression>(decl.expression->v));
}

// export default 字符串字面量
TEST(ParserModule, ExportDefaultString) {
    ASSERT_PARSE_OK("export default 'hello';", prog);
    const auto& decl = std::get<ExportDefaultDeclaration>(prog.body[0].v);
    ASSERT_NE(decl.expression, nullptr);
    ASSERT_TRUE(std::holds_alternative<StringLiteral>(decl.expression->v));
    EXPECT_EQ(std::get<StringLiteral>(decl.expression->v).value, "hello");
}

// export default 带参数的具名函数，name 正确捕获
TEST(ParserModule, ExportDefaultNamedFunctionWithParams) {
    ASSERT_PARSE_OK("export default function add(a, b) { return a + b; }", prog);
    const auto& decl = std::get<ExportDefaultDeclaration>(prog.body[0].v);
    ASSERT_TRUE(std::holds_alternative<FunctionExpression>(decl.expression->v));
    const auto& fe = std::get<FunctionExpression>(decl.expression->v);
    ASSERT_TRUE(fe.name.has_value());
    EXPECT_EQ(*fe.name, "add");
    ASSERT_EQ(fe.params.size(), 2u);
    EXPECT_EQ(fe.params[0], "a");
    EXPECT_EQ(fe.params[1], "b");
}

// ---- 重复导出：具名声明与 specifier 混合 ----

// export const x 与 export { x } 重复应报错
TEST(ParserModule, DuplicateExportDeclAndSpecifier) {
    ASSERT_PARSE_ERR("export const x = 1; export { x };");
}

// export function foo 与 export { foo } 重复应报错
TEST(ParserModule, DuplicateExportFunctionAndSpecifier) {
    ASSERT_PARSE_ERR("export function foo() {} export { foo };");
}

// export default 与 export { default as x } 不冲突（export_name 不同）
TEST(ParserModule, ExportDefaultAndSpecifierNoDuplicate) {
    ASSERT_PARSE_OK("export default 1; export { x };", prog);
    EXPECT_EQ(prog.body.size(), 2u);
}

// ---- is_top_level_ 嵌套上下文 ----

// import 在 if 体内应报错（if 体由 parse_stmt 进入，is_top_level_ 仍为 true 时……
// 但 parse_block_stmt 会设置 is_top_level_=false；if 的 consequent 直接调用 parse_stmt
// 不走 block，is_top_level_ 仍为 true。验证实际行为：应报错
TEST(ParserModule, ImportInIfBody) {
    ASSERT_PARSE_ERR("if (true) import './m';");
}

TEST(ParserModule, ExportInIfBody) {
    ASSERT_PARSE_ERR("if (true) export const x = 1;");
}

// import/export 在 while 体内应报错
TEST(ParserModule, ImportInWhileBody) {
    ASSERT_PARSE_ERR("while (false) import './m';");
}

TEST(ParserModule, ExportInWhileBody) {
    ASSERT_PARSE_ERR("while (false) export const x = 1;");
}

// import/export 在 for 体内应报错
TEST(ParserModule, ImportInForBody) {
    ASSERT_PARSE_ERR("for (;;) import './m';");
}

// import/export 在 try/catch 体内应报错（parse_block 设置 is_top_level_=false）
TEST(ParserModule, ImportInTryBlock) {
    ASSERT_PARSE_ERR("try { import './m'; } catch(e) {}");
}

TEST(ParserModule, ExportInCatchBlock) {
    ASSERT_PARSE_ERR("try {} catch(e) { export const x = 1; }");
}

// ---- ASI（自动分号插入）行为 ----

// import 副作用导入，无显式分号，换行触发 ASI
TEST(ParserModule, ImportSideEffectASI) {
    ASSERT_PARSE_OK("import './m'\n", prog);
    ASSERT_EQ(prog.body.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<ImportDeclaration>(prog.body[0].v));
}

// export default 表达式，无显式分号，换行触发 ASI
TEST(ParserModule, ExportDefaultASI) {
    ASSERT_PARSE_OK("export default 42\n", prog);
    ASSERT_EQ(prog.body.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<ExportDefaultDeclaration>(prog.body[0].v));
}

// ---- 多语句顺序与 body 计数 ----

// 多个 import 语句顺序正确
TEST(ParserModule, MultipleImports) {
    ASSERT_PARSE_OK("import './a'; import './b'; import './c';", prog);
    ASSERT_EQ(prog.body.size(), 3u);
    EXPECT_EQ(std::get<ImportDeclaration>(prog.body[0].v).specifier, "./a");
    EXPECT_EQ(std::get<ImportDeclaration>(prog.body[1].v).specifier, "./b");
    EXPECT_EQ(std::get<ImportDeclaration>(prog.body[2].v).specifier, "./c");
}

// import 与普通语句混合，顺序正确
TEST(ParserModule, ImportMixedWithOtherStmts) {
    ASSERT_PARSE_OK("import './m'; const x = 1; export { x };", prog);
    ASSERT_EQ(prog.body.size(), 3u);
    EXPECT_TRUE(std::holds_alternative<ImportDeclaration>(prog.body[0].v));
    EXPECT_TRUE(std::holds_alternative<VariableDeclaration>(prog.body[1].v));
    EXPECT_TRUE(std::holds_alternative<ExportNamedDeclaration>(prog.body[2].v));
}

// ---- default 作为上下文关键字 ----

// default 不是硬关键字，可作为标识符（非 import/export 上下文）
TEST(ParserModule, DefaultAsOrdinaryIdent) {
    auto r = parse_program("let default_ = 1;");
    EXPECT_TRUE(r.ok());
}

// ---- export specifier 尾随逗号 ----
TEST(ParserModule, ExportSpecifierTrailingComma) {
    ASSERT_PARSE_OK("export { x, y, };", prog);
    const auto& decl = std::get<ExportNamedDeclaration>(prog.body[0].v);
    ASSERT_EQ(decl.specifiers.size(), 2u);
    EXPECT_EQ(decl.specifiers[0].local_name, "x");
    EXPECT_EQ(decl.specifiers[1].local_name, "y");
}

// ---- M2 回归：import/export 作为属性名合法 ----

TEST(ParserModule, ImportAsObjectKey) {
    // ({ import: 1 }).import 不应报错
    auto r = parse_program("({ import: 1 }).import;");
    EXPECT_TRUE(r.ok()) << r.error().message();
}

TEST(ParserModule, ExportAsDotAccess) {
    // obj.export 不应报错
    auto r = parse_program("let obj = {}; obj.export;");
    EXPECT_TRUE(r.ok()) << r.error().message();
}

// ---- M3 回归：labeled statement 内不允许 import/export ----

TEST(ParserModule, LabeledImportIsError) {
    // label: import './m' 应解析失败
    ASSERT_PARSE_ERR("label: import './m';");
}

TEST(ParserModule, LabeledExportIsError) {
    // label: export const x = 1 应解析失败
    ASSERT_PARSE_ERR("label: export const x = 1;");
}

TEST(ParserModule, ImportExportAsIdentifierInFunctionBody) {
    // 函数体内 import/export 作为普通标识符不报错
    auto r = parse_program("function f() { var import = 1; var export = 2; return import + export; }");
    EXPECT_TRUE(r.ok()) << r.error().message();
}

// ---- export async function ----

TEST(ParserModule, ExportAsyncFunction) {
    ASSERT_PARSE_OK("export async function foo() {}", prog);
    const auto& decl = std::get<ExportNamedDeclaration>(prog.body[0].v);
    ASSERT_NE(decl.declaration, nullptr);
    ASSERT_TRUE(std::holds_alternative<AsyncFunctionDeclaration>(decl.declaration->v));
    const auto& afd = std::get<AsyncFunctionDeclaration>(decl.declaration->v);
    EXPECT_EQ(afd.name, "foo");
}

TEST(ParserModule, ExportDefaultAnonAsyncFunction) {
    ASSERT_PARSE_OK("export default async function() {}", prog);
    const auto& decl = std::get<ExportDefaultDeclaration>(prog.body[0].v);
    ASSERT_NE(decl.expression, nullptr);
    ASSERT_TRUE(std::holds_alternative<AsyncFunctionExpression>(decl.expression->v));
    const auto& afe = std::get<AsyncFunctionExpression>(decl.expression->v);
    EXPECT_FALSE(afe.name.has_value());
}

TEST(ParserModule, ExportDefaultNamedAsyncFunction) {
    ASSERT_PARSE_OK("export default async function named() {}", prog);
    const auto& decl = std::get<ExportDefaultDeclaration>(prog.body[0].v);
    ASSERT_NE(decl.expression, nullptr);
    ASSERT_TRUE(std::holds_alternative<AsyncFunctionExpression>(decl.expression->v));
    const auto& afe = std::get<AsyncFunctionExpression>(decl.expression->v);
    ASSERT_TRUE(afe.name.has_value());
    EXPECT_EQ(*afe.name, "named");
}

// M-1 修复回归：export default async function foo() {} 的 local_name 应为 "foo"
TEST(ParserModule, ExportDefaultNamedAsyncFunctionLocalName) {
    ASSERT_PARSE_OK("export default async function foo() {}", prog);
    const auto& decl = std::get<ExportDefaultDeclaration>(prog.body[0].v);
    ASSERT_TRUE(decl.local_name.has_value());
    EXPECT_EQ(*decl.local_name, "foo");
}

// 匿名 export default async function 的 local_name 应为 nullopt
TEST(ParserModule, ExportDefaultAnonAsyncFunctionNoLocalName) {
    ASSERT_PARSE_OK("export default async function() {}", prog);
    const auto& decl = std::get<ExportDefaultDeclaration>(prog.body[0].v);
    EXPECT_FALSE(decl.local_name.has_value());
}

// export async 后跟换行再 function 不是 async function 声明（ASI 语义）
TEST(ParserModule, ExportAsyncWithNewlineIsError) {
    // async\nfunction 之间有换行，不应被解析为 async function 声明
    ASSERT_PARSE_ERR("export async\nfunction foo() {}");
}

// 回归：export function 仍然正常工作
TEST(ParserModule, ExportFunctionRegression) {
    ASSERT_PARSE_OK("export function bar() { return 1; }", prog);
    const auto& decl = std::get<ExportNamedDeclaration>(prog.body[0].v);
    ASSERT_NE(decl.declaration, nullptr);
    ASSERT_TRUE(std::holds_alternative<FunctionDeclaration>(decl.declaration->v));
}

// 回归：顶层 async function 声明仍然正常工作
TEST(ParserModule, TopLevelAsyncFunctionRegression) {
    ASSERT_PARSE_OK("async function baz() { return 1; }", prog);
    ASSERT_EQ(prog.body.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<AsyncFunctionDeclaration>(prog.body[0].v));
}

// ---- export async function 错误路径 ----

// export async 后没有 function（跟普通标识符）→ 报错
TEST(ParserModule, ExportAsyncNoFunction) {
    ASSERT_PARSE_ERR("export async foo() {}");
}

// export async 后没有任何 token（EOF）→ 报错
TEST(ParserModule, ExportAsyncEof) {
    ASSERT_PARSE_ERR("export async");
}

// export async function 后没有函数名 → 报错（具名导出必须有名字）
TEST(ParserModule, ExportAsyncFunctionNoName) {
    ASSERT_PARSE_ERR("export async function() {}");
}

// export async function foo 后没有 { → 报错（语法错误）
TEST(ParserModule, ExportAsyncFunctionMissingBody) {
    ASSERT_PARSE_ERR("export async function foo");
}

// export async function 参数列表不完整 → 报错
TEST(ParserModule, ExportAsyncFunctionMissingCloseParen) {
    ASSERT_PARSE_ERR("export async function foo(a {}");
}

// ---- export async function 合法边界 ----

// export async function 带参数，params 正确捕获
TEST(ParserModule, ExportAsyncFunctionWithParams) {
    ASSERT_PARSE_OK("export async function add(a, b) {}", prog);
    const auto& decl = std::get<ExportNamedDeclaration>(prog.body[0].v);
    ASSERT_NE(decl.declaration, nullptr);
    ASSERT_TRUE(std::holds_alternative<AsyncFunctionDeclaration>(decl.declaration->v));
    const auto& afd = std::get<AsyncFunctionDeclaration>(decl.declaration->v);
    EXPECT_EQ(afd.name, "add");
    ASSERT_EQ(afd.params.size(), 2u);
    EXPECT_EQ(afd.params[0], "a");
    EXPECT_EQ(afd.params[1], "b");
}

// export async function 与 export function 在同一模块顶层，两个节点均正确
TEST(ParserModule, ExportAsyncAndSyncFunctionCoexist) {
    ASSERT_PARSE_OK("export async function asyncFoo() {} export function syncFoo() {}", prog);
    ASSERT_EQ(prog.body.size(), 2u);
    const auto& decl0 = std::get<ExportNamedDeclaration>(prog.body[0].v);
    ASSERT_TRUE(std::holds_alternative<AsyncFunctionDeclaration>(decl0.declaration->v));
    const auto& decl1 = std::get<ExportNamedDeclaration>(prog.body[1].v);
    ASSERT_TRUE(std::holds_alternative<FunctionDeclaration>(decl1.declaration->v));
}

// 同一模块导出两个 async function，两个节点均正确
TEST(ParserModule, TwoExportAsyncFunctions) {
    ASSERT_PARSE_OK("export async function f1() {} export async function f2() {}", prog);
    ASSERT_EQ(prog.body.size(), 2u);
    const auto& afd1 = std::get<AsyncFunctionDeclaration>(
        std::get<ExportNamedDeclaration>(prog.body[0].v).declaration->v);
    EXPECT_EQ(afd1.name, "f1");
    const auto& afd2 = std::get<AsyncFunctionDeclaration>(
        std::get<ExportNamedDeclaration>(prog.body[1].v).declaration->v);
    EXPECT_EQ(afd2.name, "f2");
}

// export default async function 后跟换行再 function 应报错（ASI 规则）
TEST(ParserModule, ExportDefaultAsyncWithNewlineIsError) {
    ASSERT_PARSE_ERR("export default async\nfunction() {}");
}

// export async function 重复导出名 → 报错
TEST(ParserModule, ExportAsyncFunctionDuplicateName) {
    ASSERT_PARSE_ERR("export async function foo() {} export async function foo() {}");
}

// export async function 与 export function 重复名 → 报错
TEST(ParserModule, ExportAsyncFunctionDuplicateWithSync) {
    ASSERT_PARSE_ERR("export async function foo() {} export function foo() {}");
}
