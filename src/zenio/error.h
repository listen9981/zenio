#ifndef ZENIO_ERROR_H
#define ZENIO_ERROR_H

#include "zenio/detail/export.h"

#include <cstdint>
#include <exception>
#include <string>

namespace zenio {

//! @brief 取消的原因
enum class CancelReason : std::uint8_t {
    ABORT,       //!< Abort() 或 AbortAll() 发起的取消
    SHUTDOWN,    //!< Runtime 析构时的取消
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
    [[nodiscard]] const char*  what() const noexcept override;

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

}    // namespace zenio

#endif    // ZENIO_ERROR_H
