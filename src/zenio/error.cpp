#include "zenio/error.h"

#include <cerrno>
#include <cstring>
#include <mutex>
#include <string>

namespace {
// std::strerror 不保证线程安全: 裸调有数据竞争, 返回值指向的缓冲区也可能被后续调用改写。
// 所以调用时持锁, 并立刻把文本拷进 std::string, 不保存返回的指针。
std::mutex s_strerror_mutex;

std::string StrerrorCopy(int err) {
    std::lock_guard<std::mutex> lock(s_strerror_mutex);
    return std::string(std::strerror(err));
}
}    // namespace

namespace zenio {

SystemError::SystemError(int err, const char* op) : err_(err), op_(op) {
    const std::string text = StrerrorCopy(err_);
    message_.reserve(std::strlen(op_) + text.size() + 32);
    message_ += op_;
    message_ += ": ";
    message_ += text;
    message_ += " [errno=";
    message_ += std::to_string(err_);
    message_ += ']';
}

int SystemError::err() const noexcept {
    return err_;
}

const char* SystemError::op() const noexcept {
    return op_;
}

const char* SystemError::what() const noexcept {
    return message_.c_str();
}

CancelledError::CancelledError(CancelReason reason) noexcept : reason_(reason) {}

CancelReason CancelledError::reason() const noexcept {
    return reason_;
}

const char* CancelledError::what() const noexcept {
    return reason_ == CancelReason::ABORT ? "cancelled (abort)" : "cancelled (shutdown)";
}

ElapsedError::ElapsedError() noexcept : CancelledError(CancelReason::ABORT) {}

const char* ElapsedError::what() const noexcept {
    return "timeout";
}

}    // namespace zenio
