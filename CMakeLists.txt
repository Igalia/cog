cmake_minimum_required (VERSION 3.3)

list(INSERT CMAKE_MODULE_PATH 0 "${CMAKE_SOURCE_DIR}/cmake")
include(VersioningUtils)
include(GNUInstallDirs)

set_project_version(0 5 1)

# Before making a release, the LT_VERSION string should be modified.
# The string is of the form C:R:A.
# - If interfaces have been changed or added, but binary compatibility has
#   been preserved, change to C+1:0:A+1
# - If binary compatibility has been broken (eg removed or changed interfaces)
#   change to C+1:0:0
# - If the interface is the same as the previous version, change to C:R+1:A
calculate_library_versions_from_libtool_triple(COGCORE 3 0 2)

project(cog VERSION "${PROJECT_VERSION}" LANGUAGES C)
include(DistTargets)

set(COG_VERSION_EXTRA "")
if (IS_DIRECTORY "${CMAKE_SOURCE_DIR}/.git")
    set(COG_VERSION_EXTRA "+git")
    find_package(Git)
    if (GIT_FOUND)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" rev-list --max-count=1 --abbrev-commit HEAD
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            RESULT_VARIABLE GIT_STATUS
            OUTPUT_VARIABLE GIT_OUTPUT
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
        if (${GIT_STATUS} EQUAL 0 AND GIT_OUTPUT)
            set(COG_VERSION_EXTRA "${COG_VERSION_EXTRA}-${GIT_OUTPUT}")
        endif ()
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" status --porcelain
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            RESULT_VARIABLE GIT_STATUS
            OUTPUT_VARIABLE GIT_OUTPUT
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
        if (${GIT_STATUS} EQUAL 0 AND GIT_OUTPUT)
            set(COG_VERSION_EXTRA "${COG_VERSION_EXTRA}-dirty")
        endif ()
        unset(GIT_STATUS)
        unset(GIT_OUTPUT)
    endif ()
    message(STATUS "Source tree revision: ${PROJECT_VERSION}${COG_VERSION_EXTRA}")
endif ()


option(COG_DBUS_SYSTEM_BUS "Expose remote control interface on system bus" OFF)
set(COG_DBUS_OWN_USER "" CACHE STRING
    "Additional user allowed to own the well-known name on the system bus")

option(COG_USE_WEBKITGTK "Use WebKitGTK+ instead of WPEWebKit" OFF)
option(COG_PLATFORM_FDO "Build the FDO platform module" ON)
option(COG_PLATFORM_DRM "Build the DRM platform module" OFF)
option(COG_BUILD_PROGRAMS "Build and install programs as well" ON)
option(INSTALL_MAN_PAGES "Install the man(1) pages if COG_BUILD_PROGRAMS is enabled" ON)
option(COG_WESTON_DIRECT_DISPLAY "Build direct display support for the FDO platform module" OFF)

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

include_directories(${CMAKE_CURRENT_BINARY_DIR})

find_package(PkgConfig)

# libcogcore
set(COGCORE_API_HEADERS
    core/cog.h
    core/cog-launcher.h
    core/cog-request-handler.h
    core/cog-directory-files-handler.h
    core/cog-shell.h
    core/cog-utils.h
    core/cog-webkit-utils.h
    ${CMAKE_CURRENT_BINARY_DIR}/cog-config.h
)
set(COGCORE_SOURCES
    core/cog-launcher.c
    core/cog-request-handler.c
    core/cog-directory-files-handler.c
    core/cog-utils.c
    core/cog-shell.c
    core/cog-webkit-utils.c
)

pkg_check_modules(GIO REQUIRED gio-2.0>=2.44)
pkg_check_modules(SOUP REQUIRED libsoup-2.4)
if (COG_USE_WEBKITGTK)
    list(APPEND COGCORE_SOURCES core/cog-gtk-utils.c)
    pkg_check_modules(WEB_ENGINE REQUIRED webkit2gtk-4.0)
    if (WEB_ENGINE_VERSION VERSION_LESS 2.20.0)
        message(FATAL_ERROR "webkit2gtk-4.0 >= 2.20.0 is required.")
    endif ()
    add_definitions(-DCOG_BG_COLOR_API_SUPPORTED=1)
else ()
    list(APPEND COGCORE_API_HEADERS core/cog-platform.h)
    list(APPEND COGCORE_SOURCES core/cog-platform.c)
    # There is no need to explicitly check wpe-1.0 here because it's a
    # dependency already specified in the wpe-webkit.pc file.
    pkg_check_modules(WEB_ENGINE REQUIRED wpe-webkit-1.0>=2.23.91)
    if ("${WEB_ENGINE_VERSION}" VERSION_GREATER "2.23")
        add_definitions(-DCOG_BG_COLOR_API_SUPPORTED=1)
    else ()
        add_definitions(-DCOG_BG_COLOR_API_SUPPORTED=0)
    endif ()
    if ("${WEB_ENGINE_VERSION}" VERSION_GREATER "2.27.3")
        set(COG_IM_API_SUPPORTED ON)
    else ()
        set(COG_IM_API_SUPPORTED OFF)
    endif ()
endif ()

include(CheckCCompilerFlag)
check_c_compiler_flag(-Wall HAS_WALL)
set(COGCORE_INCLUDE_DIRS ${WEB_ENGINE_INCLUDE_DIRS} ${SOUP_INCLUDE_DIRS})
set(COGCORE_CFLAGS ${WEB_ENGINE_CFLAGS_OTHER} ${SOUP_CFLAGS_OTHER})
if (HAS_WALL)
  set(COGCORE_CFLAGS ${WEB_ENGINE_CFLAGS_OTHER} ${SOUP_CFLAGS_OTHER} "-Wall")
endif ()
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
set_target_properties(cogcore PROPERTIES
    C_STANDARD 99
    VERSION ${COGCORE_VERSION}
    SOVERSION ${COGCORE_VERSION_MAJOR}
)
target_include_directories(cogcore PUBLIC core ${COGCORE_INCLUDE_DIRS})
target_link_libraries(cogcore ${COGCORE_LDFLAGS})
target_compile_options(cogcore
    PUBLIC ${COGCORE_CFLAGS}
    PRIVATE -DG_LOG_DOMAIN=\"Cog\"
)

if (COG_BUILD_PROGRAMS)
    add_executable(cog cog.c)
    set_property(TARGET cog PROPERTY C_STANDARD 99)
    if (HAS_WALL)
      target_compile_options(cog PUBLIC "-Wall")
    endif ()
    target_link_libraries(cog cogcore -ldl)

    add_executable(cogctl cogctl.c core/cog-utils.c)
    set_property(TARGET cogctl PROPERTY C_STANDARD 99)
    target_include_directories(cogctl PUBLIC ${GIO_INCLUDE_DIRS} ${SOUP_INCLUDE_DIRS})
    target_compile_options(cogctl PUBLIC ${GIO_CFLAGS_OTHER} ${SOUP_CFLAGS_OTHER})
    if (HAS_WALL)
      target_compile_options(cogctl PUBLIC "-Wall")
    endif ()
    target_link_libraries(cogctl ${GIO_LDFLAGS} ${SOUP_LDFLAGS})

    install(TARGETS cog cogctl
        DESTINATION ${CMAKE_INSTALL_BINDIR}
        COMPONENT "runtime"
    )
    if (INSTALL_MAN_PAGES)
        install(FILES data/cog.1 data/cogctl.1
            DESTINATION ${CMAKE_INSTALL_MANDIR}/man1
            COMPONENT "runtime"
        )
    endif ()
endif ()


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

if (COG_PLATFORM_FDO AND NOT COG_USE_WEBKITGTK)
    set(WAYLAND_1_10_OR_GREATER OFF)
    pkg_check_modules(WAYLAND REQUIRED wayland-client)
    if (NOT (WAYLAND_VERSION VERSION_LESS 1.10))
        set(WAYLAND_1_10_OR_GREATER ON)
    endif ()

    pkg_check_modules(COGPLATFORM_FDO_DEPS REQUIRED wpe-webkit-1.0>=2.24.0 wpebackend-fdo-1.0>=1.3.1 egl xkbcommon)
    pkg_check_modules(WAYLAND_EGL wayland-egl)

    set(COGPLATFORM_FDO_INCLUDE_DIRS
        ${COGPLATFORM_FDO_DEPS_INCLUDE_DIRS}
        ${WAYLAND_INCLUDE_DIRS}
        ${WAYLAND_EGL_INCLUDE_DIRS}
    )
    set(COGPLATFORM_FDO_CFLAGS
        ${COGPLATFORM_FDO_DEPS_CFLAGS_OTHER}
        ${WAYLAND_CFLAGS_OTHER}
        ${WAYLAND_EGL_CFLAGS_OTHER}
    )
    set(COGPLATFORM_FDO_LDFLAGS
        ${COGPLATFORM_FDO_DEPS_LDFLAGS}
        ${WAYLAND_LDFLAGS}
        ${WAYLAND_EGL_LDFLAGS}
    )
    set(COGPLATFORM_FDO_SOURCES platform/cog-platform-fdo.c)

    if (COG_IM_API_SUPPORTED)
        list(APPEND COGPLATFORM_FDO_SOURCES
            platform/cog-im-context-fdo.c
            platform/cog-im-context-fdo-v1.c
        )
        add_definitions(-DCOG_IM_API_SUPPORTED=1)
    else ()
        add_definitions(-DCOG_IM_API_SUPPORTED=0)
    endif ()

    find_path(WPEBACKEND_FDO_HAS_VIDEO_PLANE_DISPLAY_DMABUF_EXT
        NAMES wpe/extensions/video-plane-display-dmabuf.h
        PATHS ${COGPLATFORM_FDO_DEPS_INCLUDE_DIRS}
        NO_DEFAULT_PATH
    )
    if (WPEBACKEND_FDO_HAS_VIDEO_PLANE_DISPLAY_DMABUF_EXT)
        set(WPEBACKEND_FDO_HAS_VIDEO_PLANE_DISPLAY_DMABUF_EXT ON)
    else ()
        set(WPEBACKEND_FDO_HAS_VIDEO_PLANE_DISPLAY_DMABUF_EXT OFF)
    endif ()

    if (COG_WESTON_DIRECT_DISPLAY AND WPEBACKEND_FDO_HAS_VIDEO_PLANE_DISPLAY_DMABUF_EXT)
        file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/weston)
        pkg_check_modules(LIBWESTON REQUIRED libweston-9-protocols)
        pkg_get_variable(LIBWESTON_PKG_DATA_DIR libweston-9-protocols "pkgdatadir")
        find_package(WaylandScanner REQUIRED)
        add_custom_command(
            OUTPUT ${CMAKE_BINARY_DIR}/weston/weston-direct-display-client-protocol.h
                   ${CMAKE_BINARY_DIR}/weston/weston-direct-display-protocol.c
            DEPENDS ${LIBWESTON_PKG_DATA_DIR}/weston-direct-display.xml
            COMMAND ${WAYLAND_SCANNER} client-header < ${LIBWESTON_PKG_DATA_DIR}/weston-direct-display.xml > ${CMAKE_BINARY_DIR}/weston/weston-direct-display-client-protocol.h
            COMMAND ${WAYLAND_SCANNER} ${WAYLAND_SCANNER_CODE_ARG} < ${LIBWESTON_PKG_DATA_DIR}/weston-direct-display.xml > ${CMAKE_BINARY_DIR}/weston/weston-direct-display-protocol.c
            VERBATIM
        )

        add_custom_command(
            OUTPUT ${CMAKE_BINARY_DIR}/weston/weston-content-protection-client-protocol.h
                   ${CMAKE_BINARY_DIR}/weston/weston-content-protection-protocol.c
            DEPENDS ${LIBWESTON_PKG_DATA_DIR}/weston-content-protection.xml
            COMMAND ${WAYLAND_SCANNER} client-header < ${LIBWESTON_PKG_DATA_DIR}/weston-content-protection.xml > ${CMAKE_BINARY_DIR}/weston/weston-content-protection-client-protocol.h
            COMMAND ${WAYLAND_SCANNER} ${WAYLAND_SCANNER_CODE_ARG} < ${LIBWESTON_PKG_DATA_DIR}/weston-content-protection.xml > ${CMAKE_BINARY_DIR}/weston/weston-content-protection-protocol.c
            VERBATIM
        )

        list(APPEND COGPLATFORM_FDO_SOURCES
            ${CMAKE_BINARY_DIR}/weston/weston-direct-display-protocol.c
            ${CMAKE_BINARY_DIR}/weston/weston-content-protection-protocol.c
        )
        list(APPEND COGPLATFORM_FDO_INCLUDE_DIRS ${CMAKE_BINARY_DIR}/weston)
        set(ENABLE_WESTON_DIRECT_DISPLAY 1)
    else ()
        set(ENABLE_WESTON_DIRECT_DISPLAY 0)
    endif ()

    add_library(cogplatform-fdo MODULE ${COGPLATFORM_FDO_SOURCES})
    set_property(TARGET cogplatform-fdo PROPERTY C_STANDARD 99)
    target_include_directories(cogplatform-fdo PUBLIC wayland ${COGPLATFORM_FDO_INCLUDE_DIRS})
    target_link_libraries(cogplatform-fdo cogcore ${COGPLATFORM_FDO_LDFLAGS})
    target_compile_options(cogplatform-fdo
        PUBLIC ${COGPLATFORM_FDO_CFLAGS}
        PRIVATE -DG_LOG_DOMAIN=\"Cog-FDO\"
    )
    target_compile_options(cogplatform-fdo PRIVATE -DCOG_ENABLE_WESTON_DIRECT_DISPLAY=${ENABLE_WESTON_DIRECT_DISPLAY})

    find_package(WaylandProtocols REQUIRED)
    add_wayland_protocol(cogplatform-fdo CLIENT xdg-shell)
    add_wayland_protocol(cogplatform-fdo CLIENT fullscreen-shell-unstable-v1)
    add_wayland_protocol(cogplatform-fdo CLIENT presentation-time)
    if (COG_IM_API_SUPPORTED)
        add_wayland_protocol(cogplatform-fdo CLIENT text-input-unstable-v1)
        add_wayland_protocol(cogplatform-fdo CLIENT text-input-unstable-v3)
    endif ()
    add_wayland_protocol(cogplatform-fdo CLIENT linux-dmabuf-unstable-v1)

    install(TARGETS cogplatform-fdo
        DESTINATION ${CMAKE_INSTALL_LIBDIR}
        COMPONENT "runtime"
    )
endif ()  # !COG_USE_WEBKITGTK

# libcogplaform-drm

if (COG_PLATFORM_DRM AND NOT COG_USE_WEBKITGTK)
    pkg_check_modules(COGPLATFORM_DRM_DEPS REQUIRED wpe-webkit-1.0>=2.24.0 wpebackend-fdo-1.0>=1.3.1 libdrm>=2.4.71 gbm>=13.0 egl libinput libudev wayland-server)

    set(COGPLATFORM_DRM_INCLUDE_DIRS
        ${COGPLATFORM_DRM_DEPS_INCLUDE_DIRS}
    )
    set(COGPLATFORM_DRM_CFLAGS
        ${COGPLATFORM_DRM_DEPS_CFLAGS_OTHER}
    )
    set(COGPLATFORM_DRM_LDFLAGS
        ${COGPLATFORM_DRM_DEPS_LDFLAGS}
    )

    add_library(cogplatform-drm MODULE platform/cog-platform-drm.c)
    set_property(TARGET cogplatform-drm PROPERTY C_STANDARD 99)
    target_include_directories(cogplatform-drm PUBLIC ${COGPLATFORM_DRM_INCLUDE_DIRS})
    target_compile_options(cogplatform-drm PUBLIC ${COGPLATFORM_DRM_CFLAGS})
    target_link_libraries(cogplatform-drm cogcore ${COGPLATFORM_DRM_LDFLAGS})

    install(TARGETS cogplatform-drm
        DESTINATION ${CMAKE_INSTALL_LIBDIR}
        COMPONENT "runtime"
    )
endif () # COG_PLATFORM_DRM AND NOT COG_USE_WEBKITGTK

configure_file(core/cog-config.h.in cog-config.h @ONLY)
