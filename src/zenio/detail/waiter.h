#ifndef ZENIO_DETAIL_WAITER_H
#define ZENIO_DETAIL_WAITER_H

#include "zenio/detail/scheduler.h"

#include <atomic>
#include <coroutine>
#include <cstdint>

namespace zenio::detail {

/**
 * @brief 每个 await 点一个, 嵌在协程帧里
 * @note 唤醒方可能在任意线程, 也可能早于协程挂起。本类保证协程恰好被唤醒一次
 */
class Waiter {
public:
    /**
     * @brief 在 awaiter 的 await_suspend 里调用
     * @return false 表示 Signal 已经到了, 协程应原地恢复; true 表示挂起
     */
    bool TrySuspend(std::coroutine_handle<> self) noexcept;

    /**
     * @brief 任意线程可调, 多次调用只有第一次生效
     * @note 第一次调用负责把协程交给调度器
     */
    void Signal() noexcept;

private:
    enum class State : std::uint32_t {
        EMPTY,        //!< 还没有走到挂起这一步
        SUSPENDED,    //!< 已登记, 协程正在挂起
        SCHEDULED,    //!< 已经有人负责恢复这个协程
    };

    std::atomic<State> state_ {State::EMPTY};
    std::atomic<bool>  signaled_ {false};
    Runnable           self_;    //!< 挂起时记下: 句柄和当时的上下文
};

}    // namespace zenio::detail

#endif    // ZENIO_DETAIL_WAITER_H
