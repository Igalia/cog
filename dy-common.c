/*
 * dy-common.c
 * Copyright (C) 2018 Adrian Perez <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#include "dy-common.h"
#include <gio/gio.h>
#include <string.h>


char*
dy_uri_guess_from_user_input (const char *uri_like,
                              gboolean    is_cli_arg,
                              GError    **error)
{
    g_return_val_if_fail (uri_like, NULL);

    g_autoptr(GFile) file = is_cli_arg
        ? g_file_new_for_commandline_arg (uri_like)
        : g_file_new_for_path (uri_like);

    if (g_file_is_native (file) && g_file_query_exists (file, NULL))
        return g_file_get_uri (file);

    g_autofree char *utf8_uri_like = NULL;
    if (is_cli_arg) {
        if (!(utf8_uri_like = g_locale_to_utf8 (uri_like, -1, NULL, NULL, error)))
            return NULL;
    } else {
        utf8_uri_like = g_strdup (uri_like);
    }

    g_autofree char *lc = g_utf8_strdown (utf8_uri_like, -1);
    if (g_str_has_prefix (lc, "http://") || g_str_has_prefix (lc, "https://"))
        return g_steal_pointer (&utf8_uri_like);

    return g_strconcat ("http://", utf8_uri_like, NULL);
}
