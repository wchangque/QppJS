#pragma once

#include "qppjs/runtime/js_object.h"
#include "qppjs/runtime/rc_object.h"
#include "qppjs/runtime/value.h"

#include <string>

namespace qppjs {

enum class NativeErrorType {
    kError,          // index 0
    kTypeError,      // index 1
    kReferenceError, // index 2
    kRangeError,     // index 3
    kCount
};

// Create an Error instance with the given prototype and message.
// Sets message property; name is inherited from proto.
Value MakeNativeErrorValue(RcPtr<JSObject> proto, const std::string& message);

}  // namespace qppjs
