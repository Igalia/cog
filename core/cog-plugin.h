/*
 * cog-plugin.h
 * Copyright (C) 2018 Adrian Perez <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#pragma once

#if !(defined(COG_INSIDE_COG__) && COG_INSIDE_COG__)
# error "Do not include this header directly, use <cog.h> instead"
#endif

#include "cog-webkit-utils.h"
#include <glib.h>

G_BEGIN_DECLS

typedef struct _CogPlugin CogPlugin;
typedef struct _CogPluginRegistry CogPluginRegistry;

typedef void     (*CogPluginForEachFunc) (CogPluginRegistry*,
                                          const char *name,
                                          CogPlugin *plugin,
                                          void *userdata);

typedef gboolean (*CogPluginModuleRegisterFunc) (CogPluginRegistry*);

#define COG_PLUGIN_MODULE_REGISTER_FUNC_NAME  "cog_module_initialize"

struct _CogPlugin
{
    gboolean (*setup)     (CogPlugin*, const char *params, GError **error);
    void     (*teardown)  (CogPlugin*);

#if COG_USE_WEBKITGTK
    void     (*__pading0) (void);
#else
    WebKitWebViewBackend* (*get_view_backend) (CogPlugin*,
                                               WebKitWebView *related_view,
                                               GError **error);
#endif /* COG_USE_WEBKITGTK */

    void     (*__pading1) (void);
    void     (*__pading2) (void);
    void     (*__pading3) (void);
    void     (*__pading4) (void);
    void     (*__pading5) (void);
};

static inline gboolean
cog_plugin_setup (CogPlugin *plugin, const char *params, GError **error)
{
    g_return_val_if_fail (plugin != NULL, FALSE);
    return plugin->setup ? (*plugin->setup) (plugin, params, error) : TRUE;
}

static inline void
cog_plugin_teardown (CogPlugin *plugin)
{
    g_return_if_fail (plugin != NULL);
    if (plugin->teardown)
        (*plugin->teardown) (plugin);
}

#if !COG_USE_WEBKITGTK
static inline WebKitWebViewBackend*
cog_plugin_get_view_backend (CogPlugin      *plugin,
                             WebKitWebView  *related_view,
                             GError        **error)
{
    g_return_val_if_fail (plugin != NULL, NULL);
    return plugin->get_view_backend
        ? (*plugin->get_view_backend) (plugin, related_view, error)
        : NULL;
}
#endif /* !COG_USE_WEBKITGTK */


CogPluginRegistry *cog_plugin_registry_new     (void);
gboolean           cog_plugin_registry_add     (CogPluginRegistry *registry,
                                                const char        *name,
                                                CogPlugin         *plugin);
CogPlugin         *cog_plugin_registry_find    (CogPluginRegistry *registry,
                                                const char        *name);
gboolean           cog_plugin_registry_load    (CogPluginRegistry *registry,
                                                const char        *module_path,
                                                GError           **error);
void               cog_plugin_registry_free    (CogPluginRegistry *registry);
void               cog_plugin_registry_foreach (CogPluginRegistry *registry,
                                                CogPluginForEachFunc func,
                                                void              *userdata);

 G_END_DECLS
