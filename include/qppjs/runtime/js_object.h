#pragma once

#include "qppjs/runtime/value.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace qppjs {

class JSObject : public Object {
public:
    JSObject() = default;

    ObjectKind object_kind() const override;

    Value get_property(const std::string& key) const;
    void set_property(const std::string& key, Value value);
    bool has_own_property(const std::string& key) const;

private:
    struct PropertyEntry {
        std::string key;
        Value value;
    };

    std::vector<PropertyEntry> properties_;
    std::unordered_map<std::string, size_t> index_map_;
};

}  // namespace qppjs
