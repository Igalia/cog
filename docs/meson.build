gi_docgen_exe = find_program('gi-docgen', 'gi-docgen.py', native: true)

python3_exe = import('python').find_installation('python3')

docs_toml = configure_file(
    configuration: {
        'PROJECT_VERSION': meson.project_version(),
        'LIBSOUP_API': soup_target_api,
    },
    input: 'cog.toml.in',
    output: 'cog.toml',
)

gi_outputs = import('gnome').generate_gir(
    cogcore_lib,
    sources: [cogcore_sources, cogcore_headers],
    dependencies: cogcore_dep,
    includes: [
        'GLib-2.0', 'GObject-2.0', 'Gio-2.0',
        'Soup-' + soup_target_api,
        # TODO (breaks native CI builds): 'WPEWebKit-' + wpe_target_api
    ],
    nsversion: '@0@.0'.format(project_version_components[0]),
    header: 'cog/cog.h',
    namespace: 'Cog',
    identifier_prefix: 'Cog',
    symbol_prefix: 'cog',
    install: false,
)
gi_cogcore_gir = gi_outputs[0]

docs_dependencies_list = custom_target('docs-deps',
    input: [docs_toml, gi_cogcore_gir],
    output: ['cog-docs.deps'],
    command: [gi_docgen_exe, 'gen-deps',
        '--quiet',
        '--content-dir', '@CURRENT_SOURCE_DIR@',
        '--config', '@INPUT0@',
        '@INPUT1@',
        '@OUTPUT@',
    ],
)

docs_depfile = custom_target('docs-depfile',
    input: [docs_dependencies_list],
    output: ['cog-docs.d'],
    command: [python3_exe, join_paths(meson.source_root(), 'data', 'depfile.py'),
        '@INPUT@',
        '@OUTPUT@',
        'cog-docs.d', 'html'
    ],
)

custom_target('docs',
    build_by_default: true,
    input: [docs_toml, gi_cogcore_gir, docs_depfile],
    output: 'html',
    depfile: 'cog-docs.d',
    command: [gi_docgen_exe, 'generate',
        '--quiet',
        '--no-namespace-dir',
        '--content-dir', '@CURRENT_SOURCE_DIR@',
        '--output-dir', '@OUTPUT@',
        '--config', '@INPUT0@',
        '@INPUT1@',
    ],
)
