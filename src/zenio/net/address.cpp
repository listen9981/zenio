#include "zenio/net/address.h"

#include <arpa/inet.h>
#include <cassert>
#include <charconv>
#include <cstddef>
#include <cstring>

namespace zenio::net {

namespace {

// 把地址部分的文本拷进以空字符结尾的缓冲区: inet_pton 只接受 C 字符串 (net.md 6)。
template <std::size_t kCapacity>
bool CopyAddressText(char (&buf)[kCapacity], std::string_view text) {
    if (text.size() >= kCapacity) {
        return false;
    }
    std::memcpy(buf, text.data(), text.size());
    buf[text.size()] = '\0';
    return true;
}

// IPv4 自己解析, 不交给 inet_pton: 它对前导零的处理两个平台不一致 (net.md 6)。
// 规则 (net.md 4.1): 四段, '.' 分隔, 每段 1 到 3 个十进制数字, 不超过 255, 除 0 之外不允许前导零。
bool ParseIpv4(std::string_view text, Ipv4Addr& out) {
    std::array<std::uint8_t, 4> octets {};
    std::size_t                 pos = 0;
    for (std::size_t i = 0; i < octets.size(); ++i) {
        if (i > 0) {
            if (pos >= text.size() || text[pos] != '.') {
                return false;
            }
            ++pos;
        }
        const std::size_t start = pos;
        while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
            ++pos;
        }
        const std::size_t len = pos - start;
        if (len < 1 || len > 3) {
            return false;
        }
        unsigned value = 0;
        for (std::size_t j = start; j < pos; ++j) {
            value = value * 10 + static_cast<unsigned>(text[j] - '0');
        }
        if (value > 255) {
            return false;
        }
        if (len > 1 && text[start] == '0') {
            return false;
        }
        octets[i] = static_cast<std::uint8_t>(value);
    }
    if (pos != text.size()) {
        return false;
    }
    out = Ipv4Addr(octets[0], octets[1], octets[2], octets[3]);
    return true;
}

bool ParseIpv6(std::string_view text, Ipv6Addr& out) {
    char buf[INET6_ADDRSTRLEN];
    if (!CopyAddressText(buf, text)) {
        return false;
    }
    in6_addr addr {};
    if (::inet_pton(AF_INET6, buf, &addr) != 1) {
        return false;
    }
    // s6_addr 是两个平台共有的字节字段; 网络字节序, 高字节在前
    std::array<std::uint8_t, 16> bytes {};
    std::memcpy(bytes.data(), addr.s6_addr, bytes.size());
    std::array<std::uint16_t, 8> segments {};
    for (std::size_t i = 0; i < segments.size(); ++i) {
        segments[i] = static_cast<std::uint16_t>((bytes[2 * i] << 8) | bytes[2 * i + 1]);
    }
    out = Ipv6Addr(segments[0], segments[1], segments[2], segments[3], segments[4], segments[5], segments[6],
                   segments[7]);
    return true;
}

// 端口: 必须整个是十进制数字, 范围 0 ~ 65535; 允许前导零 (net.md 4.1)。
std::uint16_t ParsePort(std::string_view text) {
    std::uint32_t port   = 0;
    const char*   begin  = text.data();
    const char*   end    = begin + text.size();
    const auto [ptr, ec] = std::from_chars(begin, end, port, 10);
    if (ec != std::errc {} || ptr != end || port > 65535) {
        throw std::invalid_argument("SocketAddr::Parse: invalid port");
    }
    return static_cast<std::uint16_t>(port);
}

}    // namespace

Ipv4Addr::Ipv4Addr(std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d) noexcept : octets_ {a, b, c, d} {}

Ipv4Addr Ipv4Addr::localhost() {
    return Ipv4Addr(127, 0, 0, 1);
}

Ipv4Addr Ipv4Addr::unspecified() {
    return Ipv4Addr(0, 0, 0, 0);
}

std::array<std::uint8_t, 4> Ipv4Addr::octets() const noexcept {
    return octets_;
}

bool Ipv4Addr::is_loopback() const noexcept {
    return octets_[0] == 127;
}

bool Ipv4Addr::is_unspecified() const noexcept {
    return octets_ == std::array<std::uint8_t, 4> {};
}

Ipv6Addr::Ipv6Addr(std::uint16_t s0,
                   std::uint16_t s1,
                   std::uint16_t s2,
                   std::uint16_t s3,
                   std::uint16_t s4,
                   std::uint16_t s5,
                   std::uint16_t s6,
                   std::uint16_t s7) noexcept
    : segments_ {s0, s1, s2, s3, s4, s5, s6, s7} {}

Ipv6Addr Ipv6Addr::localhost() {
    return Ipv6Addr(0, 0, 0, 0, 0, 0, 0, 1);
}

Ipv6Addr Ipv6Addr::unspecified() {
    return Ipv6Addr(0, 0, 0, 0, 0, 0, 0, 0);
}

std::array<std::uint16_t, 8> Ipv6Addr::segments() const noexcept {
    return segments_;
}

bool Ipv6Addr::is_loopback() const noexcept {
    for (std::size_t i = 0; i < segments_.size() - 1; ++i) {
        if (segments_[i] != 0) {
            return false;
        }
    }
    return segments_.back() == 1;
}

bool Ipv6Addr::is_unspecified() const noexcept {
    return segments_ == std::array<std::uint16_t, 8> {};
}

SocketAddr::SocketAddr(const Ipv4Addr& ip, std::uint16_t port) noexcept : ip_(ip), port_(port) {}

SocketAddr::SocketAddr(const Ipv6Addr& ip, std::uint16_t port) noexcept : ip_(ip), port_(port) {}

SocketAddr SocketAddr::Parse(std::string_view text) {
    if (!text.empty() && text.front() == '[') {
        // IPv6 形式: 第一个字符是 '[', 最后一个 ']' 后面必须紧跟 ':' 和端口 (net.md 4.1)
        const std::size_t close = text.rfind(']');
        if (close == std::string_view::npos || close + 1 >= text.size() || text[close + 1] != ':') {
            throw std::invalid_argument("SocketAddr::Parse: invalid IPv6 form");
        }
        Ipv6Addr addr;
        if (!ParseIpv6(text.substr(1, close - 1), addr)) {
            throw std::invalid_argument("SocketAddr::Parse: invalid IPv6 address");
        }
        return SocketAddr(addr, ParsePort(text.substr(close + 2)));
    }

    // IPv4 形式: 地址 ':' 端口, 按最后一个 ':' 切分; 地址里不允许出现 '[' 或 ']'
    const std::size_t colon = text.rfind(':');
    if (colon == std::string_view::npos) {
        throw std::invalid_argument("SocketAddr::Parse: missing ':' and port");
    }
    const std::string_view addr_text = text.substr(0, colon);
    if (addr_text.find('[') != std::string_view::npos || addr_text.find(']') != std::string_view::npos) {
        throw std::invalid_argument("SocketAddr::Parse: brackets not allowed in IPv4 address");
    }
    Ipv4Addr addr;
    if (!ParseIpv4(addr_text, addr)) {
        throw std::invalid_argument("SocketAddr::Parse: invalid IPv4 address");
    }
    return SocketAddr(addr, ParsePort(text.substr(colon + 1)));
}

bool SocketAddr::is_ipv4() const noexcept {
    return ip_.index() == 0;
}

bool SocketAddr::is_ipv6() const noexcept {
    return ip_.index() == 1;
}

std::uint16_t SocketAddr::port() const noexcept {
    return port_;
}

const Ipv4Addr& SocketAddr::ipv4() const noexcept {
    assert(is_ipv4());
    return std::get<Ipv4Addr>(ip_);
}

const Ipv6Addr& SocketAddr::ipv6() const noexcept {
    assert(is_ipv6());
    return std::get<Ipv6Addr>(ip_);
}

std::string SocketAddr::to_string() const {
    if (is_ipv4()) {
        const std::array<std::uint8_t, 4> o    = ipv4().octets();
        std::string                       text = std::to_string(o[0]);
        for (std::size_t i = 1; i < o.size(); ++i) {
            text += '.';
            text += std::to_string(o[i]);
        }
        text += ':';
        text += std::to_string(port_);
        return text;
    }

    // IPv6: inet_ntop 给出压缩写法, 如 "::1", 外面套方括号 (net.md 4.1)
    const std::array<std::uint16_t, 8> s = ipv6().segments();
    in6_addr                           addr {};
    for (std::size_t i = 0; i < s.size(); ++i) {
        addr.s6_addr[2 * i]     = static_cast<std::uint8_t>(s[i] >> 8);
        addr.s6_addr[2 * i + 1] = static_cast<std::uint8_t>(s[i] & 0xFF);
    }
    char buf[INET6_ADDRSTRLEN];
    if (::inet_ntop(AF_INET6, &addr, buf, sizeof(buf)) == nullptr) {
        return std::string();
    }
    std::string text(1, '[');
    text += buf;
    text += "]:";
    text += std::to_string(port_);
    return text;
}

}    // namespace zenio::net
