#pragma once

#include "qppjs/frontend/ast.h"
#include "qppjs/runtime/environment.h"
#include "qppjs/runtime/gc_heap.h"
#include "qppjs/runtime/js_object.h"
#include "qppjs/runtime/rc_object.h"
#include "qppjs/runtime/value.h"

#include <optional>
#include <string>
#include <vector>

namespace qppjs {

enum class ModuleStatus {
    kUnlinked,
    kLinking,
    kLinked,
    kEvaluating,
    kEvaluated,
    kErrored,
};

class ModuleRecord : public RcObject {
public:
    ModuleRecord() : RcObject(ObjectKind::kModule) {}

    void TraceRefs(GcHeap& heap) override;
    void ClearRefs() override;

    // 按名字查找导出 Cell（线性查找）
    Cell* find_export(const std::string& name) const;

    // ---- 字段 ----

    std::string specifier;           // 绝对路径
    ModuleStatus status = ModuleStatus::kUnlinked;
    Program ast;
    std::vector<std::string> requested_modules;  // 依赖的模块 specifier（原始字符串）

    // 导出表：线性小表
    struct ExportEntry {
        std::string name;
        RcPtr<Cell> cell;
    };
    std::vector<ExportEntry> exports;

    // re-export 条目（source 非空的 ExportNamedDeclaration 解析出来的）
    struct ReExportEntry {
        std::string export_name;       // 本模块导出的名字
        std::string source_specifier;  // 来源模块原始 specifier
        std::string import_name;       // 来源模块的导出名
    };
    std::vector<ReExportEntry> re_exports;

    RcPtr<JSObject> meta_obj;  // import.meta 对象，Link 阶段创建
    RcPtr<Environment> module_env;
    std::vector<RcPtr<ModuleRecord>> dependencies;
    std::optional<Value> eval_exception;  // 执行失败时缓存的错误值（kErrored 状态）
};

}  // namespace qppjs
