cmake_minimum_required (VERSION 3.3)
project(dinghy VERSION 0.1.0 LANGUAGES C)

include(GNUInstallDirs)

option(DY_DBUS_SYSTEM_BUS "Expose remote control interface on system bus" OFF)
set(DY_DBUS_OWN_USER "" CACHE STRING
    "Additional user allowed to own the well-known name on the system bus")

option(DY_USE_WEBKITGTK "Use WebKitGTK+ instead of WPEWebKit" OFF)
option(DY_USE_MODE_MONITOR "Use the SysFS graphics mode monitor" OFF)
set(DY_USE_DRM_MODE_MONITOR OFF CACHE BOOL "Enable the DRM/KMS graphics mode monitor")
set(DY_APPID "" CACHE STRING "Default GApplication unique identifier")
set(DY_HOME_URI "" CACHE STRING "Default home URI")

if (NOT DY_APPID OR DY_APPID STREQUAL "")
    if (DY_USE_WEBKITGTK)
        set(DY_DEFAULT_APPID com.igalia.DinghyGtk)
    else ()
        set(DY_DEFAULT_APPID com.igalia.Dinghy)
    endif ()
else ()
    set(DY_DEFAULT_APPID ${DY_APPID})
endif ()

if (DY_HOME_URI AND NOT DY_HOME_URI STREQUAL "")
    set(DY_DEFAULT_HOME_URI ${DY_HOME_URI})
endif ()

if (BUILD_SHARED_LIBS)
    set(DINGHYCORE_COMPONENT "runtime")
else()
    set(DINGHYCORE_COMPONENT "development")
endif()

add_definitions(-DDY_INSIDE_DINGHY__=1)
add_definitions(-DG_LOG_DOMAIN=\"Dy\")

configure_file(dy-config.h.in dy-config.h @ONLY)
include_directories(${CMAKE_CURRENT_BINARY_DIR})

find_package(PkgConfig)

# libdinghycore
set(DINGHYCORE_API_HEADERS
    dinghy.h
    dy-launcher.h
    dy-platform.h
    dy-request-handler.h
    dy-directory-files-handler.h
    dy-utils.h
    dy-webkit-utils.h
    ${CMAKE_CURRENT_BINARY_DIR}/dy-config.h
)
set(DINGHYCORE_SOURCES
    dy-launcher.c
    dy-platform.c
    dy-request-handler.c
    dy-directory-files-handler.c
    dy-utils.c
    dy-webkit-utils.c
)

pkg_check_modules(GIO REQUIRED gio-2.0)
pkg_check_modules(SOUP REQUIRED libsoup-2.4)
if (DY_USE_WEBKITGTK)
    list(APPEND DINGHYCORE_SOURCES dy-gtk-utils.c)
    pkg_check_modules(WEB_ENGINE REQUIRED webkit2gtk-4.0)
else ()
    pkg_check_modules(WEB_ENGINE REQUIRED wpe-webkit-0.1 wpe-0.1)
endif ()

set(DINGHYCORE_INCLUDE_DIRS ${WEB_ENGINE_INCLUDE_DIRS} ${SOUP_INCLUDE_DIRS})
set(DINGHYCORE_LDFLAGS ${WEB_ENGINE_LDFLAGS} ${SOUP_LDFLAGS})


if (DY_USE_MODE_MONITOR)
    list(APPEND DINGHYCORE_SOURCES
        dy-mode-monitor.c
        dy-sysfs-mode-monitor.c
    )
    list(APPEND DINGHYCORE_API_HEADERS
        dy-mode-monitor.h
        dy-sysfs-mode-monitor.h
    )
    # If libudev is available, build also de DRM/KMS mode monitor.
    pkg_check_modules(UDEV libudev)
    pkg_check_modules(LIBDRM libdrm)
    if (UDEV_FOUND AND LIBDRM_FOUND)
        set(DY_USE_DRM_MODE_MONITOR ON)
        list(APPEND DINGHYCORE_SOURCES dy-drm-mode-monitor.c)
        list(APPEND DINGHYCORE_API_HEADERS dy-drm-mode-monitor.h)
        list(APPEND DINGHYCORE_INCLUDE_DIRS ${UDEV_INCLUDE_DIRS} ${LIBDRM_INCLUDE_DIRS})
        list(APPEND DINGHYCORE_LDFLAGS ${UDEV_LDFLAGS} ${LIBDRM_LDFLAGS})
    else ()
        message(WARNING "DRM/KMS mode monitor won't be built: libudev not found")
    endif ()
endif ()

if (DY_DBUS_SYSTEM_BUS)
    # Generate and install D-Bus policy configuration file.
    configure_file(dbus/policy.conf.in ${DY_DEFAULT_APPID}.conf @ONLY)
    install(
        FILES ${CMAKE_CURRENT_BINARY_DIR}/${DY_DEFAULT_APPID}.conf
        DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/dbus-1/system.d
        COMPONENT "runtime"
    )

    # Let the source code know that the option is enabled.
    add_definitions(-DDY_DBUS_SYSTEM_BUS=1)
    add_definitions(-DDY_DBUS_OWN_USER=\"${DY_DBUS_OWN_USER}\")
endif ()

add_library(dinghycore ${DINGHYCORE_SOURCES})
set_property(TARGET dinghycore PROPERTY C_STANDARD 99)
target_include_directories(dinghycore PUBLIC ${DINGHYCORE_INCLUDE_DIRS})
target_link_libraries(dinghycore ${DINGHYCORE_LDFLAGS})

add_executable(dinghy dinghy.c)
set_property(TARGET dinghy PROPERTY C_STANDARD 99)
target_link_libraries(dinghy dinghycore -ldl)

add_executable(dinghyctl dinghyctl.c dy-utils.c)
set_property(TARGET dinghyctl PROPERTY C_STANDARD 99)
target_include_directories(dinghyctl PUBLIC ${GIO_INCLUDE_DIRS} ${SOUP_INCLUDE_DIRS})
target_link_libraries(dinghyctl ${GIO_LDFLAGS} ${SOUP_LDFLAGS})

install(TARGETS dinghy dinghyctl
    DESTINATION ${CMAKE_INSTALL_BINDIR}
    COMPONENT "runtime"
)
install(TARGETS dinghycore
    DESTINATION ${CMAKE_INSTALL_LIBDIR}
    COMPONENT ${DINGHYCORE_COMPONENT}
)
install(FILES ${DINGHYCORE_API_HEADERS}
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/dinghy
    COMPONENT "development"
)

configure_file(dinghycore.pc.in dinghycore.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/dinghycore.pc
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
    COMPONENT "development"
)

# libdinghyplaform-fdo

project(dinghyplatform-fdo VERSION 0.1.0 DESCRIPTION "Dinghy FDO platform")

set(DINGHYPLATFORM_FDO_SOURCES
    dy-platform-fdo.c
    xdg-shell-unstable-v6-protocol.c
    fullscreen-shell-unstable-v1-protocol.c
)

pkg_check_modules(DINGHYPLATFORM_FDO_DEPS REQUIRED wpe-webkit-0.1 wpe-0.1 wpebackend-fdo-0.1 egl glesv2 wayland-egl xkbcommon)

set(DINGHYPLATFORM_FDO_INCLUDE_DIRS ${DINGHYPLATFORM_FDO_DEPS_INCLUDE_DIRS})
set(DINGHYPLATFORM_FDO_LDFLAGS ${DINGHYPLATFORM_FDO_DEPS_LDFLAGS})

add_library(dinghyplatform-fdo SHARED ${DINGHYPLATFORM_FDO_SOURCES})

set_property(TARGET dinghyplatform-fdo PROPERTY VERSION ${PROJECT_VERSION})

set_property(TARGET dinghyplatform-fdo PROPERTY C_STANDARD 99)
target_include_directories(dinghyplatform-fdo PUBLIC ${DINGHYPLATFORM_FDO_INCLUDE_DIRS})
target_link_libraries(dinghyplatform-fdo ${DINGHYPLATFORM_FDO_LDFLAGS})

install(TARGETS dinghyplatform-fdo
    DESTINATION ${CMAKE_INSTALL_LIBDIR}
    COMPONENT "runtime"
)
