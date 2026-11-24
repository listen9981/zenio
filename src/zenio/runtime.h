#ifndef ZENIO_RUNTIME_H
#define ZENIO_RUNTIME_H

#include "zenio/detail/context.h"
#include "zenio/spawn.h"
#include "zenio/task.h"

#include <memory>

namespace zenio::detail {

class RuntimeCore;

}    // namespace zenio::detail

namespace zenio::runtime {

/**
 * @brief Handle::Enter() 的返回值, 把句柄登记为当前线程的上下文
 * @note 离开作用域后恢复之前的上下文
 */
class ZENIO_EXPORT EnterGuard {
public:
    EnterGuard(EnterGuard&& other) noexcept;
    EnterGuard& operator=(EnterGuard&& other) noexcept;
    EnterGuard(const EnterGuard&)            = delete;
    EnterGuard& operator=(const EnterGuard&) = delete;

    ~EnterGuard();

private:
    friend class Handle;

    EnterGuard() noexcept;

    detail::PromiseContext* previous_ = nullptr;
};

/**
 * @brief 指向运行时的句柄 (对应 tokio::runtime::Handle)
 * @note 可拷贝, 可跨线程使用
 */
class ZENIO_EXPORT Handle {
public:
    Handle() noexcept = default;

    /**
     * @brief 当前运行时的句柄 (对应 Handle::current)
     * @throws std::runtime_error 不在运行时上下文里
     */
    [[nodiscard]] static Handle Current();

    /**
     * @brief 当前运行时的句柄 (对应 Handle::try_current)
     * @return 不在运行时上下文里时返回空句柄
     */
    [[nodiscard]] static Handle TryCurrent() noexcept;

    [[nodiscard]] bool is_valid() const noexcept;

    /**
     * @brief 把句柄登记为当前线程的上下文 (对应 Handle::enter)
     * @note 作用域内可以在普通线程上调用需要上下文的同步函数, 如 TcpListener::Bind(addr)
     */
    [[nodiscard]] EnterGuard Enter() const;

    /**
     * @brief 在这个运行时上派发一个游离任务 (对应 Handle::spawn)
     * @note 不依赖当前上下文, 可以在非 Runtime 线程调用
     */
    template <class T>
    JoinHandle<T> Spawn(Task<T> task);

private:
    friend class Runtime;
    friend class Builder;

    std::shared_ptr<detail::RuntimeCore> core_;
};

class Runtime;

/**
 * @brief Runtime 的构造器 (对应 tokio::runtime::Builder)
 */
class ZENIO_EXPORT Builder {
public:
    //! @brief 单线程: 调用 BlockOn 的线程运行整个运行时
    [[nodiscard]] static Builder CurrentThread();

    /**
     * @brief 多线程: n 个 worker 线程
     * @throws std::invalid_argument n 小于 1
     */
    [[nodiscard]] static Builder MultiThread(int workers);

    //! @brief 开启 I/O 和信号
    Builder& EnableIo();
    //! @brief 开启定时器
    Builder& EnableTime();
    //! @brief 同时开启两者 (对应 enable_all)
    Builder& EnableAll();

    [[nodiscard]] Runtime Build();

private:
    int  workers_     = 0;
    bool enable_io_   = false;
    bool enable_time_ = false;
};

/**
 * @brief 运行时 (对应 tokio::runtime::Runtime)
 */
class ZENIO_EXPORT Runtime {
public:
    Runtime(Runtime&& other) noexcept;
    Runtime& operator=(Runtime&& other) noexcept;
    Runtime(const Runtime&)            = delete;
    Runtime& operator=(const Runtime&) = delete;

    /**
     * @brief Abort 所有任务并等它们结束, 然后 join 线程
     */
    ~Runtime();

    /**
     * @brief 运行主任务直到结束
     * @return 任务的产出; 任务抛了异常就在调用线程上重抛
     */
    template <class T>
    T BlockOn(Task<T> task);

    [[nodiscard]] Handle handle() const;

    //! @brief 在这个运行时上派发一个游离任务 (对应 Runtime::spawn)
    template <class T>
    JoinHandle<T> Spawn(Task<T> task);

private:
    friend class Builder;

    explicit Runtime(std::shared_ptr<detail::RuntimeCore> core) noexcept;

    std::shared_ptr<detail::RuntimeCore> core_;
};

}    // namespace zenio::runtime

#endif    // ZENIO_RUNTIME_H
