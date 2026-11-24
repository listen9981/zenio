#ifndef ZENIO_DETAIL_CONTEXT_H
#define ZENIO_DETAIL_CONTEXT_H

#include "zenio/detail/export.h"

#include <cassert>
#include <coroutine>
#include <stop_token>

namespace zenio::detail {

class RuntimeCore;

/**
 * @brief 每个 zenio 协程 promise 的公共部分
 */
struct PromiseContext {
    std::stop_token token;                //!< 取消令牌, 启动时从上一级复制下来
    RuntimeCore*    core    = nullptr;    //!< 所属运行时的内部对象: 调度器和 Poller
    bool            started = false;      //!< 已经开始执行, Task 的析构函数用它做断言
};

/**
 * @brief 取当前线程的上下文
 * @return 不在 zenio 协程里运行时返回空指针
 * @note 标了导出: 公开头文件里的模板实现要调用它, 见 design/build.md 2.3
 */
ZENIO_EXPORT PromiseContext* CurrentContext() noexcept;

/**
 * @brief 把 context 设为当前线程的上下文
 * @note 调度器恢复协程时装上它挂起时的上下文; Task 的交接也用它切换
 */
ZENIO_EXPORT void SetCurrentContext(PromiseContext* context) noexcept;

/**
 * @brief Task 的 initial_suspend() 的返回值
 * @note await_resume 是协程启动后、函数体之前执行的第一件事
 */
template <class Promise>
class InitialAwaiter {
public:
    [[nodiscard]] static constexpr bool await_ready() noexcept { return false; }

    //! @brief 一定挂起: 协程在被 co_await 之前不运行
    void await_suspend(std::coroutine_handle<Promise> self) noexcept { self_ = self; }

    //! @brief 把线程局部存储里的上下文复制进 promise 并切换过去; seize_caller_ 非空时把它重新投递
    void await_resume() noexcept;

private:
    std::coroutine_handle<Promise> self_;    //!< 启动的协程
};

/**
 * @brief Task 的 final_suspend() 的返回值
 */
template <class Promise>
class FinalAwaiter {
public:
    [[nodiscard]] static constexpr bool await_ready() noexcept { return false; }

    /**
     * @brief 返回等待方的句柄, 它在同一线程上接着运行 (symmetric transfer)
     * @note 没有等待方时返回 std::noop_coroutine()
     */
    std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> self) noexcept;

    //! @brief 不应被调用: 协程在 final_suspend 之后不会再恢复
    void await_resume() noexcept;
};

/**
 * @brief Yield() 的 awaiter: 把调用方投递到队列, 让别的任务先运行
 * @note 类上标了导出: 使用者的协程体直接调用它的 await_suspend, 见 design/build.md 2.3
 */
class ZENIO_EXPORT YieldAwaiter {
public:
    [[nodiscard]] static constexpr bool await_ready() noexcept { return false; }

    //! @brief 一定挂起; 调用方被投递到当前 worker 的本地队列
    void await_suspend(std::coroutine_handle<> caller) noexcept;

    void await_resume() noexcept {}
};

template <class Promise>
void InitialAwaiter<Promise>::await_resume() noexcept {
    PromiseContext& context = self_.promise();
    if (const PromiseContext* current = CurrentContext()) {
        context.token = current->token;
        context.core  = current->core;
    }
    context.started = true;
    SetCurrentContext(&context);
    // M5: self_.promise().seize_caller_ 非空时在这里把它重新投递, 见 design/spawn.md 4.2
}

template <class Promise>
std::coroutine_handle<> FinalAwaiter<Promise>::await_suspend(std::coroutine_handle<Promise> self) noexcept {
    const std::coroutine_handle<> continuation = self.promise().continuation_;
    return continuation ? continuation : std::noop_coroutine();
}

template <class Promise>
void FinalAwaiter<Promise>::await_resume() noexcept {
    assert(false && "FinalAwaiter::await_resume 不应被调用");
}

}    // namespace zenio::detail

#endif    // ZENIO_DETAIL_CONTEXT_H
