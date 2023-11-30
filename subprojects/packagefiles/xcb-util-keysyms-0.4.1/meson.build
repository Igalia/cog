project('xcb-util-keysyms', 'c',
  version: '0.4.1',
  default_options: [
  ],
)

xcb_dep = dependency('xcb', version: '>=1.4')
xcb_util_keysyms_dependencies = [xcb_dep, dependency('xproto', version: '>=7.0.8')]

xcb_proto_version = xcb_dep.get_variable('xcbproto_version')
assert(xcb_proto_version.version_compare('>=1.6'),
  'libxcb was compiled against xcb-proto @0@, it needs to be compiled against 1.6 or newer'.format(xcb_proto_version))

xcb_util_keysyms_lib = static_library('xcb-util-keysyms',
  'keysyms/keysyms.c',
  dependencies: xcb_util_keysyms_dependencies,
  pic: true,
)

xcb_util_keysyms_dep = declare_dependency(
  link_with: xcb_util_keysyms_lib,
  dependencies: xcb_util_keysyms_dependencies,
  include_directories: include_directories('keysyms'),
)
