#include "zenio/detail/waiter.h"

#include "zenio/detail/runtime_core.h"

#include <cassert>

namespace zenio::detail {

bool Waiter::TrySuspend(std::coroutine_handle<> self) noexcept {
    self_ = MakeRunnable(self);
    state_.store(State::SUSPENDED, std::memory_order::release);
    if (!signaled_.load(std::memory_order::acquire)) {
        return true;
    }
    // Signal 抢在挂起之前到了: 自己负责原地恢复
    State expected = State::SUSPENDED;
    return !state_.compare_exchange_strong(expected, State::SCHEDULED, std::memory_order::acq_rel,
                                           std::memory_order::acquire);
}

void Waiter::Signal() noexcept {
    if (signaled_.exchange(true, std::memory_order::acq_rel)) {
        return;
    }
    if (state_.exchange(State::SCHEDULED, std::memory_order::acq_rel) == State::SUSPENDED) {
        assert(self_.context != nullptr && self_.context->core != nullptr);
        self_.context->core->Schedule(self_);    // 之后不能再访问 this
    }
}

}    // namespace zenio::detail
