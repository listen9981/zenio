#ifndef ZENIO_TASK_H
#define ZENIO_TASK_H

#include "zenio/detail/context.h"
#include "zenio/result.h"

#include <cassert>
#include <coroutine>
#include <exception>
#include <type_traits>
#include <utility>

namespace zenio::detail {

struct TaskAccess;

}    // namespace zenio::detail

namespace zenio {

/**
 * @brief 惰性启动的协程任务
 * @note 被 co_await 时才开始执行; 结束时产出 T, 或把协程体内的异常原样重抛
 * @note 协程帧归本对象所有, 在析构函数里销毁
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
    friend struct detail::TaskAccess;

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
    detail::PromiseContext*             saved_ = nullptr;    //!< 调用方的上下文, child 结束之后还原
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
    friend struct detail::TaskAccess;

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
    detail::PromiseContext*             saved_ = nullptr;    //!< 调用方的上下文, child 结束之后还原
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
    friend struct detail::TaskAccess;

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
    detail::PromiseContext*             saved_ = nullptr;    //!< 调用方的上下文, child 结束之后还原
};

//! @brief 让出执行权 (对应 tokio::task::yield_now)
[[nodiscard]] ZENIO_EXPORT detail::YieldAwaiter Yield();

}    // namespace zenio

// —— Task 的成员函数定义 (M3), 见 design/task.md 2.4 ——

namespace zenio::detail {

/**
 * @brief 库内部取 Task 的协程句柄
 * @note 只有创建根任务的地方用 (Runtime::BlockOn); 协程体里一律走 co_await
 */
struct TaskAccess {
    template <class T>
    [[nodiscard]] static std::coroutine_handle<typename Task<T>::promise_type> Handle(const Task<T>& task) noexcept {
        return task.handle_;
    }
};

/**
 * @brief 取走一个已经结束的 Task 的产出; 存的是异常就重抛
 * @tparam T       产出类型, 即 await_resume() 的返回类型
 * @tparam Stored  storage_ 的 OkType
 * @note Task<T>::Awaiter::await_resume() 和 Runtime::BlockOn 共用。
 *       Task<void> 的 storage_ 存 std::monostate, Task<T&> 存 T*, 所以 Stored 和 T 不总是同一个类型
 */
template <class T, class Stored>
T TakeTaskResult(Result<Stored, std::exception_ptr>& storage) {
    assert(!storage.is_empty() && "协程已经结束, storage_ 不该是空态");
    if (storage.is_err()) {
        std::rethrow_exception(std::move(storage).error());
    }
    if constexpr (std::is_void_v<T>) {
        (void)std::move(storage).value();
    } else if constexpr (std::is_reference_v<T>) {
        return *std::move(storage).value();
    } else {
        return std::move(storage).value();
    }
}

}    // namespace zenio::detail

namespace zenio {

// —— 主模板 ——

template <class T>
Task<T>::Task(std::coroutine_handle<promise_type> handle) noexcept : handle_(handle) {}

template <class T>
Task<T>::Task(Task&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}

template <class T>
Task<T>& Task<T>::operator=(Task&& other) noexcept {
    if (this != &other) {
        Task discarded(std::move(*this));    // 原来的协程在语句结束时销毁
        handle_ = std::exchange(other.handle_, {});
    }
    return *this;
}

template <class T>
Task<T>::~Task() {
    if (handle_) {
        assert((!handle_.promise().started || handle_.done()) && "Task 在协程结束之前被销毁");
        handle_.destroy();
    }
}

template <class T>
bool Task<T>::is_valid() const noexcept {
    return static_cast<bool>(handle_);
}

template <class T>
typename Task<T>::Awaiter Task<T>::operator co_await() const noexcept {
    return Awaiter(handle_);
}

template <class T>
Task<T> Task<T>::promise_type::get_return_object() {
    return Task<T>(std::coroutine_handle<promise_type>::from_promise(*this));
}

template <class T>
auto Task<T>::promise_type::initial_suspend() noexcept -> detail::InitialAwaiter<promise_type> {
    return {};
}

template <class T>
auto Task<T>::promise_type::final_suspend() noexcept -> detail::FinalAwaiter<promise_type> {
    return {};
}

template <class T>
void Task<T>::promise_type::return_value(T value) {
    storage_ = Result<T, std::exception_ptr>::Ok(std::move(value));
}

template <class T>
void Task<T>::promise_type::unhandled_exception() noexcept {
    storage_ = Result<T, std::exception_ptr>::Err(std::current_exception());
}

template <class T>
Task<T>::Awaiter::Awaiter(std::coroutine_handle<promise_type> handle) noexcept : handle_(handle) {}

template <class T>
bool Task<T>::Awaiter::await_ready() const noexcept {
    assert(handle_ && "co_await 了一个无效的 Task");
    return false;
}

template <class T>
std::coroutine_handle<> Task<T>::Awaiter::await_suspend(std::coroutine_handle<> caller) noexcept {
    promise_type& promise = handle_.promise();
    assert(promise.continuation_ == nullptr && "一个 Task 只能被 co_await 一次");
    promise.continuation_ = caller;
    saved_                = detail::CurrentContext();
    return handle_;
}

template <class T>
T Task<T>::Awaiter::await_resume() {
    detail::SetCurrentContext(saved_);
    return detail::TakeTaskResult<T>(handle_.promise().storage_);
}

// —— Task<T&> 偏特化: 与主模板逐字相同, 只换产出相关的部分 ——

template <class T>
Task<T&>::Task(std::coroutine_handle<promise_type> handle) noexcept : handle_(handle) {}

template <class T>
Task<T&>::Task(Task&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}

template <class T>
Task<T&>& Task<T&>::operator=(Task&& other) noexcept {
    if (this != &other) {
        Task discarded(std::move(*this));    // 原来的协程在语句结束时销毁
        handle_ = std::exchange(other.handle_, {});
    }
    return *this;
}

template <class T>
Task<T&>::~Task() {
    if (handle_) {
        assert((!handle_.promise().started || handle_.done()) && "Task 在协程结束之前被销毁");
        handle_.destroy();
    }
}

template <class T>
bool Task<T&>::is_valid() const noexcept {
    return static_cast<bool>(handle_);
}

template <class T>
typename Task<T&>::Awaiter Task<T&>::operator co_await() const noexcept {
    return Awaiter(handle_);
}

template <class T>
Task<T&> Task<T&>::promise_type::get_return_object() {
    return Task<T&>(std::coroutine_handle<promise_type>::from_promise(*this));
}

template <class T>
auto Task<T&>::promise_type::initial_suspend() noexcept -> detail::InitialAwaiter<promise_type> {
    return {};
}

template <class T>
auto Task<T&>::promise_type::final_suspend() noexcept -> detail::FinalAwaiter<promise_type> {
    return {};
}

template <class T>
void Task<T&>::promise_type::return_value(T& value) {
    storage_ = Result<T*, std::exception_ptr>::Ok(&value);
}

template <class T>
void Task<T&>::promise_type::unhandled_exception() noexcept {
    storage_ = Result<T*, std::exception_ptr>::Err(std::current_exception());
}

template <class T>
Task<T&>::Awaiter::Awaiter(std::coroutine_handle<promise_type> handle) noexcept : handle_(handle) {}

template <class T>
bool Task<T&>::Awaiter::await_ready() const noexcept {
    assert(handle_ && "co_await 了一个无效的 Task");
    return false;
}

template <class T>
std::coroutine_handle<> Task<T&>::Awaiter::await_suspend(std::coroutine_handle<> caller) noexcept {
    promise_type& promise = handle_.promise();
    assert(promise.continuation_ == nullptr && "一个 Task 只能被 co_await 一次");
    promise.continuation_ = caller;
    saved_                = detail::CurrentContext();
    return handle_;
}

template <class T>
T& Task<T&>::Awaiter::await_resume() {
    detail::SetCurrentContext(saved_);
    return detail::TakeTaskResult<T&>(handle_.promise().storage_);
}

// —— Task<void> 显式特化: 不是模板, 类外的定义加 inline ——

inline Task<void>::Task(std::coroutine_handle<promise_type> handle) noexcept : handle_(handle) {}

inline Task<void>::Task(Task&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}

inline Task<void>& Task<void>::operator=(Task&& other) noexcept {
    if (this != &other) {
        Task discarded(std::move(*this));    // 原来的协程在语句结束时销毁
        handle_ = std::exchange(other.handle_, {});
    }
    return *this;
}

inline Task<void>::~Task() {
    if (handle_) {
        assert((!handle_.promise().started || handle_.done()) && "Task 在协程结束之前被销毁");
        handle_.destroy();
    }
}

inline bool Task<void>::is_valid() const noexcept {
    return static_cast<bool>(handle_);
}

inline Task<void>::Awaiter Task<void>::operator co_await() const noexcept {
    return Awaiter(handle_);
}

inline Task<void> Task<void>::promise_type::get_return_object() {
    return Task<void>(std::coroutine_handle<promise_type>::from_promise(*this));
}

inline auto Task<void>::promise_type::initial_suspend() noexcept -> detail::InitialAwaiter<promise_type> {
    return {};
}

inline auto Task<void>::promise_type::final_suspend() noexcept -> detail::FinalAwaiter<promise_type> {
    return {};
}

inline void Task<void>::promise_type::return_void() {
    storage_ = Result<void, std::exception_ptr>::Ok();
}

inline void Task<void>::promise_type::unhandled_exception() noexcept {
    storage_ = Result<void, std::exception_ptr>::Err(std::current_exception());
}

inline Task<void>::Awaiter::Awaiter(std::coroutine_handle<promise_type> handle) noexcept : handle_(handle) {}

inline bool Task<void>::Awaiter::await_ready() const noexcept {
    assert(handle_ && "co_await 了一个无效的 Task");
    return false;
}

inline std::coroutine_handle<> Task<void>::Awaiter::await_suspend(std::coroutine_handle<> caller) noexcept {
    promise_type& promise = handle_.promise();
    assert(promise.continuation_ == nullptr && "一个 Task 只能被 co_await 一次");
    promise.continuation_ = caller;
    saved_                = detail::CurrentContext();
    return handle_;
}

inline void Task<void>::Awaiter::await_resume() {
    detail::SetCurrentContext(saved_);
    detail::TakeTaskResult<void>(handle_.promise().storage_);
}

}    // namespace zenio

#endif    // ZENIO_TASK_H
