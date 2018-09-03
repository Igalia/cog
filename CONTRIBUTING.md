Contributing to Dinghy
======================

Thank you for considering contributing to Dinghy! There are different ways if
contributing, and we appreciate all of them.

- [Source repository](#source-repository)
- [Reporting bugs](#reporting-bugs)
- [Hacking on Dinghy](#hacking-on-dinghy)


Source Repository
-----------------

Dinghy's source repository is hosted at:

> https://github.com/Igalia/dinghy

Development happens in the `master` branch, which must be always buildable.


Reporting Bugs
--------------

Please report bugs at https://github.com/Igalia/dinghy/issues — you can also
browse the issues already reported, and in case someone has already reported
your bug, do not hesitate to add additional information which may help solve
it.

A general advice for bug reports is to mention the version of WebKit that
you are using, and which steps can be followed to reproduce the issue.


Hacking on Dinghy
-----------------

### Working on the source

Please read the [ARCHITECTURE.md](ARCHITECTURE.md) file, which describes the
structure of the source code and some design decisions. That hopefully will
make the code easier to navigate.

### Creating and sending a patch

*(TODO: Provide some notes on how to prepare a development environment which
includes the needed dependencies.)*

Pull requests should be also prepared to be merged onto the `master` branch,
except when the changes specifically apply to a release branch (like
`cog-0.1`) and do not apply to `master`. If the changes in a PR should be
backported to a release branch, link the PR in [this wiki
page](https://github.com/Igalia/cog/wiki/Release-Branches).
