#include "qppjs/runtime/module_loader.h"

#include "qppjs/frontend/parser.h"
#include "qppjs/runtime/environment.h"
#include "qppjs/runtime/gc_heap.h"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace qppjs {

namespace fs = std::filesystem;

std::string ModuleLoader::ResolvePath(const std::string& specifier,
                                       const std::string& base_dir) const {
    // 只支持相对路径（./xxx 或 ../xxx）
    if (specifier.empty() || (specifier[0] != '.' && specifier[0] != '/')) {
        return "";  // 裸模块说明符，不支持
    }
    fs::path base(base_dir);
    fs::path resolved = (base / specifier).lexically_normal();
    return resolved.string();
}

ModuleRecord* ModuleLoader::FindCached(const std::string& resolved_path) const {
    auto it = cache_.find(resolved_path);
    if (it != cache_.end()) {
        return it->second.get();
    }
    return nullptr;
}

ModuleLoader::LoadResult ModuleLoader::Load(const std::string& specifier,
                                             const std::string& base_dir) {
    std::string resolved = ResolvePath(specifier, base_dir);
    if (resolved.empty()) {
        return LoadResult::Err(Error{ErrorKind::Runtime,
                                     "Error: Cannot resolve module specifier '" + specifier + "'"});
    }

    // 缓存命中
    auto it = cache_.find(resolved);
    if (it != cache_.end()) {
        return LoadResult::Ok(it->second);
    }

    // 先插入空 ModuleRecord（防止循环依赖死循环）
    auto mod = RcPtr<ModuleRecord>::make();
    mod->specifier = resolved;
    cache_[resolved] = mod;

    // 读取文件
    std::ifstream file(resolved);
    if (!file.is_open()) {
        cache_.erase(resolved);
        return LoadResult::Err(Error{ErrorKind::Runtime,
                                     "Error: Cannot open module file '" + resolved + "'"});
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    std::string source = ss.str();

    // 解析 AST
    auto parse_result = parse_program(source);
    if (!parse_result.ok()) {
        cache_.erase(resolved);
        return LoadResult::Err(parse_result.error());
    }
    mod->ast = std::move(parse_result.value());

    // 提取 requested_modules 和 re-export 条目
    for (const auto& stmt : mod->ast.body) {
        if (const auto* imp = std::get_if<ImportDeclaration>(&stmt.v)) {
            mod->requested_modules.push_back(imp->specifier);
        } else if (const auto* exp = std::get_if<ExportNamedDeclaration>(&stmt.v)) {
            if (exp->source.has_value()) {
                // re-export：export { x as y } from './a.js'
                mod->requested_modules.push_back(*exp->source);
                for (const auto& spec : exp->specifiers) {
                    mod->re_exports.push_back(ModuleRecord::ReExportEntry{
                        spec.export_name,
                        *exp->source,
                        spec.local_name,
                    });
                }
            }
        }
    }

    // 预分配导出 Cell（初始值 undefined）
    for (const auto& stmt : mod->ast.body) {
        if (const auto* exp = std::get_if<ExportNamedDeclaration>(&stmt.v)) {
            if (exp->source.has_value()) {
                // re-export：Cell 在 Link 阶段从依赖模块获取，不在这里分配
                continue;
            }
            if (exp->declaration) {
                // export let/const/var x = ...  或  export function/async function f() {}
                std::string name;
                if (const auto* vd = std::get_if<VariableDeclaration>(&exp->declaration->v)) {
                    name = vd->name;
                } else if (const auto* fd = std::get_if<FunctionDeclaration>(&exp->declaration->v)) {
                    name = fd->name;
                } else if (const auto* afd = std::get_if<AsyncFunctionDeclaration>(&exp->declaration->v)) {
                    name = afd->name;
                }
                if (!name.empty() && mod->find_export(name) == nullptr) {
                    mod->exports.push_back(ModuleRecord::ExportEntry{
                        name, RcPtr<Cell>::make(Cell{Value::undefined()})});
                }
            } else {
                // export { x as y }（本地 specifiers，无 source）：live binding
                // 为 export_name 分配 Cell（如果尚未存在）
                // Link 阶段将该 Cell 以 local_name 为 key 注入 module_env，实现共享
                for (const auto& spec : exp->specifiers) {
                    if (mod->find_export(spec.export_name) == nullptr) {
                        mod->exports.push_back(ModuleRecord::ExportEntry{
                            spec.export_name, RcPtr<Cell>::make(Cell{Value::undefined()})});
                    }
                }
            }
        } else if (std::holds_alternative<ExportDefaultDeclaration>(stmt.v)) {
            if (mod->find_export("default") == nullptr) {
                mod->exports.push_back(ModuleRecord::ExportEntry{
                    "default", RcPtr<Cell>::make(Cell{Value::undefined()})});
            }
        }
    }

    return LoadResult::Ok(mod);
}

void ModuleLoader::TraceRoots(GcHeap& heap) const {
    for (const auto& [path, mod] : cache_) {
        if (mod) {
            heap.MarkPending(mod.get());
        }
    }
}

void ModuleLoader::ClearModuleEnvs() {
    for (const auto& [path, mod] : cache_) {
        if (mod) {
            if (mod->module_env) {
                mod->module_env->clear_function_bindings();
            }
            // 打破 ModuleRecord 之间的循环引用（循环依赖场景）
            mod->dependencies.clear();
        }
    }
}

void ModuleLoader::Clear() {
    cache_.clear();
}

}  // namespace qppjs
