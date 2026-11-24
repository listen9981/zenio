# Runtime

状态: 草稿
细化程度: `CurrentThread` 形态全部已细化 (M3); Poller、定时器驱动、游离任务、多线程的部分是概要 (M4 到 M6 细化)
依据: S5、S6、S9、S10、S11、S15、S22、S26; A1、A3、A9、A19、A20、A21; O6

## 1. 职责与边界

Runtime 是 zenio 的所有者: 线程、调度器、Poller、游离任务的登记表都归它。

- **`Builder`** 决定 Runtime 的形态 (对应 `tokio::runtime::Builder`)。
- **`Runtime`** 拥有线程, 提供入口 `BlockOn` (对应 `tokio::runtime::Runtime`)。
- **`Handle`** 是指向 Runtime 的轻量句柄, 可拷贝, 可以跨线程使用 (对应 `tokio::runtime::Handle`)。

两种形态 (S6、S9 ~ S11):

- **`CurrentThread()`**: 调用线程运行单线程循环。调度器和 Poller 都在这个线程上, 没有额外线程。
- **`MultiThread(n)`**: n 个 worker 线程。Poller 是其中一个 worker 上的普通任务, 等待 I/O 时有一个 worker 睡在 Poller 里, 用户代码最多用 n-1 个线程。满载时 Poller 排队, 那个线程让给用户代码 (D11)。

M3 只实现 `CurrentThread`。`MultiThread(n)` 能构造, `Build()` 抛 `std::runtime_error`, M6 实现 (A20)。

Poller 只在整个 Runtime 没有可运行任务时才阻塞 (S10), 见 design/poller.md。

任务归属:

- `BlockOn` 的主任务。
- `JoinSet` 里 spawn 的任务, 归集合管理。
- 全局 `Spawn` / `SpawnAndSeize` 产生的游离任务, 由 Runtime 兜底。

线程安全:

- `Handle` 的拷贝、`Handle::Spawn`、`Abort` 可以在任意线程调用。
- `Runtime` 自己 (`BlockOn`、析构) 只能由拥有它的线程使用。同一个 Runtime 上 `BlockOn` 不能重入 (A19)。

不在 v1: `SpawnBlocking`、线程名等 Builder 选项、`shutdown_background`。

## 2. 对外接口

`zenio/runtime.h`:

```cpp
namespace zenio::detail {

class RuntimeCore;

}  // namespace zenio::detail

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

    EnterGuard() noexcept = default;

    //! @brief 还原, 只有正在生效的时候才做
    void Reset() noexcept;

    detail::PromiseContext* previous_ = nullptr;    //!< 进入之前的上下文, 可以是空指针
    bool                    active_   = false;      //!< 正在生效
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

    //! @brief 两个句柄指向同一个运行时
    [[nodiscard]] bool operator==(const Handle& other) const noexcept;

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

    explicit Handle(std::shared_ptr<detail::RuntimeCore> core) noexcept;

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
    int  workers_     = 0;
    bool enable_io_   = false;
    bool enable_time_ = false;
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
     * @throws std::runtime_error 这个线程上已经有上下文, 或者 BlockOn 重入
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

    //! @brief BlockOn 的非模板部分: 把 task 当作根任务运行, 直到没事可做
    void RunRoot(std::coroutine_handle<> task);

    std::shared_ptr<detail::RuntimeCore> core_;
};

template <class T>
T Runtime::BlockOn(Task<T> task) {
    const auto handle = detail::TaskAccess::Handle(task);
    assert(handle && "BlockOn 收到了无效的 Task");
    RunRoot(handle);
    assert(handle.done() && "RunRoot 返回时主任务应当已经结束");
    return detail::TakeTaskResult<T>(handle.promise().storage_);
}

}  // namespace zenio::runtime
```

相对 M1 的改动, 都不涉及公开的函数签名: `Handle` 多了 `operator==` (A21)、一个私有的带参构造函数; `EnterGuard` 多了私有成员 `active_` 和私有函数 `Reset`; `Runtime` 多了私有的非模板函数 `RunRoot`, `BlockOn` 的定义从头文件外的声明变成了头文件里的模板。`Handle::Spawn`、`Runtime::Spawn` 在 M5 之前只有声明。

说明:

- **默认值 (O6, 倾向与 tokio 一致)**: I/O 和定时器默认都不开启, 要显式打开。
- **线程数 (A20)**: `workers_` 为 0 表示单线程。`CurrentThread()` 返回的就是一个默认构造的 `Builder`, 所以默认构造的 `Builder` 等同 `CurrentThread()`。`MultiThread(n)` 把 `workers_` 设成 n。以后如果加不带参数的默认形态, worker 数取硬件并发数。
- **`TryCurrent()` 返回可判空的句柄**: 细化时定成句柄自带 `is_valid()`, 用起来少一层解引用。
- **`operator==` (A21)**: 比较两个句柄是不是指向同一个运行时。tokio 的 `Handle` 没有, zenio 补上是为了能断言"协程里拿到的就是这个运行时"。
- **`Builder` 不可拷贝**: 没有列出的拷贝和移动操作按 C++ 的默认规则处理, 用法是链式调用之后直接 `Build()`。
- **三个 `Spawn` 入口** (全局 `zenio::Spawn`、`Handle::Spawn`、`Runtime::Spawn`) 语义相同, 只是获得运行时的方式不同。它们是模板, 实现时共用一个非模板的内部函数, 头文件里只留模板包装。`BlockOn` 同理: 模板里只有取句柄和取结果两步, 其余在非模板的 `Runtime::RunRoot` 里。
- **`EnableIo` / `EnableTime` 的规则 (A3)**: 两个选项各管各的 (S26)。`EnableIo` 决定是否创建 Poller, `EnableTime` 决定是否创建定时器驱动。没开 `EnableIo` 时 I/O 和信号操作抛 `std::runtime_error`; 没开 `EnableTime` 时定时器操作抛 `std::runtime_error`。M3 里 `Build()` 只是把两个选项记进 `RuntimeCore`, Poller 和定时器驱动在 M4 创建。

## 3. 内部数据结构

| 类型 | 职责 |
|------|------|
| `Runtime` | 持有 `std::shared_ptr<detail::RuntimeCore>` |
| `runtime::Handle` | 持有同一个 `RuntimeCore` 的 `shared_ptr` |
| `detail::RuntimeCore` | 配置; 调度器; Poller (开了 `EnableIo` 才有, M4); 定时器驱动 (开了 `EnableTime` 才有, M4); 关闭用的 `stop_source`; 根上下文; 游离任务的登记表 (M5); 没人取的异常的处理器 (M5) |
| `detail::RuntimeOptions` | `Builder::Build()` 交给 `RuntimeCore` 的三个选项 |
| `Builder` | 三个选项 |

`RuntimeCore` 总是由 `shared_ptr` 持有, 并且继承 `std::enable_shared_from_this`。`Handle` 和 I/O 对象靠它续命: Runtime 析构之后线程已经停了, 但这些对象引用的数据还在, 直到它们全部销毁。

`zenio/detail/runtime_core.h` (M3 的部分):

```cpp
namespace zenio::detail {

//! @brief Builder::Build() 交给 RuntimeCore 的配置
struct RuntimeOptions {
    int  workers     = 0;
    bool enable_io   = false;
    bool enable_time = false;
};

/**
 * @brief Runtime 的内部对象: 配置、调度器, 以后还有 Poller 和定时器驱动
 * @note 总是由 shared_ptr 持有: Handle 和 I/O 对象靠它续命
 */
class RuntimeCore : public std::enable_shared_from_this<RuntimeCore> {
public:
    explicit RuntimeCore(const RuntimeOptions& options);

    RuntimeCore(const RuntimeCore&)            = delete;
    RuntimeCore& operator=(const RuntimeCore&) = delete;

    //! @brief 把一个可恢复的协程入队, 见 design/scheduler.md 4.2
    void Schedule(Runnable runnable) { scheduler_.Schedule(runnable); }

    //! @brief 把 root 当作根任务运行, 直到没事可做
    void RunRoot(std::coroutine_handle<> root);

    [[nodiscard]] bool            io_enabled() const noexcept { return io_enabled_; }
    [[nodiscard]] bool            time_enabled() const noexcept { return time_enabled_; }
    [[nodiscard]] Scheduler&      scheduler() noexcept { return scheduler_; }
    [[nodiscard]] PromiseContext& root_context() noexcept { return root_context_; }

private:
    bool            io_enabled_;       //!< 记下的选项, M4 用
    bool            time_enabled_;     //!< 记下的选项, M4 用
    bool            blocking_ = false; //!< BlockOn 正在运行
    std::stop_source shutdown_source_; //!< 关闭令牌的来源
    PromiseContext  root_context_;     //!< 根上下文: 令牌是关闭令牌, core 指向自己
    Scheduler       scheduler_;
};

}  // namespace zenio::detail
```

根上下文是 `BlockOn` 的主任务和 `Handle::Enter()` 用的那一份, 它属于 `RuntimeCore`, 所以活得更久。

## 4. 关键流程

### 4.1 Build

M3:

```cpp
// runtime.cpp
Runtime Builder::Build() {
    if (workers_ != 0) {
        throw std::runtime_error("zenio: Builder::MultiThread 在 M6 实现");
    }
    detail::RuntimeOptions options;
    options.workers     = workers_;
    options.enable_io   = enable_io_;
    options.enable_time = enable_time_;
    return Runtime(std::make_shared<detail::RuntimeCore>(options));
}

// runtime_core.cpp
RuntimeCore::RuntimeCore(const RuntimeOptions& options)
    : io_enabled_(options.enable_io), time_enabled_(options.enable_time) {
    root_context_.token = shutdown_source_.get_token();
    root_context_.core  = this;
}
```

`Builder::MultiThread(n)` 在 n 小于 1 时抛 `std::invalid_argument`。

M4 起 `Build` 还要创建 Poller 和定时器驱动, 两者互不相干; M6 起多线程形态下启动 n 个 worker 线程。那句 `std::runtime_error` 在 M6 删掉。

### 4.2 BlockOn

```
Runtime::BlockOn(task)                    模板, 头文件里
    handle = TaskAccess::Handle(task)     取主任务的句柄
    RunRoot(handle)                       非模板部分
    TakeTaskResult<T>(handle.promise().storage_)   取产出, 或者重抛
```

`Runtime::RunRoot` 转给 `RuntimeCore::RunRoot`:

```cpp
void RuntimeCore::RunRoot(std::coroutine_handle<> root) {
    if (CurrentContext() != nullptr || blocking_) {
        throw std::runtime_error("zenio: BlockOn 不能在运行时上下文里调用, 也不能重入");
    }
    FlagGuard guard(blocking_);
    scheduler_.Schedule(Runnable{root, &root_context_});
    while (scheduler_.RunOne()) {
    }
}
```

`FlagGuard` 是 `runtime_core.cpp` 里的一个文件内辅助类: 构造时把一个 `bool&` 置为 true, 析构时置回 false。

逐步说明:

1. **进入条件** (A19)。线程上已经有上下文, 说明调用方正在某个 zenio 协程里, 或者在 `Handle::Enter()` 的作用域里。这时再进循环, 会把正在等待的任务调度到它自己的调用栈上, 是错的, 所以拒绝。tokio 在同样的情况下 panic。`blocking_` 挡住同一个 Runtime 上的重入: 在协程里再调这个 Runtime 的 `BlockOn` 时, 两个条件都成立。上一次 `BlockOn` 返回之后可以再调。
2. **投递根任务**。上下文给的是运行时的根上下文, 不是主任务自己的: 主任务还没启动, 它的 promise 里什么都没有 (design/scheduler.md 第 2 节)。启动的时候 `InitialAwaiter` 把根上下文复制进主任务的 promise, 所以主任务的令牌就是运行时的关闭令牌。v1 没有取消主任务的入口, 这个令牌在 `BlockOn` 运行期间不会被触发。
3. **循环**。队列空了就退出, 见 design/scheduler.md 4.4。M4 起这里变成空闲等待的循环, 退出条件改成主任务结束。
4. **取结果**。回到模板: 断言主任务已经结束, `TakeTaskResult<T>` 取走产出, 存的是异常就重抛。`task` 是 `BlockOn` 的参数, 函数返回或者异常展开时析构, 主任务的帧在那里销毁。
5. **还原**。循环里每运行一个协程, 结束后都把线程上的上下文还原成运行之前的 (design/scheduler.md 4.3), 所以 `BlockOn` 返回之后线程上没有上下文。

多线程形态 (M6) 下调用线程只等待, 不参与调度 (D8)。

### 4.3 析构

M3: 没有游离任务, 没有线程。`~Runtime()` 释放 `core_` 这个 `shared_ptr`, 没有别人持有 `RuntimeCore` 时它被销毁。队列里在 `BlockOn` 返回之后不会剩东西 (design/scheduler.md 4.4)。

M4 起: 关闭定时器驱动 (挂着的定时器都以取消结束, 见 design/timer.md)。M5 起: 通过关闭令牌取消所有任务, 等游离任务的数量归零。M6 起: 停止 worker 线程并 join。

### 4.4 Handle

```cpp
Handle Handle::Current() {
    Handle handle = TryCurrent();
    if (!handle.is_valid()) {
        throw std::runtime_error("zenio: 不在运行时上下文里");
    }
    return handle;
}

Handle Handle::TryCurrent() noexcept {
    const detail::PromiseContext* context = detail::CurrentContext();
    if (context == nullptr || context->core == nullptr) {
        return Handle();
    }
    return Handle(context->core->shared_from_this());
}

EnterGuard Handle::Enter() const {
    assert(core_ && "空句柄上不能调用 Enter()");
    EnterGuard guard;
    guard.previous_ = detail::CurrentContext();
    guard.active_   = true;
    detail::SetCurrentContext(&core_->root_context());
    return guard;
}
```

- **`Current()` / `TryCurrent()`**: 读线程上的上下文。有上下文并且它带着运行时, 就返回指向那个运行时的句柄。协程启动之后 `core` 一定有值; 为空只可能是协程在运行时外面被启动的 (design/task.md 2.2 第 5 条), 这时不算在上下文里。
- **`Enter()`**: 把根上下文装到线程上, 返回的 `EnterGuard` 记下之前的上下文。作用域内 `CurrentContext()->core` 就是这个运行时, 需要上下文的同步函数能用 (M4 的 `TcpListener::Bind(addr)`)。`EnterGuard` 析构时还原。在协程里 `Enter()` 另一个运行时的句柄也可以, 离开作用域之后回到原来那个。
- **`EnterGuard` 的移动**: 移动之后源对象不再生效 (它的 `active_` 变成 false), 由目标对象负责还原。`Reset` 只在 `active_` 为 true 时还原。需要这个标志是因为"之前的上下文"本身就可以是空指针, 不能拿空指针表示"没有生效"。
- **句柄比 Runtime 活得久**: `Handle` 持有 `RuntimeCore` 的 `shared_ptr`, `Runtime` 对象销毁之后句柄仍然有效, `is_valid()` 仍是 true。这时运行时的线程已经停了 (M6 起), 再往上派发任务是未定义行为, 见第 6 节。
- **在 `Enter()` 的作用域里不能 `BlockOn`**: 线程上有上下文, 被 4.2 的进入条件挡住。

### 4.5 没人取的异常 (S15, M5)

游离任务的 `JoinHandle` 被丢弃之后, 任务抛出的异常交给 Runtime 的处理器。默认的处理器什么都不做, 即丢弃。配置入口待定 (A9)。

## 5. 不变量

- 一个 `RuntimeCore` 同一时刻只被一个 `Runtime` 对象拥有。
- Runtime 析构返回时, 它启动的线程都已 join, 所有任务都已结束。
- `BlockOn` 运行期间线程上有且只有正在运行的那个协程的上下文; `BlockOn` 返回之后线程上没有上下文。
- 不在运行时上下文里时, `Handle::Current()` 抛 `std::runtime_error`, `Handle::TryCurrent()` 返回空句柄。
- `Handle::Current()` 得到的句柄与这个运行时的 `handle()` 相等。
- `Handle::Enter()` 的作用域内 `Handle::Current()` 不抛; 作用域结束之后线程上的上下文恢复成进入之前的。
- 没开 `EnableIo` 的 Runtime 里, I/O 和信号操作抛 `std::runtime_error`; 没开 `EnableTime` 的 Runtime 里, 定时器操作抛 `std::runtime_error`。

## 6. 已知的坑

- **`Runtime` 在 `BlockOn` 运行期间被析构**是未定义行为。v1 不支持从别的线程关闭运行时。
- **在协程里调 `BlockOn`**: 被进入条件挡住, 抛 `std::runtime_error`。这是最容易犯的错: 想在协程里跑另一个任务, 用 `co_await`, 不要用 `BlockOn`。
- **主任务不可取消**: v1 没有取消主任务的入口。
- **I/O 对象不能比它引用的数据活得久**: I/O 对象持有 `RuntimeCore` 的 `shared_ptr`, 所以数据会留着, 但线程已经停了, 这时再 `co_await` 它的操作是未定义行为。
- **`EnterGuard` 不可拷贝**, 也不能在作用域结束前手工解除。
- **空句柄上调用 `Enter()`** 是未定义行为, debug 构建下断言。
- **`BlockOn` 收到无效的 `Task`** (被移动过的) 是未定义行为, debug 构建下断言。

## 7. 验收测试

M3 的任务书里给出, 文件是 `tests/zenio/runtime_unittest.cpp`:

- `Builder::CurrentThread().Build()` 得到的 Runtime 能 `BlockOn` 一个立即返回的任务并取到产出, `int`、`void`、只可移动类型、引用都覆盖。
- `BlockOn` 把主任务的异常重抛给调用方, 类型和 `what()` 不变。
- `BlockOn` 返回之后 `Handle::TryCurrent()` 是空句柄, `CurrentContext()` 是空指针。
- 连续两次 `BlockOn` 都成功。
- 在协程里再调 `BlockOn` 抛 `std::runtime_error`; 内层被拒绝之后外层照常运行完。
- 在 `Handle::Enter()` 的作用域里调 `BlockOn` 抛 `std::runtime_error`。
- 不在上下文里时 `Handle::Current()` 抛 `std::runtime_error`, `TryCurrent()` 返回空句柄。
- 协程里 `Handle::Current()` 成功, 并且等于 `rt.handle()`; `TryCurrent()` 也相等。
- `Handle::Enter()`: 作用域内 `Handle::Current()` 成功且等于这个句柄; 作用域外恢复原状。嵌套的 `Enter()` 各自还原。
- `EnterGuard` 移动之后, 源对象析构不还原, 目标对象析构时还原。
- `Enter()` 一个在别的线程上创建的运行时, 离开作用域之后回到原状。
- `Runtime` 对象销毁之后它的 `Handle` 仍然有效。
- `operator==`: 同一个运行时的两个句柄相等, 不同运行时的不等, 空句柄与任何有效句柄不等。
- `Runtime` 移动之后新对象能 `BlockOn`。
- `Builder::MultiThread(0)` 抛 `std::invalid_argument`。
- M3 期间 `Builder::MultiThread(2).Build()` 抛 `std::runtime_error`。M6 起这一条删掉。
- `Builder::CurrentThread().EnableIo().EnableTime().Build()`、`EnableAll()`、默认构造的 `Builder` 都能得到能 `BlockOn` 的 Runtime。
- 多个线程各自有自己的 Runtime, 同时 `BlockOn` 互不干扰。

M4 起:

- I/O 没开启时 `TcpListener::Bind` 抛 `std::runtime_error`。
- 只开 `EnableTime` 的 Runtime 里 `Sleep` 能工作, `TcpListener::Bind` 抛异常; 只开 `EnableIo` 的 Runtime 里反过来。
