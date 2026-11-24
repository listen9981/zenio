#include "zenio/detail/scheduler.h"

#include "zenio/detail/context.h"
#include "zenio/detail/runtime_core.h"
#include "zenio/task.h"

#include <memory>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

// design/scheduler.md 第 7 节的 M3 部分: 直接构造 RuntimeCore 并驱动它的调度器

namespace {

zenio::Task<void> LogStep(std::vector<int>* log, int marker) {
    log->push_back(marker);
    co_return;
}

zenio::Task<void> LogWithYield(std::vector<int>* log, int marker) {
    log->push_back(marker);
    co_await zenio::Yield();
    log->push_back(marker + 100);
}

std::shared_ptr<zenio::detail::RuntimeCore> MakeCore() {
    return std::make_shared<zenio::detail::RuntimeCore>(zenio::detail::RuntimeOptions {});
}

void ScheduleRoot(zenio::detail::RuntimeCore* core, const zenio::Task<void>& task) {
    core->scheduler().Schedule(
        zenio::detail::Runnable {zenio::detail::TaskAccess::Handle(task), &core->root_context()});
}

}    // namespace

TEST(Scheduler, EmptyQueueReturnsFalse) {
    auto core = MakeCore();
    EXPECT_FALSE(core->scheduler().RunOne());
}

TEST(Scheduler, FifoOrder) {
    auto              core = MakeCore();
    std::vector<int>  log;
    zenio::Task<void> a = LogStep(&log, 1);
    zenio::Task<void> b = LogStep(&log, 2);
    ScheduleRoot(core.get(), a);
    ScheduleRoot(core.get(), b);
    ASSERT_TRUE(core->scheduler().RunOne());
    ASSERT_TRUE(core->scheduler().RunOne());
    EXPECT_FALSE(core->scheduler().RunOne());
    EXPECT_EQ(log, (std::vector<int> {1, 2}));    // 依次投递 A、B, 运行顺序是 A、B
}

TEST(Scheduler, YieldLetsOtherRunFirst) {
    auto              core = MakeCore();
    std::vector<int>  log;
    zenio::Task<void> a = LogWithYield(&log, 1);
    zenio::Task<void> b = LogStep(&log, 2);
    ScheduleRoot(core.get(), a);
    ScheduleRoot(core.get(), b);
    ASSERT_TRUE(core->scheduler().RunOne());    // A 开始, 运行到 Yield 把自己投递到 B 之后
    ASSERT_TRUE(core->scheduler().RunOne());    // B
    ASSERT_TRUE(core->scheduler().RunOne());    // A 继续
    EXPECT_FALSE(core->scheduler().RunOne());
    EXPECT_EQ(log, (std::vector<int> {1, 2, 101}));
}

TEST(Scheduler, ContextRestoredAfterRuns) {
    auto              core = MakeCore();
    std::vector<int>  log;
    zenio::Task<void> a = LogWithYield(&log, 1);
    ScheduleRoot(core.get(), a);
    ASSERT_EQ(zenio::detail::CurrentContext(), nullptr);    // 运行之前没有上下文
    while (core->scheduler().RunOne()) {
    }
    EXPECT_EQ(log, (std::vector<int> {1, 101}));
    EXPECT_EQ(zenio::detail::CurrentContext(), nullptr);    // 运行完之后还原成空
}

TEST(Scheduler, SingleChainYieldsAndCompletes) {
    auto              core = MakeCore();
    std::vector<int>  log;
    zenio::Task<void> a = LogWithYield(&log, 1);
    ScheduleRoot(core.get(), a);
    ASSERT_TRUE(core->scheduler().RunOne());    // 让出之后自己被再次取出来
    ASSERT_TRUE(core->scheduler().RunOne());
    EXPECT_FALSE(core->scheduler().RunOne());
    EXPECT_EQ(log, (std::vector<int> {1, 101}));
}
