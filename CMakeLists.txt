cmake_minimum_required (VERSION 3.3)
project(dinghy VERSION 0.1.0 LANGUAGES C)

include(GNUInstallDirs)

option(DY_USE_WEBKITGTK "Use WebKitGTK+ instead of WPEWebKit" OFF)
option(BUILD_SHARED_LIBS "Build lib${PROJECT_NAME}core as a shared library" ON)
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

configure_file(dy-config.h.in dy-config.h @ONLY)
include_directories(${CMAKE_CURRENT_BINARY_DIR})

find_package(PkgConfig)

# libdinghycore
set(DINGHYCORE_API_HEADERS
	dinghy.h
	dy-launcher.h
	dy-request-handler.h
	dy-directory-files-handler.h
	dy-webkit-utils.h
	${CMAKE_CURRENT_BINARY_DIR}/dy-config.h
)
set(DINGHYCORE_SOURCES
	dy-launcher.c
	dy-request-handler.c
	dy-directory-files-handler.c
)

if (DY_USE_WEBKITGTK)
	list(APPEND DINGHYCORE_SOURCES dy-gtk-utils.c)
	pkg_check_modules(WEB_ENGINE REQUIRED webkit2gtk-4.0)
else ()
	pkg_check_modules(WEB_ENGINE REQUIRED wpe-webkit)
endif ()


add_library(dinghycore ${DINGHYCORE_SOURCES})
set_property(TARGET dinghycore PROPERTY C_STANDARD 99)
target_include_directories(dinghycore PUBLIC ${WEB_ENGINE_INCLUDE_DIRS})
target_link_libraries(dinghycore ${WEB_ENGINE_LIBRARIES})

add_executable(dinghy dinghy.c)
set_property(TARGET dinghy PROPERTY C_STANDARD 99)
target_link_libraries(dinghy dinghycore)
		
install(TARGETS dinghy
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

configure_file(dinghyctl.in dinghyctl @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/dinghyctl
	DESTINATION ${CMAKE_INSTALL_BINDIR}
	COMPONENT "runtime"
	PERMISSIONS
		OWNER_READ OWNER_WRITE OWNER_EXECUTE
		GROUP_READ GROUP_EXECUTE
		WORLD_READ WORLD_EXECUTE
)

configure_file(dinghycore.pc.in dinghycore.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/dinghycore.pc
	DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
	COMPONENT "development"
)
