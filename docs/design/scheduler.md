# 调度器

状态: 草稿
细化程度: 单线程部分全部已细化 (M3); 多线程部分是概要 (M6 前细化)
依据: S2、S9、S10、S12、S14、S25、S26; A5、A6、A15、A18、A22; D7、D11

## 1. 职责与边界

调度器决定哪个协程在哪个 worker 上、什么时候运行。它是 `detail::RuntimeCore` 的一部分, 使用者看不到。

它只做两件事:

- **队列**: 谁在等着运行。
- **park**: 没有任务的时候线程做什么。

调度是协作式的: 协程只在 await 点让出线程, 不抢占。

线程模型 (S2): 多线程形态下 n 个 worker 共享一个调度器, 从彼此的队列里偷任务, 协程可以在不同的 worker 上恢复。

M3 只有单线程形态 (A22): 一个先进先出的队列, 没有偷任务, 没有休眠, 也没有任何同步。

不在本文件: Poller 什么时候被运行 (design/poller.md), 定时器本身 (design/timer.md), 任务怎么被派发出去 (design/spawn.md), 上下文的内容 (design/task.md 2.2)。

## 2. 对外接口

内部接口。对外只有一个动作, 其余都是给驱动循环的代码用的:

| 动作 | 谁调用 | 含义 |
|------|--------|------|
| `RuntimeCore::Schedule(runnable)` | 投递方 | 把一个可恢复的协程入队。M6 起任意线程可调, M3 里只有运行时的线程会调 |
| `Scheduler::RunOne()` | 运行时 (`RuntimeCore::RunRoot`) | 执行一个任务; 队列是空的就返回 false |

`zenio/detail/scheduler.h`:

```cpp
namespace zenio::detail {

/**
 * @brief 交给调度器的协程: 句柄, 加上恢复它时要装进线程局部存储的上下文
 */
struct Runnable {
    std::coroutine_handle<> handle;             //!< 已经挂起、可以恢复的协程
    PromiseContext*         context = nullptr;  //!< 它挂起时的上下文, 见 design/task.md 2.2
};

/**
 * @brief 为一个正在挂起的协程生成 Runnable
 * @note 在协程自己的线程上、挂起的时候调用: 当时的上下文就是它恢复时要装回去的
 */
[[nodiscard]] Runnable MakeRunnable(std::coroutine_handle<> handle) noexcept;

/**
 * @brief 在作用域内把当前线程的上下文换成另一个, 离开作用域时还原
 */
class ScopedContext {
public:
    explicit ScopedContext(PromiseContext* context) noexcept;

    ScopedContext(const ScopedContext&)            = delete;
    ScopedContext& operator=(const ScopedContext&) = delete;

    ~ScopedContext();

private:
    PromiseContext* previous_;
};

/**
 * @brief 调度器: 决定哪个协程什么时候运行
 * @note M3: 单线程, 一个先进先出的队列, 没有同步
 */
class Scheduler {
public:
    Scheduler() = default;
    Scheduler(const Scheduler&)            = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    //! @brief 入队
    void Schedule(Runnable runnable);

    //! @brief 取一个任务运行。队列是空的就返回 false
    [[nodiscard]] bool RunOne();

private:
    std::deque<Runnable> queue_;
};

}  // namespace zenio::detail
```

`RuntimeCore::Schedule` 和 `Runtime::BlockOn` 怎么驱动循环, 见 design/runtime.md。

`Runnable` 的两个字段是一对, 谁投递谁负责配好:

| 谁投递 | `handle` | `context` |
|--------|----------|-----------|
| 协程自己挂起 (`YieldAwaiter`、`Waiter::TrySuspend`) | 自己的句柄 | `MakeRunnable` 取的当前上下文 |
| 创建根任务的一方 (`BlockOn`; M5 的 `Spawn`) | 根任务的句柄 | 自己准备的一份上下文: `BlockOn` 用运行时的根上下文, `Spawn` 用任务记录里的 |

根任务还没有启动, 它的 promise 里的上下文是空的, 所以不能用它自己的。启动的时候 `InitialAwaiter` 会把投递时给的那份复制进 promise, 见 design/task.md 2.2 第 4 条。

与之相关的规则:

- **`Spawn` 就是 `StartBackground`** (S12): 入队, 按需唤醒空闲 worker, 调用方继续运行。
- **事件唤醒一律经过调度器** (A5): I/O、定时器、取消、信号把协程交给 `RuntimeCore::Schedule`, 不在唤醒方的栈上嵌套恢复。
- **唯一在原地交接的是 `SpawnAndSeize`** (S14), 见 design/spawn.md。
- **子任务完成不经过调度器**: `final_suspend` 直接把控制权交给等待方, 见 design/task.md。

## 3. 内部数据结构

M3:

| 结构 | 内容 |
|------|------|
| `Scheduler::queue_` | 一个 `std::deque<Runnable>`, 先进先出。只在运行时的线程上访问 |

M6 加入 (概要):

- **每个 worker 一个 Chase-Lev 双端队列** (本地队列): 自己在底部压入和弹出, 后进先出; 别的 worker 从顶部偷, 先进先出。
- **一个全局队列**: 接收非 worker 线程的投递。
- **每个 worker 一个 park 状态**: 运行中、空闲、阻塞在 Poller 里。
- **空闲 worker 的计数**: `Schedule` 用它决定要不要唤醒谁。
- **对 Poller 和定时器驱动的引用**: 空闲等待路径把两者组合起来, 见 design/timer.md 4.6。

## 4. 关键流程

### 4.1 取任务

M6 细化, 这里保留概要。worker 没有任务时按这个顺序找:

1. 本地队列。
2. 全局队列。
3. 从别的 worker 偷。

都取不到, 进入空闲等待 (S10)。这一段把 Poller 和定时器驱动组合在一起, 结构照 tokio 的 `park_internal`:

1. 向定时器驱动要最近一个到期时刻。有就换算成阻塞的上限 (已经到期就是 0); 没有定时器驱动, 或者没有定时器, 就一直等。
2. 有 Poller 就运行 Poller, 把上限传给它, 在 Poller 的阻塞等待里等事件; 没有 Poller 就按上限休眠 (D7 的标准库同步原语)。
3. 醒来之后调用 `TimerDriver::Process()`, 触发到期的定时器。
4. 回到循环的开头。

如果 Poller 正被另一个 worker 运行, 这个 worker 不走上面的路径, 直接休眠。

忙碌的 worker 也会顺手处理定时器 (A15): 每运行完一个任务, 用不加锁的 `TimerDriver::HasDue()` 看一眼, 有到期的就调用 `Process()`。tokio 只在休眠时处理定时器, 所有 worker 都忙的时候定时器会晚。zenio 加了这一步, 代价是每个任务一次原子读; 有定时器挂着时再加一次取当前时间。

### 4.2 投递

M3:

```
RuntimeCore::Schedule(runnable):
    scheduler_.Schedule(runnable)       // 就是 queue_.push_back
```

没有唤醒, 因为没有别的线程。运行中的协程投递的任务排在当前队列的末尾。

M6 细化, 概要如下:

1. 入队。在 worker 线程上调用时进当前 worker 的本地队列, 否则进全局队列。
2. 有空闲的 worker 就唤醒一个。
3. 空闲的 worker 都阻塞在 Poller 里时, 往唤醒管道写一个字节, 打断 Poller。Poller 返回后那个 worker 会重新找任务。

### 4.3 运行一个任务

`scheduler.cpp` 的全部内容 (M3):

```cpp
Runnable MakeRunnable(std::coroutine_handle<> handle) noexcept {
    return Runnable{handle, CurrentContext()};
}

ScopedContext::ScopedContext(PromiseContext* context) noexcept : previous_(CurrentContext()) {
    SetCurrentContext(context);
}

ScopedContext::~ScopedContext() {
    SetCurrentContext(previous_);
}

void Scheduler::Schedule(Runnable runnable) {
    queue_.push_back(runnable);
}

bool Scheduler::RunOne() {
    if (queue_.empty()) {
        return false;
    }
    const Runnable task = queue_.front();
    queue_.pop_front();
    ScopedContext scope(task.context);
    task.handle.resume();
    return true;
}
```

- **先把 `Runnable` 拷出来, 再恢复协程**: 恢复期间可能有新的投递 (协程 `co_await Yield()`), 不能拿着队列里的引用去恢复。
- **恢复之前把它的上下文装到线程上, 返回之后还原**: 由 `ScopedContext` 完成。所以协程恢复时看到的上下文就是它挂起时的那一份; `RunOne` 返回之后线程上的上下文和调用之前一样。
- **`resume()` 在两种情况下返回**: 协程挂起了 (`Yield`, 以后还有各种 awaiter); 或者协程结束并且没有等待方 (根任务, `FinalAwaiter` 返回 `noop_coroutine`)。`co_await Task` 把控制权交给 child 的时候不会返回到这里: 那是一次尾调用, 栈上还是同一个 `resume()` 调用, 见 design/task.md 第 6 节。
- **`resume()` 不会抛异常**: `Task` 的 `unhandled_exception` 把异常存进了 `storage_`。万一有异常穿出来 (析构函数抛的), `ScopedContext` 也会把上下文还原。

顺序是先进先出: 投递的顺序就是运行的顺序, 运行中的协程投递的排在最后。

### 4.4 单线程循环

`RuntimeCore::RunRoot` 的循环, 见 design/runtime.md 4.2: 投递根任务, 然后 `while (scheduler_.RunOne()) {}`。

M3 里队列空了就是没事可做: 没有 Poller 也没有定时器, 一个协程只可能挂在 `co_await Task` 上 (它的 child 在同一个 `resume()` 调用里接着运行) 或者 `Yield` 上 (它在队列里)。所以根任务结束的时候队列一定是空的。

M4 起队列空了进入 4.1 的空闲等待, 循环跑到根任务结束为止。

### 4.5 SpawnAndSeize 的重新投递

M5 细化。child 启动时把调用方投递到当前 worker 的本地队列。因为本地队列是后进先出, child 挂起之后这个 worker 取到的通常就是调用方。

## 5. 不变量

- 一个协程同一时刻要么在运行, 要么在恰好一个队列里, 要么挂起等待事件, 三者只居其一。
- 队列里的 `Runnable`, 它的协程已经挂起, 它的 `context` 在协程结束之前一直有效。
- 一个协程恢复运行时, 线程上的上下文是它挂起时的那一份; `RunOne` 返回之后, 线程上的上下文和调用之前一样。
- `RunOne` 在恢复协程期间不持有队列里的引用。
- M6 起: 有可运行任务且有空闲 worker 时, 这个 worker 会被唤醒。
- 唤醒方从不在自己的栈上恢复被唤醒的协程。
- 正在运行 Poller 的 worker 至多一个。

## 6. 已知的坑

- **M3 的调度器没有同步**: `queue_` 是普通的 `std::deque`。M3 里 `RuntimeCore::Schedule` 只能在运行时的线程上调用, `Waiter::Signal` 也是。别的线程投递是 M6 的事, 那时才加同步。
- **恢复协程期间不能动队列里取出来的那一项**: 见 4.3。
- **先进先出在 M6 会变**: 本地队列是后进先出的。这对 `SpawnAndSeize` 正合适 (child 挂起之后先取回调用方), 对 `Yield` 正相反 (调用方会被立刻再取出来, 别的任务得不到机会)。M6 细化时必须处理这个矛盾, 到时 `Yield` 的投递位置和 design/task.md 4.4 里的说法可能要改。M3 只有一个先进先出的队列, 没有这个问题。
- **满载时 Poller 得不到 worker** (D11): 所有 worker 都在跑长任务时没人轮询, I/O 事件要等有 worker 空出来才被处理。定时器不受影响, 忙碌的 worker 会顺手处理 (A15)。
- **定时器和 Poller 是两个东西**: 空闲等待路径只是按顺序驱动它们。不要把定时器的数据或者处理逻辑挪进 Poller (S26)。
- **唤醒丢失**: worker 判断"没有任务"和进入休眠之间, 别的线程可能刚好投递了任务。判断和休眠必须是一个原子动作, M6 细化。
- **Chase-Lev 的内存序**: 压入、弹出、偷取三个方向的内存序容易写错。M6 细化时给出完整的实现步骤, 实现会话照着写。
- **休眠唤醒先用标准库的同步原语** (D7): `std::mutex` 加 `std::condition_variable`, 不直接用 futex。
- **唤醒管道** (D6): 见 design/poller.md。

## 7. 验收测试

M3 的任务书里给出单线程部分, M6 的任务书里给出多线程部分。

M3, `tests/zenio/detail/scheduler_unittest.cpp`。直接构造 `RuntimeCore` 并驱动它的调度器, 用两个 `Task<void>` 当互不相干的链:

- 空队列上 `RunOne()` 返回 false。
- 先进先出: 依次投递 A、B, 运行顺序是 A、B。
- 让出: A 运行到 `co_await Yield()` 时把自己投递进队列, 排在 B 之后。运行顺序是 A 开始、B、A 继续。
- 运行完之后线程上的上下文还原成空。
- 一条链只有 A 一个任务时, 让出之后它自己被再次取出来运行完。

M4 起:

- 被事件唤醒的协程能恢复。

M6:

- 多线程: 4 个 worker 上跑完 10 万个任务, 没有丢失、没有重复; TSAN 下全量单测通过。
- 空闲时确实有且只有一个 worker 阻塞在 Poller 里。
- 有定时器时, 空闲等待在到期时刻醒来, 定时器被触发; 登记一个更早的定时器能让阻塞中的等待提前醒来。
- 所有 worker 都在忙的时候定时器仍能被触发。
