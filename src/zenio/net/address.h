#ifndef ZENIO_NET_ADDRESS_H
#define ZENIO_NET_ADDRESS_H

#include "zenio/detail/export.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>

namespace zenio::net {

/**
 * @brief IPv4 地址
 */
class ZENIO_EXPORT Ipv4Addr {
public:
    Ipv4Addr() noexcept = default;
    Ipv4Addr(std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d) noexcept;

    //! @brief 127.0.0.1
    [[nodiscard]] static Ipv4Addr localhost();
    //! @brief 0.0.0.0
    [[nodiscard]] static Ipv4Addr unspecified();

    [[nodiscard]] std::array<std::uint8_t, 4> octets() const noexcept;
    [[nodiscard]] bool                        is_loopback() const noexcept;
    [[nodiscard]] bool                        is_unspecified() const noexcept;

    [[nodiscard]] bool operator==(const Ipv4Addr&) const = default;

private:
    std::array<std::uint8_t, 4> octets_ {};
};

/**
 * @brief IPv6 地址
 */
class ZENIO_EXPORT Ipv6Addr {
public:
    Ipv6Addr() noexcept = default;
    Ipv6Addr(std::uint16_t s0,
             std::uint16_t s1,
             std::uint16_t s2,
             std::uint16_t s3,
             std::uint16_t s4,
             std::uint16_t s5,
             std::uint16_t s6,
             std::uint16_t s7) noexcept;

    //! @brief ::1
    [[nodiscard]] static Ipv6Addr localhost();
    //! @brief ::
    [[nodiscard]] static Ipv6Addr unspecified();

    [[nodiscard]] std::array<std::uint16_t, 8> segments() const noexcept;
    [[nodiscard]] bool                         is_loopback() const noexcept;
    [[nodiscard]] bool                         is_unspecified() const noexcept;

    [[nodiscard]] bool operator==(const Ipv6Addr&) const = default;

private:
    std::array<std::uint16_t, 8> segments_ {};
};

/**
 * @brief 套接字地址: IP 地址加端口
 */
class ZENIO_EXPORT SocketAddr {
public:
    SocketAddr(const Ipv4Addr& ip, std::uint16_t port) noexcept;
    SocketAddr(const Ipv6Addr& ip, std::uint16_t port) noexcept;

    /**
     * @brief 解析点分十进制形式的地址
     * @param text 形如 "127.0.0.1:7777" 或 "[::1]:7777"
     * @throws std::invalid_argument 格式不对
     */
    [[nodiscard]] static SocketAddr Parse(std::string_view text);

    [[nodiscard]] bool is_ipv4() const noexcept;
    [[nodiscard]] bool is_ipv6() const noexcept;

    [[nodiscard]] std::uint16_t port() const noexcept;

    //! @brief 取 IPv4 地址。前提是 is_ipv4()
    [[nodiscard]] const Ipv4Addr& ipv4() const noexcept;
    //! @brief 取 IPv6 地址。前提是 is_ipv6()
    [[nodiscard]] const Ipv6Addr& ipv6() const noexcept;

    //! @brief 文本形式, 如 "127.0.0.1:7777" 或 "[::1]:7777"
    [[nodiscard]] std::string to_string() const;

    [[nodiscard]] bool operator==(const SocketAddr&) const = default;

private:
    std::variant<Ipv4Addr, Ipv6Addr> ip_;
    std::uint16_t                    port_ = 0;
};

}    // namespace zenio::net

#endif    // ZENIO_NET_ADDRESS_H
