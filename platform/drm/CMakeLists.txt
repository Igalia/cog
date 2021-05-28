# libcogplatform-drm

pkg_check_modules(EGL IMPORTED_TARGET REQUIRED egl)
pkg_check_modules(WpeFDO IMPORTED_TARGET REQUIRED wpebackend-fdo-1.0>=1.4.0)
pkg_check_modules(LibDRM IMPORTED_TARGET REQUIRED libdrm>=2.4.71)
pkg_check_modules(LibGBM IMPORTED_TARGET REQUIRED gbm>=13.0)
pkg_check_modules(LibInput IMPORTED_TARGET REQUIRED libinput)
pkg_check_modules(LibUdev IMPORTED_TARGET REQUIRED libudev)
pkg_check_modules(WaylandServer IMPORTED_TARGET REQUIRED wayland-server)

add_library(cogplatform-drm MODULE
    cog-platform-drm.c
    kms.c
    cursor-drm.c
)
set_target_properties(cogplatform-drm PROPERTIES
    C_STANDARD 99
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
target_compile_definitions(cogplatform-drm PRIVATE G_LOG_DOMAIN=\"Cog-DRM\")
target_link_libraries(cogplatform-drm PRIVATE
    cogcore
    PkgConfig::EGL
    PkgConfig::LibDRM
    PkgConfig::LibGBM
    PkgConfig::LibInput
    PkgConfig::LibUdev
    PkgConfig::WaylandServer
    PkgConfig::WebKit
    PkgConfig::WpeFDO
)

install(TARGETS cogplatform-drm
    DESTINATION ${CMAKE_INSTALL_LIBDIR}
    COMPONENT "runtime"
)
