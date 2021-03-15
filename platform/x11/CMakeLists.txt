# libcogplaform-x11

pkg_check_modules(COGPLATFORM_X11_DEPS IMPORTED_TARGET
    REQUIRED wpebackend-fdo-1.0>=1.6.0 egl xcb xkbcommon-x11)

add_library(cogplatform-x11 MODULE cog-platform-x11.c)
set_target_properties(cogplatform-x11 PROPERTIES
    C_STANDARD 99
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
target_compile_definitions(cogplatform-x11 PRIVATE G_LOG_DOMAIN=\"Cog-X11\")
target_link_libraries(cogplatform-x11 PRIVATE cogcore
    PkgConfig::COGPLATFORM_X11_DEPS
    PkgConfig::WebKit
)

install(TARGETS cogplatform-x11
    DESTINATION ${CMAKE_INSTALL_LIBDIR}
    COMPONENT "runtime"
)
