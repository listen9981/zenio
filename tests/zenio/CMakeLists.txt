file(GLOB_RECURSE zenio_unittest_srcs CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
file(GLOB_RECURSE zenio_unittest_hdrs CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.h)

add_executable(zenio_unittest ${zenio_unittest_srcs} ${zenio_unittest_hdrs})

target_include_directories(zenio_unittest PRIVATE
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src>
        $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/src>
)

target_link_libraries(zenio_unittest PRIVATE GTest::gtest_main zenio::zenio)
