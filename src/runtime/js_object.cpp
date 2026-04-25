#include "qppjs/runtime/js_object.h"

#include "qppjs/runtime/js_function.h"

#include <unordered_set>

namespace qppjs {

Value JSObject::get_property(const std::string& key) const {
    const JSObject* cur = this;
    while (cur != nullptr) {
        auto it = cur->index_map_.find(key);
        if (it != cur->index_map_.end()) {
            return cur->properties_[it->second].value;
        }
        if (key == "constructor" && cur->has_constructor_property_) {
            if (cur->constructor_property_ != nullptr) {
                return Value::object(ObjectPtr(cur->constructor_property_));
            }
            return Value::undefined();
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

void JSObject::set_constructor_property(RcObject* value) {
    constructor_property_ = value;
    has_constructor_property_ = true;
}

bool JSObject::has_own_property(const std::string& key) const {
    return key == "constructor" ? has_constructor_property_ || index_map_.contains(key)
                                : index_map_.contains(key);
}

void JSObject::clear_function_properties() {
    std::unordered_set<const JSObject*> visited;
    clear_function_properties(visited);
}

void JSObject::clear_function_properties(std::unordered_set<const JSObject*>& visited) {
    if (visited.contains(this)) {
        return;
    }
    visited.insert(this);

    if (proto_) {
        proto_->clear_function_properties(visited);
    }

    if (constructor_property_ != nullptr) {
        constructor_property_ = nullptr;
    }

    for (auto& property : properties_) {
        if (!property.value.is_object()) {
            continue;
        }
        RcObject* raw = property.value.as_object_raw();
        if (raw == nullptr) {
            continue;
        }
        ObjectPtr keep_alive(raw);
        if (raw->object_kind() == ObjectKind::kFunction) {
            auto* function = static_cast<JSFunction*>(raw);
            RcPtr<JSObject> prototype = function->prototype_obj();
            if (prototype) {
                prototype->clear_function_properties(visited);
            }
            property.value = Value::undefined();
            continue;
        }
        if (raw->object_kind() == ObjectKind::kOrdinary) {
            static_cast<JSObject*>(raw)->clear_function_properties(visited);
        }
    }
}

}  // namespace qppjs
