# libcogplatform-drm

pkg_check_modules(Epoxy IMPORTED_TARGET REQUIRED epoxy)
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
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/modules
)
target_link_libraries(cogplatform-drm PRIVATE
    cogcore
    PkgConfig::Epoxy
    PkgConfig::LibDRM
    PkgConfig::LibGBM
    PkgConfig::LibInput
    PkgConfig::LibUdev
    PkgConfig::WaylandServer
    PkgConfig::WebKit
    PkgConfig::WpeFDO
)

string(REGEX MATCH "^([0-9]+)\.([0-9]+)\.([0-9]+)" _ "${LibInput_VERSION}")
target_compile_definitions(cogplatform-drm PRIVATE
    G_LOG_DOMAIN=\"Cog-DRM\"
    LIBINPUT_VER_MAJOR=${CMAKE_MATCH_1}
    LIBINPUT_VER_MINOR=${CMAKE_MATCH_2}
    LIBINPUT_VER_MICRO=${CMAKE_MATCH_3}
)

install(TARGETS cogplatform-drm
    DESTINATION ${COG_MODULEDIR}
    COMPONENT "runtime"
)
