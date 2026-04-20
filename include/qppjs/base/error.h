#pragma once

#include <string>

namespace qppjs {

enum class ErrorKind {
    Cli,
    Syntax,
    Runtime,
    Internal,
};

class Error {
public:
    Error(ErrorKind kind, std::string message);

    [[nodiscard]] ErrorKind kind() const;
    [[nodiscard]] const std::string& message() const;

private:
    ErrorKind kind_;
    std::string message_;
};

[[nodiscard]] std::string error_kind_name(ErrorKind kind);

} // namespace qppjs
