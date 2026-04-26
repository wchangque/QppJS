#include "qppjs/runtime/module_record.h"

#include "qppjs/runtime/gc_heap.h"

namespace qppjs {

Cell* ModuleRecord::find_export(const std::string& name) const {
    for (const auto& entry : exports) {
        if (entry.name == name) {
            return entry.cell.get();
        }
    }
    return nullptr;
}

void ModuleRecord::TraceRefs(GcHeap& heap) {
    if (module_env) {
        heap.MarkPending(module_env.get());
    }
    for (auto& dep : dependencies) {
        if (dep) heap.MarkPending(dep.get());
    }
    // exports 中的 Cell 持有 Value，Value 中的 Object 需要 trace
    for (auto& entry : exports) {
        if (entry.cell && entry.cell->value.is_object()) {
            RcObject* raw = entry.cell->value.as_object_raw();
            if (raw) heap.MarkPending(raw);
        }
    }
    if (eval_exception.has_value() && eval_exception->is_object()) {
        RcObject* raw = eval_exception->as_object_raw();
        if (raw) heap.MarkPending(raw);
    }
}

void ModuleRecord::ClearRefs() {
    module_env = RcPtr<Environment>();
    dependencies.clear();
    for (auto& entry : exports) {
        if (entry.cell) {
            entry.cell->value = Value::undefined();
        }
    }
    eval_exception = std::nullopt;
}

}  // namespace qppjs
