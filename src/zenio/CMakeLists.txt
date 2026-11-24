file(GLOB_RECURSE zenio_srcs CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
file(GLOB_RECURSE zenio_hdrs CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.h)

# S28: 全部源文件只编译一次 (zenio_obj), 库和单元测试共用这份目标文件,
# 单测直接链接 zenio_obj, 不经过共享库 (design/build.md 2.2、3.2)。
add_library(zenio_obj OBJECT ${zenio_srcs} ${zenio_hdrs})

add_library(zenio ${ZENIO_LIB_TYPE} $<TARGET_OBJECTS:zenio_obj>)
add_library(zenio::zenio ALIAS zenio)

set_target_properties(zenio PROPERTIES
        EXPORT_NAME "zenio"
        OUTPUT_NAME "zenio"
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR}
)

target_include_directories(zenio_obj PRIVATE
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src>
        $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/src>
)

if (ZENIO_INSTALL)

    # 公开头文件清单 (design/build.md 2.4)。
    # version.h 在构建目录里, 所以 BASE_DIRS 除 src/ 之外还要加上 ${CMAKE_BINARY_DIR}/src。
    target_sources(zenio
            PUBLIC
            FILE_SET zenio_headers
            TYPE HEADERS
            BASE_DIRS ${CMAKE_SOURCE_DIR}/src ${CMAKE_BINARY_DIR}/src
            FILES
            ${CMAKE_SOURCE_DIR}/src/zenio/zenio.h
            ${CMAKE_SOURCE_DIR}/src/zenio/error.h
            ${CMAKE_SOURCE_DIR}/src/zenio/result.h
            ${CMAKE_SOURCE_DIR}/src/zenio/task.h
            ${CMAKE_SOURCE_DIR}/src/zenio/try.h
            ${CMAKE_SOURCE_DIR}/src/zenio/cancel.h
            ${CMAKE_SOURCE_DIR}/src/zenio/spawn.h
            ${CMAKE_SOURCE_DIR}/src/zenio/join_set.h
            ${CMAKE_SOURCE_DIR}/src/zenio/select.h
            ${CMAKE_SOURCE_DIR}/src/zenio/time.h
            ${CMAKE_SOURCE_DIR}/src/zenio/runtime.h
            ${CMAKE_SOURCE_DIR}/src/zenio/signal.h
            ${CMAKE_SOURCE_DIR}/src/zenio/net/address.h
            ${CMAKE_SOURCE_DIR}/src/zenio/net/tcp.h
            ${CMAKE_SOURCE_DIR}/src/zenio/net/udp.h
            ${CMAKE_SOURCE_DIR}/src/zenio/detail/export.h
            ${CMAKE_SOURCE_DIR}/src/zenio/detail/context.h
            ${CMAKE_SOURCE_DIR}/src/zenio/detail/awaiters.h
            ${CMAKE_SOURCE_DIR}/src/zenio/detail/fd.h
            ${CMAKE_SOURCE_DIR}/src/zenio/detail/io_object.h
            ${CMAKE_BINARY_DIR}/src/zenio/version.h
    )

    install(TARGETS zenio
            EXPORT zenioTargets
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
            FILE_SET zenio_headers DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )

endif ()