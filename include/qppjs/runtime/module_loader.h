#pragma once

#include "qppjs/base/result.h"
#include "qppjs/runtime/module_record.h"
#include "qppjs/runtime/rc_object.h"

#include <string>
#include <unordered_map>

namespace qppjs {

class GcHeap;

class ModuleLoader {
public:
    using LoadResult = ParseResult<RcPtr<ModuleRecord>>;

    // 加载（或从缓存返回）一个模块。base_dir 为发起 import 的模块所在目录。
    LoadResult Load(const std::string& specifier, const std::string& base_dir);

    // 查找已缓存的模块（按绝对路径）
    ModuleRecord* FindCached(const std::string& resolved_path) const;

    // 将所有缓存模块的 module_env 和 exports 中的 Cell 加入 GC roots
    void TraceRoots(GcHeap& heap) const;

    // 清理所有模块环境中的函数引用（打破循环引用，在 Clear() 之前调用）
    void ClearModuleEnvs();

    // 清空缓存
    void Clear();

private:
    std::string ResolvePath(const std::string& specifier, const std::string& base_dir) const;

    std::unordered_map<std::string, RcPtr<ModuleRecord>> cache_;
};

}  // namespace qppjs
