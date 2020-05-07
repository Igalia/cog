/*
 * cog-shell.c
 * Copyright (C) 2018 Adrian Perez <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#include "cog-shell.h"


typedef struct {
    char             *name;
    WebKitSettings   *web_settings;
    WebKitWebContext *web_context;
    WebKitWebView    *web_view;
    GKeyFile         *config_file;
    char             *rotation;
    GHashTable       *request_handlers;  /* (string, RequestHandlerMapEntry) */
} CogShellPrivate;


G_DEFINE_TYPE_WITH_PRIVATE (CogShell, cog_shell, G_TYPE_OBJECT)

#define PRIV(obj) \
        ((CogShellPrivate*) cog_shell_get_instance_private (COG_SHELL (obj)))

enum {
    PROP_0,
    PROP_NAME,
    PROP_WEB_SETTINGS,
    PROP_WEB_CONTEXT,
    PROP_WEB_VIEW,
    PROP_CONFIG_FILE,
    PROP_ROTATION,
    N_PROPERTIES,
};

static GParamSpec *s_properties[N_PROPERTIES] = { NULL, };


enum {
    CREATE_VIEW,
    STARTUP,
    SHUTDOWN,
    N_SIGNALS,
};

static int s_signals[N_SIGNALS] = { 0, };


static WebKitWebView*
cog_shell_create_view_base (CogShell *shell)
{
    return g_object_new (WEBKIT_TYPE_WEB_VIEW,
                         "settings", cog_shell_get_web_settings (shell),
                         "web-context", cog_shell_get_web_context (shell),
                         NULL);
}


typedef struct {
    CogRequestHandler *handler;
    gboolean           registered;
} RequestHandlerMapEntry;


static inline RequestHandlerMapEntry*
request_handler_map_entry_new (CogRequestHandler *handler)
{
    g_assert (COG_IS_REQUEST_HANDLER (handler));
    RequestHandlerMapEntry *entry = g_slice_new (RequestHandlerMapEntry);
    entry->handler = g_object_ref_sink (handler);
    entry->registered = FALSE;
    return entry;
}


static inline void
request_handler_map_entry_free (void *pointer)
{
    if (pointer) {
        RequestHandlerMapEntry *entry = pointer;
        g_clear_object (&entry->handler);
        g_slice_free (RequestHandlerMapEntry, entry);
    }
}


static void
handle_uri_scheme_request (WebKitURISchemeRequest *request,
                           void                   *userdata)
{
    RequestHandlerMapEntry *entry = userdata;
    g_assert (COG_IS_REQUEST_HANDLER (entry->handler));
    cog_request_handler_run (entry->handler, request);
}


static void
request_handler_map_entry_register (const char             *scheme,
                                    RequestHandlerMapEntry *entry,
                                    WebKitWebContext       *context)
{
    if (context && !entry->registered) {
        webkit_web_context_register_uri_scheme (context,
                                                scheme,
                                                handle_uri_scheme_request,
                                                entry,
                                                NULL);
    }
}



static void
cog_shell_startup_base (CogShell *shell)
{
    CogShellPrivate *priv = PRIV (shell);

    if (priv->request_handlers) {
        g_hash_table_foreach (priv->request_handlers,
                              (GHFunc) request_handler_map_entry_register,
                              priv->web_context);
    }

    g_signal_emit (shell, s_signals[CREATE_VIEW], 0, &priv->web_view);
    g_object_ref_sink (priv->web_view);
    g_object_notify_by_pspec (G_OBJECT (shell), s_properties[PROP_WEB_VIEW]);

    /*
     * The web context and settings being used by the web view must be
     * the same that were pre-created by shell.
     */
    g_assert (webkit_web_view_get_settings (priv->web_view) == priv->web_settings);
    g_assert (webkit_web_view_get_context (priv->web_view) == priv->web_context);
}


static void
cog_shell_shutdown_base (CogShell *shell G_GNUC_UNUSED)
{
}


static void
cog_shell_get_property (GObject    *object,
                        unsigned    prop_id,
                        GValue     *value,
                        GParamSpec *pspec)
{
    CogShell *shell = COG_SHELL (object);
    switch (prop_id) {
        case PROP_NAME:
            g_value_set_string (value, cog_shell_get_name (shell));
            break;
        case PROP_WEB_SETTINGS:
            g_value_set_object (value, cog_shell_get_web_settings (shell));
            break;
        case PROP_WEB_CONTEXT:
            g_value_set_object (value, cog_shell_get_web_context (shell));
            break;
        case PROP_WEB_VIEW:
            g_value_set_object (value, cog_shell_get_web_view (shell));
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}


static void
cog_shell_set_property (GObject      *object,
                        unsigned      prop_id,
                        const GValue *value,
                        GParamSpec   *pspec)
{
    CogShell *shell = COG_SHELL (object);
    switch (prop_id) {
        case PROP_NAME:
            PRIV (shell)->name = g_value_dup_string (value);
            break;
        case PROP_CONFIG_FILE:
            PRIV (shell)->config_file = g_value_get_boxed (value);
            break;
        case PROP_ROTATION:
            PRIV (shell)->rotation = g_value_dup_string (value);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}


static void
cog_shell_constructed (GObject *object)
{
    G_OBJECT_CLASS (cog_shell_parent_class)->constructed (object);

    CogShellPrivate *priv = PRIV (object);

    priv->web_settings = g_object_ref_sink (webkit_settings_new ());

    g_autofree char *data_dir =
        g_build_filename (g_get_user_data_dir (), priv->name, NULL);
    g_autofree char *cache_dir =
        g_build_filename (g_get_user_cache_dir (), priv->name, NULL);

    g_autoptr(WebKitWebsiteDataManager) manager =
        webkit_website_data_manager_new ("base-data-directory", data_dir,
                                         "base-cache-directory", cache_dir,
                                         NULL);

    priv->web_context =
        webkit_web_context_new_with_website_data_manager (manager);
}


static void
cog_shell_dispose (GObject *object)
{
    CogShellPrivate *priv = PRIV (object);

    g_clear_object (&priv->web_view);
    g_clear_object (&priv->web_context);
    g_clear_object (&priv->web_settings);

    g_clear_pointer (&priv->request_handlers, g_hash_table_unref);
    g_clear_pointer (&priv->name, g_free);
    g_clear_pointer (&priv->config_file, g_key_file_unref);

    G_OBJECT_CLASS (cog_shell_parent_class)->dispose (object);
}


static void
cog_shell_class_init (CogShellClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    object_class->dispose = cog_shell_dispose;
    object_class->constructed = cog_shell_constructed;
    object_class->get_property = cog_shell_get_property;
    object_class->set_property = cog_shell_set_property;

    klass->create_view = cog_shell_create_view_base;
    klass->startup = cog_shell_startup_base;
    klass->shutdown = cog_shell_shutdown_base;

    s_signals[CREATE_VIEW] =
        g_signal_new ("create-view",
                      COG_TYPE_SHELL,
                      G_SIGNAL_RUN_LAST,
                      G_STRUCT_OFFSET (CogShellClass, create_view),
                      g_signal_accumulator_first_wins,
                      NULL,
                      NULL,
                      WEBKIT_TYPE_WEB_VIEW,
                      0);

    s_properties[PROP_NAME] =
        g_param_spec_string ("name",
                             "Name",
                             "Name of the CogShell instance",
                             NULL,
                             G_PARAM_READWRITE |
                             G_PARAM_CONSTRUCT_ONLY |
                             G_PARAM_STATIC_STRINGS);

    s_properties[PROP_WEB_SETTINGS] =
        g_param_spec_object ("web-settings",
                             "Web Settings",
                             "The WebKitSettings used by the shell",
                             WEBKIT_TYPE_SETTINGS,
                             G_PARAM_READABLE |
                             G_PARAM_STATIC_STRINGS);

    s_properties[PROP_WEB_CONTEXT] =
        g_param_spec_object ("web-context",
                             "Web Contxt",
                             "The WebKitWebContext used by the shell",
                             WEBKIT_TYPE_WEB_CONTEXT,
                             G_PARAM_READABLE |
                             G_PARAM_STATIC_STRINGS);

    s_properties[PROP_WEB_VIEW] =
        g_param_spec_object ("web-view",
                             "Web View",
                             "The WebKitWebView used by the shell",
                             WEBKIT_TYPE_WEB_VIEW,
                             G_PARAM_READABLE |
                             G_PARAM_STATIC_STRINGS);

    s_properties[PROP_CONFIG_FILE] =
        g_param_spec_boxed ("config-file",
                            "Configuration File",
                            "Configuration file made available to the platform plugin",
                            G_TYPE_KEY_FILE,
                            G_PARAM_READWRITE |
                            G_PARAM_STATIC_STRINGS);

    s_properties[PROP_ROTATION] =
        g_param_spec_string ("rotation",
                             "Rotation",
                             "Rotation of the CogShell instance",
                             NULL,
                             G_PARAM_READWRITE |
                             G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPERTIES, s_properties);
}


static void
cog_shell_init (CogShell *shell G_GNUC_UNUSED)
{
    CogShellPrivate *priv = PRIV (shell);
    if (!priv->name)
        priv->name = g_strdup (g_get_prgname ());
}


CogShell*
cog_shell_new (const char *name)
{
    return g_object_new (COG_TYPE_SHELL,
                         "name", name,
                         NULL);
}


WebKitWebContext*
cog_shell_get_web_context (CogShell *shell)
{
    g_return_val_if_fail (COG_IS_SHELL (shell), NULL);
    return PRIV (shell)->web_context;
}


WebKitSettings*
cog_shell_get_web_settings (CogShell *shell)
{
    g_return_val_if_fail (COG_IS_SHELL (shell), NULL);
    return PRIV (shell)->web_settings;
}


WebKitWebView*
cog_shell_get_web_view (CogShell *shell)
{
    g_return_val_if_fail (COG_IS_SHELL (shell), NULL);
    return PRIV (shell)->web_view;
}


const char*
cog_shell_get_name (CogShell *shell)
{
    g_return_val_if_fail (COG_IS_SHELL (shell), NULL);
    return PRIV (shell)->name;
}


GKeyFile*
cog_shell_get_config_file (CogShell *shell)
{
    g_return_val_if_fail (COG_IS_SHELL (shell), NULL);
    return PRIV (shell)->config_file;
}


const char*
cog_shell_get_rotation (CogShell *shell)
{
    g_return_val_if_fail (COG_IS_SHELL (shell), NULL);
    return PRIV (shell)->rotation;
}


void
cog_shell_set_request_handler (CogShell          *shell,
                               const char        *scheme,
                               CogRequestHandler *handler)
{
    g_return_if_fail (COG_IS_SHELL (shell));
    g_return_if_fail (scheme != NULL);
    g_return_if_fail (COG_IS_REQUEST_HANDLER (handler));

    CogShellPrivate *priv = PRIV (shell);

    if (!priv->request_handlers) {
        priv->request_handlers =
            g_hash_table_new_full (g_str_hash,
                                   g_str_equal,
                                   g_free,
                                   request_handler_map_entry_free);
    }

    RequestHandlerMapEntry *entry =
        g_hash_table_lookup (priv->request_handlers, scheme);

    if (!entry) {
        entry = request_handler_map_entry_new (handler);
        g_hash_table_insert (priv->request_handlers, g_strdup (scheme), entry);
    } else if (entry->handler != handler) {
        g_clear_object (&entry->handler);
        entry->handler = g_object_ref_sink (handler);
    }

    request_handler_map_entry_register (scheme, entry, priv->web_context);
}


void
cog_shell_startup  (CogShell *shell)
{
    g_return_if_fail (COG_IS_SHELL (shell));
    CogShellClass *klass = COG_SHELL_GET_CLASS (shell);
    (*klass->startup) (shell);
}

void
cog_shell_shutdown (CogShell *shell)
{
    g_return_if_fail (COG_IS_SHELL (shell));
    CogShellClass *klass = COG_SHELL_GET_CLASS (shell);
    (*klass->shutdown) (shell);
}
