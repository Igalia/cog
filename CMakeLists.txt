cmake_minimum_required (VERSION 3.3)
project(cog VERSION 0.1.0 LANGUAGES C)

include(GNUInstallDirs)

option(COG_DBUS_SYSTEM_BUS "Expose remote control interface on system bus" OFF)
set(COG_DBUS_OWN_USER "" CACHE STRING
    "Additional user allowed to own the well-known name on the system bus")

option(COG_USE_WEBKITGTK "Use WebKitGTK+ instead of WPEWebKit" OFF)
set(COG_APPID "" CACHE STRING "Default GApplication unique identifier")
set(COG_HOME_URI "" CACHE STRING "Default home URI")

if (NOT COG_APPID OR COG_APPID STREQUAL "")
    if (COG_USE_WEBKITGTK)
        set(COG_DEFAULT_APPID com.igalia.CogGtk)
    else ()
        set(COG_DEFAULT_APPID com.igalia.Cog)
    endif ()
else ()
    set(COG_DEFAULT_APPID ${COG_APPID})
endif ()

if (COG_HOME_URI AND NOT COG_HOME_URI STREQUAL "")
    set(COG_DEFAULT_HOME_URI ${COG_HOME_URI})
endif ()

if (BUILD_SHARED_LIBS)
    set(COGCORE_COMPONENT "runtime")
else()
    set(COGCORE_COMPONENT "development")
endif()

add_definitions(-DCOG_INSIDE_COG__=1)
add_definitions(-DG_LOG_DOMAIN=\"Cog\")

configure_file(core/cog-config.h.in cog-config.h @ONLY)
include_directories(${CMAKE_CURRENT_BINARY_DIR})

find_package(PkgConfig)

# libcogcore
set(COGCORE_API_HEADERS
    core/cog.h
    core/cog-launcher.h
    core/cog-request-handler.h
    core/cog-directory-files-handler.h
    core/cog-utils.h
    core/cog-webkit-utils.h
    ${CMAKE_CURRENT_BINARY_DIR}/cog-config.h
)
set(COGCORE_SOURCES
    core/cog-launcher.c
    core/cog-request-handler.c
    core/cog-directory-files-handler.c
    core/cog-utils.c
    core/cog-webkit-utils.c
)

pkg_check_modules(GIO REQUIRED gio-2.0)
pkg_check_modules(SOUP REQUIRED libsoup-2.4)
if (COG_USE_WEBKITGTK)
    list(APPEND COGCORE_SOURCES core/cog-gtk-utils.c)
    pkg_check_modules(WEB_ENGINE REQUIRED webkit2gtk-4.0)
else ()
    list(APPEND COGCORE_API_HEADERS core/cog-platform.h)
    list(APPEND COGCORE_SOURCES core/cog-platform.c)
    pkg_check_modules(WEB_ENGINE REQUIRED wpe-webkit-0.1 wpe-0.1)
endif ()

set(COGCORE_INCLUDE_DIRS ${WEB_ENGINE_INCLUDE_DIRS} ${SOUP_INCLUDE_DIRS})
set(COGCORE_CFLAGS ${WEB_ENGINE_CFLAGS_OTHER} ${SOUP_CFLAGS_OTHER})
set(COGCORE_LDFLAGS ${WEB_ENGINE_LDFLAGS} ${SOUP_LDFLAGS})


if (COG_DBUS_SYSTEM_BUS)
    # Generate and install D-Bus policy configuration file.
    configure_file(dbus/policy.conf.in ${COG_DEFAULT_APPID}.conf @ONLY)
    install(
        FILES ${CMAKE_CURRENT_BINARY_DIR}/${COG_DEFAULT_APPID}.conf
        DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/dbus-1/system.d
        COMPONENT "runtime"
    )

    # Let the source code know that the option is enabled.
    add_definitions(-DCOG_DBUS_SYSTEM_BUS=1)
    add_definitions(-DCOG_DBUS_OWN_USER=\"${COG_DBUS_OWN_USER}\")
endif ()

add_library(cogcore SHARED ${COGCORE_SOURCES})
set_property(TARGET cogcore PROPERTY C_STANDARD 99)
target_include_directories(cogcore PUBLIC core ${COGCORE_INCLUDE_DIRS})
target_compile_options(cogcore PUBLIC ${COGCORE_CFLAGS})
target_link_libraries(cogcore ${COGCORE_LDFLAGS})

add_executable(cog cog.c)
set_property(TARGET cog PROPERTY C_STANDARD 99)
target_link_libraries(cog cogcore -ldl)

add_executable(cogctl cogctl.c core/cog-utils.c)
set_property(TARGET cogctl PROPERTY C_STANDARD 99)
target_include_directories(cogctl PUBLIC ${GIO_INCLUDE_DIRS} ${SOUP_INCLUDE_DIRS})
target_compile_options(cogctl PUBLIC ${GIO_CFLAGS_OTHER} ${SOUP_CFLAGS_OTHER})
target_link_libraries(cogctl ${GIO_LDFLAGS} ${SOUP_LDFLAGS})

install(TARGETS cog cogctl
    DESTINATION ${CMAKE_INSTALL_BINDIR}
    COMPONENT "runtime"
)
install(TARGETS cogcore
    DESTINATION ${CMAKE_INSTALL_LIBDIR}
    COMPONENT ${COGCORE_COMPONENT}
)
install(FILES ${COGCORE_API_HEADERS}
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/cog
    COMPONENT "development"
)

configure_file(core/cogcore.pc.in cogcore.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/cogcore.pc
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
    COMPONENT "development"
)

# libcogplaform-fdo

if (NOT COG_USE_WEBKITGTK)

set(COGPLATFORM_VERSION "0.1.0")

set(COGPLATFORM_FDO_SOURCES
    platform/cog-platform-fdo.c
    wayland/xdg-shell-unstable-v6-protocol.c
    wayland/fullscreen-shell-unstable-v1-protocol.c
)

pkg_check_modules(COGPLATFORM_FDO_DEPS REQUIRED wpe-webkit-0.1 wpe-0.1 wpebackend-fdo-0.1 egl glesv2 wayland-egl xkbcommon)

set(COGPLATFORM_FDO_INCLUDE_DIRS ${COGPLATFORM_FDO_DEPS_INCLUDE_DIRS})
set(COGPLATFORM_FDO_CFLAGS ${COGPLATFORM_FDO_DEPS_CFLAGS_OTHER})
set(COGPLATFORM_FDO_LDFLAGS ${COGPLATFORM_FDO_DEPS_LDFLAGS})

add_library(cogplatform-fdo SHARED ${COGPLATFORM_FDO_SOURCES})

set_property(TARGET cogplatform-fdo PROPERTY VERSION ${COGPLATFORM_VERSION})

set_property(TARGET cogplatform-fdo PROPERTY C_STANDARD 99)
target_include_directories(cogplatform-fdo PUBLIC wayland ${COGPLATFORM_FDO_INCLUDE_DIRS})
target_compile_options(cogplatform-fdo PUBLIC ${COGPLATFORM_FDO_CFLAGS})
target_link_libraries(cogplatform-fdo cogcore ${COGPLATFORM_FDO_LDFLAGS})

install(TARGETS cogplatform-fdo
    DESTINATION ${CMAKE_INSTALL_LIBDIR}
    COMPONENT "runtime"
)

endif ()  # !COG_USE_WEBKITGTK
