file(GLOB_RECURSE zenio_unittest_srcs CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
file(GLOB_RECURSE zenio_unittest_hdrs CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.h)

add_executable(zenio_unittest ${zenio_unittest_srcs} ${zenio_unittest_hdrs})

target_include_directories(zenio_unittest PRIVATE
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src>
        $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/src>
)

# S28: 单测直接链接库的目标文件, 不经过共享库, 因此可以使用 zenio::detail 里的内部实体
target_link_libraries(zenio_unittest PRIVATE GTest::gtest_main zenio_obj)

# 每个用例在 ctest 里单独一项。不设置 ASAN_OPTIONS 之类的 sanitizer 环境变量 (S29)
gtest_discover_tests(zenio_unittest
        DISCOVERY_MODE POST_BUILD
        PROPERTIES TIMEOUT 60
)
