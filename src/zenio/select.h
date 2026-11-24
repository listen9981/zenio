#ifndef ZENIO_SELECT_H
#define ZENIO_SELECT_H

#include "zenio/detail/awaiters.h"

#include <type_traits>
#include <variant>

namespace zenio {

/**
 * @brief Select 的产出: 第一个完成的分支的结果
 * @note 就是 std::variant。产出 void 的分支用 std::monostate 占位, 与 Result<void> 的做法相同
 */
template <class... Ts>
using SelectResult = std::variant<detail::VoidToMonostate<Ts>...>;

/**
 * @brief 并发等待多个操作, 第一个完成的获胜, 其余被取消
 * @note 至少 1 个分支, 至多 64 个, 与 tokio 的 select! 一致。分支的产出不能是引用
 * @note 分支产出类型的约束 SelectOutputType 定义在 detail/awaiters.h, 见 M1 实现偏差
 * @throws zenio::CancelledError 调用方在等待期间被取消
 * @throws ... 获胜分支的异常, 原样重抛
 */
template <class... Awaitables>
    requires(sizeof...(Awaitables) >= 1 && sizeof...(Awaitables) <= 64 &&
             (detail::SelectOutputType<detail::await_result_t<std::decay_t<Awaitables>>> && ...))
[[nodiscard]] ZENIO_EXPORT detail::SelectAwaiter<std::decay_t<Awaitables>...> Select(Awaitables&&... awaitables);

}    // namespace zenio

#endif    // ZENIO_SELECT_H
