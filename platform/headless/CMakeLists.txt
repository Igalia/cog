
pkg_check_modules(WpeFDO IMPORTED_TARGET REQUIRED wpebackend-fdo-1.0>=1.8.0)
add_library(cogplatform-headless MODULE cog-platform-headless.c)
set_target_properties(cogplatform-headless PROPERTIES
    C_STANDARD 99
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/modules
)

target_compile_definitions(cogplatform-headless PRIVATE G_LOG_DOMAIN=\"Cog-HEADLESS\")
target_link_libraries(cogplatform-headless PRIVATE
    cogcore
    PkgConfig::WpeFDO
)

install(TARGETS cogplatform-headless
    DESTINATION ${COG_MODULEDIR}
    COMPONENT "runtime"
)
