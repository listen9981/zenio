# 定时器: 移植 tokio 的时间轮

状态: 草稿
细化程度: 移植指南已细化 (M4)。`SleepAwaiter` 和驱动的衔接、满载时怎么处理是假设, 待审查
依据: S25、S26; A15、A16

**tokio 源码位置**: `~/workspace/from_git/tokio`, 即 `/Users/listen/workspace/from_git/tokio`。本文档对着提交 `1daf3c5a` (2026-09-24) 写, 下文所有 tokio 路径都相对于它的 `tokio/src/`。动手前先核对提交号, 不一致就记偏差。

## 1. 职责与边界

- 定时器是独立的子系统: 时间轮、时间源, 外加一层驱动接口 (S26)。它不放在 Poller 里, Poller 只管 fd。
- 实现直接移植 tokio 的传统定时器 (S25): 分层时间轮 (hashed timing wheel), 出自 Varghese 和 Lauck 的论文。
- 精度 1 毫秒 (S25)。不足 1 毫秒的时长向上取整到下一毫秒。最远支持 2^36 毫秒, 约 2 年。
- 对使用者, 定时器只通过 `Sleep`、`SleepUntil`、`Timeout` 露面, 接口见 design/select.md。本文档全是内部接口, 在 `zenio::detail` 里。

### 1.1 移植什么

| tokio 的部分 | 移植吗 | 说明 |
|--------------|--------|------|
| `runtime/time/wheel/mod.rs`、`runtime/time/wheel/level.rs` | 移植 | 时间轮本体 |
| `runtime/time/entry.rs` | 移植 | 条目和它的状态机。并发正确性都在这里 |
| `runtime/time/source.rs` | 移植 | 时间源 |
| `runtime/time/mod.rs` 里的 `Handle`、`Inner`、`InnerState`, 以及 `process_at_time`、`clear_entry`、`reregister` | 移植 | 驱动的数据和处理逻辑 |
| `runtime/time/mod.rs` 里的 `Driver::park_internal` | 不搬代码, 搬结构 | 它是"休眠"和定时器的组合。zenio 里由调度器的空闲等待路径承担, 见 4.6 和 design/scheduler.md |
| `util/linked_list.rs` | 移植 | 侵入式双向链表。槽和待触发队列都用它 |
| `time/clock.rs` 里 `Clock` 这层抽象 | 移植接口 | 时间源通过它取当前时间。生产环境用真实时钟, 测试里换成可手动拨动的实现 (A16) |
| `runtime/time/tests/mod.rs`、`wheel/` 下的单元测试 | 移植 | 作为验收测试, 见第 7 节 |

### 1.2 不移植什么

| tokio 的部分 | 原因 |
|--------------|------|
| `runtime/time_alt/` 整个目录 | tokio 的另一种实验性实现, 要 `tokio_unstable` 才能启用, 不是你说的那个时间轮 |
| `time/clock.rs` 里可暂停、自动推进的时钟 | 那是 tokio 给测试用的功能。zenio 只移植 `Clock` 的接口, 测试里用最简单的手动拨动 |
| `time/interval.rs` | 周期定时器不在 v1 |
| `entry.rs` 里的 `reset`、`extend_expiration` | 它们服务于"同一个定时器反复改到期时间"。v1 的 `SleepAwaiter` 是一次性的。以后做 `Interval` 时再移植 |
| `time/sleep.rs` 里依赖可暂停时钟的测试 | 跑不起来 |

不在本文件: 空闲时谁来驱动定时器 (design/scheduler.md), I/O 等待 (design/poller.md), 唤醒等待者的协议 (design/waiter.md)。

## 2. 对外接口

### 2.1 文件

| zenio 文件 | 内容 | 对应 tokio |
|------------|------|------------|
| `zenio/detail/intrusive_list.h` | `IntrusiveList<T>` | `util/linked_list.rs` |
| `zenio/detail/time_source.h/.cpp` | `Clock`、`SteadyClock`、`TimeSource` | `time/clock.rs`、`runtime/time/source.rs` |
| `zenio/detail/time_wheel.h/.cpp` | `TimerWheel`、`TimerLevel`、`TimerExpiration` | `runtime/time/wheel/mod.rs`、`runtime/time/wheel/level.rs` |
| `zenio/detail/timer.h/.cpp` | `TimerEntry`、`TimerDriver` | `runtime/time/entry.rs`、`runtime/time/mod.rs` |

都是内部头文件, 不导出, 不标 `ZENIO_EXPORT`。命名按 `docs/coding-style.md`: 类名 PascalCase, 成员变量加下划线后缀, 常量加 `k` 前缀。

### 2.2 时间源

```cpp
namespace zenio::detail {

using TimePoint = std::chrono::steady_clock::time_point;

/**
 * @brief 取当前时间的接口。定时器只通过它看时间
 */
class Clock {
public:
    virtual ~Clock() = default;

    [[nodiscard]] virtual TimePoint Now() const noexcept = 0;
};

//! @brief 真实时钟, 生产环境用
class SteadyClock final : public Clock {
public:
    [[nodiscard]] TimePoint Now() const noexcept override;
};

/**
 * @brief 把时刻换算成 tick (毫秒)
 * @note tick 从时间源创建时算起
 */
class TimeSource {
public:
    explicit TimeSource(const Clock& clock) noexcept;

    /**
     * @brief 把到期时刻换算成 tick, 向上取整到下一毫秒
     */
    [[nodiscard]] std::uint64_t DeadlineToTick(TimePoint deadline) const noexcept;

    [[nodiscard]] std::uint64_t               InstantToTick(TimePoint t) const noexcept;
    [[nodiscard]] std::chrono::milliseconds   TickToDuration(std::uint64_t tick) const noexcept;
    [[nodiscard]] std::uint64_t               Now() const noexcept;

private:
    const Clock* clock_;
    TimePoint    start_;
};

}  // namespace zenio::detail
```

### 2.3 条目

```cpp
namespace zenio::detail {

//! @brief 时间轮里最多能表示的 tick 数。超过它的定时器被硬塞进最高一层
inline constexpr std::uint64_t kTimerMaxDuration = std::uint64_t{1} << 36;

//! @brief 条目已经结束 (触发过, 或者从未登记)
inline constexpr std::uint64_t kTimerStateDeregistered = ~std::uint64_t{0};
//! @brief 条目在待触发队列里
inline constexpr std::uint64_t kTimerStatePendingFire = kTimerStateDeregistered - 1;
//! @brief 特殊状态值里最小的那个。小于它的值都是真实的到期 tick
inline constexpr std::uint64_t kTimerStateMinValue = kTimerStatePendingFire;

/**
 * @brief 一个定时器条目, 内嵌在等待它的 awaiter 里
 * @note 登记期间地址不能变: 它被挂在侵入式链表上
 */
class TimerEntry {
public:
    TimerEntry() noexcept;

    //! @brief 已经触发或者已经取消时返回 true
    [[nodiscard]] bool is_elapsed() const noexcept;

private:
    friend class TimerDriver;
    friend class TimerWheel;

    // 以下三项只在持有驱动锁时访问
    IntrusiveListHook             hook_;               //!< 所在槽或待触发队列的链接
    Waiter*                        waiter_;             //!< 登记上来的等待者
    bool                           fired_by_shutdown_;  //!< 是因为驱动关闭而被触发的

    // 只在持有驱动锁时访问
    std::atomic<std::uint64_t>     registered_when_;  //!< 登记时用的到期 tick, 用来找到它挂在哪个槽
    std::atomic<std::uint64_t>     state_;            //!< 真实到期 tick, 或者上面三个特殊值之一
};

}  // namespace zenio::detail
```

tokio 把 `state` 和等待者放在一个叫 `StateCell` 的结构里。zenio 的 `Waiter` 本身就是线程安全的, 所以不需要 `AtomicWaker`, 等待者直接是一个裸指针, 由驱动锁保护。

### 2.4 驱动

```cpp
namespace zenio::detail {

enum class TimerInsertError {
    ELAPSED,   //!< 到期时刻已经过了
};

/**
 * @brief 定时器驱动: 时间轮、时间源, 外加登记和处理的入口
 * @note 线程安全。对应 tokio 的 runtime/time/mod.rs 里的 Handle 和 Inner
 */
class TimerDriver {
public:
    /**
     * @param clock    取时间的接口
     * @param unpark   唤醒"正在空闲等待的 worker"。登记了更早到期的定时器时要用
     * @param context  传给 unpark 的参数
     */
    TimerDriver(const Clock& clock, void (*unpark)(void* context), void* context);

    [[nodiscard]] std::uint64_t Now() const noexcept;
    [[nodiscard]] std::uint64_t DeadlineToTick(TimePoint deadline) const noexcept;

    /**
     * @brief 下一个到期定时器的 tick
     * @return 没有定时器时返回空
     */
    [[nodiscard]] std::optional<std::uint64_t> NextExpiration() const;

    /**
     * @brief 登记条目, 并把等待者挂上去
     * @return true 表示条目已经结束 (到期的时刻已经过了, 或者驱动已关闭), 调用方不用挂起
     * @note 调用方独占 entry, 不能有别的线程同时操作它
     */
    bool Wait(TimerEntry& entry, std::uint64_t tick, Waiter& waiter);

    /**
     * @brief 取消登记并结束这个条目, 然后唤醒它的等待者
     * @note 给 awaiter 的 stop callback 用: 协程正挂在这个定时器上, 要叫醒它
     */
    void Cancel(TimerEntry& entry);

    /**
     * @brief 取消登记并结束这个条目, 不唤醒它的等待者
     * @note entry 所在的内存被回收之前必须调用
     */
    void ClearEntry(TimerEntry& entry);

    /**
     * @brief 推进时间轮, 触发已到期的定时器
     * @note 任意线程可调, 内部有锁
     */
    void Process();

    /**
     * @brief 有没有已经到期的定时器。不加锁, 给忙碌的 worker 做廉价检查用
     */
    [[nodiscard]] bool HasDue() const noexcept;

    //! @brief 触发全部还没到期的定时器, 结果是取消。Runtime 析构时调用
    void Shutdown();

    [[nodiscard]] bool is_shutdown() const noexcept;

private:
    mutable std::mutex              mutex_;
    struct State;
    State                           state_;        //!< 被 mutex_ 保护: next_wake 和时间轮
    std::atomic<bool>               is_shutdown_;
    std::atomic<std::uint64_t>      next_wake_;    //!< 发布出去的副本, 0 表示没有定时器
    TimeSource                      time_source_;
    void                           (*unpark_)(void* context);
    void*                           context_;
};

}  // namespace zenio::detail
```

`Wait` 是 zenio 自己加的: tokio 的 `reregister` 只管登记, 挂等待者是 `Sleep` 的 `poll` 另做的。zenio 把两步合成一次加锁, 见 4.7。这是假设 (A15)。

## 3. 内部数据结构

### 3.1 时间轮

```cpp
namespace zenio::detail {

//! @brief 层数。每层 64 个槽。6 层能以 1 毫秒的精度覆盖约 2 年
inline constexpr std::size_t kTimerNumLevels = 6;
//! @brief 每层的槽数: 2 的 6 次方
inline constexpr std::size_t kTimerBitsPerLevel = 6;
inline constexpr std::size_t kTimerSlotsPerLevel = std::size_t{1} << kTimerBitsPerLevel;

//! @brief 某一层的某一个槽需要被处理的时刻
struct TimerExpiration {
    std::size_t    level;      //!< 层
    std::size_t    slot;       //!< 槽
    std::uint64_t  deadline;   //!< 这个槽需要被处理的 tick
};

class TimerWheel {
public:
    TimerWheel();

    /**
     * @brief 把条目放进时间轮
     * @return 成功时返回它的到期 tick。已经到期时返回 ELAPSED, 调用方直接触发
     */
    Result<std::uint64_t, TimerInsertError> Insert(TimerEntry& entry);
    void                                    Remove(TimerEntry& entry);

    /**
     * @brief 推进到 now, 顺便把到期的槽逐层下沉。每调用一次返回一个该触发的条目
     * @return 没有该触发的条目时返回空
     */
    TimerEntry* Poll(std::uint64_t now);

    //! @brief 下一个需要处理的槽
    [[nodiscard]] std::optional<TimerExpiration> NextExpiration() const;

private:
    [[nodiscard]] std::size_t LevelFor(std::uint64_t when) const;

    void ProcessExpiration(const TimerExpiration& expiration);

    std::uint64_t elapsed_;                 //!< 时间轮创建以来经过的毫秒数
    TimerLevel    levels_[kTimerNumLevels];
    IntrusiveList<TimerEntry> pending_;     //!< 已到期、等着被触发的条目
};

}  // namespace zenio::detail
```

六层各自的范围 (来自 tokio `Driver` 的文档):

| 层 | 每个槽覆盖 | 整层覆盖 |
|----|------------|----------|
| 0 | 1 毫秒 | 64 毫秒 |
| 1 | 64 毫秒 | 约 4 秒 |
| 2 | 约 4 秒 | 约 4 分钟 |
| 3 | 约 4 分钟 | 约 4 小时 |
| 4 | 约 4 小时 | 约 12 天 |
| 5 | 约 12 天 | 约 2 年 |

三个辅助函数, 都是从 tokio 原样搬过来的:

```cpp
//! @brief 一个槽覆盖的毫秒数
std::uint64_t SlotRange(std::size_t level);     // 1 << (kTimerBitsPerLevel * level)
//! @brief 一整层覆盖的毫秒数
std::uint64_t LevelRange(std::size_t level);    // 1 << (kTimerBitsPerLevel * (level + 1))
//! @brief 到期 tick 落在这一层的哪个槽
std::size_t SlotFor(std::uint64_t when, std::size_t level);   // (when >> (kTimerBitsPerLevel * level)) % kTimerSlotsPerLevel

//! @brief 到期 tick 应该放进哪一层。tokio 的 level_for
inline std::size_t LevelFor(std::uint64_t elapsed, std::uint64_t when) {
    constexpr std::uint64_t kSlotMask = kTimerSlotsPerLevel - 1;
    // 把低位或上掩码, 限制前导零的个数
    const std::uint64_t masked = (elapsed ^ when) | kSlotMask;
    if (masked >= kTimerMaxDuration) {
        return kTimerNumLevels - 1;   // 太远了, 硬塞进最高层
    }
    return static_cast<std::size_t>(std::bit_width(masked) - 1) / kTimerBitsPerLevel;
}
```

tokio 用 `masked.ilog2()`, 对应 C++20 的 `std::bit_width(masked) - 1`。

### 3.2 每一层

```cpp
class TimerLevel {
public:
    explicit TimerLevel(std::size_t level);

    //! @brief 这一层下一个需要处理的槽。返回值的 deadline 一定不早于 now
    [[nodiscard]] std::optional<TimerExpiration> NextExpiration(std::uint64_t now) const;

    void                  AddEntry(TimerEntry& entry);
    void                  RemoveEntry(TimerEntry& entry);
    IntrusiveList<TimerEntry> TakeSlot(std::size_t slot);

private:
    [[nodiscard]] std::optional<std::size_t> NextOccupiedSlot(std::uint64_t now) const;

    std::size_t                level_;
    std::uint64_t              occupied_;    //!< 位图: 哪些槽里有条目, 最低位是 0 号槽
    IntrusiveList<TimerEntry>  slots_[kTimerSlotsPerLevel];
};
```

位图让"找下一个非空槽"不用扫描。两个函数的步骤 (tokio `level.rs`):

**`NextExpiration(now)`**

1. `slot = NextOccupiedSlot(now)`, 为空就返回空。
2. `level_start = now & ~(LevelRange(level) - 1)`, 即把 now 低位清零, 得到当前这一圈的起点。
3. `deadline = level_start + slot * SlotRange(level)`。
4. 如果 `deadline <= now`: 只可能发生在最高层。最高层的槽当环用, 算出来的槽其实在下一圈, 所以 `deadline += LevelRange(level)`。

**`NextOccupiedSlot(now)`**

1. `occupied_ == 0` 返回空。
2. `now_slot = ((now / SlotRange(level)) % 64) + 1`。加 1 是为了跳过 now 所在的槽: 那里面的条目可能要等整整一圈。
3. 把 `occupied_` 循环右移 `now_slot` 位, 数末尾的零, 得到 `zeros`。
4. `slot = (zeros + now_slot) % 64`。

C++20 里循环右移是 `std::rotr`, 数末尾的零是 `std::countr_zero`, 都在 `<bit>` 里。

### 3.3 条目的状态

`TimerEntry::state_` 用一个 64 位值表示三种情况:

| 取值 | 含义 | 条目在哪里 |
|------|------|------------|
| 小于 `kTimerStateMinValue` | 已登记, 值就是真实的到期 tick | 时间轮的某个槽里 |
| `kTimerStatePendingFire` | 已到期, 等着被触发 | `TimerWheel::pending_` 里 |
| `kTimerStateDeregistered` | 已结束: 触发过, 或者从未登记 | 不在任何链表里 |

`registered_when_` 是另一回事: 条目放进时间轮时用的到期 tick, 用来算出它挂在哪个槽, 取消时才能找到它。条目在 `pending_` 里时它等于 `kTimerStateDeregistered`。

为什么要有两个时间 (tokio `entry.rs` 的 "Registered vs true timeouts"): tokio 支持不拿锁把定时器往后推, 只改 `state_`, 时间轮里的位置不动, 等驱动处理到那个槽时再发现并重新安置。v1 不移植这个功能, 但两个字段都保留, 状态机的形状和 tokio 一致。

状态只能这样走, 不能回头:

```
已结束 --登记--> 已登记 --到期--> 待触发 --触发--> 已结束
   ^                 |                            |
   |                 +------ 取消 ----------------+
   +---------------- 关闭: 直接从"已登记"到"已结束" --+
```

以下操作只能在持有驱动锁时调用, 步骤照 tokio `StateCell`:

| 操作 | 步骤 |
|------|------|
| `SetExpiration(tick)` | `state_ = tick`, `registered_when_ = tick` |
| `SyncWhen()` | `registered_when_ = state_`, 返回它 |
| `MarkPending(not_after)` | 循环: `state_ > not_after` 就返回失败并带上 `state_`; 否则 CAS 把 `state_` 换成 `kTimerStatePendingFire`, 成功后 `registered_when_ = kTimerStateDeregistered`。CAS 失败就用读到的值重试 |
| `Fire()` | `state_` 已经是 `kTimerStateDeregistered` 就什么都不做; 否则 `state_ = kTimerStateDeregistered` (release), 取走 `waiter_` 并返回它 |
| `MightBeRegistered()` | `state_ != kTimerStateDeregistered`, relaxed 读 |

## 4. 关键流程

标了"锁内"的步骤在持有 `TimerDriver::mutex_` 时执行。

### 4.1 登记 (tokio `reregister`, 加上挂等待者)

`TimerDriver::Wait(entry, tick, waiter)`:

1. 加锁。
2. 条目可能还在时间轮里, 和触发赛跑过: `MightBeRegistered()` 为真就 `wheel.Remove(entry)`。
3. 驱动已经关闭: `entry.Fire()` 后面补一个取消的结果, 解锁, 返回 true。
4. 否则 `entry.SetExpiration(tick)`, 然后 `wheel.Insert(entry)`:
   - 成功, 返回 `when`: 如果 `next_wake` 为空或者 `when < next_wake`, 调用 `unpark_(context_)`。它让正在空闲等待的 worker 醒过来, 重新计算阻塞多久。
   - `ELAPSED`: `entry.Fire()`, 解锁, 返回 true。调用方原地恢复。
5. 都没触发: `entry.waiter_ = &waiter`, 解锁, 返回 false。调用方接着挂起。

### 4.2 取消和结束 (tokio `clear_entry`)

两个入口, 前四步相同:

1. 加锁。
2. `MightBeRegistered()` 为真就 `wheel.Remove(entry)`。
3. `entry.Fire()`。状态变成已结束, 取走 `waiter_`。
4. 解锁。

- **`ClearEntry(entry)`**: 第 3 步取走的等待者直接丢掉, 不唤醒。awaiter 析构时调用。
- **`Cancel(entry)`**: 第 3 步取走的等待者, 解锁之后调用 `Waiter::Signal()`。awaiter 的 stop callback 调用。

`ClearEntry` 必须加锁, 哪怕条目看起来根本没登记。原因 (tokio `TimerEntry::cancel` 的注释): 调用之后这块内存马上要被回收另作他用; 驱动线程刚才对它做过大量 relaxed 的写, 以后别的线程还要往里写别的东西。两边之间需要一个双向的 acquire-release 屏障, 拿一次驱动锁正好提供了它。

为什么取消要分两个入口, 见第 6 节"不能用 stop callback 直接 `Signal`"。

### 4.3 处理到期 (tokio `process_at_time`)

`TimerDriver::Process()`:

1. `now = time_source_.Now()`。如果 `now < wheel.elapsed_`, 说明时间倒退了, 改用 `wheel.elapsed_`。见第 6 节。
2. 加锁。
3. 循环调用 `wheel.Poll(now)`, 每拿到一个条目就 `Fire()`, 把取走的等待者收进本地列表。
4. `next_wake = wheel.NextExpiration()`, 同时更新 `next_wake_`。
5. 解锁。
6. 对列表里的每个等待者调用 `Waiter::Signal()`。

第 6 步必须在解锁之后: 被唤醒的协程可能立刻去登记新的定时器, 那要拿同一把锁。

### 4.4 推进时间轮 (tokio `Wheel::poll`)

```
循环:
    pending_ 非空: 取下一个, 返回它
    next = NextExpiration()
    next 存在且 next.deadline <= now: ProcessExpiration(next); elapsed_ = next.deadline
    否则: elapsed_ = max(elapsed_, now); 跳出循环
最后再看一眼 pending_, 非空就取一个返回, 否则返回空
```

一次调用最多返回一个条目, 但会把沿途到期的槽都处理掉。

### 4.5 处理一个槽 (tokio `process_expiration`)

1. 把这个槽的整条链表取下来。必须先全部取下来再处理, 见第 6 节。
2. 逐个取出条目, 调用 `MarkPending(槽的 deadline)`:
   - 成功: 条目确实到期了, 放进 `pending_`。
   - 失败, 带回真实 tick: 条目属于更靠下的某一层。用 `LevelFor(deadline, 真实 tick)` 算出层数, `AddEntry` 放回去。

第 0 层处理到的条目, `registered_when_` 一定等于槽的 deadline, debug 构建下断言。

### 4.6 空闲等待时的组合 (tokio `Driver::park_internal`)

这一段不在定时器里, 在调度器的空闲等待路径里。列在这里是因为它是移植的一部分, 结构照 tokio:

1. 向定时器驱动要 `next = NextExpiration()`。
2. 有定时器: `duration = max(0, next - now)`, 按毫秒算。
   - 大于 0: 这就是阻塞的上限。
   - 等于 0: 定时器已经到期, 不阻塞, 非阻塞地等一次事件就行。
3. 没有定时器: 没有上限, 一直等到有事件。
4. 阻塞 (运行 Poller, 或者直接休眠)。
5. 醒来之后调用 `TimerDriver::Process()`。

tokio 这里还有一个由调用方给的上限 `limit`, 和定时器的上限取较小者。zenio 的空闲等待路径没有这种调用方给的上限, 所以省掉。完整步骤见 design/scheduler.md。

### 4.7 SleepAwaiter 怎么用它 (假设, A15)

`SleepAwaiter` 内嵌一个 `TimerEntry` 和一个 `Waiter`。tokio 的 `Sleep` 是第一次被 poll 时才登记, zenio 也这样, 理由见第 6 节。

- `await_ready()`: 第一次调用, 返回 false。
- `await_suspend(caller)`:
  1. 没有定时器驱动 (运行时没开 `EnableTime`) 就抛 `std::runtime_error`。
  2. `tick = driver.DeadlineToTick(deadline)`。
  3. `driver.Wait(entry_, tick, waiter_)` 返回 true: 返回 false, 原地恢复。
  4. 在当前令牌上注册 stop callback。
  5. 返回 `waiter_.TrySuspend(caller)`。驱动在解锁之后才 `Signal`, 而 `Waiter` 本来就允许 `Signal` 早于挂起, 见 design/waiter.md。
- `await_resume()`: 调用方被恢复。有取消标志就抛 `CancelledError`; 条目的 `fired_by_shutdown_` 为真就抛 `CancelledError(SHUTDOWN)`; 否则正常返回。
- stop callback 的函数体: 置 awaiter 上的取消标志, 然后调用 `driver.Cancel(entry_)`。定时器驱动在解锁之后唤醒等待者。
- 析构: `driver.ClearEntry(entry_)`。覆盖三种情况: 定时器正常触发之后; 协程在等待中被取消之后 (这时条目已经结束, `ClearEntry` 什么都不做); awaiter 没被 `co_await` 就销毁了。

`await_suspend` 里三步的顺序不能换: 先登记, 再注册 stop callback, 最后 `TrySuspend`。令牌已经取消时, 回调会在注册的那一刻在当前线程上执行, `Cancel` 把条目结束掉并 `Signal`, 接着 `TrySuspend` 返回 false, 协程原地恢复。见 design/waiter.md 4.2。

取消挂起的 `Sleep` 不经过 Poller, 因为定时器不在 Poller 里。

## 5. 不变量

- 条目已登记时: `state_` 小于 `kTimerStateMinValue`, `registered_when_` 等于它所在槽对应的 tick, 它正好在某个槽的链表里。
- 条目待触发时: `state_ == kTimerStatePendingFire`, `registered_when_ == kTimerStateDeregistered`, 它正好在 `pending_` 里。
- 条目已结束时: `state_ == kTimerStateDeregistered`, 它不在任何链表里。
- 每个条目恰好被结束一次, 不管登记、取消、触发以什么顺序交错。
- 时间轮只在持有驱动锁时被读写。`state_` 和 `registered_when_` 的 relaxed 内存序靠的就是这一点。
- `elapsed_` 只增不减。
- 同一层里 `NextExpiration` 给出的是当前这一圈里最早的槽, 而且比它高的层里没有更早的到期 (tokio 的 `no_expirations_before`, debug 断言)。
- 等待者的 `Signal` 一定在解锁之后。
- 登记期间的条目地址不变。
- 挂起中的定时器等待只能由定时器驱动唤醒: 触发、取消、关闭, 三种都经过驱动的锁。别的地方不许直接 `Signal` 一个定时器等待者。

## 6. 已知的坑

- **处理槽之前先把整条链表取下来**: 最高层的条目到期时刻超过 `kTimerMaxDuration` 时, 处理完要插回同一个槽。如果不先取下来, 会把它再处理一遍, 死循环。
- **`MarkPending` 用 CAS**: 它和"条目被往后推"赛跑时, 要么标记成功, 要么发现真实到期时刻还没到。已经触发、已经在队列里的条目不会被重复标记。
- **解锁之后才能唤醒**: 见 4.3。
- **取消必须拿驱动锁**: 见 4.2。这不是为了改时间轮, 是为了内存回收前的屏障。
- **时间倒退**: `Instant` 理论上单调, 但 tokio 在 Windows 主机上的 Linux 虚拟机里观察到过倒退。处理时发现 `now` 小于 `elapsed_` 就用 `elapsed_`。
- **向上取整到毫秒**: `DeadlineToTick` 先加 999999 纳秒再换算。阻塞时长算出来是 0 时不阻塞, 否则内核可能把微秒级的等待当成 0。
- **最高层是环**: 超过 `kTimerMaxDuration` 的定时器被 `LevelFor` 硬塞进最高层。所以 `NextExpiration` 在最高层算出"过去"的 deadline 时, 加上一层范围就是未来。
- **跳过 now 所在的槽**: `NextOccupiedSlot` 里那个加 1。最高层里 now 所在的槽可能有条目, 但那是一整圈之后的事。
- **`next_wake` 里 0 当 1 处理**: tokio 用 `NonZeroU64`, tick 为 0 的定时器记成 1。
- **relaxed 内存序的前提**: `state_` 和 `registered_when_` 只在"持有驱动锁"或者"独占这个条目"时访问。离开这两个前提, relaxed 就不成立。
- **不能在 awaiter 的构造函数里登记**: `SleepAwaiter` 是按值返回的临时对象, 从创建到被 `co_await` 之间可能被移动。第一次 `await_suspend` 时它已经在协程帧里, 地址不再变。tokio 的 `Sleep` 也是第一次 poll 才登记。
- **关闭**: `Shutdown()` 把时间推进到最大值, 触发全部还没到期的定时器, 结果是取消, 等待者收到 `CancelledError(SHUTDOWN)`。之后登记的新定时器立刻以同样方式结束。
- **唤醒不会丢**: 判断"要不要 unpark"在锁里做, 阻塞的 worker 记下 `next_wake` 也在锁里做, 两边看到的是同一个值。
- **不能用 stop callback 直接 `Signal` 定时器等待者**: 那样就有了两个唤醒方。等待者被取消唤醒之后, 协程在别的线程上运行并销毁 awaiter; 而驱动线程还拿着它的指针准备 `Signal`, 释放之后再用。所以取消必须走 `Cancel`, 在驱动的锁里完成交接。I/O 那边是同一个道理, 取消要经过 Poller。
- **`Waiter::Signal()` 里 `Schedule` 之后不能再碰 `this`**: `Schedule` 之后另一个 worker 可能立刻恢复协程并销毁 awaiter。见 design/waiter.md。

## 7. 验收测试

### 7.1 移植 tokio 的单元测试

用 `SteadyClock` 换成测试里手动拨动的时钟, 不需要真实等待。

`time_wheel` 的测试, 来自 `runtime/time/wheel/mod.rs` 和 `wheel/level.rs`:

- `test_level_for`: 各层各位置的 tick 都落在正确的层。
- `test_slot_for`: 各层各位置的 tick 都落在正确的槽。
- `next_occupied_slot_*` 一组: 空层、单个槽、选最近的、跳过 now 所在的槽、now 所在的槽是唯一非空槽、最后一个槽。
- `next_expiration_*` 一组: 返回槽的起点、最高层以下、最高层上、now 附近回绕一圈。
- `test_next_expiration_to_level_4`、`_to_level_5`、`_after_level_5`、`_after_level_5_twice`、`_to_level_5_and_after_level_5`、`_at_slot_5_of_the_top_level`: 高层条目逐层下沉的过程。

`timer` 的测试, 来自 `runtime/time/tests/mod.rs`:

- `single_timer`: 登记一个, 拨到到期, 处理, 等待者被触发。
- `drop_timer`: 登记后不处理就取消, 等待者不被触发, 时间轮里没有残留。
- `change_waker`: 换等待者之后触发的还是有效的那一个。zenio 里对应"同一个条目先后挂两个 `Waiter`", 视 A15 的最终形态决定保留还是改写。
- `instant_to_tick_max`: 超出范围的时长被钳到 `kTimerStateMinValue - 1`。

### 7.2 zenio 层面的测试 (M4)

- `Sleep(10ms)` 的实际等待不小于 10 毫秒, 不超过 10 毫秒加一个合理的调度误差。
- 一万个 `Sleep` 同时挂起, 到期时间各不相同, 全部被唤醒且顺序正确。
- 取消挂起的 `Sleep`, 协程收到 `CancelledError`, 时间轮里没有残留。
- 只开 `EnableTime`、不开 `EnableIo` 的运行时里 `Sleep` 能工作。
- 没开 `EnableTime` 的运行时里 `Sleep` 抛 `std::runtime_error`。
- 以上测试在 ASAN 下无泄漏。
