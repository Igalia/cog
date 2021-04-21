cmake_minimum_required (VERSION 3.3)

list(INSERT CMAKE_MODULE_PATH 0 "${CMAKE_SOURCE_DIR}/cmake")
include(VersioningUtils)

set_project_version(0 9 1)

# Before making a release, the LT_VERSION string should be modified.
# The string is of the form C:R:A.
# - If interfaces have been changed or added, but binary compatibility has
#   been preserved, change to C+1:0:A+1
# - If binary compatibility has been broken (eg removed or changed interfaces)
#   change to C+1:0:0
# - If the interface is the same as the previous version, change to C:R+1:A
calculate_library_versions_from_libtool_triple(COGCORE 5 0 4)

project(cog VERSION "${PROJECT_VERSION}" LANGUAGES C)
include(DistTargets)
include(GNUInstallDirs)

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

option(COG_PLATFORM_FDO "Build the FDO platform module" ON)
option(COG_PLATFORM_DRM "Build the DRM platform module" ON)
option(COG_PLATFORM_X11 "Build the X11 platform module" OFF)
option(COG_PLATFORM_GTK4 "Build the GTK4 platform module" OFF)
option(COG_PLATFORM_XCB "Build the XCB (Simplified X11) platform module" OFF)

option(COG_BUILD_PROGRAMS "Build and install programs as well" ON)
option(INSTALL_MAN_PAGES "Install the man(1) pages if COG_BUILD_PROGRAMS is enabled" ON)
option(COG_WESTON_DIRECT_DISPLAY "Build direct display support for the FDO platform module" OFF)
option(BUILD_DOCS "Build the documentation" OFF)

set(COG_APPID "" CACHE STRING "Default GApplication unique identifier")
set(COG_HOME_URI "" CACHE STRING "Default home URI")

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
    core/cog-prefix-routes-handler.h
    core/cog-shell.h
    core/cog-utils.h
    core/cog-webkit-utils.h
    core/cog-platform.h
    ${CMAKE_CURRENT_BINARY_DIR}/cog-config.h
)
set(COGCORE_SOURCES
    core/cog-launcher.c
    core/cog-request-handler.c
    core/cog-directory-files-handler.c
    core/cog-prefix-routes-handler.c
    core/cog-utils.c
    core/cog-shell.c
    core/cog-webkit-utils.c
    core/cog-platform.c
)

pkg_check_modules(GIO IMPORTED_TARGET REQUIRED gio-2.0>=2.44)
pkg_check_modules(SOUP IMPORTED_TARGET REQUIRED libsoup-2.4)
pkg_check_modules(WebKit IMPORTED_TARGET REQUIRED wpe-webkit-1.0>=2.28.0 wpe-1.0>=1.6.0)

include(CheckCCompilerFlag)
check_c_compiler_flag(-Wall HAS_WALL)

if (COG_DBUS_SYSTEM_BUS)
    # Generate and install D-Bus policy configuration file.
    configure_file(dbus/policy.conf.in ${COG_DEFAULT_APPID}.conf @ONLY)
    install(
        FILES ${CMAKE_CURRENT_BINARY_DIR}/${COG_DEFAULT_APPID}.conf
        DESTINATION ${CMAKE_INSTALL_DATADIR}/dbus-1/system.d
        COMPONENT "runtime"
    )

    # Let the source code know that the option is enabled.
    add_definitions(-DCOG_DBUS_SYSTEM_BUS=1)
    add_definitions(-DCOG_DBUS_OWN_USER=\"${COG_DBUS_OWN_USER}\")
endif ()

add_library(cogcore SHARED ${COGCORE_SOURCES} ${COGCORE_API_HEADERS})
set_target_properties(cogcore PROPERTIES
    C_STANDARD 99
    VERSION ${COGCORE_VERSION}
    SOVERSION ${COGCORE_VERSION_MAJOR}
)
target_link_libraries(cogcore PkgConfig::WebKit PkgConfig::SOUP)
target_compile_definitions(cogcore PRIVATE G_LOG_DOMAIN=\"Cog-Core\")
if (HAS_WALL)
    target_compile_options(cogcore PUBLIC -Wall)
endif ()

if (COG_BUILD_PROGRAMS)
    add_executable(cog cog.c)
    set_property(TARGET cog PROPERTY C_STANDARD 99)
    if (HAS_WALL)
      target_compile_options(cog PUBLIC "-Wall")
    endif ()
    target_compile_definitions(cog PRIVATE G_LOG_DOMAIN=\"Cog\")
    target_link_libraries(cog cogcore -ldl)

    add_executable(cogctl cogctl.c core/cog-utils.c)
    set_property(TARGET cogctl PROPERTY C_STANDARD 99)
    target_compile_definitions(cogctl PRIVATE G_LOG_DOMAIN=\"Cog-Control\")
    if (HAS_WALL)
      target_compile_options(cogctl PUBLIC "-Wall")
    endif ()
    target_link_libraries(cogctl PkgConfig::GIO PkgConfig::SOUP)

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

configure_file(core/cog-config.h.in cog-config.h @ONLY)
configure_file(core/cogcore.pc.in cogcore.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/cogcore.pc
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
    COMPONENT "development"
)

if (BUILD_DOCS)
    add_subdirectory(docs)
endif ()

add_subdirectory(platform/headless)
if (COG_PLATFORM_FDO)
    add_subdirectory(platform/fdo)
endif ()
if (COG_PLATFORM_DRM)
    add_subdirectory(platform/drm)
endif ()
if (COG_PLATFORM_X11)
    add_subdirectory(platform/x11)
endif ()
if (COG_PLATFORM_GTK4)
    add_subdirectory(platform/gtk4)
endif ()
if (COG_PLATFORM_XCB)
    add_subdirectory(platform/xcb)
endif ()
