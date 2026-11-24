#include "zenio/result.h"

#include <memory>
#include <system_error>
#include <utility>
#include <variant>

#include <gtest/gtest.h>

// 编译期检查 (error.md 第 7 节): 删除的函数在非模板上下文的 requires 表达式里是硬错误,
// 所以先把检查写进 concept。
namespace {

template <class R>
concept BoolContextuallyConvertible = requires(const R& r) { static_cast<bool>(r); };

template <class R>
concept HasConstValueOr = requires(const R& r) { r.value_or(typename R::OkType {}); };

template <class R>
concept HasRvalueValueOr = requires(R&& r) { std::move(r).value_or(typename R::OkType {}); };

static_assert(!std::is_convertible_v<zenio::Result<int>, bool>);
static_assert(BoolContextuallyConvertible<zenio::Result<int>>);
static_assert(!BoolContextuallyConvertible<zenio::Result<bool>>);
static_assert(!HasConstValueOr<zenio::Result<std::unique_ptr<int>>>);
static_assert(HasRvalueValueOr<zenio::Result<std::unique_ptr<int>>>);
static_assert(HasConstValueOr<zenio::Result<int>>);

}    // namespace

// design/error.md 第 7 节列出的用例

TEST(Result, DefaultIsEmpty) {
    const zenio::Result<int> r;
    EXPECT_TRUE(r.is_empty());
    EXPECT_FALSE(r.is_ok());
    EXPECT_FALSE(r.is_err());
    EXPECT_THROW((void)r.value(), std::bad_variant_access);
    EXPECT_THROW((void)r.error(), std::bad_variant_access);
}

TEST(Result, OkInt) {
    auto r = zenio::Result<int>::Ok(42);
    EXPECT_TRUE(r.is_ok());
    EXPECT_FALSE(r.is_empty());
    EXPECT_EQ(r.value(), 42);
    EXPECT_THROW((void)r.error(), std::bad_variant_access);
    if (r) {    // Result<int> 允许布尔转换
        SUCCEED();
    } else {
        FAIL() << "Ok 的 Result 应当转换为 true";
    }
}

TEST(Result, ErrInt) {
    const std::error_code err = std::make_error_code(std::errc::io_error);
    auto                  r   = zenio::Result<int>::Err(err);
    EXPECT_TRUE(r.is_err());
    EXPECT_EQ(r.error(), err);
    EXPECT_THROW((void)r.value(), std::bad_variant_access);
}

TEST(Result, VoidOk) {
    auto r = zenio::Result<void>::Ok();
    EXPECT_TRUE(r.is_ok());
    EXPECT_NO_THROW((void)r.value());
}

TEST(Result, VoidErr) {
    const std::error_code err = std::make_error_code(std::errc::io_error);
    auto                  r   = zenio::Result<void>::Err(err);
    EXPECT_TRUE(r.is_err());
    EXPECT_THROW((void)r.value(), std::bad_variant_access);
}

TEST(Result, SameOkAndErrType) {
    const std::error_code ec = std::make_error_code(std::errc::io_error);

    // 右值实参
    auto ok_rvalue = zenio::Result<std::error_code>::Ok(std::make_error_code(std::errc::io_error));
    EXPECT_TRUE(ok_rvalue.is_ok());
    EXPECT_EQ(ok_rvalue.value(), ec);
    auto err_rvalue = zenio::Result<std::error_code>::Err(std::make_error_code(std::errc::io_error));
    EXPECT_TRUE(err_rvalue.is_err());
    EXPECT_EQ(err_rvalue.error(), ec);

    // 左值实参
    auto ok_lvalue = zenio::Result<std::error_code>::Ok(ec);
    EXPECT_TRUE(ok_lvalue.is_ok());
    EXPECT_EQ(ok_lvalue.value(), ec);
    auto err_lvalue = zenio::Result<std::error_code>::Err(ec);
    EXPECT_TRUE(err_lvalue.is_err());
    EXPECT_EQ(err_lvalue.error(), ec);

    // Result<int, int> 同理
    int  seven  = 7;
    auto ok_int = zenio::Result<int, int>::Ok(std::move(seven));
    EXPECT_TRUE(ok_int.is_ok());
    EXPECT_EQ(ok_int.value(), 7);
    auto err_int = zenio::Result<int, int>::Err(7);
    EXPECT_TRUE(err_int.is_err());
    EXPECT_EQ(err_int.error(), 7);
    int  four          = 4;
    auto ok_int_lvalue = zenio::Result<int, int>::Ok(four);
    EXPECT_TRUE(ok_int_lvalue.is_ok());
    EXPECT_EQ(ok_int_lvalue.value(), 4);
}

TEST(Result, BoolPayloadMustBeExplicit) {
    auto r = zenio::Result<bool>::Ok(true);
    EXPECT_TRUE(r.is_ok());
    EXPECT_TRUE(r.value());    // 布尔载荷必须显式取值; 上下文转换已被删除 (见文件头的 static_assert)
}

TEST(Result, ValueOr) {
    auto ok = zenio::Result<int>::Ok(42);
    EXPECT_EQ(ok.value_or(-1), 42);

    const std::error_code err    = std::make_error_code(std::errc::io_error);
    auto                  failed = zenio::Result<int>::Err(err);
    EXPECT_EQ(failed.value_or(-1), -1);

    zenio::Result<int> empty;
    EXPECT_EQ(empty.value_or(-1), -1);
}

TEST(Result, DefaultErrorTypeIsErrorCode) {
    auto r = zenio::Result<int>::Err(std::make_error_code(std::errc::io_error));
    EXPECT_EQ(r.error(), std::make_error_code(std::errc::io_error));
}

TEST(Result, MoveOnlyPayload) {
    auto r = zenio::Result<std::unique_ptr<int>>::Ok(std::make_unique<int>(1));
    EXPECT_TRUE(r.is_ok());
    EXPECT_EQ(*r.value(), 1);
    auto v = std::move(r).value();
    EXPECT_EQ(*v, 1);

    auto failed   = zenio::Result<std::unique_ptr<int>>::Err(std::make_error_code(std::errc::io_error));
    auto fallback = std::move(failed).value_or(nullptr);
    EXPECT_EQ(fallback, nullptr);
}

TEST(Result, CopyAndMove) {
    auto a = zenio::Result<int>::Ok(42);
    auto b = a;
    EXPECT_TRUE(a.is_ok());
    EXPECT_TRUE(b.is_ok());
    EXPECT_EQ(b.value(), 42);

    auto c = std::move(b);
    EXPECT_TRUE(c.is_ok());
    EXPECT_EQ(c.value(), 42);

    const std::error_code err = std::make_error_code(std::errc::io_error);
    auto                  e1  = zenio::Result<int>::Err(err);
    auto                  e2  = e1;
    EXPECT_TRUE(e1.is_err());
    EXPECT_TRUE(e2.is_err());
    EXPECT_EQ(e2.error(), err);
}
