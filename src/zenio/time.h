#ifndef ZENIO_TIME_H
#define ZENIO_TIME_H

#include "zenio/detail/awaiters.h"
#include "zenio/task.h"

#include <chrono>

namespace zenio {

/**
 * @brief 等待一段时间 (对应 tokio::time::sleep)
 * @throws zenio::CancelledError 调用方在等待期间被取消
 */
[[nodiscard]] ZENIO_EXPORT detail::SleepAwaiter Sleep(std::chrono::nanoseconds duration);

/**
 * @brief 等到一个时刻 (对应 tokio::time::sleep_until)。时钟用 std::chrono::steady_clock
 * @throws zenio::CancelledError 调用方在等待期间被取消
 */
[[nodiscard]] ZENIO_EXPORT detail::SleepAwaiter SleepUntil(std::chrono::steady_clock::time_point deadline);

/**
 * @brief 给任务加超时 (对应 tokio::time::timeout)
 * @return 任务的产出
 * @throws zenio::ElapsedError 超过 duration。此时 task 被取消
 * @throws ... 任务自身的异常, 原样重抛
 */
template <class T>
[[nodiscard]] ZENIO_EXPORT Task<T> Timeout(Task<T> task, std::chrono::nanoseconds duration);

}    // namespace zenio

#endif    // ZENIO_TIME_H
