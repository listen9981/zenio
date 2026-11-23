# Runtime

状态: 草稿
细化程度: 对外接口已细化 (M1); 内部是概要 (M3 前细化, 多线程部分 M6 前细化)
依据: S5、S6、S9、S10、S11、S15、S22、S26; A1、A3、A9; O6

## 1. 职责与边界

Runtime 是 zenio 的所有者: 线程、调度器、Poller、游离任务的登记表都归它。

- **`Builder`** 决定 Runtime 的形态 (对应 `tokio::runtime::Builder`)。
- **`Runtime`** 拥有线程, 提供入口 `BlockOn` (对应 `tokio::runtime::Runtime`)。
- **`Handle`** 是指向 Runtime 的轻量句柄, 可拷贝, 可以跨线程使用 (对应 `tokio::runtime::Handle`)。

两种形态 (S6、S9 ~ S11):

- **`CurrentThread()`**: 调用线程运行单线程循环。调度器和 Poller 都在这个线程上, 没有额外线程。
- **`MultiThread(n)`**: n 个 worker 线程。Poller 是其中一个 worker 上的普通任务, 等待 I/O 时有一个 worker 睡在 Poller 里, 用户代码最多用 n-1 个线程。满载时 Poller 排队, 那个线程让给用户代码 (D11)。

Poller 只在整个 Runtime 没有可运行任务时才阻塞 (S10), 见 design/poller.md。

任务归属:

- `BlockOn` 的主任务。
- `JoinSet` 里 spawn 的任务, 归集合管理。
- 全局 `Spawn` / `SpawnAndSeize` 产生的游离任务, 由 Runtime 兜底。

线程安全:

- `Handle` 的拷贝、`Handle::Spawn`、`Abort` 可以在任意线程调用。
- `Runtime` 自己 (`BlockOn`、析构) 只能由拥有它的线程使用。v1 只支持调用一次 `BlockOn`。

不在 v1: `SpawnBlocking`、线程名等 Builder 选项、`shutdown_background`。

## 2. 对外接口

`zenio/runtime.h`:

```cpp
namespace zenio::runtime {

/**
 * @brief Handle::Enter() 的返回值, 把句柄登记为当前线程的上下文
 * @note 离开作用域后恢复之前的上下文
 */
class ZENIO_EXPORT EnterGuard {
public:
    EnterGuard(EnterGuard&& other) noexcept;
    EnterGuard& operator=(EnterGuard&& other) noexcept;
    EnterGuard(const EnterGuard&)            = delete;
    EnterGuard& operator=(const EnterGuard&) = delete;

    ~EnterGuard();

private:
    friend class Handle;

    EnterGuard() noexcept;

    detail::PromiseContext* previous_ = nullptr;
};

/**
 * @brief 指向运行时的句柄 (对应 tokio::runtime::Handle)
 * @note 可拷贝, 可跨线程使用
 */
class ZENIO_EXPORT Handle {
public:
    Handle() noexcept = default;

    /**
     * @brief 当前运行时的句柄 (对应 Handle::current)
     * @throws std::runtime_error 不在运行时上下文里
     */
    [[nodiscard]] static Handle Current();

    /**
     * @brief 当前运行时的句柄 (对应 Handle::try_current)
     * @return 不在运行时上下文里时返回空句柄
     */
    [[nodiscard]] static Handle TryCurrent() noexcept;

    [[nodiscard]] bool is_valid() const noexcept;

    /**
     * @brief 把句柄登记为当前线程的上下文 (对应 Handle::enter)
     * @note 作用域内可以在普通线程上调用需要上下文的同步函数, 如 TcpListener::Bind(addr)
     */
    [[nodiscard]] EnterGuard Enter() const;

    /**
     * @brief 在这个运行时上派发一个游离任务 (对应 Handle::spawn)
     * @note 不依赖当前上下文, 可以在非 Runtime 线程调用
     */
    template <class T>
    JoinHandle<T> Spawn(Task<T> task);

private:
    friend class Runtime;
    friend class Builder;

    std::shared_ptr<detail::RuntimeCore> core_;
};

class Runtime;

/**
 * @brief Runtime 的构造器 (对应 tokio::runtime::Builder)
 */
class ZENIO_EXPORT Builder {
public:
    //! @brief 单线程: 调用 BlockOn 的线程运行整个运行时
    [[nodiscard]] static Builder CurrentThread();

    /**
     * @brief 多线程: n 个 worker 线程
     * @throws std::invalid_argument n 小于 1
     */
    [[nodiscard]] static Builder MultiThread(int workers);

    //! @brief 开启 I/O 和信号
    Builder& EnableIo();
    //! @brief 开启定时器
    Builder& EnableTime();
    //! @brief 同时开启两者 (对应 enable_all)
    Builder& EnableAll();

    [[nodiscard]] Runtime Build();

private:
    int  workers_      = 0;
    bool enable_io_    = false;
    bool enable_time_  = false;
};

/**
 * @brief 运行时 (对应 tokio::runtime::Runtime)
 */
class ZENIO_EXPORT Runtime {
public:
    Runtime(Runtime&& other) noexcept;
    Runtime& operator=(Runtime&& other) noexcept;
    Runtime(const Runtime&)            = delete;
    Runtime& operator=(const Runtime&) = delete;

    /**
     * @brief Abort 所有任务并等它们结束, 然后 join 线程
     */
    ~Runtime();

    /**
     * @brief 运行主任务直到结束
     * @return 任务的产出; 任务抛了异常就在调用线程上重抛
     */
    template <class T>
    T BlockOn(Task<T> task);

    [[nodiscard]] Handle handle() const;

    //! @brief 在这个运行时上派发一个游离任务 (对应 Runtime::spawn)
    template <class T>
    JoinHandle<T> Spawn(Task<T> task);

private:
    friend class Builder;

    explicit Runtime(std::shared_ptr<detail::RuntimeCore> core) noexcept;

    std::shared_ptr<detail::RuntimeCore> core_;
};

}  // namespace zenio::runtime
```

说明:

- **默认值 (O6, 倾向与 tokio 一致)**: I/O 和定时器默认都不开启, 要显式打开。`MultiThread` 的 n 由调用方给出; 以后如果加不带参数的默认形态, worker 数取硬件并发数。
- **`TryCurrent()` 返回可判空的句柄**: 之前的概要写的是返回 `std::optional<Handle>`。细化时改成句柄自带 `is_valid()`, 用起来少一层解引用。两种都可行, 待审查。
- **`EnableAll()` 是这次细化新增的**, 对应 tokio 的 `enable_all`, 免得每个 Runtime 都要写两次。
- **`Builder` 不可拷贝**: 没有列出的拷贝和移动操作按 C++ 的默认规则处理, 用法是链式调用之后直接 `Build()`。
- **三个 `Spawn` 入口** (全局 `zenio::Spawn`、`Handle::Spawn`、`Runtime::Spawn`) 语义相同, 只是获得运行时的方式不同。它们是模板, 实现时共用一个非模板的内部函数, 头文件里只留模板包装。`BlockOn` 同理。
- **`EnableIo` / `EnableTime` 的规则 (A3)**: 两个选项各管各的 (S26)。`EnableIo` 决定是否创建 Poller, `EnableTime` 决定是否创建定时器驱动。没开 `EnableIo` 时 I/O 和信号操作抛 `std::runtime_error`; 没开 `EnableTime` 时定时器操作抛 `std::runtime_error`。

## 3. 内部数据结构 (概要)

| 类型 | 职责 |
|------|------|
| `Runtime` | 持有 `std::shared_ptr<detail::RuntimeCore>` |
| `runtime::Handle` | 持有同一个 `RuntimeCore` 的 `shared_ptr` |
| `detail::RuntimeCore` | 配置; 调度器; Poller (开了 `EnableIo` 才有); 定时器驱动 (开了 `EnableTime` 才有); 关闭用的 `stop_source`; 游离任务的登记表; 没人取的异常的处理器 |
| `Builder` | 三个选项 |

`RuntimeCore` 被 `Handle` 和 I/O 对象续命: Runtime 析构之后线程已经停了, 但这些对象引用的数据还在, 直到它们全部销毁。

单线程循环 (M3 细化): 取任务并执行; 取不到任务时进入空闲等待 (M4 起: 运行 Poller, 处理定时器, 见 design/scheduler.md 4.1); 所有任务都结束且主任务返回之后, `BlockOn` 返回。

## 4. 关键流程 (概要)

1. **Build**: 创建 `RuntimeCore`; `EnableIo` 决定是否创建 Poller, `EnableTime` 决定是否创建定时器驱动, 两者互不相干; 多线程形态下启动 n 个 worker 线程。
2. **BlockOn**: 把主任务登记为根任务 (它的令牌链接到 Runtime 的关闭令牌), 交给调度器, 等它结束, 取出产出或重抛异常。多线程形态下调用线程只等待, 不参与调度 (D8)。
3. **析构**: 通过关闭令牌取消所有任务, 等游离任务的数量归零, 关闭定时器驱动 (挂着的定时器都以取消结束, 见 design/timer.md), 停止 worker 线程并 join。
4. **没人取的异常 (S15)**: 游离任务的 `JoinHandle` 被丢弃之后, 任务抛出的异常交给 Runtime 的处理器。默认的处理器什么都不做, 即丢弃。配置入口待定 (A9)。

## 5. 不变量

- 一个 `RuntimeCore` 同一时刻只被一个 `Runtime` 对象拥有。
- Runtime 析构返回时, 它启动的线程都已 join, 所有任务都已结束。
- 不在运行时上下文里时, `Handle::Current()` 抛 `std::runtime_error`, `Handle::TryCurrent()` 返回空句柄。
- 没开 `EnableIo` 的 Runtime 里, I/O 和信号操作抛 `std::runtime_error`; 没开 `EnableTime` 的 Runtime 里, 定时器操作抛 `std::runtime_error`。

## 6. 已知的坑

- **`Runtime` 在 `BlockOn` 运行期间被析构**是未定义行为。v1 不支持从别的线程关闭运行时。
- **主任务不可取消**: v1 没有取消主任务的入口。
- **I/O 对象不能比它引用的数据活得久**: I/O 对象持有 `RuntimeCore` 的 `shared_ptr`, 所以数据会留着, 但线程已经停了, 这时再 `co_await` 它的操作是未定义行为。
- **`EnterGuard` 不可拷贝**, 也不能在作用域结束前手工解除。

## 7. 验收测试

M3 的任务书里给出。概要:

- `Builder::CurrentThread().Build()` 得到的 Runtime 能 `BlockOn` 一个立即返回的任务并取到产出。
- `BlockOn` 把主任务的异常重抛给调用方。
- 不在上下文里时 `Handle::Current()` 抛 `std::runtime_error`, `TryCurrent()` 返回空句柄。
- `Handle::Enter()` 的作用域内 `Handle::Current()` 成功, 作用域外恢复原状。
- I/O 没开启时 `TcpListener::Bind` 抛 `std::runtime_error`。
- 只开 `EnableTime` 的 Runtime 里 `Sleep` 能工作, `TcpListener::Bind` 抛异常; 只开 `EnableIo` 的 Runtime 里反过来。
