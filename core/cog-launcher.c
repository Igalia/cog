/*
 * cog-launcher.c
 * Copyright (C) 2020 Igalia S.L.
 * Copyright (C) 2017-2018 Adrian Perez <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#include "cog-launcher.h"
#include "cog-shell.h"
#include "cog-request-handler.h"
#include "cog-webkit-utils.h"
#include "cog-utils.h"
#include <glib-unix.h>
#include <stdlib.h>
#include <string.h>

#if !GLIB_CHECK_VERSION(2, 56, 0)
typedef void (* GClearHandleFunc) (guint handle_id);

#undef g_clear_handle_id
void
g_clear_handle_id (guint            *tag_ptr,
                   GClearHandleFunc  clear_func)
{
  guint _handle_id;

  _handle_id = *tag_ptr;
  if (_handle_id > 0)
    {
      *tag_ptr = 0;
      if (clear_func != NULL)
        clear_func (_handle_id);
    }
}
#endif

#if !GLIB_CHECK_VERSION(2, 58, 0)
#define G_SOURCE_FUNC(f) ((GSourceFunc) (void (*)(void)) (f))
#endif

struct _CogLauncher {
    GApplication parent;
    CogShell    *shell;
    gboolean     allow_all_requests;

    guint        sigint_source;
    guint        sigterm_source;
};

G_DEFINE_TYPE (CogLauncher, cog_launcher, G_TYPE_APPLICATION)


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
    webkit_web_view_go_back (WEBKIT_WEB_VIEW (cog_shell_get_active_view (launcher->shell)));
}

static void
on_action_next (G_GNUC_UNUSED GAction  *action,
                G_GNUC_UNUSED GVariant *param,
                CogLauncher            *launcher)
{
    webkit_web_view_go_forward (WEBKIT_WEB_VIEW (cog_shell_get_active_view (launcher->shell)));
}

static void
on_action_reload (G_GNUC_UNUSED GAction  *action,
                  G_GNUC_UNUSED GVariant *param,
                  CogLauncher            *launcher)
{
    webkit_web_view_reload (WEBKIT_WEB_VIEW (cog_shell_get_active_view (launcher->shell)));
}

static void
on_action_open (G_GNUC_UNUSED GAction *action,
                GVariant              *param,
                CogLauncher           *launcher)
{
    g_return_if_fail (g_variant_is_of_type (param, G_VARIANT_TYPE_STRING));
    webkit_web_view_load_uri (WEBKIT_WEB_VIEW (cog_shell_get_active_view (launcher->shell)),
                              g_variant_get_string (param, NULL));
}

static void
on_action_add (G_GNUC_UNUSED GAction *action,
               GVariant              *param,
               CogLauncher           *launcher)
{
    g_return_if_fail (g_variant_is_of_type (param, G_VARIANT_TYPE_STRING));

    CogView* view = cog_shell_create_view (launcher->shell,
                                           g_variant_get_string (param, NULL), NULL);
    cog_shell_add_view (launcher->shell, view);
    cog_shell_set_active_view (launcher->shell, view);
}

static void
on_action_remove (G_GNUC_UNUSED GAction *action,
                  GVariant              *param,
                  CogLauncher           *launcher)
{
    g_return_if_fail (g_variant_is_of_type (param, G_VARIANT_TYPE_STRING));
}

static void
on_action_present (G_GNUC_UNUSED GAction *action,
                   GVariant              *param,
                   CogLauncher           *launcher)
{
    g_return_if_fail (g_variant_is_of_type (param, G_VARIANT_TYPE_STRING));

    cog_shell_set_active_view (launcher->shell,
                               cog_shell_get_view (launcher->shell,
                                                   g_variant_get_string (param, NULL)));
}

static gboolean
on_signal_quit (CogLauncher *launcher)
{
    g_application_quit (G_APPLICATION (launcher));
    return G_SOURCE_CONTINUE;
}

static gboolean
on_permission_request (G_GNUC_UNUSED WebKitWebView *web_view,
                       WebKitPermissionRequest *request,
                       CogLauncher *launcher)
{
    if (launcher->allow_all_requests)
        webkit_permission_request_allow (request);
    else
        webkit_permission_request_deny (request);

    return TRUE;
}

static void
on_notify_web_view (CogShell *shell, GParamSpec * arg G_GNUC_UNUSED, CogLauncher *launcher)
{
    // WebKitWebView* web_view = cog_shell_get_web_view (shell);

    // g_signal_connect (web_view, "permission-request", G_CALLBACK (on_permission_request), launcher);
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


static void
cog_launcher_open (GApplication *application,
                   GFile       **files,
                   int           n_files,
                   const char   *hint)
{
    g_assert (n_files);

    if (n_files > 1)
        g_warning ("Requested opening %i files, opening only the first one", n_files);

    g_autofree char *uri = g_file_get_uri (files[0]);
    webkit_web_view_load_uri (WEBKIT_WEB_VIEW (cog_shell_get_active_view (COG_LAUNCHER (application)->shell)),
                              uri);
}


static void
cog_launcher_startup (GApplication *application)
{
    G_APPLICATION_CLASS (cog_launcher_parent_class)->startup (application);

    /*
     * Ensure that the application is kept active instead of exiting
     * immediately after startup.
     */
    g_application_hold (application);
}


static void
cog_launcher_dispose (GObject *object)
{
    CogLauncher *launcher = COG_LAUNCHER (object);

    if (launcher->shell) {
        g_signal_handlers_disconnect_by_func (launcher->shell, G_CALLBACK (on_notify_web_view), NULL);
        g_clear_object (&launcher->shell);
    }

    g_clear_handle_id (&launcher->sigint_source, g_source_remove);
    g_clear_handle_id (&launcher->sigterm_source, g_source_remove);

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
# define COG_DEFAULT_APPID "com.igalia." COG_DEFAULT_APPNAME
#endif


static void
cog_launcher_constructed (GObject *object)
{
    G_OBJECT_CLASS (cog_launcher_parent_class)->constructed (object);

    CogLauncher *launcher = COG_LAUNCHER (object);

    launcher->shell = g_object_ref_sink (cog_shell_new (g_get_prgname ()));

    // Created the default view
    CogView* view = cog_shell_create_view (launcher->shell, "default", NULL);
    cog_shell_add_view (launcher->shell, view);
    cog_shell_set_active_view (launcher->shell, view);

    g_signal_connect (launcher->shell, "notify::web-view", G_CALLBACK (on_notify_web_view), launcher);

    cog_launcher_add_action (launcher, "quit", on_action_quit, NULL);
    cog_launcher_add_action (launcher, "previous", on_action_prev, NULL);
    cog_launcher_add_action (launcher, "next", on_action_next, NULL);
    cog_launcher_add_action (launcher, "reload", on_action_reload, NULL);
    cog_launcher_add_action (launcher, "open", on_action_open, G_VARIANT_TYPE_STRING);
    cog_launcher_add_action (launcher, "add", on_action_add, G_VARIANT_TYPE_STRING);
    cog_launcher_add_action (launcher, "remove", on_action_remove, G_VARIANT_TYPE_STRING);
    cog_launcher_add_action (launcher, "present", on_action_present, G_VARIANT_TYPE_STRING);

    launcher->sigint_source = g_unix_signal_add (SIGINT,
                                                 G_SOURCE_FUNC (on_signal_quit),
                                                 launcher);
    launcher->sigterm_source = g_unix_signal_add (SIGTERM,
                                                  G_SOURCE_FUNC (on_signal_quit),
                                                  launcher);

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

    GApplicationClass *application_class = G_APPLICATION_CLASS (klass);
    application_class->open = cog_launcher_open;
    application_class->startup = cog_launcher_startup;
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


CogShell*
cog_launcher_get_shell (CogLauncher *launcher)
{
    g_return_val_if_fail (COG_IS_LAUNCHER (launcher), NULL);
    return launcher->shell;
}


void
cog_launcher_add_web_settings_option_entries (CogLauncher *launcher)
{
    g_return_if_fail (COG_IS_LAUNCHER (launcher));

#if 0
    g_autofree GOptionEntry *option_entries =
        cog_option_entries_from_class (G_OBJECT_GET_CLASS (cog_shell_get_web_settings (launcher->shell)));
    if (!option_entries) {
        g_critical ("Could not deduce option entries for WebKitSettings."
                    " This should not happen, continuing but YMMV.");
        return;
    }

    g_autoptr(GOptionGroup) option_group =
        g_option_group_new ("websettings",
                            "WebKitSettings options can be used to configure features exposed to the loaded Web content.\n"
                            "\n"
                            "  BOOL values are either 'true', '1', 'false', or '0'. Omitting the value implies '1'.\n"
                            "  INTEGER values can be decimal, octal (prefix '0'), or hexadecimal (prefix '0x').\n"
                            "  UNSIGNED values behave like INTEGER, but negative values are not accepted.\n"
                            "  FLOAT values may optionally use decimal separators and scientific notation.\n"
                            "  STRING values may need quoting when passed from the shell.\n",
                            "Show WebKitSettings options",
                            cog_shell_get_web_settings (launcher->shell),
                            NULL);
    g_option_group_add_entries (option_group, option_entries);
    g_application_add_option_group (G_APPLICATION (launcher),
                                    g_steal_pointer (&option_group));
#endif
}


typedef struct {
    GMainLoop               *loop;
    WebKitCookieAcceptPolicy result;
} GetCookieAcceptPolicyData;


static void
on_got_cookie_accept_policy (WebKitCookieManager       *manager,
                             GAsyncResult              *result,
                             GetCookieAcceptPolicyData *data)
{
    data->result =
        webkit_cookie_manager_get_accept_policy_finish (manager, result, NULL);
    g_main_loop_quit (data->loop);
}


static WebKitCookieAcceptPolicy
cookie_manager_get_accept_policy (WebKitCookieManager *cookie_manager)
{
    g_autoptr(GMainLoop) loop = g_main_loop_new (NULL, FALSE);
    GetCookieAcceptPolicyData data = { .loop = loop };
    webkit_cookie_manager_get_accept_policy (cookie_manager,
                                             NULL,  // GCancellable
                                             (GAsyncReadyCallback) on_got_cookie_accept_policy,
                                             &data);
    g_main_loop_run (loop);
    return data.result;
}


static gboolean
option_entry_parse_cookie_store (const char          *option G_GNUC_UNUSED,
                                 const char          *value,
                                 WebKitCookieManager *cookie_manager,
                                 GError             **error)
{
    if (strcmp (value, "help") == 0) {
        const WebKitCookieAcceptPolicy default_mode =
            cookie_manager_get_accept_policy (cookie_manager);
        g_autoptr(GEnumClass) enum_class =
            g_type_class_ref (WEBKIT_TYPE_COOKIE_ACCEPT_POLICY);
        for (unsigned i = 0; i < enum_class->n_values; i++) {
            const char *format = (enum_class->values[i].value == default_mode)
                ? "%s (default)\n"
                : "%s\n";
            g_print (format, enum_class->values[i].value_nick);
        }
        exit (EXIT_SUCCESS);
        g_assert_not_reached ();
    }

    const GEnumValue *enum_value =
        cog_g_enum_get_value (WEBKIT_TYPE_COOKIE_ACCEPT_POLICY, value);
    if (!enum_value) {
        g_set_error (error,
                     G_OPTION_ERROR,
                     G_OPTION_ERROR_BAD_VALUE,
                     "Invalid cookie storing mode '%s'",
                     value);
        return FALSE;
    }

    webkit_cookie_manager_set_accept_policy (cookie_manager, enum_value->value);
    return TRUE;
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
    g_autoptr(GMainLoop) loop = NULL;
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
    loop = g_main_loop_new (NULL, FALSE);
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

#if 0
    if (!cookie_jar_path) {
        g_autofree char *file_name = g_strconcat ("cookies.", format_name, NULL);
        WebKitWebContext *context =
            cog_shell_get_web_context (cog_launcher_get_default ()->shell);
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
#endif
    return TRUE;
}


static GOptionEntry s_cookies_options[] =
{
    {
        .long_name = "cookie-store",
        .arg = G_OPTION_ARG_CALLBACK,
        .arg_data = option_entry_parse_cookie_store,
        .description = "How to store cookies. Pass 'help' for a list of modes.",
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
                            // webkit_web_context_get_cookie_manager (cog_shell_get_web_context (launcher->shell)),
                            NULL,
                            NULL);
    g_option_group_add_entries (option_group, s_cookies_options);
    g_application_add_option_group (G_APPLICATION (launcher),
                                    g_steal_pointer (&option_group));
}

static gboolean
option_entry_parse_permissions (const char          *option G_GNUC_UNUSED,
                                const char          *value,
                                CogLauncher         *launcher,
                                GError             **error)
{

    if (g_strcmp0 (value, "all") && g_strcmp0 (value, "none")) {
        g_set_error (error,
                     G_OPTION_ERROR,
                     G_OPTION_ERROR_BAD_VALUE,
                     "Invalid permission value '%s' (allowed values: ['none', 'all'])",
                     value);
        return FALSE;
    }

    launcher->allow_all_requests = !g_strcmp0 (value, "all");
    return TRUE;
}

static GOptionEntry s_permissions_options[] =
{
    {
        .long_name = "set-permissions",
        .arg = G_OPTION_ARG_CALLBACK,
        .arg_data = option_entry_parse_permissions,
        .description = "Set permissions to access certain resources (default: 'none')",
        .arg_description = "[all | none]",
    },
    { NULL }
};

void
cog_launcher_add_web_permissions_option_entries (CogLauncher *launcher)
{
    g_return_if_fail (COG_IS_LAUNCHER (launcher));

    g_autoptr(GOptionGroup) option_group =
        g_option_group_new ("permissions",
                            "Options which control permissions.\n",
                            "Show options for permission request",
                            launcher,
                            NULL);
    g_option_group_add_entries (option_group, s_permissions_options);
    g_application_add_option_group (G_APPLICATION (launcher),
                                    g_steal_pointer (&option_group));
}
