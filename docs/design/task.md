# Task 与上下文

状态: 草稿
细化程度: 对外接口和 awaiter 约定已细化 (M1); 内部是概要 (M3 前细化)
依据: S5、S8、S17; A4、A5

## 1. 职责与边界

- `Task<T>` 是 zenio 的协程返回类型: 惰性启动, 被 `co_await` 时才开始执行, 结束时产出 `T` 或把协程体内的异常原样重抛。tokio 没有同名的类型, 因为 Rust 的 async 函数直接就是 future。
- `Yield()` 对应 `tokio::task::yield_now`。
- 本文件还规定两件所有子系统都要用的事: 协程上下文怎么传播, awaiter 怎么写。

不在本文件: Task 怎么被派发 (design/spawn.md), 怎么成组管理 (design/join_set.md)。

## 2. 对外接口

### 2.1 `zenio/task.h`

```cpp
namespace zenio::detail {
template <class Promise> class InitialAwaiter;
template <class Promise> class FinalAwaiter;
class YieldAwaiter;
}

namespace zenio {

/**
 * @brief 惰性启动的协程任务
 * @note 被 co_await 时才开始执行; 结束时产出 T, 或把协程体内的异常原样重抛
 */
template <class T>
class [[nodiscard]] Task {
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

//! @brief 让出执行权 (对应 tokio::task::yield_now)
[[nodiscard]] detail::YieldAwaiter Yield();

}  // namespace zenio
```

`promise_type` 是 Task 的核心, 整个定义在头文件里:

```cpp
template <class T>
struct Task<T>::promise_type : detail::PromiseContext {
    using value_type = T;

    [[nodiscard]] Task get_return_object();

    //! @brief 总是挂起。它的 await_resume 是协程启动后、函数体之前执行的第一件事
    detail::InitialAwaiter<promise_type> initial_suspend() noexcept;
    //! @brief 把控制权直接交给等待方 (symmetric transfer)
    detail::FinalAwaiter<promise_type> final_suspend() noexcept;

    //! @brief zenio 的 awaiter 自己从线程局部存储取上下文, 这里原样透传
    template <class A>
    A&& await_transform(A&& a) noexcept {
        return std::forward<A>(a);
    }

    void return_value(T value);
    void unhandled_exception() noexcept;

    Result<T, std::exception_ptr> storage_;       //!< 产出, 或协程体内的异常; 空态表示还没结束
    std::coroutine_handle<>       continuation_;  //!< 等待方, 没有则为空
    std::coroutine_handle<>       seize_caller_;  //!< SpawnAndSeize 的调用方, 见 design/spawn.md
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
    friend class Task;

    explicit Awaiter(std::coroutine_handle<promise_type> handle) noexcept;

    std::coroutine_handle<promise_type> handle_;
};
```

另外两个特化, 只有下表列出的地方不同:

| | `Task<void>` | `Task<T&>` |
|---|---|---|
| 写法 | 显式特化 `template <> class Task<void>` | 偏特化 `template <class T> class Task<T&>` |
| 协程返回 | `void return_void();` | `void return_value(T& value);` |
| `storage_` 的类型 | `Result<void, std::exception_ptr>` | `Result<T*, std::exception_ptr>` |
| `Awaiter::await_resume()` 返回 | `void` | `T&` |

`Yield()` 返回的 awaiter:

```cpp
namespace zenio::detail {

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

}  // namespace zenio::detail
```

### 2.2 协程上下文

下面这些定义在 `zenio/detail/context.h`。`InitialAwaiter`、`FinalAwaiter`、`YieldAwaiter` 三个类也定义在这个头文件里 (声明见 2.1), 这样 `task.h` 不需要依赖 `awaiters.h`:

```cpp
namespace zenio::detail {

class RuntimeCore;

/**
 * @brief 每个 zenio 协程 promise 的公共部分
 */
struct PromiseContext {
    std::stop_token token;          //!< 取消令牌, 启动时从上一级复制下来
    RuntimeCore*    core = nullptr; //!< 所属运行时的内部对象: 调度器和 Poller
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

}  // namespace zenio::detail
```

规则:

- 上下文存在 promise 里。协程第一次被恢复时, 从线程局部存储里读出当时的上下文, 复制进自己的 promise。所以子协程的令牌和运行时默认与启动它的那一方相同。
- 调度器每次恢复一个协程, 都先把该协程的上下文登记到线程局部存储, 恢复返回后还原。`CurrentContext()` 读的就是这个位置。
- awaiter 在挂起时自己调用 `CurrentContext()`, 不靠 promise 注入。所以 `await_transform` 只是把 awaiter 原样透传。
- 上下文和当前不一样的交接 (`SpawnAndSeize`、`Uncancellable`、`JoinSet` 派发的子任务) 由交接的一方负责切换线程局部存储, 见各自的文档。
- 三种获得运行时的方式: 协程内隐式; `Handle::Enter()` 作用域; 显式传 `Handle`。

### 2.3 awaiter 约定

所有返回 awaiter 的操作都按下面的规则写 (S8)。

1. **命名和位置**: 返回值的类型是 `zenio::detail` 里的一个类, 名字是操作名加 `Awaiter`, 如 `AcceptAwaiter`。除了 `Task` 自己用的 `InitialAwaiter`、`FinalAwaiter`、`YieldAwaiter` (它们在 `zenio/detail/context.h`), 全部声明在 `zenio/detail/awaiters.h`。`awaiters.h` 被很多公开头文件包含, 所以它不包含任何公开头文件, 只前置声明用到的东西: `JoinSet<T>`、`SelectResult<Ts...>`、`net::TcpListener`、`net::TcpStream`、`net::UdpSocket`、`signal::Signal`。
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

M1 阶段这些类只有 2 里列出的成员函数和一个私有构造函数; 数据成员在实现它们的里程碑里按各自的文档加入。

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

## 3. 内部数据结构 (概要)

| 类型 | 职责 |
|------|------|
| `Task<T>::promise_type` | 见 2.1 |
| `InitialAwaiter<Promise>` | `initial_suspend()` 的返回值。它的 `await_resume` 在协程启动后、函数体之前运行, 做两件事: 把线程局部存储里的上下文复制进 promise; 如果 `seize_caller_` 非空, 把它重新投递 (design/spawn.md) |
| `FinalAwaiter<Promise>` | `final_suspend()` 的返回值。`await_suspend` 返回 `continuation_`, 为空时返回 `std::noop_coroutine()` |

## 4. 关键流程 (概要, M3 前细化)

1. **启动**: `co_await task`。`Awaiter::await_suspend(caller)` 把 caller 记进 child 的 `continuation_`, 返回 child 的句柄。调用方挂起, child 在同一线程上第一次运行, 进入 `InitialAwaiter::await_resume`, 然后执行函数体。
2. **正常结束**: 函数体 `co_return` 之后, `return_value` 把产出存成 `Ok`, 进入 `FinalAwaiter`, 控制权交给 `continuation_`。调用方在 `await_resume` 里销毁 child 的帧, 取出产出返回。
3. **异常结束**: 协程体里没被接住的异常进入 `unhandled_exception()`, 存成 `Err`。后面的流程同上, 调用方在 `await_resume` 里重抛。
4. **让出**: `co_await Yield()`。调用方被投递到当前 worker 的本地队列, 线程接着运行别的任务。

## 5. 不变量

- `Task` 是惰性的: 构造之后不运行, 被 `co_await` 才运行。
- 一个 `Task` 只被 `co_await` 一次。
- 已启动但未结束的 `Task` 不被销毁 (A5)。
- 子协程的令牌在启动时确定, 运行期间不变。
- 协程体正在运行的那个线程上, 线程局部存储里的上下文就是它的上下文。

## 6. 已知的坑

- **恢复一个还没挂起的协程是未定义行为**。交接的时候, 把调用方重新投递必须发生在它真正挂起之后。`InitialAwaiter::await_resume` 满足这个时序, 因为它是 child 启动后的第一件事, 那时调用方已经挂起。
- **`final_suspend()` 和 `unhandled_exception()` 必须标 `noexcept`**, 这是协程 promise 的硬性要求。
- **`final_suspend()` 必须挂起**: child 的帧由等待方在 `await_resume` 里销毁。
- **交接发生在 `FinalAwaiter::await_suspend` 里, 不是 `await_resume`**: `await_suspend` 返回等待方的句柄, 协程机制接着恢复它。`await_resume` 在 final suspend 之后不会被调用。
- **`storage_` 是三态的**: 空态表示还没结束。判断 child 是否结束不能只看 `is_ok()`。
- **`Task::Awaiter::await_ready()`**: child 还没启动时必须返回 false, 让它启动。
- **协程参数**: 按值传入的类型 (如 `TcpStream`) 是只可移动的, 会被移动进协程帧。
- **不能在普通函数里 `co_await`**: 需要等待的函数必须本身是协程。

## 7. 验收测试

M3 的任务书里给出。概要:

- 10 万层嵌套的 `co_await` 不爆栈。
- 协程体内抛出的异常在 `co_await` 处原样重抛, 类型不变。
- `co_await Yield()` 之后别的任务先运行。
- 在 `Task<bool>`、`Task<void>`、`Task<std::unique_ptr<int>>`、`Task<int&>` 上都能编译并正确产出。
