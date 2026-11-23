# 信号

状态: 草稿
细化程度: 对外接口已细化 (M1); 内部是概要 (M7 前细化)
依据: S7; D12

## 1. 职责与边界

- `CtrlC()` 返回 SIGINT 的接收流。每收到一次信号, `Recv()` 返回一次。对应 `tokio::signal::ctrl_c`, 以及 `tokio::signal::unix::Signal` 的 `recv()`。
- 信号不进 poller (S7): 靠信号处理器加唤醒管道, 不用 kqueue 的 `EVFILT_SIGNAL`, 也不用 Linux 的 `signalfd`。
- 信号是进程级的 (D12): v1 假定一个进程只有一个 Runtime 使用信号。

v1 只有 `CtrlC()`。任意信号的流以后再做。

## 2. 对外接口

`zenio/signal.h`:

```cpp
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

    std::uint64_t id_ = 0;   //!< 信号源编号, 0 表示空句柄
};

/**
 * @brief SIGINT 的接收流 (对应 tokio::signal::ctrl_c)
 * @throws std::runtime_error 不在运行时上下文里, 或运行时没有开启 I/O
 */
[[nodiscard]] Signal CtrlC();

}  // namespace zenio::signal
```

## 3. 内部数据结构 (概要)

| 结构 | 说明 |
|------|------|
| 信号源登记表 | 进程级的静态数组, 每种信号一项。v1 只有 SIGINT 一项 |
| 计数 | 每个信号源一个原子计数: 收到了但还没被取走的次数 |
| 等待者链表 | 每个信号源一条, 挂着正在 `Recv()` 上等待的 `Waiter` |
| 信号处理器 | 进程级, 第一次调用 `CtrlC()` 时安装 |
| 唤醒管道 | 用 Poller 的那一根, 见 design/poller.md |

处理器只能访问静态数据, 不持有任何对象的指针, 所以没有生命周期问题。

## 4. 关键流程 (概要)

### 4.1 信号处理器

在收到信号的那个线程上执行, 只有两种动作:

1. 给信号源的计数加一。
2. 往唤醒管道的写端写一个字节。

两步都是异步信号安全的。处理器必须显式写管道: 线程睡在 poller 里时只能被 fd 事件叫醒, 而多线程进程里信号落在哪个线程上不确定。

### 4.2 Recv

- 计数大于零: 减一, 立即返回, 不挂起。
- 否则把自己挂进信号源的等待者链表, 然后挂起。

### 4.3 唤醒

Runtime 线程处理唤醒时 (Poller 从 `Poll` 返回, 或者 worker 从休眠中醒来), 检查信号源的计数。计数大于零就把链表里的等待者交给调度器。

### 4.4 取消 Recv

只是把自己从等待者链表里摘掉, 不经过 Poller。

## 5. 不变量

- 没人在 `Recv()` 上等待时到达的信号不丢: 计数保留, 下一次 `Recv()` 立即返回。
- 处理器不碰任何对象, 不拿锁, 不恢复协程。
- 两个平台走同一条代码路径, 不改动线程的信号屏蔽字。

## 6. 已知的坑

- **安装处理器之后 SIGINT 不再终止进程**, 即使 Runtime 已经析构。处理器一旦安装就不再卸载, 唤醒管道也不关闭 (D12)。
- **默认动作必须被接管**: 没有安装处理器之前, SIGINT 的默认行为是终止进程。所以处理器要在第一次 `CtrlC()` 时就装好, 不能等到第一次 `Recv()`。
- **处理器在任意线程上打断正在运行的代码**: 它所在的线程当时可能正持有着用户代码里的锁, 所以处理器里不能做任何可能拿锁的事。
- **唤醒管道的写端是非阻塞的**: 管道满了就丢弃这次写入, 已有的字节足够唤醒。
- **`Signal` 依赖创建它的运行时**: Runtime 析构之后再 `Recv()` 是未定义行为。

## 7. 验收测试

M7 的任务书里给出。概要:

- 进程内 `raise(SIGINT)` 之后, 挂起的 `Recv()` 返回。
- 无人在 `Recv()` 上等待时连续 `raise` 三次, 之后三次 `Recv()` 都立即返回。
- 多线程 Runtime 下, 在 worker 线程上 `raise` 也能唤醒。
- 取消挂起的 `Recv()`, 协程收到 `CancelledError`。
- 安装处理器之后, SIGINT 不再终止进程。
