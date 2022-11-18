Title: Overview

# Overview

Cog is both a utility library (`cogcore`) for developing applications which
embed the [WPE WebKit][wpewebkit] web rendering engine and a reference
launcher (`cog`, a minimal browser) which is suitable to be used as a web
application container. Cog is released under the terms of the [MIT/X11
license][mit-license]

[wpewebkit]: https://wpewebkit.org
[mit-license]: https://opensource.org/licenses/MIT

Cog depends on the following libraries:

- **GLib**: A general-purpose utility library which provides useful data
  types, macros, string utilities, file utilities, a main loop abstraction,
  and so on. More information available on the [GLib website][glib].
- **GObject**: A library that provides a type system, a collection of
  fundamental types including an object type, and a signal system. More
  information available on the [GObject website][gobject].
- **Soup**:
- **WPE WebKit**:

[glib]: https://developer.gnome.org/glib/stable/
[gobject]: https://developer.gnome.org/gobject/stable/
[gio]: https://developer.gnome.org/gio/stable/

The Cog components are divided in three:

- **Core library** (`libcogcore`): Contains most of the functionality provided
  by Cog in a reusable library.
- **Programs**: Reference implementation of a launcher (`cog`) and a companion
  remote control tool (`cogctl`).
- **Platform modules**: Loadable plug-ins which allow running Cog-based
  programs on different environments (Wayland, X11, etc.)
