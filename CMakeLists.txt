cmake_minimum_required (VERSION 3.0)
project(dinghy VERSION 0.1.0 LANGUAGES C)

add_definitions(
	-DPROJECT_VERSION="${PROJECT_VERSION}"
	-DPROJECT_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
	-DPROJECT_VERSION_MINOR=${PROJECT_VERSION_MINOR}
	-DPROJECT_VERSION_PATCH=${PROJECT_VERSION_PATCH}
)

find_package(PkgConfig)
pkg_search_module(WPE REQUIRED wpe-webkit)

add_executable(${PROJECT_NAME}
	${PROJECT_NAME}.c
	dy-launcher.c
)
target_include_directories(${PROJECT_NAME} PUBLIC ${WPE_INCLUDE_DIRS})
target_link_libraries(${PROJECT_NAME} ${WPE_LIBRARIES})

set_property(
	TARGET ${PROJECT_NAME}
	PROPERTY C_STANDARD 99
)
		
install(TARGETS ${PROJECT_NAME} DESTINATION "${CMAKE_INSTALL_PREFIX}/bin")
