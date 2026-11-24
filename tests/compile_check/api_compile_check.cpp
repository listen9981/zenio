#include "zenio/zenio.h"

#include <cstdio>
#include <utility>

zenio::Task<void> EchoConn(zenio::net::TcpStream stream) {
    char buf[4096];
    try {
        while (true) {
            std::size_t n = co_await stream.ReadSome(buf);    // 返回 0 表示对端关闭
            co_await stream.WriteAll({buf, n});
        }
    } catch (const zenio::CancelledError&) {
        // 被上层取消
    } catch (const zenio::SystemError&) {
        // ECONNRESET / EPIPE 之类的常规错误
    }
}

zenio::Task<void> EchoServer(zenio::net::SocketAddr addr) {
    auto listener = zenio::net::TcpListener::Bind(addr);
    auto sig      = zenio::signal::CtrlC();

    zenio::JoinSet<void> joins;
    try {
        while (true) {
            auto result = co_await zenio::Select(sig.Recv(), listener.Accept());
            if (result.index() == 0) {
                std::printf("SIGINT: stop accepting\n");
                joins.AbortAll();
                break;
            }
            auto [stream, peer] = std::get<1>(std::move(result));    // (TcpStream, SocketAddr), 与 tokio 一致
            joins.Spawn(EchoConn(std::move(stream)));
        }
    } catch (const zenio::CancelledError&) {
        // 被取消
    }

    co_await joins.JoinAll();
}

int main() {
    auto rt = zenio::runtime::Builder::CurrentThread().EnableIo().EnableTime().Build();
    // 多线程: zenio::runtime::Builder::MultiThread(4).EnableIo().EnableTime().Build();

    const zenio::net::SocketAddr addr(zenio::net::Ipv4Addr::localhost(), 7777);
    rt.BlockOn(EchoServer(addr));

    return 0;
}
