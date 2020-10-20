# libcogplaform-x11

pkg_check_modules(COGPLATFORM_X11_DEPS REQUIRED wpe-webkit-1.0>=2.24.0 wpebackend-fdo-1.0>=1.3.1 egl xcb xkbcommon-x11)

set(COGPLATFORM_X11_INCLUDE_DIRS
    ${COGPLATFORM_X11_DEPS_INCLUDE_DIRS}
)
set(COGPLATFORM_X11_CFLAGS
    ${COGPLATFORM_X11_DEPS_CFLAGS_OTHER}
)
set(COGPLATFORM_X11_LDFLAGS
    ${COGPLATFORM_X11_DEPS_LDFLAGS}
)

add_library(cogplatform-x11 MODULE cog-platform-x11.c)
set_target_properties(cogplatform-x11 PROPERTIES
    C_STANDARD 99
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
target_include_directories(cogplatform-x11 PUBLIC ${COGPLATFORM_X11_INCLUDE_DIRS})
target_compile_options(cogplatform-x11 PUBLIC ${COGPLATFORM_X11_CFLAGS})
target_link_libraries(cogplatform-x11 cogcore ${COGPLATFORM_X11_LDFLAGS})

install(TARGETS cogplatform-x11
    DESTINATION ${CMAKE_INSTALL_LIBDIR}
    COMPONENT "runtime"
)
