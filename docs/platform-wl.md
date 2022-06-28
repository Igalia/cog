Title: Platform: WL

# Wayland Platform

## Requirements

The `wl` (Wayland) platform plug-in additionally requires the following
libraries and packages:

- **WPEBackend-fdo**
- **Wayland**
- **libxkbcommon**
- **wayland-protocols**
- **wayland-scanner**

## Environment Variables

The following environment variables can be set to change how the Wayland
surface used for output will be configured:

| Variable | Type | Default |
|:---------|:-----|--------:|
| `COG_PLATFORM_WL_VIEW_FULLSCREEN` | boolean | `0` |
| `COG_PLATFORM_WL_VIEW_MAXIMIZE`   | boolean | `0` |
| `COG_PLATFORM_WL_VIEW_WIDTH`      | number  | `1024` |
| `COG_PLATFORM_WL_VIEW_HEIGHT`     | number  | `768` |

Setting `COG_PLATFORM_WL_VIEW_FULLSCREEN` will take precedence over
`COG_PLATFORM_WL_VIEW_MAXIMIZE`, and if either of those are enabled the size
specified with `COG_PLATFORM_WL_VIEW_WIDTH` and `COG_PLATFORM_WL_VIEW_HEIGHT`
will be ignored.

Note that these are *requests* made to the Wayland compositor, which has the
final say in how the surface is presented to the user. In general, surfaces
which have been fullscreened completely cover the output (i.e. they take the
whole screen), while for maximized surfaces the compositor my still show
some user interface elements (like borders or buttons) while trying to make
the surface as big as possible. Some compositors take faster approaches when
there is only a single fullscreen surface being displayed.


## Key Bindings

The following key bindings are available:

| Binding           | Action                          |
|:------------------|:--------------------------------|
| `F11`             | Toggle fullscreen.              |
| `Ctrl-W`          | Exit the application.           |
| `Ctrl-+`          | Zoom in.                        |
| `Ctrl--`          | Zoom out.                       |
| `Ctrl-0`          | Restore default zoom level.     |
| `Alt-Left`        | Go to previous page in history. |
| `Alt-Right`       | Go to next page in history.     |
| `Ctrl-R` / `F5`   | Reload current page.            |

Currently there is no mechanism to modify or disable these key bindings.
