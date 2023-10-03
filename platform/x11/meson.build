x11_platform_sources = []
if libportal_dep.found()
    x11_platform_sources += ['cog-xdp-parent-x11.c']
endif

x11_platform_plugin = shared_module('cogplatform-x11',
    'cog-platform-x11.c',
    x11_platform_sources,
    c_args: ['-DG_LOG_DOMAIN="Cog-X11"'],
    dependencies: [
        wpebackend_fdo_dep,
        cogplatformcommon_dep,
        dependency('egl'),
        dependency('xcb'),
        dependency('xkbcommon-x11'),
        dependency('x11-xcb'),
        dependency('xcb-cursor'),
    ],
    gnu_symbol_visibility: 'hidden',
    install_dir: plugin_path,
    install: true,
)
platform_plugin_targets += [x11_platform_plugin]
