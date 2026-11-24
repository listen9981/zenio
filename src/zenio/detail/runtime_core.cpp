#include "zenio/detail/runtime_core.h"

#include "zenio/detail/context.h"

#include <stdexcept>

namespace zenio::detail {

namespace {

//! @brief 构造时把一个 bool 置为 true, 析构时置回 false
class FlagGuard {
public:
    explicit FlagGuard(bool& flag) noexcept : flag_(flag) { flag_ = true; }

    FlagGuard(const FlagGuard&)            = delete;
    FlagGuard& operator=(const FlagGuard&) = delete;

    ~FlagGuard() { flag_ = false; }

private:
    bool& flag_;
};

}    // namespace

RuntimeCore::RuntimeCore(const RuntimeOptions& options)
    : io_enabled_(options.enable_io), time_enabled_(options.enable_time) {
    root_context_.token = shutdown_source_.get_token();
    root_context_.core  = this;
}

void RuntimeCore::RunRoot(std::coroutine_handle<> root) {
    if (CurrentContext() != nullptr || blocking_) {
        throw std::runtime_error("zenio: BlockOn 不能在运行时上下文里调用, 也不能重入");
    }
    FlagGuard guard(blocking_);
    // 根任务还没启动, 它的 promise 里什么都没有, 上下文给运行时的根上下文 (design/runtime.md 4.2)
    scheduler_.Schedule(Runnable {root, &root_context_});
    while (scheduler_.RunOne()) {
    }
}

}    // namespace zenio::detail
