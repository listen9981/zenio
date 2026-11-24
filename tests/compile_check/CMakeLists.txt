add_library(zenio_api_compile_check OBJECT api_compile_check.cpp)

target_include_directories(zenio_api_compile_check PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_BINARY_DIR}/src
)
