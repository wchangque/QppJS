#include "qppjs/frontend/token.h"

#include <gtest/gtest.h>

using qppjs::TokenKind;

TEST(TokenKindTest, IsKeywordReturnsTrueForKeywords) {
    EXPECT_TRUE(qppjs::is_keyword(TokenKind::KwLet));
    EXPECT_TRUE(qppjs::is_keyword(TokenKind::KwFinally));
    EXPECT_TRUE(qppjs::is_keyword(TokenKind::KwTrue));
    EXPECT_TRUE(qppjs::is_keyword(TokenKind::KwWhile));
}

TEST(TokenKindTest, IsKeywordReturnsFalseForNonKeywords) {
    EXPECT_FALSE(qppjs::is_keyword(TokenKind::Ident));
    EXPECT_FALSE(qppjs::is_keyword(TokenKind::Eof));
    EXPECT_FALSE(qppjs::is_keyword(TokenKind::Invalid));
    EXPECT_FALSE(qppjs::is_keyword(TokenKind::LParen));
    EXPECT_FALSE(qppjs::is_keyword(TokenKind::Number));
}

TEST(TokenKindTest, TokenKindNameReturnsNonEmpty) {
    EXPECT_FALSE(qppjs::token_kind_name(TokenKind::Eof).empty());
    EXPECT_FALSE(qppjs::token_kind_name(TokenKind::KwLet).empty());
    EXPECT_FALSE(qppjs::token_kind_name(TokenKind::LParen).empty());
    EXPECT_FALSE(qppjs::token_kind_name(TokenKind::Invalid).empty());
}

TEST(TokenKindTest, TokenKindNameForKwLetIsConsistent) {
    // token_kind_name 对关键字返回 "KwLet" 或 "let"，只要非空且一致即可
    std::string_view name = qppjs::token_kind_name(TokenKind::KwLet);
    EXPECT_FALSE(name.empty());
    // 再次调用应一致
    EXPECT_EQ(name, qppjs::token_kind_name(TokenKind::KwLet));
}
