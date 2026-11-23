# 网络

状态: 草稿
细化程度: 地址类型和 `UniqueFd` 全部已细化 (M2); 套接字的接口已细化 (M1), 内部是概要 (M4 前细化)
依据: S5、S8、S18、S19、S20; A10

## 1. 职责与边界

本文件覆盖三块内容:

- 地址类型 `Ipv4Addr`、`Ipv6Addr`、`SocketAddr`。
- `UniqueFd`: 内部用的 fd 所有者。
- 套接字 `TcpListener`、`TcpStream`、`UdpSocket`。

通用规则:

- 不需要等待的系统调用是普通函数, 失败抛 `SystemError` (S8)。会挂起的操作返回 awaiter, 用 `co_await` 等。
- 返回类型与 tokio 一致 (S19)。
- 生命周期按 tokio 的处理方式 (S20): 对象独占持有 fd, 挂起的操作必须先于对象结束。tokio 靠借用检查在编译期保证这一点; C++ 没有这个保证, 所以约定 I/O 对象按值移动进使用它的协程, 由协程独占。有挂起等待时销毁对象视为未定义行为, debug 构建下断言。
- 公开头文件不包含任何平台头。`sockaddr_in`、`sockaddr_in6`、`inet_pton` 这些只出现在 `.cpp` 里。

不在 v1: 主机名形态 (需要 DNS 和阻塞线程池)、`Readable` / `Writable`、组播、Unix socket。

## 2. 对外接口

### 2.1 `zenio/net/address.h`

```cpp
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
    [[nodiscard]] bool is_loopback() const noexcept;
    [[nodiscard]] bool is_unspecified() const noexcept;

    [[nodiscard]] bool operator==(const Ipv4Addr&) const = default;

private:
    std::array<std::uint8_t, 4> octets_{};
};

/**
 * @brief IPv6 地址
 */
class ZENIO_EXPORT Ipv6Addr {
public:
    Ipv6Addr() noexcept = default;
    Ipv6Addr(std::uint16_t s0, std::uint16_t s1, std::uint16_t s2, std::uint16_t s3,
             std::uint16_t s4, std::uint16_t s5, std::uint16_t s6, std::uint16_t s7) noexcept;

    //! @brief ::1
    [[nodiscard]] static Ipv6Addr localhost();
    //! @brief ::
    [[nodiscard]] static Ipv6Addr unspecified();

    [[nodiscard]] std::array<std::uint16_t, 8> segments() const noexcept;
    [[nodiscard]] bool is_loopback() const noexcept;
    [[nodiscard]] bool is_unspecified() const noexcept;

    [[nodiscard]] bool operator==(const Ipv6Addr&) const = default;

private:
    std::array<std::uint16_t, 8> segments_{};
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

}  // namespace zenio::net
```

### 2.2 `zenio/net/tcp.h`

```cpp
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

    [[nodiscard]] int        fd() const noexcept;
    //! @brief 注销并交还 fd, 之后对象不再持有 fd
    [[nodiscard]] int        into_std();
    [[nodiscard]] SocketAddr local_addr() const;

    [[nodiscard]] std::uint32_t ttl() const;
    void                        set_ttl(std::uint32_t ttl);
    //! @brief 取走并清掉套接字上的未决错误, 有就抛出
    void                        take_error();

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
    void                        Shutdown();

    //! @brief 读一段数据, 返回读到的字节数; 返回 0 表示对端关闭
    [[nodiscard]] detail::ReadSomeAwaiter  ReadSome(std::span<char> buf);
    //! @brief 读满整个缓冲区, 返回读到的字节数, 一定等于 buf.size()
    [[nodiscard]] detail::ReadExactAwaiter ReadExact(std::span<char> buf);
    //! @brief 写一段数据, 返回写出的字节数
    [[nodiscard]] detail::WriteSomeAwaiter WriteSome(std::span<const char> buf);
    //! @brief 写完整个缓冲区
    [[nodiscard]] detail::WriteAllAwaiter   WriteAll(std::span<const char> buf);
    // 以上四个另有 std::byte、unsigned char 两种字节类型的重载, 语义相同

private:
    explicit TcpStream(detail::IoObject io) noexcept;

    detail::IoObject io_;
};

}  // namespace zenio::net
```

### 2.3 `zenio/net/udp.h`

```cpp
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

}  // namespace zenio::net
```

### 2.4 缓冲区参数的写法

读写的参数是非模板的 `std::span`, 所以下面两种写法都能编译:

```cpp
char buf[4096];
co_await stream.ReadSome(buf);        // 数组隐式转成 std::span<char>
co_await stream.WriteAll({buf, n});   // 用 (指针, 长度) 构造 std::span<const char>
```

`WriteAll` 如果写成模板函数, `{buf, n}` 这种花括号实参推导不出模板参数, 所以这里不用模板, 而是为三种字节类型各写一个重载。重载的实现在内部统一转成 `(void*, 字节数)`。

## 3. 内部数据结构

### 3.1 地址 (M2)

| 类型 | 存储 | 说明 |
|------|------|------|
| `Ipv4Addr` | `std::array<std::uint8_t, 4>` | 四个八位组, 顺序是 a、b、c、d |
| `Ipv6Addr` | `std::array<std::uint16_t, 8>` | 八个 16 位段, 主机字节序 |
| `SocketAddr` | `std::variant<Ipv4Addr, Ipv6Addr>` 加 `std::uint16_t` | |

判定规则:

- `Ipv4Addr::is_loopback()`: 第一段是 127。
- `Ipv4Addr::is_unspecified()`: 四段全是 0。
- `Ipv6Addr::is_loopback()`: 只有最后一段是 1, 其余是 0。
- `Ipv6Addr::is_unspecified()`: 八段全是 0。

### 3.2 `zenio/detail/fd.h` 的 `UniqueFd` (M2)

```cpp
namespace zenio::detail {

/**
 * @brief 独占一个 fd, 析构时关闭
 */
class UniqueFd {
public:
    UniqueFd() noexcept = default;
    explicit UniqueFd(int fd) noexcept;

    UniqueFd(UniqueFd&& other) noexcept;
    UniqueFd& operator=(UniqueFd&& other) noexcept;

    UniqueFd(const UniqueFd&)            = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    ~UniqueFd();

    [[nodiscard]] int  get() const noexcept;
    [[nodiscard]] bool is_valid() const noexcept;

    //! @brief 放弃所有权并返回 fd, 之后本对象不再持有 fd
    int release() noexcept;

    //! @brief 换成 fd; 先关闭原来持有的 fd
    void reset(int fd = -1) noexcept;

private:
    int fd_ = -1;
};

/**
 * @brief 关闭一个 fd, 忽略错误
 * @note close 遇到 EINTR 不重试
 */
void CloseFd(int fd) noexcept;

}  // namespace zenio::detail
```

行为:

- `get()` 在不持有 fd 时返回 -1。
- 移动之后源对象不再持有 fd, 它析构时不会关闭任何东西。
- 移动赋值先关闭自己原来持有的 fd, 再接管对方的。
- `reset()` 不带参数就是关闭并清空。

### 3.3 `zenio/detail/io_object.h` 的 `IoObject`

每个套接字对象内部持有一个 `IoObject`, 它是三种套接字的公共部分:

```cpp
namespace zenio::detail {

/**
 * @brief 一个登记到运行时的 fd, 是套接字对象内部的公共部分
 */
struct IoObject {
    UniqueFd                     fd;      //!< 对象独占的 fd
    std::shared_ptr<RuntimeCore> core;    //!< 所属运行时, 让运行时的内部数据在对象存续期间有效
    std::uint64_t                id = 0;  //!< 对象代号, Poller 用它识别被复用的 fd 编号
};

}  // namespace zenio::detail
```

- `id` 在创建 `IoObject` 时从一个进程级的原子计数器取, 每个对象一个, 用完不复用。怎么取在 M4 细化。
- `core` 是空指针表示对象是默认构造的空句柄, 或者已经调用过 `into_std()`。
- 套接字类的私有构造函数接收一个 `IoObject`。创建套接字的代码 (`Bind`、`FromStd`、`Connect`、`Accept`) 先组装 `IoObject`, 再交给私有构造函数。awaiters 通过友元关系直接使用它。

## 4. 关键流程

### 4.1 地址的解析与格式化 (M2)

`SocketAddr::Parse(text)`:

1. 找最后一个 `]`。有的话按 IPv6 处理: `]` 前面必须有 `[`, 后面必须紧跟 `:` 和端口。
2. 没有 `]` 时按 IPv4 处理: 找最后一个 `:`, 前面是地址, 后面是端口。
3. 地址部分拷进以空字符结尾的缓冲区, 交给 `inet_pton` 解析; 它返回 0 表示不是合法地址。
4. 端口部分用 `std::from_chars` 解析, 必须整个是十进制数字, 范围 0 ~ 65535。
5. 任何一步不满足就抛 `std::invalid_argument`。

`SocketAddr::to_string()`:

- IPv4: `a.b.c.d:端口`。
- IPv6: `[inet_ntop 的结果]:端口`。`inet_ntop` 给出压缩写法, 如 `::1`。

### 4.2 套接字的操作 (概要, M4 细化)

- **Bind**: 创建非阻塞套接字, 设 `SO_REUSEADDR`, bind, listen。返回前不经过 Poller, 到第一次等待时才登记。
- **Accept**: 先直接调用 accept, 得到连接就构造 `TcpStream` 返回; 返回 `EAGAIN` 才等待监听 fd 可读, 就绪后重试。
- **Connect**: 发起非阻塞 connect, 等待 fd 可写, 再用 `SO_ERROR` 取连接结果。
- **读写**: 先直接调用 read / write, 返回 `EAGAIN` 才等待就绪, 就绪后重试。
- **FromStd**: 把 fd 设成非阻塞并登记。

## 5. 不变量

- 公开头文件里不出现任何平台头的类型。
- 一个 fd 同一时刻只被一个 `UniqueFd` 持有。
- I/O 对象析构之后, 它持有的 fd 一定已被关闭 (除非调用过 `into_std()`)。
- 一个 fd 上同一时刻、同一方向最多只有一个挂起的等待者。
- 有挂起等待的对象不会被销毁 (约定, debug 构建下断言)。

## 6. 已知的坑

- **SIGPIPE**: 往已被对端关闭的连接上写, `write` 会触发 SIGPIPE, 默认行为是终止进程。发送数据时 Linux 用 `MSG_NOSIGNAL` 标志; macOS 没有这个标志, 创建套接字时要设置 `SO_NOSIGPIPE`。
- **errno 被覆盖**: 系统调用返回 -1 之后先把 errno 存下来, 再做别的操作。
- **`EINTR`**: 系统调用被信号打断时重试, 不算失败。重试之前仍要先保存 errno 判断是不是 `EINTR`。
- **`EAGAIN` 和 `EWOULDBLOCK`**: 两个平台上数值相同, 判断时写成一个条件。
- **fd 编号被复用**: fd 关闭后编号可能立刻被新对象占用。Poller 的注册表靠对象代号区分, 见 design/poller.md。
- **花括号实参推导不出模板参数**: 见 2.4。
- **`inet_pton` 需要以空字符结尾的字符串**: `std::string_view` 不带空字符, 必须先拷贝。
- **`close` 遇到 `EINTR`**: 不重试。Linux 上重试可能关掉别的线程刚打开的 fd。

## 7. 验收测试

### M2

`tests/zenio/net/address_unittest.cpp`:

- `Ipv4Addr(127, 0, 0, 1)`: `octets()` 是 `{127, 0, 0, 1}`, `is_loopback()` 为真。
- `Ipv4Addr(127, 1, 2, 3)` 是回环地址; `Ipv4Addr(128, 0, 0, 1)` 不是。
- `Ipv4Addr::localhost()` 等于 `Ipv4Addr(127, 0, 0, 1)`; `Ipv4Addr::unspecified()` 四段全 0 且 `is_unspecified()` 为真。
- `Ipv6Addr::localhost()`: `segments()` 只有最后一段是 1, `is_loopback()` 为真; `Ipv6Addr::unspecified()` 全 0。
- `SocketAddr(Ipv4Addr::localhost(), 7777)`: `is_ipv4()`, `port() == 7777`, `ipv4()` 是回环地址。
- `SocketAddr(Ipv6Addr::localhost(), 7777)`: `is_ipv6()`, `ipv6()` 是回环地址。
- `Parse("127.0.0.1:7777")` 等于 `SocketAddr(Ipv4Addr(127, 0, 0, 1), 7777)`。
- `Parse("[::1]:7777")` 等于 `SocketAddr(Ipv6Addr::localhost(), 7777)`。
- `Parse("0.0.0.0:0")` 成功。
- 下面这些抛 `std::invalid_argument`: 空串、`"127.0.0.1"`、`"127.0.0.1:"`、`"127.0.0.1:70000"`、`"256.1.1.1:80"`、`"::1:80"` (没有方括号)、`"example.com:80"`、`"[::1]"`。
- `to_string()`: `SocketAddr(Ipv4Addr(127, 0, 0, 1), 7777)` 是 `"127.0.0.1:7777"`; `::1` 是 `"[::1]:7777"`; `2001:db8::1` 是 `"[2001:db8::1]:80"`。
- 往返: 对上面三个地址, `Parse(a.to_string()) == a`。

`tests/zenio/detail/fd_unittest.cpp`:

- 用 `pipe()` 创建一个管道, 把读端放进 `UniqueFd`: `is_valid()`, `get()` 等于那个 fd。
- `UniqueFd` 析构之后, 对那个 fd 调 `fcntl(fd, F_GETFD)` 返回 -1 且 errno 是 `EBADF`。
- 移动构造之后: 源对象 `get()` 是 -1 且 `is_valid()` 为假; 目标对象持有 fd; 源对象析构后 fd 仍然有效; 目标对象析构后 fd 才被关闭。
- 移动赋值: 目标对象原来持有的 fd 被关闭, 接管了源对象的 fd。
- `release()`: 返回 fd, 之后对象不再持有 fd, 析构不关闭它 (测试最后手动关闭)。
- `reset(new_fd)`: 原来持有的 fd 被关闭, 新 fd 被持有。
- 默认构造的对象 `get()` 是 -1, 析构不关闭任何东西。

### M4 和 M8

套接字的验收测试在 M4 (TCP) 和 M8 (UDP) 的任务书里给出。
