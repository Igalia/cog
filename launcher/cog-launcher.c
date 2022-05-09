/*
 * cog-launcher.c
 * Copyright (C) 2021 Igalia S.L.
 * Copyright (C) 2017-2018 Adrian Perez <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#include "cog-launcher.h"
#include <glib-unix.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

enum webprocess_fail_action {
    WEBPROCESS_FAIL_UNKNOWN = 0,
    WEBPROCESS_FAIL_ERROR_PAGE,
    WEBPROCESS_FAIL_EXIT,
    WEBPROCESS_FAIL_EXIT_OK,
    WEBPROCESS_FAIL_RESTART,
};

static struct {
    char *home_uri;
    union {
        char     *config_file;
        GKeyFile *key_file;
    };
    gboolean version;
    gboolean print_appid;
    gboolean doc_viewer;
    gdouble  scale_factor;
    gdouble  device_scale_factor;
    union {
        GStrv       dir_handlers;
        GHashTable *handler_map;
    };
    GStrv arguments;
    char *background_color;
    char *platform_params;
    union {
        char *platform_name;
    };
    union {
        char                    *filter_path;
        WebKitUserContentFilter *filter;
    };
    union {
        char                       *action_name;
        enum webprocess_fail_action action_id;
    } on_failure;
    char    *web_extensions_dir;
    gboolean ignore_tls_errors;
    gboolean enable_sandbox;
    gboolean automation;
} s_options = {
    .scale_factor = 1.0,
    .device_scale_factor = 1.0,
};

#if !GLIB_CHECK_VERSION(2, 56, 0)
typedef void (*GClearHandleFunc)(guint handle_id);

#    undef g_clear_handle_id
void
g_clear_handle_id(guint *tag_ptr, GClearHandleFunc clear_func)
{
    guint _handle_id;

    _handle_id = *tag_ptr;
    if (_handle_id > 0) {
        *tag_ptr = 0;
        if (clear_func != NULL)
            clear_func(_handle_id);
    }
}
#endif

#if !GLIB_CHECK_VERSION(2, 58, 0)
#    define G_SOURCE_FUNC(f) ((GSourceFunc) (void (*)(void))(f))
#endif

enum {
    PROP_0,
    PROP_AUTOMATED,
};

/**
 * CogLauncher:
 *
 * Main application object.
 *
 * Wraps a [class@CogShell] into a [class@Gio.Application], and provides
 * actions which can be remotely activated using the
 * `org.freedesktop.Application` D-Bus interface.
 */

struct _CogLauncher {
    GApplication parent;
    CogPlatform *platform;
    CogShell    *shell;
    gboolean     allow_all_requests;
    gboolean     automated;

    WebKitSettings           *web_settings;
    WebKitWebsiteDataManager *web_data_manager;

#if COG_HAVE_MEM_PRESSURE
    WebKitMemoryPressureSettings *web_mem_settings;
    WebKitMemoryPressureSettings *net_mem_settings;
#endif /* COG_HAVE_MEM_PRESSURE */

    guint sigint_source;
    guint sigterm_source;
};

G_DEFINE_TYPE(CogLauncher, cog_launcher, G_TYPE_APPLICATION)

static void
on_action_quit(G_GNUC_UNUSED GAction *action, G_GNUC_UNUSED GVariant *param, CogLauncher *launcher)
{
    g_application_quit(G_APPLICATION(launcher));
}

static void
on_action_prev(G_GNUC_UNUSED GAction *action, G_GNUC_UNUSED GVariant *param, CogLauncher *launcher)
{
    webkit_web_view_go_back(cog_shell_get_web_view(launcher->shell));
}

static void
on_action_next(G_GNUC_UNUSED GAction *action, G_GNUC_UNUSED GVariant *param, CogLauncher *launcher)
{
    webkit_web_view_go_forward(cog_shell_get_web_view(launcher->shell));
}

static void
on_action_reload(G_GNUC_UNUSED GAction *action, G_GNUC_UNUSED GVariant *param, CogLauncher *launcher)
{
    webkit_web_view_reload(cog_shell_get_web_view(launcher->shell));
}

static void
on_action_open(G_GNUC_UNUSED GAction *action, GVariant *param, CogLauncher *launcher)
{
    g_return_if_fail(g_variant_is_of_type(param, G_VARIANT_TYPE_STRING));
    webkit_web_view_load_uri(cog_shell_get_web_view(launcher->shell), g_variant_get_string(param, NULL));
}

static gboolean
on_signal_quit(CogLauncher *launcher)
{
    g_application_quit(G_APPLICATION(launcher));
    return G_SOURCE_CONTINUE;
}

static gboolean
on_permission_request(G_GNUC_UNUSED WebKitWebView *web_view, WebKitPermissionRequest *request, CogLauncher *launcher)
{
    if (launcher->allow_all_requests)
        webkit_permission_request_allow(request);
    else
        webkit_permission_request_deny(request);

    return TRUE;
}

static void
on_notify_web_view(CogShell *shell, GParamSpec *arg G_GNUC_UNUSED, CogLauncher *launcher)
{
    WebKitWebView *web_view = cog_shell_get_web_view(shell);

    g_signal_connect(web_view, "permission-request", G_CALLBACK(on_permission_request), launcher);
}

static void
cog_launcher_add_action(CogLauncher *launcher,
                        const char  *name,
                        void (*callback)(GAction *, GVariant *, CogLauncher *),
                        const GVariantType *param_type)
{
    g_assert(COG_IS_LAUNCHER(launcher));
    g_assert_nonnull(name);
    g_assert_nonnull(callback);

    GSimpleAction *action = g_simple_action_new(name, param_type);
    g_signal_connect(action, "activate", G_CALLBACK(callback), launcher);
    g_action_map_add_action(G_ACTION_MAP(launcher), G_ACTION(action));
}

static void
cog_launcher_open(GApplication *application, GFile **files, int n_files, const char *hint)
{
    g_assert(n_files);

    if (n_files > 1)
        g_warning("Requested opening %i files, opening only the first one", n_files);

    g_autofree char *uri = g_file_get_uri(files[0]);
    webkit_web_view_load_uri(cog_shell_get_web_view(COG_LAUNCHER(application)->shell), uri);
}

static gboolean
platform_setup(CogLauncher *self)
{
    /*
     * Here we resolve the CogPlatform we are going to use. A Cog platform
     * is dynamically loaded object that abstracts the specifics about how
     * a WebView's WPE backend is going to be constructed and rendered on
     * a given platform.
     */

    g_debug("%s: Platform name: %s", __func__, s_options.platform_name);

    g_autoptr(GError)      error = NULL;
    g_autoptr(CogPlatform) platform = cog_platform_new(s_options.platform_name, &error);
    if (!platform) {
        g_warning("Cannot create platform: %s", error->message);
        return FALSE;
    }
    g_clear_pointer(&s_options.platform_name, g_free);

    if (!cog_platform_setup(platform, self->shell, s_options.platform_params ?: "", &error)) {
        g_warning("Platform setup failed: %s", error->message);
        return FALSE;
    }

    self->platform = g_steal_pointer(&platform);

    g_debug("%s: Selected %s @ %p", __func__, G_OBJECT_TYPE_NAME(self->platform), self->platform);
    return TRUE;
}

static void *
on_web_view_create(WebKitWebView *web_view, WebKitNavigationAction *action)
{
    webkit_web_view_load_request(web_view, webkit_navigation_action_get_request(action));
    return NULL;
}

static WebKitWebView *
cog_launcher_create_view(CogLauncher *self, CogShell *shell)
{
    g_assert(shell == self->shell);

    WebKitWebContext *web_context = cog_shell_get_web_context(shell);

    if (s_options.doc_viewer) {
        webkit_web_context_set_cache_model(web_context, WEBKIT_CACHE_MODEL_DOCUMENT_VIEWER);
    }

    WebKitWebViewBackend *view_backend = NULL;

    // Try to load the platform plug-in specified in the command line.
    if (platform_setup(self)) {
        g_autoptr(GError) error = NULL;
        view_backend = cog_platform_get_view_backend(self->platform, NULL, &error);
        if (!view_backend) {
            g_assert(error);
            g_warning("Failed to get platform's view backend: %s", error->message);
        }
    }

    // If the platform plug-in failed, try the default WPE backend.
    if (!view_backend) {
        g_debug("Instantiating default WPE backend as fall-back.");
        view_backend = webkit_web_view_backend_new(wpe_view_backend_create(), NULL, NULL);
    }

    // At this point, either the platform plug-in or the default WPE backend
    // must have succeeded in providing a WebKitWebViewBackend* instance.
    if (!view_backend)
        g_error("Could not instantiate any WPE backend.");

    g_autoptr(WebKitWebView) web_view =
        g_object_new(WEBKIT_TYPE_WEB_VIEW, "settings", cog_shell_get_web_settings(shell), "web-context", web_context,
                     "zoom-level", s_options.scale_factor, "backend", view_backend, "is-controlled-by-automation",
                     cog_shell_is_automated(shell), NULL);

    if (s_options.filter) {
        WebKitUserContentManager *manager = webkit_web_view_get_user_content_manager(web_view);
        webkit_user_content_manager_add_filter(manager, s_options.filter);
        g_clear_pointer(&s_options.filter, webkit_user_content_filter_unref);
    }

    g_signal_connect(web_view, "create", G_CALLBACK(on_web_view_create), NULL);

    cog_platform_init_web_view(self->platform, web_view);
    g_autoptr(WebKitInputMethodContext) im_context = cog_platform_create_im_context(self->platform);
    webkit_web_view_set_input_method_context(web_view, im_context);

    if (s_options.background_color != NULL) {
        WebKitColor color;
        gboolean    has_valid_color = webkit_color_parse(&color, s_options.background_color);

        if (has_valid_color)
            webkit_web_view_set_background_color(web_view, &color);
        else
            g_error("'%s' doesn't represent a valid #RRGGBBAA or CSS color format.", s_options.background_color);
    }

    switch (s_options.on_failure.action_id) {
    case WEBPROCESS_FAIL_ERROR_PAGE:
        // Nothing else needed, the default error handler (connected
        // below) already implements displaying an error page.
        break;

    case WEBPROCESS_FAIL_EXIT:
        cog_web_view_connect_web_process_terminated_exit_handler(web_view, EXIT_FAILURE);
        break;

    case WEBPROCESS_FAIL_EXIT_OK:
        cog_web_view_connect_web_process_terminated_exit_handler(web_view, EXIT_SUCCESS);
        break;

    case WEBPROCESS_FAIL_RESTART:
        // TODO: Un-hardcode the 5 retries per second.
        cog_web_view_connect_web_process_terminated_restart_handler(web_view, 5, 1000);
        break;

    default:
        g_assert_not_reached();
    }

    cog_web_view_connect_default_progress_handlers(web_view);
    cog_web_view_connect_default_error_handlers(web_view);

    webkit_web_view_load_uri(web_view, s_options.home_uri);
    g_clear_pointer(&s_options.home_uri, g_free);

    return g_steal_pointer(&web_view);
}

static void
cog_launcher_startup(GApplication *application)
{
    G_APPLICATION_CLASS(cog_launcher_parent_class)->startup(application);

    /*
     * We have to manually call g_application_hold(), otherwise nothing will
     * prevent GApplication from shutting down immediately after startup.
     */
    g_application_hold(application);

    CogLauncher *self = COG_LAUNCHER(application);
    self->shell =
        g_object_new(COG_TYPE_SHELL, "name", g_get_prgname(), "automated", self->automated, "web-settings",
                     self->web_settings, "web-data-manager", self->web_data_manager,
#if COG_HAVE_MEM_PRESSURE
                     "web-memory-settings", self->web_mem_settings, "network-memory-settings", self->net_mem_settings,
#endif /* COG_HAVE_MEM_PRESSURE */
                     NULL);
    g_signal_connect_swapped(self->shell, "create-view", G_CALLBACK(cog_launcher_create_view), self);
    g_signal_connect(self->shell, "notify::web-view", G_CALLBACK(on_notify_web_view), self);

    if (s_options.web_extensions_dir)
        webkit_web_context_set_web_extensions_directory(cog_shell_get_web_context(self->shell),
                                                        s_options.web_extensions_dir);
    webkit_web_context_set_sandbox_enabled(cog_shell_get_web_context(self->shell), s_options.enable_sandbox);

    g_object_set(self->shell, "device-scale-factor", s_options.device_scale_factor, NULL);

    cog_shell_startup(self->shell);

    if (s_options.handler_map) {
        GHashTableIter i;
        void          *key, *value;
        g_hash_table_iter_init(&i, s_options.handler_map);
        while (g_hash_table_iter_next(&i, &key, &value)) {
            cog_shell_set_request_handler(self->shell, key, value);
            g_hash_table_iter_remove(&i);
        }
        g_clear_pointer(&s_options.handler_map, g_hash_table_destroy);
    }

    if (s_options.key_file) {
        g_autoptr(GKeyFile) key_file = g_steal_pointer(&s_options.key_file);
        g_object_set(self->shell, "config-file", key_file, NULL);
    }

#if WEBKIT_CHECK_VERSION(2, 32, 0)
    webkit_website_data_manager_set_tls_errors_policy(cog_launcher_get_web_data_manager(self),
                                                      s_options.ignore_tls_errors ? WEBKIT_TLS_ERRORS_POLICY_IGNORE
                                                                                  : WEBKIT_TLS_ERRORS_POLICY_FAIL);
#else
    webkit_web_context_set_tls_errors_policy(cog_shell_get_web_context(self->shell),
                                             s_options.ignore_tls_errors ? WEBKIT_TLS_ERRORS_POLICY_IGNORE
                                                                         : WEBKIT_TLS_ERRORS_POLICY_FAIL);
#endif
}

static void
cog_launcher_shutdown(GApplication *application)
{
    cog_shell_shutdown(cog_launcher_get_shell(COG_LAUNCHER(application)));

    G_APPLICATION_CLASS(cog_launcher_parent_class)->shutdown(application);
}

static void
cog_launcher_dispose(GObject *object)
{
    CogLauncher *launcher = COG_LAUNCHER(object);

    if (launcher->shell) {
        g_signal_handlers_disconnect_by_func(launcher->shell, G_CALLBACK(on_notify_web_view), NULL);
        g_signal_handlers_disconnect_by_func(launcher->shell, G_CALLBACK(cog_launcher_create_view), NULL);
        g_clear_object(&launcher->shell);
    }

    g_clear_object(&launcher->platform);

    g_clear_handle_id(&launcher->sigint_source, g_source_remove);
    g_clear_handle_id(&launcher->sigterm_source, g_source_remove);

    g_clear_object(&launcher->web_settings);

#if COG_HAVE_MEM_PRESSURE
    g_clear_pointer(&launcher->web_mem_settings, webkit_memory_pressure_settings_free);
    g_clear_pointer(&launcher->net_mem_settings, webkit_memory_pressure_settings_free);
#endif /* COG_HAVE_MEM_PRESSURE */

    G_OBJECT_CLASS(cog_launcher_parent_class)->dispose(object);
}

#if COG_DBUS_SYSTEM_BUS
static void
on_system_bus_acquired(GDBusConnection *connection, const char *name, void *userdata)
{
    g_autofree char *object_path =
        cog_appid_to_dbus_object_path(g_application_get_application_id(G_APPLICATION(userdata)));
    g_autoptr(GError) error = NULL;
    if (!g_dbus_connection_export_action_group(connection, object_path, G_ACTION_GROUP(userdata), &error))
        g_warning("Cannot expose remote control interface to system bus: %s", error->message);
}

static void
on_system_bus_name_acquired(G_GNUC_UNUSED GDBusConnection *connection, const char *name, G_GNUC_UNUSED void *userdata)
{
    g_message("Acquired D-Bus well-known name %s", name);
}

static void
on_system_bus_name_lost(GDBusConnection *connection, const char *name, G_GNUC_UNUSED void *userdata)
{
    if (connection) {
        g_message("Lost D-Bus well-known name %s", name);
    } else {
        g_message("Lost D-Bus connection to system bus");
    }
}
#endif // COG_DBUS_SYSTEM_BUS

#ifndef COG_DEFAULT_APPID
#    define COG_DEFAULT_APPID "com.igalia." COG_DEFAULT_APPNAME
#endif /* !COG_DEFAULT_APPID */

static gboolean
option_entry_parse_cookie_jar(const char *option G_GNUC_UNUSED,
                              const char        *value,
                              CogLauncher       *launcher,
                              GError           **error)
{
    if (strcmp(value, "help") == 0) {
        g_autoptr(GEnumClass) enum_class = g_type_class_ref(WEBKIT_TYPE_COOKIE_PERSISTENT_STORAGE);
        for (unsigned i = 0; i < enum_class->n_values; i++)
            g_print("%s\n", enum_class->values[i].value_nick);
        exit(EXIT_SUCCESS);
        g_assert_not_reached();
    }

    g_autofree char *cookie_jar_path = NULL;
    g_autofree char *format_name = g_strdup(value);
    char            *path = strchr(format_name, ':');

    if (path) {
        *path++ = '\0';
        g_autoptr(GFile) cookie_jar = g_file_new_for_path(path);
        cookie_jar_path = g_file_get_path(cookie_jar);

        if (!g_file_is_native(cookie_jar)) {
            g_set_error(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE, "Path '%s' is not local", cookie_jar_path);
            return FALSE;
        }

        GFileType file_type = g_file_query_file_type(cookie_jar, G_FILE_QUERY_INFO_NONE, NULL);
        switch (file_type) {
        case G_FILE_TYPE_UNKNOWN: // Does not exist yet, will be created.
        case G_FILE_TYPE_REGULAR:
            break;
        default:
            g_set_error(error,
                        G_OPTION_ERROR,
                        G_OPTION_ERROR_BAD_VALUE,
                        "Cannot use %s path '%s' for cookies",
                        cog_g_enum_get_nick(G_TYPE_FILE_TYPE, file_type),
                        cookie_jar_path);
            return FALSE;
        }
    }

    const GEnumValue *enum_value = cog_g_enum_get_value(WEBKIT_TYPE_COOKIE_PERSISTENT_STORAGE, format_name);
    if (!enum_value) {
        g_set_error(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE, "Invalid cookie jar format '%s'", value);
        return FALSE;
    }

    if (!cookie_jar_path) {
        g_autofree char *file_name = g_strconcat("cookies.", format_name, NULL);
        cookie_jar_path = g_build_filename(
            webkit_website_data_manager_get_base_data_directory(launcher->web_data_manager), file_name, NULL);
    }

    webkit_cookie_manager_set_persistent_storage(
        webkit_website_data_manager_get_cookie_manager(launcher->web_data_manager), cookie_jar_path, enum_value->value);
    return TRUE;
}

typedef struct {
    GMainLoop               *loop;
    WebKitCookieAcceptPolicy result;
} GetCookieAcceptPolicyData;

static void
on_got_cookie_accept_policy(WebKitCookieManager *manager, GAsyncResult *result, GetCookieAcceptPolicyData *data)
{
    data->result = webkit_cookie_manager_get_accept_policy_finish(manager, result, NULL);
    g_main_loop_quit(data->loop);
}

static WebKitCookieAcceptPolicy
cookie_manager_get_accept_policy(WebKitCookieManager *cookie_manager)
{
    g_autoptr(GMainLoop)      loop = g_main_loop_new(NULL, FALSE);
    GetCookieAcceptPolicyData data = {.loop = loop};
    webkit_cookie_manager_get_accept_policy(cookie_manager,
                                            NULL, // GCancellable
                                            (GAsyncReadyCallback) on_got_cookie_accept_policy,
                                            &data);
    g_main_loop_run(loop);
    return data.result;
}

static gboolean
option_entry_parse_cookie_store(const char *option G_GNUC_UNUSED,
                                const char        *value,
                                CogLauncher       *launcher,
                                GError           **error)
{
    WebKitCookieManager *cookie_manager = webkit_website_data_manager_get_cookie_manager(launcher->web_data_manager);

    if (strcmp(value, "help") == 0) {
        const WebKitCookieAcceptPolicy default_mode = cookie_manager_get_accept_policy(cookie_manager);
        g_autoptr(GEnumClass)          enum_class = g_type_class_ref(WEBKIT_TYPE_COOKIE_ACCEPT_POLICY);
        for (unsigned i = 0; i < enum_class->n_values; i++) {
            const char *format = (enum_class->values[i].value == default_mode) ? "%s (default)\n" : "%s\n";
            g_print(format, enum_class->values[i].value_nick);
        }
        exit(EXIT_SUCCESS);
        g_assert_not_reached();
    }

    const GEnumValue *enum_value = cog_g_enum_get_value(WEBKIT_TYPE_COOKIE_ACCEPT_POLICY, value);
    if (!enum_value) {
        g_set_error(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE, "Invalid cookie storing mode '%s'", value);
        return FALSE;
    }

    webkit_cookie_manager_set_accept_policy(cookie_manager, enum_value->value);
    return TRUE;
}

typedef void (*CookieFlagCallback)(SoupCookie *, gboolean);

static void
cookie_set_session(SoupCookie *cookie, gboolean session)
{
    if (session)
        soup_cookie_set_expires(cookie, NULL);
}

static inline CookieFlagCallback
option_entry_parse_cookie_add_get_flag_callback(const char *name)
{
    static const struct {
        const char        *name;
        CookieFlagCallback callback;
    } flag_map[] = {
        {"httponly", soup_cookie_set_http_only},
        {"secure", soup_cookie_set_secure},
        {"session", cookie_set_session},
    };

    for (unsigned i = 0; i < G_N_ELEMENTS(flag_map); i++)
        if (strcmp(name, flag_map[i].name) == 0)
            return flag_map[i].callback;

    return NULL;
}

static void
on_cookie_added(WebKitCookieManager *cookie_manager, GAsyncResult *result, GMainLoop *loop)
{
    g_autoptr(GError) error = NULL;
    if (!webkit_cookie_manager_add_cookie_finish(cookie_manager, result, &error)) {
        g_warning("Error setting cookie: %s", error->message);
    }
    g_main_loop_quit(loop);
}

static gboolean
option_entry_parse_cookie_add(const char *option G_GNUC_UNUSED,
                              const char        *value,
                              CogLauncher       *launcher,
                              GError **error     G_GNUC_UNUSED)
{
    g_autoptr(GMainLoop)  loop = NULL;
    g_autoptr(SoupCookie) cookie = NULL;
    g_autofree char      *domain = g_strdup(value);

    char *flagstr = strchr(domain, ':');
    if (!flagstr)
        goto bad_format;
    *flagstr++ = '\0';

    char *contents = strchr(flagstr, ':');
    if (!contents)
        goto bad_format;

    // The domain might include a port in the domain, in that
    // case skip forward to the next colon after the port number.
    if (g_ascii_isdigit(contents[1])) {
        if (!(contents = strchr(contents + 1, ':')))
            goto bad_format;
    }
    *contents++ = '\0';

    // The contents of the cookie cannot be empty.
    if (!contents[0])
        goto bad_format;

    cookie = soup_cookie_parse(contents, NULL);
    if (!cookie)
        goto bad_format;

    soup_cookie_set_domain(cookie, domain);

    // Go through the flags.
    if (flagstr && flagstr[0]) {
        g_auto(GStrv) flags = g_strsplit(flagstr, ",", -1);

        for (unsigned i = 0; flags[i] != NULL; i++) {
            // Skip the optional leading +/- signs.
            const char *flag = flags[i];
            gboolean    flag_value = flag[0] != '-';
            if (flag[0] == '+' || flag[0] == '-')
                flag++;

            const CookieFlagCallback flag_callback = option_entry_parse_cookie_add_get_flag_callback(flag);
            if (!flag_callback) {
                g_set_error(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE, "Invalid cookie flag '%s'", flag);
                return FALSE;
            }
            (*flag_callback)(cookie, flag_value);
        }
    }

    // XXX: If the cookie has no path defined, conversion to WebKit's
    //      internal format will fail and the WebProcess will spit ouy
    //      a critical error -- and the cookie won't be set. Workaround
    //      the issue while this is not fixed inside WebKit.
    if (!soup_cookie_get_path(cookie))
        soup_cookie_set_path(cookie, "/");

    // Adding a cookie is an asynchronous operation, so spin up an
    // event loop until to block until the operation completes.
    loop = g_main_loop_new(NULL, FALSE);
    webkit_cookie_manager_add_cookie(webkit_website_data_manager_get_cookie_manager(launcher->web_data_manager),
                                     cookie,
                                     NULL, // GCancellable
                                     (GAsyncReadyCallback) on_cookie_added,
                                     loop);
    g_main_loop_run(loop);
    return TRUE;

bad_format:
    g_set_error(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE, "Invalid cookie specification '%s'", value);
    return FALSE;
}

static GOptionEntry s_cookies_options[] = {
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
    {NULL}};

static void
cog_launcher_add_web_cookies_option_entries(CogLauncher *launcher)
{
    g_return_if_fail(COG_IS_LAUNCHER(launcher));

    g_autoptr(GOptionGroup) option_group =
        g_option_group_new("cookies",
                           "Options which control storage and bahviour of cookies.\n",
                           "Show options for cookies",
                           launcher,
                           NULL);
    g_option_group_add_entries(option_group, s_cookies_options);
    g_application_add_option_group(G_APPLICATION(launcher), g_steal_pointer(&option_group));
}

#if COG_HAVE_MEM_PRESSURE
static WebKitMemoryPressureSettings *
mem_settings_pick(CogLauncher *launcher, const char *option)
{
    if (strncmp(option, "--web-", 6) == 0)
        return launcher->web_mem_settings;
    if (strncmp(option, "--net-", 6) == 0)
        return launcher->net_mem_settings;

    g_assert_not_reached();
    return NULL;
}

static gboolean
option_entry_parse_mem_limit(const char *option, const char *value, CogLauncher *launcher, GError **error)
{
    errno = 0;
    char          *endp = NULL;
    const uint64_t v = g_ascii_strtoull(value, &endp, 0);
    if (errno || *endp != '\0' || v <= 0) {
        g_set_error(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE, "Invalid memory size value '%s'", value);
        return FALSE;
    }

    webkit_memory_pressure_settings_set_memory_limit(mem_settings_pick(launcher, option), v);
    return TRUE;
}

static gboolean
parse_mem_double(const char *value, double *r, gboolean up_to_one, GError **error)
{
    errno = 0;
    char        *endp = NULL;
    const double v = g_ascii_strtod(value, &endp);
    if (errno || *endp != '\0' || v <= 0.0 || (up_to_one && v >= 1.0)) {
        g_set_error(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE, "Invalid value '%s'", value);
        return FALSE;
    }
    *r = v;
    return TRUE;
}

static void
set_mem_double(CogLauncher *launcher, const char *option, double v)
{
    static const struct {
        const char *name;
        void (*set)(WebKitMemoryPressureSettings *, double);
    } settings[] = {
        {"check-interval", webkit_memory_pressure_settings_set_poll_interval},
        {"conservative-threshold", webkit_memory_pressure_settings_set_conservative_threshold},
        {"strict-threshold", webkit_memory_pressure_settings_set_strict_threshold},
        {"kill-threshold", webkit_memory_pressure_settings_set_kill_threshold},
    };

    for (unsigned i = 0; i < G_N_ELEMENTS(settings); i++) {
        if (strcmp(settings[i].name, option + 6) == 0) {
            settings[i].set(mem_settings_pick(launcher, option), v);
            return;
        }
    }

    g_assert_not_reached();
}

static gboolean
option_entry_parse_mem_double(const char *option, const char *value, CogLauncher *launcher, GError **error)
{
    double v;
    if (!parse_mem_double(value, &v, FALSE, error))
        return FALSE;

    set_mem_double(launcher, option, v);
    return TRUE;
}

static gboolean
option_entry_parse_mem_double_01(const char *option, const char *value, CogLauncher *launcher, GError **error)
{
    double v;
    if (!parse_mem_double(value, &v, TRUE, error))
        return FALSE;

    set_mem_double(launcher, option, v);
    return TRUE;
}

static void
cog_launcher_add_mem_pressure_option_entries(CogLauncher *self)
{
    static const GOptionEntry entries[] = {{
                                               .long_name = "web-mem-limit",
                                               .arg = G_OPTION_ARG_CALLBACK,
                                               .arg_data = option_entry_parse_mem_limit,
                                               .description = "Maximum amount of memory to use, in MiB.",
                                               .arg_description = "SIZE",
                                           },
                                           {
                                               .long_name = "web-check-interval",
                                               .arg = G_OPTION_ARG_CALLBACK,
                                               .arg_data = option_entry_parse_mem_double,
                                               .description = "Interval of time between memory usage checks.",
                                               .arg_description = "SECONDS",
                                           },
                                           {
                                               .long_name = "web-conservative-threshold",
                                               .arg = G_OPTION_ARG_CALLBACK,
                                               .arg_data = option_entry_parse_mem_double_01,
                                               .description = "Conservative threshold (default: 0.33).",
                                               .arg_description = "(0..1)",
                                           },
                                           {
                                               .long_name = "web-strict-threshold",
                                               .arg = G_OPTION_ARG_CALLBACK,
                                               .arg_data = option_entry_parse_mem_double_01,
                                               .description = "Strict threshold (default: 0.5).",
                                               .arg_description = "(0..1)",
                                           },
                                           {
                                               .long_name = "web-kill-threshold",
                                               .arg = G_OPTION_ARG_CALLBACK,
                                               .arg_data = option_entry_parse_mem_double,
                                               .description = "Kill threshold (default: 0).",
                                               .arg_description = "(0..",
                                           },
                                           {
                                               .long_name = "net-mem-limit",
                                               .arg = G_OPTION_ARG_CALLBACK,
                                               .arg_data = option_entry_parse_mem_limit,
                                               .description = "Maximum amount of memory to use, in MiB.",
                                               .arg_description = "SIZE",
                                           },
                                           {
                                               .long_name = "net-check-interval",
                                               .arg = G_OPTION_ARG_CALLBACK,
                                               .arg_data = option_entry_parse_mem_double,
                                               .description = "Interval of time between memory usage checks.",
                                               .arg_description = "SECONDS",
                                           },
                                           {
                                               .long_name = "net-conservative-threshold",
                                               .arg = G_OPTION_ARG_CALLBACK,
                                               .arg_data = option_entry_parse_mem_double_01,
                                               .description = "Conservative threshold (default: 0.33).",
                                               .arg_description = "(0..1)",
                                           },
                                           {
                                               .long_name = "net-strict-threshold",
                                               .arg = G_OPTION_ARG_CALLBACK,
                                               .arg_data = option_entry_parse_mem_double_01,
                                               .description = "Strict threshold (default: 0.5).",
                                               .arg_description = "(0..1)",
                                           },
                                           {
                                               .long_name = "net-kill-threshold",
                                               .arg = G_OPTION_ARG_CALLBACK,
                                               .arg_data = option_entry_parse_mem_double,
                                               .description = "Kill threshold (default: 0).",
                                               .arg_description = "(0..",
                                           },
                                           {NULL}};

    GOptionGroup *group =
        g_option_group_new("memory-limits",
                           "These options allow configuring WebKit's memory pressure handling mechanism.\n"
                           "\n"
                           "  In particular, a limit for the maximum amount of memory to use can be set,\n"
                           "  and thresholds relative to the limit which determine at which points memory\n"
                           "  will be reclaimed. The conservative threshold is typically lower when reached\n"
                           "  memory will be reclaimed; the strict threshold works in the same way but the\n"
                           "  process is more aggresive. The kill threshold configures when worker processes\n"
                           "  will be forcibly killed. Note that if there is no memory limit set, the other\n"
                           "  settings are ignored.\n",
                           "Options to configure memory usage limits",
                           self,
                           NULL);
    g_option_group_add_entries(group, entries);
    g_application_add_option_group(G_APPLICATION(self), group);
}
#endif /* COG_HAVE_MEM_PRESSURE */

static gboolean
option_entry_parse_permissions(const char *option G_GNUC_UNUSED,
                               const char        *value,
                               CogLauncher       *launcher,
                               GError           **error)
{

    if (g_strcmp0(value, "all") && g_strcmp0(value, "none")) {
        g_set_error(error,
                    G_OPTION_ERROR,
                    G_OPTION_ERROR_BAD_VALUE,
                    "Invalid permission value '%s' (allowed values: ['none', 'all'])",
                    value);
        return FALSE;
    }

    launcher->allow_all_requests = !g_strcmp0(value, "all");
    return TRUE;
}

static GOptionEntry s_permissions_options[] = {
    {
        .long_name = "set-permissions",
        .arg = G_OPTION_ARG_CALLBACK,
        .arg_data = option_entry_parse_permissions,
        .description = "Set permissions to access certain resources (default: 'none')",
        .arg_description = "[all | none]",
    },
    {NULL}};

static void
cog_launcher_add_web_permissions_option_entries(CogLauncher *launcher)
{
    g_return_if_fail(COG_IS_LAUNCHER(launcher));

    g_autoptr(GOptionGroup) option_group = g_option_group_new("permissions",
                                                              "Options which control permissions.\n",
                                                              "Show options for permission request",
                                                              launcher,
                                                              NULL);
    g_option_group_add_entries(option_group, s_permissions_options);
    g_application_add_option_group(G_APPLICATION(launcher), g_steal_pointer(&option_group));
}

static void
cog_launcher_add_web_settings_option_entries(CogLauncher *launcher)
{
    g_return_if_fail(COG_IS_LAUNCHER(launcher));

    g_autofree GOptionEntry *option_entries = cog_option_entries_from_class(G_OBJECT_GET_CLASS(launcher->web_settings));

    if (!option_entries) {
        g_critical("Could not deduce option entries for WebKitSettings."
                   " This should not happen, continuing but YMMV.");
        return;
    }

    g_autoptr(GOptionGroup) option_group = g_option_group_new(
        "websettings",
        "WebKitSettings options can be used to configure features exposed to the loaded Web content.\n"
        "\n"
        "  BOOL values are either 'true', '1', 'false', or '0'. Omitting the value implies '1'.\n"
        "  INTEGER values can be decimal, octal (prefix '0'), or hexadecimal (prefix '0x').\n"
        "  UNSIGNED values behave like INTEGER, but negative values are not accepted.\n"
        "  FLOAT values may optionally use decimal separators and scientific notation.\n"
        "  STRING values may need quoting when passed from the shell.\n",
        "Show WebKitSettings options",
        launcher->web_settings,
        NULL);
    g_option_group_add_entries(option_group, option_entries);
    g_application_add_option_group(G_APPLICATION(launcher), g_steal_pointer(&option_group));
}

static GOptionEntry s_cli_options[] = {
    {"version", '\0', 0, G_OPTION_ARG_NONE, &s_options.version, "Print version and exit", NULL},
    {"print-appid", '\0', 0, G_OPTION_ARG_NONE, &s_options.print_appid, "Print application ID and exit", NULL},
    {"scale", '\0', 0, G_OPTION_ARG_DOUBLE, &s_options.scale_factor,
     "Zoom/Scaling factor applied to Web content (default: 1.0, no scaling)", "FACTOR"},
    {"device-scale", '\0', 0, G_OPTION_ARG_DOUBLE, &s_options.device_scale_factor,
     "Output device scaling factor (default: 1.0, no scaling, 96 DPI)", "FACTOR"},
    {"doc-viewer", '\0', 0, G_OPTION_ARG_NONE, &s_options.doc_viewer,
     "Document viewer mode: optimizes for local loading of Web content. "
     "This reduces memory usage at the cost of reducing caching of "
     "resources loaded from the network.",
     NULL},
    {"dir-handler", 'd', 0, G_OPTION_ARG_STRING_ARRAY, &s_options.dir_handlers,
     "Add a URI scheme handler for a directory", "SCHEME:PATH"},
    {"webprocess-failure", '\0', 0, G_OPTION_ARG_STRING, &s_options.on_failure.action_name,
     "Action on WebProcess failures: error-page (default), exit, exit-ok, restart.", "ACTION"},
    {"config", 'C', 0, G_OPTION_ARG_FILENAME, &s_options.config_file, "Path to a configuration file", "PATH"},
    {"bg-color", 'b', 0, G_OPTION_ARG_STRING, &s_options.background_color,
     "Background color, as a CSS name or in #RRGGBBAA hex syntax (default: white)", "BG_COLOR"},
    {"platform", 'P', 0, G_OPTION_ARG_STRING, &s_options.platform_name, "Platform plug-in to use.", "NAME"},
    {"platform-params", 'O', 0, G_OPTION_ARG_STRING, &s_options.platform_params,
     "Comma separated list of platform parameters.", "PARAMS"},
    {"web-extensions-dir", '\0', 0, G_OPTION_ARG_STRING, &s_options.web_extensions_dir,
     "Load Web Extensions from given directory.", "PATH"},
    {"ignore-tls-errors", '\0', 0, G_OPTION_ARG_NONE, &s_options.ignore_tls_errors,
     "Ignore TLS errors (default: disabled).", NULL},
    {"content-filter", 'F', 0, G_OPTION_ARG_FILENAME, &s_options.filter_path,
     "Path to content filter JSON rule set (default: none).", "PATH"},
    {"enable-sandbox", 's', 0, G_OPTION_ARG_NONE, &s_options.enable_sandbox,
     "Enable WebProcess sandbox (default: disabled).", NULL},
    {"automation", '\0', 0, G_OPTION_ARG_NONE, &s_options.automation, "Enable automation mode (default: disabled).",
     NULL},
    {G_OPTION_REMAINING, '\0', 0, G_OPTION_ARG_FILENAME_ARRAY, &s_options.arguments, "", "[URL]"},
    {NULL}};

static void
cog_launcher_constructed(GObject *object)
{
    G_OBJECT_CLASS(cog_launcher_parent_class)->constructed(object);

    CogLauncher *launcher = COG_LAUNCHER(object);

    g_autofree char *data_dir = g_build_filename(g_get_user_data_dir(), g_get_prgname(), NULL);
    g_autofree char *cache_dir = g_build_filename(g_get_user_cache_dir(), g_get_prgname(), NULL);

    launcher->web_data_manager = g_object_new(WEBKIT_TYPE_WEBSITE_DATA_MANAGER, "is-ephemeral", launcher->automated,
                                              "base-data-directory", data_dir, "base-cache-directory", cache_dir, NULL);

    cog_launcher_add_action(launcher, "quit", on_action_quit, NULL);
    cog_launcher_add_action(launcher, "previous", on_action_prev, NULL);
    cog_launcher_add_action(launcher, "next", on_action_next, NULL);
    cog_launcher_add_action(launcher, "reload", on_action_reload, NULL);
    cog_launcher_add_action(launcher, "open", on_action_open, G_VARIANT_TYPE_STRING);

    g_application_add_main_option_entries(G_APPLICATION(object), s_cli_options);
    cog_launcher_add_web_settings_option_entries(launcher);
    cog_launcher_add_web_cookies_option_entries(launcher);
    cog_launcher_add_web_permissions_option_entries(launcher);

#if COG_HAVE_MEM_PRESSURE
    cog_launcher_add_mem_pressure_option_entries(launcher);
#endif /* COG_HAVE_MEM_PRESSURE */

    launcher->sigint_source = g_unix_signal_add(SIGINT, G_SOURCE_FUNC(on_signal_quit), launcher);
    launcher->sigterm_source = g_unix_signal_add(SIGTERM, G_SOURCE_FUNC(on_signal_quit), launcher);

#if COG_DBUS_SYSTEM_BUS
    g_bus_own_name(G_BUS_TYPE_SYSTEM,
                   COG_DEFAULT_APPID,
                   G_BUS_NAME_OWNER_FLAGS_NONE,
                   on_system_bus_acquired,
                   on_system_bus_name_acquired,
                   on_system_bus_name_lost,
                   launcher,
                   NULL);
#endif
}

static int
string_to_webprocess_fail_action(const char *action)
{
    static const struct {
        const char                 *action;
        enum webprocess_fail_action action_id;
    } action_map[] = {
        {"error-page", WEBPROCESS_FAIL_ERROR_PAGE},
        {"exit", WEBPROCESS_FAIL_EXIT},
        {"exit-ok", WEBPROCESS_FAIL_EXIT_OK},
        {"restart", WEBPROCESS_FAIL_RESTART},
    };

    if (!action) // Default.
        return WEBPROCESS_FAIL_ERROR_PAGE;

    for (unsigned i = 0; i < G_N_ELEMENTS(action_map); i++)
        if (strcmp(action, action_map[i].action) == 0)
            return action_map[i].action_id;

    return WEBPROCESS_FAIL_UNKNOWN;
}

static void
on_filter_saved(WebKitUserContentFilterStore *store, GAsyncResult *result, GMainLoop *loop)
{
    g_autoptr(GError) error = NULL;
    s_options.filter = webkit_user_content_filter_store_save_from_file_finish(store, result, &error);
    if (!s_options.filter)
        g_warning("Cannot compile filter: %s", error->message);
    g_main_loop_quit(loop);
}

static gboolean
load_settings(WebKitSettings *settings, GKeyFile *key_file, GError **error)
{
    if (g_key_file_has_group(key_file, "websettings")) {
        if (!cog_webkit_settings_apply_from_key_file(settings, key_file, "websettings", error)) {
            return FALSE;
        }
    }

    return TRUE;
}

static int
cog_launcher_handle_local_options(GApplication *application, GVariantDict *options)
{
    CogLauncher *launcher = COG_LAUNCHER(application);

    if (s_options.version) {
        g_print("%s (WPE WebKit %u.%u.%u)\n",
                COG_VERSION_STRING COG_VERSION_EXTRA,
                webkit_get_major_version(),
                webkit_get_minor_version(),
                webkit_get_micro_version());
        return EXIT_SUCCESS;
    }
    if (s_options.print_appid) {
        const char *appid = g_application_get_application_id(G_APPLICATION(launcher));
        if (appid)
            g_print("%s\n", appid);
        return EXIT_SUCCESS;
    }

    {
        enum webprocess_fail_action action_id = string_to_webprocess_fail_action(s_options.on_failure.action_name);
        if (action_id == WEBPROCESS_FAIL_UNKNOWN) {
            g_printerr("Invalid action name: '%s'\n", s_options.on_failure.action_name);
            return EXIT_FAILURE;
        }
        g_clear_pointer(&s_options.on_failure.action_name, g_free);
        s_options.on_failure.action_id = action_id;
    }

    const char *uri = NULL;
    if (cog_launcher_is_automated(launcher)) {
        uri = "about:blank";
    } else if (!s_options.arguments) {
        if (!(uri = g_getenv("COG_URL"))) {
#ifdef COG_DEFAULT_HOME_URI
            uri = COG_DEFAULT_HOME_URI;
#else
            g_printerr("%s: URL not passed in the command line, and COG_URL not set\n", g_get_prgname());
            return EXIT_FAILURE;
#endif // COG_DEFAULT_HOME_URI
        }
    } else if (g_strv_length(s_options.arguments) > 1) {
        g_printerr("%s: Cannot load more than one URL.\n", g_get_prgname());
        return EXIT_FAILURE;
    } else {
        uri = s_options.arguments[0];
    }

    g_autoptr(GError) error = NULL;
    g_autofree char  *utf8_uri = cog_uri_guess_from_user_input(uri, TRUE, &error);
    if (!utf8_uri) {
        g_printerr("%s: URI '%s' is invalid UTF-8: %s\n", g_get_prgname(), uri, error->message);
        return EXIT_FAILURE;
    }
    g_strfreev(s_options.arguments);
    s_options.arguments = NULL;

    /*
     * Validate the supplied local URI handler specification and check
     * whether the directory exists. Note that this creation of the
     * corresponding CogURIHandler objects is done at GApplication::startup.
     */
    g_autoptr(GHashTable) handler_map = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
    for (size_t i = 0; s_options.dir_handlers && s_options.dir_handlers[i]; i++) {
        char *colon = strchr(s_options.dir_handlers[i], ':');
        if (!colon) {
            g_printerr("%s: Invalid URI handler specification '%s'\n", g_get_prgname(), s_options.dir_handlers[i]);
            return EXIT_FAILURE;
        }

        if (s_options.dir_handlers[i] == colon - 1) {
            g_printerr("%s: No scheme specified for '%s' URI handler\n", g_get_prgname(), s_options.dir_handlers[i]);
            return EXIT_FAILURE;
        }

        if (colon[1] == '\0') {
            g_printerr("%s: Empty path specified for '%s' URI handler\n", g_get_prgname(), s_options.dir_handlers[i]);
            return EXIT_FAILURE;
        }

        g_autoptr(GFile) file = g_file_new_for_commandline_arg(colon + 1);

        g_autoptr(GError) error = NULL;
        if (!cog_directory_files_handler_is_suitable_path(file, &error)) {
            g_printerr("%s: %s\n", g_get_prgname(), error->message);
            return EXIT_FAILURE;
        }

        *colon = '\0'; /* NULL-terminate the URI scheme name. */
        g_hash_table_insert(handler_map, g_strdup(s_options.dir_handlers[i]), cog_directory_files_handler_new(file));
    }
    if (s_options.dir_handlers)
        g_strfreev(s_options.dir_handlers);
    s_options.handler_map = g_hash_table_size(handler_map) ? g_steal_pointer(&handler_map) : NULL;

    s_options.home_uri = g_steal_pointer(&utf8_uri);

    if (s_options.config_file) {
        g_autoptr(GFile) file = g_file_new_for_commandline_arg(s_options.config_file);
        g_autofree char *config_file_path = g_file_get_path(file);

        if (!g_file_query_exists(file, NULL)) {
            g_printerr("%s: File does not exist: %s\n", g_get_prgname(), config_file_path);
            return EXIT_FAILURE;
        }

        g_autoptr(GError)   error = NULL;
        g_autoptr(GKeyFile) key_file = g_key_file_new();
        if (!g_key_file_load_from_file(key_file, config_file_path, G_KEY_FILE_NONE, &error) ||
            !load_settings(cog_launcher_get_webkit_settings(launcher), key_file, &error)) {
            g_printerr("%s: Cannot load configuration file: %s\n", g_get_prgname(), error->message);
            return EXIT_FAILURE;
        }

        g_free(s_options.config_file);
        s_options.key_file = g_steal_pointer(&key_file);
    }

    if (s_options.filter_path) {
        WebKitWebsiteDataManager *data_manager = cog_launcher_get_web_data_manager(launcher);
        g_autofree char          *filters_path =
            g_build_filename(webkit_website_data_manager_get_base_cache_directory(data_manager), "filters", NULL);
        g_autoptr(WebKitUserContentFilterStore) store = webkit_user_content_filter_store_new(filters_path);

        g_autoptr(GFile) file = g_file_new_for_commandline_arg(s_options.filter_path);
        g_clear_pointer(&s_options.filter_path, g_free);

        g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
        webkit_user_content_filter_store_save_from_file(store,
                                                        "CogFilter",
                                                        file,
                                                        NULL,
                                                        (GAsyncReadyCallback) on_filter_saved,
                                                        loop);
        g_main_loop_run(loop);
    }

    return -1; /* Continue startup. */
}

static void
cog_launcher_set_property(GObject *object, guint propId, const GValue *value, GParamSpec *pspec)
{
    CogLauncher *launcher = COG_LAUNCHER(object);

    switch (propId) {
    case PROP_AUTOMATED:
        launcher->automated = g_value_get_boolean(value);
        break;
    default:
        break;
    }
}

static void
cog_launcher_class_init(CogLauncherClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = cog_launcher_dispose;
    object_class->constructed = cog_launcher_constructed;
    object_class->set_property = cog_launcher_set_property;

    GApplicationClass *application_class = G_APPLICATION_CLASS(klass);
    application_class->open = cog_launcher_open;
    application_class->startup = cog_launcher_startup;
    application_class->shutdown = cog_launcher_shutdown;
    application_class->handle_local_options = cog_launcher_handle_local_options;

    g_object_class_install_property(object_class,
                                    PROP_AUTOMATED,
                                    g_param_spec_boolean("automated",
                                                         "Automated",
                                                         "Whether this launcher is automated",
                                                         FALSE,
                                                         G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY));
}

static void
cog_launcher_init(CogLauncher *self)
{
    self->web_settings = g_object_ref_sink(webkit_settings_new());

#if COG_HAVE_MEM_PRESSURE
    self->web_mem_settings = webkit_memory_pressure_settings_new();
    self->net_mem_settings = webkit_memory_pressure_settings_new();
#endif /* COG_HAVE_MEM_PRESSURE */
}

CogLauncher *
cog_launcher_new(CogSessionType session_type)
{
    /* Global singleton */
    const GApplicationFlags app_flags =
#if GLIB_CHECK_VERSION(2, 48, 0)
        G_APPLICATION_CAN_OVERRIDE_APP_ID |
#endif // GLIB_CHECK_VERSION
        G_APPLICATION_HANDLES_OPEN;
    return g_object_new(COG_TYPE_LAUNCHER, "application-id", COG_DEFAULT_APPID, "flags", app_flags, "automated",
                        (session_type == COG_SESSION_AUTOMATED), NULL);
}

/**
 * cog_launcher_get_shell:
 *
 * Obtains the [class@Shell] instance managed by the launcher.
 *
 * Returns: (transfer none): Shell instance for the launcher.
 */
CogShell *
cog_launcher_get_shell(CogLauncher *launcher)
{
    g_return_val_if_fail(COG_IS_LAUNCHER(launcher), NULL);
    return launcher->shell;
}

/**
 * cog_launcher_is_automated:
 *
 * Whether this launcher was created in automated mode.
 *
 * Returns: TRUE if this session is automated, FALSE otherwise.
 */
gboolean
cog_launcher_is_automated(CogLauncher *launcher)
{
    g_return_val_if_fail(COG_IS_LAUNCHER(launcher), FALSE);
    return launcher->automated;
}

WebKitSettings *
cog_launcher_get_webkit_settings(CogLauncher *launcher)
{
    g_return_val_if_fail(COG_IS_LAUNCHER(launcher), NULL);
    return launcher->web_settings;
}

WebKitWebsiteDataManager *
cog_launcher_get_web_data_manager(CogLauncher *launcher)
{
    g_return_val_if_fail(COG_IS_LAUNCHER(launcher), NULL);
    return launcher->web_data_manager;
}
