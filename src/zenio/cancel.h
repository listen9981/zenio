#ifndef ZENIO_CANCEL_H
#define ZENIO_CANCEL_H

#include "zenio/detail/awaiters.h"
#include "zenio/detail/context.h"

#include <type_traits>

namespace zenio {

/**
 * @brief 用一个不可取消的上下文等待 awaitable
 * @note 给清理代码用: 当前协程已经被取消, 但还需要 co_await 的时候
 */
template <class Awaitable>
[[nodiscard]] ZENIO_EXPORT detail::UncancellableAwaiter<std::decay_t<Awaitable>> Uncancellable(Awaitable&& awaitable);

}    // namespace zenio

#endif    // ZENIO_CANCEL_H
