# Select、定时器函数与 Timeout

状态: 草稿
细化程度: 对外接口已细化 (M1); 内部是概要 (M5 前细化)
依据: S5、S6、S25、S26、S27; A2、A7、A15; D9

## 1. 职责与边界

- **`Select`**: 并发等待多个操作, 第一个完成的获胜, 其余被取消。对应 `tokio::select!`。它是补充的 API 之一, 形态来自目标用法草图。
- **`Sleep`、`SleepUntil`、`Timeout`**: 对应 `tokio::time` 的同名函数。

不在 v1: `Interval`、可暂停的时钟、`join!` / `try_join!` 那种并发等待一组任务。

## 2. 对外接口

### 2.1 `zenio/select.h`

```cpp
namespace zenio::detail {

template <class... Awaitables> class SelectAwaiter;

/**
 * @brief 分支的产出类型要能放进 std::variant
 * @note 产出是引用的分支不支持: std::variant 里放不了引用
 * @note 定义在 zenio/detail/awaiters.h, 不在本文件: SelectAwaiter 的友元声明需要完整约束 (M1 偏差)
 */
template <class T>
concept SelectOutputType = !std::is_reference_v<T>;

}

namespace zenio {

/**
 * @brief Select 的产出: 第一个完成的分支的结果
 * @note 就是 std::variant。产出 void 的分支用 std::monostate 占位, 与 Result<void> 的做法相同
 */
template <class... Ts>
using SelectResult = std::variant<detail::VoidToMonostate<Ts>...>;

/**
 * @brief 并发等待多个操作, 第一个完成的获胜, 其余被取消
 * @note 至少 1 个分支, 至多 64 个, 与 tokio 的 select! 一致。分支的产出不能是引用
 * @throws zenio::CancelledError 调用方在等待期间被取消
 * @throws ... 获胜分支的异常, 原样重抛
 */
template <class... Awaitables>
    requires(sizeof...(Awaitables) >= 1 && sizeof...(Awaitables) <= 64 &&
             (detail::SelectOutputType<detail::await_result_t<std::decay_t<Awaitables>>> && ...))
[[nodiscard]] detail::SelectAwaiter<std::decay_t<Awaitables>...> Select(Awaitables&&... awaitables);

}  // namespace zenio
```

`Select` 的模板参数是各分支的 awaiter 类型, `SelectResult<Ts...>` 的模板参数是各分支的产出类型, 中间靠 design/task.md 2.3 的 `await_result_t` 对应。例如:

```cpp
auto result = co_await zenio::Select(sig.Recv(), listener.Accept());
// result 的类型是 SelectResult<void, std::pair<net::TcpStream, net::SocketAddr>>
// 也就是 std::variant<std::monostate, std::pair<net::TcpStream, net::SocketAddr>>
if (result.index() == 0) { /* 信号先到 */ }
auto conn = std::get<1>(std::move(result));   // std::pair<net::TcpStream, net::SocketAddr>&&
```

取结果一律用下标: `result.index()` 和 `std::get<I>`。两个分支产出同一种类型时, variant 里有重复的备选类型, `std::variant` 允许, 但按类型取 (`std::get<T>`) 就有歧义了。

### 2.2 `zenio/time.h`

```cpp
namespace zenio {

/**
 * @brief 等待一段时间 (对应 tokio::time::sleep)
 * @throws zenio::CancelledError 调用方在等待期间被取消
 */
[[nodiscard]] detail::SleepAwaiter Sleep(std::chrono::nanoseconds duration);

/**
 * @brief 等到一个时刻 (对应 tokio::time::sleep_until)。时钟用 std::chrono::steady_clock
 * @throws zenio::CancelledError 调用方在等待期间被取消
 */
[[nodiscard]] detail::SleepAwaiter SleepUntil(std::chrono::steady_clock::time_point deadline);

/**
 * @brief 给任务加超时 (对应 tokio::time::timeout)
 * @return 任务的产出
 * @throws zenio::ElapsedError 超过 duration。此时 task 被取消
 * @throws ... 任务自身的异常, 原样重抛
 */
template <class T>
[[nodiscard]] Task<T> Timeout(Task<T> task, std::chrono::nanoseconds duration);

}  // namespace zenio
```

参数类型是 `std::chrono::nanoseconds`, 所以任何更粗的时长 (`100ms`、`1s`) 都能直接传。

## 3. 内部数据结构 (概要)

M1 就需要的两样东西:

- `detail::VoidToMonostate<T>`: 定义在 `zenio/result.h`, 见 design/error.md 2.2。`Result<void>` 和 `Select` 的 void 分支共用它。
- `SelectAwaiter<A...>::await_resume()` 的返回类型写成 `std::variant<detail::VoidToMonostate<detail::await_result_t<A>>...>`, 不引用别名 `SelectResult`: `awaiters.h` 不能包含 `select.h`。

其余是概要:

- `SelectAwaiter<A...>`: 每个分支包成一个子协程, 把 awaiter 的产出转存进结果。所有分支共用一个 `stop_source`, 它链接到调用方的令牌。分支的产出类型用 design/task.md 2.3 的 `await_result_t` 取。
- 结果: 一个 `std::optional<SelectResult<各分支的产出类型...>>`。variant 的第一个备选类型不一定能默认构造 (比如 `std::pair<TcpStream, SocketAddr>`), 所以不能直接持有 variant; 等获胜分支出来, 再用 `emplace<I>` 就地构造。
- 一个位图记录各分支的状态, 至少 64 位。这是分支上限为 64 的原因, 和 tokio 的做法相同: 它按分支数在 u8、u16、u32、u64 里选一个。位图用原子操作更新, 保证只有一个分支获胜。
- `SleepAwaiter`: 一个 `Waiter`、一个内嵌的 `TimerEntry` (design/timer.md 2.3)、到期时刻。

## 4. 关键流程 (概要)

### 4.1 Select

1. 启动所有分支。
2. 第一个结束的分支把自己记成获胜者。记成功的那个分支把结果写进 `SelectResult`。
3. 取消其余分支, 等它们全部结束。输家的 `CancelledError` 被吞掉。
4. 恢复调用方, 交出 `SelectResult`。

例外:

- 获胜分支抛了异常: 取消其余分支并等它们结束, 然后原样重抛。
- 调用方在等待期间被取消: 所有分支都被取消, 等它们结束后抛 `CancelledError`。

### 4.2 Sleep

向定时器驱动登记, 同时把 `Waiter` 挂上去, 然后 `Waiter::TrySuspend`。到期后定时器驱动在解锁之后唤醒它。不经过 Poller, 因为定时器是独立的子系统 (S26)。完整步骤见 design/timer.md 4.7。

### 4.3 Timeout

`Select` 加 `Sleep` 组合而成: 并发等待 task 和 `Sleep(duration)`。Sleep 先结束就抛 `ElapsedError`, task 先结束就取它的产出或重抛它的异常。

## 5. 不变量

- 恰好有一个分支获胜。
- `Select` 返回时, 所有分支都已结束。
- variant 里只设置了获胜分支对应的那个备选类型, 所以 `index()` 就是获胜分支的下标。

## 6. 已知的坑

- **每次调用每个分支一次帧分配** (D9): 分支被包成子协程。在循环里反复 `Select` (如 accept 循环) 时成本明显。
- **获胜分支的结果要活到被取走**: 所以结果转存进 `SelectResult`, 而不是留在分支的帧里。
- **输家的取消必须等到它们结束**: 输家的帧里有指向 `SelectResult` 的指针, 不能在它们还运行时返回。
- **`Timeout` 每次调用多一层协程帧**, 与 `Try` 同类 (D13)。
- **重复的备选类型**: 两个分支产出同一种类型 (包括都是 void) 时, variant 里有重复的备选类型。`std::variant` 允许, 但只能按下标取。
- **variant 不能变空**: 某个备选类型的移动构造抛异常时, variant 可能进入 `valueless_by_exception`。约定放进 `Select` 结果的类型移动构造不抛, 与 `Result` 相同。
- **产出是引用的分支不支持**: `Task<T&>` 的产出是 `T&`, variant 里放不了引用。`Select` 的约束会拦住它, 报错指向 `SelectOutputType`。以后要支持的话, 映射成指针即可。

## 7. 验收测试

M4 的任务书里给出 `Sleep`, M5 的任务书里给出其余部分。`Sleep` 本身的测试在 design/timer.md 第 7 节。概要:

- 两个分支, 先完成的那个获胜, 另一个收到 `CancelledError`。
- 获胜分支抛异常时, `Select` 重抛它, 其余分支被取消。
- 产出 `void` 的分支能编译, `std::get<I>()` 得到 `std::monostate`。
- 两个分支产出同一种类型时, 按下标能取到正确的结果。
- 0 个分支、65 个分支、产出是引用的分支, 这三种都不能编译。
- `Timeout`: 任务按时完成则返回产出; 超时则抛 `ElapsedError`, 任务收到 `CancelledError`。
