/*
 * dy-utils.c
 * Copyright (C) 2018 Adrian Perez <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#include "dy-utils.h"
#include <libsoup/soup.h>
#include <gio/gio.h>
#include <string.h>


char*
dy_appid_to_dbus_object_path (const char *appid)
{
    g_return_val_if_fail (appid != NULL, NULL);

    GString *s = g_string_new ("/");
    for (; *appid; appid++) {
        g_string_append_c (s, (*appid == '.') ? '/' : *appid);
    }
    return g_string_free (s, FALSE);
}


char*
dy_uri_guess_from_user_input (const char *uri_like,
                              gboolean    is_cli_arg,
                              GError    **error)
{
    g_return_val_if_fail (uri_like, NULL);

    g_autofree char *utf8_uri_like = NULL;
    if (is_cli_arg) {
        if (!(utf8_uri_like = g_locale_to_utf8 (uri_like, -1, NULL, NULL, error)))
            return NULL;
    } else {
        utf8_uri_like = g_strdup (uri_like);
    }

    g_autoptr(SoupURI) uri = soup_uri_new (utf8_uri_like);
    if (uri) {
        // GFile does not handle query strings nor fragments, so use it only
        // to get obtain the absolute path and modify the SoupURI in place.
        if (uri->scheme == SOUP_URI_SCHEME_FILE && uri->path && *uri->path != '\0') {
            g_autoptr(GFile) file = g_file_new_for_path (uri->path);
            g_autofree char *path = g_file_get_path (file);
            if (path) soup_uri_set_path (uri, path);
        }

        // If we have a scheme, do not try to guess whether the URI is a local
        // file or whether a scheme should be added to it, just return it. This
        // also covers the case of custom URI scheme handlers.
        if (uri->scheme) {
            // Allow "scheme:" to be a shorthand for "scheme:/", which is handy
            // when using custom URI scheme handlers.
            if (!uri->path || *uri->path == '\0')
                soup_uri_set_path (uri, "/");
            return soup_uri_to_string (uri, FALSE);
        }
    }

    // At this point we know that we have been given a shorthand without an
    // URI scheme, or something that cannot be parsed as an URI: try to find
    // a local file, otherwise add http:// as the scheme.
    g_autoptr(GFile) file = is_cli_arg
        ? g_file_new_for_commandline_arg (uri_like)
        : g_file_new_for_path (uri_like);

    if (g_file_is_native (file) && g_file_query_exists (file, NULL))
        return g_file_get_uri (file);

    return g_strconcat ("http://", utf8_uri_like, NULL);
}
