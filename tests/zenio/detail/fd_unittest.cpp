#include "zenio/detail/fd.h"

#include <cerrno>
#include <fcntl.h>
#include <memory>
#include <unistd.h>
#include <utility>

#include <gtest/gtest.h>

// net.md 第 7 节 M2 部分列出的用例

namespace {

// 建一个管道, 返回读端 fd; 写端立刻关闭。失败返回 -1。
int MakePipeReadEnd() {
    int fds[2];
    if (::pipe(fds) != 0) {
        return -1;
    }
    ::close(fds[1]);
    return fds[0];
}

bool FdIsOpen(int fd) {
    errno = 0;
    return ::fcntl(fd, F_GETFD) != -1 || errno != EBADF;
}

void ExpectClosed(int fd) {
    errno = 0;
    EXPECT_EQ(::fcntl(fd, F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);
}

}    // namespace

TEST(UniqueFd, HoldsPipeFd) {
    const int fd = MakePipeReadEnd();
    ASSERT_NE(fd, -1);
    {
        zenio::detail::UniqueFd ufd(fd);
        EXPECT_TRUE(ufd.is_valid());
        EXPECT_EQ(ufd.get(), fd);
    }
    ExpectClosed(fd);
}

TEST(UniqueFd, DestructorClosesFd) {
    const int fd = MakePipeReadEnd();
    ASSERT_NE(fd, -1);
    {
        zenio::detail::UniqueFd ufd(fd);
        (void)ufd;
    }
    ExpectClosed(fd);
}

TEST(UniqueFd, MoveConstructor) {
    const int fd = MakePipeReadEnd();
    ASSERT_NE(fd, -1);
    {
        auto                    src = std::make_unique<zenio::detail::UniqueFd>(fd);
        zenio::detail::UniqueFd dst(std::move(*src));
        EXPECT_FALSE(src->is_valid());
        EXPECT_EQ(src->get(), -1);
        EXPECT_TRUE(dst.is_valid());
        EXPECT_EQ(dst.get(), fd);

        src.reset();    // 源对象析构, 不应关闭 fd
        EXPECT_TRUE(FdIsOpen(fd));
    }    // 目标对象析构, fd 才被关闭
    ExpectClosed(fd);
}

TEST(UniqueFd, MoveAssignment) {
    const int fd1 = MakePipeReadEnd();
    ASSERT_NE(fd1, -1);
    const int fd2 = MakePipeReadEnd();
    ASSERT_NE(fd2, -1);
    {
        zenio::detail::UniqueFd a(fd1);
        zenio::detail::UniqueFd b(fd2);
        a = std::move(b);
        ExpectClosed(fd1);    // 目标对象原来持有的 fd 被关闭
        EXPECT_TRUE(a.is_valid());
        EXPECT_EQ(a.get(), fd2);    // 接管了源对象的 fd
        EXPECT_FALSE(b.is_valid());
        EXPECT_EQ(b.get(), -1);
        EXPECT_TRUE(FdIsOpen(fd2));
    }
    ExpectClosed(fd2);
}

TEST(UniqueFd, Release) {
    const int fd = MakePipeReadEnd();
    ASSERT_NE(fd, -1);
    {
        zenio::detail::UniqueFd ufd(fd);
        const int               released = ufd.release();
        EXPECT_EQ(released, fd);
        EXPECT_FALSE(ufd.is_valid());
        EXPECT_EQ(ufd.get(), -1);
        EXPECT_TRUE(FdIsOpen(fd));    // 对象还在, fd 未被关闭
    }    // 对象析构, 不关闭已释放的 fd
    EXPECT_TRUE(FdIsOpen(fd));
    ::close(fd);    // 测试最后手动关闭
    ExpectClosed(fd);
}

TEST(UniqueFd, Reset) {
    const int fd1 = MakePipeReadEnd();
    ASSERT_NE(fd1, -1);
    const int fd2 = MakePipeReadEnd();
    ASSERT_NE(fd2, -1);
    {
        zenio::detail::UniqueFd ufd(fd1);
        ufd.reset(fd2);
        ExpectClosed(fd1);    // 原来持有的 fd 被关闭
        EXPECT_TRUE(ufd.is_valid());
        EXPECT_EQ(ufd.get(), fd2);
    }
    ExpectClosed(fd2);
}

TEST(UniqueFd, ResetWithoutArgument) {
    const int fd = MakePipeReadEnd();
    ASSERT_NE(fd, -1);
    {
        zenio::detail::UniqueFd ufd(fd);
        ufd.reset();
        ExpectClosed(fd);
        EXPECT_FALSE(ufd.is_valid());
        EXPECT_EQ(ufd.get(), -1);
    }    // 再次析构不会重复关闭
}

TEST(UniqueFd, DefaultConstructorIsEmpty) {
    zenio::detail::UniqueFd ufd;
    EXPECT_EQ(ufd.get(), -1);
    EXPECT_FALSE(ufd.is_valid());
}    // 析构不关闭任何东西
