Cog
===

![Cog (boat)](data/cog.png)

[![Build - Native](https://github.com/Igalia/cog/actions/workflows/ci-native.yml/badge.svg)](https://github.com/Igalia/cog/actions/workflows/ci-native.yml)
[![Build - ARM](https://github.com/Igalia/cog/actions/workflows/ci-cross.yml/badge.svg)](https://github.com/Igalia/cog/actions/workflows/ci-cross.yml)
[![Code Style](https://github.com/Igalia/cog/actions/workflows/codestyle.yml/badge.svg)](https://github.com/Igalia/cog/actions/workflows/codestyle.yml)

Cog is a small single “window” launcher for the [WebKit WPE
port](https://trac.webkit.org/wiki/WPE). It is small, provides no user
interface, and is suitable to be used as a Web application container. The
“window” may be fullscreen depending on the WPE backend being used.

This project provides the following components:

- `libcogcore` is a library with ready-to-use components typically needed
  for implementing applications which use the WPE WebKit API.

- `cog` is the launcher itself, implemented using the `libcogcore`
  library.

- `cogctl` is a tool which can be used to control a `cog` instance
  using the D-Bus session bus.

It is possible to disable building the `cog` and `cogctl` programs by passing
`-Dprograms=false` to Meson.


Dependencies
------------

Stable releases have the following dependencies:

- WPE WebKit 2.28.x
- WPEBackend-fdo 1.8.x *(optional, recommended)*

Note that building from the `master` branch will often require development
releases of WPE WebKit, libwpe, and WPEBackend-fdo; while older Cog releases
may have different [version
requirements](https://wpewebkit.org/release/schedule/#compatible-components).


Using Cog
---------

**Compiling** Cog follows the usual procedure for projects which use
[Meson](https://mesonbuild.com): `meson setup build && ninja -C build` should
get you started, if your system has the needed [dependencies](#dependencies)
installed.

**Documentation** is available at
[igalia.github.io/cog](https://igalia.github.io/cog/) but it is currently
incomplete. Contributions in this regard are very welcome.

**Bug tracking**: If you have found a bug, take a look at [out issue
tracker](https://github.com/Igalia/cog/issues). Please see the “[reporting
bugs](CONTRIBUTING.md#reporting-bugs)” section in the
[CONTRIBUTING.md](CONTRIBUTING.md) file for guidelines on how to provide a
good bug report.


Contributing
------------

For information on how to report bugs, or how to contribute to Cog, please
check the [CONTRIBUTING.md](CONTRIBUTING.md) file.


License
-------

This project is licensed under the terms of the MIT license. Check the
[COPYING](COPYING) file for details.

