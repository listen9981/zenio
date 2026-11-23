# 任务派发: Spawn 与 SpawnAndSeize

状态: 草稿
细化程度: 对外接口已细化 (M1); 内部是概要 (M5 前细化)
依据: S12、S13、S14、S15; A8

## 1. 职责与边界

本文件规定游离任务怎么派发出去, 以及管理它们的两个句柄。

- **`Spawn`** 对应 `tokio::spawn`: 派发一个任务, 返回 `JoinHandle<T>`。
- **`SpawnAndSeize`** 是补充的 API, 语义来自 bthread 的 `bthread_start_urgent`。
- **`JoinHandle<T>`** 对应 `tokio::task::JoinHandle`: 可以等结果, 可以取消。
- **`AbortHandle`** 对应 `tokio::task::AbortHandle`: 只能取消。

派发出去的任务叫游离任务, 由 Runtime 兜底: Runtime 析构时 Abort 它们并等它们结束。

不在本文件: `JoinSet` 里的任务怎么管理 (design/join_set.md), 三个 `Spawn` 入口里 `Handle::Spawn` 和 `Runtime::Spawn` 的声明 (design/runtime.md)。

## 2. 对外接口

`zenio/spawn.h`:

```cpp
namespace zenio::detail {
template <class T>          class JoinHandleAwaiter;
template <class T, class R> class SpawnAndSeizeAwaiter;
}

namespace zenio {

/**
 * @brief 游离任务的句柄 (对应 tokio::task::JoinHandle)
 * @note 句柄析构不取消任务
 */
template <class T>
class JoinHandle {
public:
    JoinHandle() noexcept = default;
    JoinHandle(JoinHandle&& other) noexcept;
    JoinHandle& operator=(JoinHandle&& other) noexcept;
    JoinHandle(const JoinHandle&)            = delete;
    JoinHandle& operator=(const JoinHandle&) = delete;

    ~JoinHandle();

    [[nodiscard]] bool is_valid() const noexcept;

    //! @brief 任务是否已经结束
    [[nodiscard]] bool is_finished() const;

    //! @brief 取消这一个任务
    void Abort();

    //! @brief 等任务结束: 产出 T, 或重抛任务的异常
    detail::JoinHandleAwaiter<T> operator co_await() const;

private:
    std::shared_ptr<detail::TaskRecord> record_;
};

/**
 * @brief 只能用来取消的句柄 (对应 tokio::task::AbortHandle)
 * @note 句柄析构不取消任务
 */
class ZENIO_EXPORT AbortHandle {
public:
    AbortHandle() noexcept = default;
    AbortHandle(AbortHandle&& other) noexcept;
    AbortHandle& operator=(AbortHandle&& other) noexcept;
    AbortHandle(const AbortHandle&)            = delete;
    AbortHandle& operator=(const AbortHandle&) = delete;

    ~AbortHandle();

    [[nodiscard]] bool is_valid() const noexcept;

    //! @brief 取消这一个任务
    void Abort();

private:
    std::shared_ptr<detail::TaskRecord> record_;
};

/**
 * @brief 派发一个游离任务 (对应 tokio::spawn)
 * @throws std::runtime_error 不在运行时上下文里
 */
template <class T>
JoinHandle<T> Spawn(Task<T> task);

/**
 * @brief 派发任务, 并把当前 worker 让给它
 * @note 补充的 API, 语义来自 bthread 的 bthread_start_urgent。只能写成 co_await
 */
template <class T>
[[nodiscard]] detail::SpawnAndSeizeAwaiter<T, JoinHandle<T>> SpawnAndSeize(Task<T> task);

}  // namespace zenio
```

### 2.1 语义

**`Spawn` 就是 `StartBackground`** (S12): child 入队, 按需唤醒空闲 worker, 调用方继续运行。它在任意调用位置都能用, 不要求调用方是协程。返回的 `JoinHandle` 可以直接丢弃, 丢弃就是让它游离, 与 tokio 一致, 所以 `Spawn` 不标 `[[nodiscard]]`。

**`SpawnAndSeize` 只能写成 awaitable** (S14):

```cpp
zenio::JoinHandle<int> h = co_await zenio::SpawnAndSeize(Compute());
```

- 让出当前 worker 给 child: child 在调用方所在的 worker 上立即启动。
- 把当前协程重新投递到队列: 当前协程挂起, 排进当前 worker 的本地队列, 可以被别的 worker 偷走。
- 调用方必须是一个协程。

`co_await` 之后的代码不能假设 child 已经运行到哪一步: child 挂起之后, 如果调用方还没被偷走, 这个 worker 接着运行的通常就是它; 有空闲 worker 时, 调用方可能已经在别的线程上和 child 并发运行。

**不提供函数形式** (S14): 无栈协程只能在 `co_await` 处挂起。在普通函数里直接 `resume()` child 可以做到, 但调用方的栈还在, 无法被重新投递, 语义就变成"child 跑到挂起, 调用方再继续", 而且栈会随嵌套加深。

**`JoinHandle<T>`**:

- `co_await handle` 产出任务的 `T`, 或者重抛任务的异常。
- `Abort()` 取消这一个任务。
- 句柄析构不取消任务。句柄被丢弃之后任务抛的异常没人取, 默认丢弃 (S15), 见 design/runtime.md。

**`AbortHandle`**: 只有 `Abort()`。句柄析构不取消任务。

**归属** (S13): 全局入口产生的任务是游离任务; `JoinSet` 的同名方法产生的任务归集合管理, 返回 `AbortHandle`。

## 3. 内部数据结构 (概要)

`detail::TaskRecord` 是每个被派发出去的任务一份的记录, M5 细化。它至少包含:

- 这个任务自己的 `stop_source`。`Abort()` 调用它的 `request_stop()`。
- 任务状态: 未开始、运行中、已结束。
- 产出的存储, 用 `Result<T, std::exception_ptr>`。
- 等 `JoinHandle` 的等待者。

`JoinHandle<T>` 和 `AbortHandle` 持有它的 `shared_ptr`。所以句柄被丢弃不影响任务, 任务结束后句柄也还能问 `is_finished()`。

Runtime 的游离任务登记表在任务开始时登记它、结束时移除, 析构时靠这张表取消并等待全部游离任务。

## 4. 关键流程 (概要)

### 4.1 Spawn

1. 从 `CurrentContext()` 取运行时, 没有 `core` 就抛 `std::runtime_error`。
2. 创建 `TaskRecord`: 它的令牌链接到当前协程的令牌和 Runtime 的关闭令牌。
3. 把 child 登记进游离任务表, 投递到调度器。
4. 返回持有它的 `JoinHandle<T>`。

### 4.2 SpawnAndSeize

```cpp
// 调用方一侧: SpawnAndSeizeAwaiter<T, R>::await_suspend
std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller) noexcept {
    child_.promise().seize_caller_ = caller;
    return child_;                         // 调用方先完成挂起, child 在同一个 worker 上启动
}

// child 一侧: InitialAwaiter 的 await_resume, 是 child 启动后、函数体之前的第一件事
void await_resume() noexcept {
    promise_.token = /* 从线程局部存储取 */;   // 与普通的 Task 启动相同
    if (auto c = std::exchange(promise_.seize_caller_, {})) {
        Schedule(c);                        // 此时调用方已经挂起, 投递是安全的
    }
}
```

调用方被投递到当前 worker 的本地队列 (A8)。

重新投递必须等调用方挂起完成之后才能做。否则另一个 worker 可能在 `await_suspend` 返回前就恢复它, 这是未定义行为。

`JoinSet<T>::SpawnAndSeize` 流程相同, 只是 child 归集合管理, `R` 是 `AbortHandle`。

### 4.3 等待 JoinHandle

`co_await handle`: 任务已结束就直接取结果 (已结束且异常 → 重抛); 否则挂起, 任务结束时被唤醒。

### 4.4 Abort

`request_stop()`。协程在下一个 await 点抛 `CancelledError(CancelReason::ABORT)` 并展开。

## 5. 不变量

- 被派发出去的任务一定归属于某一方: 游离任务表, 或者一个 `JoinSet`。
- 句柄析构不取消任务, 也不销毁协程帧。
- 已启动未结束的任务不被销毁 (A5)。
- `SpawnAndSeize` 返回之后, 调用方恰好被投递一次。

## 6. 已知的坑

- **`SpawnAndSeize` 的调用方可能和 child 并发**: 调用方被投递之后可能被空闲 worker 偷走。`co_await SpawnAndSeize(child)` 之后的代码不能假设 child 已经运行到哪一步。
- **这个 await 点也是取消点**: 恢复后如果当前协程已被取消则抛 `CancelledError`。
- **`[[nodiscard]]` 只标在函数上**: 返回 awaiter 的函数不 `co_await` 就什么都不做, 所以 `SpawnAndSeize` 标了 `[[nodiscard]]`, 漏写 `co_await` 会编译警告。`JoinHandle` 的类上不能标: 标在类上等于所有返回它的函数都不能丢弃返回值, 而"丢弃句柄即游离"是正常用法。
- **每次 `Spawn` 一次堆分配** (`TaskRecord`, D10)。

## 7. 验收测试

M5 的任务书里给出。概要:

- `Spawn` 的任务在调用方继续运行之后才执行, 结果能通过 `JoinHandle` 取到。
- `SpawnAndSeize`: child 在调用方的 worker 上先启动; 调用方随后在某个 worker 上恢复, 恰好一次。
- `JoinHandle` 被丢弃后任务继续运行。
- `Abort()` 之后任务在下一个 await 点收到 `CancelledError`, `reason()` 是 `ABORT`。
- 没人取的异常默认丢弃, 不终止进程。
