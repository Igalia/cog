/*
 * cog-shell.c
 * Copyright (C) 2018 Adrian Perez <aperez@igalia.com>
 *
 * SPDX-License-Identifier: MIT
 */

#include "cog-shell.h"

#include "cog-view.h"
#include "cog-viewport.h"

/**
 * CogShell:
 *
 * A shell managed a [class@WebKit.WebView], the default URI that it will
 * load, the view configuration, and keeps track of a number of registered
 * [iface@Cog.RequestHandler] instances.
 *
 * Applications using a shell can handle the [signal@Cog.Shell::create-view]
 * signal to customize the web view.
 */

typedef struct {
    char       *name;
    GKeyFile   *config_file;
    gdouble     device_scale_factor;
    GHashTable *request_handlers; /* (string, RequestHandlerMapEntry) */
    gboolean    automated;

    WebKitSettings   *web_settings;
    WebKitWebContext *web_context;
    GHashTable       *viewports; /* (CogViewportKey, CogViewport) */

#if !COG_USE_WPE2
    WebKitWebsiteDataManager *web_data_manager;
#endif

#if COG_HAVE_MEM_PRESSURE
    WebKitMemoryPressureSettings *web_mem_settings;
    WebKitMemoryPressureSettings *net_mem_settings;
#endif /* COG_HAVE_MEM_PRESSURE */
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
    PROP_VIEWPORT,
    PROP_CONFIG_FILE,
    PROP_DEVICE_SCALE_FACTOR,
    PROP_AUTOMATED,
    PROP_WEB_DATA_MANAGER,
#if COG_HAVE_MEM_PRESSURE
    PROP_WEB_MEMORY_SETTINGS,
    PROP_NETWORK_MEMORY_SETTINGS,
#endif /* COG_HAVE_MEM_PRESSURE */
    N_PROPERTIES,
};

static GParamSpec *s_properties[N_PROPERTIES] = { NULL, };

enum {
    CREATE_VIEW,
    CREATE_VIEWPORT,
    STARTUP,
    SHUTDOWN,
    N_SIGNALS,
};

static int s_signals[N_SIGNALS] = { 0, };

static WebKitWebView*
cog_shell_create_view_base (CogShell *shell)
{
    CogShellPrivate *priv = PRIV(shell);
    return g_object_new(cog_view_get_impl_type(), "settings", cog_shell_get_web_settings(shell), "web-context",
                        cog_shell_get_web_context(shell), "is-controlled-by-automation", priv->automated, NULL);
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

static WebKitWebView *
cog_shell_create_web_view_for_automation(WebKitAutomationSession *session, CogShell *shell)
{
    /* FIXME: Actually create a fresh web view. */
    return cog_shell_get_web_view(shell);
}

static void
cog_shell_automation_started_callback(WebKitWebContext *context, WebKitAutomationSession *session, CogShell *shell)
{
    g_autoptr(WebKitApplicationInfo) info = webkit_application_info_new();
    webkit_application_info_set_version(info, WEBKIT_MAJOR_VERSION, WEBKIT_MINOR_VERSION, WEBKIT_MICRO_VERSION);
    webkit_automation_session_set_application_info(session, info);

    g_signal_connect(session, "create-web-view", G_CALLBACK(cog_shell_create_web_view_for_automation), shell);
}

static void
cog_shell_startup_base(CogShell *shell)
{
    CogShellPrivate *priv = PRIV(shell);

    if (priv->request_handlers) {
        g_hash_table_foreach (priv->request_handlers,
                              (GHFunc) request_handler_map_entry_register,
                              priv->web_context);
    }

    g_autoptr(WebKitWebView) web_view = NULL;
    g_signal_emit(shell, s_signals[CREATE_VIEW], 0, &web_view);

    g_assert(COG_IS_VIEW(web_view));

    /*
     * The web context and settings being used by the web view must be
     * the same that were pre-created by shell.
     */
    g_assert(webkit_web_view_get_settings(web_view) == priv->web_settings);
    g_assert(webkit_web_view_get_context(web_view) == priv->web_context);

    webkit_web_context_set_automation_allowed(priv->web_context, priv->automated);
    g_signal_connect(priv->web_context, "automation-started", G_CALLBACK(cog_shell_automation_started_callback), shell);
    CogViewport *viewport = cog_shell_get_viewport(shell);
    g_assert(viewport);

    cog_viewport_add(viewport, COG_VIEW(web_view));
}

static void
cog_shell_shutdown_base(CogShell *shell G_GNUC_UNUSED)
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
        case PROP_VIEWPORT:
            g_value_set_object(value, cog_shell_get_viewport(shell));
            break;
#if COG_HAVE_MEM_PRESSURE
        case PROP_WEB_MEMORY_SETTINGS:
            g_value_set_boxed(value, PRIV(shell)->web_mem_settings);
            break;
        case PROP_NETWORK_MEMORY_SETTINGS:
            g_value_set_boxed(value, PRIV(shell)->net_mem_settings);
            break;
#endif /* COG_HAVE_MEM_PRESSURE */
        case PROP_WEB_DATA_MANAGER:
#if COG_USE_WPE2
            g_value_set_object(value, NULL);
#else
            g_value_set_object(value, PRIV(shell)->web_data_manager);
#endif
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
    CogShell        *shell = COG_SHELL(object);
    CogShellPrivate *priv = PRIV(shell);
    switch (prop_id) {
        case PROP_NAME:
            priv->name = g_value_dup_string(value);
            break;
        case PROP_WEB_SETTINGS:
            g_clear_object(&priv->web_settings);
            priv->web_settings = g_value_dup_object(value);
            break;
        case PROP_CONFIG_FILE:
            priv->config_file = g_value_dup_boxed(value);
            break;
        case PROP_DEVICE_SCALE_FACTOR:
            priv->device_scale_factor = g_value_get_double(value);
            break;
        case PROP_AUTOMATED:
            priv->automated = g_value_get_boolean(value);
            break;
#if COG_HAVE_MEM_PRESSURE
        case PROP_WEB_MEMORY_SETTINGS:
            g_clear_pointer(&priv->web_mem_settings, webkit_memory_pressure_settings_free);
            priv->web_mem_settings = g_value_dup_boxed(value);
            break;
        case PROP_NETWORK_MEMORY_SETTINGS:
            g_clear_pointer(&priv->net_mem_settings, webkit_memory_pressure_settings_free);
            priv->net_mem_settings = g_value_dup_boxed(value);
            break;
#endif /* COG_HAVE_MEM_PRESSURE */
        case PROP_WEB_DATA_MANAGER:
#if !COG_USE_WPE2
            g_clear_object(&priv->web_data_manager);
            priv->web_data_manager = g_value_dup_object(value);
#endif
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
        }
}

static void
cog_shell_viewport_visible_view_changed(CogViewport *viewport G_GNUC_UNUSED,
                                        GParamSpec *pspec     G_GNUC_UNUSED,
                                        GObject              *shell)
{
    g_assert(COG_IS_SHELL(shell));
    g_assert(COG_IS_VIEWPORT(viewport));

    g_object_notify_by_pspec(shell, s_properties[PROP_WEB_VIEW]);
}

static void
cog_shell_constructed(GObject *object)
{
    G_OBJECT_CLASS(cog_shell_parent_class)->constructed(object);

    CogShellPrivate *priv = PRIV (object);

    if (!priv->web_settings)
        priv->web_settings = g_object_ref_sink(webkit_settings_new());

#if !COG_USE_WPE2
    g_autofree char *data_dir =
        g_build_filename (g_get_user_data_dir (), priv->name, NULL);
    g_autofree char *cache_dir =
        g_build_filename (g_get_user_cache_dir (), priv->name, NULL);

    if (!priv->web_data_manager) {
        if (priv->automated)
            priv->web_data_manager = webkit_website_data_manager_new_ephemeral();
        else
            priv->web_data_manager = webkit_website_data_manager_new("base-data-directory", data_dir,
                                                                     "base-cache-directory", cache_dir, NULL);
    }
#endif

    priv->web_context = g_object_new(WEBKIT_TYPE_WEB_CONTEXT,
#if !COG_USE_WPE2
                                     "website-data-manager", priv->web_data_manager,
#endif
#if COG_HAVE_MEM_PRESSURE
                                     "memory-pressure-settings", priv->web_mem_settings,
#endif
                                     NULL);

    priv->viewports = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_object_unref);
}

static void
cog_shell_dispose(GObject *object)
{
    CogShellPrivate *priv = PRIV(object);

    g_clear_object(&priv->web_context);
    g_clear_object(&priv->web_settings);
#if !COG_USE_WPE2
    g_clear_object(&priv->web_data_manager);
#endif

    g_clear_pointer(&priv->viewports, g_hash_table_unref);
    g_clear_pointer(&priv->request_handlers, g_hash_table_unref);
    g_clear_pointer(&priv->name, g_free);
    g_clear_pointer(&priv->config_file, g_key_file_unref);

    G_OBJECT_CLASS(cog_shell_parent_class)->dispose(object);
}

static void
cog_shell_class_init(CogShellClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    object_class->dispose = cog_shell_dispose;
    object_class->constructed = cog_shell_constructed;
    object_class->get_property = cog_shell_get_property;
    object_class->set_property = cog_shell_set_property;

    klass->create_view = cog_shell_create_view_base;
    klass->startup = cog_shell_startup_base;
    klass->shutdown = cog_shell_shutdown_base;

    /**
     * CogShell::create-view:
     * @self: The shell to create the view for.
     * @user_data: User data.
     *
     * The `create-view` signal is emitted when the shell needs to create
     * a [class@WebKit.WebView].
     *
     * Handling this signal allows to customize how the web view is
     * configured. Note that the web view returned by a signal handler
     * **must** use the settings and context returned by
     * [id@cog_shell_get_web_settings] and [id@cog_shell_get_web_context].
     *
     * Returns: (transfer full) (nullable): A new web view that will be used
     *   by the shell.
     */
    s_signals[CREATE_VIEW] = g_signal_new("create-view",
                                          COG_TYPE_SHELL,
                                          G_SIGNAL_RUN_LAST,
                                          G_STRUCT_OFFSET(CogShellClass, create_view),
                                          g_signal_accumulator_first_wins,
                                          NULL,
                                          NULL,
                                          COG_TYPE_VIEW,
                                          0);

    /**
     * CogShell::create-viewport:
     * @self: The shell creating the new CogViewport.
     * @viewport: The created new viewport.
     * @user_data: User data.
     *
     * The `create-viewport` signal is emitted when the shell creates a new a
     * new CogViewport.
     *
     * Handling this signal allows to customize the actions required to be
     * done during the creation of a new viewport.
     *
     * Returns: (void).
     */
    s_signals[CREATE_VIEWPORT] = g_signal_new("create-viewport", COG_TYPE_SHELL, G_SIGNAL_RUN_FIRST, 0, NULL, NULL,
                                              NULL, G_TYPE_NONE, 1, COG_TYPE_VIEWPORT);

    /**
     * CogShell:name: (attributes org.gtk.Property.get=cog_shell_get_name):
     *
     * Name of the shell.
     *
     * The shell name is used to determine the paths inside the XDG user
     * directories where application-specific files (caches, website data,
     * etc.) will be stored.
     */
    s_properties[PROP_NAME] =
        g_param_spec_string ("name",
                             "Name",
                             "Name of the CogShell instance",
                             NULL,
                             G_PARAM_READWRITE |
                             G_PARAM_CONSTRUCT_ONLY |
                             G_PARAM_STATIC_STRINGS);

    /**
     * CogShell:web-settings: (attributes org.gtk.Property.get=cog_shell_get_web_settings):
     *
     * WebKit settings for this shell.
     */
    s_properties[PROP_WEB_SETTINGS] =
        g_param_spec_object("web-settings",
                            "Web Settings",
                            "The WebKitSettings used by the shell",
                            WEBKIT_TYPE_SETTINGS,
                            G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

    /**
     * CogShell:web-context: (attributes org.gtk.Property.get=cog_shell_get_web_context):
     *
     * The [class@WebKit.WebContext] for this shell's view.
     */
    s_properties[PROP_WEB_CONTEXT] =
        g_param_spec_object ("web-context",
                             "Web Contxt",
                             "The WebKitWebContext used by the shell",
                             WEBKIT_TYPE_WEB_CONTEXT,
                             G_PARAM_READABLE |
                             G_PARAM_STATIC_STRINGS);

    /**
     * CogShell:web-view: (attributes org.gtk.Property.get=cog_shell_get_web_view):
     *
     * The [class@WebKit.WebView] managed by this shell.
     */
    s_properties[PROP_WEB_VIEW] =
        g_param_spec_object("web-view",
                            "Web View",
                            "The WebKitWebView used by the shell",
                            WEBKIT_TYPE_WEB_VIEW,
                            G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

    /**
     * CogShell:viewport: (attributes org.gtk.Property.get=cog_shell_get_viewport) (getter get_viewport):
     *
     * The [class@Viewport] managed by this shell.
     *
     * Since: 0.20
     */
    s_properties[PROP_VIEWPORT] =
        g_param_spec_object("viewport", NULL, NULL, COG_TYPE_VIEWPORT, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    /**
     * CogShell:config-file: (attributes org.gtk.Property.get=cog_shell_get_config_file):
     *
     * Optional configuration as a `GKeyFile`. This allows setting options
     * which can be read elsewhere. This is typically used to provide
     * additional options to [platform modules](overview.html).
     */
    s_properties[PROP_CONFIG_FILE] =
        g_param_spec_boxed ("config-file",
                            "Configuration File",
                            "Configuration file made available to the platform plugin",
                            G_TYPE_KEY_FILE,
                            G_PARAM_READWRITE |
                            G_PARAM_STATIC_STRINGS);

    s_properties[PROP_DEVICE_SCALE_FACTOR] =
        g_param_spec_double ("device-scale-factor",
                             "Device Scale Factor",
                             "Device scale factor used for this shell",
                             0, 64.0, 1.0,
                             G_PARAM_READWRITE);

    s_properties[PROP_AUTOMATED] = g_param_spec_boolean("automated",
                                                        "Automated",
                                                        "Whether this session is automated",
                                                        FALSE,
                                                        G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY);

#if COG_HAVE_MEM_PRESSURE
    /**
     * CogShell:web-memory-settings:
     *
     * Optional memory settings to be applied to web processes, as a
     * `WebKitMemoryPressureSettings` instance.
     */
    s_properties[PROP_WEB_MEMORY_SETTINGS] =
        g_param_spec_boxed("web-memory-settings",
                           "Web process memory pressure settings",
                           "Memory pressure handling settings for web processes",
                           WEBKIT_TYPE_MEMORY_PRESSURE_SETTINGS,
                           G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

    /**
     * CogShell:network-memory-settings:
     *
     * Optional memory settings to be applied to network processes, as a
     * `WebKitMemoryPressureSettings` instance.
     */
    s_properties[PROP_NETWORK_MEMORY_SETTINGS] =
        g_param_spec_boxed("network-memory-settings",
                           "Network process memory pressure settings",
                           "Memory pressure handling settings for network processes",
                           WEBKIT_TYPE_MEMORY_PRESSURE_SETTINGS,
                           G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
#endif /* COG_HAVE_MEM_PRESSURE */

    /**
     * CogShell:web-data-manager:
     *
     * Optional `WebKitWebsiteDataManager` to be used by the shell. If
     * specified at construction, then the [property@Cog.Shell:automated]
     * property will be ignored and the provided object should have
     * [property@WebKit.WebsiteDataManager:is-ephemeral] enabled for running
     * in automation mode.
     */
    s_properties[PROP_WEB_DATA_MANAGER] =
        g_param_spec_object("web-data-manager",
                            "Website data manager",
                            "Data manager applied to web views managed by the shell",
                            WEBKIT_TYPE_WEBSITE_DATA_MANAGER,
                            G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties(object_class, N_PROPERTIES, s_properties);
}

static void
cog_shell_init(CogShell *shell G_GNUC_UNUSED)
{
    CogShellPrivate *priv = PRIV(shell);
    if (!priv->name)
        priv->name = g_strdup(g_get_prgname());
}

/**
 * cog_shell_new: (constructor)
 * @name: Name of the shell.
 * @automated: Whether this shell is controlled by automation.
 *
 * Creates a new shell.
 *
 * Returns: (transfer full): A new shell instance.
 */
CogShell *
cog_shell_new(const char *name, gboolean automated)
{
    return g_object_new(COG_TYPE_SHELL, "name", name, "automated", automated, NULL);
}

/**
 * cog_shell_get_web_context:
 *
 * Obtains the [class@WebKit.WebContext] for this shell.
 *
 * Returns: A web context.
 */
WebKitWebContext *
cog_shell_get_web_context(CogShell *shell)
{
    g_return_val_if_fail(COG_IS_SHELL(shell), NULL);
    return PRIV(shell)->web_context;
}

/**
 * cog_shell_get_web_settings:
 *
 * Obtains the [class@WebKit.Settings] for this shell.
 *
 * Returns: A settings object.
 */
WebKitSettings*
cog_shell_get_web_settings (CogShell *shell)
{
    g_return_val_if_fail (COG_IS_SHELL (shell), NULL);
    return PRIV (shell)->web_settings;
}

/**
 * cog_shell_get_web_view:
 *
 * Obtains the visible [class@WebKit.WebView] for this shell.
 *
 * Returns: (nullable): A web view.
 */
WebKitWebView *
cog_shell_get_web_view(CogShell *shell)
{
    g_return_val_if_fail(COG_IS_SHELL(shell), NULL);
    CogViewport *viewport = cog_shell_get_viewport(shell);
    return WEBKIT_WEB_VIEW(cog_viewport_get_visible_view(viewport));
}

/**
 * cog_shell_get_name:
 *
 * Obtains the name of this shell.
 *
 * Returns: Shell name.
 */
const char*
cog_shell_get_name (CogShell *shell)
{
    g_return_val_if_fail (COG_IS_SHELL (shell), NULL);
    return PRIV (shell)->name;
}

/**
 * cog_shell_get_config_file:
 *
 * Obtains the additional configuration for this shell.
 *
 * See [property@Cog.Shell:config-file] for details.
 *
 * Returns: (nullable): `GKeyFile` used as configuration.
 */
GKeyFile*
cog_shell_get_config_file (CogShell *shell)
{
    g_return_val_if_fail (COG_IS_SHELL (shell), NULL);
    return PRIV (shell)->config_file;
}


gdouble
cog_shell_get_device_scale_factor (CogShell *shell)
{
    g_return_val_if_fail (COG_IS_SHELL (shell), 0);
    return PRIV(shell)->device_scale_factor;
}

gboolean
cog_shell_is_automated(CogShell *shell)
{
    g_return_val_if_fail(COG_IS_SHELL(shell), 0);
    return PRIV(shell)->automated;
}

/**
 * cog_shell_set_request_handler:
 * @scheme: Name of the custom URI scheme.
 * @handler: Handler for the custom URI scheme.
 *
 * Installs a handler for a custom URI scheme.
 */
void
cog_shell_set_request_handler(CogShell *shell, const char *scheme, CogRequestHandler *handler)
{
    g_return_if_fail(COG_IS_SHELL(shell));
    g_return_if_fail(scheme != NULL);
    g_return_if_fail(COG_IS_REQUEST_HANDLER(handler));

    CogShellPrivate *priv = PRIV(shell);

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

/**
 * cog_shell_startup: (virtual startup)
 *
 * Finish initializing the shell.
 *
 * This takes care of registering custom URI scheme handlers and emitting
 * [signal@Cog.Shell::create-view].
 *
 * Subclasses which override this method **must** invoke the base
 * implementation.
 */
void
cog_shell_startup  (CogShell *shell)
{
    g_return_if_fail (COG_IS_SHELL (shell));
    CogShellClass *klass = COG_SHELL_GET_CLASS (shell);
    (*klass->startup) (shell);
}

/**
 * cog_shell_shutdown: (virtual shutdown)
 *
 * Deinitialize the shell.
 */
void
cog_shell_shutdown (CogShell *shell)
{
    g_return_if_fail (COG_IS_SHELL (shell));
    CogShellClass *klass = COG_SHELL_GET_CLASS (shell);
    (*klass->shutdown) (shell);
}

/**
 * cog_shell_get_viewport:
 *
 * Gets the default viewport managed by the shell.
 *
 * Returns: (transfer none): A viewport.
 *
 * Since: 0.20
 */
CogViewport *
cog_shell_get_viewport(CogShell *shell)
{
    CogViewport *viewport = cog_shell_viewport_lookup(shell, COG_VIEWPORT_DEFAULT);

    // Create and add the default viewport if this doesn't exist already
    if (!viewport)
        return cog_shell_viewport_new(shell, COG_VIEWPORT_DEFAULT);

    return viewport;
}

GHashTable *
cog_shell_get_viewports(CogShell *shell)
{
    g_return_val_if_fail(COG_IS_SHELL(shell), NULL);
    return PRIV(shell)->viewports;
}

CogViewport *
cog_shell_viewport_lookup(CogShell *shell, CogViewportKey key)
{
    g_return_val_if_fail(COG_IS_SHELL(shell), NULL);
    return g_hash_table_lookup(PRIV(shell)->viewports, key);
}

CogViewport *
cog_shell_viewport_new(CogShell *shell, CogViewportKey key)
{
    g_return_val_if_fail(COG_IS_SHELL(shell), NULL);
    CogViewport *viewport = cog_viewport_new();
    g_hash_table_insert(PRIV(shell)->viewports, key, viewport);

    g_signal_connect_object(viewport, "notify::visible-view", G_CALLBACK(cog_shell_viewport_visible_view_changed),
                            shell, 0);

    g_signal_emit(shell, s_signals[CREATE_VIEWPORT], 0, viewport);

    return viewport;
}

gboolean
cog_shell_viewport_remove(CogShell *shell, CogViewportKey key)
{
    g_return_val_if_fail(COG_IS_SHELL(shell), false);
    return g_hash_table_remove(PRIV(shell)->viewports, key);
}
