gi_docgen_exe = find_program('gi-docgen', 'gi-docgen.py', native: true)

# XXX: gi-docgen needs to be able to spit out a depfile.
docs_md = [
    'contributing.md',
    'overview.md',
    'platform-drm.md',
    'platform-headless.md',
    'platform-wl.md',
    'platform-x11.md',
    'webdriver.md',
]

docs_toml = configure_file(
    input: 'cog.toml.in',
    output: 'cog.toml',
    configuration: cog_config,
)

gi_outputs = import('gnome').generate_gir(
    cogcore_lib,
    sources: [cogcore_sources, cogcore_headers],
    dependencies: cogcore_dep,
    nsversion: '@0@.0'.format(project_version_components[0]),
    header: 'cog/cog.h',
    namespace: 'Cog',
    identifier_prefix: 'Cog',
    symbol_prefix: 'cog',
    install: false,
)
gi_cogcore_gir = gi_outputs[0]

custom_target('docs',
    build_by_default: true,
    input: [docs_toml, gi_cogcore_gir],
    output: 'html',
    depend_files: docs_md,
    command: [gi_docgen_exe, 'generate',
        '--quiet',
        '--no-namespace-dir',
        '--content-dir', '@CURRENT_SOURCE_DIR@',
        '--output-dir', '@OUTPUT@',
        '--config', '@INPUT0@',
        '@INPUT1@',
    ],
)
