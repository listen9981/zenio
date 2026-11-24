#include "zenio/detail/context.h"

#include "zenio/detail/runtime_core.h"
#include "zenio/detail/scheduler.h"

#include <cassert>

namespace zenio::detail {

namespace {

// 当前线程的上下文。定义在 .cpp 里, 不写成头文件里的 inline 函数加 static thread_local:
// 那样写的话, 共享库和使用者程序里会是两份互不相干的变量 (design/milestones/M3.md 5.2)
thread_local PromiseContext* t_current_context = nullptr;

}    // namespace

PromiseContext* CurrentContext() noexcept {
    return t_current_context;
}

void SetCurrentContext(PromiseContext* context) noexcept {
    t_current_context = context;
}

void YieldAwaiter::await_suspend(std::coroutine_handle<> caller) noexcept {
    const PromiseContext* context = CurrentContext();
    assert(context != nullptr && context->core != nullptr && "co_await Yield() 必须在运行时里");
    context->core->Schedule(MakeRunnable(caller));
}

}    // namespace zenio::detail
