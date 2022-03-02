find_package (PkgConfig REQUIRED QUIET)
pkg_check_modules(WpeFDO QUIET wpebackend-fdo-1.0>=1.8.0 IMPORTED_TARGET)

if (TARGET PkgConfig::WpeFDO)
  add_library(Wpe::FDO ALIAS PkgConfig::WpeFDO)
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(WpeFDO
  REQUIRED_VARS WpeFDO_LIBRARIES WpeFDO_INCLUDE_DIRS
  FOUND_VAR WpeFDO_FOUND
  VERSION_VAR WpeFDO_VERSION)
