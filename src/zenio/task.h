#ifndef ZENIO_TASK_H
#define ZENIO_TASK_H

#include "zenio/detail/context.h"
#include "zenio/result.h"

#include <coroutine>
#include <exception>
#include <utility>

namespace zenio {

/**
 * @brief 惰性启动的协程任务
 * @note 被 co_await 时才开始执行; 结束时产出 T, 或把协程体内的异常原样重抛
 */
template <class T>
class [[nodiscard]] ZENIO_EXPORT Task {
public:
    using value_type = T;
    struct promise_type;
    class Awaiter;

    Task(Task&& other) noexcept;
    Task& operator=(Task&& other) noexcept;
    Task(const Task&)            = delete;
    Task& operator=(const Task&) = delete;

    //! @note 协程已启动但还没结束时销毁 Task, 视为未定义行为
    ~Task();

    //! @brief 是否持有一个协程
    [[nodiscard]] bool is_valid() const noexcept;

    //! @brief 开始执行并等待它结束
    Awaiter operator co_await() const noexcept;

private:
    explicit Task(std::coroutine_handle<promise_type> handle) noexcept;

    std::coroutine_handle<promise_type> handle_;
};

/**
 * @brief Task 的 promise: 惰性启动, 结束时把产出或异常存进 storage_, final_suspend 交接等待方
 */
template <class T>
struct Task<T>::promise_type : detail::PromiseContext {
    using value_type = T;

    [[nodiscard]] Task<T> get_return_object();

    //! @brief 总是挂起。它的 await_resume 是协程启动后、函数体之前执行的第一件事
    [[nodiscard]] detail::InitialAwaiter<promise_type> initial_suspend() noexcept;
    //! @brief 把控制权直接交给等待方 (symmetric transfer)
    [[nodiscard]] detail::FinalAwaiter<promise_type> final_suspend() noexcept;

    //! @brief zenio 的 awaiter 自己从线程局部存储取上下文, 这里原样透传
    template <class A>
    A&& await_transform(A&& a) noexcept {
        return std::forward<A>(a);
    }

    void return_value(T value);
    void unhandled_exception() noexcept;

    Result<T, std::exception_ptr> storage_;         //!< 产出, 或协程体内的异常; 空态表示还没结束
    std::coroutine_handle<>       continuation_;    //!< 等待方, 没有则为空
    std::coroutine_handle<>       seize_caller_;    //!< SpawnAndSeize 的调用方, 见 design/spawn.md
};

/**
 * @brief co_await Task<T> 的 awaiter: 挂起调用方并启动 child, 结束时产出或重抛异常
 */
template <class T>
class Task<T>::Awaiter {
public:
    [[nodiscard]] bool await_ready() const noexcept;

    //! @brief 返回 child 的句柄: 调用方先完成挂起, child 在同一线程上启动
    std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller) noexcept;

    //! @brief 产出 T; child 存的是异常就重抛
    T await_resume();

private:
    friend class Task<T>;

    explicit Awaiter(std::coroutine_handle<promise_type> handle) noexcept;

    std::coroutine_handle<promise_type> handle_;
};

/**
 * @brief 惰性启动的协程任务 (void 特化)
 * @note 被 co_await 时才开始执行; 协程体内的异常原样重抛
 */
template <>
class [[nodiscard]] ZENIO_EXPORT Task<void> {
public:
    using value_type = void;
    struct promise_type;
    class Awaiter;

    Task(Task&& other) noexcept;
    Task& operator=(Task&& other) noexcept;
    Task(const Task&)            = delete;
    Task& operator=(const Task&) = delete;

    //! @note 协程已启动但还没结束时销毁 Task, 视为未定义行为
    ~Task();

    //! @brief 是否持有一个协程
    [[nodiscard]] bool is_valid() const noexcept;

    //! @brief 开始执行并等待它结束
    Awaiter operator co_await() const noexcept;

private:
    explicit Task(std::coroutine_handle<promise_type> handle) noexcept;

    std::coroutine_handle<promise_type> handle_;
};

/**
 * @brief Task<void> 的 promise: return_void, 产出只区分正常结束与异常
 */
struct Task<void>::promise_type : detail::PromiseContext {
    using value_type = void;

    [[nodiscard]] Task<void> get_return_object();

    //! @brief 总是挂起。它的 await_resume 是协程启动后、函数体之前执行的第一件事
    [[nodiscard]] detail::InitialAwaiter<promise_type> initial_suspend() noexcept;
    //! @brief 把控制权直接交给等待方 (symmetric transfer)
    [[nodiscard]] detail::FinalAwaiter<promise_type> final_suspend() noexcept;

    //! @brief zenio 的 awaiter 自己从线程局部存储取上下文, 这里原样透传
    template <class A>
    A&& await_transform(A&& a) noexcept {
        return std::forward<A>(a);
    }

    void return_void();
    void unhandled_exception() noexcept;

    Result<void, std::exception_ptr> storage_;         //!< 协程体内的异常; 空态表示还没结束
    std::coroutine_handle<>          continuation_;    //!< 等待方, 没有则为空
    std::coroutine_handle<>          seize_caller_;    //!< SpawnAndSeize 的调用方, 见 design/spawn.md
};

/**
 * @brief co_await Task<void> 的 awaiter: 挂起调用方并启动 child, 结束时重抛异常
 */
class Task<void>::Awaiter {
public:
    [[nodiscard]] bool await_ready() const noexcept;

    //! @brief 返回 child 的句柄: 调用方先完成挂起, child 在同一线程上启动
    std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller) noexcept;

    //! @brief 产出 void; child 存的是异常就重抛
    void await_resume();

private:
    friend class Task<void>;

    explicit Awaiter(std::coroutine_handle<promise_type> handle) noexcept;

    std::coroutine_handle<promise_type> handle_;
};

/**
 * @brief 惰性启动的协程任务 (引用特化): 产出按引用返回, 内部以指针存储
 * @note 被 co_await 时才开始执行; 结束时产出 T&, 或把协程体内的异常原样重抛
 */
template <class T>
class [[nodiscard]] ZENIO_EXPORT Task<T&> {
public:
    using value_type = T&;
    struct promise_type;
    class Awaiter;

    Task(Task&& other) noexcept;
    Task& operator=(Task&& other) noexcept;
    Task(const Task&)            = delete;
    Task& operator=(const Task&) = delete;

    //! @note 协程已启动但还没结束时销毁 Task, 视为未定义行为
    ~Task();

    //! @brief 是否持有一个协程
    [[nodiscard]] bool is_valid() const noexcept;

    //! @brief 开始执行并等待它结束
    Awaiter operator co_await() const noexcept;

private:
    explicit Task(std::coroutine_handle<promise_type> handle) noexcept;

    std::coroutine_handle<promise_type> handle_;
};

/**
 * @brief Task<T&> 的 promise: return_value(T&), 产出以 T* 存进 storage_
 */
template <class T>
struct Task<T&>::promise_type : detail::PromiseContext {
    using value_type = T&;

    [[nodiscard]] Task<T&> get_return_object();

    //! @brief 总是挂起。它的 await_resume 是协程启动后、函数体之前执行的第一件事
    [[nodiscard]] detail::InitialAwaiter<promise_type> initial_suspend() noexcept;
    //! @brief 把控制权直接交给等待方 (symmetric transfer)
    [[nodiscard]] detail::FinalAwaiter<promise_type> final_suspend() noexcept;

    //! @brief zenio 的 awaiter 自己从线程局部存储取上下文, 这里原样透传
    template <class A>
    A&& await_transform(A&& a) noexcept {
        return std::forward<A>(a);
    }

    void return_value(T& value);
    void unhandled_exception() noexcept;

    Result<T*, std::exception_ptr> storage_;         //!< 产出的指针, 或协程体内的异常; 空态表示还没结束
    std::coroutine_handle<>        continuation_;    //!< 等待方, 没有则为空
    std::coroutine_handle<>        seize_caller_;    //!< SpawnAndSeize 的调用方, 见 design/spawn.md
};

/**
 * @brief co_await Task<T&> 的 awaiter: 挂起调用方并启动 child, 结束时产出引用或重抛异常
 */
template <class T>
class Task<T&>::Awaiter {
public:
    [[nodiscard]] bool await_ready() const noexcept;

    //! @brief 返回 child 的句柄: 调用方先完成挂起, child 在同一线程上启动
    std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller) noexcept;

    //! @brief 产出 T&; child 存的是异常就重抛
    T& await_resume();

private:
    friend class Task<T&>;

    explicit Awaiter(std::coroutine_handle<promise_type> handle) noexcept;

    std::coroutine_handle<promise_type> handle_;
};

//! @brief 让出执行权 (对应 tokio::task::yield_now)
[[nodiscard]] ZENIO_EXPORT detail::YieldAwaiter Yield();

}    // namespace zenio

#endif    // ZENIO_TASK_H
