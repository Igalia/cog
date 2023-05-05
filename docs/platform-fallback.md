Title: Platform: Fallback

# Fallback Platform

## Requirements

The fallback platform plug-in does not have any additional requirement
at build time. An usable WPE backend must be installed at run time. The
following are known to work:

- [WPEBackend-rdk](https://github.com/WebPlatformForEmbedded/WPEBackend-rdk)


## Parameters

The only parameter accepted by the plug-in is the name of the WPE backend
library to search for. This is `default` if not specified, which results
in searching for a shared library named `libWPEBackend-default.so` (and
a few related variants).

The following example set the WPE backend library to use WPEBackend-rdk:

```sh
cog --platform=fallback --platform-params=rdk ...
```
