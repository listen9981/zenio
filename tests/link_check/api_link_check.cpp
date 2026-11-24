#include "zenio/zenio.h"

#include <stdexcept>
#include <string>
#include <utility>

#include <gtest/gtest.h>

// 链接检查: 只包含 zenio.h, 不包含任何 zenio/detail/ 下的头文件 (design/milestones/M3.md 5.8)。
// 只链接共享库 zenio, 库外需要的符号缺一个都会在这里链接失败 (S31)。

namespace {

zenio::Task<int> AddOne(int value) {
    co_return value + 1;
}

zenio::Task<int> NestedWithYield() {
    int value = 1;
    for (int i = 0; i < 5; ++i) {
        value = co_await AddOne(value);
        co_await zenio::Yield();
    }
    co_return value;
}

zenio::Task<int> Boom() {
    throw std::runtime_error("link check boom");
    co_return 0;    // 不可达: 让这个函数成为协程 (design/task.md 第 6 节)
}

zenio::Task<std::string> CheckHandle(zenio::runtime::Handle expected) {
    const zenio::runtime::Handle current     = zenio::runtime::Handle::Current();
    const zenio::runtime::Handle try_current = zenio::runtime::Handle::TryCurrent();
    if (current == expected && try_current == expected) {
        co_return "same";
    }
    co_return "different";
}

}    // namespace

TEST(LinkCheck, BlockOnNestedWithYield) {
    auto rt = zenio::runtime::Builder::CurrentThread().EnableAll().Build();
    EXPECT_EQ(rt.BlockOn(NestedWithYield()), 6);
}

TEST(LinkCheck, BlockOnTryWrapsFailure) {
    auto rt = zenio::runtime::Builder::CurrentThread().EnableAll().Build();
    auto r  = rt.BlockOn(zenio::Try(Boom()));
    ASSERT_TRUE(r.is_err());
    try {
        std::rethrow_exception(r.error());
        FAIL() << "应当重抛";
    } catch (const std::runtime_error& e) {
        EXPECT_STREQ(e.what(), "link check boom");
    }
}

TEST(LinkCheck, HandleCurrentInsideCoroutine) {
    auto rt = zenio::runtime::Builder::CurrentThread().EnableAll().Build();
    EXPECT_EQ(rt.BlockOn(CheckHandle(rt.handle())), "same");
}

TEST(LinkCheck, HandleEnterScope) {
    auto rt = zenio::runtime::Builder::CurrentThread().EnableAll().Build();
    EXPECT_FALSE(zenio::runtime::Handle::TryCurrent().is_valid());
    {
        auto guard = rt.handle().Enter();
        EXPECT_TRUE(zenio::runtime::Handle::Current() == rt.handle());
    }
    EXPECT_FALSE(zenio::runtime::Handle::TryCurrent().is_valid());
}
