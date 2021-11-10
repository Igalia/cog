cmake_minimum_required (VERSION 3.3)

list(INSERT CMAKE_MODULE_PATH 0 "${CMAKE_SOURCE_DIR}/cmake")
include(VersioningUtils)

set_project_version(0 11 1)

# Before making a release, the LT_VERSION string should be modified.
# The string is of the form C:R:A.
# - If interfaces have been changed or added, but binary compatibility has
#   been preserved, change to C+1:0:A+1
# - If binary compatibility has been broken (eg removed or changed interfaces)
#   change to C+1:0:0
# - If the interface is the same as the previous version, change to C:R+1:A
calculate_library_versions_from_libtool_triple(COGCORE 6 0 5)

project(cog VERSION "${PROJECT_VERSION}" LANGUAGES C)
include(DistTargets)
include(GNUInstallDirs)

set(COG_VERSION_EXTRA "")
if (IS_DIRECTORY "${CMAKE_SOURCE_DIR}/.git")
    set(COG_VERSION_EXTRA "+git")
    find_package(Git)
    file(COPY ${CMAKE_SOURCE_DIR}/hooks/pre-commit DESTINATION ${CMAKE_SOURCE_DIR}/.git/hooks FILE_PERMISSIONS OWNER_EXECUTE OWNER_WRITE OWNER_READ)
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

option(COG_PLATFORM_HEADLESS "Build the headless platform module" ON)
option(COG_PLATFORM_WL "Build the Wayland platform module" ON)
option(COG_PLATFORM_DRM "Build the DRM platform module" ON)
option(COG_PLATFORM_X11 "Build the X11 platform module" OFF)
option(COG_PLATFORM_GTK4 "Build the GTK4 platform module" OFF)

option(COG_BUILD_PROGRAMS "Build and install programs as well" ON)
option(INSTALL_MAN_PAGES "Install the man(1) pages if COG_BUILD_PROGRAMS is enabled" ON)
option(COG_WESTON_DIRECT_DISPLAY "Build direct display support for the Wayland platform module" OFF)
option(BUILD_DOCS "Build the documentation" OFF)

option(USE_SOUP2 "Build with libsoup2 instead of libsoup3" ON)

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
    core/cog-host-routes-handler.h
    core/cog-prefix-routes-handler.h
    core/cog-shell.h
    core/cog-utils.h
    core/cog-webkit-utils.h
    core/cog-platform.h
    core/cog-modules.h
    core/cog-modules.c
    ${CMAKE_CURRENT_BINARY_DIR}/cog-config.h
)
set(COGCORE_SOURCES
    core/cog-launcher.c
    core/cog-request-handler.c
    core/cog-directory-files-handler.c
    core/cog-host-routes-handler.c
    core/cog-prefix-routes-handler.c
    core/cog-utils.c
    core/cog-shell.c
    core/cog-webkit-utils.c
    core/cog-platform.c
)

if (USE_SOUP2)
    set(WPEWEBKIT_PC_NAME "wpe-webkit-1.0")
    set(REQUIRED_GIO_VERSION "gio-2.0>=2.44")
    set(REQUIRED_SOUP_VERSION "libsoup-2.4")
    set(REQUIRED_WEBKIT_VERSION "wpe-webkit-1.0>=2.28.0")
    add_definitions("-DCOG_USE_SOUP2=1")
else()
    set(WPEWEBKIT_PC_NAME "wpe-webkit-1.1")
    set(REQUIRED_GIO_VERSION "gio-2.0>=2.67.4")
    set(REQUIRED_SOUP_VERSION "libsoup-3.0>=2.99.7")
    set(REQUIRED_WEBKIT_VERSION "wpe-webkit-1.1>=2.33.1")
    add_definitions("-DCOG_USE_SOUP2=0")
endif()

pkg_check_modules(GIO IMPORTED_TARGET REQUIRED ${REQUIRED_GIO_VERSION})
pkg_check_modules(SOUP IMPORTED_TARGET REQUIRED ${REQUIRED_SOUP_VERSION})
pkg_check_modules(WebKit IMPORTED_TARGET REQUIRED ${REQUIRED_WEBKIT_VERSION})

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
target_link_libraries(cogcore PkgConfig::GIO PkgConfig::WebKit PkgConfig::SOUP)
target_compile_definitions(cogcore PRIVATE G_LOG_DOMAIN=\"Cog-Core\")
if (HAS_WALL)
    target_compile_options(cogcore PUBLIC -Wall)
endif ()

if (COG_BUILD_PROGRAMS)
    add_subdirectory(launcher)
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

if (COG_PLATFORM_HEADLESS)
    add_subdirectory(platform/headless)
endif ()
if (COG_PLATFORM_WL)
    add_subdirectory(platform/wayland)
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
