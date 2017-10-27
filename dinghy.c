/*
 * dinghy.c
 * Copyright (C) 2017 Adrian Perez <aperez@igalia.com>
 *
 * Distributed under terms of the BSD-2-Clause license.
 */

#include <wpe/webkit.h>
#include <stdlib.h>

#include "dinghy.h"


static struct {
    gboolean version;
    GStrv    arguments;
} s_options = { 0, };


static GOptionEntry s_cli_options[] =
{
    { "version", '\0', 0, G_OPTION_ARG_NONE, &s_options.version, "Print version and exit", NULL },
    { G_OPTION_REMAINING, '\0', 0, G_OPTION_ARG_FILENAME_ARRAY, &s_options.arguments, "", "[URL]" },
    { NULL }
};


static gboolean
handle_shutdown_signal (gpointer user_data)
{
    
}


static int
on_handle_local_options (GApplication *application,
                         GVariantDict *options,
                         gpointer      user_data)
{
    if (s_options.version) {
        g_print ("%s\n", PROJECT_VERSION);
        return EXIT_SUCCESS;
    }

#if 0
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

    /*
     * At this point we have the URL that will be loaded: store it in the
     * options dictionary, and return -1 to let the default GApplication
     * implementation emit the "open" signal passing the URI to it.
     */
    g_assert_nonnull (utf8_uri);
    g_variant_dict_insert (options,
                           G_OPTION_REMAINING,
                           "^aay", &utf8_uri, 1);
#endif

    return -1;
}


int
main (int argc, char *argv[])
{
    g_autoptr(GApplication) app = G_APPLICATION (dy_launcher_get_default ());
    g_application_add_main_option_entries (app, s_cli_options);
    g_signal_connect (app, "handle-local-options",
                      G_CALLBACK (on_handle_local_options), NULL);
    return g_application_run (app, argc, argv);
}
