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

#include <string.h>


struct _CogLauncher {
    CogLauncherBase   parent;
    WebKitSettings   *web_settings;
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
    PROP_WEB_SETTINGS,
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

    /*
     * Request handlers can be registered with CogLauncher before the web
     * context is created: register them now that it has been created.
     */
    if (launcher->request_handlers) {
        g_hash_table_foreach (launcher->request_handlers,
                              (GHFunc) request_handler_map_entry_register,
                              launcher->web_context);
    }

    g_signal_emit (launcher,
                   s_signals[CREATE_WEB_VIEW],
                   0,
                   &launcher->web_view);

    /*
     * Create the web view ourselves if the signal handler did not.
     */
    if (!launcher->web_view) {
        launcher->web_view = WEBKIT_WEB_VIEW (g_object_new (WEBKIT_TYPE_WEB_VIEW,
                                                            "settings", cog_launcher_get_web_settings (launcher),
                                                            "web-context", cog_launcher_get_web_context (launcher),
                                                            NULL));
    }

    /*
     * The web context and settings being used by the web view must be
     * the same that were pre-created by CogLauncher.
     */
    g_assert (webkit_web_view_get_settings (launcher->web_view) == cog_launcher_get_web_settings (launcher));
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
        case PROP_WEB_SETTINGS:
            g_value_set_object (value, cog_launcher_get_web_settings (launcher));
            break;
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
    g_clear_object (&launcher->web_settings);

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

    launcher->web_settings = g_object_ref_sink (webkit_settings_new ());

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

    launcher->web_context =
        webkit_web_context_new_with_website_data_manager (manager);

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

    s_properties[PROP_WEB_SETTINGS] =
        g_param_spec_object ("web-settings",
                             "Web Settings",
                             "The WebKitSettings for the launcher",
                             WEBKIT_TYPE_SETTINGS,
                             G_PARAM_READABLE |
                             G_PARAM_STATIC_STRINGS);

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


WebKitSettings*
cog_launcher_get_web_settings (CogLauncher *launcher)
{
    g_return_val_if_fail (COG_IS_LAUNCHER (launcher), NULL);
    return launcher->web_settings;
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


void
cog_launcher_add_web_settings_option_entries (CogLauncher *launcher)
{
    g_return_if_fail (COG_IS_LAUNCHER (launcher));

    g_autofree GOptionEntry *option_entries =
        cog_option_entries_from_class (G_OBJECT_GET_CLASS (launcher->web_settings));
    if (!option_entries) {
        g_critical ("Could not deduce option entries for WebKitSettings."
                    " This should not happen, continuing but YMMV.");
        return;
    }

    g_autoptr(GOptionGroup) option_group =
        g_option_group_new ("websettings",
                            "WebKitSettings options can be used to configure features exposed to the loaded Web content.\n"
                            "\n"
                            "  BOOL values are either 'true', '1', 'false', or '0'. Ommitting the value implies '1'.\n"
                            "  INTEGER values can be decimal, octal (prefix '0'), or hexadecimal (prefix '0x').\n"
                            "  UNSIGNED values behave like INTEGER, but negative values are not accepted.\n"
                            "  FLOAT values may optionally use decimal separators and scientific notation.\n"
                            "  STRING values may need quoting when passed from the shell.\n",
                            "Show WebKitSettings options",
                            launcher->web_settings,
                            NULL);
    g_option_group_add_entries (option_group, option_entries);
    g_application_add_option_group (G_APPLICATION (launcher),
                                    g_steal_pointer (&option_group));
}


static gboolean
option_entry_parse_cookie_store (const char          *option G_GNUC_UNUSED,
                                 const char          *value,
                                 WebKitCookieManager *cookie_manager,
                                 GError             **error)
{
    static const struct {
        const char              *name;
        WebKitCookieAcceptPolicy policy;
    } value_map[] = {
        { "always",       WEBKIT_COOKIE_POLICY_ACCEPT_ALWAYS         },
        { "never",        WEBKIT_COOKIE_POLICY_ACCEPT_NEVER          },
        { "nothirdparty", WEBKIT_COOKIE_POLICY_ACCEPT_NO_THIRD_PARTY },
    };

    for (unsigned i = 0; i < G_N_ELEMENTS (value_map); i++) {
        if (strcmp (value, value_map[i].name) == 0) {
            webkit_cookie_manager_set_accept_policy (cookie_manager,
                                                     value_map[i].policy);
            return TRUE;
        }
    }

    g_set_error (error,
                 G_OPTION_ERROR,
                 G_OPTION_ERROR_BAD_VALUE,
                 "Invalid cookie mode '%s'",
                 value);
    return FALSE;
}


static void
cookie_set_session (SoupCookie *cookie, gboolean session)
{
    if (session)
        soup_cookie_set_expires (cookie, NULL);
}


typedef void (*CookieFlagCallback) (SoupCookie*, gboolean);


static inline CookieFlagCallback
option_entry_parse_cookie_add_get_flag_callback (const char *name)
{
    static const struct {
        const char *name;
        CookieFlagCallback callback;
    } flag_map[] = {
        { "httponly", soup_cookie_set_http_only },
        { "secure",   soup_cookie_set_secure    },
        { "session",  cookie_set_session        },
    };

    for (unsigned i = 0; i < G_N_ELEMENTS (flag_map); i++)
        if (strcmp (name, flag_map[i].name) == 0)
            return flag_map[i].callback;

    return NULL;
}


static void
on_cookie_added (WebKitCookieManager *cookie_manager,
                 GAsyncResult        *result,
                 GMainLoop           *loop)
{
    g_autoptr(GError) error = NULL;
    if (!webkit_cookie_manager_add_cookie_finish (cookie_manager, result, &error)) {
        g_warning ("Error setting cookie: %s", error->message);
    }
    g_main_loop_quit (loop);
}


static gboolean
option_entry_parse_cookie_add (const char          *option G_GNUC_UNUSED,
                               const char          *value,
                               WebKitCookieManager *cookie_manager,
                               GError             **error G_GNUC_UNUSED)
{
    g_autofree char *domain = g_strdup (value);

    char *flagstr = strchr (domain, ':');
    if (!flagstr)
        goto bad_format;
    *flagstr++ = '\0';

    char *contents = strchr (flagstr, ':');
    if (!contents)
        goto bad_format;

    // The domain might include a port in the domain, in that
    // case skip forward to the next colon after the port number.
    if (g_ascii_isdigit (contents[1])) {
        if (!(contents = strchr (contents + 1, ':')))
            goto bad_format;
    }
    *contents++ = '\0';

    // The contents of the cookie cannot be empty.
    if (!contents[0])
        goto bad_format;

    g_autoptr(SoupCookie) cookie = soup_cookie_parse (contents, NULL);
    if (!cookie)
        goto bad_format;

    soup_cookie_set_domain (cookie, domain);

    // Go through the flags.
    if (flagstr && flagstr[0]) {
        g_auto(GStrv) flags = g_strsplit (flagstr, ",", -1);

        for (unsigned i = 0; flags[i] != NULL; i++) {
            // Skip the optional leading +/- signs.
            const char *flag = flags[i];
            gboolean flag_value = flag[0] != '-';
            if (flag[0] == '+' || flag[0] == '-')
                flag++;

            const CookieFlagCallback flag_callback =
                option_entry_parse_cookie_add_get_flag_callback (flag);
            if (!flag_callback) {
                g_set_error (error,
                             G_OPTION_ERROR,
                             G_OPTION_ERROR_BAD_VALUE,
                             "Invalid cookie flag '%s'",
                             flag);
                return FALSE;
            }
            (*flag_callback) (cookie, flag_value);
        }
    }

    // XXX: If the cookie has no path defined, conversion to WebKit's
    //      internal format will fail and the WebProcess will spit ouy
    //      a critical error -- and the cookie won't be set. Workaround
    //      the issue while this is not fixed inside WebKit.
    if (!soup_cookie_get_path (cookie))
        soup_cookie_set_path (cookie, "/");

    // Adding a cookie is an asynchronous operation, so spin up an
    // event loop until to block until the operation completes.
    g_autoptr(GMainLoop) loop = g_main_loop_new (NULL, FALSE);
    webkit_cookie_manager_add_cookie (cookie_manager,
                                      cookie,
                                      NULL,  // GCancellable
                                      (GAsyncReadyCallback) on_cookie_added,
                                      loop);
    g_main_loop_run (loop);
    return TRUE;

bad_format:
    g_set_error (error,
                 G_OPTION_ERROR,
                 G_OPTION_ERROR_BAD_VALUE,
                 "Invalid cookie specification '%s'",
                 value);
    return FALSE;
}


static gboolean
option_entry_parse_cookie_jar (const char          *option G_GNUC_UNUSED,
                               const char          *value,
                               WebKitCookieManager *cookie_manager,
                               GError             **error)
{
    if (strcmp (value, "help") == 0) {
        g_autoptr(GEnumClass) enum_class =
            g_type_class_ref (WEBKIT_TYPE_COOKIE_PERSISTENT_STORAGE);
        for (unsigned i = 0; i < enum_class->n_values; i++)
            g_print ("%s\n", enum_class->values[i].value_nick);
        exit (EXIT_SUCCESS);
        g_assert_not_reached ();
    }

    g_autofree char *cookie_jar_path = NULL;
    g_autofree char *format_name = g_strdup (value);
    char *path = strchr (format_name, ':');

    if (path) {
        *path++ = '\0';
        g_autoptr(GFile) cookie_jar = g_file_new_for_path (path);
        cookie_jar_path = g_file_get_path (cookie_jar);

        if (!g_file_is_native (cookie_jar)) {
            g_set_error (error,
                         G_OPTION_ERROR,
                         G_OPTION_ERROR_BAD_VALUE,
                         "Path '%s' is not local",
                         cookie_jar_path);
            return FALSE;
        }

        GFileType file_type =
            g_file_query_file_type (cookie_jar, G_FILE_QUERY_INFO_NONE, NULL);
        switch (file_type) {
            case G_FILE_TYPE_UNKNOWN:  // Does not exist yet, will be created.
            case G_FILE_TYPE_REGULAR:
                break;
            default:
                g_set_error (error,
                             G_OPTION_ERROR,
                             G_OPTION_ERROR_BAD_VALUE,
                             "Cannot use %s path '%s' for cookies",
                             cog_g_enum_get_nick (G_TYPE_FILE_TYPE, file_type),
                             cookie_jar_path);
                return FALSE;
        }
    }

    const GEnumValue *enum_value =
        cog_g_enum_get_value (WEBKIT_TYPE_COOKIE_PERSISTENT_STORAGE, format_name);
    if (!enum_value) {
        g_set_error (error,
                     G_OPTION_ERROR,
                     G_OPTION_ERROR_BAD_VALUE,
                     "Invalid cookie jar format '%s'",
                     value);
        return FALSE;
    }

    if (!cookie_jar_path) {
        g_autofree char *file_name = g_strconcat ("cookies.", format_name, NULL);
        WebKitWebContext *context =
            cog_launcher_get_web_context (cog_launcher_get_default ());
        WebKitWebsiteDataManager *data_manager =
            webkit_web_context_get_website_data_manager (context);
        cookie_jar_path =
            g_build_filename (webkit_website_data_manager_get_base_data_directory (data_manager),
                              file_name,
                              NULL);
    }

    webkit_cookie_manager_set_persistent_storage (cookie_manager,
                                                  cookie_jar_path,
                                                  enum_value->value);
    return TRUE;
}


static GOptionEntry s_cookies_options[] =
{
    {
        .long_name = "cookie-store",
        .arg = G_OPTION_ARG_CALLBACK,
        .arg_data = option_entry_parse_cookie_store,
        .description = "When to store cookies: always (default), never, nothirdparty.",
        .arg_description = "MODE",
    },
    {
        .long_name = "cookie-add",
        .arg = G_OPTION_ARG_CALLBACK,
        .arg_data = option_entry_parse_cookie_add,
        .description = "Pre-set a cookie, available flags: httponly, secure, session.",
        .arg_description = "DOMAIN:[FLAG,-FLAG,..]:CONTENTS",
    },
    {
        .long_name = "cookie-jar",
        .arg = G_OPTION_ARG_CALLBACK,
        .arg_data = option_entry_parse_cookie_jar,
        .description = "Enable persisting cookies to disk. Pass 'help' for a list of formats.",
        .arg_description = "FORMAT[:PATH]",
    },
    { NULL }
};


void
cog_launcher_add_web_cookies_option_entries (CogLauncher *launcher)
{
    g_return_if_fail (COG_IS_LAUNCHER (launcher));

    g_autoptr(GOptionGroup) option_group =
        g_option_group_new ("cookies",
                            "Options which control storage and bahviour of cookies.\n",
                            "Show options for cookies",
                            webkit_web_context_get_cookie_manager (launcher->web_context),
                            NULL);
    g_option_group_add_entries (option_group, s_cookies_options);
    g_application_add_option_group (G_APPLICATION (launcher),
                                    g_steal_pointer (&option_group));
}
