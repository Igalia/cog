wpebackend_fdo_dep = dependency('wpebackend-fdo-1.0', version: '>=1.8.0')

# Check whether any of the chosen platform plug-ins needs the common library.
foreach platform_plugin_name : ['drm', 'x11', 'gtk4', 'wayland']
    if platform_plugins.contains(platform_plugin_name)
        subdir('common')
        break
    endif
endforeach

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
