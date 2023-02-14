Title: Platform: Headless

# Headless Platform

## Requirements

The headless platform plug-in additionally requires the following libraries:

- **WPEBackend-fdo**


## Parameters

The only parameter accepted by the plug-in is a single unsigned number, used
to configure the maximum allowed refresh rate in frames per second (FPS/Hz);
the default value is 30 Hz.

The following example sets the maximum refresh rate to 60 Hz:

```sh
cog --platform=headless --platform-params=60 ...
```
