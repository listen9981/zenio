#ifndef ZENIO_DETAIL_FD_H
#define ZENIO_DETAIL_FD_H

#include "zenio/detail/export.h"

namespace zenio::detail {

/**
 * @brief 独占一个 fd, 析构时关闭
 * @note 单元测试能直接用它, 是因为单测链接的是库的目标文件 (S28), 不需要导出
 */
class UniqueFd {
public:
    UniqueFd() noexcept = default;
    explicit UniqueFd(int fd) noexcept;

    UniqueFd(UniqueFd&& other) noexcept;
    UniqueFd& operator=(UniqueFd&& other) noexcept;

    UniqueFd(const UniqueFd&)            = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    ~UniqueFd();

    [[nodiscard]] int  get() const noexcept;
    [[nodiscard]] bool is_valid() const noexcept;

    //! @brief 放弃所有权并返回 fd, 之后本对象不再持有 fd
    int release() noexcept;

    //! @brief 换成 fd; 先关闭原来持有的 fd
    void reset(int fd = -1) noexcept;

private:
    int fd_ = -1;
};

/**
 * @brief 关闭一个 fd, 忽略错误
 * @note close 遇到 EINTR 不重试
 */
void CloseFd(int fd) noexcept;

}    // namespace zenio::detail

#endif    // ZENIO_DETAIL_FD_H
