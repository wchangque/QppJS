#pragma once

#include "qppjs/runtime/value.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace qppjs {

class JSObject : public Object {
public:
    JSObject() = default;

    ObjectKind object_kind() const override;

    void set_proto(std::shared_ptr<JSObject> proto) { proto_ = std::move(proto); }
    const std::shared_ptr<JSObject>& proto() const { return proto_; }

    Value get_property(const std::string& key) const;
    void set_property(const std::string& key, Value value);
    void set_constructor_property(ObjectPtr value);
    bool has_own_property(const std::string& key) const;

private:
    std::shared_ptr<JSObject> proto_;
    struct PropertyEntry {
        std::string key;
        Value value;
    };

    std::vector<PropertyEntry> properties_;
    std::unordered_map<std::string, size_t> index_map_;
    std::weak_ptr<Object> constructor_property_;
    bool has_constructor_property_ = false;
};

}  // namespace qppjs
