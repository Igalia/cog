add_executable(cog
    cog.c
    cog-launcher.c
)
set_target_properties(cog PROPERTIES
    C_STANDARD 99
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
target_compile_definitions(cog PRIVATE G_LOG_DOMAIN=\"Cog\")
target_link_libraries(cog cogcore -ldl)

add_executable(cogctl
    cogctl.c
    ../core/cog-utils.c
)
set_target_properties(cogctl PROPERTIES
    C_STANDARD 99
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
target_compile_definitions(cogctl PRIVATE G_LOG_DOMAIN=\"Cog-Control\")
target_link_libraries(cogctl PkgConfig::GIO PkgConfig::SOUP)

if (HAS_WALL)
    target_compile_options(cog PUBLIC "-Wall")
    target_compile_options(cogctl PUBLIC "-Wall")
endif ()

install(TARGETS cog cogctl
    DESTINATION ${CMAKE_INSTALL_BINDIR}
    COMPONENT "runtime"
)
if (INSTALL_MAN_PAGES)
    install(FILES ../data/cog.1 ../data/cogctl.1
        DESTINATION ${CMAKE_INSTALL_MANDIR}/man1
        COMPONENT "runtime"
    )
endif ()
