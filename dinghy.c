/*
 * dinghy.c
 * Copyright (C) 2017-2018 Adrian Perez <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#include <stdlib.h>
#include <string.h>
#include "dinghy.h"


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
#if DY_USE_MODE_MONITOR
    char    *sysfs_path;
#if DY_USE_DRM_MODE_MONITOR
    char    *drmdev_path;
#endif
#endif /* DY_USE_MODE_MONITOR */
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
#if DY_USE_MODE_MONITOR
    { "sysfs-mode-monitor", '\0', 0, G_OPTION_ARG_STRING, &s_options.sysfs_path,
        "SysFS framebuffer mode file to monitor", "PATH" },
#if DY_USE_DRM_MODE_MONITOR
    { "drm-mode-monitor", '\0', 0, G_OPTION_ARG_STRING, &s_options.drmdev_path,
        "Path to a DRM/KMS device node", "PATH" },
#endif
#endif /* !DY_VERSION_STRING */
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


#if DY_USE_MODE_MONITOR
static void
on_mode_monitor_notify (DyModeMonitor *monitor,
                        GParamSpec    *pspec,
                        DyLauncher    *launcher)
{
    const DyModeMonitorInfo *info = dy_mode_monitor_get_info (monitor);

    const char *device_name = "(unnamed)";
    if (DY_IS_SYSFS_MODE_MONITOR (monitor))
        device_name = dy_sysfs_mode_monitor_get_path (DY_SYSFS_MODE_MONITOR (monitor));
#if DY_USE_DRM_MODE_MONITOR
    else if (DY_IS_DRM_MODE_MONITOR (monitor))
        device_name = dy_drm_mode_monitor_get_device_path (DY_DRM_MODE_MONITOR (monitor));
#endif

    g_printerr ("Device '%s', mode %" PRIu32 "x%" PRIu32 " (%s)\n",
                device_name, info->width, info->height, info->mode_id);

#if DY_USE_WEBKITGTK
#else
#endif /* DY_USE_WEBKITGTK */
}

static inline gboolean
attach_sysfs_mode_monitor (DyLauncher *launcher,
                           GError    **error)
{
    g_autoptr(GFile) sysfs_file = g_file_new_for_commandline_arg (s_options.sysfs_path);
    g_autoptr(DySysfsModeMonitor) monitor = dy_sysfs_mode_monitor_new (sysfs_file, error);
    if (!monitor)
        return FALSE;

    g_signal_connect_object (g_steal_pointer (&monitor),
                             "notify::mode-id",
                             G_CALLBACK (on_mode_monitor_notify),
                             launcher,
                             G_CONNECT_AFTER);
    return TRUE;
}

#if DY_USE_DRM_MODE_MONITOR

static inline gboolean
attach_drm_mode_monitor (DyLauncher *launcher,
                         GError    **error)
{
    g_autoptr(GFile) drmdev_file = g_file_new_for_commandline_arg (s_options.drmdev_path);
    g_autoptr(DyDrmModeMonitor) monitor = dy_drm_mode_monitor_new (drmdev_file, error);
    if (!monitor)
        return FALSE;

    g_signal_connect_object (g_steal_pointer (&monitor),
                             "notify::mode-id",
                             G_CALLBACK (on_mode_monitor_notify),
                             launcher,
                             G_CONNECT_AFTER);
    return TRUE;
}

#endif /* DY_USE_DRM_MODE_MONITOR */

#endif /* DY_USE_MODE_MONITOR */


static int
on_handle_local_options (GApplication *application,
                         GVariantDict *options,
                         void         *user_data)
{
    if (s_options.version) {
        g_print ("%s\n", DY_VERSION_STRING);
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
        if (!(uri = g_getenv ("DINGHY_URL"))) {
            g_printerr ("%s: URL not passed in the command line, and DINGHY_URL not set\n", g_get_prgname ());
            return EXIT_FAILURE;
        }
    } else if (g_strv_length (s_options.arguments) > 1) {
        g_printerr ("%s: Cannot load more than one URL.\n", g_get_prgname ());
        return EXIT_FAILURE;
    } else {
        uri = s_options.arguments[0];
    }

    g_autoptr(GError) error = NULL;
    g_autofree char *utf8_uri = dy_uri_guess_from_user_input (uri, TRUE, &error);
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
     * corresponding DyURIHandler objects is done at GApplication::startup.
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
        if (!dy_directory_files_handler_is_suitable_path (file, &error)) {
            g_printerr ("%s: %s\n", g_get_prgname (), error->message);
            return EXIT_FAILURE;
        }

        *colon = '\0';  /* NULL-terminate the URI scheme name. */
        g_autoptr(DyRequestHandler) handler = dy_directory_files_handler_new (file);
        dy_launcher_set_request_handler (DY_LAUNCHER (application),
                                         s_options.dir_handlers[i],
                                         handler);
    }

    dy_launcher_set_home_uri (DY_LAUNCHER (application), utf8_uri);

#if DY_USE_MODE_MONITOR
    if (s_options.sysfs_path) {
        g_autoptr(GError) error = NULL;
        if (!attach_sysfs_mode_monitor (DY_LAUNCHER (application), &error)) {
            g_printerr ("%s: Cannot monitor SysFS path '%s': %s\n",
                        g_get_prgname (), s_options.sysfs_path, error->message);
            return EXIT_FAILURE;
        }
        g_clear_pointer (&s_options.sysfs_path, g_free);
    }

#if DY_USE_DRM_MODE_MONITOR
    if (s_options.drmdev_path) {
        g_autoptr(GError) error = NULL;
        if (!attach_drm_mode_monitor (DY_LAUNCHER (application), &error)) {
            g_printerr ("%s: Cannot monitor DRM/KMS device '%s': %s\n",
                        g_get_prgname (), s_options.drmdev_path, error->message);
            return EXIT_FAILURE;
        }
        g_clear_pointer (&s_options.drmdev_path, g_free);
    }
#endif /* DY_USE_DRM_MODE_MONITOR */
#endif /* DY_USE_MODE_MONITOR */

    return -1;  /* Continue startup. */
}


static WebKitWebView*
on_create_web_view (DyLauncher *launcher,
                    void       *user_data)
{
    WebKitWebContext *web_context = dy_launcher_get_web_context (launcher);

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

    WebKitWebViewBackend *view_backend = NULL;
#if !(DY_USE_WEBKITGTK)
    DyPlatform *platform = user_data;
    if (platform) {
        GError *error = NULL;
        view_backend = dy_platform_get_view_backend (platform, NULL, &error);
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
#if !(DY_USE_WEBKITGTK)
                                                      "backend", view_backend,
#endif
                                                      NULL);


    switch (s_options.on_failure.action_id) {
        case WEBPROCESS_FAIL_ERROR_PAGE:
            // Nothing else needed, the default error handler (connected
            // below) already implements displaying an error page.
            break;

        case WEBPROCESS_FAIL_EXIT:
            dy_web_view_connect_web_process_crashed_exit_handler (web_view, EXIT_FAILURE);
            break;

        case WEBPROCESS_FAIL_EXIT_OK:
            dy_web_view_connect_web_process_crashed_exit_handler (web_view, EXIT_SUCCESS);
            break;

        case WEBPROCESS_FAIL_RESTART:
            // TODO: Un-hardcode the 5 retries per second.
            dy_web_view_connect_web_process_crashed_restart_handler (web_view, 5, 1000);
            break;

        default:
            g_assert_not_reached();
    }

    dy_web_view_connect_default_progress_handlers (web_view);
    dy_web_view_connect_default_error_handlers (web_view);

    return g_steal_pointer (&web_view);
}


int
main (int argc, char *argv[])
{
    g_autoptr(GApplication) app = G_APPLICATION (dy_launcher_get_default ());
    g_application_add_main_option_entries (app, s_cli_options);

    DyPlatform *platform = NULL;

#if !(DY_USE_WEBKITGTK)
    /*
     * Here we resolve the DyPlatform we are going to use. A Dinghy platform
     * is dynamically loaded object
     * that abstracts the specifics about how a WebView's WPE backend is going
     * to be constructed and rendered on a given platform.
     */

    /*
     * @FIXME: for now this is hardcoded to use DyPlatformFdo, which
     * essentially uses WPEBackend-fdo and renders on a running wayland
     * compositor.
     */
    const gchar *platform_soname = "libdinghyplatform-fdo.so";

    platform = dy_platform_new ();
    if (dy_platform_try_load (platform, platform_soname)) {
        GError *error = NULL;
        if (!dy_platform_setup (platform, DY_LAUNCHER (app), "", &error)) {
            g_printerr ("%s: Failed to load FDO platform: %s\n",
                        g_get_prgname (),
                        error->message);
            dy_platform_free (platform);
            platform = NULL;
        }
    }
#endif

    g_signal_connect (app, "handle-local-options",
                      G_CALLBACK (on_handle_local_options), NULL);
    g_signal_connect (app, "create-web-view",
                      G_CALLBACK (on_create_web_view), platform);

    int result = g_application_run (app, argc, argv);

#if !(DY_USE_WEBKITGTK)
    if (platform) {
        dy_platform_teardown (platform);
        dy_platform_free (platform);
    }
#endif

    return result;
}
