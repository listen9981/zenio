#include "zenio/runtime.h"

#include "zenio/detail/runtime_core.h"

#include <cassert>
#include <stdexcept>
#include <utility>

namespace zenio::runtime {

// —— EnterGuard ——

EnterGuard::EnterGuard(EnterGuard&& other) noexcept
    : previous_(std::exchange(other.previous_, nullptr)), active_(std::exchange(other.active_, false)) {}

EnterGuard& EnterGuard::operator=(EnterGuard&& other) noexcept {
    if (this != &other) {
        Reset();
        previous_ = std::exchange(other.previous_, nullptr);
        active_   = std::exchange(other.active_, false);
    }
    return *this;
}

EnterGuard::~EnterGuard() {
    Reset();
}

void EnterGuard::Reset() noexcept {
    if (active_) {
        detail::SetCurrentContext(previous_);
        active_ = false;
    }
}

// —— Handle ——

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

bool Handle::is_valid() const noexcept {
    return core_ != nullptr;
}

bool Handle::operator==(const Handle& other) const noexcept {
    return core_ == other.core_;
}

EnterGuard Handle::Enter() const {
    assert(core_ && "空句柄上不能调用 Enter()");
    EnterGuard guard;
    guard.previous_ = detail::CurrentContext();
    guard.active_   = true;
    detail::SetCurrentContext(&core_->root_context());
    return guard;
}

Handle::Handle(std::shared_ptr<detail::RuntimeCore> core) noexcept : core_(std::move(core)) {}

// —— Builder ——

Builder Builder::CurrentThread() {
    return Builder();
}

Builder Builder::MultiThread(int workers) {
    if (workers < 1) {
        throw std::invalid_argument("zenio: worker 数量不能小于 1");
    }
    Builder builder;
    builder.workers_ = workers;
    return builder;
}

Builder& Builder::EnableIo() {
    enable_io_ = true;
    return *this;
}

Builder& Builder::EnableTime() {
    enable_time_ = true;
    return *this;
}

Builder& Builder::EnableAll() {
    enable_io_   = true;
    enable_time_ = true;
    return *this;
}

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

// —— Runtime ——

Runtime::Runtime(Runtime&& other) noexcept = default;

Runtime& Runtime::operator=(Runtime&& other) noexcept = default;

Runtime::~Runtime() = default;

Runtime::Runtime(std::shared_ptr<detail::RuntimeCore> core) noexcept : core_(std::move(core)) {}

void Runtime::RunRoot(std::coroutine_handle<> task) {
    core_->RunRoot(task);
}

Handle Runtime::handle() const {
    return Handle(core_);
}

}    // namespace zenio::runtime
