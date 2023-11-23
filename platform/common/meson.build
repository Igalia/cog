cogplatformcommon_dependencies = [
    cogcore_dep,
    dependency('epoxy'),
]

cogplatformcommon_sources = []
if libportal_dep.found()
    cogplatformcommon_dependencies += [libportal_dep]
    cogplatformcommon_sources += ['cog-file-chooser.c']
endif

cogplatformcommon_lib = static_library('cogplatformcommon',
    'cog-gl-utils.c',
    'cog-cursors.c',
    cogplatformcommon_sources,
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
