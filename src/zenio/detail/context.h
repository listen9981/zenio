#ifndef ZENIO_DETAIL_CONTEXT_H
#define ZENIO_DETAIL_CONTEXT_H

#include "zenio/detail/export.h"

#include <coroutine>
#include <stop_token>

namespace zenio::detail {

class RuntimeCore;

/**
 * @brief 每个 zenio 协程 promise 的公共部分
 */
struct PromiseContext {
    std::stop_token token;             //!< 取消令牌, 启动时从上一级复制下来
    RuntimeCore*    core = nullptr;    //!< 所属运行时的内部对象: 调度器和 Poller
};

/**
 * @brief 取当前协程的上下文
 * @return 不在 zenio 协程里运行时返回空指针
 */
PromiseContext* CurrentContext() noexcept;

/**
 * @brief Task 的 initial_suspend() 的返回值
 * @note await_resume 是协程启动后、函数体之前执行的第一件事
 */
template <class Promise>
class InitialAwaiter {
public:
    [[nodiscard]] static constexpr bool await_ready() noexcept { return false; }

    //! @brief 一定挂起: 协程在被 co_await 之前不运行
    void await_suspend(std::coroutine_handle<Promise>) noexcept {}

    //! @brief 把线程局部存储里的上下文复制进 promise; seize_caller_ 非空时把它重新投递
    void await_resume() noexcept;
};

/**
 * @brief Task 的 final_suspend() 的返回值
 */
template <class Promise>
class FinalAwaiter {
public:
    [[nodiscard]] static constexpr bool await_ready() noexcept { return false; }

    /**
     * @brief 返回等待方的句柄, 它在同一线程上接着运行 (symmetric transfer)
     * @note 没有等待方时返回 std::noop_coroutine()
     */
    std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> self) noexcept;

    //! @brief 不应被调用: 协程在 final_suspend 之后不会再恢复
    void await_resume() noexcept;
};

/**
 * @brief Yield() 的 awaiter: 把调用方投递到队列, 让别的任务先运行
 */
class YieldAwaiter {
public:
    [[nodiscard]] static constexpr bool await_ready() noexcept { return false; }

    //! @brief 一定挂起; 调用方被投递到当前 worker 的本地队列
    void await_suspend(std::coroutine_handle<> caller) noexcept;

    void await_resume() noexcept {}
};

}    // namespace zenio::detail

#endif    // ZENIO_DETAIL_CONTEXT_H
