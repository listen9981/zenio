#include "zenio/net/address.h"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

// net.md 第 7 节 M2 部分列出的用例

TEST(Ipv4Addr, OctetsAndLoopback) {
    const zenio::net::Ipv4Addr        a(127, 0, 0, 1);
    const std::array<std::uint8_t, 4> octets = a.octets();
    EXPECT_EQ(octets, (std::array<std::uint8_t, 4> {127, 0, 0, 1}));
    EXPECT_TRUE(a.is_loopback());
}

TEST(Ipv4Addr, LoopbackClassification) {
    EXPECT_TRUE(zenio::net::Ipv4Addr(127, 1, 2, 3).is_loopback());
    EXPECT_FALSE(zenio::net::Ipv4Addr(128, 0, 0, 1).is_loopback());
}

TEST(Ipv4Addr, LocalhostAndUnspecified) {
    EXPECT_EQ(zenio::net::Ipv4Addr::localhost(), zenio::net::Ipv4Addr(127, 0, 0, 1));
    const zenio::net::Ipv4Addr unspec = zenio::net::Ipv4Addr::unspecified();
    EXPECT_EQ(unspec.octets(), (std::array<std::uint8_t, 4> {0, 0, 0, 0}));
    EXPECT_TRUE(unspec.is_unspecified());
    EXPECT_FALSE(unspec.is_loopback());
}

TEST(Ipv6Addr, LocalhostAndUnspecified) {
    const zenio::net::Ipv6Addr         loopback = zenio::net::Ipv6Addr::localhost();
    const std::array<std::uint16_t, 8> segments = loopback.segments();
    for (std::size_t i = 0; i + 1 < segments.size(); ++i) {
        EXPECT_EQ(segments[i], 0);
    }
    EXPECT_EQ(segments.back(), 1);
    EXPECT_TRUE(loopback.is_loopback());

    const zenio::net::Ipv6Addr unspec = zenio::net::Ipv6Addr::unspecified();
    EXPECT_EQ(unspec.segments(), (std::array<std::uint16_t, 8> {0, 0, 0, 0, 0, 0, 0, 0}));
    EXPECT_TRUE(unspec.is_unspecified());
}

TEST(SocketAddr, Ipv4Construction) {
    const zenio::net::SocketAddr addr(zenio::net::Ipv4Addr::localhost(), 7777);
    EXPECT_TRUE(addr.is_ipv4());
    EXPECT_FALSE(addr.is_ipv6());
    EXPECT_EQ(addr.port(), 7777);
    EXPECT_TRUE(addr.ipv4().is_loopback());
}

TEST(SocketAddr, Ipv6Construction) {
    const zenio::net::SocketAddr addr(zenio::net::Ipv6Addr::localhost(), 7777);
    EXPECT_TRUE(addr.is_ipv6());
    EXPECT_FALSE(addr.is_ipv4());
    EXPECT_EQ(addr.port(), 7777);
    EXPECT_TRUE(addr.ipv6().is_loopback());
}

TEST(SocketAddr, ParseIpv4) {
    EXPECT_EQ(zenio::net::SocketAddr::Parse("127.0.0.1:7777"),
              zenio::net::SocketAddr(zenio::net::Ipv4Addr(127, 0, 0, 1), 7777));
}

TEST(SocketAddr, ParseIpv6) {
    EXPECT_EQ(zenio::net::SocketAddr::Parse("[::1]:7777"),
              zenio::net::SocketAddr(zenio::net::Ipv6Addr::localhost(), 7777));
}

TEST(SocketAddr, ParseUnspecifiedPortZero) {
    EXPECT_EQ(zenio::net::SocketAddr::Parse("0.0.0.0:0"),
              zenio::net::SocketAddr(zenio::net::Ipv4Addr::unspecified(), 0));
    // 端口允许前导零 (net.md 4.1)
    EXPECT_EQ(zenio::net::SocketAddr::Parse("0.0.0.0:00080"),
              zenio::net::SocketAddr(zenio::net::Ipv4Addr::unspecified(), 80));
}

TEST(SocketAddr, ParseInvalid) {
    EXPECT_THROW((void)zenio::net::SocketAddr::Parse(""), std::invalid_argument);
    EXPECT_THROW((void)zenio::net::SocketAddr::Parse("127.0.0.1"), std::invalid_argument);
    EXPECT_THROW((void)zenio::net::SocketAddr::Parse("127.0.0.1:"), std::invalid_argument);
    EXPECT_THROW((void)zenio::net::SocketAddr::Parse("127.0.0.1:70000"), std::invalid_argument);
    EXPECT_THROW((void)zenio::net::SocketAddr::Parse("256.1.1.1:80"), std::invalid_argument);
    EXPECT_THROW((void)zenio::net::SocketAddr::Parse("::1:80"), std::invalid_argument);
    EXPECT_THROW((void)zenio::net::SocketAddr::Parse("example.com:80"), std::invalid_argument);
    EXPECT_THROW((void)zenio::net::SocketAddr::Parse("[::1]"), std::invalid_argument);
}

// 解析的严格性 (net.md 第 7 节, M2 审查后补充)
TEST(SocketAddr, ParseStrictness) {
    // 方括号前有字符: '[' 必须是第一个字符
    EXPECT_THROW((void)zenio::net::SocketAddr::Parse("x[::1]:80"), std::invalid_argument);
    // 地址段前导零
    EXPECT_THROW((void)zenio::net::SocketAddr::Parse("010.0.0.1:80"), std::invalid_argument);
    EXPECT_THROW((void)zenio::net::SocketAddr::Parse("127.0.00.1:80"), std::invalid_argument);
    // 只有三段
    EXPECT_THROW((void)zenio::net::SocketAddr::Parse("127.0.1:80"), std::invalid_argument);
    // 以点结尾
    EXPECT_THROW((void)zenio::net::SocketAddr::Parse("1.2.3.4.:80"), std::invalid_argument);
}

TEST(SocketAddr, ToStringIpv4) {
    const zenio::net::SocketAddr addr(zenio::net::Ipv4Addr(127, 0, 0, 1), 7777);
    EXPECT_EQ(addr.to_string(), "127.0.0.1:7777");
}

TEST(SocketAddr, ToStringIpv6) {
    EXPECT_EQ(zenio::net::SocketAddr(zenio::net::Ipv6Addr::localhost(), 7777).to_string(), "[::1]:7777");

    // 2001:db8::1
    const zenio::net::Ipv6Addr doc(0x2001, 0x0db8, 0, 0, 0, 0, 0, 0x0001);
    EXPECT_EQ(zenio::net::SocketAddr(doc, 80).to_string(), "[2001:db8::1]:80");
}

TEST(SocketAddr, ParseTostringRoundtrip) {
    const zenio::net::SocketAddr addrs[] = {
        zenio::net::SocketAddr(zenio::net::Ipv4Addr(127, 0, 0, 1), 7777),
        zenio::net::SocketAddr(zenio::net::Ipv6Addr::localhost(), 7777),
        zenio::net::SocketAddr(zenio::net::Ipv6Addr(0x2001, 0x0db8, 0, 0, 0, 0, 0, 0x0001), 80),
    };
    for (const zenio::net::SocketAddr& addr : addrs) {
        EXPECT_EQ(zenio::net::SocketAddr::Parse(addr.to_string()), addr) << addr.to_string();
    }
}
