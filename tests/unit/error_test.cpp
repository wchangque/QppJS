#include "qppjs/base/error.h"

#include "qppjs/debug/format.h"

#include <gtest/gtest.h>

TEST(ErrorTest, StoresKindAndMessage) {
    const qppjs::Error error(qppjs::ErrorKind::Cli, "usage: qppjs <source>");

    EXPECT_EQ(error.kind(), qppjs::ErrorKind::Cli);
    EXPECT_EQ(error.message(), "usage: qppjs <source>");
}

TEST(ErrorTest, FormatsCliError) {
    const qppjs::Error error(qppjs::ErrorKind::Cli, "usage: qppjs <source>");

    EXPECT_EQ(qppjs::error_kind_name(error.kind()), "UsageError");
    EXPECT_EQ(qppjs::format_error(error), "UsageError: usage: qppjs <source>");
}
