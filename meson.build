project('cog', 'c',
    meson_version: '>=0.55',
    default_options: [
        'buildtype=debugoptimized',
        'b_ndebug=if-release',
        'c_std=c11',
    ],
    license: 'MIT',
    version: '0.19.1',
)

# Before making a release, the LT_VERSION string should be modified.
# The string is of the form [C, R, A].
# - If interfaces have been changed or added, but binary compatibility has
#   been preserved, change to [C+1, 0, A+1].
# - If binary compatibility has been broken (eg removed or changed interfaces)
#   change to [C+1, 0, 0]
# - If the interface is the same as the previous version, use [C, R+1, A].
cogcore_soversion = [12, 0, 0]

# Mangle [C, R, A] into an actual usable *soversion*.
cogcore_soversion_major = cogcore_soversion[0] - cogcore_soversion[2]  # Current-Age
cogcore_soversion_minor = cogcore_soversion[2]  # Age
cogcore_soversion_micro = cogcore_soversion[1]  # Revision
cogcore_soversion = '@0@.@1@.@2@'.format(cogcore_soversion_major, cogcore_soversion_minor, cogcore_soversion_micro)

project_version_components = meson.project_version().split('.')

# When running from a Git checkout, try to obtain the current revision.
project_version_git_tag = ''
if meson.version().version_compare('>=0.56')
    git_dir = join_paths(meson.project_source_root(), '.git')
else
    git_dir = join_paths(meson.current_source_dir(), '.git')
endif

if import('fs').exists(git_dir)
    project_version_git_tag = '+git'

    git_exe = find_program('git', native: true, required: false)
    if git_exe.found()
        git_result = run_command(git_exe, 'rev-list', '--max-count=1',
            '--abbrev-commit', 'HEAD',
            env: {'GIT_DIR': git_dir},
            capture: true,
            check: false,
        )
        git_output = git_result.stdout().strip()
        if git_result.returncode() == 0 and git_output != ''
            project_version_git_tag = '@0@-@1@'.format(project_version_git_tag, git_output)
        endif

        git_result = run_command(git_exe, 'status', '--porcelain',
            env: {'GIT_DIR': git_dir},
            capture: true,
            check: false,
        )
        git_output = git_result.stdout().strip()
        if git_result.returncode() == 0 and git_output != ''
            project_version_git_tag = '@0@-dirty'.format(project_version_git_tag)
        endif
    endif

    message('Git tag: @0@'.format(project_version_git_tag))
endif

plugin_path = get_option('plugin_path')
if plugin_path == ''
    # TODO: Consider renaming s/modules/plugins/
    plugin_path = join_paths(get_option('prefix'), get_option('libdir'), 'cog', 'modules')
endif

with_programs = get_option('programs')

cog_launcher_appid = get_option('cog_appid')
cog_launcher_home_uri = get_option('cog_home_uri')
cog_launcher_system_bus = with_programs and get_option('cog_dbus_control') == 'system'

add_project_arguments('-DCOG_INSIDE_COG__=1', language: 'c')

wpe_target_api_version_required = {
    '2.0': '>=2.39.91',
    '1.1': '>=2.33.1',
    '1.0': '>=2.28.0',
}

wpe_target_api = get_option('wpe_api')
if wpe_target_api == 'auto'
    foreach try_api : ['2.0', '1.1', '1.0']
        wpewebkit_dep = dependency('wpe-webkit-' + try_api,
            version: wpe_target_api_version_required[try_api],
            required: false)
        if wpewebkit_dep.found()
            wpe_target_api = try_api
            break
        endif
    endforeach
else
    wpewebkit_dep = dependency('wpe-webkit-' + wpe_target_api,
        version: wpe_target_api_version_required[wpe_target_api])
endif
if wpe_target_api == 'auto' or not wpewebkit_dep.found()
    error('WPE WebKit not found')
endif

if wpe_target_api == '1.0'
    add_project_arguments('-DCOG_USE_SOUP2=1', language: 'c')
    gio_dep = dependency('gio-2.0', version: '>=2.44')
    libsoup_dep = dependency('libsoup-2.4')
    soup_target_api = '2.4'
else
    add_project_arguments('-DCOG_USE_SOUP2=0', language: 'c')
    gio_dep = dependency('gio-2.0', version: '>=2.67.4')
    libsoup_dep = dependency('libsoup-3.0', version: '>=2.99.7')
    soup_target_api = '3.0'
endif

if wpe_target_api == '2.0'
    add_project_arguments('-DCOG_USE_WPE2=1', language: 'c')
else
    add_project_arguments('-DCOG_USE_WPE2=0', language: 'c')
endif

if cog_launcher_system_bus
    cog_dbus_system_owner = get_option('cog_dbus_system_owner')
    # Generate and install D-Bus policy configuration file.
    configure_file(
        configuration: {
            'COG_DEFAULT_APPID': cog_launcher_appid,
            'COG_DBUS_OWN_USER': cog_dbus_system_owner,
        },
        input: 'dbus/policy.conf.in',
        output: '@0@.conf'.format(cog_launcher_appid),
        install_dir: join_paths(get_option('datadir'), 'dbus-1', 'system.d'),
        install: true,
    )

    # Let the source code know that the option is enabled.
    add_project_arguments(
        '-DCOG_DBUS_SYSTEM_BUS=1',
        '-DCOG_DBUS_OWN_USER="@0@"'.format(cog_dbus_system_owner),
        language: 'c'
    )
endif

wpe_dep = dependency('wpe-1.0', version: '>=1.14.0')
manette_dep = dependency('manette-0.2', version: '>=0.2.4', required: false)

subdir('core')
subdir('platform')

if get_option('documentation')
    subdir('docs')
endif

if get_option('examples')
    subdir('examples')
endif

if with_programs
    subdir('launcher')
    if get_option('manpages')
        install_man('data/cog.1', 'data/cogctl.1')
    endif
endif
