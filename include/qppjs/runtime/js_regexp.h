#pragma once

#include "qppjs/runtime/js_object.h"
#include "qppjs/runtime/rc_object.h"

#include <regex>
#include <string>

namespace qppjs {

// JSRegExp: heap-allocated JS RegExp object (ObjectKind::kRegExp).
// Inherits JSObject to reuse property storage (for own properties and prototype chain).
class JSRegExp : public JSObject {
public:
    JSRegExp(std::string pattern, std::string flags);

    // [[OriginalSource]] — empty pattern is stored as "" but reported as "(?:)"
    std::string pattern_;
    // [[OriginalFlags]] — normalized order: g/i/m/s/u/y
    std::string flags_str_;

    bool global_ = false;
    bool ignore_case_ = false;
    bool multiline_ = false;
    bool dot_all_ = false;
    bool sticky_ = false;
    bool unicode_ = false;

    // C++ compiled regex (ECMAScript syntax)
    std::regex compiled_;
    bool is_valid_ = false;

    // lastIndex — stored as C++ field to avoid property hash lookup on every exec
    uint32_t last_index_ = 0;
};

}  // namespace qppjs
