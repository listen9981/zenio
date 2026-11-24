# Task 与上下文

状态: 草稿
细化程度: 全部已细化 (M1 写声明, M3 写实现)。`seize_caller_` 的重新投递在 M5 细化
依据: S5、S8、S17; A4、A5、A17、A18

## 1. 职责与边界

- `Task<T>` 是 zenio 的协程返回类型: 惰性启动, 被 `co_await` 时才开始执行, 结束时产出 `T` 或把协程体内的异常原样重抛。tokio 没有同名的类型, 因为 Rust 的 async 函数直接就是 future。
- `Yield()` 对应 `tokio::task::yield_now`。
- 本文件还规定两件所有子系统都要用的事: 协程上下文怎么传播, awaiter 怎么写。

不在本文件: Task 怎么被派发 (design/spawn.md), 怎么成组管理 (design/join_set.md), 协程什么时候被恢复 (design/scheduler.md)。

## 2. 对外接口

### 2.1 `zenio/task.h`

```cpp
namespace zenio::detail {
struct TaskAccess;
}

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

//! @brief 让出执行权 (对应 tokio::task::yield_now)
[[nodiscard]] ZENIO_EXPORT detail::YieldAwaiter Yield();

}  // namespace zenio
```

相对 M1 有三处改动, 都不涉及公开的函数签名: `Task` 多了一个友元 `detail::TaskAccess`, `Awaiter` 多了成员 `saved_`, `promise_type` 从 `PromiseContext` 继承来的成员多了一个 `started` (见 2.2)。

`Task<void>` 和 `Task<T&>` 只有下表列出的地方不同:

| | `Task<void>` | `Task<T&>` |
|---|---|---|
| 写法 | 显式特化 `template <> class Task<void>` | 偏特化 `template <class T> class Task<T&>` |
| 协程返回 | `void return_void();` | `void return_value(T& value);` |
| `storage_` 的类型 | `Result<void, std::exception_ptr>` | `Result<T*, std::exception_ptr>` |
| `Awaiter::await_resume()` 返回 | `void` | `T&` |
| 成员函数定义 | 不是模板, 类外的定义要加 `inline` | 与主模板相同 |

### 2.2 协程上下文

下面这些定义在 `zenio/detail/context.h`。`InitialAwaiter`、`FinalAwaiter`、`YieldAwaiter` 三个类也定义在这个头文件里, 这样 `task.h` 不需要依赖 `awaiters.h`:

```cpp
namespace zenio::detail {

class RuntimeCore;

/**
 * @brief 每个 zenio 协程 promise 的公共部分
 */
struct PromiseContext {
    std::stop_token token;             //!< 取消令牌, 启动时从上一级复制下来
    RuntimeCore*    core = nullptr;    //!< 所属运行时的内部对象: 调度器和 Poller
    bool            started = false;   //!< 已经开始执行, Task 的析构函数用它做断言
};

/**
 * @brief 取当前线程的上下文
 * @return 不在 zenio 协程里运行时返回空指针
 * @note 标了导出: 公开头文件里的模板实现要调用它, 见 design/build.md 2.3
 */
ZENIO_EXPORT PromiseContext* CurrentContext() noexcept;

/**
 * @brief 把 context 设为当前线程的上下文
 * @note 调度器恢复协程时装上它挂起时的上下文; Task 的交接也用它切换
 */
ZENIO_EXPORT void SetCurrentContext(PromiseContext* context) noexcept;

/**
 * @brief Task 的 initial_suspend() 的返回值
 * @note await_resume 是协程启动后、函数体之前执行的第一件事
 */
template <class Promise>
class InitialAwaiter {
public:
    [[nodiscard]] static constexpr bool await_ready() noexcept { return false; }

    //! @brief 一定挂起: 协程在被 co_await 之前不运行
    void await_suspend(std::coroutine_handle<Promise> self) noexcept { self_ = self; }

    //! @brief 把线程局部存储里的上下文复制进 promise 并切换过去; seize_caller_ 非空时把它重新投递
    void await_resume() noexcept;

private:
    std::coroutine_handle<Promise> self_;    //!< 启动的协程
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
 * @note 类上标了导出: 使用者的协程体直接调用它的 await_suspend, 见 design/build.md 2.3
 */
class ZENIO_EXPORT YieldAwaiter {
public:
    [[nodiscard]] static constexpr bool await_ready() noexcept { return false; }

    //! @brief 一定挂起; 调用方被投递到当前 worker 的本地队列
    void await_suspend(std::coroutine_handle<> caller) noexcept;

    void await_resume() noexcept {}
};

}  // namespace zenio::detail
```

线程局部存储里放的是一个指针, 也就是"当前上下文"。每个线程各有一份, 定义在 `context.cpp` 里。规则 (A18):

1. **启动** (`InitialAwaiter::await_resume`): 把当前上下文的令牌和运行时复制进自己的 promise, 然后把当前上下文切换成自己的 promise。所以子协程的令牌和运行时与启动它的那一方相同; 当前上下文为空 (不在运行时里) 时保持默认值。
2. **挂起**: 协程把"句柄加当前上下文"一起交给调度器 (design/scheduler.md 的 `Runnable`)。调度器恢复它之前把这份上下文装回去, 运行完再还原成运行之前的。所以协程恢复时看到的上下文和挂起时一样。
3. **`co_await Task` 结束** (`Task<T>::Awaiter::await_resume`): 把当前上下文还原成调用方的。child 启动时切走了, 这里切回来。
4. **根任务**没有上一级, 它的上下文由创建它的一方提供: 那一方把上下文的地址随句柄一起交给调度器, 启动时照第 1 条复制。`BlockOn` 的做法见 design/runtime.md 4.2, `Spawn` 的做法见 design/spawn.md。
5. 当前上下文是空指针, 表示这个线程现在不在任何运行时里。这时启动的协程拿不到运行时, 需要运行时的 awaiter 会抛 `std::runtime_error`。
6. 上下文和当前不一样的交接 (`Uncancellable`) 由交接的一方负责切换和还原, 见 design/waiter.md 4.3。

三个类的成员函数定义, 模板写在头文件里, `YieldAwaiter::await_suspend` 写在 `context.cpp` 里:

```cpp
template <class Promise>
void InitialAwaiter<Promise>::await_resume() noexcept {
    PromiseContext& context = self_.promise();
    if (const PromiseContext* current = CurrentContext()) {
        context.token = current->token;
        context.core  = current->core;
    }
    context.started = true;
    SetCurrentContext(&context);
    // M5: self_.promise().seize_caller_ 非空时在这里把它重新投递, 见 design/spawn.md 4.2
}

template <class Promise>
std::coroutine_handle<> FinalAwaiter<Promise>::await_suspend(std::coroutine_handle<Promise> self) noexcept {
    const std::coroutine_handle<> continuation = self.promise().continuation_;
    return continuation ? continuation : std::noop_coroutine();
}

template <class Promise>
void FinalAwaiter<Promise>::await_resume() noexcept {
    assert(false && "FinalAwaiter::await_resume 不应被调用");
}
```

`context.cpp` 里的部分:

```cpp
namespace zenio::detail {

namespace {

thread_local PromiseContext* t_current_context = nullptr;

}  // namespace

PromiseContext* CurrentContext() noexcept {
    return t_current_context;
}

void SetCurrentContext(PromiseContext* context) noexcept {
    t_current_context = context;
}

void YieldAwaiter::await_suspend(std::coroutine_handle<> caller) noexcept {
    const PromiseContext* context = CurrentContext();
    assert(context != nullptr && context->core != nullptr && "co_await Yield() 必须在运行时里");
    context->core->Schedule(MakeRunnable(caller));
}

}  // namespace zenio::detail
```

`RuntimeCore` 和 `MakeRunnable` 是内部类型, 定义在 `zenio/detail/runtime_core.h`、`zenio/detail/scheduler.h`, `context.cpp` 包含它们。

### 2.3 awaiter 约定

所有返回 awaiter 的操作都按下面的规则写 (S8)。

1. **命名和位置**: 返回值的类型是 `zenio::detail` 里的一个类, 名字是操作名加 `Awaiter`, 如 `AcceptAwaiter`。除了 `Task` 自己用的 `InitialAwaiter`、`FinalAwaiter`、`YieldAwaiter` (它们在 `zenio/detail/context.h`), 全部声明在 `zenio/detail/awaiters.h`。`awaiters.h` 被很多公开头文件包含, 所以它不包含任何公开头文件, 只前置声明用到的东西 (M1 偏差): 类 `JoinHandle<T>`、`JoinSet<T>`、`net::TcpListener`、`net::TcpStream`、`net::UdpSocket`、`signal::Signal`, 以及六个创建方的函数声明 `SpawnAndSeize`、`Uncancellable`、`Select` (带完整约束, `SelectOutputType` 因此也定义在 `awaiters.h`)、`Sleep`、`SleepUntil`——限定名的友元必须有先前声明, 这些声明与各自头文件里的逐字一致。`SelectResult` 不在其中: 别名模板没有前置声明, `SelectAwaiter` 的 `await_resume()` 直接写开 `std::variant`, 不引用别名。
2. **三个成员函数**:

   ```cpp
   [[nodiscard]] bool await_ready() noexcept;
   bool              await_suspend(std::coroutine_handle<> caller) noexcept;
   T                 await_resume();
   ```

   - `await_ready()`: 操作已经完成、不用等的时候返回 true, 协程不挂起。例如数据已经在内核缓冲区里, read 直接成功。
   - `await_suspend()`: 返回 false 表示不用挂起, 协程原地恢复。等待期间完成、或者已取消, 都可能走到这里。
   - `await_resume()`: 返回产出, 或者抛异常 (`SystemError`、`CancelledError`)。它不标 `noexcept`。

3. **两个例外**:
   - `Task<T>::Awaiter` 和 `SpawnAndSeizeAwaiter` 的 `await_suspend` 返回 `std::coroutine_handle<>`。这是交接: 被返回的那个协程在当前线程上接着运行。
   - `YieldAwaiter` 的 `await_suspend` 返回 void。调用方一定挂起, 被投递到队列。
4. **生命周期**: awaiter 是按值返回的临时对象, 活到包含它的 `co_await` 表达式结束。它的数据成员里不能有悬垂引用。缓冲区这类指针参数指向调用方的对象, 由调用方保证它在 `co_await` 期间存活。
5. **构造函数是私有的**, 只有创建它的操作能构造。
6. **awaiter 不创建取消令牌**, 只用 `CurrentContext()->token`。
7. **上下文只在 `await_ready` 和 `await_suspend` 里读**, 读到的令牌、运行时记在自己的成员里。`await_resume` 只看自己记下的状态: 被调度器恢复时, 当前上下文是挂起时的那一份, 在 `Uncancellable` 里那是组合子的一份副本, 不是调用方自己的 (见 2.2 第 2 条)。

全部 awaiter 类型:

| 类型 | 产出 | 由谁返回 | `await_suspend` 返回 |
|------|------|----------|----------------------|
| `AcceptAwaiter` | `std::pair<net::TcpStream, net::SocketAddr>` | `TcpListener::Accept()` | `bool` |
| `ConnectAwaiter` | `net::TcpStream` | `TcpStream::Connect()` | `bool` |
| `ReadSomeAwaiter` | `size_t` | `TcpStream::ReadSome()` | `bool` |
| `ReadExactAwaiter` | `size_t` | `TcpStream::ReadExact()` | `bool` |
| `WriteSomeAwaiter` | `size_t` | `TcpStream::WriteSome()` | `bool` |
| `WriteAllAwaiter` | `void` | `TcpStream::WriteAll()` | `bool` |
| `SendToAwaiter` | `size_t` | `UdpSocket::SendTo()` | `bool` |
| `RecvFromAwaiter` | `std::pair<size_t, net::SocketAddr>` | `UdpSocket::RecvFrom()` | `bool` |
| `SleepAwaiter` | `void` | `Sleep()`、`SleepUntil()` | `bool` |
| `SignalRecvAwaiter` | `void` | `Signal::Recv()` | `bool` |
| `JoinHandleAwaiter<T>` | `T` | `JoinHandle<T>::operator co_await()` | `bool` |
| `SpawnAndSeizeAwaiter<T, R>` | `R` | `SpawnAndSeize()`、`JoinSet<T>::SpawnAndSeize()` | `std::coroutine_handle<>` |
| `JoinAllAwaiter<T>` | `JoinSet<T>::JoinAllOut` | `JoinSet<T>::JoinAll()` | `bool` |
| `JoinNextAwaiter<T>` | `JoinSet<T>::JoinNextOut` | `JoinSet<T>::JoinNext()` | `bool` |
| `SelectAwaiter<A...>` | `SelectResult<Ts...>` | `Select()` | `bool` |
| `UncancellableAwaiter<A>` | 与 `A` 相同 | `Uncancellable()` | 与 `A` 相同 |
| `YieldAwaiter` | `void` | `Yield()` | `void` |
| `Task<T>::Awaiter` | `T` | `Task<T>::operator co_await()` | `std::coroutine_handle<>` |

除 `Task<T>::Awaiter`、`YieldAwaiter`、`InitialAwaiter`、`FinalAwaiter` 之外, 这些类在 M1 只有 2 里列出的成员函数和一个私有构造函数; 数据成员在实现它们的里程碑里按各自的文档加入。

每个 awaiter 类的私有构造函数需要友元: 让创建它的那个类或函数成为友元, 例如 `AcceptAwaiter` 的友元是 `net::TcpListener`。

"一个 awaitable 的产出类型"由下面这个 trait 给出, 定义在 `zenio/detail/awaiters.h`。`SelectAwaiter` 和 `UncancellableAwaiter` 靠它声明自己的 `await_resume()`:

```cpp
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

}  // namespace zenio::detail
```

不能写成 `std::conditional_t<有 operator co_await, ..., ...>`: 它的两个分支都会被实例化, 没有 `operator co_await` 的类型会直接报错。

### 2.4 `Task` 的成员函数定义

`Task<T>` 是模板, 成员函数都是模板, 定义写在 `task.h` 里、三个类的后面 (M3)。先看主模板:

```cpp
namespace detail {

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

}  // namespace detail

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
```

两个特化按下面的规则从主模板得到:

- `Task<T&>`: 逐字照抄。`return_value` 的函数体是 `storage_ = Result<T*, std::exception_ptr>::Ok(&value);`, `Awaiter::await_resume()` 返回 `T&`, 函数体是 `detail::SetCurrentContext(saved_); return detail::TakeTaskResult<T&>(handle_.promise().storage_);`。
- `Task<void>`: 去掉 `template <class T>`, 每个定义前加 `inline`, `Task<T>` 写成 `Task<void>`。`initial_suspend()`、`final_suspend()` 的返回类型直接写 `detail::InitialAwaiter<promise_type>`、`detail::FinalAwaiter<promise_type>`。`return_value` 换成:

  ```cpp
  inline void Task<void>::promise_type::return_void() {
      storage_ = Result<void, std::exception_ptr>::Ok();
  }
  ```

  `unhandled_exception()` 里的 `Result<T, ...>` 写成 `Result<void, ...>`。`Awaiter::await_resume()` 返回 `void`, 函数体是:

  ```cpp
  inline void Task<void>::Awaiter::await_resume() {
      detail::SetCurrentContext(saved_);
      detail::TakeTaskResult<void>(handle_.promise().storage_);
  }
  ```

`Yield()` 的定义在 `task.cpp` 里, 这个文件只有它:

```cpp
#include "zenio/task.h"

namespace zenio {

[[nodiscard]] detail::YieldAwaiter Yield() {
    return {};
}

}  // namespace zenio
```

## 3. 内部数据结构

| 类型 | 成员 | 说明 |
|------|------|------|
| `Task<T>::promise_type` | `storage_` | `Result<T, std::exception_ptr>`: 空态是还没结束, Ok 是产出, Err 是协程体里的异常 |
| | `continuation_` | 等待方的句柄。`Awaiter::await_suspend` 里设置, 结束之后不清 |
| | `seize_caller_` | `SpawnAndSeize` 的调用方, M5 用 |
| | `token`、`core`、`started` | 来自 `PromiseContext`, 见 2.2 |
| `Task<T>::Awaiter` | `handle_` | child 的句柄, 是 `Task` 的句柄的副本 |
| | `saved_` | 调用方挂起时的上下文, `await_resume` 里还原 |
| `InitialAwaiter<Promise>` | `self_` | 启动的协程, `await_suspend` 里记下 |
| `FinalAwaiter<Promise>` | 无 | 只读 promise 的 `continuation_` |
| `detail::TaskAccess` | 无 | `Task` 的友元, 只有 `Runtime::BlockOn` 用它取句柄 |
| `detail::TakeTaskResult<T, Stored>` | | 从 `storage_` 取产出或重抛, 见 2.4 |

帧的所有权 (A17): 一个协程帧由一个 `Task` 对象独占, 移动 `Task` 就是转移帧。帧在 `~Task()` 里销毁, 别的地方都不销毁它。`Awaiter::await_resume()` 只把产出从 `storage_` 里取走。

## 4. 关键流程

### 4.1 启动

`co_await child`:

1. 调用方得到 `child.operator co_await()` 返回的 `Awaiter`。`await_ready()` 返回 false, 调用方挂起。
2. `Awaiter::await_suspend(调用方的句柄)`: 断言 child 还没有等待方; 把调用方记进 `continuation_`, 把当前上下文记进 `saved_`; 返回 child 的句柄。
3. 协程机制接着恢复 child, 这是一次尾调用, 调用方的栈帧被复用 (见第 6 节)。child 的 `InitialAwaiter::await_resume` 先运行: 复制上下文, 把当前上下文切换成 child 自己的。
4. child 的函数体开始执行。

### 4.2 正常结束

1. child 执行 `co_return v`: `return_value(v)` 把 `storage_` 设成 `Ok(v)`。然后函数体里的局部变量析构。
2. `final_suspend()`: `FinalAwaiter::await_suspend(child 的句柄)` 返回 `continuation_`, 协程机制恢复调用方, 也是尾调用。child 的帧还在, 停在最后的挂起点。
3. 调用方在自己的挂起点继续: `Awaiter::await_resume()` 还原当前上下文, 再从 child 的 `storage_` 里取走产出。
4. `co_await` 表达式结束: 持有 child 的 `Task` 对象析构, child 的帧被销毁。child 是临时对象时, 这发生在表达式末尾; 是调用方的局部变量时, 发生在变量离开作用域的时候。

### 4.3 异常结束

1. child 的函数体里没被接住的异常传到协程边界, 协程机制调用 `unhandled_exception()`, 它把 `storage_` 设成 `Err(std::current_exception())`。
2. 之后同 4.2 的第 2、3 步。调用方在 `await_resume` 里 `std::rethrow_exception`, 异常的类型和内容不变。
3. 异常沿着 `co_await` 链一层层向上: 每一层都在自己恢复运行之后才重抛, 所以栈的深度不随层数增长 (见第 6 节)。中间某一层接住了, 链就在那里断开, 那一层按正常结束处理。

### 4.4 让出

`co_await Yield()`: `YieldAwaiter::await_suspend(调用方的句柄)` 把"句柄加当前上下文"交给 `RuntimeCore::Schedule`, 协程挂起, 控制回到调度器的循环。之后调度器按顺序恢复它, 见 design/scheduler.md 4.3。

### 4.5 根任务

没有上一级的 Task 由创建它的一方启动, 不经过 `Task::Awaiter`:

1. `detail::TaskAccess::Handle(task)` 取句柄。
2. 把句柄交给运行时, 运行时装好上下文并投递, 见 design/runtime.md 4.2。
3. 运行时返回时协程已经结束, `detail::TakeTaskResult<T>(handle.promise().storage_)` 取产出或者重抛。

## 5. 不变量

- `Task` 是惰性的: 构造之后不运行, 被 `co_await` 才运行。
- 一个 `Task` 只被 `co_await` 一次。第二次是未定义行为, debug 构建下断言。
- 已启动但未结束的 `Task` 不被销毁 (A5)。销毁它是未定义行为, debug 构建下断言。
- 一个协程帧只被销毁一次, 在 `~Task()` 里。
- 子协程的令牌在启动时确定, 运行期间不变。
- 一个协程的函数体执行期间, `CurrentContext()` 指向它自己的 promise; 经 `Uncancellable` 启动的内层协程指向那份副本。
- 一个协程恢复运行时看到的上下文, 就是它挂起时的那一份。
- `storage_` 不是空态当且仅当协程已经结束。

## 6. 已知的坑

- **对称交接靠编译器做尾调用**。`await_suspend` 返回句柄时, 协程机制对返回的句柄调用 `resume()`, 它必须是一个不占栈的尾调用, 否则嵌套的 `co_await` 会一层层把栈压下去。Clang 14 起把它编译成 `musttail` 调用, 与优化级别无关。设计会话在 Apple Clang 21 上实测 (arm64):

  | | 每层占用的栈 | 10 万层 |
  |---|---|---|
  | 有交接, `-O0` 和 `-O2` | 0 | 不增长, 100 万层也一样 |
  | 没有交接, `-O0` | 约 280 字节 | 约 28 MB, 溢出 |
  | 没有交接, `-O2` | 约 31 字节 | 约 3.1 MB, 在 8 MB 的主线程栈上不溢出 |

  两点要注意。第一, GCC 上的行为没有在开发机上验证 (D14)。第二, 验收测试要在一个栈很小的线程上跑, 否则 Release 构建下交接写坏了也测不出来。
- **交接的两个方向都经过尾调用**: 向下是 `Task<T>::Awaiter::await_suspend` 返回 child 的句柄, 向上是 `FinalAwaiter::await_suspend` 返回 `continuation_`。两个函数的返回类型必须是 `std::coroutine_handle<>`, 中间不能插入别的调用再去恢复协程。
- **恢复一个还没挂起的协程是未定义行为**。交接的时候, 把调用方重新投递必须发生在它真正挂起之后。`InitialAwaiter::await_resume` 满足这个时序, 因为它是 child 启动后的第一件事, 那时调用方已经挂起。
- **`final_suspend()` 和 `unhandled_exception()` 必须标 `noexcept`**, 这是协程 promise 的硬性要求。
- **`final_suspend()` 必须挂起**: child 的帧要到 `~Task()` 才销毁, 协程不能自己走完最后一格把帧释放掉。
- **交接发生在 `FinalAwaiter::await_suspend` 里, 不是 `await_resume`**: `await_suspend` 返回等待方的句柄, 协程机制接着恢复它。`await_resume` 在 final suspend 之后不会被调用。
- **child 的帧在 `co_await` 表达式结束时就没了**: child 是临时对象时, `co_await` 表达式一结束它的帧就被销毁。不要把指向 child 帧里任何东西的指针 (比如 child 里 `CurrentContext()` 的返回值、child 局部变量的地址) 留到 `co_await` 之后用。M3 细化时原型里的测试代码就这么错过一次, ASAN 报 use-after-free。
- **不要在 `await_resume` 里销毁 child 的帧**: `Awaiter` 只持有句柄的副本, 销毁了帧之后 `Task` 对象里的句柄就悬垂了, `~Task()` 会再销毁一次。帧只由 `~Task()` 销毁 (A17)。
- **`storage_` 是三态的**: 空态表示还没结束。判断 child 是否结束不能只看 `is_ok()`。
- **`Task::Awaiter::await_ready()`**: child 还没启动时必须返回 false, 让它启动。
- **不是协程的函数**: 函数体里一个 `co_await`、`co_yield`、`co_return` 都没有的函数不是协程, 哪怕返回类型写的是 `Task<void>`。它返回的是一个没初始化的 `Task`, 用了就是未定义行为; 编译器只会给一句 `-Wreturn-type` 警告 ("non-void function does not return a value")。所以 `Task<void>` 的函数体末尾也要写 `co_return;`。
- **带捕获的 lambda 不能写成协程**: 协程帧里只存了闭包对象的指针, 不会把捕获的内容拷进帧里。临时的闭包在函数返回 `Task` 的时候就销毁了, 等协程体真正运行时, 通过指针访问的捕获已经失效。`-O0` 下那块内存往往还没被覆盖, 看起来能用; `-O2` 下读到的是垃圾。要带状态就写成普通的协程函数, 状态通过参数传进去 (按值传进去的参数会拷进帧里)。
- **`Task<void>` 的成员函数不是模板**: 类外的定义必须加 `inline`, 否则多个翻译单元包含 `task.h` 时重复定义。
- **协程参数**: 按值传入的类型 (如 `TcpStream`) 是只可移动的, 会被移动进协程帧。
- **不能在普通函数里 `co_await`**: 需要等待的函数必须本身是协程。
- **`co_await` 一个无效的 Task** (默认移动之后的那种): debug 构建下断言。

## 7. 验收测试

M3 交付, 文件在 `tests/zenio/`。具体步骤见 design/milestones/M3.md。

`task_unittest.cpp`:

- 产出类型: `Task<int>`、`Task<bool>`、`Task<void>`、`Task<std::string>`、`Task<std::unique_ptr<int>>`、`Task<int&>` 都能编译, 产出正确。`Task<int&>` 产出的引用指向原来的对象。
- 只可移动的类型按值传进协程, 在函数体里能用。
- 10 万层嵌套的 `co_await`, 每层带一个计数的 RAII 对象: 结果正确, 同时活着的对象数等于层数, 结束之后计数归零 (每层的析构函数恰好执行一次)。测试在一个 512 KB 栈的线程上跑, Debug 和 Release 构建下都要通过。
- 10 万层深处的异常一路重抛到 `BlockOn`, 类型和 `what()` 不变; 沿途每层的析构函数都执行了。
- 中间某一层接住下层异常之后正常返回。
- `Task` 惰性: 创建之后协程体没运行; 没被 `co_await` 就销毁的 `Task`, 协程体始终没运行, ASAN 下没有泄漏。
- 移动: 移动之后源对象的 `is_valid()` 是 false, 目标对象能正常运行; 移动赋值时目标原来持有的协程被销毁。
- `Yield()`: 在别的任务先运行完之后才继续, 见 design/scheduler.md 第 7 节。
- 反向检查 (debug 构建下中止进程): 同一个 `Task` 被 `co_await` 两次; 协程运行期间销毁它的 `Task`。
- 反向检查 (编译期): 函数体里没有任何 `co_` 关键字的 `Task<void>` 函数有 `-Wreturn-type` 警告。
