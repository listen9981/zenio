# 异常、Result 与 Try

状态: 草稿
细化程度: 全部已细化 (M1 写声明, M2 写实现)
依据: S3、S15、S16、S17、S24; A7、A13

## 1. 职责与边界

本文件定义 zenio 的异常类型层级、`Result<T, E>` 和 `Try`。

- 运行时 API 的错误一律走异常, 不用 `Result` 包装返回值 (S3)。
- `Result` 有三个用处: 用户自己的可失败域; `Task` 产出的存储 (见 design/task.md); `Try` 的产出。
- `detail::VoidToMonostate` 也定义在 `result.h` 里。它把 `void` 映射成 `std::monostate`, `Result<void>` 和 `Select` 的 void 分支共用, 见 design/select.md。
- 异常在模块之间怎么传播, 由各模块的文档规定: `JoinSet` 见 design/join_set.md, 游离任务见 design/spawn.md。
- **命名规则 (S24)**: 异常类型的名字一律以 `Error` 结尾, 一眼能认出是异常类型。v1 一共四个: `Error`、`SystemError`、`CancelledError`、`ElapsedError`。以后新增的异常类型照此。

不在本文件: 取消怎么触发 (design/waiter.md), `Task` 怎么存取产出 (design/task.md)。

"不在运行时上下文里"和"I/O 或定时器没有开启"这两种情况不用本文件的类型, 它们抛 `std::runtime_error`。

## 2. 对外接口

### 2.1 `zenio/error.h`

```cpp
namespace zenio {

//! @brief 取消的原因
enum class CancelReason : std::uint8_t {
    ABORT,     //!< Abort() 或 AbortAll() 发起的取消
    SHUTDOWN,  //!< Runtime 析构时的取消
};

/**
 * @brief zenio 异常类型的根
 * @note 抽象类, 只通过派生类抛出
 */
class ZENIO_EXPORT Error : public std::exception {
public:
    Error(const Error&)                = default;
    Error& operator=(const Error&)     = default;
    Error(Error&&) noexcept            = default;
    Error& operator=(Error&&) noexcept = default;
    ~Error() override                  = default;

    [[nodiscard]] const char* what() const noexcept override = 0;

protected:
    Error() = default;
};

/**
 * @brief 系统调用失败
 */
class ZENIO_EXPORT SystemError : public Error {
public:
    /**
     * @param err 失败时的 errno
     * @param op  失败的系统调用名, 必须是静态字符串, 如 "read"
     */
    SystemError(int err, const char* op);

    [[nodiscard]] int         err() const noexcept;
    [[nodiscard]] const char* op() const noexcept;
    [[nodiscard]] const char* what() const noexcept override;

private:
    int         err_;
    const char* op_;
    std::string message_;
};

/**
 * @brief 操作被取消
 * @note 取消不可撤销: 当前协程之后的 await 点仍会抛 CancelledError
 */
class ZENIO_EXPORT CancelledError : public Error {
public:
    explicit CancelledError(CancelReason reason) noexcept;

    [[nodiscard]] CancelReason reason() const noexcept;
    [[nodiscard]] const char*   what() const noexcept override;

private:
    CancelReason reason_;
};

/**
 * @brief Timeout() 超时。名字来自 tokio 的 tokio::time::error::Elapsed
 * @note 是 CancelledError 的一种, catch (const CancelledError&) 也能接住
 * @note 不叫 Timeout: 它和函数 Timeout() 重名, 那样写 `zenio::Timeout e;` 会被编译器拒绝
 */
class ZENIO_EXPORT ElapsedError : public CancelledError {
public:
    ElapsedError() noexcept;

    [[nodiscard]] const char* what() const noexcept override;
};

}  // namespace zenio
```

### 2.2 `zenio/result.h`

```cpp
namespace zenio::detail {

//! @brief void 映射成 std::monostate, 其它类型原样保留。Result<void> 和 Select 的 void 分支都用它
template <class T>
using VoidToMonostate = std::conditional_t<std::is_void_v<T>, std::monostate, T>;

}  // namespace zenio::detail

namespace zenio {

/**
 * @brief 轻量的 Result 类型, 对齐 Rust Result<T, E> 的语义
 * @note 三态: 空 (默认构造) / Ok / Err。T 为 void 时用 std::monostate 承载 Ok 的值
 * @note 运行时 API 不用它包装返回值; 它给用户自己的可失败域、Task 产出的存储和 Try 使用
 */
template <class T, class E = std::error_code>
class Result {
public:
    using OkType = detail::VoidToMonostate<T>;

    //! @brief 空态: 尚未产出, 既不是 Ok 也不是 Err
    Result() = default;

    Result(const Result&)            = default;
    Result& operator=(const Result&) = default;
    Result(Result&&)                 = default;
    Result& operator=(Result&&)      = default;

    [[nodiscard]] static Result Ok()
        requires std::is_void_v<T>;

    template <class U = T>
        requires(!std::is_void_v<U>)
    [[nodiscard]] static Result Ok(U&& value);

    [[nodiscard]] static Result Err(E err);

    [[nodiscard]] bool is_empty() const noexcept;
    [[nodiscard]] bool is_ok() const noexcept;
    [[nodiscard]] bool is_err() const noexcept;

    //! @brief 是否 Ok。Result<bool> 删除了这个转换, 布尔载荷必须显式取值
    explicit operator bool() const noexcept
        requires(!std::is_same_v<OkType, bool>);

    explicit operator bool() const noexcept
        requires(std::is_same_v<OkType, bool>)
    = delete;

    /**
     * @brief 取 Ok 的载荷
     * @throws std::bad_variant_access 不是 Ok
     */
    [[nodiscard]] OkType&       value() &;
    [[nodiscard]] const OkType& value() const&;
    [[nodiscard]] OkType&&      value() &&;

    /**
     * @brief 取 Err 的载荷
     * @throws std::bad_variant_access 不是 Err
     */
    [[nodiscard]] E&       error() &;
    [[nodiscard]] const E& error() const&;
    [[nodiscard]] E&&      error() &&;

    //! @brief 是 Ok 就取载荷, 否则取 fallback
    [[nodiscard]] OkType value_or(OkType fallback) const&
        requires std::is_copy_constructible_v<OkType>;
    [[nodiscard]] OkType value_or(OkType fallback) &&;

private:
    //! @brief 私有构造函数的标签。不能靠参数类型区分 Ok 和 Err, 见第 6 节
    struct OkTag {};
    struct ErrTag {};

    template <class U>
        requires(!std::is_void_v<U>)
    explicit Result(OkTag, U&& value);

    explicit Result(ErrTag, E err);

    std::variant<std::monostate, OkType, E> value_;
};

}  // namespace zenio
```

### 2.3 `zenio/try.h`

`Try` 是建立在 `Task` 上的组合子, 把任务的异常收进 `Result`。它是协程模板, 整个写在头文件里。

```cpp
namespace zenio {

/**
 * @brief 把任务的异常收进 Result, 供不想用异常的代码使用
 * @note CancelledError 也会被收进 Err
 * @note 不支持产出引用的 Task<T&>: Result 的存储里放不了引用, 所以用约束拦住
 */
template <class T>
    requires(!std::is_reference_v<T>)
[[nodiscard]] Task<Result<T, std::exception_ptr>> Try(Task<T> task) {
    try {
        if constexpr (std::is_void_v<T>) {
            co_await std::move(task);
            co_return Result<void, std::exception_ptr>::Ok();
        } else {
            co_return Result<T, std::exception_ptr>::Ok(co_await std::move(task));
        }
    } catch (...) {
        co_return Result<T, std::exception_ptr>::Err(std::current_exception());
    }
}

}  // namespace zenio
```

用法: `auto r = co_await zenio::Try(Compute());`, 失败时 `std::rethrow_exception(r.error())` 或按需处理。

## 3. 内部数据结构

| 类型 | 成员 | 说明 |
|------|------|------|
| `SystemError` | `err_`、`op_`、`message_` | `op_` 指向调用方给的静态字符串, 不拷贝; `message_` 在构造函数里生成 |
| `CancelledError` | `reason_` | |
| `ElapsedError` | 无额外成员 | 构造时 reason 取 `CancelReason::ABORT` |
| `Result` | `value_` | `std::variant<std::monostate, OkType, E>`; 下标 0 是空态, 1 是 Ok, 2 是 Err |

`what()` 的文本:

| 类型 | 文本 |
|------|------|
| `SystemError` | `<操作名>: <strerror 的文本> [errno=<errno>]`, 例如 `read: Connection reset by peer [errno=54]` |
| `CancelledError` | `cancelled (abort)` 或 `cancelled (shutdown)` |
| `ElapsedError` | `timeout` |

`Result` 各成员的实现 (M2):

- `is_empty()`、`is_ok()`、`is_err()` 就是 `value_.index()` 分别等于 0、1、2。
- `value()`、`error()` 用 `std::get<1>`、`std::get<2>` 取, 状态不对时由标准库抛 `std::bad_variant_access`。
- `Ok()` (T 为 void) 先默认构造, 再 `value_.emplace<1>()`。不能让默认构造的结果直接当 Ok 用。
- `value_or` 的 `const&` 版本返回 `value()` 的拷贝或 fallback; `&&` 版本两者都移动。
- 私有构造函数带一个标签参数: `Ok(U&&)` 走带 `OkTag` 的那个, `Err` 走带 `ErrTag` 的那个。

## 4. 关键流程

都发生在调用方的线程上, 不涉及并发。

1. **系统调用失败**: 返回 -1 后先把 errno 存进局部变量, 再 `throw SystemError(saved, "read")`。`EINTR` 不算失败, 由调用方重试。
2. **取消**: awaiter 恢复后发现取消标志, `throw CancelledError(reason)`。
3. **存进 Task**: 协程体里没被接住的异常进入 `unhandled_exception()`, 存成 `Err(std::current_exception())`。
4. **重抛**: 等待方在 `await_resume` 里看到 `Err`, 调用 `std::rethrow_exception`。
5. **Try**: 见 2.3。

## 5. 不变量

- `Result` 任何时刻处于且只处于一种状态: `is_empty() == !is_ok() && !is_err()`。
- `is_ok()` 为真时 `value()` 一定可用; `is_err()` 为真时 `error()` 一定可用。
- `Result<bool>` 不能上下文转换成 bool。
- `SystemError::what()` 返回的指针在对象存续期间有效。

## 6. 已知的坑

- **私有构造函数不能靠参数类型区分 Ok 和 Err**: `T` 和 `E` 是同一种类型时 (比如 `Result<std::error_code>`, `E` 的默认值就是它), `Ok(值)` 和 `Err(值)` 的实参类型一样, 重载决议会把 `Ok` 判给 `Err` 那个构造函数, 结果悄悄变成错误状态。M2 审查时实测确认: `Result<std::error_code>::Ok(ec)` 得到的是 `Err`。所以两个私有构造函数各带一个标签类型。
- **`Result<bool>` 的布尔转换**: 对布尔载荷做显式布尔转换是语义陷阱, `!r` 取到的是"是否 Ok"而不是载荷。所以 `Result<bool>` 删除了 `operator bool`, 布尔载荷必须显式取值: `r.value()`、`r.value_or(false)`、`(r.is_ok() && r.value())`。
- **errno 被覆盖**: 构造 `SystemError` 之前不能调用任何可能改 errno 的函数。`message_` 在构造函数里生成, 之后不再调用系统函数。
- **`value_or` 的两个重载**: `const&` 版本要求 `OkType` 可拷贝; 只可移动的类型必须用 `&&` 版本, 写成 `std::move(r).value_or(...)`。
- **`await_resume` 不能标 `noexcept`**: 它要重抛异常。
- **`Try` 的 `catch (...)` 也接住 `CancelledError`** (S17)。取消不会因此丢失: 令牌仍是取消状态, 当前协程的下一个 await 点还会抛 `CancelledError`。
- **`Try` 不支持 `Task<T&>`**: `Result<T&>` 的存储里放不了引用。用约束拦住, 报错才指得清楚。`static_assert` 放在函数体里不起作用, 因为返回类型先被实例化、先报错, 编译器不会再去实例化函数体。
- **`std::variant` 的 `valueless_by_exception`**: `OkType` 的拷贝或移动构造抛异常时会出现第四种状态。v1 不处理, 约定放进 `Result` 的类型移动构造不抛。

## 7. 验收测试

M2 交付, 文件在 `tests/zenio/`。

`error_unittest.cpp`:

- `SystemError(ENOENT, "read")`: `err() == ENOENT`, `op()` 指向的字符串等于 `"read"`, `what()` 同时包含 `"read"` 和 errno 的数值。
- `SystemError` 能被 `catch (const zenio::Error&)` 和 `catch (const std::exception&)` 接住。
- `CancelledError(CancelReason::ABORT)`: `reason()` 一致; 能被 `catch (const zenio::Error&)` 接住。
- `ElapsedError`: 能被 `catch (const zenio::ElapsedError&)`、`catch (const zenio::CancelledError&)`、`catch (const zenio::Error&)` 接住。
- 拷贝一个 `SystemError` 后, 两个对象的 `what()` 都正确。

`result_unittest.cpp`:

- 默认构造: `is_empty()`, `!is_ok()`, `!is_err()`; `value()` 和 `error()` 抛 `std::bad_variant_access`。
- `Result<int>::Ok(42)`: `is_ok()`, `value() == 42`; `error()` 抛异常。
- `Result<int>::Err(err)`: `is_err()`, `error() == err`; `value()` 抛异常。
- `Result<void>`: `Ok()` 之后 `is_ok()`, `value()` 不抛; `Err(err)` 之后 `is_err()`。
- `T` 和 `E` 是同一种类型: `Result<std::error_code>::Ok(ec)` 是 Ok 且 `value() == ec`, `Err(ec)` 是 Err; 左值和右值实参都要测。`Result<int, int>` 同理。
- 编译期检查。删除的函数在非模板上下文的 `requires` 表达式里是硬错误, 不是 false, 所以先把检查写进 concept:

  ```cpp
  template <class R>
  concept BoolContextuallyConvertible = requires(const R& r) { static_cast<bool>(r); };

  template <class R>
  concept HasConstValueOr = requires(const R& r) { r.value_or(typename R::OkType{}); };

  template <class R>
  concept HasRvalueValueOr = requires(R&& r) { std::move(r).value_or(typename R::OkType{}); };
  ```

  - `static_assert(!std::is_convertible_v<Result<int>, bool>)` 和 `static_assert(BoolContextuallyConvertible<Result<int>>)`。
  - `static_assert(!BoolContextuallyConvertible<Result<bool>>)`。
  - 只可移动的类型: `static_assert(!HasConstValueOr<Result<std::unique_ptr<int>>>)` 和 `static_assert(HasRvalueValueOr<Result<std::unique_ptr<int>>>)`。运行时再验证它能 `Ok(std::make_unique<int>(1))`, 能 `std::move(r).value()`。
  - `static_assert(HasConstValueOr<Result<int>>)`。
- `value_or`: Ok 时返回载荷, Err 和空态时返回 fallback。
- 默认的 `E` 是 `std::error_code`: `Result<int>::Err(std::make_error_code(std::errc::io_error))` 能编译。
- 拷贝和移动: 拷贝后两个对象状态和载荷一致; 移动后新对象状态一致。

fd 和地址的测试见 design/net.md。
