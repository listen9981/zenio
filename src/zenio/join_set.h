#ifndef ZENIO_JOIN_SET_H
#define ZENIO_JOIN_SET_H

#include "zenio/detail/awaiters.h"
#include "zenio/spawn.h"
#include "zenio/task.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <type_traits>
#include <vector>

namespace zenio::detail {

template <class T>
class JoinAllAwaiter;
template <class T>
class JoinNextAwaiter;

class JoinSetCore;

}    // namespace zenio::detail

namespace zenio {

/**
 * @brief 一组由集合管理的任务 (对应 tokio::task::JoinSet<T>)
 * @note 必须在运行时上下文里构造
 */
template <class T>
class [[nodiscard]] ZENIO_EXPORT JoinSet {
public:
    //! @brief JoinAll 的产出: T 为 void 时是 void (只收割), 否则是 std::vector<T>, 按 spawn 顺序排列
    using JoinAllOut = std::conditional_t<std::is_void_v<T>, void, std::vector<T>>;

    //! @brief JoinNext 的产出: T 为 void 时是 bool (false 表示集合已空), 否则是 std::optional<T> (nullopt 表示集合已空)
    using JoinNextOut = std::conditional_t<std::is_void_v<T>, bool, std::optional<T>>;

    JoinSet();
    JoinSet(const JoinSet&)            = delete;
    JoinSet& operator=(const JoinSet&) = delete;
    JoinSet(JoinSet&&)                 = delete;
    JoinSet& operator=(JoinSet&&)      = delete;

    //! @brief Abort 集合里所有任务, 并等它们结束
    ~JoinSet();

    /**
     * @brief 把任务放进集合并开始执行 (对应 JoinSet::spawn)
     * @note 语义同全局的 Spawn。返回值可以直接丢弃
     */
    AbortHandle Spawn(Task<T> task);

    /**
     * @brief 把任务放进集合, 并把当前 worker 让给它
     * @note 语义同全局的 SpawnAndSeize。只能写成 co_await
     */
    [[nodiscard]] detail::SpawnAndSeizeAwaiter<T, AbortHandle> SpawnAndSeize(Task<T> task);

    /**
     * @brief 收割全部产出 (对应 join_all)
     * @note 返回后集合已空
     * @note 调用方被取消时, 先等全部任务结束, 再抛 CancelledError
     * @throws ... 任务抛出的异常, 重抛最先完成的一个
     */
    [[nodiscard]] detail::JoinAllAwaiter<T> JoinAll();

    /**
     * @brief 等任一任务完成并收割 (对应 join_next)
     * @return 集合已空返回空标记, 否则返回这个任务的产出
     * @throws CancelledError 调用方在等待期间被取消
     * @throws ... 任务自身的未捕获异常, 原样重抛
     */
    [[nodiscard]] detail::JoinNextAwaiter<T> JoinNext();

    //! @brief Abort 集合里全部任务, 不等待
    void AbortAll();

    [[nodiscard]] std::size_t len() const noexcept;
    [[nodiscard]] bool        is_empty() const noexcept;

private:
    std::unique_ptr<detail::JoinSetCore> core_;
};

}    // namespace zenio

#endif    // ZENIO_JOIN_SET_H
