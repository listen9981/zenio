#include "zenio/detail/scheduler.h"

namespace zenio::detail {

Runnable MakeRunnable(std::coroutine_handle<> handle) noexcept {
    return Runnable {handle, CurrentContext()};
}

ScopedContext::ScopedContext(PromiseContext* context) noexcept : previous_(CurrentContext()) {
    SetCurrentContext(context);
}

ScopedContext::~ScopedContext() {
    SetCurrentContext(previous_);
}

void Scheduler::Schedule(Runnable runnable) {
    queue_.push_back(runnable);
}

bool Scheduler::RunOne() {
    if (queue_.empty()) {
        return false;
    }
    // 先把 Runnable 拷出来、从队列里弹掉, 再恢复协程: 恢复期间可能有新的投递,
    // 不能拿着队列里的引用去恢复 (design/scheduler.md 4.3)
    const Runnable task = queue_.front();
    queue_.pop_front();
    ScopedContext scope(task.context);
    task.handle.resume();
    return true;
}

}    // namespace zenio::detail
