Title: Platform: DRM

# DRM/KMS Platform

## Requirements

The DRM platform plug-in additionally requires the following libraries:

- **WPEBackend-fdo**
- **Wayland**
- **GLESv2**
- **libdrm**
- **libgbm**
- **libinput**
- **libudev**

## Configuration File Options

If a section named `drm` is found in the configuration file (see
[property@Cog.Shell:config-file]), the following options will be honored:

| Option                       | Type    | Default     |
|:-----------------------------|:--------|:------------|
| `device-scale-factor`        | float   | `1.0`       |
| `disable-atomic-modesetting` | boolean | *detect*    |
| `renderer`                   | string  | `"modeset"` |
| `cursor`                     | boolean | `false`     |
| `pointer`                    | boolean | `false`     |

The `device-scale-factor` option indicates a scaling factor to be applied to
the rendered content. This is particularly useful for displays with a high
<abbr title="Dots Per Inch">DPI</abbr> to avoid rendered content to appear
abnormally small. A typical setting for displays around the 192DPI mark would
be `2.0`. Note that currently no attempt is done to try guessing a suitable
value, and values other than the default need to be explicitly set.

The `disable-atomic-modesetting` option can be used to explicitly disable
usage of [atomic mode setting][lwn-modesetting]. This is a feature supported
by many modern GPU drivers and it will be used by default when available.  In
some rare cases—mostly buggy or incomplete drivers—it might need to be
manually disable its usage by setting this option to `true`.

The `renderer` option controls how renderer content will be displayed. The
default value is `"modeset"`, which attaches rendered frames directly to
the output. Using the value `"gles"` will “paint” frames onto a quad using
OpenGL ES. The main reason to use the latter is that it supports [output
rotation](#output-rotation).

The `cursor` and `pointer` options control whether the hardware cursor
sprite is drawn and whether `libinput` pointer events are forwarded to
the page, respectively. See [Pointer Input](#pointer-input) for details.


## Parameters

The following parameters can be passed to the platform plug-in during
initialization (e.g. using `cog --platform-params=…`):

| Parameter  | Type    | Default   |
|:-----------|:--------|:----------|
| `renderer` | string  | `modeset` |
| `rotation` | number  | `0`       |
| `cursor`   | boolean | `false`   |
| `pointer`  | boolean | `false`   |

The `renderer` parameter is the same as the [configuration file
option](#configuration-file-options) of the same name.

The `rotation` parameter indicates the initial [output
rotation](#output-rotation) applied.

The `cursor` and `pointer` parameters are the same as the
[configuration file options](#configuration-file-options) of the same
name. See [Pointer Input](#pointer-input) for details. Boolean values
accept `true`, `on`, `1`, `false`, `off`, or `0`.


## Environment Variables

The following environment variables can be set to further tweak how the
DRM plug-in operates:

| Variable | Type | Default |
|:---------|:-----|--------:|
| `COG_PLATFORM_DRM_VIDEO_MODE` | string | *(unset*) |
| `COG_PLATFORM_DRM_MODE_MAX`   | string | *(unset)* |
| `COG_PLATFORM_DRM_CURSOR`     | string | *(unset)* |
| `COG_PLATFORM_DRM_POINTER`    | string | *(unset)* |

By default the preferred mode for the first found connected output is used
(if available), otherwise the mode with highest resolution.
Setting `COG_PLATFORM_DRM_VIDEO_MODE` instructs the plug-in to pick a
particular video mode, while `COG_PLATFORM_DRM_MODE_MAX` can be used to
limit which modes are considered. `COG_PLATFORM_DRM_VIDEO_MODE` accepts
mode strings in the format `WxR` (`W`idth and `H`eight in pixels).
`COG_PLATFORM_DRM_MODE_MAX` additionally accepts mode strings with
a refresh rate in the format `WxH@R`.
for example `1920x1080@60` for a typical Full-HD mode.

Setting `COG_PLATFORM_DRM_CURSOR` or `COG_PLATFORM_DRM_POINTER` to any
non-empty value enables the corresponding option from
[Pointer Input](#pointer-input).


## Pointer Input

Two independent options control how the DRM platform handles
mouse / touchpad input. Both default to off, in which case the plug-in
still delivers keyboard and touchscreen events to the loaded page but
ignores pointer-class devices entirely.

The `pointer` option forwards `libinput` pointer motion, button, and
scroll events to WPE. The page then sees them as DOM `pointermove`,
`pointerdown`, `pointerup`, and `wheel` events. This is independent of
whether anything is drawn on screen; the page is responsible for any
visual cursor (e.g. one drawn on a `<canvas>`).

The `cursor` option allocates a hardware cursor plane and draws a
16×16 sprite on top of the rendered output, moved on every pointer-motion
event. This requires the underlying DRM driver to expose a
`DRM_PLANE_TYPE_CURSOR` plane; many minimal drivers (e.g. SPI/MIPI panels
using `drm_simple_kms_helper`) do not, in which case this option has no
visible effect even when enabled.

For backwards compatibility, enabling `cursor` implies `pointer`. The
four combinations:

| `cursor` | `pointer` | Pointer events to WPE | Hardware cursor sprite |
|:--------:|:---------:|:---------------------:|:----------------------:|
| off      | off       | no                    | no                     |
| off      | on        | yes                   | no                     |
| on       | off       | yes (implied)         | yes (if plane exists)  |
| on       | on        | yes                   | yes (if plane exists)  |

Configuration precedence is: command-line parameter overrides
configuration file, which overrides environment variable. The legacy
behaviour of `COG_PLATFORM_DRM_CURSOR=1` (enabling both pointer dispatch
and the sprite) is preserved by the `cursor` ⇒ `pointer` implication.


## Output Rotation

When using the OpenGL ES renderer using `gles` as value for the `renderer`
parameter, it is possible to rotate the output by multiples of 90 degrees.
This can be set in two ways:

- During initialization via the `rotation` [parameter](#parameters).
- At run time by modifying the `CogDrmPlatform.rotation` object property.

In both cases the value is an integer in the *[0, 3]* range, which is the
amount 90 degree counter-clockwise turns to apply. For example a value of
`3` would result in `3 × 90 = 270` degrees.

The following example shows how to change the rotation at run time:

```c
int main()
{
    cog_modules_add_directory(COG_MODULEDIR);
    g_autoptr(CogPlatform) plat = cog_platform_new("drm", NULL);

    g_autoptr(CogShell) shell = cog_shell_new("example", FALSE);
    cog_platform_setup(plat, shell, "renderer=gles", NULL);

    /* Set the rotation. */
    g_object_set(plat, "rotation", 3);

    /* Now proceed to run a GMainLoop normally… */
}
```


[lwn-modesetting]: https://lwn.net/Articles/653071/
