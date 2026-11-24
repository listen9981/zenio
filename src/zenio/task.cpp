#include "zenio/task.h"

namespace zenio {

[[nodiscard]] detail::YieldAwaiter Yield() {
    return {};
}

}    // namespace zenio
