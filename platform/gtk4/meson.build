if libportal_dep.found()
    libportal_gtk4_dep = dependency('libportal-gtk4')
else
    libportal_gtk4_dep = disabler()
endif

gtk4_platform_plugin = shared_module('cogplatform-gtk4',
    'cog-platform-gtk4.c',
    'cog-gtk-settings-dialog.c',
    'cog-gtk-settings-cell-renderer-variant.c',
    c_args: platform_c_args + ['-DG_LOG_DOMAIN="Cog-Gtk4"'],
    dependencies: [
        cogplatformcommon_dep,
        wpebackend_fdo_dep,
        libportal_gtk4_dep,
        dependency('gtk4'),
    ],
    gnu_symbol_visibility: 'hidden',
    install_dir: plugin_path,
    install: true,
)
platform_plugin_targets += [gtk4_platform_plugin]
