#include "qppjs/base/error.h"

#include <utility>

namespace qppjs {

Error::Error(ErrorKind kind, std::string message)
    : kind_(kind), message_(std::move(message)) {}

ErrorKind Error::kind() const {
    return kind_;
}

const std::string& Error::message() const {
    return message_;
}

std::string error_kind_name(ErrorKind kind) {
    switch (kind) {
    case ErrorKind::Cli:
        return "UsageError";
    case ErrorKind::Syntax:
        return "SyntaxError";
    case ErrorKind::Runtime:
        return "RuntimeError";
    case ErrorKind::Internal:
        return "InternalError";
    }

    return "InternalError";
}

} // namespace qppjs
