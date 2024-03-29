Title: Running Cog Applications

Running Cog Applications
========================

Environment Variables
---------------------

The runtime behaviour of the Cog core library can be influenced by a number of
environment variables.

`COG_MODULEDIR`
:  Allows to specify a non-default location where to search for platform
   plug-in modules. The default location is under `<libdir>/cog/modules`
   unless a custom location was specified during configuration when
   building. See [id@cog_modules_add_directory] and [id@cog_init] for
   more information.

`COG_PLATFORM_NAME`
:  This variable can be set to the name of the platform plug-in module to
   use and prevent automatically determining which one to use.
   See [id@cog_init] for more information.

`COG_PLATFORM_PARAMS`
:  This variable may contain setup parameters for the chosen platform plug-in
   module. The parameters supported by each platform may be found in their
   respective documentation pages. The format of the parameters string is
   typically (but not always) a comma-separated list of `variable=value`
   assignments. See [id@cog_platform_setup] for more information.
