Title: Platform: WL

# Wayland Platform

The Wayland (`wl`) platform plug-in allows Cog to run under a compositor which
supports the [Wayland protocol](https://wayland.freedesktop.org/). The
following is a non-comprehensive list of compositors known to work:

- [GNOME](https://gnome.org) Shell, in a Wayland session.
- [Weston](https://wayland.pages.freedesktop.org/weston/), the reference
  compositor developed as part of the Wayland project.
- [Sway](https://swaywm.org/), a tiling compositor based on [wlroots][wlroots].
- [Cage](https://www.hjdskes.nl/projects/cage/), a kiosk-oriented compositor
  based on [wlroots][wlroots].
- [labwc](https://labwc.github.io/), a lean stacking compositor based on
  [wlroots][wlroots].

[wlroots]: https://gitlab.freedesktop.org/wlroots/wlroots

The compositor needs to be running *before* attempting to launch Cog. The
environment variables `WAYLAND_DISPLAY` and `XDG_RUNTIME_DIR` need to be
defined with suitable values. The [troubleshooting](#troubleshooting)
section provides some tips on how to obtain the needed values.

If you start Cog *from within* the compositor, e.g. running `cog <url>`
from a terminal in a GNOME session, should work without any manual
intervention.

A notable exception is Cage, because it is designed to run a single
application under the compositor control. The command used to execute
Cog needs to be provided when starting it:

```sh
cage -- cog wpewebkit.org
```

Weston and all the [wlroots][wlroots]-based compositors listed above will run
nested in their own window, inside another Wayland or X11 session, if the
needed environment variables are set accordingly (`XDG_RUNTIME_DIR`, plus
`WAYLAND_DISPLAY`, or `DISPLAY` for X11). This setup can be useful for
development.


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

On top of the [built-in keybindings][id@cog_view_set_use_key_bindings], the
additional following key bindings are supported:

| Binding | Action             |
|:--------|:-------------------|
| `F11`   | Toggle fullscreen. |


## Troubleshooting

Before looking further, check that:

* There is a Wayland compositor running.
* The `XDG_RUNTIME_DIR` environment variable points to a local path writable
  by the user account which will run Cog.
* The `WAYLAND_DISPLAY` environment variable is defined and contains the name
  of an Unix socket which must be present at
  `$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY`.
* The user account which will run Cog can access the device nodes needed for
  GPU rendering, typically present under `/dev/dri`.

If a compositor is running, the values needed for the `WAYLAND_DISPLAY` and
`XDG_RUNTIME_DIR` environment variables can be found by pulling
`XDG_RUNTIME_DIR` from the running process&mdash;make sure to replace `cage`
when using a different compositor:

```sh
tr '\0' '\n' < /proc/$(pidof cage)/environ | grep '^XDG_RUNTIME_DIR='
```

Then look into the resulting location for a socket named `wayland-N`, where
*N* is a number. The name of the socket is the value for `WAYLAND_DISPLAY`.

The following shell snippet will define both variables if needed:

```bash
# Needs Bash or Zsh

if [[ -z $XDG_RUNTIME_DIR ]] ; then
  eval "$(tr '\0' '\n' < /proc/$(pidof cage)/environ | grep '^XDG_RUNTIME_DIR=')"
fi

if [[ -z $WAYLAND_DISPLAY ]] ; then
  for path in "$XDG_RUNTIME_DIR"/* ; do
    name=${path#${XDG_RUNTIME_DIR}/}
    if [[ $name = wayland-* && -S $path ]] ; then
      WAYLAND_DISPLAY=$name
      break
    fi
  done
fi

if [[ -z $WAYLAND_DISPLAY ]] ; then
  echo 'No Wayland compositor running'
else
  export WAYLAND_DISPLAY XDG_RUNTIME_DIR
fi
```
