Contributing to Cog
=======================

Thank you for considering contributing to Cog! There are different ways if
contributing, and we appreciate all of them.

- [Source repository](#source-repository)
- [Reporting bugs](#reporting-bugs)
- [Hacking on Cog](#hacking-on-cog)


Source Repository
-----------------

Cog's source repository is hosted at:

> https://github.com/Igalia/cog

Development happens in the `master` branch, which must be always buildable.


Reporting Bugs
--------------

Please report bugs at https://github.com/Igalia/cog/issues — you can also
browse the issues already reported, and in case someone has already reported
your bug, do not hesitate to add additional information which may help solve
it.

A general advice for bug reports is to mention the version of WebKit that
you are using, and which steps can be followed to reproduce the issue.


Hacking on Cog
-----------------

### Building/running
This project requires libwpewebkit-1.0-dev, 
libwpe-1.0-dev (https://github.com/WebPlatformForEmbedded/libwpe/), 
libwpebackend-fdo-1.0-dev (https://github.com/Igalia/WPEBackend-fdo) 
and GTK4 for the build/run process.

After cloning each dependency, change directory to the cloned directory. 
If this directory doesn't have a build folder,
create one. Then change directory to the build directory and run `meson ..` then `ninja` to build this
dependency.
You could also directly install them through the terminal if you can find them
on your linux distro's repositories. For example `sudo apt-get install libwpewebkit-1.0-dev libwpe-1.0-dev` 
installs the respective packages in versions of ubuntu.

Clone the cog repository and do the following 
* change directory to the cloned repository
* create a build directory
* cd into this directory. This is where your build files will be stored.
* run `cmake -DCOG_PLATFORM_X11=ON -DCOG_PLATFORM_GTK4=ON ..` to generate the build files.
* Then run `make` to generate the binaries.

You just finished building cog. To run it, run `COG_MODULEDIR=$PWD/modules ./cog --platform=gtk4 https://www.igalia.com/`
while in the build directory. If you are running Wayland, this will startup fine. Valid platforms include 'gtk4', 'x11', 'drm', 'fdo' and 'headless'.

### Working on the source

Please read the [ARCHITECTURE.md](ARCHITECTURE.md) file, which describes the
structure of the source code and some design decisions. That hopefully will
make the code easier to navigate.

### Creating and sending a patch

Pull requests should be also prepared to be merged onto the `master` branch,
except when the changes specifically apply to a release branch (like
`cog-0.1`) and do not apply to `master`. If the changes in a PR should be
backported to a release branch, link the PR in [this wiki
page](https://github.com/Igalia/cog/wiki/Release-Branches).
