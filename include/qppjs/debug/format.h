#pragma once

#include <string>

namespace qppjs {

class Error;
class Value;

[[nodiscard]] std::string format_error(const Error& error);
[[nodiscard]] std::string format_value(const Value& value);

} // namespace qppjs
