#include "qppjs/runtime/native_errors.h"

#include "qppjs/runtime/js_object.h"
#include "qppjs/runtime/value.h"

namespace qppjs {

Value MakeNativeErrorValue(RcPtr<JSObject> proto, const std::string& message) {
    auto obj = RcPtr<JSObject>::make();
    obj->set_proto(proto);
    obj->set_property("message", Value::string(message));
    return Value::object(ObjectPtr(obj));
}

}  // namespace qppjs
