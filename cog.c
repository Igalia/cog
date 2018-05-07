/*
 * cog.c
 * Copyright (C) 2018 Eduardo Lima <elima@igalia.com>
 * Copyright (C) 2017-2018 Adrian Perez <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#include <stdlib.h>
#include <string.h>
#include "cog.h"


enum webprocess_fail_action {
    WEBPROCESS_FAIL_UNKNOWN = 0,
    WEBPROCESS_FAIL_ERROR_PAGE,
    WEBPROCESS_FAIL_EXIT,
    WEBPROCESS_FAIL_EXIT_OK,
    WEBPROCESS_FAIL_RESTART,
};


static struct {
    gboolean version;
    gboolean print_appid;
    gboolean doc_viewer;
    gboolean dev_tools;
    gboolean webgl;
    gboolean log_console;
    gdouble  scale_factor;
    GStrv    dir_handlers;
    GStrv    arguments;
    union {
        char *action_name;
        enum webprocess_fail_action action_id;
    } on_failure;
} s_options = {
    .scale_factor = 1.0,
};


static GOptionEntry s_cli_options[] =
{
    { "version", '\0', 0, G_OPTION_ARG_NONE, &s_options.version,
        "Print version and exit",
        NULL },
    { "print-appid", '\0', 0, G_OPTION_ARG_NONE, &s_options.print_appid,
        "Print application ID and exit",
        NULL },
    { "dev-tools", 'D', 0, G_OPTION_ARG_NONE, &s_options.dev_tools,
        "Enable usage of the inspector and JavaScript console",
        NULL },
    { "log-console", 'v', 0, G_OPTION_ARG_NONE, &s_options.log_console,
        "Log JavaScript console messages to standard output",
        NULL },
    { "webgl", '\0', 0, G_OPTION_ARG_NONE, &s_options.webgl,
        "Allow web content to use the WebGL API",
        NULL },
    { "scale", '\0', 0, G_OPTION_ARG_DOUBLE, &s_options.scale_factor,
        "Zoom/Scaling factor (default: 1.0, no scaling)",
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
    { G_OPTION_REMAINING, '\0', 0, G_OPTION_ARG_FILENAME_ARRAY, &s_options.arguments,
        "", "[URL]" },
    { NULL }
};


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
        g_print ("%s\n", COG_VERSION_STRING);
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
        cog_launcher_set_request_handler (COG_LAUNCHER (application),
                                          s_options.dir_handlers[i],
                                          handler);
    }

    cog_launcher_set_home_uri (COG_LAUNCHER (application), utf8_uri);

    return -1;  /* Continue startup. */
}


static WebKitWebView*
on_create_web_view (CogLauncher *launcher,
                    void        *user_data)
{
    WebKitWebContext *web_context = cog_launcher_get_web_context (launcher);

    if (s_options.doc_viewer) {
        webkit_web_context_set_cache_model (web_context,
                                            WEBKIT_CACHE_MODEL_DOCUMENT_VIEWER);
    }

    g_autoptr(WebKitSettings) settings =
        webkit_settings_new_with_settings ("enable-developer-extras", s_options.dev_tools,
                                           "enable-page-cache", !s_options.doc_viewer,
                                           "enable-webgl", s_options.webgl,
                                           "enable-write-console-messages-to-stdout", s_options.log_console,
                                           NULL);

#if !COG_USE_WEBKITGTK
    WebKitWebViewBackend *view_backend = NULL;
    CogPlatform *platform = user_data;
    if (platform) {
        g_autoptr(GError) error = NULL;
        view_backend = cog_platform_get_view_backend (platform, NULL, &error);
        if (!view_backend) {
            g_assert_nonnull (error);
            g_printerr ("%s, Failed to get platform's view backend: %s\n",
                        g_get_prgname (),
                        error->message);
        }
    }
#endif

    g_autoptr(WebKitWebView) web_view = g_object_new (WEBKIT_TYPE_WEB_VIEW,
                                                      "settings", settings,
                                                      "web-context", web_context,
                                                      "zoom-level", s_options.scale_factor,
#if !(COG_USE_WEBKITGTK)
                                                      "backend", view_backend,
#endif
                                                      NULL);


    switch (s_options.on_failure.action_id) {
        case WEBPROCESS_FAIL_ERROR_PAGE:
            // Nothing else needed, the default error handler (connected
            // below) already implements displaying an error page.
            break;

        case WEBPROCESS_FAIL_EXIT:
            cog_web_view_connect_web_process_crashed_exit_handler (web_view, EXIT_FAILURE);
            break;

        case WEBPROCESS_FAIL_EXIT_OK:
            cog_web_view_connect_web_process_crashed_exit_handler (web_view, EXIT_SUCCESS);
            break;

        case WEBPROCESS_FAIL_RESTART:
            // TODO: Un-hardcode the 5 retries per second.
            cog_web_view_connect_web_process_crashed_restart_handler (web_view, 5, 1000);
            break;

        default:
            g_assert_not_reached();
    }

    cog_web_view_connect_default_progress_handlers (web_view);
    cog_web_view_connect_default_error_handlers (web_view);

    return g_steal_pointer (&web_view);
}


int
main (int argc, char *argv[])
{
    g_autoptr(GApplication) app = G_APPLICATION (cog_launcher_get_default ());
    g_application_add_main_option_entries (app, s_cli_options);

#if COG_USE_WEBKITGTK
    void *platform = NULL;
#else
    /*
     * Here we resolve the CogPlatform we are going to use. A Cog platform
     * is dynamically loaded object
     * that abstracts the specifics about how a WebView's WPE backend is going
     * to be constructed and rendered on a given platform.
     */

    /*
     * @FIXME: for now this is hardcoded to use CogPlatformFdo, which
     * essentially uses WPEBackend-fdo and renders on a running wayland
     * compositor.
     */
    const gchar *platform_soname = "libcogplatform-fdo.so";

    g_autoptr(CogPlatform) platform = cog_platform_new ();
    if (cog_platform_try_load (platform, platform_soname)) {
        g_autoptr(GError) error = NULL;
        if (!cog_platform_setup (platform, COG_LAUNCHER (app), "", &error)) {
            g_printerr ("%s: Failed to load FDO platform: %s\n",
                        g_get_prgname (),
                        error->message);
        }
    }
#endif

    g_signal_connect (app, "handle-local-options",
                      G_CALLBACK (on_handle_local_options), NULL);
    g_signal_connect (app, "create-web-view",
                      G_CALLBACK (on_create_web_view), platform);

    int result = g_application_run (app, argc, argv);

#if !COG_USE_WEBKITGTK
    if (platform)
        cog_platform_teardown (platform);
#endif

    return result;
}
