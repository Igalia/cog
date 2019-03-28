Cog
===

![Cog (boat)](data/cog.png)

Cog is a small single “window” launcher for the [WebKit WPE
port](https://trac.webkit.org/wiki/WPE). It is small, provides no user
interface, and is suitable to be used as a Web application container. The
“window” may be fullscreen depending on the WPE backend being used.

This project provides the following components:

- `libcogcore` is a library with ready-to-use components typically needed
  for implementing applications which use the WebKit WPE/GTK+ API.

- `cog` is the launcher itself, implemented using the `libcogcore`
  library.

- `cogctl` is a tool which can be used to control a `cog` instance
  using the D-Bus session bus.

It is possible to disable building the `cog` and `cogctl` programs by passing
`-DCOG_BUILD_PROGRAMS=OFF` to CMake.


Dependencies
------------

For the `master` and [cog-0.3](https://github.com/Igalia/cog/commits/cog-0.3)
branches:

- WPE WebKit 2.24.x, or WebKitGTK 2.20.x when building with
  `COG_USE_WEBKITGTK`.
- [libwpe 1.2.0](https://wpewebkit.org/release/libwpe-1.2.0.html).
- [WPEBackend-fdo 1.2.0](https://wpewebkit.org/release/wpebackend-fdo-1.2.0.html).
  *(Optional, but recommended.)*
- [GLib](https://gitlab.gnome.org/GNOME/glib/) 2.40 or newer.

For the [cog-0.2 branch](https://github.com/Igalia/cog/commits/cog-0.2):

- WPE WebKit 2.22.x, or WebKitGTK+ 2.18.x when building with
  `COG_USE_WEBKITGTK`.
- [libwpe 1.0.0](https://wpewebkit.org/release/libwpe-1.0.0.html).
- [WPEBackend-fdo
  1.0.0](https://wpewebkit.org/release/wpebackend-fdo-1.0.0.html). *(Optional,
  but recommended.)*
- [GLib](https://gitlab.gnome.org/GNOME/glib/) 2.40 or newer.


For the [cog-0.1 branch](https://github.com/Igalia/cog/commits/cog-0.1):

- WPE WebKit 2.20.x, or WebKitGTK+ 2.18.x when building with
  `COG_USE_WEBKITGTK`.
- [WPEBackend 0.2.0](https://wpewebkit.org/release/wpebackend-0.2.0.html).
- [GLib](https://gitlab.gnome.org/GNOME/glib/) 2.40 or newer.


Using Cog
---------

**Compiling** Cog follows the usual procedure for projects which use
[CMake](http://cmake.org): `cmake . && make` should get you started, if your
system has the needed [dependencies](#dependencies) installed.

**Documentation** for `libcogcore` is currently unavailable, and
contributions in this regard are very welcome.

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

