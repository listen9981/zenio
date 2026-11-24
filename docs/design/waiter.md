# 挂起协议与取消

状态: 草稿
细化程度: Waiter 的协议和实现已细化 (M3); 取消流程是概要 (M5 前细化)
依据: S14、S17; A5、A18

## 1. 职责与边界

本文件规定两件事:

- **`Waiter`**: 每个 await 点里的状态机。唤醒它的那一方可能在任意线程, 也可能在协程真正挂起之前就来了。`Waiter` 保证协程恰好被唤醒一次。
- **取消**: 令牌怎么传播, 取消怎么送到协程, 清理代码怎么绕开取消。

`Waiter` 是内部类型, 使用者看不到。使用者能看到的只有 `Uncancellable`。

不在本文件: 具体的 awaiter 怎么用 `Waiter` (design/task.md 的 awaiter 约定, 以及各操作的文档), 被唤醒的协程怎么排到队列里、什么时候恢复 (design/scheduler.md)。

## 2. 对外接口

### 2.1 `zenio/cancel.h`

```cpp
namespace zenio::detail {
template <class Awaitable> class UncancellableAwaiter;
}

namespace zenio {

/**
 * @brief 用一个不可取消的上下文等待 awaitable
 * @note 给清理代码用: 当前协程已经被取消, 但还需要 co_await 的时候
 */
template <class Awaitable>
[[nodiscard]] detail::UncancellableAwaiter<std::decay_t<Awaitable>> Uncancellable(Awaitable&& awaitable);

}  // namespace zenio
```

`Uncancellable` 是补充的 API, tokio 没有。

### 2.2 内部接口 `zenio/detail/waiter.h`

```cpp
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
        EMPTY,       //!< 还没有走到挂起这一步
        SUSPENDED,   //!< 已登记, 协程正在挂起
        SCHEDULED,   //!< 已经有人负责恢复这个协程
    };

    std::atomic<State> state_{State::EMPTY};
    std::atomic<bool>  signaled_{false};
    Runnable           self_;    //!< 挂起时记下: 句柄和当时的上下文
};

}  // namespace zenio::detail
```

`self_` 是一个 `Runnable` (design/scheduler.md 第 2 节): 除了句柄, 还带上协程挂起时的上下文, `Signal` 用它投递。

## 3. 内部数据结构

`Waiter` 的三个成员见 2.2。两个原子变量分工明确:

- `signaled_`: 事件到了没有。只从 false 变成 true 一次。
- `state_`: 谁负责恢复协程。`SCHEDULED` 表示已经有确定的负责方。

`self_` 只在 `TrySuspend` 里写 (协程自己的线程上), `Signal` 里读。它的内容在 `Signal` 看来必须是完整的, 这由两个原子变量的内存序保证, 见 4.1。

## 4. 关键流程

### 4.1 Waiter 的协议

**`TrySuspend(self)`**, 在协程自己的线程上执行:

1. 记下 `self_ = MakeRunnable(self)`: 句柄, 加上此时的当前上下文 (A18)。
2. 把 `state_` 设成 `SUSPENDED` (release)。
3. 读 `signaled_` (acquire)。是 false 就返回 true, 协程挂起。
4. 是 true 说明 `Signal` 抢在挂起之前到了: 把 `state_` 从 `SUSPENDED` 改成 `SCHEDULED` (CAS)。改成功返回 false, 协程原地恢复; 改失败返回 true, 说明 `Signal` 那边已经把协程投递了。

**`Signal()`**, 可能在任意线程上执行:

1. 把 `signaled_` 设成 true。如果它原本就是 true, 直接返回。
2. 把 `state_` 换成 `SCHEDULED`, 看旧值。旧值是 `SUSPENDED` 就把 `self_` 交给调度器; 否则什么都不做, 交给第 4 步里调用方自己的那次 CAS。

三种时序:

| 时序 | `TrySuspend` 的结果 | 谁恢复协程 |
|------|--------------------|------------|
| `Signal` 在挂起之后才到 | 返回 true | `Signal`, 它把协程投递给调度器 |
| `Signal` 在第 2 步和第 3 步之间到 | CAS 失败, 返回 true | `Signal`, 同上 |
| `Signal` 在第 2 步之前到 | CAS 成功, 返回 false | 调用方自己, 原地恢复 |

内存序: 第 2 步的 release 和 `Signal` 第 2 步的 acq_rel 配对, 保证 `Signal` 读到的 `self_` 是完整的。

实现 (M3, `waiter.cpp`):

```cpp
bool Waiter::TrySuspend(std::coroutine_handle<> self) noexcept {
    self_ = MakeRunnable(self);
    state_.store(State::SUSPENDED, std::memory_order::release);
    if (!signaled_.load(std::memory_order::acquire)) {
        return true;
    }
    // Signal 抢在挂起之前到了: 自己负责原地恢复
    State expected = State::SUSPENDED;
    return !state_.compare_exchange_strong(expected, State::SCHEDULED, std::memory_order::acq_rel,
                                           std::memory_order::acquire);
}

void Waiter::Signal() noexcept {
    if (signaled_.exchange(true, std::memory_order::acq_rel)) {
        return;
    }
    if (state_.exchange(State::SCHEDULED, std::memory_order::acq_rel) == State::SUSPENDED) {
        assert(self_.context != nullptr && self_.context->core != nullptr);
        self_.context->core->Schedule(self_);    // 之后不能再访问 this
    }
}
```

第 1 步的 CAS 是"谁赢谁负责": 成功说明自己是第一个把状态改成 `SCHEDULED` 的, 协程还没被投递, 由自己原地恢复; 失败说明 `Signal` 已经投递了, 不能再恢复一次。

`Signal` 的两个分支都到位之后, 协程被投递恰好一次, 或者由挂起方原地恢复。`TrySuspend` 返回 false 的那条路上 `signaled_` 已经是 true, 之后再来 `Signal` 也不会再投递。

被投递的协程什么时候恢复、带着什么上下文恢复, 见 design/scheduler.md 4.3。

### 4.2 awaiter 怎么用 Waiter (概要)

一个会等待的 awaiter, `await_suspend` 大致是:

1. 从 `CurrentContext()` 取令牌和运行时。
2. 登记到对应的驱动: I/O 等待向 Poller 投递登记命令 (arm); 定时器向定时器驱动登记 (design/timer.md); 信号挂到信号源的等待者链表上 (design/signal.md)。
3. 注册 stop callback。
4. `return waiter_.TrySuspend(caller)`。两个函数的返回值含义相同: true 表示挂起, false 表示原地恢复。

`await_resume` 里注销 stop callback, 先看取消标志 (有就抛 `CancelledError`), 再看事件结果。它不读当前上下文 (design/task.md 2.3 第 7 条)。

### 4.3 取消 (概要, M5 前细化)

规则:

- 取消令牌用 `std::stop_token`, 沿任务树向下传: Runtime、主任务、`JoinSet` / `Select`、子任务。
- 令牌的传播本身在 M3 已经生效: 每个协程启动时从当前上下文复制令牌 (design/task.md 2.2), `BlockOn` 的主任务拿到的是运行时的关闭令牌 (design/runtime.md 4.2)。
- `JoinSet` 和 `Select` 各有自己的 `stop_source`, 链接到父任务的令牌, 父任务被取消时连带取消子任务。
- 游离任务和 `JoinSet` 里的每个任务另有一个独立的 `stop_source`: `Abort` 只取消这一个任务, `AbortAll` 取消集合里全部任务。所有任务的令牌都链接到 Runtime 的关闭令牌。
- 每个 await 点都是取消点: 挂起前和恢复后都检查令牌, 已取消则抛 `CancelledError`。
- 例外: 等待子任务的 await 点 (`JoinAll`、`Select`) 在父任务被取消时, 先等子任务全部结束, 再抛 `CancelledError`。
- 取消不可撤销: `catch (const CancelledError&)` 之后再次 `co_await` 仍会抛 `CancelledError`。
- 取消不销毁协程帧。协程在 await 点抛出 `CancelledError` 后正常展开。

I/O 等待的取消流程:

1. 任意线程 `request_stop()`, 该令牌下所有挂起 awaiter 注册的 stop callback 被调用: 置取消标志, 给 Poller 投一条取消提示。
2. Poller 摘掉注册并 `Signal`, 与正常就绪走同一个出口。所以协程帧不会在 Poller 还持有指针时被释放。
3. 协程恢复后看到取消标志, 抛 `CancelledError`, 沿 `co_await` 链向上展开。

取消提示可能先于登记命令到达 Poller, 所以 Poller 处理登记命令时必须先检查取消标志。

定时器等待的取消不经过 Poller: stop callback 调用 `TimerDriver::Cancel`, 它在定时器驱动的锁里摘掉登记, 解锁之后 `Signal`。道理和 I/O 一样, 唤醒方只能有一个。见 design/timer.md 4.2。

**`Uncancellable(a)`**: 交接给 `a` 之前, 把当前上下文换成一个令牌永不可取消的副本; `a` 结束之后还原。副本放在 awaiter 里, 活在调用方的协程帧内, 所以在 `a` 挂起期间它一直有效: 调用方带着这份副本被投递, 恢复时装回去的也是它 (design/task.md 2.2 第 2 条)。

## 5. 不变量

- 一个 `Waiter` 对应的协程恰好被恢复一次, 不管 `TrySuspend` 和 `Signal` 以什么顺序、在多少个线程上交错。
- `Signal` 在 `TrySuspend` 之前被调用时, 协程不会挂起。
- `TrySuspend` 返回 false 时, 这个 `Waiter` 不会再导致协程被投递。
- 挂起中的 I/O 等待只能由 Poller 唤醒; 取消也走这个出口。
- 挂起中的定时器等待只能由定时器驱动唤醒; 取消也走这个出口。
- 已取消的令牌不会恢复。

## 6. 已知的坑

- **恢复一个还没挂起的协程是未定义行为**。任何"把协程句柄交给调度器"的动作, 都必须发生在协程真正挂起之后。`Waiter` 的协议就是为了这一点。
- **`TrySuspend` 返回 false 时 awaiter 的外部登记已经处理完了**: 原地恢复只可能是对应的驱动 (Poller 或定时器驱动) 刚刚 `Signal` 过, 那时登记已经摘掉。
- **`Signal()` 里 `Schedule` 之后不能再访问 `this`**: `Schedule` 把协程交给调度器之后, 另一个 worker 可能立刻恢复它; 协程往下走, awaiter 被销毁, 这块内存就不能再用了。所以 `Schedule(self_)` 必须是 `Signal` 里最后一次访问成员。上面实现里的 `assert` 在 `Schedule` 之前, 也是这个原因。
- **M3 里 `Signal` 只能在运行时的线程上调用**: 协议本身是线程安全的, 但它最后调用的 `RuntimeCore::Schedule` 在 M3 没有同步 (design/scheduler.md 第 6 节)。M6 给调度器加上同步之后这条限制解除。
- **`TrySuspend` 必须在协程自己的线程上调用**: 它用 `MakeRunnable` 取当前上下文, 别的线程上取到的是别人的。
- **stop callback 可能在注册的那一刻就被调用**: 令牌已经取消时, `std::stop_callback` 的构造函数会在当前线程上立刻调用回调。awaiter 要先登记、后检查, 顺序不能反。
- **stop callback 在别的线程上运行时不能销毁它所在的协程帧**: `std::stop_callback` 的析构函数会等回调结束, 所以 `await_resume` 的第一件事是注销 callback。
- **stop callback 运行在发起取消的那个线程上**: 它只做置标志和投递命令, 不拿锁, 不恢复协程。

## 7. 验收测试

M3 的任务书里给出协议的单线程部分, M5 的任务书里给出取消, M6 用多线程压力测试验证全部交错。

M3, `tests/zenio/detail/waiter_unittest.cpp`。直接构造 `RuntimeCore`, 被唤醒的协程是一个 `Task<void>`, 它通过一个测试专用的 awaiter 挂在 `Waiter` 上:

```cpp
// 测试专用的 awaiter: 挂在一个 Waiter 上。signal_first 模拟 Signal 抢在 TrySuspend 之前到
struct TestAwaiter {
    zenio::detail::Waiter* waiter;
    bool                   signal_first = false;

    bool await_ready() noexcept { return false; }
    bool await_suspend(std::coroutine_handle<> self) noexcept {
        if (signal_first) {
            waiter->Signal();
        }
        return waiter->TrySuspend(self);
    }
    void await_resume() noexcept {}
};

zenio::Task<void> WaitOn(zenio::detail::Waiter* waiter, bool signal_first, std::vector<int>* log) {
    log->push_back(1);
    co_await TestAwaiter{waiter, signal_first};
    log->push_back(2);
}
```

协程照根任务的方式启动 (design/scheduler.md 第 2 节的表, 用运行时的根上下文投递):

- 先挂起后唤醒: 运行到 `await_suspend` 后挂起, 记录是 `{1}`, 队列是空的; `Signal` 之后队列里有一项; 运行它, 协程接着走完, 记录是 `{1, 2}`。
- 先唤醒后挂起 (`signal_first`): `TrySuspend` 返回 false, 协程原地恢复, 一次 `RunOne` 就走完, 记录是 `{1, 2}`, `Signal` 之后队列里什么都没有。
- 重复唤醒只投递一次: 挂起之后 `Signal` 两次, 队列里只有一项。
- 没有挂起过就 `Signal`: 什么都不发生。
- `Signal` 在第 2 步和第 3 步之间到的时序, 单线程里没法确定性地复现, 由 M6 的多线程压力测试覆盖: 一个 `Waiter`, 多个线程同时 `TrySuspend` 和 `Signal`, 协程恰好被投递一次。

M5:

- 取消挂起中的 read: 协程收到 `CancelledError`。
- 取消之后清理代码里的 `co_await` 抛 `CancelledError`; 换成 `Uncancellable` 之后正常等待。
