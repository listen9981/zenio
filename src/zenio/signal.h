#ifndef ZENIO_SIGNAL_H
#define ZENIO_SIGNAL_H

#include "zenio/detail/awaiters.h"

#include <cstdint>

namespace zenio::signal {

/**
 * @brief 一种信号的接收流
 * @note 只能在创建它的运行时里使用
 */
class ZENIO_EXPORT Signal {
public:
    Signal() noexcept = default;
    Signal(Signal&& other) noexcept;
    Signal& operator=(Signal&& other) noexcept;
    Signal(const Signal&)            = delete;
    Signal& operator=(const Signal&) = delete;

    ~Signal();

    /**
     * @brief 等下一次信号, 每收到一次返回一次
     * @throws zenio::CancelledError 调用方在等待期间被取消
     */
    [[nodiscard]] detail::SignalRecvAwaiter Recv();

    //! @brief 是否是一个有效的信号流
    [[nodiscard]] bool is_valid() const noexcept;

private:
    friend Signal CtrlC();

    explicit Signal(std::uint64_t id) noexcept;

    std::uint64_t id_ = 0;    //!< 信号源编号, 0 表示空句柄
};

/**
 * @brief SIGINT 的接收流 (对应 tokio::signal::ctrl_c)
 * @throws std::runtime_error 不在运行时上下文里, 或运行时没有开启 I/O
 */
[[nodiscard]] ZENIO_EXPORT Signal CtrlC();

}    // namespace zenio::signal

#endif    // ZENIO_SIGNAL_H
