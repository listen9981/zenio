#ifndef ZENIO_DETAIL_IO_OBJECT_H
#define ZENIO_DETAIL_IO_OBJECT_H

#include "zenio/detail/fd.h"

#include <cstdint>
#include <memory>

namespace zenio::detail {

class RuntimeCore;

/**
 * @brief 一个登记到运行时的 fd, 是套接字对象内部的公共部分
 */
struct IoObject {
    UniqueFd                     fd;        //!< 对象独占的 fd
    std::shared_ptr<RuntimeCore> core;      //!< 所属运行时, 让运行时的内部数据在对象存续期间有效
    std::uint64_t                id = 0;    //!< 对象代号, Poller 用它识别被复用的 fd 编号
};

}    // namespace zenio::detail

#endif    // ZENIO_DETAIL_IO_OBJECT_H
