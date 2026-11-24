#ifndef ZENIO_DETAIL_RUNTIME_CORE_H
#define ZENIO_DETAIL_RUNTIME_CORE_H

#include "zenio/detail/scheduler.h"

#include <coroutine>
#include <memory>
#include <stop_token>

namespace zenio::detail {

//! @brief Builder::Build() 交给 RuntimeCore 的配置
struct RuntimeOptions {
    int  workers     = 0;
    bool enable_io   = false;
    bool enable_time = false;
};

/**
 * @brief Runtime 的内部对象: 配置、调度器, 以后还有 Poller 和定时器驱动
 * @note 总是由 shared_ptr 持有: Handle 和 I/O 对象靠它续命
 */
class RuntimeCore : public std::enable_shared_from_this<RuntimeCore> {
public:
    explicit RuntimeCore(const RuntimeOptions& options);

    RuntimeCore(const RuntimeCore&)            = delete;
    RuntimeCore& operator=(const RuntimeCore&) = delete;

    //! @brief 把一个可恢复的协程入队, 见 design/scheduler.md 4.2
    void Schedule(Runnable runnable) { scheduler_.Schedule(runnable); }

    //! @brief 把 root 当作根任务运行, 直到没事可做
    void RunRoot(std::coroutine_handle<> root);

    [[nodiscard]] bool            io_enabled() const noexcept { return io_enabled_; }
    [[nodiscard]] bool            time_enabled() const noexcept { return time_enabled_; }
    [[nodiscard]] Scheduler&      scheduler() noexcept { return scheduler_; }
    [[nodiscard]] PromiseContext& root_context() noexcept { return root_context_; }

private:
    bool             io_enabled_;          //!< 记下的选项, M4 用
    bool             time_enabled_;        //!< 记下的选项, M4 用
    bool             blocking_ = false;    //!< BlockOn 正在运行
    std::stop_source shutdown_source_;     //!< 关闭令牌的来源
    PromiseContext   root_context_;        //!< 根上下文: 令牌是关闭令牌, core 指向自己
    Scheduler        scheduler_;
};

}    // namespace zenio::detail

#endif    // ZENIO_DETAIL_RUNTIME_CORE_H
