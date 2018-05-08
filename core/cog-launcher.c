/*
 * cog-launcher.c
 * Copyright (C) 2017-2018 Adrian Perez <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#include "cog-launcher.h"
#include "cog-request-handler.h"
#include "cog-webkit-utils.h"
#include "cog-utils.h"

#if COG_USE_WEBKITGTK
# include "cog-gtk-utils.h"
#endif


struct _CogLauncher {
    CogLauncherBase   parent;
    WebKitWebContext *web_context;
    WebKitWebView    *web_view;
    char             *home_uri;
    GHashTable       *request_handlers;  /* (string, CogRequestHandler) */
};

G_DEFINE_TYPE (CogLauncher, cog_launcher, COG_LAUNCHER_BASE_TYPE)


enum {
    CREATE_WEB_VIEW,
    LAST_SIGNAL,
};

static int s_signals[LAST_SIGNAL] = { 0, };


enum {
    PROP_0,
    PROP_WEB_CONTEXT,
    PROP_WEB_VIEW,
    PROP_HOME_URI,
    N_PROPERTIES
};

static GParamSpec *s_properties[N_PROPERTIES] = { NULL, };


static void
on_action_quit (G_GNUC_UNUSED GAction  *action,
                G_GNUC_UNUSED GVariant *param,
                CogLauncher            *launcher)
{
    g_application_quit (G_APPLICATION (launcher));
}

static void
on_action_prev (G_GNUC_UNUSED GAction  *action,
                G_GNUC_UNUSED GVariant *param,
                CogLauncher            *launcher)
{
    webkit_web_view_go_back (cog_launcher_get_web_view (launcher));
}

static void
on_action_next (G_GNUC_UNUSED GAction  *action,
                G_GNUC_UNUSED GVariant *param,
                CogLauncher            *launcher)
{
    webkit_web_view_go_forward (cog_launcher_get_web_view (launcher));
}

static void
on_action_reload (G_GNUC_UNUSED GAction  *action,
                  G_GNUC_UNUSED GVariant *param,
                  CogLauncher            *launcher)
{
    webkit_web_view_reload (cog_launcher_get_web_view (launcher));
}

static void
on_action_open (G_GNUC_UNUSED GAction *action,
                GVariant              *param,
                CogLauncher           *launcher)
{
    g_return_if_fail (g_variant_is_of_type (param, G_VARIANT_TYPE_STRING));
    cog_launcher_set_home_uri (launcher, g_variant_get_string (param, NULL));
}

static void
cog_launcher_add_action (CogLauncher *launcher,
                         const char *name,
                         void (*callback) (GAction*, GVariant*, CogLauncher*),
                         const GVariantType *param_type)
{
    g_assert (COG_IS_LAUNCHER (launcher));
    g_assert_nonnull (name);
    g_assert_nonnull (callback);

    GSimpleAction *action = g_simple_action_new (name, param_type);
    g_signal_connect (action, "activate", G_CALLBACK (callback), launcher);
    g_action_map_add_action (G_ACTION_MAP (launcher), G_ACTION (action));
}


typedef struct _RequestHandlerMapEntry RequestHandlerMapEntry;
static void request_handler_map_entry_register (const char*, RequestHandlerMapEntry*, WebKitWebContext*);


static void
cog_launcher_create_web_context (CogLauncher *launcher)
{
    g_autofree char *data_dir = g_build_filename (g_get_user_data_dir (),
                                                  g_get_prgname (),
                                                  NULL);
    g_autofree char *cache_dir = g_build_filename (g_get_user_cache_dir (),
                                                   g_get_prgname (),
                                                   NULL);

    g_autoptr(WebKitWebsiteDataManager) manager =
        webkit_website_data_manager_new ("base-data-directory", data_dir,
                                         "base-cache-directory", cache_dir,
                                         NULL);

    launcher->web_context = webkit_web_context_new_with_website_data_manager (manager);

    /*
     * Request handlers can be registered with CogLauncher before the web
     * context is created: register them now that it has been created.
     */
    if (launcher->request_handlers) {
        g_hash_table_foreach (launcher->request_handlers,
                              (GHFunc) request_handler_map_entry_register,
                              launcher->web_context);
    }
}


static void
cog_launcher_open (GApplication *application,
                   GFile       **files,
                   int           n_files,
                   const char   *hint)
{
    g_assert (n_files);

    if (n_files > 1)
        g_warning ("Requested opening %i files, opening only the first one", n_files);

    g_autofree char *home_uri = g_file_get_uri (files[0]);
    cog_launcher_set_home_uri (COG_LAUNCHER (application), home_uri);
}

static void
cog_launcher_startup (GApplication *application)
{
    G_APPLICATION_CLASS (cog_launcher_parent_class)->startup (application);

    CogLauncher *launcher = COG_LAUNCHER (application);
    cog_launcher_create_web_context (launcher);

    g_signal_emit (launcher,
                   s_signals[CREATE_WEB_VIEW],
                   0,
                   &launcher->web_view);

    /*
     * Create the web view ourselves if the signal handler did not.
     */
    if (!launcher->web_view) {
        g_autoptr(WebKitSettings) settings =
            webkit_settings_new_with_settings ("enable-developer-extras", TRUE, NULL);
        launcher->web_view = WEBKIT_WEB_VIEW (g_object_new (WEBKIT_TYPE_WEB_VIEW,
                                                            "settings", settings,
                                                            "web-context", cog_launcher_get_web_context (launcher),
                                                            NULL));
    }

    /*
     * The web context being used must be the same created by CogLauncher.
     */
    g_assert (webkit_web_view_get_context (launcher->web_view) == cog_launcher_get_web_context (launcher));

    /*
     * Make sure we keep the floating reference to the web view alive.
     */
    g_object_ref_sink (launcher->web_view);

    /*
     * When building with GTK+ support, the GtkApplicationWindow will
     * automatically "hold" the GApplication instance and release it when
     * the window is closed. We have to manually call g_application_hold()
     * ourselves when building against the WPE port.
     */
#if COG_USE_WEBKITGTK
    GtkWidget *window = cog_gtk_create_window (launcher);
    g_object_ref_sink (window);  // Keep the window object alive.
#else
    g_application_hold (application);
#endif

    webkit_web_view_load_uri (launcher->web_view, launcher->home_uri);
}


static void
cog_launcher_activate (GApplication *application)
{
    /*
     * Give user code the chance of creating a customized web view.
     */
    CogLauncher *launcher = COG_LAUNCHER (application);

#if COG_USE_WEBKITGTK
    cog_gtk_present_window (launcher);
#else
    (void) launcher;
#endif
}


static void
cog_launcher_get_property (GObject    *object,
                           unsigned    prop_id,
                           GValue     *value,
                           GParamSpec *pspec)
{
    CogLauncher *launcher = COG_LAUNCHER (object);
    switch (prop_id) {
        case PROP_WEB_CONTEXT:
            g_value_set_object (value, cog_launcher_get_web_context (launcher));
            break;
        case PROP_WEB_VIEW:
            g_value_set_object (value, cog_launcher_get_web_view (launcher));
            break;
        case PROP_HOME_URI:
            g_value_set_string (value, cog_launcher_get_home_uri (launcher));
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}


static void
cog_launcher_set_property (GObject      *object,
                           unsigned      prop_id,
                           const GValue *value,
                           GParamSpec   *pspec)
{
    CogLauncher *launcher = COG_LAUNCHER (object);
    switch (prop_id) {
        case PROP_HOME_URI:
            cog_launcher_set_home_uri (launcher, g_value_get_string (value));
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}


static void
cog_launcher_dispose (GObject *object)
{
    CogLauncher *launcher = COG_LAUNCHER (object);

    g_clear_object (&launcher->web_view);
    g_clear_object (&launcher->web_context);

    g_clear_pointer (&launcher->request_handlers, g_hash_table_unref);

    G_OBJECT_CLASS (cog_launcher_parent_class)->dispose (object);
}


#if COG_DBUS_SYSTEM_BUS
static void
on_system_bus_acquired (GDBusConnection *connection,
                        const char      *name,
                        void            *userdata)
{
    g_autofree char* object_path =
        cog_appid_to_dbus_object_path (g_application_get_application_id (G_APPLICATION (userdata)));
    g_autoptr(GError) error = NULL;
    if (!g_dbus_connection_export_action_group (connection,
                                                object_path,
                                                G_ACTION_GROUP (userdata),
                                                &error))
        g_warning ("Cannot expose remote control interface to system bus: %s",
                   error->message);
}

static void
on_system_bus_name_acquired (G_GNUC_UNUSED GDBusConnection *connection,
                             const char                    *name,
                             G_GNUC_UNUSED void            *userdata)
{
    g_message ("Acquired D-Bus well-known name %s", name);
}

static void
on_system_bus_name_lost (GDBusConnection    *connection,
                         const char         *name,
                         G_GNUC_UNUSED void *userdata)
{
    if (connection) {
        g_message ("Lost D-Bus well-known name %s", name);
    } else {
        g_message ("Lost D-Bus connection to system bus");
    }
}
#endif // COG_DBUS_SYSTEM_BUS


#ifndef COG_DEFAULT_APPID
#  if COG_USE_WEBKITGTK
#    define COG_DEFAULT_APPID "com.igalia." COG_DEFAULT_APPNAME "Gtk"
#  else
#    define COG_DEFAULT_APPID "com.igalia." COG_DEFAULT_APPNAME
#  endif
#endif


static void
cog_launcher_constructed (GObject *object)
{
    G_OBJECT_CLASS (cog_launcher_parent_class)->constructed (object);

    CogLauncher *launcher = COG_LAUNCHER (object);

    cog_launcher_add_action (launcher, "quit", on_action_quit, NULL);
    cog_launcher_add_action (launcher, "previous", on_action_prev, NULL);
    cog_launcher_add_action (launcher, "next", on_action_next, NULL);
    cog_launcher_add_action (launcher, "reload", on_action_reload, NULL);
    cog_launcher_add_action (launcher, "open", on_action_open, G_VARIANT_TYPE_STRING);

#if COG_DBUS_SYSTEM_BUS
    g_bus_own_name (G_BUS_TYPE_SYSTEM,
                    COG_DEFAULT_APPID,
                    G_BUS_NAME_OWNER_FLAGS_NONE,
                    on_system_bus_acquired,
                    on_system_bus_name_acquired,
                    on_system_bus_name_lost,
                    launcher,
                    NULL);
#endif
}


static void
cog_launcher_class_init (CogLauncherClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    object_class->dispose = cog_launcher_dispose;
    object_class->constructed = cog_launcher_constructed;
    object_class->get_property = cog_launcher_get_property;
    object_class->set_property = cog_launcher_set_property;

    GApplicationClass *application_class = G_APPLICATION_CLASS (klass);
    application_class->open = cog_launcher_open;
    application_class->startup = cog_launcher_startup;
    application_class->activate = cog_launcher_activate;

    s_properties[PROP_WEB_CONTEXT] =
        g_param_spec_object ("web-context",
                             "Web Context",
                             "The WebKitWebContext for the launcher",
                             WEBKIT_TYPE_WEB_CONTEXT,
                             G_PARAM_READABLE |
                             G_PARAM_STATIC_STRINGS);
    s_properties[PROP_WEB_VIEW] =
        g_param_spec_object ("web-view",
                             "Web View",
                             "The main WebKitWebView for the launcher",
                             WEBKIT_TYPE_WEB_VIEW,
                             G_PARAM_READABLE |
                             G_PARAM_STATIC_STRINGS);

    s_properties[PROP_HOME_URI] =
        g_param_spec_string ("home-uri",
                             "Home URI",
                             "URI loaded by the main WebKitWebView for the launcher at launch",
                             NULL,
                             G_PARAM_READWRITE |
                             G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class,
                                       N_PROPERTIES,
                                       s_properties);

    s_signals[CREATE_WEB_VIEW] =
        g_signal_new ("create-web-view",
                      COG_TYPE_LAUNCHER,
                      G_SIGNAL_RUN_LAST,
                      0,
                      NULL,
                      NULL,
                      NULL,
                      WEBKIT_TYPE_WEB_VIEW,
                      0);
}


static void
cog_launcher_init (G_GNUC_UNUSED CogLauncher *launcher)
{
}


static void*
cog_launcher_create_instance (void* user_data)
{
    /* Global singleton */
    const GApplicationFlags app_flags =
#if GLIB_CHECK_VERSION(2, 48, 0)
        G_APPLICATION_CAN_OVERRIDE_APP_ID |
#endif // GLIB_CHECK_VERSION
        G_APPLICATION_HANDLES_OPEN ;
    return g_object_new (COG_TYPE_LAUNCHER,
                         "application-id", COG_DEFAULT_APPID,
                         "flags",  app_flags,
                         NULL);
}


CogLauncher*
cog_launcher_get_default (void)
{
    static GOnce create_instance_once = G_ONCE_INIT;
    g_once (&create_instance_once, cog_launcher_create_instance, NULL);
    g_assert_nonnull (create_instance_once.retval);
    return create_instance_once.retval;
}


WebKitWebView*
cog_launcher_get_web_view (CogLauncher *launcher)
{
    g_return_val_if_fail (COG_IS_LAUNCHER (launcher), NULL);
    return launcher->web_view;
}


WebKitWebContext*
cog_launcher_get_web_context (CogLauncher *launcher)
{
    g_return_val_if_fail (COG_IS_LAUNCHER (launcher), NULL);
    return launcher->web_context;
}


const char*
cog_launcher_get_home_uri (CogLauncher *launcher)
{
    g_return_val_if_fail (COG_IS_LAUNCHER (launcher), NULL);
    return launcher->home_uri;
}


void
cog_launcher_set_home_uri (CogLauncher *launcher,
                           const char *home_uri)
{
    g_return_if_fail (COG_IS_LAUNCHER (launcher));

    if (g_strcmp0 (launcher->home_uri, home_uri) == 0)
        return;

    launcher->home_uri = home_uri ? g_strdup (home_uri) : NULL;
    g_object_notify_by_pspec (G_OBJECT (launcher),
                              s_properties[PROP_HOME_URI]);

    if (launcher->web_view) {
        if (launcher->home_uri) {
            webkit_web_view_load_uri (launcher->web_view, launcher->home_uri);
        } else {
            // TODO: Load something nicer than an empty string which would
            //       clearly show that no content is loaded at all.
            webkit_web_view_load_plain_text (launcher->web_view, "");
        }
    }
}


struct _RequestHandlerMapEntry {
    CogRequestHandler *handler;
    gboolean           registered;
};


static inline RequestHandlerMapEntry*
request_handler_map_entry_new (CogRequestHandler *handler)
{
    g_assert_nonnull (handler);

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
                           void                   *user_data)
{
    RequestHandlerMapEntry *entry = user_data;
    g_assert_nonnull (entry->handler);
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


void
cog_launcher_set_request_handler (CogLauncher       *launcher,
                                  const char        *scheme,
                                  CogRequestHandler *handler)
{
    g_return_if_fail (COG_IS_LAUNCHER (launcher));
    g_return_if_fail (scheme != NULL);
    g_return_if_fail (COG_IS_REQUEST_HANDLER (handler));

    if (!launcher->request_handlers) {
        launcher->request_handlers =
            g_hash_table_new_full (g_str_hash,
                                   g_str_equal,
                                   g_free,
                                   request_handler_map_entry_free);
    }

    RequestHandlerMapEntry *entry =
        g_hash_table_lookup (launcher->request_handlers, scheme);

    if (!entry) {
        entry = request_handler_map_entry_new (handler);
        g_hash_table_insert (launcher->request_handlers, g_strdup (scheme), entry);
    } else if (entry->handler != handler) {
        g_clear_object (&entry->handler);
        entry->handler = g_object_ref_sink (handler);
    }

    request_handler_map_entry_register (scheme, entry, launcher->web_context);
}
