executable('cog',
    'cog.c',
    'cog-launcher.c',
    c_args: ['-DG_LOG_DOMAIN="Cog"'],
    dependencies: cogcore_dep,
    install: true,
)
executable('cogctl',
    'cogctl.c',
    '../core/cog-utils.c',
    c_args: ['-DG_LOG_DOMAIN="Cog-Control"'],
    dependencies: [libsoup_dep, cogcore_dep.partial_dependency(includes: true)],
    install: true,
)
