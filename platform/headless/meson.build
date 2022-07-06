headless_platform_plugin = shared_module('cogplatform-headless',
    'cog-platform-headless.c',
    c_args: ['-DG_LOG_DOMAIN="Cog-Headless"'],
    dependencies: [cogcore_dep, wpebackend_fdo_dep],
    gnu_symbol_visibility: 'hidden',
    install_dir: plugin_path,
    install: true,
)
platform_plugin_targets += [headless_platform_plugin]
