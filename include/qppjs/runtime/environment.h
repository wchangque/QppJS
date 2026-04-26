#pragma once

#include "qppjs/frontend/ast.h"
#include "qppjs/runtime/completion.h"
#include "qppjs/runtime/gc_heap.h"
#include "qppjs/runtime/rc_object.h"
#include "qppjs/runtime/value.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// Note: <memory> is still needed for std::unique_ptr used in FlatBindingMap.

namespace qppjs {

struct Cell {
    Value value;
    int32_t ref_count = 0;
    bool initialized = false;  // TDZ 标志，导出/导入双方共享

    void add_ref() { ++ref_count; }

    void release() {
        if (--ref_count == 0) {
            delete this;
        }
    }
};

using CellPtr = RcPtr<Cell>;

struct Binding {
    CellPtr cell;
    bool mutable_;      // false for const
    bool initialized;   // false means TDZ
    bool function_like;  // function declaration hoist slot
};

// Small flat map for environment bindings. Uses linear scan for <= kUpgradeThreshold entries,
// then upgrades to unordered_map.
class FlatBindingMap {
public:
    static constexpr int kUpgradeThreshold = 16;

    Binding* find(const std::string& name) {
        if (large_) {
            auto it = large_->find(name);
            return it != large_->end() ? &it->second : nullptr;
        }
        for (auto& [k, v] : entries_) {
            if (k == name) return &v;
        }
        return nullptr;
    }

    const Binding* find(const std::string& name) const {
        if (large_) {
            auto it = large_->find(name);
            return it != large_->end() ? &it->second : nullptr;
        }
        for (const auto& [k, v] : entries_) {
            if (k == name) return &v;
        }
        return nullptr;
    }

    void insert_or_assign(const std::string& name, Binding binding) {
        if (large_) {
            (*large_)[name] = std::move(binding);
            return;
        }
        for (auto& [k, v] : entries_) {
            if (k == name) {
                v = std::move(binding);
                return;
            }
        }
        entries_.emplace_back(name, std::move(binding));
        maybe_upgrade();
    }

    void emplace(const std::string& name, Binding binding) {
        if (large_) {
            large_->emplace(name, std::move(binding));
            return;
        }
        for (const auto& [k, v] : entries_) {
            if (k == name) return;  // already exists, no-op like unordered_map::emplace
        }
        entries_.emplace_back(name, std::move(binding));
        maybe_upgrade();
    }

    int count(const std::string& name) const {
        return find(name) != nullptr ? 1 : 0;
    }

    std::vector<std::pair<std::string, Binding>> entries() const {
        if (large_) {
            std::vector<std::pair<std::string, Binding>> result;
            result.reserve(large_->size());
            for (const auto& [k, v] : *large_) {
                result.emplace_back(k, v);
            }
            return result;
        }
        return entries_;
    }

    std::vector<Binding*> binding_ptrs() {
        std::vector<Binding*> result;
        if (large_) {
            result.reserve(large_->size());
            for (auto& [k, v] : *large_) {
                result.push_back(&v);
            }
            return result;
        }
        result.reserve(entries_.size());
        for (auto& [k, v] : entries_) {
            result.push_back(&v);
        }
        return result;
    }

private:
    void maybe_upgrade() {
        if (static_cast<int>(entries_.size()) > kUpgradeThreshold) {
            large_ = std::make_unique<std::unordered_map<std::string, Binding>>();
            for (auto& [k, v] : entries_) {
                (*large_)[k] = std::move(v);
            }
            entries_.clear();
        }
    }

    std::vector<std::pair<std::string, Binding>> entries_;
    std::unique_ptr<std::unordered_map<std::string, Binding>> large_;
};

class Environment : public RcObject {
public:
    explicit Environment(RcPtr<Environment> outer);

    void TraceRefs(GcHeap& heap) override;
    void ClearRefs() override;

    // Declare binding by VarKind:
    //   Var   -> initialized=true,  value=undefined, mutable=true
    //   Let   -> initialized=false, mutable=true  (TDZ)
    //   Const -> initialized=false, mutable=false (TDZ)
    void define(const std::string& name, VarKind kind);

    // Declare an already-initialized binding (for var hoisting); idempotent.
    void define_initialized(const std::string& name);
    void define_function(const std::string& name);
    // Walk the outer chain; returns nullptr if not found.
    Binding* lookup(const std::string& name);

    // Read variable value; checks TDZ and undefined-reference.
    EvalResult get(const std::string& name);

    // Write variable value; checks const and undefined-reference.
    EvalResult set(const std::string& name, Value value);

    // Initialize a TDZ binding (called when let/const declaration executes).
    EvalResult initialize(const std::string& name, Value value);

    // 将已有 Cell 注入 Binding（live binding 核心，用于模块导出变量）
    // initialized=false 表示 TDZ（let/const 导出），initialized=true 表示无 TDZ（var/function 导出）
    void define_binding_with_cell(const std::string& name, RcPtr<Cell> cell, bool is_mutable,
                                  bool initialized = false);

    // 创建不可变 import binding（共享 Cell，赋值抛 TypeError）
    void define_import_binding(const std::string& name, RcPtr<Cell> cell);

    const RcPtr<Environment>& outer() const { return outer_; }
    const FlatBindingMap& bindings() const { return bindings_; }
    // 只在本层查找（不沿 outer 链），返回可修改指针
    Binding* find_local(const std::string& name) { return bindings_.find(name); }
    void clear_function_bindings();

private:
    void clear_function_bindings(std::unordered_set<const Environment*>& visited);

private:
    FlatBindingMap bindings_;
    RcPtr<Environment> outer_;
};

}  // namespace qppjs
