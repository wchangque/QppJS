#pragma once

#include "qppjs/frontend/token.h"

#include <string_view>

namespace qppjs {

struct LexerState {
    std::string_view source;  // 不拥有，调用方保证生命周期
    uint32_t pos;             // 当前扫描字节偏移
    uint32_t line;            // 当前行号（1-based）
    bool got_lf;              // 当前 token 前是否有换行（ASI 用）
    bool scan_regex{false};   // 下一个 / 是否应扫描为正则字面量
};

LexerState lexer_init(std::string_view source);
Token next_token(LexerState& state);
// 扫描模板字符串的一段（从当前 pos 开始，已消耗起始 ` 或上一段的 }）
// 返回 TemplateNoSub / TemplateHead / TemplateMiddle / TemplateTail / Invalid
// token.range 覆盖从起始定界符（含）到当前段结束定界符（含）的原始文本
Token scan_template_part(LexerState& state);

}  // namespace qppjs
