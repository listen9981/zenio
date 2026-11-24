#ifndef ZENIO_RESULT_H
#define ZENIO_RESULT_H

#include "zenio/detail/export.h"

#include <system_error>
#include <type_traits>
#include <variant>

namespace zenio::detail {

//! @brief void 映射成 std::monostate, 其它类型原样保留。Result<void> 和 Select 的 void 分支都用它
template <class T>
using VoidToMonostate = std::conditional_t<std::is_void_v<T>, std::monostate, T>;

}    // namespace zenio::detail

namespace zenio {

/**
 * @brief 轻量的 Result 类型, 对齐 Rust Result<T, E> 的语义
 * @note 三态: 空 (默认构造) / Ok / Err。T 为 void 时用 std::monostate 承载 Ok 的值
 * @note 运行时 API 不用它包装返回值; 它给用户自己的可失败域、Task 产出的存储和 Try 使用
 */
template <class T, class E = std::error_code>
class ZENIO_EXPORT Result {
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
    //! @brief 私有构造函数的标签。不能靠参数类型区分 Ok 和 Err, 见 design/error.md 第 6 节
    struct OkTag {};
    struct ErrTag {};

    template <class U>
        requires(!std::is_void_v<U>)
    explicit Result(OkTag, U&& value);

    explicit Result(ErrTag, E err);

    std::variant<std::monostate, OkType, E> value_;
};

// —— 成员函数定义 (M2)。实现写在类定义之后, 头文件里, 见 design/error.md 第 3 节 ——

template <class T, class E>
template <class U>
    requires(!std::is_void_v<U>)
Result<T, E>::Result(OkTag, U&& value) : value_(std::in_place_index<1>, std::forward<U>(value)) {}

template <class T, class E>
Result<T, E>::Result(ErrTag, E err) : value_(std::in_place_index<2>, std::move(err)) {}

template <class T, class E>
Result<T, E> Result<T, E>::Ok()
    requires std::is_void_v<T>
{
    Result r;
    r.value_.template emplace<1>();
    return r;
}

template <class T, class E>
template <class U>
    requires(!std::is_void_v<U>)
Result<T, E> Result<T, E>::Ok(U&& value) {
    return Result(OkTag {}, std::forward<U>(value));
}

template <class T, class E>
Result<T, E> Result<T, E>::Err(E err) {
    return Result(ErrTag {}, std::move(err));
}

template <class T, class E>
bool Result<T, E>::is_empty() const noexcept {
    return value_.index() == 0;
}

template <class T, class E>
bool Result<T, E>::is_ok() const noexcept {
    return value_.index() == 1;
}

template <class T, class E>
bool Result<T, E>::is_err() const noexcept {
    return value_.index() == 2;
}

template <class T, class E>
Result<T, E>::operator bool() const noexcept
    requires(!std::is_same_v<OkType, bool>)
{
    return is_ok();
}

template <class T, class E>
auto Result<T, E>::value() & -> OkType& {
    return std::get<1>(value_);
}

template <class T, class E>
auto Result<T, E>::value() const& -> const OkType& {
    return std::get<1>(value_);
}

template <class T, class E>
auto Result<T, E>::value() && -> OkType&& {
    return std::get<1>(std::move(value_));
}

template <class T, class E>
auto Result<T, E>::error() & -> E& {
    return std::get<2>(value_);
}

template <class T, class E>
auto Result<T, E>::error() const& -> const E& {
    return std::get<2>(value_);
}

template <class T, class E>
auto Result<T, E>::error() && -> E&& {
    return std::get<2>(std::move(value_));
}

template <class T, class E>
auto Result<T, E>::value_or(OkType fallback) const& -> OkType
    requires std::is_copy_constructible_v<OkType>
{
    return is_ok() ? value() : fallback;
}

template <class T, class E>
auto Result<T, E>::value_or(OkType fallback) && -> OkType {
    return is_ok() ? std::move(*this).value() : std::move(fallback);
}

}    // namespace zenio

#endif    // ZENIO_RESULT_H
