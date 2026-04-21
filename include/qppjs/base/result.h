#pragma once

#include "qppjs/base/error.h"

#include <utility>
#include <variant>

namespace qppjs {

template <typename T>
struct ParseResult {
    std::variant<T, Error> data;

    bool ok() const { return std::holds_alternative<T>(data); }
    T& value() { return std::get<T>(data); }
    const T& value() const { return std::get<T>(data); }
    Error& error() { return std::get<Error>(data); }
    const Error& error() const { return std::get<Error>(data); }

    static ParseResult<T> Ok(T v) { return {std::variant<T, Error>(std::move(v))}; }
    static ParseResult<T> Err(Error e) { return {std::variant<T, Error>(std::move(e))}; }
};

}  // namespace qppjs
