/*
 * test.c
 * Copyright (C) 2018 Adrian Perez <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#include "cog.h"


static gboolean
test_setup (CogPlugin   *plugin,
            const char  *params,
            GError     **error)
{
    g_message ("test: Setup.");
    return TRUE;
}


static void
test_teardown (CogPlugin *plugin)
{
    g_message ("test: Teardown.");
}


G_MODULE_EXPORT
gboolean cog_module_initialize (CogPluginRegistry *registry)
{
    g_message ("test: Module loaded.");

    static CogPlugin test_plugin = {
        .setup = test_setup,
        .teardown = test_teardown,
    };

    return cog_plugin_registry_add (registry, "test", &test_plugin);
}
