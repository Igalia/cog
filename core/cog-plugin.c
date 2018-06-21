/*
 * cog-plugin.c
 * Copyright (C) 2018 Adrian Perez <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#include "cog-plugin.h"

#include <errno.h>


struct _CogPluginRegistry
{
    GHashTable *plugins;  /* (name, CogPlugin*) */
};


CogPluginRegistry*
cog_plugin_registry_new (void)
{
    CogPluginRegistry *registry = g_slice_new0 (CogPluginRegistry);
    registry->plugins = g_hash_table_new_full (g_str_hash,
                                               g_str_equal,
                                               g_free,
                                               NULL);
    return registry;
}


gboolean
cog_plugin_registry_add (CogPluginRegistry *registry,
                         const char        *name,
                         CogPlugin         *plugin)
{
    if (cog_plugin_registry_find (registry, name))
        return FALSE;

    g_hash_table_insert (registry->plugins, g_strdup (name), plugin);
    return TRUE;
}


CogPlugin*
cog_plugin_registry_find (CogPluginRegistry *registry,
                          const char        *name)
{
    return g_hash_table_lookup (registry->plugins, name);
}


gboolean
cog_plugin_registry_load (CogPluginRegistry *registry,
                          const char        *module_path,
                          GError           **error)
{
    GModule *module = g_module_open (module_path, G_MODULE_BIND_LOCAL);
    if (!module) {
        g_set_error (error,
                     G_FILE_ERROR,
                     g_file_error_from_errno (EINVAL),
                     "Cannot load '%s': %s",
                     module_path,
                     g_module_error ());
        goto beach;
    }

    void *ptr = NULL;
    if (!g_module_symbol (module, COG_PLUGIN_MODULE_REGISTER_FUNC_NAME, &ptr)) {
        g_set_error (error,
                     G_FILE_ERROR,
                     g_file_error_from_errno (EINVAL),
                     "Missing symbol %s in '%s' (%s)",
                     COG_PLUGIN_MODULE_REGISTER_FUNC_NAME,
                     module_path,
                     g_module_error ());
        goto beach;
    }

    CogPluginModuleRegisterFunc module_initialize = ptr;
    if (!(*module_initialize) (registry))
        goto beach;

    g_module_make_resident (module);
    return TRUE;

beach:
    g_clear_pointer (&module, g_module_close);
    return FALSE;
}


void
cog_plugin_registry_free (CogPluginRegistry *registry)
{
    g_clear_pointer (&registry->plugins, g_hash_table_unref);
    g_slice_free (CogPluginRegistry, registry);
}


typedef struct {
    CogPluginRegistry *registry;
    CogPluginForEachFunc callback;
    void *userdata;
} ForeachPluginData;


static void
foreach_plugin (const char        *name,
                CogPlugin         *plugin,
                ForeachPluginData *data)
{
    (*data->callback) (data->registry, name, plugin, data->userdata);
}


void
cog_plugin_registry_foreach (CogPluginRegistry *registry,
                             CogPluginForEachFunc func,
                             void              *userdata)
{
    g_hash_table_foreach (registry->plugins,
                          (GHFunc) foreach_plugin,
                          &((ForeachPluginData) {
                            .registry = registry,
                            .callback = func,
                            .userdata = userdata,
                          }));
}
