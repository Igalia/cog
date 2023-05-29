with_wayland_weston_direct_display = (with_wayland_platform
    and get_option('wayland_weston_direct_display')
)
with_wayland_weston_content_protection = (with_wayland_platform
    and with_wayland_weston_direct_display
    and get_option('wayland_weston_content_protection')
)

wayland_platform_dependencies = [
    cogcore_dep,
    wpebackend_fdo_dep,
    dependency('cairo'),
    dependency('egl'),
    dependency('wayland-client'),
    dependency('wayland-egl', required: false),
]
wayland_platform_c_args = [
    '-DG_LOG_DOMAIN="Cog-Wayland"',
    '-DCOG_ENABLE_WESTON_DIRECT_DISPLAY=@0@'.format(with_wayland_weston_direct_display.to_int()),
    '-DCOG_ENABLE_WESTON_CONTENT_PROTECTION=@0@'.format(with_wayland_weston_content_protection.to_int()),
]

wayland_platform_weston_protocols = []
if with_wayland_weston_direct_display
    wayland_platform_weston_protocols += ['weston-direct-display']
    if with_wayland_weston_content_protection
        wayland_platform_weston_protocols += ['weston-content-protection']
    endif
endif

wayland_platform_protocols = {
    'stable': [
        'presentation-time',
        'xdg-shell',
    ],
    'unstable': [
        ['fullscreen-shell', 1],
        ['linux-dmabuf', 1],
        ['text-input', 1],
        ['text-input', 3],
    ],
    'weston': wayland_platform_weston_protocols,
}

wayland_scanner_exe = find_program('wayland-scanner', native: true)
wayland_protocols_dep = dependency('wayland-protocols', method: 'pkg-config')
wayland_protocols_path = wayland_protocols_dep.get_variable(pkgconfig: 'pkgdatadir')

if wayland_platform_weston_protocols.length() > 0
    foreach weston_dep_name : [
            'libweston-13-protocols',
            'libweston-12-protocols',
            'libweston-11-protocols',
            'libweston-10-protocols',
            'libweston-9-protocols',
            'libweston-8-protocols',
        ]
        weston_protocols_dep = dependency(weston_dep_name, method: 'pkg-config', required: false)
        if weston_protocols_dep.found()
            break
        endif
    endforeach

    if not weston_protocols_dep.found()
        error('No usable weston-protocols dependency found')
    endif

    weston_protocols_path = weston_protocols_dep.get_variable(pkgconfig: 'pkgdatadir')

    # The code uses definitions from the drm_fourcc.h header, but does not
    # need to link the library; libdrm here is only a build-time dependency.
    wayland_platform_dependencies += [dependency('libdrm').partial_dependency(compile_args: true)]
endif


fs = import('fs')
wayland_platform_sources = []
foreach kind, proto_list : wayland_platform_protocols
    foreach item : proto_list
        if kind == 'stable'
            proto_name = item
            proto_dir = join_paths(wayland_protocols_path, 'stable', item)
        elif kind == 'unstable' or kind == 'staging'
            proto_name = '@0@-@1@-v@2@'.format(item[0], kind, item[1])
            proto_dir = join_paths(wayland_protocols_path, 'unstable', item[0])
        elif kind == 'weston'
            proto_name = item
            proto_dir = weston_protocols_path
        else
            error('Unknown Wayland protocol type: @0@'.format(kind))
        endif

        xml_path = join_paths(proto_dir, '@0@.xml'.format(proto_name))
        if not fs.is_file(xml_path)
            error('Cannot find protocol @0@, file does not exist: @1@'.format(proto_name, xml_path))
        endif

        wayland_platform_sources += [custom_target(
                '@0@-client-header'.format(proto_name),
                output: '@0@-client.h'.format(proto_name),
                input: xml_path,
                command: [wayland_scanner_exe, 'client-header', '@INPUT@', '@OUTPUT@'],
            ),
            custom_target(
                '@0@-protocol-code'.format(proto_name),
                output: '@0@-protocol.c'.format(proto_name),
                input: xml_path,
                command: [wayland_scanner_exe, 'private-code', '@INPUT@', '@OUTPUT@'],
            ),
        ]
    endforeach
endforeach

wayland_cursor_dep = dependency('wayland-cursor', required: false)
if wayland_cursor_dep.found()
    wayland_platform_dependencies += [wayland_cursor_dep]
    wayland_platform_c_args += ['-DCOG_USE_WAYLAND_CURSOR=1']
endif

wayland_platform_plugin = shared_module('cogplatform-wl',
    'cog-im-context-wl-v1.c',
    'cog-im-context-wl.c',
    'cog-platform-wl.c',
    'cog-popup-menu-wl.c',
    'os-compatibility.c',
    wayland_platform_sources,
    c_args: wayland_platform_c_args,
    dependencies: wayland_platform_dependencies,
    gnu_symbol_visibility: 'hidden',
    install_dir: plugin_path,
    install: true,
)
platform_plugin_targets += [wayland_platform_plugin]
