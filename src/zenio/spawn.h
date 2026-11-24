#ifndef ZENIO_SPAWN_H
#define ZENIO_SPAWN_H

#include "zenio/task.h"

#include <memory>

namespace zenio::detail {

template <class T>
class JoinHandleAwaiter;
template <class T, class R>
class SpawnAndSeizeAwaiter;

class TaskRecord;

}    // namespace zenio::detail

namespace zenio {

/**
 * @brief 游离任务的句柄 (对应 tokio::task::JoinHandle)
 * @note 句柄析构不取消任务
 */
template <class T>
class ZENIO_EXPORT JoinHandle {
public:
    JoinHandle() noexcept = default;
    JoinHandle(JoinHandle&& other) noexcept;
    JoinHandle& operator=(JoinHandle&& other) noexcept;
    JoinHandle(const JoinHandle&)            = delete;
    JoinHandle& operator=(const JoinHandle&) = delete;

    ~JoinHandle();

    [[nodiscard]] bool is_valid() const noexcept;

    //! @brief 任务是否已经结束
    [[nodiscard]] bool is_finished() const;

    //! @brief 取消这一个任务
    void Abort();

    //! @brief 等任务结束: 产出 T, 或重抛任务的异常
    detail::JoinHandleAwaiter<T> operator co_await() const;

private:
    std::shared_ptr<detail::TaskRecord> record_;
};

/**
 * @brief 只能用来取消的句柄 (对应 tokio::task::AbortHandle)
 * @note 句柄析构不取消任务
 */
class ZENIO_EXPORT AbortHandle {
public:
    AbortHandle() noexcept = default;
    AbortHandle(AbortHandle&& other) noexcept;
    AbortHandle& operator=(AbortHandle&& other) noexcept;
    AbortHandle(const AbortHandle&)            = delete;
    AbortHandle& operator=(const AbortHandle&) = delete;

    ~AbortHandle();

    [[nodiscard]] bool is_valid() const noexcept;

    //! @brief 取消这一个任务
    void Abort();

private:
    std::shared_ptr<detail::TaskRecord> record_;
};

/**
 * @brief 派发一个游离任务 (对应 tokio::spawn)
 * @throws std::runtime_error 不在运行时上下文里
 */
template <class T>
ZENIO_EXPORT JoinHandle<T> Spawn(Task<T> task);

/**
 * @brief 派发任务, 并把当前 worker 让给它
 * @note 补充的 API, 语义来自 bthread 的 bthread_start_urgent。只能写成 co_await
 */
template <class T>
[[nodiscard]] ZENIO_EXPORT detail::SpawnAndSeizeAwaiter<T, JoinHandle<T>> SpawnAndSeize(Task<T> task);

}    // namespace zenio

#endif    // ZENIO_SPAWN_H
