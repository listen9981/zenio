# Poller 与 Driver

状态: 草稿
细化程度: 概要 (M4 前细化, epoll 部分 M8 前)
依据: S1、S7、S9、S10、S26; A5、A6; D4、D6、D11

## 1. 职责与边界

Poller 是 Runtime 启动时创建的一个内部协程 (S9), 和用户任务一样由 worker 运行。哪个 worker 运行它, 哪个 worker 就是在做 I/O。

- 它独占 epoll/kqueue 实例和 fd 注册表。
- 它只做"等事件、唤醒等待者"。真正的 read / write / accept 由被唤醒的协程自己做, Poller 不碰缓冲区。
- Driver 收敛 epoll 与 kqueue 的差异, 编译期选择实现。
- 信号不是 poller 的事件 (S7), 见 design/signal.md。
- **定时器不在 Poller 里** (S26)。定时器是独立的子系统, 见 design/timer.md。Poller 和它只有一处关系: 调度器把"最近的定时器什么时候到期"换算成 Poller 的阻塞上限。

不在本文件: Poller 什么时候被 worker 运行 (design/scheduler.md), 唤醒等待者的协议 (design/waiter.md), 定时器 (design/timer.md)。

## 2. 对外接口

内部接口。Poller 对其它子系统只有两条通道:

- **命令队列**: 多个生产者 (任意线程), 单个消费者 (Poller)。
- **就绪分发**: Poller 调用 `Waiter::Signal`, 把协程交给调度器。

命令只和 fd 有关 (概要, M4 细化):

| 命令 | 参数 | 作用 |
|------|------|------|
| 登记 | fd、方向 (读或写)、`Waiter*`、对象代号 | 让这个 fd 在指定方向就绪时唤醒该等待者 |
| 取消登记 | fd、方向、`Waiter*` | 摘掉登记, 并唤醒该等待者 (它恢复后会看到取消标志) |

Driver 接口收敛 epoll 与 kqueue 的全部差异, 定义在 `zenio/detail/driver.h`:

```cpp
namespace zenio::detail {

//! @brief 等待的方向
enum class Interest : std::uint32_t {
    READ  = 1,
    WRITE = 2,
};

struct FdSlot;   //!< fd 注册表里的一个槽, M4 细化
struct Event;    //!< Poll 产出的一个事件, M4 细化

/**
 * @brief epoll / kqueue 的平台差异都收敛在这里
 * @note 只在运行 Poller 的那个线程上调用
 */
class Driver {
public:
    virtual ~Driver() = default;

    //! @brief 首次登记一个 fd
    virtual void Attach(FdSlot& slot) = 0;
    //! @brief 一次性登记, 每次等待都要重新 arm
    virtual void Arm(FdSlot& slot, Interest interest) = 0;
    //! @brief 注销一个 fd
    virtual void Detach(FdSlot& slot) = 0;
    //! @brief 等事件。timeout 为负表示一直等
    //! @note timeout 由调用方给, Poller 不自己算, 见 design/scheduler.md 4.1
    virtual int Poll(Event* events, int max_events, std::chrono::nanoseconds timeout) = 0;
};

}  // namespace zenio::detail
```

实现文件: Linux 编译 `driver_epoll.cpp`, macOS 编译 `driver_kqueue.cpp`。

## 3. 内部数据结构 (概要)

| 结构 | 内容 |
|------|------|
| Driver | epoll 或 kqueue 的 fd, 事件缓冲区 |
| fd 注册表 | 按 fd 编号索引的槽表。每个槽记录对象代号、读等待者、写等待者 |
| 命令队列 | 多个生产者, 单个消费者 |
| 唤醒管道 | 一根非阻塞管道, 读端登记在 poller 里 |

## 4. 关键流程 (概要)

### 4.1 Poller 的循环 (S10)

Poller 被一个空闲的 worker 恢复时, 带一个参数: 阻塞的上限。

1. 处理命令队列: 登记和取消登记交给 Driver。
2. 调用 `Poll`, 把上限传给它。上限来自调度器: 有定时器驱动时取最近一个到期时刻, 没有就一直等 (design/timer.md 4.6)。
3. 对每个就绪事件: 摘掉这一条登记, 调用对应等待者的 `Signal`。
4. 挂起自己, 等下一个空闲的 worker 来恢复。

Poller 不把自己重新投递。"只在空闲时阻塞"就是这样实现的: 只有取不到任务的 worker 才会运行它。

醒来之后触发到期的定时器 (`TimerDriver::Process()`) 不是 Poller 的事。它在 Poller 挂起之后, 由运行 Poller 的那个 worker 在调度器的空闲等待路径里做, 见 design/scheduler.md 4.1。

### 4.2 唤醒管道

三种情况往写端写一个字节:

- worker 投递命令之后。
- 调度器发现空闲的 worker 都阻塞在 Poller 里, 而这时来了新的可运行任务。
- 定时器驱动登记了一个比当前阻塞上限更早到期的定时器, 要让 Poller 提前醒来重新计算上限。这是定时器驱动和 Poller 之间唯一的联系。

信号处理器也用它, 见 design/signal.md。

### 4.3 I/O 等待到就绪

1. worker 上协程调用 `read()`, 返回 `EAGAIN`。
2. awaiter 往命令队列投一条登记命令, 注册 stop callback, 然后 `Waiter::TrySuspend`。
3. 某个空闲的 worker 运行 Poller。Poller 处理命令, `Poll` 返回就绪事件, 摘掉登记, `Signal` 把协程交给调度器, 然后挂起。
4. worker 回到循环, 恢复这个协程。协程注销 stop callback, 重新 `read()`。

### 4.4 和定时器的关系

只有一条: Poller 的阻塞上限。`Sleep` 怎么登记、怎么触发、怎么取消, 全在 design/timer.md。

## 5. 不变量

- Poller 的数据只在运行 Poller 的那个线程上访问, 不加锁。"只有一个所有者"靠"一个协程同一时刻只在一个线程上运行"保证。
- 挂起中的 I/O 等待只能由 Poller 唤醒。取消也是经由 Poller 摘掉登记后再唤醒, 和正常就绪走同一个出口。所以协程帧不会在 Poller 还持有指针时被释放。
- 一次性注册: 就绪后恰好唤醒一个等待者。
- Poller 只在整个 Runtime 没有可运行任务时才阻塞。
- 信号不是 poller 的事件类型。poller 里与信号有关的只有那根唤醒管道。
- Poller 里没有定时器的数据。定时器驱动的数据结构不出现在 Poller 里。

## 6. 已知的坑

- **取消提示可能先于登记命令到达**: 两个命令来自不同的线程。所以处理登记命令时必须先检查等待者上的取消标志。
- **fd 编号被复用**: fd 关闭之后编号可能立刻被新对象占用。注册表靠对象代号区分新旧对象, 登记时代号对不上就当作新的 fd 重新登记。
- **每次等待都重新登记**: 一次性注册 (epoll 用 `EPOLLONESHOT`, kqueue 用 `EV_ONESHOT`), 每次等待多一次系统调用 (D4)。
- **epoll 注销**: 关闭 fd 会自动注销, 但 Poller 的注册表里还留着那个槽, 靠代号在下次登记时清掉。
- **唤醒管道是一个普通 fd** (D6): 两个 fd, 每次唤醒一次 write 和一次 read。写端是非阻塞的, 管道满了就丢弃这次写入, 因为管道里已有的字节足够唤醒。
- **满载时 Poller 得不到 worker** (D11): 见 design/scheduler.md。
- **不要把定时器加回 Poller**: 以前的设计是把定时器堆挂在 Poller 里, 已经推翻 (S26)。看到"Poller 处理定时器"之类的代码就是错的。

## 7. 验收测试

M4 的任务书里给出 kqueue 部分, M6 的任务书里给出空闲 worker 运行 Poller 的部分, M8 在 Linux 服务器上验证 epoll。定时器的测试在 design/timer.md 第 7 节。概要:

- TCP 回显: 上千个并发连接全部正确回显, ASAN 下无泄漏。
- 空闲时确实有且只有一个 worker 阻塞在 Poller 里; 新任务到达后它能及时返回。
- 有定时器时, Poller 的阻塞上限等于最近一个到期时刻换算出的时长; 登记更早的定时器之后, 阻塞中的 Poller 被唤醒并换用新的上限。
