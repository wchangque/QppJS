#include "qppjs/runtime/js_function.h"

#include "qppjs/runtime/js_object.h"

namespace qppjs {

void JSFunction::set_property(const std::string& key, Value value) {
    own_properties_[key] = std::move(value);
}

Value JSFunction::get_property(const std::string& key) const {
    auto it = own_properties_.find(key);
    if (it != own_properties_.end()) {
        return it->second;
    }
    return Value::undefined();
}

void JSFunction::clear_own_properties() {
    own_properties_.clear();
}

}  // namespace qppjs
