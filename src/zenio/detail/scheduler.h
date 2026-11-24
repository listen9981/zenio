#ifndef ZENIO_DETAIL_SCHEDULER_H
#define ZENIO_DETAIL_SCHEDULER_H

#include "zenio/detail/context.h"

#include <coroutine>
#include <deque>

namespace zenio::detail {

/**
 * @brief 交给调度器的协程: 句柄, 加上恢复它时要装进线程局部存储的上下文
 */
struct Runnable {
    std::coroutine_handle<> handle;               //!< 已经挂起、可以恢复的协程
    PromiseContext*         context = nullptr;    //!< 它挂起时的上下文, 见 design/task.md 2.2
};

/**
 * @brief 为一个正在挂起的协程生成 Runnable
 * @note 在协程自己的线程上、挂起的时候调用: 当时的上下文就是它恢复时要装回去的
 */
[[nodiscard]] Runnable MakeRunnable(std::coroutine_handle<> handle) noexcept;

/**
 * @brief 在作用域内把当前线程的上下文换成另一个, 离开作用域时还原
 */
class ScopedContext {
public:
    explicit ScopedContext(PromiseContext* context) noexcept;

    ScopedContext(const ScopedContext&)            = delete;
    ScopedContext& operator=(const ScopedContext&) = delete;

    ~ScopedContext();

private:
    PromiseContext* previous_;
};

/**
 * @brief 调度器: 决定哪个协程什么时候运行
 * @note M3: 单线程, 一个先进先出的队列, 没有同步
 */
class Scheduler {
public:
    Scheduler()                            = default;
    Scheduler(const Scheduler&)            = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    //! @brief 入队
    void Schedule(Runnable runnable);

    //! @brief 取一个任务运行。队列是空的就返回 false
    [[nodiscard]] bool RunOne();

private:
    std::deque<Runnable> queue_;
};

}    // namespace zenio::detail

#endif    // ZENIO_DETAIL_SCHEDULER_H
