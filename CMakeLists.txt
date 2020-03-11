cmake_minimum_required (VERSION 3.3)

list(INSERT CMAKE_MODULE_PATH 0 "${CMAKE_SOURCE_DIR}/cmake")
include(VersioningUtils)
include(GNUInstallDirs)

set_project_version(0 5 0)

# Before making a release, the LT_VERSION string should be modified.
# The string is of the form C:R:A.
# - If interfaces have been changed or added, but binary compatibility has
#   been preserved, change to C+1:0:A+1
# - If binary compatibility has been broken (eg removed or changed interfaces)
#   change to C+1:0:0
# - If the interface is the same as the previous version, change to C:R+1:A
calculate_library_versions_from_libtool_triple(COGCORE 2 0 1)

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

option(COG_MODULE_FDO_SHELL "Build the FDO shell module" ON)
option(COG_BUILD_PROGRAMS "Build and install programs as well" ON)
option(INSTALL_MAN_PAGES "Install the man(1) pages if COG_BUILD_PROGRAMS is enabled" ON)
option(COG_WESTON_DIRECT_DISPLAY "Build direct display support for the FDO platform module" OFF)
option(COG_ENABLE_IVI_SHELL "Build with support for the IVI application interface" OFF)

set(COG_APPID "" CACHE STRING "Default GApplication unique identifier")
set(COG_HOME_URI "" CACHE STRING "Default home URI")
set(COG_MODULEDIR "${CMAKE_INSTALL_PREFIX}/lib/cog/modules"
    CACHE STRING "Default search path for loadable modules")

if (NOT COG_APPID OR COG_APPID STREQUAL "")
    set(COG_DEFAULT_APPID com.igalia.Cog)
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
    core/cog-modules.h
    core/cog-shell.h
    core/cog-utils.h
    core/cog-view.h
    core/cog-webkit-utils.h
    ${CMAKE_CURRENT_BINARY_DIR}/cog-config.h
)
set(COGCORE_SOURCES
    core/cog-launcher.c
    core/cog-request-handler.c
    core/cog-directory-files-handler.c
    core/cog-modules.c
    core/cog-utils.c
    core/cog-shell.c
    core/cog-minimal-shell.c
    core/cog-view.c
    core/cog-webkit-utils.c
)

pkg_check_modules(Gio IMPORTED_TARGET REQUIRED gio-2.0>=2.44)
pkg_check_modules(Soup IMPORTED_TARGET REQUIRED libsoup-2.4)
pkg_check_modules(WebKit IMPORTED_TARGET REQUIRED wpe-webkit-1.0>=2.23.91)

if (WebKit_VERSION VERSION_GREATER 2.23)
    add_definitions(-DCOG_BG_COLOR_API_SUPPORTED=1)
else ()
    add_definitions(-DCOG_BG_COLOR_API_SUPPORTED=0)
endif ()

if (WebKit_VERSION VERSION_GREATER 2.27.3)
    set(COG_IM_API_SUPPORTED ON)
else ()
    set(COG_IM_API_SUPPORTED OFF)
endif ()

set (EXTRA_CFLAGS "")
include (CheckCCompilerFlag)
check_c_compiler_flag (-Wall CC_HAS_FLAG)
if (CC_HAS_FLAG)
    list (APPEND EXTRA_CFLAGS -Wall)
endif ()

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
target_include_directories(cogcore PUBLIC core)
target_link_libraries(cogcore PUBLIC PkgConfig::WebKit)
target_compile_options(cogcore PRIVATE -DG_LOG_DOMAIN=\"Cog\" ${EXTRA_CFLAGS})

if (COG_BUILD_PROGRAMS)
    add_executable(cog cog.c)
    set_property(TARGET cog PROPERTY C_STANDARD 99)
    target_compile_options(cog PRIVATE ${EXTRA_CFLAGS})
    target_link_libraries(cog cogcore -ldl)

    add_executable(cogctl cogctl.c core/cog-utils.c)
    set_property(TARGET cogctl PROPERTY C_STANDARD 99)
    target_compile_options(cogctl PUBLIC ${EXTRA_CFLAGS})
    target_link_libraries(cogctl PkgConfig::Gio PkgConfig::Soup)

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

# Platform support libraries: Wayland.
if (COG_MODULE_FDO_SHELL)
    add_library (platform-wl STATIC EXCLUDE_FROM_ALL
        platform/pwl.h
        platform/pwl.c
    )
    set_property (TARGET platform-wl PROPERTY C_STANDARD 99)
    set_property (TARGET platform-wl PROPERTY POSITION_INDEPENDENT_CODE ON)
    target_compile_options (platform-wl PRIVATE
        -DG_LOG_DOMAIN=\"Cog-PWL\" ${EXTRA_CFLAGS})

    pkg_check_modules (PlatformWl IMPORTED_TARGET REQUIRED wayland-client egl)
    pkg_check_modules (PlatformWlOptional IMPORTED_TARGET wayland-egl)
    target_link_libraries (platform-wl PUBLIC
        PkgConfig::Gio
        PkgConfig::PlatformWl
        PkgConfig::PlatformWlOptional
    )

    find_package (WaylandProtocols REQUIRED)
    add_wayland_protocol (platform-wl CLIENT xdg-shell)
    add_wayland_protocol (platform-wl CLIENT fullscreen-shell-unstable-v1)
    if (COG_ENABLE_IVI_SHELL)
        add_wayland_protocol (platform-wl CLIENT ivi-application.xml)
        target_compile_definitions (platform-wl PRIVATE HAVE_IVI_SHELL)
    endif ()
endif ()

# FDO shell module
if (COG_MODULE_FDO_SHELL)
    pkg_check_modules (ModuleFdoShell IMPORTED_TARGET REQUIRED
        wpe-webkit-1.0>=2.24.0
        wpebackend-fdo-1.0>=1.3.1
        egl
        xkbcommon
    )

    add_library (fdo-shell MODULE modules/cog-fdo-shell.c)
    set_property (TARGET fdo-shell PROPERTY C_STANDARD 99)
    target_link_libraries (fdo-shell PRIVATE
        cogcore platform-wl PkgConfig::ModuleFdoShell)
    target_compile_options (fdo-shell PRIVATE
        -DG_LOG_DOMAIN=\"Cog-FDO\" ${EXTRA_CFLAGS})
    find_package (WaylandProtocols REQUIRED)
    add_wayland_protocol (fdo-shell CLIENT xdg-shell)
    add_wayland_protocol (fdo-shell CLIENT fullscreen-shell-unstable-v1)

    install (TARGETS fdo-shell
        DESTINATION ${COG_MODULEDIR}
        COMPONENT "runtime"
    )
endif ()

configure_file(core/cog-config.h.in cog-config.h @ONLY)
