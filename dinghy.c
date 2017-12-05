/*
 * dinghy.c
 * Copyright (C) 2017 Adrian Perez <aperez@igalia.com>
 *
 * Distributed under terms of the BSD-2-Clause license.
 */

#include <stdlib.h>
#include <string.h>
#include "dinghy.h"


static struct {
    gboolean version;
    gboolean print_appid;
    gboolean doc_viewer;
    GStrv    dir_handlers;
    GStrv    arguments;
} s_options = { 0, };


static GOptionEntry s_cli_options[] =
{
    { "version", '\0', 0, G_OPTION_ARG_NONE, &s_options.version,
        "Print version and exit",
        NULL },
    { "print-appid", '\0', 0, G_OPTION_ARG_NONE, &s_options.print_appid,
        "Print application ID and exit",
        NULL },
    { "doc-viewer", '\0', 0, G_OPTION_ARG_NONE, &s_options.doc_viewer,
        "Document viewer mode: optimizes for local loading of Web content. "
        "This reduces memory usage at the cost of reducing caching of "
        "resources loaded from the network.",
        NULL },
    { "dir-handler", 'd', 0, G_OPTION_ARG_STRING_ARRAY, &s_options.dir_handlers,
        "Add a URI scheme handler for a directory",
        "SCHEME:PATH" },
    { G_OPTION_REMAINING, '\0', 0, G_OPTION_ARG_FILENAME_ARRAY, &s_options.arguments,
        "", "[URL]" },
    { NULL }
};


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

    g_autofree char *utf8_uri = NULL;
    g_autoptr(GFile) file = g_file_new_for_commandline_arg (uri);

    if (g_file_is_native (file) && g_file_query_exists (file, NULL)) {
        utf8_uri = g_file_get_uri (file);
    } else {
        g_autoptr(GError) error = NULL;
        utf8_uri = g_locale_to_utf8 (uri, -1, NULL, NULL, &error);
        if (!utf8_uri) {
            g_printerr ("%s: URI '%s' is invalid UTF-8: %s\n", g_get_prgname (), uri, error->message);
            return EXIT_FAILURE;
        }
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
            g_printerr ("%s", "No scheme specified for '%s' URI handler\n",
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

    return NULL;  /* Let DyLauncher create the Web view. */
}


int
main (int argc, char *argv[])
{
    g_autoptr(GApplication) app = G_APPLICATION (dy_launcher_get_default ());
    g_application_add_main_option_entries (app, s_cli_options);
    g_signal_connect (app, "handle-local-options",
                      G_CALLBACK (on_handle_local_options), NULL);
    g_signal_connect (app, "create-web-view",
                      G_CALLBACK (on_create_web_view), NULL);
    return g_application_run (app, argc, argv);
}
