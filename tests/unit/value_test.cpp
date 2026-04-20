#include "qppjs/debug/format.h"
#include "qppjs/runtime/value.h"

#include <gtest/gtest.h>
#include <memory>

TEST(ValueTest, PreservesKindsAndPayloads) {
    const qppjs::Value undefined_value = qppjs::Value::undefined();
    const qppjs::Value null_value = qppjs::Value::null();
    const qppjs::Value bool_value = qppjs::Value::boolean(true);
    const qppjs::Value number_value = qppjs::Value::number(3.5);
    const qppjs::Value string_value = qppjs::Value::string("hello");
    const qppjs::Value object_value = qppjs::Value::object(std::make_shared<qppjs::Object>());

    EXPECT_EQ(undefined_value.kind(), qppjs::ValueKind::Undefined);
    EXPECT_EQ(null_value.kind(), qppjs::ValueKind::Null);
    EXPECT_TRUE(bool_value.is_bool());
    EXPECT_TRUE(bool_value.as_bool());
    EXPECT_TRUE(number_value.is_number());
    EXPECT_DOUBLE_EQ(number_value.as_number(), 3.5);
    EXPECT_TRUE(string_value.is_string());
    EXPECT_EQ(string_value.as_string(), "hello");
    EXPECT_TRUE(object_value.is_object());
    EXPECT_TRUE(static_cast<bool>(object_value.as_object()));
}

TEST(ValueTest, FormatsValuesForDebugOutput) {
    EXPECT_EQ(qppjs::format_value(qppjs::Value::undefined()), "undefined");
    EXPECT_EQ(qppjs::format_value(qppjs::Value::null()), "null");
    EXPECT_EQ(qppjs::format_value(qppjs::Value::boolean(true)), "true");
    EXPECT_EQ(qppjs::format_value(qppjs::Value::number(3.5)), "3.5");
    EXPECT_EQ(qppjs::format_value(qppjs::Value::string("hello")), "hello");
    EXPECT_EQ(qppjs::format_value(qppjs::Value::object(std::make_shared<qppjs::Object>())), "[object]");
}
