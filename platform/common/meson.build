cogplatformcommon_dependencies = [
    cogcore_dep,
    dependency('epoxy'),
]

cogplatformcommon_lib = static_library('cogplatformcommon',
    'cog-gl-utils.c',
    dependencies: cogplatformcommon_dependencies,
    build_by_default: false,
    gnu_symbol_visibility: 'hidden',
    implicit_include_directories: false,
    pic: true,
)

cogplatformcommon_dep = declare_dependency(
    link_with: cogplatformcommon_lib,
    dependencies: cogplatformcommon_dependencies,
)
