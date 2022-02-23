# libcogplaform-x11

pkg_check_modules(Epoxy IMPORTED_TARGET REQUIRED epoxy)
pkg_check_modules(COGPLATFORM_X11_DEPS IMPORTED_TARGET
    REQUIRED egl xcb xkbcommon-x11 x11-xcb)
find_package(WpeFDO REQUIRED)

add_library(cogplatform-x11 MODULE
    ../common/cog-gl-utils.c
    cog-platform-x11.c
)
set_target_properties(cogplatform-x11 PROPERTIES
    C_STANDARD 99
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/modules
)
target_compile_definitions(cogplatform-x11 PRIVATE G_LOG_DOMAIN=\"Cog-X11\")
target_link_libraries(cogplatform-x11 PRIVATE cogcore
    PkgConfig::Epoxy
    PkgConfig::COGPLATFORM_X11_DEPS
    PkgConfig::WebKit
    Wpe::FDO
)

install(TARGETS cogplatform-x11
    DESTINATION ${COG_MODULEDIR}
    COMPONENT "runtime"
)
