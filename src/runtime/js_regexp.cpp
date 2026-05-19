#include "qppjs/runtime/js_regexp.h"

#include <regex>
#include <stdexcept>

namespace qppjs {

JSRegExp::JSRegExp(std::string pattern, std::string flags)
    : JSObject(ObjectKind::kRegExp), pattern_(std::move(pattern)) {
    // Parse flags
    for (char c : flags) {
        switch (c) {
        case 'g': global_ = true; break;
        case 'i': ignore_case_ = true; break;
        case 'm': multiline_ = true; break;
        case 's': dot_all_ = true; break;
        case 'u': unicode_ = true; break;
        case 'y': sticky_ = true; break;
        default: break;
        }
    }
    // Normalize flags to canonical order: g/i/m/s/u/y (per spec)
    flags_str_.clear();
    if (global_) flags_str_ += 'g';
    if (ignore_case_) flags_str_ += 'i';
    if (multiline_) flags_str_ += 'm';
    if (dot_all_) flags_str_ += 's';
    if (unicode_) flags_str_ += 'u';
    if (sticky_) flags_str_ += 'y';

    // Build std::regex flags
    std::regex_constants::syntax_option_type opts = std::regex_constants::ECMAScript;
    if (ignore_case_) opts |= std::regex_constants::icase;
    if (multiline_) opts |= std::regex_constants::multiline;

    // std::regex does not support dotall natively; we handle it via pattern rewriting
    // (replace . with [\s\S]) when dot_all_ is set.
    std::string compile_pattern = pattern_;
    if (dot_all_ && !compile_pattern.empty()) {
        // Simple rewrite: replace unescaped '.' with '[\s\S]'
        // We do a character-by-character scan to avoid replacing escaped dots or dots inside []
        std::string rewritten;
        rewritten.reserve(compile_pattern.size() * 2);
        bool in_class = false;
        for (size_t i = 0; i < compile_pattern.size(); ++i) {
            char ch = compile_pattern[i];
            if (ch == '\\' && i + 1 < compile_pattern.size()) {
                rewritten += ch;
                rewritten += compile_pattern[++i];
                continue;
            }
            if (ch == '[') {
                in_class = true;
                rewritten += ch;
            } else if (ch == ']') {
                in_class = false;
                rewritten += ch;
            } else if (ch == '.' && !in_class) {
                rewritten += "[\\s\\S]";
            } else {
                rewritten += ch;
            }
        }
        compile_pattern = std::move(rewritten);
    }

    try {
        compiled_ = std::regex(compile_pattern, opts);
        is_valid_ = true;
    } catch (const std::regex_error&) {
        is_valid_ = false;
    }
}

}  // namespace qppjs
