Architecture
============

This document contains an overview description of Cog's architecture.

From the point of view “what gets built”, Cog is split in three components:

- `cog`: The launcher itself. Built from [cog.c](cog.c), uses
  `libcogcore`.

- `cogctl`: Utility to control a launcher instance remotely via the D-Bus
  session bus using the
  [org.freedesktop.Application](https://specifications.freedesktop.org/desktop-entry-spec/desktop-entry-spec-latest.html#dbus)
  interface. Built from [cogctl.c](cogctl.c) and
  [cog-utils.c](cog-utils.c).

- `libcogcore`: Library which contains most of the functionality used to
  implement the launcher, in reusable form.

Most of the items described below are built as part of the latter.


DyLauncher
----------

> *Sources: [cog-launcher.h](core/cog-launcher.h), [cog-launcher.c](core/cog-launcher.c)*

This is the main application object, and inherits from
[GApplication](https://developer.gnome.org/gio/stable/GApplication.html)
(either directly or indirectly, see [GTK+ support](#gtk-support) below).
This class is responsible for:

- Holding a
  [WebKitWebView](https://webkitgtk.org/reference/webkit2gtk/stable/WebKitWebView.html),
  the default URI that it will load, and configuring under the hood
  [WebKitWebContext](https://webkitgtk.org/reference/webkit2gtk/stable/WebKitWebContext.html)
  and
  [WebKitWebsiteDataManager](https://webkitgtk.org/reference/webkit2gtk/stable/WebKitWebsiteDataManager.html)
  instances used with the web view.

- Implements actions which can be invoked using the
  `org.freedesktop.Application` D-Bus interface.

- Keeping track of registered [CogRequestHandler](#cogrequesthandler) instances.

In order to allow customization by applications using the library, the
`CogLauncher::create-web-view` signal can be handled, which should return
a `WebKitWebView` which will prevent an instance being created with the
default settings.


CogRequestHandler
----------------

> _Sources: [cog-request-handler.h](core/cog-request-handler.h),
> [cog-request-handler.c](core/cog-request-handler.c)_

This is a convenience interface which allows implementing custom URI scheme
handlers without having to deal with the details of
[webkit_web_context_register_uri_scheme()](https://webkitgtk.org/reference/webkit2gtk/stable/WebKitWebContext.html#webkit-web-context-register-uri-scheme):
any object which implements this interface can be passed to
`cog_launcher_set_request_handler()`. An advantage of having such an interface
is that it allows for easily extending handlers (by subclassing) or combining
different handlers in an aggregate one, and that handler implementation can
easily keep their state in the object instances.

The following classes provide an implementation if this interface:

- [CogDirectoryFilesHandler](#cogdirectoryfileshandler)


### CogDirectoryFilesHandler

> _Sources: [cog-directory-files-handler.h](core/cog-directory-files-handler.h),
> [cog-directory-files-handler.c](core/cog-directory-files-handler.c)_

This is an implementation of the `CogRequestHandler` interface which loads
content from files inside a local a directory. It handles paths to
directories, and when it gets a request for a path which resolves to a
directory, it returns the contents of an `index.html` file (if present)
or a listing of the directory contents.

The `--dir-handler=` CLI option in Cog uses this class.


Miscellaneous Utilities
-----------------------

> *Sources: [cog-webkit-utils.h](core/cog-webkit-utils.h),
> [cog-webkit-utils.c](core/cog-webkit-utils.c)*

Functions to handle page load errors, page load progress, and Web process
crashes are provided, which can be used as signal callbacks for their
respective `WebKitWebView` signals.

Handlers for errors will load a simple error page in the Web view, and *also*
write a warning to the standard error output.

The load progress handler will write load status messages to the standard
error output.


GTK+ Support
------------

> *Sources: [cog-gtk-utils.h](core/cog-gtk-utils.h),
> [cog-gtk-utils.c](core/cog-gtk-utils.c)*

It is possible to build Cog using WebKitGTK+ instead of the WPE WebKit
port. This is mostly useful for developing features which are independent
of the WebKit port being used. To enable this, pass `-DCOG_USE_WEBKITGTK=ON`
to CMake when configuring your build.

The main change when using WebKitGTK+ is that [CogLauncher](#coglauncher)
will be a subclass of `GtkApplication` —instead of `GApplication`— and that
after creation of the Web view, the `cog_gtk_create_window()` function is
called to ensure that the `WebKitWebView` widget is inserted into a window.
Other than that, the GTK+ specific code is mostly self-contained.
