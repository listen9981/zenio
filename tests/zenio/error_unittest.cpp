#include "zenio/error.h"

#include <cerrno>
#include <exception>
#include <string>

#include <gtest/gtest.h>

// error.md 第 7 节列出的用例

TEST(Error, SystemErrorFields) {
    const zenio::SystemError e(ENOENT, "read");
    EXPECT_EQ(e.err(), ENOENT);
    EXPECT_STREQ(e.op(), "read");
    const std::string what = e.what();
    EXPECT_NE(what.find("read"), std::string::npos);
    EXPECT_NE(what.find(std::to_string(ENOENT)), std::string::npos);
}

TEST(Error, SystemErrorCaughtAsError) {
    EXPECT_THROW({ throw zenio::SystemError(EIO, "write"); }, zenio::Error);
}

TEST(Error, SystemErrorCaughtAsException) {
    EXPECT_THROW({ throw zenio::SystemError(EIO, "write"); }, std::exception);
}

TEST(Error, CancelledErrorReason) {
    const zenio::CancelledError e(zenio::CancelReason::ABORT);
    EXPECT_EQ(e.reason(), zenio::CancelReason::ABORT);
}

TEST(Error, CancelledErrorCaughtAsError) {
    EXPECT_THROW({ throw zenio::CancelledError(zenio::CancelReason::SHUTDOWN); }, zenio::Error);
}

TEST(Error, ElapsedErrorCaughtAsElapsed) {
    EXPECT_THROW({ throw zenio::ElapsedError(); }, zenio::ElapsedError);
}

TEST(Error, ElapsedErrorCaughtAsCancelled) {
    EXPECT_THROW({ throw zenio::ElapsedError(); }, zenio::CancelledError);
}

TEST(Error, ElapsedErrorCaughtAsError) {
    EXPECT_THROW({ throw zenio::ElapsedError(); }, zenio::Error);
}

TEST(Error, SystemErrorCopy) {
    const zenio::SystemError a(ENOENT, "open");
    const zenio::SystemError b(a);
    EXPECT_STREQ(a.what(), b.what());
    EXPECT_NE(std::string(b.what()).find("open"), std::string::npos);
    EXPECT_EQ(b.err(), ENOENT);
    EXPECT_STREQ(b.op(), "open");
}
