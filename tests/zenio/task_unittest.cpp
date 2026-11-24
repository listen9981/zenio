#include "zenio/task.h"

#include "zenio/detail/context.h"
#include "zenio/detail/runtime_core.h"
#include "zenio/detail/scheduler.h"
#include "zenio/runtime.h"

#include <memory>
#include <pthread.h>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

// design/task.md 第 7 节列出的用例 (M3 交付)

namespace {

// —— 被测协程: 一律普通命名函数, 状态用指针传进去 ——

zenio::Task<int> ComputeInt() {
    co_return 42;
}

zenio::Task<bool> ComputeBool() {
    co_return true;
}

zenio::Task<void> ComputeVoid() {
    co_return;
}

zenio::Task<std::string> ComputeString() {
    co_return std::string("hello");
}

zenio::Task<std::unique_ptr<int>> ComputeMoveOnly() {
    co_return std::make_unique<int>(7);
}

// 只可移动的类型按值传进协程, 在函数体里能用
zenio::Task<int> ConsumeMoveOnly(std::unique_ptr<int> value) {
    co_return *value + 4;
}

zenio::Task<int&> MakeRef(int* referent) {
    co_return *referent;
}

zenio::Task<int> Boom() {
    throw std::runtime_error("task boom");
    co_return 0;    // 不可达: 让这个函数成为协程 (design/task.md 第 6 节)
}

zenio::Task<void> BoomVoid() {
    throw std::runtime_error("void boom");
    co_return;    // 不可达: 让这个函数成为协程
}

// 嵌套链: 每层一个 RAII 计数对象, 统计同时活着的层数

struct NestStats {
    int alive     = 0;
    int max_alive = 0;
};

struct NestGuard {
    explicit NestGuard(NestStats* stats) : stats_(stats) {
        ++stats_->alive;
        if (stats_->alive > stats_->max_alive) {
            stats_->max_alive = stats_->alive;
        }
    }
    NestGuard(const NestGuard&)            = delete;
    NestGuard& operator=(const NestGuard&) = delete;
    ~NestGuard() { --stats_->alive; }
    NestStats* stats_;
};

zenio::Task<int> NestChain(int depth, NestStats* stats) {
    NestGuard guard {stats};
    if (depth == 0) {
        co_return 42;
    }
    int child = co_await NestChain(depth - 1, stats);
    co_return child + 1;
}

zenio::Task<int> NestChainThrow(int depth, NestStats* stats) {
    NestGuard guard {stats};
    if (depth == 0) {
        throw std::runtime_error("deep");
    }
    int child = co_await NestChainThrow(depth - 1, stats);
    co_return child + 1;
}

// 中间某一层接住下层异常之后正常返回
zenio::Task<int> ThrowAt(int depth) {
    if (depth == 0) {
        throw std::runtime_error("mid boom");
    }
    int child = co_await ThrowAt(depth - 1);
    co_return child + 1;
}

zenio::Task<int> CatchAt(int depth) {
    try {
        int child = co_await ThrowAt(depth);
        co_return child + 100;
    } catch (const std::runtime_error&) {
        co_return -1;
    }
}

// 反向检查用的协程

zenio::Task<void> YieldOnce(int* steps) {
    ++*steps;
    co_await zenio::Yield();
    ++*steps;
}

// —— 512 KB 栈的线程骨架: 线程函数里不写 GTest 断言, 结果记进结构体 ——

constexpr int kNestDepth      = 100000;
constexpr int kSmallStackSize = 512 * 1024;

struct NestOutcome {
    bool        ran          = false;
    int         value        = 0;
    int         max_alive    = 0;
    int         alive_after  = -1;
    bool        exception_ok = false;
    std::string exception_what;
    bool        threw = false;
};

struct NestRunArgs {
    int          depth;
    bool         throw_at_bottom;
    NestOutcome* outcome;
};

void* NestThreadMain(void* raw) {
    auto*       args = static_cast<NestRunArgs*>(raw);
    NestStats   stats;
    NestOutcome outcome;
    try {
        auto rt = zenio::runtime::Builder::CurrentThread().Build();
        if (args->throw_at_bottom) {
            (void)rt.BlockOn(NestChainThrow(args->depth, &stats));
            outcome.threw = false;
        } else {
            outcome.value = rt.BlockOn(NestChain(args->depth, &stats));
            outcome.threw = false;
        }
    } catch (const std::runtime_error& e) {
        outcome.threw          = true;
        outcome.exception_ok   = e.what() == std::string("deep");
        outcome.exception_what = e.what();
    }
    outcome.ran         = true;
    outcome.max_alive   = stats.max_alive;
    outcome.alive_after = stats.alive;
    *args->outcome      = outcome;
    return nullptr;
}

void RunOnSmallStack(void* (*main_fn)(void*), void* arg) {
    pthread_attr_t attr;
    ASSERT_EQ(pthread_attr_init(&attr), 0);
    ASSERT_EQ(pthread_attr_setstacksize(&attr, kSmallStackSize), 0);
    pthread_t thread;
    const int err = pthread_create(&thread, &attr, main_fn, arg);
    pthread_attr_destroy(&attr);
    ASSERT_EQ(err, 0) << "pthread_create failed: " << err;
    ASSERT_EQ(pthread_join(thread, nullptr), 0);
}

zenio::runtime::Runtime MakeRuntime() {
    return zenio::runtime::Builder::CurrentThread().EnableAll().Build();
}

}    // namespace

TEST(Task, ProducesValues) {
    auto rt = MakeRuntime();
    EXPECT_EQ(rt.BlockOn(ComputeInt()), 42);
    EXPECT_EQ(rt.BlockOn(ComputeBool()), true);
    EXPECT_NO_THROW((void)rt.BlockOn(ComputeVoid()));
    EXPECT_EQ(rt.BlockOn(ComputeString()), "hello");
    auto pointer = rt.BlockOn(ComputeMoveOnly());
    ASSERT_NE(pointer, nullptr);
    EXPECT_EQ(*pointer, 7);
}

TEST(Task, MoveOnlyParameter) {
    auto rt = MakeRuntime();
    EXPECT_EQ(rt.BlockOn(ConsumeMoveOnly(std::make_unique<int>(11))), 15);
}

TEST(Task, ReferenceResultPointsAtOriginal) {
    auto rt       = MakeRuntime();
    int  referent = 5;
    int& result   = rt.BlockOn(MakeRef(&referent));
    EXPECT_EQ(&result, &referent);
    result = 9;
    EXPECT_EQ(referent, 9);
}

TEST(Task, RethrowsExceptionUnchanged) {
    auto rt = MakeRuntime();
    try {
        (void)rt.BlockOn(Boom());
        FAIL() << "应当重抛";
    } catch (const std::runtime_error& e) {
        EXPECT_STREQ(e.what(), "task boom");
    }
    try {
        (void)rt.BlockOn(BoomVoid());
        FAIL() << "应当重抛";
    } catch (const std::runtime_error& e) {
        EXPECT_STREQ(e.what(), "void boom");
    }
}

TEST(Task, MiddleLayerCatchesAndReturns) {
    auto rt = MakeRuntime();
    EXPECT_EQ(rt.BlockOn(CatchAt(5)), -1);
}

TEST(Task, DeepNestingOnSmallStack) {
    NestOutcome outcome;
    NestRunArgs args {kNestDepth, false, &outcome};
    RunOnSmallStack(NestThreadMain, &args);
    ASSERT_TRUE(outcome.ran);
    EXPECT_FALSE(outcome.threw);
    EXPECT_EQ(outcome.value, 42 + kNestDepth);
    EXPECT_EQ(outcome.max_alive, kNestDepth + 1);    // 同时活着的 RAII 对象数等于层数
    EXPECT_EQ(outcome.alive_after, 0);               // 结束之后计数归零, 每层析构恰好一次
}

TEST(Task, DeepExceptionOnSmallStack) {
    NestOutcome outcome;
    NestRunArgs args {kNestDepth, true, &outcome};
    RunOnSmallStack(NestThreadMain, &args);
    ASSERT_TRUE(outcome.ran);
    ASSERT_TRUE(outcome.threw);
    EXPECT_TRUE(outcome.exception_ok);
    EXPECT_EQ(outcome.exception_what, "deep");       // 类型和 what() 不变
    EXPECT_EQ(outcome.max_alive, kNestDepth + 1);    // 沿途每层的析构函数都执行了
    EXPECT_EQ(outcome.alive_after, 0);
}

TEST(Task, LazyNotStartedUnlessAwaited) {
    int counter = 0;
    {
        zenio::Task<void> task = ComputeVoid();
        (void)task;
        EXPECT_EQ(counter, 0);    // 创建之后协程体没运行
    }    // 没被 co_await 就销毁, 协程体始终没运行
    zenio::Task<void> never = YieldOnce(&counter);
    {
        zenio::Task<void> discarded(std::move(never));
    }
    EXPECT_EQ(counter, 0);
}

TEST(Task, MoveTransfersFrame) {
    auto             rt    = MakeRuntime();
    zenio::Task<int> task  = ComputeInt();
    zenio::Task<int> moved = std::move(task);
    EXPECT_FALSE(task.is_valid());
    EXPECT_TRUE(moved.is_valid());
    EXPECT_EQ(rt.BlockOn(std::move(moved)), 42);
    EXPECT_FALSE(moved.is_valid());
}

TEST(Task, MoveAssignDestroysOldFrame) {
    auto             rt   = MakeRuntime();
    zenio::Task<int> task = ComputeInt();
    task                  = ComputeInt();    // 目标原来持有的协程被销毁 (未启动, 不触发断言)
    EXPECT_EQ(rt.BlockOn(std::move(task)), 42);
}

#if !defined(NDEBUG)
zenio::Task<int> Once() {
    co_return 1;
}

zenio::Task<int> AwaitTwice() {
    zenio::Task<int> task = Once();
    int              a    = co_await task;
    int              b    = co_await task;
    co_return a + b;
}

// debug 构建下进程中止; Release 下 assert 不生效, 这两个用例只在 Debug/ASAN 里注册

TEST(Task, DeathDoubleCoawaitAborts) {
    auto rt = MakeRuntime();
    EXPECT_DEATH((void)rt.BlockOn(AwaitTwice()), "");    // 同一个 Task 被 co_await 两次
}

TEST(Task, DeathDestroyRunningTaskAborts) {
    int               steps  = 0;
    auto              core   = std::make_shared<zenio::detail::RuntimeCore>(zenio::detail::RuntimeOptions {});
    zenio::Task<void> task   = YieldOnce(&steps);
    const auto        handle = zenio::detail::TaskAccess::Handle(task);
    core->scheduler().Schedule(zenio::detail::Runnable {handle, &core->root_context()});
    ASSERT_TRUE(core->scheduler().RunOne());
    EXPECT_EQ(steps, 1);
    EXPECT_FALSE(handle.done());    // 协程运行期间 (挂在 Yield 上)
    EXPECT_DEATH({ zenio::Task<void> doomed(std::move(task)); }, "");
    // 父进程把协程跑完再正常销毁, 不让测试自身的析构触发断言
    ASSERT_TRUE(core->scheduler().RunOne());
    EXPECT_EQ(steps, 2);
    EXPECT_TRUE(handle.done());
}

#endif    // !defined(NDEBUG)
