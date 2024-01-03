platform_plugins = get_option('platforms')
if platform_plugins.length() == 0
    subdir_done()
endif

# List of platform plug-ins which need the common library.
libcommon_platforms = ['drm', 'x11', 'gtk4', 'wayland']
libcommon_needed = false

# List of platform plug-ins which may use libportal file dialogs.
libportal_platforms = ['x11', 'gtk4', 'wayland']
libportal_usable = false

# Check whether chosen platform plug-ins need additional dependencies.
foreach platform_plugin_name : platform_plugins
    if libcommon_platforms.contains(platform_plugin_name)
        libcommon_needed = true
    endif
    if libportal_platforms.contains(platform_plugin_name)
        libportal_usable = true
    endif
endforeach

# All platform plug-ins currently use the FDO backend as a dependency.
wpebackend_fdo_dep = dependency('wpebackend-fdo-1.0', version: '>=1.12.0')

if libportal_usable and not get_option('libportal').disabled()
    libportal_dep = dependency('libportal', required: get_option('libportal'))
else
    libportal_dep = disabler()
endif
platform_c_args = ['-DCOG_HAVE_LIBPORTAL=@0@'.format(libportal_dep.found().to_int())]

if libcommon_needed
    subdir('common')
endif

platform_plugin_targets = []
foreach platform_plugin_name : platform_plugins
    subdir(platform_plugin_name)
endforeach

fs_mod = import('fs')
ln_exe = find_program('ln', native: true)
platform_plugin_symlinks = []
foreach platform_plugin : platform_plugin_targets
    platform_plugin_symlinks += [
        custom_target(
            'link-@0@'.format(fs_mod.name(platform_plugin.full_path()).underscorify()),
            output: fs_mod.name(platform_plugin.full_path()),
            input: platform_plugin,
            command: [ln_exe, '-srnf', '@INPUT@', '@OUTPUT@'],
            build_by_default: true,
        )
    ]
endforeach
alias_target('plugin-symlinks', platform_plugin_symlinks)
