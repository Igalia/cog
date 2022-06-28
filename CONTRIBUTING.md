Title: Contributing

# Contributing to Cog

Thank you for considering contributing to Cog! There are different ways of
contributing, and we appreciate all of them.

## Source Repository

Cog's source repository is hosted at [github.com/Igalia/cog][repo].

Development happens in the `master` branch, which must be always buildable.

[repo]: https://github.com/Igalia/cog


## Reporting Bugs

Please report bugs at [github.com/Igalia/cog/issues][issues] — you can also
browse the issues already reported, and in case someone has already reported
your bug, do not hesitate to add additional information which may help solve
it.

A general advice for bug reports is to mention the version of WebKit that
you are using, and which steps can be followed to reproduce the issue.

[issues]: https://github.com/Igalia/cog/issues


## Hacking on Cog

### Dependencies

This project requires WPE WebKit, [libwpe][], and [WPEBackend-fdo][];
and it is recommended to also have the [libdrm][], X11 and GTK4
development packages installed.

[libwpe]: https://github.com/WebPlatformForEmbedded/libwpe/
[WPEBackend-fdo]: https://github.com/Igalia/WPEBackend-fdo
[libdrm]: https://gitlab.freedesktop.org/mesa/drm

The easiest way of getting the dependencies installed is using your Linux
distribution's package manager to install their development packages.
For example, on a Debian (or Ubuntu) system, the following command should
be enough:

```sh
sudo apt-get install libwpewebkit-1.0-dev libwpe-1.0-dev
```

### Building

Clone the Cog [repository][repo] and do the following:

1. Change directory to the cloned repository.
2. Create a build directory, and change into it. This is where the built
   files will be stored.
3. Run CMake to generate configuration for the build.
4. Then run Make to compile the binaries.

The following sequence of shell commands does the steps above:

```sh
git clone https://github.com/Igalia/cog
cd cog
mkdir _build && cd _build
cmake ..
make
```

Congratulations, you just finished building Cog!


### Running

To run Cog it, change into your build directory, and run the following
command:

```sh
COG_MODULEDIR=$PWD/modules ./cog https://www.igalia.com
```

Cog will try to guess which platform plug-in to use depending on what it finds
running in your system. If you want to manually indicate which one to load,
use the `--platform=` command line option:


```sh
COG_MODULEDIR=$PWD/modules ./cog --platform=x11 https://www.igalia.com
```

Typical values are `wl` (or the legacy `fdo`) or `gtk4` if you are running a
Wayland compositor, `x11` if you are running the X Window system, and so on.


### Creating and sending a patch

Pull requests should be also prepared to be merged onto the `master` branch,
except when the changes specifically apply to a release branch (like
`cog-0.1`) and do not apply to `master`. If the changes in a PR should be
backported to a release branch, link the PR in [this wiki
page](https://github.com/Igalia/cog/wiki/Release-Branches).
