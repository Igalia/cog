pkg_check_modules(COGPLATFORM_GTK4_DEPS REQUIRED
    IMPORTED_TARGET gtk4)
if (NOT TARGET Wpe::FDO)
  find_package(WpeFDO REQUIRED)
endif()
add_library(cogplatform-gtk4 MODULE
    ../common/cog-gl-utils.c
    cog-platform-gtk4.c
    cog-gtk-settings-dialog.c
    cog-gtk-settings-cell-renderer-variant.c)
target_link_libraries(cogplatform-gtk4 PRIVATE
    PkgConfig::COGPLATFORM_GTK4_DEPS
    PkgConfig::WebKit
    Wpe::FDO
)
set_target_properties(cogplatform-gtk4 PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/modules
)
target_compile_options(cogplatform-gtk4
    PRIVATE -DG_LOG_DOMAIN=\"Cog-Gtk4\")
install(TARGETS cogplatform-gtk4
    DESTINATION ${COG_MODULEDIR}
    COMPONENT "runtime")
