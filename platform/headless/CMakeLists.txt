
pkg_check_modules(WpeFDO IMPORTED_TARGET REQUIRED wpebackend-fdo-1.0>=1.8.0)
add_library(cogplatform-headless MODULE cog-platform-headless.c)
set_target_properties(cogplatform-headless PROPERTIES
    C_STANDARD 99
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

target_compile_definitions(cogplatform-headless PRIVATE G_LOG_DOMAIN=\"Cog-HEADLESS\")
target_link_libraries(cogplatform-headless PRIVATE
    cogcore
    PkgConfig::WpeFDO
)

install(TARGETS cogplatform-headless
    DESTINATION ${CMAKE_INSTALL_LIBDIR}
    COMPONENT "runtime"
)


add_library(WPEBackend-headless MODULE headless-backend.cpp)
set_target_properties(WPEBackend-headless PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden
)

pkg_check_modules(Wpe IMPORTED_TARGET REQUIRED wpe-1.0)
target_link_libraries(WPEBackend-headless PRIVATE
    PkgConfig::Wpe
)

install(TARGETS WPEBackend-headless
    DESTINATION ${CMAKE_INSTALL_LIBDIR}
    COMPONENT "runtime"
)

