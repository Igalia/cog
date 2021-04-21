Title: Platform: DRM

# DRM/KMS Platform

## Requirements

The DRM platform plug-in additionally requires the following libraries:

- **WPEBackend-fdo**:
- **Wayland**:
- **libdrm**:
- **libgbm**:
- **libinput**:
- **libudev**:

## Configuration File Options

If a section named `drm` is found in the configuration file (see
[property@Cog.Shell:config-file]), the following options will be honored:

| Option                       | Type    | Default  |
|:-----------------------------|:--------|:---------|
| `device-scale-factor`        | float   | `1.0`    |
| `disable-atomic-modesetting` | boolean | *detect* |

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

[lwn-modesetting]: https://lwn.net/Articles/653071/
