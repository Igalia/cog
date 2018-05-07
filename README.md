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


Dependencies
------------

- WebKit WPE, or WebKitGTK+ 2.18.x when building with `DY_USE_WEBKITGTK`.
- [WPEBackend](https://github.com/WebPlatformForEmbedded/WPEBackend).
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

