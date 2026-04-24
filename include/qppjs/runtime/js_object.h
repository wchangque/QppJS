#pragma once

#include "qppjs/runtime/rc_object.h"
#include "qppjs/runtime/value.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace qppjs {

class JSObject : public RcObject {
public:
    JSObject() : RcObject(ObjectKind::kOrdinary) {}

    void set_proto(RcPtr<JSObject> proto) { proto_ = std::move(proto); }
    const RcPtr<JSObject>& proto() const { return proto_; }

    Value get_property(const std::string& key) const;
    void set_property(const std::string& key, Value value);
    // constructor_property_ is a raw (non-owning) pointer — weak reference semantics.
    void set_constructor_property(RcObject* value);
    bool has_own_property(const std::string& key) const;

private:
    RcPtr<JSObject> proto_;
    struct PropertyEntry {
        std::string key;
        Value value;
    };
    std::vector<PropertyEntry> properties_;
    std::unordered_map<std::string, size_t> index_map_;
    // Raw pointer — weak reference, does not own. Caller must ensure lifetime.
    RcObject* constructor_property_ = nullptr;
    bool has_constructor_property_ = false;
};

}  // namespace qppjs
