#include "zenio/runtime.h"

#include "zenio/detail/context.h"
#include "zenio/detail/runtime_core.h"

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

// design/runtime.md 第 7 节列出的用例 (M3 交付)

namespace {

zenio::runtime::Runtime MakeRuntime() {
    return zenio::runtime::Builder::CurrentThread().EnableAll().Build();
}

zenio::Task<int> ComputeInt() {
    co_return 42;
}

zenio::Task<std::unique_ptr<int>> ComputeMoveOnly() {
    co_return std::make_unique<int>(3);
}

zenio::Task<int&> MakeRef(int* referent) {
    co_return *referent;
}

zenio::Task<void> ComputeVoid() {
    co_return;
}

zenio::Task<int> Boom() {
    throw std::runtime_error("runtime boom");
    co_return 0;    // 不可达: 让这个函数成为协程 (design/task.md 第 6 节)
}

zenio::Task<bool> CheckHandle(zenio::runtime::Handle expected) {
    const zenio::runtime::Handle    current     = zenio::runtime::Handle::Current();
    const zenio::runtime::Handle    try_current = zenio::runtime::Handle::TryCurrent();
    co_return current == expected&& try_current == expected;
}

// 在协程里再调 BlockOn: 内层被拒绝, 外层照常运行完
zenio::Task<std::string> BlockOnInsideCoroutine(zenio::runtime::Runtime* rt) {
    try {
        (void)rt->BlockOn(ComputeInt());
        co_return "no-throw";
    } catch (const std::runtime_error&) {
        co_return "threw";
    }
}

}    // namespace

TEST(Runtime, BlockOnReturnsValues) {
    auto rt = MakeRuntime();
    EXPECT_EQ(rt.BlockOn(ComputeInt()), 42);
    EXPECT_NO_THROW((void)rt.BlockOn(ComputeVoid()));
    auto pointer = rt.BlockOn(ComputeMoveOnly());
    ASSERT_NE(pointer, nullptr);
    EXPECT_EQ(*pointer, 3);
    int  referent = 5;
    int& result   = rt.BlockOn(MakeRef(&referent));
    EXPECT_EQ(&result, &referent);
}

TEST(Runtime, BlockOnRethrowsException) {
    auto rt = MakeRuntime();
    try {
        (void)rt.BlockOn(Boom());
        FAIL() << "应当重抛";
    } catch (const std::runtime_error& e) {
        EXPECT_STREQ(e.what(), "runtime boom");    // 类型和 what() 不变
    }
}

TEST(Runtime, NoContextAfterBlockOn) {
    auto rt = MakeRuntime();
    (void)rt.BlockOn(ComputeVoid());
    EXPECT_FALSE(zenio::runtime::Handle::TryCurrent().is_valid());
    EXPECT_EQ(zenio::detail::CurrentContext(), nullptr);
}

TEST(Runtime, TwoBlockOnInARow) {
    auto rt = MakeRuntime();
    EXPECT_EQ(rt.BlockOn(ComputeInt()), 42);
    EXPECT_EQ(rt.BlockOn(ComputeInt()), 42);
}

TEST(Runtime, BlockOnInsideCoroutineThrowsAndOuterSurvives) {
    auto rt = MakeRuntime();
    EXPECT_EQ(rt.BlockOn(BlockOnInsideCoroutine(&rt)), "threw");
    EXPECT_EQ(rt.BlockOn(ComputeInt()), 42);    // 内层被拒绝之后外层照常运行完
}

TEST(Runtime, BlockOnInsideEnterScopeThrows) {
    auto rt    = MakeRuntime();
    auto guard = rt.handle().Enter();
    EXPECT_THROW((void)rt.BlockOn(ComputeInt()), std::runtime_error);
}

TEST(Runtime, CurrentThrowsOutsideContext) {
    EXPECT_THROW((void)zenio::runtime::Handle::Current(), std::runtime_error);
    EXPECT_FALSE(zenio::runtime::Handle::TryCurrent().is_valid());
}

TEST(Runtime, HandleInsideCoroutineEqualsRuntime) {
    auto rt = MakeRuntime();
    EXPECT_TRUE(rt.BlockOn(CheckHandle(rt.handle())));
}

TEST(Runtime, EnterScopeAndNesting) {
    auto rt_a = MakeRuntime();
    auto rt_b = MakeRuntime();
    EXPECT_FALSE(zenio::runtime::Handle::TryCurrent().is_valid());
    {
        auto g_a = rt_a.handle().Enter();
        EXPECT_TRUE(zenio::runtime::Handle::Current() == rt_a.handle());
        {
            auto g_b = rt_b.handle().Enter();
            EXPECT_TRUE(zenio::runtime::Handle::Current() == rt_b.handle());
        }
        EXPECT_TRUE(zenio::runtime::Handle::Current() == rt_a.handle());    // 嵌套的 Enter 各自还原
    }
    EXPECT_FALSE(zenio::runtime::Handle::TryCurrent().is_valid());
}

TEST(Runtime, EnterGuardMoveOnlyTargetRestores) {
    auto rt_outer = MakeRuntime();
    auto rt_inner = MakeRuntime();
    {
        auto                                      g_outer = rt_outer.handle().Enter();    // context = rt_outer
        std::optional<zenio::runtime::EnterGuard> moved;
        {
            auto g_inner = rt_inner.handle().Enter();    // context = rt_inner
            moved.emplace(std::move(g_inner));           // 移动之后源对象不再生效
        }
        // g_inner 析构不还原: 当前上下文还是 rt_inner, 不是 rt_outer —— 职责已转移给 moved
        EXPECT_TRUE(zenio::runtime::Handle::Current() == rt_inner.handle());
        moved.reset();    // 目标对象析构时还原
        EXPECT_TRUE(zenio::runtime::Handle::Current() == rt_outer.handle());
    }    // g_outer 析构时还原到空
    EXPECT_FALSE(zenio::runtime::Handle::TryCurrent().is_valid());
}

TEST(Runtime, EnterRuntimeCreatedOnOtherThread) {
    zenio::runtime::Handle other;
    std::thread            builder([&other] {
        auto rt = zenio::runtime::Builder::CurrentThread().Build();
        other   = rt.handle();
    });
    builder.join();
    ASSERT_TRUE(other.is_valid());
    {
        auto guard = other.Enter();
        EXPECT_TRUE(zenio::runtime::Handle::Current() == other);
    }
    EXPECT_FALSE(zenio::runtime::Handle::TryCurrent().is_valid());    // 离开作用域之后回到原状
}

TEST(Runtime, HandleOutlivesRuntime) {
    zenio::runtime::Handle handle;
    {
        auto rt = MakeRuntime();
        handle  = rt.handle();
        EXPECT_EQ(rt.BlockOn(ComputeInt()), 42);
    }
    EXPECT_TRUE(handle.is_valid());    // Runtime 对象销毁之后句柄仍然有效
}

TEST(Runtime, HandleEquality) {
    auto rt_a   = MakeRuntime();
    auto rt_b   = MakeRuntime();
    auto same_a = rt_a.handle();
    EXPECT_TRUE(same_a == rt_a.handle());     // 同一个运行时的两个句柄相等
    EXPECT_FALSE(same_a == rt_b.handle());    // 不同运行时的不等
    zenio::runtime::Handle empty;
    EXPECT_FALSE(empty == rt_a.handle());    // 空句柄与有效句柄不等
    EXPECT_TRUE(empty == zenio::runtime::Handle());
    EXPECT_FALSE(empty.is_valid());
    EXPECT_TRUE(rt_a.handle().is_valid());
}

TEST(Runtime, MovedRuntimeWorks) {
    auto rt_moved_from = MakeRuntime();
    auto rt            = std::move(rt_moved_from);
    EXPECT_EQ(rt.BlockOn(ComputeInt()), 42);
    rt = MakeRuntime();    // 移动赋值
    EXPECT_EQ(rt.BlockOn(ComputeInt()), 42);
}

TEST(Runtime, MultiThreadZeroThrows) {
    EXPECT_THROW((void)zenio::runtime::Builder::MultiThread(0), std::invalid_argument);
}

// M3 期间 MultiThread 的 Build 抛 runtime_error; M6 实现后删掉这一条
TEST(Runtime, MultiThreadBuildNotImplemented) {
    EXPECT_THROW((void)zenio::runtime::Builder::MultiThread(2).Build(), std::runtime_error);
}

TEST(Runtime, AllBuilderFormsBuildRunnableRuntime) {
    EXPECT_EQ(zenio::runtime::Builder::CurrentThread().EnableIo().EnableTime().Build().BlockOn(ComputeInt()), 42);
    EXPECT_EQ(zenio::runtime::Builder::CurrentThread().EnableAll().Build().BlockOn(ComputeInt()), 42);
    zenio::runtime::Builder builder;
    EXPECT_EQ(builder.Build().BlockOn(ComputeInt()), 42);    // 默认构造等同 CurrentThread()
}

TEST(Runtime, IndependentRuntimesOnThreads) {
    constexpr int            kThreads = 4;
    std::vector<int>         results(kThreads, 0);
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([i, &results] {
            auto rt                              = zenio::runtime::Builder::CurrentThread().Build();
            results[static_cast<std::size_t>(i)] = rt.BlockOn(ComputeInt());
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    for (int result : results) {
        EXPECT_EQ(result, 42);    // 互不干扰
    }
}
