add_executable(zenio_api_link_check api_link_check.cpp)

# 只链接共享库 zenio, 不链接 zenio_obj: 要查的就是库外需要的符号都导出了 (S31)
target_link_libraries(zenio_api_link_check PRIVATE GTest::gtest_main zenio::zenio)

target_include_directories(zenio_api_link_check PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_BINARY_DIR}/src
)

gtest_discover_tests(zenio_api_link_check
        DISCOVERY_MODE POST_BUILD
        PROPERTIES TIMEOUT 60
)
