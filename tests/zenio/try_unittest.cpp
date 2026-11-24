#include "zenio/try.h"

#include "zenio/error.h"
#include "zenio/runtime.h"
#include "zenio/task.h"

#include <memory>
#include <stdexcept>
#include <utility>

#include <gtest/gtest.h>

// M3 任务书 5.7 的 Try 用例

namespace {

zenio::runtime::Runtime MakeRuntime() {
    return zenio::runtime::Builder::CurrentThread().EnableAll().Build();
}

zenio::Task<int> ComputeInt() {
    co_return 42;
}

zenio::Task<void> ComputeVoid() {
    co_return;
}

zenio::Task<int> ThrowRuntime() {
    throw std::runtime_error("try boom");
    co_return 0;    // 不可达: 让这个函数成为协程 (design/task.md 第 6 节)
}

zenio::Task<int> ThrowCancelled() {
    throw zenio::CancelledError(zenio::CancelReason::ABORT);
    co_return 0;    // 不可达: 让这个函数成为协程
}

zenio::Task<std::unique_ptr<int>> ComputeMoveOnly() {
    co_return std::make_unique<int>(7);
}

}    // namespace

TEST(Try, IntOk) {
    auto rt = MakeRuntime();
    auto r  = rt.BlockOn(zenio::Try(ComputeInt()));
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value(), 42);
}

TEST(Try, VoidOk) {
    auto rt = MakeRuntime();
    auto r  = rt.BlockOn(zenio::Try(ComputeVoid()));
    EXPECT_TRUE(r.is_ok());
    EXPECT_NO_THROW((void)r.value());
}

TEST(Try, RuntimeGoesIntoErr) {
    auto rt = MakeRuntime();
    auto r  = rt.BlockOn(zenio::Try(ThrowRuntime()));
    ASSERT_TRUE(r.is_err());
    try {
        std::rethrow_exception(r.error());
        FAIL() << "应当重抛";
    } catch (const std::runtime_error& e) {
        EXPECT_STREQ(e.what(), "try boom");    // 类型和 what() 都不变
    }
}

TEST(Try, CancelledErrorGoesIntoErr) {
    auto rt = MakeRuntime();
    auto r  = rt.BlockOn(zenio::Try(ThrowCancelled()));
    ASSERT_TRUE(r.is_err());
    try {
        std::rethrow_exception(r.error());
        FAIL() << "应当重抛";
    } catch (const zenio::CancelledError& e) {
        EXPECT_EQ(e.reason(), zenio::CancelReason::ABORT);    // 重抛接到的就是它
    }
}

TEST(Try, MoveOnlyOk) {
    auto rt = MakeRuntime();
    auto r  = rt.BlockOn(zenio::Try(ComputeMoveOnly()));
    ASSERT_TRUE(r.is_ok());
    int* pointer = r.value().get();
    ASSERT_NE(pointer, nullptr);
    EXPECT_EQ(*pointer, 7);    // value() 里的指针能用
}
