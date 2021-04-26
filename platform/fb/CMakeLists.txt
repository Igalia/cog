# libcogplaform-fb

pkg_check_modules(COGPLATFORM_FB_DEPS IMPORTED_TARGET
    REQUIRED wpebackend-fdo-1.0>=1.6.0)

add_library(cogplatform-fb MODULE cog-platform-fb.c)
set_target_properties(cogplatform-fb PROPERTIES
    C_STANDARD 99
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
target_compile_definitions(cogplatform-fb PRIVATE G_LOG_DOMAIN=\"Cog-FB\")
target_link_libraries(cogplatform-fb PRIVATE cogcore
    PkgConfig::COGPLATFORM_FB_DEPS
    PkgConfig::WebKit
)

install(TARGETS cogplatform-fb
    DESTINATION ${CMAKE_INSTALL_LIBDIR}
    COMPONENT "runtime"
)
