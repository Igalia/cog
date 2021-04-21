/*
 * cog.c
 * Copyright (C) 2021 Igalia S.L.
 * Copyright (C) 2018 Eduardo Lima <elima@igalia.com>
 * Copyright (C) 2017-2018 Adrian Perez <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "core/cog.h"

enum webprocess_fail_action {
    WEBPROCESS_FAIL_UNKNOWN = 0,
    WEBPROCESS_FAIL_ERROR_PAGE,
    WEBPROCESS_FAIL_EXIT,
    WEBPROCESS_FAIL_EXIT_OK,
    WEBPROCESS_FAIL_RESTART,
};


static struct {
    char    *home_uri;
    char    *config_file;
    gboolean version;
    gboolean print_appid;
    gboolean doc_viewer;
    gdouble  scale_factor;
    gdouble  device_scale_factor;
    GStrv    dir_handlers;
    GStrv    arguments;
    char    *background_color;
    union {
        char *platform_name;
        CogPlatform *platform;
    };
    union {
        char *action_name;
        enum webprocess_fail_action action_id;
    } on_failure;
    char *web_extensions_dir;
    gboolean ignore_tls_errors;
    char *platform_options;
} s_options = {
    .scale_factor = 1.0,
    .device_scale_factor = 1.0,
};


static GOptionEntry s_cli_options[] =
{
    { "version", '\0', 0, G_OPTION_ARG_NONE, &s_options.version,
        "Print version and exit",
        NULL },
    { "print-appid", '\0', 0, G_OPTION_ARG_NONE, &s_options.print_appid,
        "Print application ID and exit",
        NULL },
    { "scale", '\0', 0, G_OPTION_ARG_DOUBLE, &s_options.scale_factor,
        "Zoom/Scaling factor applied to Web content (default: 1.0, no scaling)",
        "FACTOR" },
    { "device-scale", '\0', 0, G_OPTION_ARG_DOUBLE, &s_options.device_scale_factor,
        "Output device scaling factor (default: 1.0, no scaling, 96 DPI)",
        "FACTOR" },
    { "doc-viewer", '\0', 0, G_OPTION_ARG_NONE, &s_options.doc_viewer,
        "Document viewer mode: optimizes for local loading of Web content. "
        "This reduces memory usage at the cost of reducing caching of "
        "resources loaded from the network.",
        NULL },
    { "dir-handler", 'd', 0, G_OPTION_ARG_STRING_ARRAY, &s_options.dir_handlers,
        "Add a URI scheme handler for a directory",
        "SCHEME:PATH" },
    { "webprocess-failure", '\0', 0, G_OPTION_ARG_STRING,
        &s_options.on_failure.action_name,
        "Action on WebProcess failures: error-page (default), exit, exit-ok, restart.",
        "ACTION" },
    { "config", 'C', 0, G_OPTION_ARG_FILENAME, &s_options.config_file,
        "Path to a configuration file",
        "PATH" },
    { "bg-color", 'b', 0, G_OPTION_ARG_STRING, &s_options.background_color,
        "Background color, as a CSS name or in #RRGGBBAA hex syntax (default: white)",
        "BG_COLOR" },
    { "platform", 'P', 0, G_OPTION_ARG_STRING, &s_options.platform_name,
        "Platform plug-in to use.",
        "NAME" },
    { "platform-options", 'O', 0, G_OPTION_ARG_STRING, &s_options.platform_options,
        "custom options to pass to the platform shared object",
        "OPT,OPT=VALUE" },
    { "web-extensions-dir", '\0', 0, G_OPTION_ARG_STRING, &s_options.web_extensions_dir,
      "Load Web Extensions from given directory.",
      "PATH"},
    { "ignore-tls-errors", '\0', 0, G_OPTION_ARG_NONE, &s_options.ignore_tls_errors,
        "Ignore TLS errors (default: disabled).", NULL },
    { G_OPTION_REMAINING, '\0', 0, G_OPTION_ARG_FILENAME_ARRAY, &s_options.arguments,
        "", "[URL]" },
    { NULL }
};


static gboolean
load_settings (CogShell *shell, GKeyFile *key_file, GError **error)
{
    if (g_key_file_has_group (key_file, "websettings")) {
        WebKitSettings *settings = cog_shell_get_web_settings (shell);
        if (!cog_webkit_settings_apply_from_key_file (settings,
                                                      key_file,
                                                      "websettings",
                                                      error)) {
            return FALSE;
        }
    }

    return TRUE;
}


static int
string_to_webprocess_fail_action (const char *action)
{
    static const struct {
        const char *action;
        enum webprocess_fail_action action_id;
    } action_map[] = {
        { "error-page", WEBPROCESS_FAIL_ERROR_PAGE },
        { "exit",       WEBPROCESS_FAIL_EXIT       },
        { "exit-ok",    WEBPROCESS_FAIL_EXIT_OK    },
        { "restart",    WEBPROCESS_FAIL_RESTART    },
    };

    if (!action)  // Default.
        return WEBPROCESS_FAIL_ERROR_PAGE;

    for (unsigned i = 0; i < G_N_ELEMENTS (action_map); i++)
        if (strcmp (action, action_map[i].action) == 0)
            return action_map[i].action_id;

    return WEBPROCESS_FAIL_UNKNOWN;
}


static int
on_handle_local_options (GApplication *application,
                         GVariantDict *options,
                         void         *user_data)
{
    if (s_options.version) {
        g_print ("%s (WPE WebKit %u.%u.%u)\n",
                 COG_VERSION_STRING COG_VERSION_EXTRA,
                 webkit_get_major_version (),
                 webkit_get_minor_version (),
                 webkit_get_micro_version ());
        return EXIT_SUCCESS;
    }
    if (s_options.print_appid) {
        const char *appid = g_application_get_application_id (application);
        if (appid) g_print ("%s\n", appid);
        return EXIT_SUCCESS;
    }

    {
        enum webprocess_fail_action action_id =
                string_to_webprocess_fail_action (s_options.on_failure.action_name);
        if (action_id == WEBPROCESS_FAIL_UNKNOWN) {
            g_printerr ("Invalid action name: '%s'\n",
                        s_options.on_failure.action_name);
            return EXIT_FAILURE;
        }
        g_clear_pointer (&s_options.on_failure.action_name, g_free);
        s_options.on_failure.action_id = action_id;
    }

    const char *uri = NULL;
    if (!s_options.arguments) {
        if (!(uri = g_getenv ("COG_URL"))) {
#ifdef COG_DEFAULT_HOME_URI
            uri = COG_DEFAULT_HOME_URI;
#else
            g_printerr ("%s: URL not passed in the command line, and COG_URL not set\n", g_get_prgname ());
            return EXIT_FAILURE;
#endif // COG_DEFAULT_HOME_URI
        }
    } else if (g_strv_length (s_options.arguments) > 1) {
        g_printerr ("%s: Cannot load more than one URL.\n", g_get_prgname ());
        return EXIT_FAILURE;
    } else {
        uri = s_options.arguments[0];
    }

    g_autoptr(GError) error = NULL;
    g_autofree char *utf8_uri = cog_uri_guess_from_user_input (uri, TRUE, &error);
    if (!utf8_uri) {
        g_printerr ("%s: URI '%s' is invalid UTF-8: %s\n",
                    g_get_prgname (), uri, error->message);
        return EXIT_FAILURE;
    }
    g_strfreev (s_options.arguments);
    s_options.arguments = NULL;

    /*
     * Validate the supplied local URI handler specification and check
     * whether the directory exists. Note that this creation of the
     * corresponding CogURIHandler objects is done at GApplication::startup.
     */
    g_autoptr(CogShell) shell = cog_launcher_get_shell (COG_LAUNCHER (application));
    for (size_t i = 0; s_options.dir_handlers && s_options.dir_handlers[i]; i++) {
        char *colon = strchr (s_options.dir_handlers[i], ':');
        if (!colon) {
            g_printerr ("%s: Invalid URI handler specification '%s'\n",
                        g_get_prgname (), s_options.dir_handlers[i]);
            return EXIT_FAILURE;
        }

        if (s_options.dir_handlers[i] == colon - 1) {
            g_printerr ("%s: No scheme specified for '%s' URI handler\n",
                        g_get_prgname (), s_options.dir_handlers[i]);
            return EXIT_FAILURE;
        }

        if (colon[1] == '\0') {
            g_printerr ("%s: Empty path specified for '%s' URI handler\n",
                        g_get_prgname (), s_options.dir_handlers[i]);
            return EXIT_FAILURE;
        }

        g_autoptr(GFile) file = g_file_new_for_commandline_arg (colon + 1);

        g_autoptr(GError) error = NULL;
        if (!cog_directory_files_handler_is_suitable_path (file, &error)) {
            g_printerr ("%s: %s\n", g_get_prgname (), error->message);
            return EXIT_FAILURE;
        }

        *colon = '\0';  /* NULL-terminate the URI scheme name. */
        g_autoptr(CogRequestHandler) handler = cog_directory_files_handler_new (file);
        cog_shell_set_request_handler (shell, s_options.dir_handlers[i], handler);
    }

    s_options.home_uri = g_steal_pointer (&utf8_uri);

    if (s_options.config_file) {
        g_autoptr(GFile) file =
            g_file_new_for_commandline_arg (s_options.config_file);
        g_autofree char *config_file_path = g_file_get_path (file);

        if (!g_file_query_exists (file, NULL)) {
            g_printerr ("%s: File does not exist: %s\n",
                        g_get_prgname (), config_file_path);
            return EXIT_FAILURE;
        }

        g_autoptr(GError) error = NULL;
        g_autoptr(GKeyFile) key_file = g_key_file_new ();
        if (!g_key_file_load_from_file (key_file,
                                        config_file_path,
                                        G_KEY_FILE_NONE,
                                        &error) ||
            !load_settings (shell, key_file, &error))
        {
            g_printerr ("%s: Cannot load configuration file: %s\n",
                        g_get_prgname (), error->message);
            return EXIT_FAILURE;
        }

        g_object_set (shell, "config-file", g_key_file_ref (key_file), NULL);
    }

    g_object_set (shell, "device-scale-factor", s_options.device_scale_factor, NULL);

    if (s_options.web_extensions_dir != NULL) {
        webkit_web_context_set_web_extensions_directory (cog_shell_get_web_context (shell),
                                                         s_options.web_extensions_dir);
    }

    webkit_web_context_set_tls_errors_policy (cog_shell_get_web_context (shell),
                                              s_options.ignore_tls_errors
                                              ? WEBKIT_TLS_ERRORS_POLICY_IGNORE
                                              : WEBKIT_TLS_ERRORS_POLICY_FAIL);

    return -1;  /* Continue startup. */
}


static gboolean
platform_setup (CogShell *shell)
{
    /*
     * Here we resolve the CogPlatform we are going to use. A Cog platform
     * is dynamically loaded object that abstracts the specifics about how
     * a WebView's WPE backend is going to be constructed and rendered on
     * a given platform.
     */

    g_debug ("%s: Platform name: %s", __func__, s_options.platform_name);

    if (!s_options.platform_name)
        return FALSE;

    g_autofree char *platform_soname =
        g_strdup_printf ("libcogplatform-%s.so", s_options.platform_name);
    g_clear_pointer (&s_options.platform_name, g_free);

    g_debug ("%s: Platform plugin: %s", __func__, platform_soname);

    g_autoptr(CogPlatform) platform = cog_platform_new ();
    if (!cog_platform_try_load (platform, platform_soname)) {
        g_warning ("Could not load: %s (possible cause: %s).\n",
                   platform_soname, strerror (errno));
        return FALSE;
    }

    g_autoptr(GError) error = NULL;
    if (!cog_platform_setup (platform, shell, s_options.platform_options, &error)) {
        g_warning ("Platform setup failed: %s", error->message);
        return FALSE;
    }

    s_options.platform = g_steal_pointer (&platform);

    g_debug ("%s: Platform = %p", __func__, s_options.platform);
    return TRUE;
}


static void
on_shutdown (CogLauncher *launcher G_GNUC_UNUSED, void *user_data G_GNUC_UNUSED)
{
    g_debug ("%s: Platform = %p", __func__, s_options.platform);

    if (s_options.platform) {
        cog_platform_teardown (s_options.platform);
        g_clear_pointer (&s_options.platform, cog_platform_free);
        g_debug ("%s: Platform teardown completed.", __func__);
    }
}

static void*
on_web_view_create (WebKitWebView          *web_view,
                    WebKitNavigationAction *action)
{
    webkit_web_view_load_request (web_view, webkit_navigation_action_get_request (action));
    return NULL;
}

static WebKitWebView*
on_create_view (CogShell *shell, void *user_data G_GNUC_UNUSED)
{
    WebKitWebContext *web_context = cog_shell_get_web_context (shell);

    if (s_options.doc_viewer) {
        webkit_web_context_set_cache_model (web_context,
                                            WEBKIT_CACHE_MODEL_DOCUMENT_VIEWER);
    }

    WebKitWebViewBackend *view_backend = NULL;

    // Try to load the platform plug-in specified in the command line.
    if (platform_setup (shell)) {
        g_autoptr(GError) error = NULL;
        view_backend = cog_platform_get_view_backend (s_options.platform, NULL, &error);
        if (!view_backend) {
            g_assert (error);
            g_warning ("Failed to get platform's view backend: %s", error->message);
        }
    }

    // If the platform plug-in failed, try the default WPE backend.
    if (!view_backend) {
        g_debug ("Instantiating default WPE backend as fall-back.");
        view_backend = webkit_web_view_backend_new (wpe_view_backend_create (),
                                                    NULL, NULL);
    }

    // At this point, either the platform plug-in or the default WPE backend
    // must have succeeded in providing a WebKitWebViewBackend* instance.
    if (!view_backend)
        g_error ("Could not instantiate any WPE backend.");

    g_autoptr(WebKitWebView) web_view = g_object_new (WEBKIT_TYPE_WEB_VIEW,
                                                      "settings", cog_shell_get_web_settings (shell),
                                                      "web-context", web_context,
                                                      "zoom-level", s_options.scale_factor,
                                                      "backend", view_backend,
                                                      NULL);

    g_signal_connect (web_view, "create", G_CALLBACK (on_web_view_create), NULL);

    if (s_options.platform) {
        cog_platform_init_web_view (s_options.platform, web_view);
        g_autoptr(WebKitInputMethodContext) im_context = cog_platform_create_im_context (s_options.platform);
        webkit_web_view_set_input_method_context (web_view, im_context);
    }

    if (s_options.background_color != NULL) {
        WebKitColor color;
        gboolean has_valid_color = webkit_color_parse (&color, s_options.background_color);

        if (has_valid_color)
            webkit_web_view_set_background_color (web_view, &color);
        else
            g_error ("'%s' doesn't represent a valid #RRGGBBAA or CSS color format.", s_options.background_color);
    }

    switch (s_options.on_failure.action_id) {
        case WEBPROCESS_FAIL_ERROR_PAGE:
            // Nothing else needed, the default error handler (connected
            // below) already implements displaying an error page.
            break;

        case WEBPROCESS_FAIL_EXIT:
            cog_web_view_connect_web_process_terminated_exit_handler (web_view, EXIT_FAILURE);
            break;

        case WEBPROCESS_FAIL_EXIT_OK:
            cog_web_view_connect_web_process_terminated_exit_handler (web_view, EXIT_SUCCESS);
            break;

        case WEBPROCESS_FAIL_RESTART:
            // TODO: Un-hardcode the 5 retries per second.
            cog_web_view_connect_web_process_terminated_restart_handler (web_view, 5, 1000);
            break;

        default:
            g_assert_not_reached();
    }

    cog_web_view_connect_default_progress_handlers (web_view);
    cog_web_view_connect_default_error_handlers (web_view);

    webkit_web_view_load_uri (web_view, s_options.home_uri);
    g_clear_pointer (&s_options.home_uri, g_free);

    return g_steal_pointer (&web_view);
}


int
main (int argc, char *argv[])
{
    // We need to set the program name early because constructing the
    // CogLauncher instance will use g_get_prgname() to determine where
    // to store the caches for Web content.
    {
        const char *dir_separator = strrchr (argv[0], G_DIR_SEPARATOR);
        g_set_prgname (dir_separator ? dir_separator + 1 : argv[0]);
        g_set_application_name ("Cog");
    }

    g_autoptr(GApplication) app = G_APPLICATION (cog_launcher_get_default ());
    g_application_add_main_option_entries (app, s_cli_options);
    cog_launcher_add_web_settings_option_entries (COG_LAUNCHER (app));
    cog_launcher_add_web_cookies_option_entries (COG_LAUNCHER (app));
    cog_launcher_add_web_permissions_option_entries (COG_LAUNCHER (app));

    g_signal_connect (app, "shutdown", G_CALLBACK (on_shutdown), NULL);
    g_signal_connect (app, "handle-local-options",
                      G_CALLBACK (on_handle_local_options), NULL);
    g_signal_connect (cog_launcher_get_shell (COG_LAUNCHER (app)), "create-view",
                      G_CALLBACK (on_create_view), NULL);

    return g_application_run (app, argc, argv);
}
