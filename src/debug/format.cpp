#include "qppjs/debug/format.h"

#include "qppjs/base/error.h"
#include "qppjs/runtime/value.h"

#include <sstream>
#include <string>

namespace qppjs {

std::string format_error(const Error& error) { return error_kind_name(error.kind()) + ": " + error.message(); }

std::string format_value(const Value& value) {
    switch (value.kind()) {
        case ValueKind::Undefined:
            return "undefined";
        case ValueKind::Null:
            return "null";
        case ValueKind::Bool:
            return value.as_bool() ? "true" : "false";
        case ValueKind::Number: {
            std::ostringstream stream;
            stream << value.as_number();
            return stream.str();
        }
        case ValueKind::String:
            return value.as_string();
        case ValueKind::Object:
            return "[object]";
    }

    return "[unknown]";
}

}  // namespace qppjs
