#include "zenio/detail/waiter.h"

#include "zenio/detail/context.h"
#include "zenio/detail/runtime_core.h"
#include "zenio/task.h"

#include <memory>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

// design/waiter.md 第 7 节的 M3 部分: 协议的单线程部分

namespace {

// 测试专用的 awaiter: 挂在一个 Waiter 上。signal_first 模拟 Signal 抢在 TrySuspend 之前到
struct TestAwaiter {
    zenio::detail::Waiter* waiter;
    bool                   signal_first = false;

    bool await_ready() noexcept { return false; }
    bool await_suspend(std::coroutine_handle<> self) noexcept {
        if (signal_first) {
            waiter->Signal();
        }
        return waiter->TrySuspend(self);
    }
    void await_resume() noexcept {}
};

zenio::Task<void> WaitOn(zenio::detail::Waiter* waiter, bool signal_first, std::vector<int>* log) {
    log->push_back(1);
    co_await TestAwaiter {waiter, signal_first};
    log->push_back(2);
}

std::shared_ptr<zenio::detail::RuntimeCore> MakeCore() {
    return std::make_shared<zenio::detail::RuntimeCore>(zenio::detail::RuntimeOptions {});
}

void ScheduleRoot(zenio::detail::RuntimeCore* core, const zenio::Task<void>& task) {
    core->scheduler().Schedule(
        zenio::detail::Runnable {zenio::detail::TaskAccess::Handle(task), &core->root_context()});
}

}    // namespace

TEST(Waiter, SuspendThenSignal) {
    auto                  core = MakeCore();
    zenio::detail::Waiter waiter;
    std::vector<int>      log;
    zenio::Task<void>     task = WaitOn(&waiter, false, &log);
    ScheduleRoot(core.get(), task);

    ASSERT_TRUE(core->scheduler().RunOne());
    EXPECT_EQ(log, (std::vector<int> {1}));      // 运行到 await_suspend 后挂起
    EXPECT_FALSE(core->scheduler().RunOne());    // 队列是空的

    waiter.Signal();
    ASSERT_TRUE(core->scheduler().RunOne());    // Signal 之后队列里有一项
    EXPECT_FALSE(core->scheduler().RunOne());
    EXPECT_EQ(log, (std::vector<int> {1, 2}));
}

TEST(Waiter, SignalBeforeSuspendResumesInPlace) {
    auto                  core = MakeCore();
    zenio::detail::Waiter waiter;
    std::vector<int>      log;
    zenio::Task<void>     task = WaitOn(&waiter, true, &log);
    ScheduleRoot(core.get(), task);

    // TrySuspend 返回 false, 协程原地恢复, 一次 RunOne 就走完
    ASSERT_TRUE(core->scheduler().RunOne());
    EXPECT_EQ(log, (std::vector<int> {1, 2}));
    EXPECT_FALSE(core->scheduler().RunOne());    // Signal 之后队列里什么都没有
    waiter.Signal();                             // 再 Signal 也不投递
    EXPECT_FALSE(core->scheduler().RunOne());
}

TEST(Waiter, DoubleSignalSchedulesOnce) {
    auto                  core = MakeCore();
    zenio::detail::Waiter waiter;
    std::vector<int>      log;
    zenio::Task<void>     task = WaitOn(&waiter, false, &log);
    ScheduleRoot(core.get(), task);
    ASSERT_TRUE(core->scheduler().RunOne());

    waiter.Signal();
    waiter.Signal();    // 重复唤醒只投递一次
    ASSERT_TRUE(core->scheduler().RunOne());
    EXPECT_FALSE(core->scheduler().RunOne());    // 队列里只有一项
    EXPECT_EQ(log, (std::vector<int> {1, 2}));
}

TEST(Waiter, SignalWithoutSuspendDoesNothing) {
    auto                  core = MakeCore();
    zenio::detail::Waiter waiter;
    waiter.Signal();    // 没有挂起过就 Signal: 什么都不发生
    EXPECT_FALSE(core->scheduler().RunOne());
}
