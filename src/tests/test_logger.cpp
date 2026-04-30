#include <gtest/gtest.h>
#include "services/logging/Logger.h"

TEST(LoggerTest, LogInfoDoesNotThrow) {
    // This test checks that logging an info message does not throw or crash.
    EXPECT_NO_THROW({
        Logger::info("LoggerTest", "This is an info message");
    });
}

TEST(LoggerTest, LogWarnDoesNotThrow) {
    EXPECT_NO_THROW({
        Logger::warn("LoggerTest", "This is a warning");
    });
}

TEST(LoggerTest, LogErrorDoesNotThrow) {
    EXPECT_NO_THROW({
        Logger::error("LoggerTest", "This is an error");
    });
}
