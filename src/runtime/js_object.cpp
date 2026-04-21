#include "qppjs/runtime/js_object.h"

namespace qppjs {

ObjectKind JSObject::object_kind() const { return ObjectKind::kOrdinary; }

Value JSObject::get_property(const std::string& key) const {
    // Walk prototype chain
    const JSObject* cur = this;
    while (cur != nullptr) {
        auto it = cur->index_map_.find(key);
        if (it != cur->index_map_.end()) {
            return cur->properties_[it->second].value;
        }
        cur = cur->proto_.get();
    }
    return Value::undefined();
}

void JSObject::set_property(const std::string& key, Value value) {
    auto it = index_map_.find(key);
    if (it != index_map_.end()) {
        properties_[it->second].value = std::move(value);
    } else {
        size_t idx = properties_.size();
        properties_.push_back(PropertyEntry{key, std::move(value)});
        index_map_.emplace(key, idx);
    }
}

bool JSObject::has_own_property(const std::string& key) const {
    return index_map_.find(key) != index_map_.end();
}

}  // namespace qppjs
