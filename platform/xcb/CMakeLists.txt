# libcogplaform-xcb

# libxcb-keysyms1-dev libxcb-image0-dev

pkg_check_modules(COGPLATFORM_XCB_DEPS IMPORTED_TARGET
    REQUIRED wpebackend-fdo-1.0>=1.6.0 xcb xcb-image xcb-keysyms)

add_library(cogplatform-xcb MODULE cog-platform-xcb.c)
set_target_properties(cogplatform-xcb PROPERTIES
    C_STANDARD 99
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
target_compile_definitions(cogplatform-xcb PRIVATE G_LOG_DOMAIN=\"Cog-XCB\")
target_link_libraries(cogplatform-xcb PRIVATE cogcore
    PkgConfig::COGPLATFORM_XCB_DEPS
    PkgConfig::WebKit
)

install(TARGETS cogplatform-xcb
    DESTINATION ${CMAKE_INSTALL_LIBDIR}
    COMPONENT "runtime"
)
