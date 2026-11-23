file(GLOB_RECURSE zenio_srcs CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
file(GLOB_RECURSE zenio_hdrs CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.h)

add_library(zenio ${ZENIO_LIB_TYPE} ${zenio_srcs} ${zenio_hdrs})
add_library(zenio::zenio ALIAS zenio)

set_target_properties(zenio PROPERTIES
        EXPORT_NAME "zenio"
        OUTPUT_NAME "zenio"
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR}
)

target_include_directories(zenio PRIVATE
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src>
        $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/src>
)

if (ZENIO_INSTALL)

    # 需要导出安装的头文件列表, 仅导出外部使用的 API 及其必须依赖导出的头文件, 其余内部实现的头文件不进此项
    target_sources(zenio
            PUBLIC
            FILE_SET ZENIO_HEADERS
            BASE_DIRS ${CMAKE_SOURCE_DIR}/src
            FILES
            # .. 待定
    )

    install(TARGETS zenio
            EXPORT zenioTargets
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
            FILE_SET ZENIO_HEADERS DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )

endif ()