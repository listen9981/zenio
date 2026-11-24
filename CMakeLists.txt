cmake_minimum_required(VERSION 4.4)

project(zenio VERSION 0.0.1 LANGUAGES C CXX)

if (CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set_property(CACHE CMAKE_INSTALL_PREFIX
            PROPERTY VALUE "${CMAKE_SOURCE_DIR}"
    )
endif ()

message(STATUS "Install prefix: ${CMAKE_INSTALL_PREFIX}")

list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)

include(GNUInstallDirs)
include(FindPkgConfig)
include(CMakePackageConfigHelpers)

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.h.in
        ${PROJECT_BINARY_DIR}/src/zenio/version.h
        @ONLY
)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(ZENIO_BUILD_TESTS "Build test programs" OFF)
option(ZENIO_BUILD_EXAMPLES "Build example programs" OFF)
option(ZENIO_BUILD_BENCHMARKS "Build benchmark programs" OFF)
option(ZENIO_ENABLE_ASAN "Enable address sanitizer" OFF)
option(ZENIO_ENABLE_TSAN "Enable thread sanitizer" OFF)
option(ZENIO_BUILD_SHARED_LIBS "Build shared libraries" ON)
option(ZENIO_INSTALL "Install zenio" OFF)

# 全局编译标志, 只对 GCC 和 Clang 生效 (design/build.md 3.2)。
# 公开的类和函数用 ZENIO_EXPORT 导出, 其余符号在共享库里不可见 (S23)。
if (CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    add_compile_options(-Wall -Wextra -fvisibility=hidden)
endif ()

# ASAN 和 TSAN 互斥, 同时启用时优先 ASAN
if (ZENIO_ENABLE_ASAN)
    set(ZENIO_ENABLE_TSAN OFF)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
elseif (ZENIO_ENABLE_TSAN)
    set(ZENIO_ENABLE_ASAN OFF)
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
endif ()

if (ZENIO_BUILD_SHARED_LIBS OR BUILD_SHARED_LIBS)
    set(ZENIO_LIB_TYPE SHARED)
else ()
    set(ZENIO_LIB_TYPE STATIC)
endif ()

add_subdirectory(src)

if (ZENIO_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif ()

if (ZENIO_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif ()

if (ZENIO_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif ()

if (ZENIO_INSTALL)

    write_basic_config_version_file(
            "${CMAKE_CURRENT_BINARY_DIR}/zenio-config-version.cmake"
            VERSION ${PROJECT_VERSION}
            COMPATIBILITY AnyNewerVersion
    )

    configure_package_config_file(
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/zenio-config.cmake.in"
            "${CMAKE_CURRENT_BINARY_DIR}/zenio-config.cmake"
            INSTALL_DESTINATION lib/cmake/zenio
    )

    install(FILES
            "${CMAKE_CURRENT_BINARY_DIR}/zenio-config.cmake"
            "${CMAKE_CURRENT_BINARY_DIR}/zenio-config-version.cmake"
            DESTINATION lib/cmake/zenio
    )

    install(EXPORT zenioTargets
            FILE zenio-targets.cmake
            NAMESPACE zenio::
            DESTINATION lib/cmake/zenio
    )

endif ()
