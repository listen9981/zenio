#ifndef ZENIO_DETAIL_AWAITERS_H
#define ZENIO_DETAIL_AWAITERS_H

#include "zenio/net/address.h"
#include "zenio/task.h"

#include <chrono>
#include <coroutine>
#include <cstddef>
#include <type_traits>
#include <utility>
#include <variant>

namespace zenio::detail {

template <class A>
concept HasOperatorCoAwait = requires(A& a) { a.operator co_await(); };

/**
 * @brief 一个 awaitable 的产出类型: 它的 awaiter 的 await_resume() 返回什么, 产出就是什么
 */
template <class A>
struct await_result {
    using type = decltype(std::declval<A&>().await_resume());
};

//! @brief Task<T>、JoinHandle<T> 这类通过 operator co_await 等待的类型
template <class A>
    requires HasOperatorCoAwait<A>
struct await_result<A> {
    using type = decltype(std::declval<A&>().operator co_await().await_resume());
};

template <class A>
using await_result_t = typename await_result<A>::type;

/**
 * @brief 分支的产出类型要能放进 std::variant
 * @note 产出是引用的分支不支持: std::variant 里放不了引用
 * @note 定义在本文件而不是 select.h: SelectAwaiter 的友元声明需要完整约束, 见 M1 实现偏差
 */
template <class T>
concept SelectOutputType = !std::is_reference_v<T>;

// —— awaiter 类的前置声明: 创建方声明要用到它们的返回类型 ——
template <class T, class R>
class SpawnAndSeizeAwaiter;
template <class Awaitable>
class UncancellableAwaiter;
template <class... Awaitables>
class SelectAwaiter;
class SleepAwaiter;

}    // namespace zenio::detail

// —— 创建方的前置声明。限定名的友元必须有先前声明, 这里的签名与各自头文件里的声明保持一致 ——
namespace zenio {

template <class T>
class JoinHandle;
template <class T>
class JoinSet;

//! @brief 派发任务, 并把当前 worker 让给它 (语义来自 bthread 的 bthread_start_urgent)。只能写成 co_await
template <class T>
[[nodiscard]] ZENIO_EXPORT detail::SpawnAndSeizeAwaiter<T, JoinHandle<T>> SpawnAndSeize(Task<T> task);

//! @brief 用一个不可取消的上下文等待 awaitable
template <class Awaitable>
[[nodiscard]] ZENIO_EXPORT detail::UncancellableAwaiter<std::decay_t<Awaitable>> Uncancellable(Awaitable&& awaitable);

//! @brief 并发等待多个操作, 第一个完成的获胜, 其余被取消
template <class... Awaitables>
    requires(sizeof...(Awaitables) >= 1 && sizeof...(Awaitables) <= 64 &&
             (detail::SelectOutputType<detail::await_result_t<std::decay_t<Awaitables>>> && ...))
[[nodiscard]] ZENIO_EXPORT detail::SelectAwaiter<std::decay_t<Awaitables>...> Select(Awaitables&&... awaitables);

//! @brief 等待一段时间 (对应 tokio::time::sleep)
[[nodiscard]] ZENIO_EXPORT detail::SleepAwaiter Sleep(std::chrono::nanoseconds duration);

//! @brief 等到一个时刻 (对应 tokio::time::sleep_until)。时钟用 std::chrono::steady_clock
[[nodiscard]] ZENIO_EXPORT detail::SleepAwaiter SleepUntil(std::chrono::steady_clock::time_point deadline);

}    // namespace zenio

namespace zenio::net {

class TcpListener;
class TcpStream;
class UdpSocket;

}    // namespace zenio::net

namespace zenio::signal {

class Signal;

}    // namespace zenio::signal

namespace zenio::detail {

/**
 * @brief TcpListener::Accept() 的 awaiter
 */
class AcceptAwaiter {
public:
    [[nodiscard]] bool                         await_ready() noexcept;
    bool                                       await_suspend(std::coroutine_handle<> caller) noexcept;
    std::pair<net::TcpStream, net::SocketAddr> await_resume();

private:
    friend class net::TcpListener;

    AcceptAwaiter() noexcept;
};

/**
 * @brief TcpStream::Connect() 的 awaiter
 */
class ConnectAwaiter {
public:
    [[nodiscard]] bool await_ready() noexcept;
    bool               await_suspend(std::coroutine_handle<> caller) noexcept;
    net::TcpStream     await_resume();

private:
    friend class net::TcpStream;

    ConnectAwaiter() noexcept;
};

/**
 * @brief TcpStream::ReadSome() 的 awaiter
 */
class ReadSomeAwaiter {
public:
    [[nodiscard]] bool await_ready() noexcept;
    bool               await_suspend(std::coroutine_handle<> caller) noexcept;
    std::size_t        await_resume();

private:
    friend class net::TcpStream;

    ReadSomeAwaiter() noexcept;
};

/**
 * @brief TcpStream::ReadExact() 的 awaiter
 */
class ReadExactAwaiter {
public:
    [[nodiscard]] bool await_ready() noexcept;
    bool               await_suspend(std::coroutine_handle<> caller) noexcept;
    std::size_t        await_resume();

private:
    friend class net::TcpStream;

    ReadExactAwaiter() noexcept;
};

/**
 * @brief TcpStream::WriteSome() 的 awaiter
 */
class WriteSomeAwaiter {
public:
    [[nodiscard]] bool await_ready() noexcept;
    bool               await_suspend(std::coroutine_handle<> caller) noexcept;
    std::size_t        await_resume();

private:
    friend class net::TcpStream;

    WriteSomeAwaiter() noexcept;
};

/**
 * @brief TcpStream::WriteAll() 的 awaiter
 */
class WriteAllAwaiter {
public:
    [[nodiscard]] bool await_ready() noexcept;
    bool               await_suspend(std::coroutine_handle<> caller) noexcept;
    void               await_resume();

private:
    friend class net::TcpStream;

    WriteAllAwaiter() noexcept;
};

/**
 * @brief UdpSocket::SendTo() 的 awaiter
 */
class SendToAwaiter {
public:
    [[nodiscard]] bool await_ready() noexcept;
    bool               await_suspend(std::coroutine_handle<> caller) noexcept;
    std::size_t        await_resume();

private:
    friend class net::UdpSocket;

    SendToAwaiter() noexcept;
};

/**
 * @brief UdpSocket::RecvFrom() 的 awaiter
 */
class RecvFromAwaiter {
public:
    [[nodiscard]] bool                      await_ready() noexcept;
    bool                                    await_suspend(std::coroutine_handle<> caller) noexcept;
    std::pair<std::size_t, net::SocketAddr> await_resume();

private:
    friend class net::UdpSocket;

    RecvFromAwaiter() noexcept;
};

/**
 * @brief Sleep()、SleepUntil() 的 awaiter
 */
class SleepAwaiter {
public:
    [[nodiscard]] bool await_ready() noexcept;
    bool               await_suspend(std::coroutine_handle<> caller) noexcept;
    void               await_resume();

private:
    friend SleepAwaiter zenio::Sleep(std::chrono::nanoseconds duration);
    friend SleepAwaiter zenio::SleepUntil(std::chrono::steady_clock::time_point deadline);

    SleepAwaiter() noexcept;
};

/**
 * @brief Signal::Recv() 的 awaiter
 */
class SignalRecvAwaiter {
public:
    [[nodiscard]] bool await_ready() noexcept;
    bool               await_suspend(std::coroutine_handle<> caller) noexcept;
    void               await_resume();

private:
    friend class zenio::signal::Signal;

    SignalRecvAwaiter() noexcept;
};

/**
 * @brief JoinHandle<T>::operator co_await() 的 awaiter
 */
template <class T>
class JoinHandleAwaiter {
public:
    [[nodiscard]] bool await_ready() noexcept;
    bool               await_suspend(std::coroutine_handle<> caller) noexcept;
    T                  await_resume();

private:
    friend class zenio::JoinHandle<T>;

    JoinHandleAwaiter() noexcept;
};

/**
 * @brief SpawnAndSeize()、JoinSet<T>::SpawnAndSeize() 的 awaiter
 */
template <class T, class R>
class SpawnAndSeizeAwaiter {
public:
    [[nodiscard]] bool await_ready() noexcept;
    bool               await_suspend(std::coroutine_handle<> caller) noexcept;
    R                  await_resume();

private:
    template <class T_>
    friend detail::SpawnAndSeizeAwaiter<T_, zenio::JoinHandle<T_>> zenio::SpawnAndSeize(zenio::Task<T_> task);
    friend class zenio::JoinSet<T>;

    SpawnAndSeizeAwaiter() noexcept;
};

/**
 * @brief JoinSet<T>::JoinAll() 的 awaiter
 */
template <class T>
class JoinAllAwaiter {
public:
    [[nodiscard]] bool                     await_ready() noexcept;
    bool                                   await_suspend(std::coroutine_handle<> caller) noexcept;
    typename zenio::JoinSet<T>::JoinAllOut await_resume();

private:
    friend class zenio::JoinSet<T>;

    JoinAllAwaiter() noexcept;
};

/**
 * @brief JoinSet<T>::JoinNext() 的 awaiter
 */
template <class T>
class JoinNextAwaiter {
public:
    [[nodiscard]] bool                      await_ready() noexcept;
    bool                                    await_suspend(std::coroutine_handle<> caller) noexcept;
    typename zenio::JoinSet<T>::JoinNextOut await_resume();

private:
    friend class zenio::JoinSet<T>;

    JoinNextAwaiter() noexcept;
};

/**
 * @brief Select() 的 awaiter
 */
template <class... Awaitables>
class SelectAwaiter {
public:
    [[nodiscard]] bool                                           await_ready() noexcept;
    bool                                                         await_suspend(std::coroutine_handle<> caller) noexcept;
    std::variant<VoidToMonostate<await_result_t<Awaitables>>...> await_resume();

private:
    template <class... As>
        requires(sizeof...(As) >= 1 && sizeof...(As) <= 64 &&
                 (SelectOutputType<await_result_t<std::decay_t<As>>> && ...))
    friend detail::SelectAwaiter<std::decay_t<As>...> zenio::Select(As&&... awaitables);

    SelectAwaiter() noexcept;
};

/**
 * @brief Uncancellable() 的 awaiter
 */
template <class Awaitable>
class UncancellableAwaiter {
public:
    [[nodiscard]] bool        await_ready() noexcept;
    bool                      await_suspend(std::coroutine_handle<> caller) noexcept;
    await_result_t<Awaitable> await_resume();

private:
    template <class A_>
    friend detail::UncancellableAwaiter<std::decay_t<A_>> zenio::Uncancellable(A_&& awaitable);

    UncancellableAwaiter() noexcept;
};

}    // namespace zenio::detail

#endif    // ZENIO_DETAIL_AWAITERS_H
