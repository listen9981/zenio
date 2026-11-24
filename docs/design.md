# ZENIO 设计文档

## 1. 概述

### 1.1 定位

ZENIO 是基于 C++20 协程的异步 IO 库, 面向 Linux (epoll) 和 macOS (kqueue)。v1 只做内核: 运行时、任务、取消、定时器、TCP/UDP 套接字和结构化并发。

API 以 tokio 为基准: 几乎所有 API 来自 tokio, 按本项目命名约定改写。底层直接使用 C++20 协程, 不移植 tokio, 不做 future/poll 桥接。个别其它项目里好用的 API 可以补充进来, 例如参照 bthread 的 `bthread_start_urgent` 设计的 `SpawnAndSeize`。

### 1.2 维护约定

文档分两层: 本文档是总览, 设计细则在 `docs/design/` 下的子系统文档里, 第 4 章是索引。每个里程碑另有一份任务书, 在 `docs/design/milestones/`。

设计与实现由不同的会话负责:

- **设计会话**负责设计、写文档、审查代码。
- **实现会话**按任务书实现。

所有会话共同遵守:

- 实现完成后, 如果实现与设计不一致, 在任务书的「实现偏差」里记下原因, 并修改对应子系统文档里受影响的章节, 两处改动和代码一起送审。
- 里程碑实现完成且验收测试全部通过后, 才能把「进度」里的状态改成已完成。
- 实现中留下债务 (已知的简化、未验证的部分、推迟的优化), 必须登记到「债务」; 清偿后移除对应条目。

实现会话额外遵守:

- 动手前先读本文档、对应的任务书, 以及任务书列出的子系统文档。
- 只做任务书范围内的事。发现设计有问题或有更好的做法, 记到「实现偏差」里, 不要自行扩大范围。
- 子系统文档里标「假设」的条目按原文实现; 觉得不对就记偏差。

文档状态:

- 子系统文档和任务书的开头标「草稿」或「已审查」。写完是草稿, 设计会话审过才标已审查。
- 只有已审查的任务书才能派给实现会话。

### 1.3 状态标记

| 标记 | 含义 |
|------|------|
| 已定 | 经过确认的设计决策 |
| 假设 | 尚未确认, 当前按此推进, 可以推翻 |
| 待定 | 尚未形成结论 |
| 草稿 | 文档写完但还没审查 |
| 已审查 | 设计会话审过, 可以作为实现依据 |

编号前缀: 已定决策 `S`, 假设 `A`, 待定事项 `O`, 债务 `D`, 里程碑 `M`。编号一经分配不再复用, 条目关闭后留空号。

### 1.4 已确定的设计决策

| 编号 | 决策 | 来源 |
|------|------|------|
| S1 | 后端: kqueue + epoll, 就绪模型。io_uring 不在 v1, 以后作为新后端加入 | 设计讨论中的选择 |
| S2 | 线程模型: 共享多线程调度器, work-stealing, 协程可在线程间迁移 | 设计讨论中的选择 |
| S3 | 错误模型: 异常。IO 失败抛 `SystemError`, 取消抛 `CancelledError` | 设计讨论中的选择 |
| S4 | v1 范围: 仅内核 | 设计讨论中的选择 |
| S5 | API 以 tokio 为基准: 几乎所有 API 来自 tokio, 按本项目命名约定改写; 可以补充其它项目里好用的 API | 讨论中的补充说明 |
| S6 | Runtime 自带线程: `Builder::CurrentThread()` / `Builder::MultiThread(n)`, 入口是 `BlockOn(task)` | 目标用法草图 |
| S7 | 信号不进 poller: 信号处理器 + 唤醒管道 | 设计讨论中的选择 |
| S8 | 不返回 `Future`, 不做 poll 桥接; 会挂起的操作返回 awaiter, 用 `co_await` 直接等; 不需要等待的系统调用 (bind、listen、setsockopt) 是普通函数, 失败抛异常 | 目标用法草图 |
| S9 | Poller 不占专门的线程, 是 Runtime 里的一个普通任务 (协程)。哪个 worker 运行它, 哪个 worker 就是在做 I/O (参照 brpc) | 设计讨论中的选择 |
| S10 | Poller 只在整个 Runtime 没有可运行任务时才阻塞等待 | 设计讨论中的选择 |
| S11 | `MultiThread(n)` 是 n 个线程, Poller 占用其中一个, 与"Poller 是普通任务"一致 | 设计讨论中的选择 |
| S12 | `Spawn` 就是 `StartBackground`: child 入队, 按需唤醒空闲 worker, 调用方继续运行 | 设计讨论中的选择 |
| S13 | `Spawn` / `SpawnAndSeize` 是全局函数 (对应 `tokio::spawn`); `JoinSet` 另外提供同名方法, 任务归集合管理 | 讨论中的补充说明 |
| S14 | `SpawnAndSeize` 只写成 awaitable: 让出当前 worker 给 child, 并把当前任务重新投递到队列。不提供函数形式 | 设计讨论中的选择 |
| S15 | 没人取结果的异常默认丢弃 (与 tokio 一致), 可以给 Runtime 配置处理器 | 设计讨论中的选择 |
| S16 | `JoinSet<T>` 带类型参数; `JoinAll` / `JoinNext` 的产出形态见 design/join_set.md; 任务的异常向外重抛, 取消向外传播, 不采用 tokio 逐个返回 `Result` 的做法 | 参考代码调整后确认 |
| S17 | `Result<T, E>` 三态 (空、Ok、Err), 默认 `E = std::error_code`; 运行时 API 不用它包装返回值, 给用户自己的可失败域、`Task` 产出的存储和 `Try` 用; `Try` 进 v1, 遇到 `CancelledError` 照样收进 `Err` | 参考代码调整后确认 |
| S18 | 类型名用 `Ipv4Addr` / `Ipv6Addr`, 与 Rust 拼写一致 | 待定事项的答复 |
| S19 | 网络接口的返回类型与 tokio 一致; `Accept()` 产出 `(TcpStream, SocketAddr)` | 待定事项的答复 |
| S20 | I/O 对象的生命周期按 tokio 的处理方式: 对象独占持有 fd, 挂起的操作必须先于对象结束 | 待定事项的答复 |
| S21 | epoll 在用户提供的 Linux 服务器上验证; 本机开发时不验证, 由用户后续验证 | 待定事项的答复 |
| S22 | 公开入口只有 `zenio.h`, 使用者 `#include <zenio/zenio.h>`; 不设顶层的 `export.h` | 待定事项的答复 |
| S23 | 符号可见性: 库按 `-fvisibility=hidden` 编译, 公开的类和函数标注 `ZENIO_EXPORT`, 宏定义在 `zenio/detail/export.h`; ABI: v1 不承诺 ABI 稳定, 只保证库和使用者用同一份源码一起编译 | 待定事项的答复 |
| S24 | 异常类型的名字一律以 `Error` 结尾, 一眼能认出是异常类型: `Error`、`SystemError`、`CancelledError`、`ElapsedError`。以后新增的异常类型照此 | 讨论中的补充说明 |
| S25 | 定时器直接移植 tokio 的时间轮 (传统实现, 在 `runtime/time/` 下), 精度 1 毫秒就够。tokio 源码位置: `~/workspace/from_git/tokio`。见 design/timer.md | 讨论中的补充说明 |
| S26 | 定时器不放进 Poller, 是独立的子系统: 时间轮加时间源单独成驱动。调度器的空闲等待路径按顺序驱动 Poller 和定时器, Poller 只管 fd | 讨论中的补充说明 |
| S27 | `Select` 的结果直接用 `std::variant`: 产出 `void` 的分支映射成 `std::monostate`; 至少 1 个、至多 64 个分支, 与 tokio 的 `select!` 一致。见 design/select.md | 讨论中的补充说明 |
| S28 | 单元测试直接链接库的目标文件 (OBJECT 库 `zenio_obj`), 不经过共享库; `zenio::detail` 里的东西一律不标 `ZENIO_EXPORT`。见 design/build.md 2.2、3.2 | 审查 M1、M2 后的决定 |
| S29 | ASAN 测试不降低检测项, 包括不关闭 container-overflow。遇到疑似误报先确认构建目录是干净的; 能复现就记录步骤并登记债务 | 审查 M1、M2 后的决定 |
| S30 | include 顺序里平台头单独一组: 项目头 → 标准库 → 平台头 → 第三方。`.clang-format` 还没有对应的分类, 暂不由工具强制。见 `docs/coding-style.md` | 审查 M1、M2 后的决定 |
| S31 | S28 的例外: 库外代码会直接调用的 `zenio::detail` 符号必须标 `ZENIO_EXPORT`, 否则使用者链接共享库时缺符号。两类: 公开头文件里的模板实现要调用的非模板函数; 各操作的 awaiter 类 (标在类上)。每个里程碑有一张清单, 验收时对照。见 design/build.md 2.3 | M3 细化时的发现, 原型编成共享库验证过 |

### 1.5 工作假设

以下各项未经确认, 当前按此推进。实现会话按原文实现。

- **A1 `BlockOn` 与 Runtime 生命周期**: 见 design/runtime.md。其中"只支持调用一次 `BlockOn`"已经由 A19 取代。
- **A2 `JoinSet` 与 `Select` 的语义**: 见 design/join_set.md、design/select.md。
- **A3 `EnableIo` / `EnableTime` 的规则**: 见 design/runtime.md。
- **A4 运行时上下文隐式获取**: 见 design/task.md。
- **A5 核心机制**: symmetric transfer、事件唤醒经调度器、Waiter 协议、绝不销毁未完成的协程帧、一次性注册。见 design/task.md、design/waiter.md。
- **A6 Poller 的实现细节**: 恢复 Poller 的时机、唤醒管道的使用。见 design/scheduler.md、design/poller.md。
- **A7 异常类型层级**: 见 design/error.md。
- **A8 任务派发的细节**: `JoinHandle` / `AbortHandle` 的成员、`SpawnAndSeize` 投递到哪个队列、`Handle::Spawn` 可在非 Runtime 线程调用。见 design/spawn.md。
- **A9 未取异常的处理器的配置入口**: 见 design/runtime.md、design/error.md。
- **A10 API 面的 v1 与以后的划分**: 见第 4 章。
- **A12 目录结构与里程碑顺序**: 见 1.7 与 2。
- **A13 `Result` 的成员细节与 `Try` 的实现**: 见 design/error.md。
- **A14 编译警告选项**: 库和测试都开启 `-Wall -Wextra`。见 design/build.md。
- **A15 SleepAwaiter 和定时器驱动的衔接**: `Wait`、`Cancel` 两个接口, `await_suspend` 里登记、注册 stop callback、挂起三步的顺序; 忙碌的 worker 每运行完一个任务检查一次到期的定时器。见 design/timer.md 4.7、design/scheduler.md 4.1。
- **A16 定时器的时间经过 `Clock` 接口取得**: 生产环境用真实时钟, 测试里换成可手动拨动的实现, 这样才能移植 tokio 里依赖可暂停时钟的测试。见 design/timer.md 1.1、2.2。

M3 细化时新增:

- **A17 Task 帧的所有权**: 帧归 `Task` 对象所有, 只在 `~Task()` 里销毁; `Awaiter::await_resume()` 只取走产出。debug 构建下断言: 一个 Task 只被 `co_await` 一次; 已启动未结束的 Task 不被销毁。见 design/task.md 2.4、第 3 节。
- **A18 上下文是线程里的一个指针**: 线程局部存储里放 `PromiseContext*`。Task 启动时把当前上下文复制进自己的 promise 并切换过去, 结束后等待方还原; 调度器恢复协程时装回它挂起时的那一份。见 design/task.md 2.2。
- **A19 `BlockOn` 的进入条件**: 线程上已经有上下文时抛 `std::runtime_error`, 同一个 Runtime 不能重入; 上一次返回之后可以再调。这取代 A1 里"v1 只支持调用一次 `BlockOn`"的说法。见 design/runtime.md 4.2。
- **A20 `Builder` 的线程数**: `workers_` 为 0 表示单线程, 默认构造的 `Builder` 等同 `CurrentThread()`; `MultiThread(n)` 的 `Build()` 在 M6 之前抛 `std::runtime_error`。见 design/runtime.md 第 2 节。
- **A21 `Handle::operator==`**: 比较是否指向同一个运行时。tokio 的 `Handle` 在提交 `1daf3c5a` 上没有这个比较, 是 zenio 补的: 单测要确认协程里拿到的句柄就是那个运行时, 没有它只能比"都有效"。见 design/runtime.md 第 2 节。
- **A22 M3 的调度器**: 只有一个先进先出的队列, 没有同步; `Schedule` 只能在运行时的线程上调用。M6 换成本地队列、全局队列和同步。见 design/scheduler.md 第 1 节。

### 1.6 待定事项

- **O6** `Builder` 各选项的默认值, 倾向与 tokio 一致。见 design/runtime.md。

### 1.7 架构总览

| 组件 | 职责 |
|------|------|
| Scheduler | n 个 worker, 各有一个 Chase-Lev 本地队列, 外加一个供外部线程投递的全局队列; 空闲时从别的 worker 偷任务; 取不到任务时去运行 Poller, 而不是休眠 |
| Poller | 一个普通任务 (协程), 独占 epoll/kqueue 实例和 fd 注册表; 对外只有命令队列和就绪分发两条通道 |
| 定时器驱动 | 移植自 tokio 的分层时间轮加时间源, 线程安全, 任意线程都能登记; 由调度器的空闲等待路径触发到期。不在 Poller 里 |
| 挂起协议 (Waiter) | 每个 `co_await` 点内的状态机, 用一个原子状态解决"完成和挂起同时发生"的竞争 |
| Driver | epoll 与 kqueue 的平台差异, 编译期选择 |

目录结构:

```
src/zenio/
  zenio.h            总入口, 使用者只包含它
  error.h/.cpp       Error / SystemError / CancelledError / ElapsedError
  result.h           Result<T, E>
  task.h/.cpp         Task<T> / Yield。Task 的成员都是模板, 定义在头文件里; .cpp 只有 Yield()
  try.h              Try
  cancel.h           Uncancellable
  spawn.h/.cpp       Spawn / SpawnAndSeize / JoinHandle / AbortHandle
  join_set.h/.cpp    JoinSet<T>
  select.h/.cpp      Select / SelectResult
  time.h/.cpp        Sleep / SleepUntil / Timeout
  runtime.h/.cpp     namespace zenio::runtime: Builder / Runtime / Handle
  signal.h/.cpp      namespace zenio::signal
  net/
    address.h/.cpp   SocketAddr / Ipv4Addr / Ipv6Addr
    tcp.h/.cpp       TcpListener / TcpStream
    udp.h/.cpp       UdpSocket
  detail/            内部实现。公开头文件依赖的五个 (export.h、context.h、awaiters.h、
                     fd.h、io_object.h) 随公开头一起导出, 其余不导出
    export.h         ZENIO_EXPORT 宏 (S23)
    context.h/.cpp   PromiseContext / InitialAwaiter / FinalAwaiter / YieldAwaiter, 线程上的当前上下文
    awaiters.h       各操作的 awaiter 类
    fd.h/.cpp        UniqueFd
    io_object.h      IoObject
    runtime_core.h/.cpp   RuntimeCore: 运行时的内部对象
    scheduler.*      work-stealing 调度器 (M3 先做单线程的队列和循环)
    run_queue.*      Chase-Lev 双端队列
    park.*           线程休眠与唤醒
    poller.*         Poller 协程: 命令队列与轮询循环
    timer.*          TimerDriver / TimerEntry
    time_wheel.*     TimerWheel / TimerLevel: 移植 tokio 的时间轮
    time_source.*    Clock / TimeSource
    intrusive_list.h 侵入式双向链表
    waiter.h/.cpp    挂起协议
    driver.h  driver_epoll.cpp  driver_kqueue.cpp
tests/
  zenio/             单元测试, 目录与 src/zenio 同构, 文件名 *_unittest.cpp
  compile_check/     只编译不链接的 API 编译检查
  link_check/        链接共享库并运行的链接检查
```

### 1.8 目标用法

由目标用法草图整理, 以子系统文档为准。这段代码同时是 M1 的编译检查用例。

```cpp
zenio::Task<void> EchoConn(zenio::net::TcpStream stream) {
    char buf[4096];
    try {
        while (true) {
            size_t n = co_await stream.ReadSome(buf);   // 返回 0 表示对端关闭
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
    auto sig = zenio::signal::CtrlC();

    zenio::JoinSet<void> joins;
    try {
        while (true) {
            auto result = co_await zenio::Select(sig.Recv(), listener.Accept());
            if (result.index() == 0) {
                std::printf("SIGINT: stop accepting\n");
                joins.AbortAll();
                break;
            }
            auto [stream, peer] = std::get<1>(std::move(result));   // (TcpStream, SocketAddr), 与 tokio 一致
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
```

相对原始草图有两处修正: `TcpStream` 在 `zenio::net` 里 (与 tokio 的 `tokio::net::TcpStream` 对应); `Accept()` 产出 `(TcpStream, SocketAddr)`。

## 2. 进度

设计: 顶层决策已定 (1.4)。M1、M2、M3 的任务书已审查, 可以派给实现会话。子系统文档的状态见 4.1: M1、M2、M3 涉及的部分已经细化, 其余还是草稿, 实现会话按原文实现, 觉得不对就记偏差。

状态取值: 未开始、进行中、已完成。里程碑实现完成且验收测试全部通过后才能标为已完成。

| 编号 | 里程碑 | 验收标准 | 任务书 | 状态 |
|------|--------|----------|--------|------|
| M1 | 公共头文件: `zenio.h`、`detail/export.h`、`detail/context.h` 和全部公开头文件的声明, 不含实现 | API 编译检查通过: 1.8 的目标用法只凭 `<zenio/zenio.h>` 能编译, 不要求链接 | design/milestones/M1.md (已审查) | 已完成 (R1 复验)。实现会话自述的验证: `cmake -S . -B cmake-build-m1 -G Ninja -DCMAKE_BUILD_TYPE=Debug -DZENIO_BUILD_TESTS=ON && cmake --build cmake-build-m1`, 三个目标 (`zenio`、`zenio_unittest`、`zenio_api_compile_check`) 全部构建通过; 反向检查 (0/65 个分支、引用产出分支、`Result<bool>` 布尔转换、`JoinSet` 任务类型、`SpawnAndSeize` 漏写 `co_await` 的警告) 全部符合预期 |
| M2 | 基础设施的实现: `error`、`result`、`detail/fd`、`net/address`, 以及构建配置 (符号可见性、警告选项、FILE_SET、测试目录) | 各子系统文档列出的单测全部通过; API 编译检查仍然通过; D2 清偿 | design/milestones/M2.md (已审查) | 已完成 (R1 复验)。实现会话自述的验证: `cmake -S . -B cmake-build-m2 -G Ninja -DCMAKE_BUILD_TYPE=Debug -DZENIO_BUILD_TESTS=ON && cmake --build cmake-build-m2 && ctest --test-dir cmake-build-m2 --output-on-failure`, 40 个用例全部通过; `-Wall -Wextra` 下零警告; `cmake -S . -B cmake-build-install -G Ninja -DZENIO_INSTALL=ON` 配置成功 |
| R1 | M1、M2 审查返工: `Result` 的构造函数、测试改为链接目标文件、ASAN 设置等 | 见 design/milestones/R1.md 第 5 节 | design/milestones/R1.md (已审查) | 已完成。验证: `cmake -S . -B cmake-build-r1 -G Ninja -DCMAKE_BUILD_TYPE=Debug -DZENIO_BUILD_TESTS=ON && cmake --build cmake-build-r1 && ctest --test-dir cmake-build-r1 --output-on-failure` (42 项全过, 四个目标 `zenio_obj`/`zenio`/`zenio_unittest`/`zenio_api_compile_check`, 零警告); `nm -gUC cmake-build-r1/src/zenio/libzenio*.dylib \| awk '{print $3}' \| grep '^zenio::detail::'` 无输出 (R1 当时写的命令 `nm -gU ... \| grep 'zenio::detail'` 匹配不上修饰过的名字, 是条空检查; M3 细化时用这条正确的命令重查过, 结论不变); 全新目录 `-DZENIO_ENABLE_ASAN=ON` 构建后不设 `ASAN_OPTIONS` 直接运行单测, 42 项全过无报告; `-DZENIO_BUILD_SHARED_LIBS=OFF` 构建且 ctest 通过; `-DZENIO_INSTALL=ON` 配置成功; 改动文件 clang-format 无差异; `Result<bool>` 布尔转换与 `Select()` 反向检查仍不能编译 |
| M3 | Task 与单线程 Runtime: `Task<T>`、`Try`、`Builder::CurrentThread()`、`BlockOn`、`Handle`、上下文传播、单线程调度循环、`Waiter` 的协议 | 10 万层嵌套 `co_await` 不爆栈 (在一个 512 KB 栈的线程上跑, Debug 和 Release 都要过); 异常在 `co_await` 处原样重抛; `Try` 把任务异常收进 `Err`, `CancelledError` 也被收进去; 共享库导出的 `zenio::detail` 符号恰好是 S31 清单里的三个 | design/milestones/M3.md (已审查) | 未开始 |
| M4 | 定时器 (移植 tokio 时间轮)、kqueue 与 Poller (单线程模式): `Sleep`、TCP 的 accept / connect / read / write | 时间轮的单元测试 (移植自 tokio) 通过; `Sleep` 的精度和取消测试通过; loopback 回显通过; 上千并发连接通过; ASAN 下无泄漏 | 未写, M3 完成前细化 | 未开始 |
| M5 | 任务派发与取消: `Spawn`、`SpawnAndSeize`、`JoinHandle`、`AbortHandle`、`JoinSet`、`Select`、`Uncancellable` | 取消挂起中的 read 成功; `SpawnAndSeize` 的 child 在调用方的 worker 上启动而调用方被重新投递; `JoinHandle` 丢弃后任务继续运行; `JoinAll` 重抛任务异常且结果按 spawn 顺序排列; `Select` 的输家被取消; 析构后没有残留任务 | 未写, M4 完成前细化 | 未开始 |
| M6 | `Builder::MultiThread(n)`: Chase-Lev 队列、偷任务、空闲 worker 运行 Poller、`Handle::Spawn` 可从非 Runtime 线程调用 | 4 个 worker 上跑完 10 万个任务; TSAN 下全量单测通过 | 未写, M5 完成前细化 | 未开始 |
| M7 | `signal::CtrlC()` | 进程内 `raise(SIGINT)` 后 `Recv()` 返回; 无人在等待时到达的信号不丢 | 未写, M6 完成前细化 | 未开始 |
| M8 | epoll、UDP、示例、基准 | 在用户提供的 Linux 服务器上全量单测通过 (由用户执行, S21); echo server 示例可运行; 连接数与吞吐基准可运行 | 未写, M7 完成前细化 | 未开始 |

## 3. 债务

来源为"设计取舍"的条目是对应里程碑实现后生效的已知简化。

| 编号 | 债务 | 来源 | 处理 |
|------|------|------|------|
| D3 | epoll 后端在开发机 (macOS) 上无法验证 | 环境 | 在用户提供的 Linux 服务器上验证 (S21); 验证通过之前 epoll 标记为未验证 |
| D4 | 每次等待都重新 arm (一次性注册), 比边缘触发加就绪提示多一次系统调用 | 设计取舍 | 基准显示成为瓶颈时再改 |
| D5 | 协程帧用默认 `operator new` / `delete`, 没有内存池 | 设计取舍 | 同上 |
| D6 | 唤醒通道用管道 (两个 fd, 每次唤醒一次 write 和一次 read), 没有用 eventfd / `EVFILT_USER` | 设计取舍 | 同上 |
| D7 | 线程休眠与唤醒用标准库同步原语, 没有用 futex / ulock | 设计取舍 | 同上 |
| D8 | MultiThread 模式下 `BlockOn` 的调用线程只等待, 不参与调度 (tokio 的调用线程参与) | 设计取舍 | 需要时再改 |
| D9 | `Select` 每个分支包一个子协程, 每次调用每个分支一次帧分配; 在循环里反复 `Select` (如 accept 循环) 成本明显 | 设计取舍 | 需要时再做可复用的分支对象 |
| D10 | `JoinSet` 每次 `Spawn` 一次堆分配 | 设计取舍 | 需要时再改 |
| D11 | 满载时 Poller 排队等待 worker, 没有线程在轮询, I/O 事件要等有 worker 空出来才被处理。定时器不受影响, 忙碌的 worker 会顺手处理 (A15) | 设计取舍 | 需要时让 worker 每运行 N 个任务做一次非阻塞 Poll |
| D12 | 信号是进程级的: 处理器一旦安装不再卸载, 唤醒管道不关闭; v1 假定一个进程只有一个 Runtime 使用信号 | 设计取舍 | 需要支持多 Runtime 时再处理 |
| D13 | `Try` 每次调用多一层协程帧 (组合子本身是一个 `Task`) | 设计取舍 | 需要时再改 |
| D14 | 对称交接的尾调用和 S31 的导出清单只在 Clang 上验证过, 开发机 (macOS) 上没有 GCC。GCC 的协程在低优化级别下是否做尾调用不确定, 10 万层嵌套在它上面可能溢出 | 环境 | M8 在用户提供的 Linux 服务器上验证: 512 KB 栈的嵌套测试在 Debug 和 Release 下都要过, 导出符号的清单要对得上 (S21)。不成立的话再定处理方案 |

## 4. 设计细则

### 4.1 子系统文档索引

每个子系统文档有固定的七节: 职责与边界、对外接口、内部数据结构、关键流程、不变量、已知的坑、验收测试。「细化程度」一栏写明哪些节已经可以直接照着实现。

| 子文档 | 内容 | 细化程度 | 状态 |
|--------|------|----------|------|
| design/error.md | 异常类型、`Result`、`Try` | 全部已细化 (M1、M2) | 草稿 |
| design/net.md | 地址、`UniqueFd`、`IoObject`、TCP、UDP | 地址、`UniqueFd`、`IoObject` 全部已细化 (M1、M2); 套接字的接口已细化 (M1), 内部是概要 (M4 前细化) | 草稿 |
| design/build.md | 构建、符号导出、测试 | 全部已细化 (M1、M2) | 草稿 |
| design/task.md | `Task<T>`、上下文传播、awaiter 约定、`Yield` | 全部已细化 (M1 写声明, M3 写实现); `seize_caller_` 的重新投递在 M5 细化 | 草稿 |
| design/runtime.md | `Runtime`、`Builder`、`Handle` | `CurrentThread` 形态全部已细化 (M3); Poller、定时器驱动、游离任务 (M4、M5), 多线程部分 M6 前细化 | 草稿 |
| design/spawn.md | `Spawn`、`SpawnAndSeize`、`JoinHandle`、`AbortHandle` | 接口已细化 (M1); 内部是概要 (M5 前细化) | 草稿 |
| design/join_set.md | `JoinSet<T>` | 接口已细化 (M1); 内部是概要 (M5 前细化) | 草稿 |
| design/select.md | `Select`、`SelectResult` | 接口已细化 (M1); 内部是概要 (M5 前细化) | 草稿 |
| design/signal.md | `Signal`、`CtrlC()` | 接口已细化 (M1); 内部是概要 (M7 前细化) | 草稿 |
| design/waiter.md | 挂起协议、取消、`Uncancellable` | Waiter 的协议和实现已细化 (M3); 取消流程是概要 (M5 前细化) | 草稿 |
| design/scheduler.md | 调度器、队列、休眠唤醒 | 单线程部分全部已细化 (M3), 多线程部分 M6 前细化 | 草稿 |
| design/poller.md | Poller、Driver、fd 注册表 | 概要 (M4 前细化, epoll 部分 M8 前) | 草稿 |
| design/timer.md | 定时器: 移植 tokio 的时间轮、时间源、条目状态机 | 移植指南已细化 (M4); `SleepAwaiter` 和驱动的衔接是假设 (A15) | 草稿 |

### 4.2 API 面

以 tokio 为准 (S5), v1 与以后的划分是假设 (A10)。各 API 的完整声明在对应的子系统文档里。

| 模块 | v1 | 对应的 tokio API | 以后 | 子系统文档 |
|------|----|------------------|------|------------|
| `runtime` | `Builder::CurrentThread()`、`Builder::MultiThread(n)`、`EnableIo()`、`EnableTime()`、`EnableAll()`、`Build()`; `Runtime::BlockOn`、`Runtime::Spawn`; `Handle::Current()`、`TryCurrent()`、`Enter()`、`Spawn()`、`operator==` | `runtime::Builder` 的 `new_current_thread`、`new_multi_thread`、`worker_threads`、`enable_io`、`enable_time`、`enable_all`、`build`; `Runtime` 的 `block_on`、`spawn`; `Handle` 的 `current`、`try_current`、`enter`、`spawn` | `SpawnBlocking`、线程名等 Builder 选项、`shutdown_background` | design/runtime.md |
| `net` | 见 design/net.md | `net::TcpListener`、`TcpStream`、`UdpSocket`; `io::AsyncReadExt`、`AsyncWriteExt` 的读写方法 | 主机名形态 (`ToSocketAddrs`、`lookup_host`)、`Readable` / `Writable` (`ready`)、组播、Unix socket | design/net.md |
| `time` | `Sleep`、`SleepUntil`、`Timeout` | `time::sleep`、`sleep_until`、`timeout`; 定时器本体移植自 `runtime/time/` | `Interval`、可暂停的时钟 (`pause`) | design/select.md、design/timer.md |
| `task` | `Task<T>`; `Spawn`、`SpawnAndSeize`; `JoinHandle<T>`、`AbortHandle`; `JoinSet<T>`: `Spawn`、`SpawnAndSeize`、`JoinNext`、`JoinAll`、`AbortAll`、`len`、`is_empty`; `Yield` | `task::spawn`、`JoinHandle`、`AbortHandle`、`JoinSet` 的同名方法、`yield_now` | `SpawnBlocking`、`JoinSet::DetachAll`、任务名 | design/spawn.md、design/join_set.md、design/task.md |
| 组合 | `Select`; `Try` | `select!` | `join!`、`try_join!` 那种并发等待一组任务 | design/select.md、design/error.md |
| `signal` | `CtrlC()` 返回的信号流, `Recv()` | `signal::ctrl_c` | 任意信号的流 (`signal::unix::Signal`) | design/signal.md |
| `sync` | 无 | 无 | `Channel`、`oneshot`、`Notify`、异步 `Mutex` (`sync` 模块) | 无 |
| 工具 | `Result<T, E>`; `Uncancellable` | 无 (对应 Rust 标准库的 `Result`) | `Result` 上的组合子 | design/error.md、design/waiter.md |

补充的非 tokio API: `SpawnAndSeize`、`JoinSet::SpawnAndSeize`、`Result`、`Try`、`Uncancellable`。

### 4.3 跨子系统的关键路径

这几条路径跨越多个子系统, 各自的详细步骤写在箭头后面列出的文档里。

1. **子任务完成**: `co_await child` 时, child 的 `final_suspend` 返回父句柄, 同线程原地恢复父协程, 不经过调度器。见 design/task.md。
2. **I/O 等待到就绪**: awaiter 投 arm 命令并挂起; 空闲的 worker 运行 Poller, Poller 得到就绪事件后把协程交给调度器; worker 接着恢复它。见 design/poller.md、design/waiter.md。
3. **取消**: 任意线程 `request_stop()`, Poller 摘掉注册后唤醒协程, 协程抛 `CancelledError`。见 design/waiter.md。
4. **SpawnAndSeize**: 调用方挂起, child 在同一个 worker 上启动, child 把调用方重新投递到本地队列。见 design/spawn.md。
5. **空闲时等待**: worker 取不到任务时, 向定时器驱动要最近一个到期时刻作为阻塞上限, 运行 Poller (或者按上限休眠), 醒来之后处理到期的定时器, 回到循环。见 design/scheduler.md、design/poller.md、design/timer.md。
6. **定时器到期**: awaiter 向定时器驱动登记并挂起; 驱动推进时间轮, 到期的条目在解锁之后被 `Signal`。见 design/timer.md。
