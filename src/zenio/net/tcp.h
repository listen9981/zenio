#ifndef ZENIO_NET_TCP_H
#define ZENIO_NET_TCP_H

#include "zenio/detail/awaiters.h"
#include "zenio/detail/io_object.h"
#include "zenio/net/address.h"
#include "zenio/runtime.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace zenio::net {

/**
 * @brief TCP 监听器 (对应 tokio::net::TcpListener)
 */
class ZENIO_EXPORT TcpListener {
public:
    /**
     * @brief 绑定并监听
     * @throws std::runtime_error 不在运行时上下文里, 或运行时没有开启 I/O
     * @throws zenio::SystemError socket、setsockopt、bind、listen 任一步失败
     */
    [[nodiscard]] static TcpListener Bind(const SocketAddr& addr);
    //! @brief 同上, 显式指定运行时, 不依赖当前上下文
    [[nodiscard]] static TcpListener Bind(const SocketAddr& addr, const runtime::Handle& handle);

    /**
     * @brief 接管一个已有的监听 fd (对应 from_std)
     * @note 把 fd 设为非阻塞, 并登记到运行时。调用之后 fd 归对象所有
     * @throws std::runtime_error 不在运行时上下文里
     * @throws zenio::SystemError fcntl 失败
     */
    [[nodiscard]] static TcpListener FromStd(int fd);
    [[nodiscard]] static TcpListener FromStd(int fd, const runtime::Handle& handle);

    TcpListener(TcpListener&& other) noexcept;
    TcpListener& operator=(TcpListener&& other) noexcept;
    TcpListener(const TcpListener&)            = delete;
    TcpListener& operator=(const TcpListener&) = delete;

    //! @brief 关闭 fd
    ~TcpListener();

    [[nodiscard]] int fd() const noexcept;
    //! @brief 注销并交还 fd, 之后对象不再持有 fd
    [[nodiscard]] int        into_std();
    [[nodiscard]] SocketAddr local_addr() const;

    [[nodiscard]] std::uint32_t ttl() const;
    void                        set_ttl(std::uint32_t ttl);
    //! @brief 取走并清掉套接字上的未决错误, 有就抛出
    void take_error();

    /**
     * @brief 接受一条连接
     * @return (TcpStream, 对端地址)
     * @throws zenio::SystemError accept 失败
     */
    [[nodiscard]] detail::AcceptAwaiter Accept();

private:
    explicit TcpListener(detail::IoObject io) noexcept;

    detail::IoObject io_;
};

/**
 * @brief TCP 连接 (对应 tokio::net::TcpStream)
 */
class ZENIO_EXPORT TcpStream {
public:
    /**
     * @brief 发起连接
     * @throws zenio::SystemError 连接失败, 包括被对端拒绝
     */
    [[nodiscard]] static detail::ConnectAwaiter Connect(const SocketAddr& addr);

    [[nodiscard]] static TcpStream FromStd(int fd);
    [[nodiscard]] static TcpStream FromStd(int fd, const runtime::Handle& handle);

    TcpStream(TcpStream&& other) noexcept;
    TcpStream& operator=(TcpStream&& other) noexcept;
    TcpStream(const TcpStream&)            = delete;
    TcpStream& operator=(const TcpStream&) = delete;

    ~TcpStream();

    [[nodiscard]] int        fd() const noexcept;
    [[nodiscard]] int        into_std();
    [[nodiscard]] SocketAddr peer_addr() const;
    [[nodiscard]] SocketAddr local_addr() const;

    [[nodiscard]] bool          nodelay() const;
    void                        set_nodelay(bool nodelay);
    [[nodiscard]] std::uint32_t ttl() const;
    void                        set_ttl(std::uint32_t ttl);
    void                        take_error();
    //! @brief 关闭写方向 (对应 shutdown)
    void Shutdown();

    //! @brief 读一段数据, 返回读到的字节数; 返回 0 表示对端关闭
    [[nodiscard]] detail::ReadSomeAwaiter ReadSome(std::span<char> buf);
    //! @brief 读满整个缓冲区, 返回读到的字节数, 一定等于 buf.size()
    [[nodiscard]] detail::ReadExactAwaiter ReadExact(std::span<char> buf);
    //! @brief 写一段数据, 返回写出的字节数
    [[nodiscard]] detail::WriteSomeAwaiter WriteSome(std::span<const char> buf);
    //! @brief 写完整个缓冲区
    [[nodiscard]] detail::WriteAllAwaiter WriteAll(std::span<const char> buf);

    //! @brief 同上, 字节类型为 unsigned char
    [[nodiscard]] detail::ReadSomeAwaiter ReadSome(std::span<unsigned char> buf);
    //! @brief 同上, 字节类型为 unsigned char
    [[nodiscard]] detail::ReadExactAwaiter ReadExact(std::span<unsigned char> buf);
    //! @brief 同上, 字节类型为 unsigned char
    [[nodiscard]] detail::WriteSomeAwaiter WriteSome(std::span<const unsigned char> buf);
    //! @brief 同上, 字节类型为 unsigned char
    [[nodiscard]] detail::WriteAllAwaiter WriteAll(std::span<const unsigned char> buf);

    //! @brief 同上, 字节类型为 std::byte
    [[nodiscard]] detail::ReadSomeAwaiter ReadSome(std::span<std::byte> buf);
    //! @brief 同上, 字节类型为 std::byte
    [[nodiscard]] detail::ReadExactAwaiter ReadExact(std::span<std::byte> buf);
    //! @brief 同上, 字节类型为 std::byte
    [[nodiscard]] detail::WriteSomeAwaiter WriteSome(std::span<const std::byte> buf);
    //! @brief 同上, 字节类型为 std::byte
    [[nodiscard]] detail::WriteAllAwaiter WriteAll(std::span<const std::byte> buf);

private:
    explicit TcpStream(detail::IoObject io) noexcept;

    detail::IoObject io_;
};

}    // namespace zenio::net

#endif    // ZENIO_NET_TCP_H
