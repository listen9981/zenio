#ifndef ZENIO_NET_UDP_H
#define ZENIO_NET_UDP_H

#include "zenio/detail/awaiters.h"
#include "zenio/detail/io_object.h"
#include "zenio/net/address.h"
#include "zenio/runtime.h"

#include <cstddef>
#include <span>

namespace zenio::net {

/**
 * @brief UDP 套接字 (对应 tokio::net::UdpSocket)
 */
class ZENIO_EXPORT UdpSocket {
public:
    //! @brief 绑定。异常同 TcpListener::Bind
    [[nodiscard]] static UdpSocket Bind(const SocketAddr& addr);
    [[nodiscard]] static UdpSocket Bind(const SocketAddr& addr, const runtime::Handle& handle);

    [[nodiscard]] static UdpSocket FromStd(int fd);
    [[nodiscard]] static UdpSocket FromStd(int fd, const runtime::Handle& handle);

    UdpSocket(UdpSocket&& other) noexcept;
    UdpSocket& operator=(UdpSocket&& other) noexcept;
    UdpSocket(const UdpSocket&)            = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    ~UdpSocket();

    [[nodiscard]] int        fd() const noexcept;
    [[nodiscard]] int        into_std();
    [[nodiscard]] SocketAddr local_addr() const;
    void                     take_error();

    /**
     * @brief 发送一个数据报
     * @return 发出的字节数
     * @throws zenio::SystemError sendto 失败
     */
    [[nodiscard]] detail::SendToAwaiter SendTo(std::span<const char> buf, const SocketAddr& target);

    /**
     * @brief 接收一个数据报
     * @return (收到的字节数, 发送方地址)
     * @throws zenio::SystemError recvfrom 失败
     */
    [[nodiscard]] detail::RecvFromAwaiter RecvFrom(std::span<char> buf);

private:
    explicit UdpSocket(detail::IoObject io) noexcept;

    detail::IoObject io_;
};

}    // namespace zenio::net

#endif    // ZENIO_NET_UDP_H
