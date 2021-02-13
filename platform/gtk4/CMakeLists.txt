pkg_check_modules(COGPLATFORM_GTK4_DEPS REQUIRED gtk4 wpebackend-fdo-1.0 epoxy)
add_library(cogplatform-gtk4 MODULE cog-platform-gtk4.c)
set_target_properties(cogplatform-gtk4 PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
target_include_directories(cogplatform-gtk4 PUBLIC
    ${COGPLATFORM_GTK4_DEPS_INCLUDE_DIRS})
target_link_libraries(cogplatform-gtk4 cogcore
    ${COGPLATFORM_GTK4_DEPS_LDFLAGS})
target_compile_options(cogplatform-gtk4
    PUBLIC ${COGPLATFORM_GTK4_DEPS_CFLAGS}
    PRIVATE -DG_LOG_DOMAIN=\"Cog-Gtk4\")
install(TARGETS cogplatform-gtk4
    DESTINATION ${CMAKE_INSTALL_LIBDIR}
    COMPONENT "runtime")
