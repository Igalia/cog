gamepad_manette = wpe_dep.version().version_compare('>= 1.13.90') and manette_dep.found()

cog_config = configuration_data()
cog_config.set('COG_MODULEDIR', plugin_path)
cog_config.set('PROJECT_VERSION_MAJOR', project_version_components[0])
cog_config.set('PROJECT_VERSION_MINOR', project_version_components[1])
cog_config.set('PROJECT_VERSION_PATCH', project_version_components[2])
cog_config.set('PROJECT_VERSION', meson.project_version())
cog_config.set('COG_VERSION_EXTRA', project_version_git_tag)
cog_config.set('COG_DEFAULT_APPID', cog_launcher_appid)
cog_config.set('COG_DEFAULT_HOME_URI', cog_launcher_home_uri)
cog_config.set10('HAVE_WEBKIT_MEM_PRESSURE_API',
    wpewebkit_dep.version().version_compare('>=2.34.0'))
cog_config.set10('ENABLE_GAMEPAD_MANETTE', gamepad_manette)

cogcore_config_h = configure_file(
    input: 'cog-config.h.in',
    output: 'cog-config.h',
    format: 'cmake@',
    configuration: cog_config,
)

cogcore_headers = files(
    'cog.h',
    'cog-export.h',
    'cog-request-handler.h',
    'cog-directory-files-handler.h',
    'cog-host-routes-handler.h',
    'cog-prefix-routes-handler.h',
    'cog-shell.h',
    'cog-utils.h',
    'cog-webkit-utils.h',
    'cog-platform.h',
    'cog-modules.h',
    'cog-gamepad.h',
    'cog-view.h',
    'cog-viewport.h',
)
cogcore_sources = files(
    'cog-directory-files-handler.c',
    'cog-host-routes-handler.c',
    'cog-modules.c',
    'cog-platform.c',
    'cog-fallback-platform.c',
    'cog-prefix-routes-handler.c',
    'cog-request-handler.c',
    'cog-shell.c',
    'cog-utils.c',
    'cog-webkit-utils.c',
    'cog-gamepad.c',
    'cog-view.c',
    'cog-viewport.c',
)

cogcore_dependencies = [
    wpewebkit_dep,
]

install_headers(cogcore_headers, cogcore_config_h, subdir: 'cog')

if (gamepad_manette)
    cogcore_sources += files('cog-gamepad-manette.c')
    cogcore_dependencies += manette_dep
endif

cogcore_lib = shared_library('cogcore',
    cogcore_config_h,
    cogcore_sources,
    c_args: ['-DG_LOG_DOMAIN="Cog-Core"'],
    dependencies: cogcore_dependencies,
    version: cogcore_soversion,
    soversion: cogcore_soversion_major,
    gnu_symbol_visibility: 'hidden',
    install: true,
)

cogcore_dep = declare_dependency(
    link_with: cogcore_lib,
    dependencies: cogcore_dependencies,
    include_directories: include_directories('.'),
)

import('pkgconfig').generate(
    cogcore_lib,
    description: 'Cog Core - WPE WebKit base launcher',
    subdirs: 'cog',
    requires: cogcore_dependencies,
)
