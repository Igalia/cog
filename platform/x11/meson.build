x11_platform_sources = []
if libportal_dep.found()
    x11_platform_sources += ['cog-xdp-parent-x11.c']
endif

x11_platform_c_args = platform_c_args + [
    '-DG_LOG_DOMAIN="Cog-X11"',
]
x11_platform_dependencies = [
    wpebackend_fdo_dep,
    cogplatformcommon_dep,
    dependency('egl'),
    dependency('xcb'),
    dependency('xkbcommon-x11'),
    dependency('x11-xcb'),
    dependency('xcb-cursor'),
]

x11_platform_keyboard = get_option('x11_keyboard')
if x11_platform_keyboard.length() == 0
    warning('No X11 keyboard support chosen, keyboard input will NOT work')
endif

x11_platform_keyboard_dep_names = {
    'xkb': 'xkbcommon-x11',
    'xcb-keysyms': 'xcb-keysyms',
}

foreach item : x11_platform_keyboard
    x11_platform_dependencies += [dependency(x11_platform_keyboard_dep_names[item])]
    x11_platform_c_args += ['-DCOG_X11_USE_@0@=1'.format(item.underscorify().to_upper())]
endforeach

x11_platform_plugin = shared_module('cogplatform-x11',
    'cog-platform-x11.c',
    x11_platform_sources,
    c_args: x11_platform_c_args,
    dependencies: x11_platform_dependencies,
    gnu_symbol_visibility: 'hidden',
    install_dir: plugin_path,
    install: true,
)
platform_plugin_targets += [x11_platform_plugin]
