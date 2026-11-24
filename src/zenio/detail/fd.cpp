#include "zenio/detail/fd.h"

#include <unistd.h>
#include <utility>

namespace zenio::detail {

UniqueFd::UniqueFd(int fd) noexcept : fd_(fd) {}

UniqueFd::UniqueFd(UniqueFd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}

UniqueFd& UniqueFd::operator=(UniqueFd&& other) noexcept {
    if (this != &other) {
        CloseFd(fd_);
        fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
}

UniqueFd::~UniqueFd() {
    CloseFd(fd_);
}

int UniqueFd::get() const noexcept {
    return fd_;
}

bool UniqueFd::is_valid() const noexcept {
    return fd_ >= 0;
}

int UniqueFd::release() noexcept {
    return std::exchange(fd_, -1);
}

void UniqueFd::reset(int fd) noexcept {
    CloseFd(fd_);
    fd_ = fd;
}

void CloseFd(int fd) noexcept {
    if (fd >= 0) {
        ::close(fd);    // 返回值忽略; EINTR 不重试 (Linux 上重试可能关掉别的线程刚打开的 fd)
    }
}

}    // namespace zenio::detail
