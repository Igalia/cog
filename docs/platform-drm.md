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

| Option                       | Type    | Default  |
|:-----------------------------|:--------|:---------|
| `device-scale-factor`        | float   | `1.0`    |
| `disable-atomic-modesetting` | boolean | *detect* |
| `renderer`                   | string | `"modeset"` |

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


## Parameters

The following parameters can be passed to the platform plug-in during
initialization (e.g. using `cog --platform-params=…`):

| Parameter  | Type   | Default   |
|:-----------|:-------|:----------|
| `renderer` | string | `modeset` |
| `rotation` | number | `0`       |

The `renderer` parameter is the same as the [configuration file
option](#configuration-file-options) of the same name.

The `rotation` parameter indicates the initial [output
rotation](#output-rotation) applied.


## Environment Variables

The following environment variables can be set to further tweak how the
DRM plug-in operates:

| Variable | Type | Default |
|:---------|:-----|--------:|
| `COG_PLATFORM_DRM_VIDEO_MODE` | string | *(unset*) |
| `COG_PLATFORM_DRM_MODE_MAX` | string | *(unset)* |
| `COG_PLATFORM_DRM_CURSOR` | string | *(unset)* |

By default the preferred mode for the first found connected output is used
(if available), otherwise the mode with most resolution.
Setting `COG_PLATFORM_DRM_VIDEO_MODE` instructs the plug-in to pick a
particular video mode, while `COG_PLATFORM_DRM_MODE_MAX` can be used to
limit which modes are considered. Mode strings are formatted as `WxH@R`
(`W`idth and `H`eight in pixels, `R`efresh rate in Hertz), for example
`1920x1080@60` for a typical Full-HD mode. The `@R` part can be omitted
in `COG_PLATFORM_DRM_MODE_MAX`.

Setting `COG_PLATFORM_DRM_CURSOR` to a non-empty string enables showing
the mouse cursor pointer.


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
