# JoinSet

状态: 草稿
细化程度: 对外接口已细化 (M1); 内部是概要 (M5 前细化)
依据: S5、S6、S13、S16; A2; D10

## 1. 职责与边界

`JoinSet<T>` 管理一组都产出 `T` 的任务, 对应 `tokio::task::JoinSet<T>`。

- 任务归集合管理: `JoinAll` / `AbortAll` 只作用于集合里的任务, 全局 `Spawn` 产生的任务不受影响。
- 构造时从线程局部存储取当前上下文, 所以必须在运行时上下文里构造。
- 不可拷贝, 不可移动: awaiter 持有指向集合的指针。

不在本文件: 单个任务怎么派发 (design/spawn.md)。`Select` 内部复用本文件的子任务管理, 见 design/select.md。

## 2. 对外接口

`zenio/join_set.h`:

```cpp
namespace zenio::detail {
template <class T> class JoinAllAwaiter;
template <class T> class JoinNextAwaiter;
}

namespace zenio {

/**
 * @brief 一组由集合管理的任务 (对应 tokio::task::JoinSet<T>)
 * @note 必须在运行时上下文里构造
 */
template <class T>
class [[nodiscard]] JoinSet {
public:
    //! @brief JoinAll 的产出: T 为 void 时是 void (只收割), 否则是 std::vector<T>, 按 spawn 顺序排列
    using JoinAllOut = std::conditional_t<std::is_void_v<T>, void, std::vector<T>>;

    //! @brief JoinNext 的产出: T 为 void 时是 bool (false 表示集合已空), 否则是 std::optional<T> (nullopt 表示集合已空)
    using JoinNextOut = std::conditional_t<std::is_void_v<T>, bool, std::optional<T>>;

    JoinSet();
    JoinSet(const JoinSet&)            = delete;
    JoinSet& operator=(const JoinSet&) = delete;
    JoinSet(JoinSet&&)                 = delete;
    JoinSet& operator=(JoinSet&&)      = delete;

    //! @brief Abort 集合里所有任务, 并等它们结束
    ~JoinSet();

    /**
     * @brief 把任务放进集合并开始执行 (对应 JoinSet::spawn)
     * @note 语义同全局的 Spawn。返回值可以直接丢弃
     */
    AbortHandle Spawn(Task<T> task);

    /**
     * @brief 把任务放进集合, 并把当前 worker 让给它
     * @note 语义同全局的 SpawnAndSeize。只能写成 co_await
     */
    [[nodiscard]] detail::SpawnAndSeizeAwaiter<T, AbortHandle> SpawnAndSeize(Task<T> task);

    /**
     * @brief 收割全部产出 (对应 join_all)
     * @note 返回后集合已空
     * @note 调用方被取消时, 先等全部任务结束, 再抛 CancelledError
     * @throws ... 任务抛出的异常, 重抛最先完成的一个
     */
    [[nodiscard]] detail::JoinAllAwaiter<T> JoinAll();

    /**
     * @brief 等任一任务完成并收割 (对应 join_next)
     * @return 集合已空返回空标记, 否则返回这个任务的产出
     * @throws CancelledError 调用方在等待期间被取消
     * @throws ... 任务自身的未捕获异常, 原样重抛
     */
    [[nodiscard]] detail::JoinNextAwaiter<T> JoinNext();

    //! @brief Abort 集合里全部任务, 不等待
    void AbortAll();

    [[nodiscard]] size_t len() const noexcept;
    [[nodiscard]] bool   is_empty() const noexcept;

private:
    std::unique_ptr<detail::JoinSetCore> core_;
};

}  // namespace zenio
```

### 2.1 语义

**`Spawn` 和 `SpawnAndSeize`**: 与 design/spawn.md 里的全局版本语义相同。区别只在归属: 任务归集合管理, 返回的是 `AbortHandle`。`Spawn` 的返回值可以直接丢弃, 所以不标 `[[nodiscard]]`; `SpawnAndSeize` 不写 `co_await` 就什么都不做, 所以标了。

**`JoinAll()`** (S16):

- 等全部任务结束。返回后集合已空。
- 不限定 `&&`: awaiter 嵌在调用方的协程帧里, 不需要接管集合。
- `vector` 按 spawn 顺序排列, 不按完成顺序, 与 tokio 一致。
- 有任务抛出未捕获的异常时, 重抛最先完成的那一个, 其余的按"没人取的结果"处理 (S15)。`CancelledError` 不算错误。
- 被 Abort 的任务不产生结果, 也不算异常。
- 调用方在等待期间被取消: 先等全部任务结束, 再抛 `CancelledError`。
- 子任务之间互不影响: 一个任务抛异常不取消兄弟任务。

**`JoinNext()`**:

- 等任一任务完成并把它从集合里取走。集合已空时返回空标记: `T` 为 `void` 时是 `false`, 否则是 `nullopt`。
- 这个任务抛了未捕获的异常: 原样重抛, 任务照样被取走。
- 被 Abort 的任务被取走并跳过, 继续等下一个。
- 调用方在等待期间被取消: 抛 `CancelledError`。

**`AbortAll()`**: 取消集合里全部任务, 不等待。

**析构兜底**: 如果因为异常或取消没有走到 `JoinAll()`, 析构函数 Abort 全部子任务并等它们结束。在 worker 线程上通过执行队列里的任务来等待, 不会因为线程都阻塞而死锁。

**`len()` 和 `is_empty()`**: 集合里还没被取走的任务数。只能在拥有这个集合的协程里调用。

## 3. 内部数据结构 (概要)

`JoinSet<T>` 是 `detail::JoinSetCore` 上的薄包装。`JoinSetCore` 是非模板的内部实现, M5 细化, 它至少包含:

- 子任务列表。每条登记项持有任务的 `TaskRecord` 和它在 `JoinAll` 结果里的下标。
- 每条登记项的状态: 运行中、正常结束、异常结束、被取消。后三种都带结果 (产出或 `std::exception_ptr`)。
- 正在等待的 `JoinNext` 或 `JoinAll`。
- 集合自己的 `stop_source`, 链接到拥有者的令牌。

## 4. 关键流程 (概要)

1. **子任务结束**: 在任意 worker 上执行。把结果写进登记项, 通知 `JoinSetCore`: 有 `JoinNext` 在等就唤醒它, 否则等到最后一个结束时唤醒 `JoinAll`。
2. **JoinAll**: 全部结束后, 按 spawn 顺序把产出组装成 `vector`。有异常的重抛第一个, 其余丢弃。最后清空集合。
3. **JoinNext**: 取一条已结束的登记项。是被取消的就跳过再取, 是异常就重抛, 否则返回产出。
4. **AbortAll**: 对每条登记项的 `stop_source` 调用 `request_stop()`。
5. **析构**: 同 AbortAll, 然后在当前线程上执行调度器里的任务, 直到子任务数量归零。

## 5. 不变量

- 集合里的每个任务在同一时刻只属于一个状态。
- 一条登记项的结果只被取走一次。
- `JoinAll` 返回时集合是空的。
- `JoinSet` 析构完成时, 它派发过的任务都已结束。

## 6. 已知的坑

- **`JoinSet` 不可移动**: awaiter 和登记项里有指向它的指针。
- **子任务结束发生在别的线程上**: 更新登记项和唤醒等待者需要原子操作, M5 细化。
- **每次 `Spawn` 一次堆分配** (D10)。
- **`JoinAll` 的等待期间又 `Spawn` 了新任务**: 新任务也在等待范围内, `JoinAll` 要等到它结束。

## 7. 验收测试

M5 的任务书里给出。概要:

- `JoinAll`: 多个任务的产出按 spawn 顺序排列, 与完成顺序无关。
- `JoinAll`: 一个任务抛异常时重抛它, 其余任务仍运行到结束, 集合被清空。
- `JoinAll`: 被 Abort 的任务不出现在结果里。
- `JoinNext`: 逐个取走任务; 集合空了之后返回空标记; 异常被重抛; 被 Abort 的任务被跳过。
- `AbortAll` 之后所有任务都收到 `CancelledError`。
- 没走到 `JoinAll` 就析构, 子任务被取消并等待结束, 没有残留。
