#pragma once

#include "qppjs/frontend/token.h"

#include <string_view>

namespace qppjs {

struct LexerState {
    std::string_view source;  // 不拥有，调用方保证生命周期
    uint32_t pos;             // 当前扫描字节偏移
    uint32_t line;            // 当前行号（1-based）
    bool got_lf;              // 当前 token 前是否有换行（ASI 用）
};

LexerState lexer_init(std::string_view source);
Token next_token(LexerState& state);

} // namespace qppjs
