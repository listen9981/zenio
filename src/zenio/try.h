#ifndef ZENIO_TRY_H
#define ZENIO_TRY_H

#include "zenio/task.h"

#include <exception>
#include <type_traits>
#include <utility>

namespace zenio {

/**
 * @brief 把任务的异常收进 Result, 供不想用异常的代码使用
 * @note CancelledError 也会被收进 Err
 * @note 不支持产出引用的 Task<T&>: Result 的存储里放不了引用, 所以用约束拦住
 */
template <class T>
    requires(!std::is_reference_v<T>)
[[nodiscard]] ZENIO_EXPORT Task<Result<T, std::exception_ptr>> Try(Task<T> task) {
    try {
        if constexpr (std::is_void_v<T>) {
            co_await std::move(task);
            co_return Result<void, std::exception_ptr>::Ok();
        } else {
            co_return Result<T, std::exception_ptr>::Ok(co_await std::move(task));
        }
    } catch (...) {
        co_return Result<T, std::exception_ptr>::Err(std::current_exception());
    }
}

}    // namespace zenio

#endif    // ZENIO_TRY_H
