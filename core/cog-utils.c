/*
 * cog-utils.c
 * Copyright (C) 2018 Adrian Perez <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#include "cog-utils.h"
#include <errno.h>
#include <libsoup/soup.h>
#include <gio/gio.h>
#include <stdlib.h>
#include <string.h>


char*
cog_appid_to_dbus_object_path (const char *appid)
{
    g_return_val_if_fail (appid != NULL, NULL);

    GString *s = g_string_new ("/");
    for (; *appid; appid++) {
        g_string_append_c (s, (*appid == '.') ? '/' : *appid);
    }
    return g_string_free (s, FALSE);
}


char*
cog_uri_guess_from_user_input (const char *uri_like,
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

    // If the URI can be parsed do not try to guess whether the argument
    // is a local file or whether a scheme should be added to it. This also
    // covers the case of custom URI scheme handlers.
    g_autoptr(SoupURI) uri = soup_uri_new (utf8_uri_like);
    if (uri) {
        if (uri->scheme == SOUP_URI_SCHEME_HTTP ||
            uri->scheme == SOUP_URI_SCHEME_HTTPS ||
            uri->scheme == SOUP_URI_SCHEME_FTP ||
            uri->scheme == SOUP_URI_SCHEME_WS ||
            uri->scheme == SOUP_URI_SCHEME_WSS)
        {
            // Use the input URI directly without further guessing.
            return g_steal_pointer (&utf8_uri_like);
        }

        // We want to allow passing relative paths, but URIs must use full
        // paths. GFile does not handle query strings nor fragments, so use it
        // only to obtain the absolute path and modify the SoupURI in place.
        g_autofree char *relpath = g_strconcat (uri->host ? uri->host : "",
                                                uri->path ? uri->path : "",
                                                NULL);
        if (uri->scheme == SOUP_URI_SCHEME_FILE && relpath && *relpath != '\0') {
            g_autoptr(GFile) file = g_file_new_for_path (relpath);
            g_autofree char *path = g_file_get_path (file);
            if (path) {
                soup_uri_set_path (uri, path);
                soup_uri_set_host (uri, "");
            }
        }

        // Allow "scheme:" to be a shorthand for "scheme:/", which
        // is handy when using custom URI scheme handlers.
        if (!uri->path || *uri->path == '\0')
            soup_uri_set_path (uri, "/");
        return soup_uri_to_string (uri, FALSE);
    }

    // At this point we know that we have been given a shorthand without an
    // URI scheme, or something that cannot be parsed as an URI: try to find
    // a local file, otherwise add http:// as the scheme.
    g_autoptr(GFile) file = is_cli_arg
        ? g_file_new_for_commandline_arg (uri_like)
        : g_file_new_for_path (utf8_uri_like);

    if (g_file_is_native (file) && g_file_query_exists (file, NULL))
        return g_file_get_uri (file);

    return g_strconcat ("http://", utf8_uri_like, NULL);
}


static gboolean
option_entry_parse_to_property (const char *option,
                                const char *value,
                                GObject    *object,
                                GError    **error)
{
    // Check and skip the two leading dashes.
    if (option[0] != '-' || option[1] != '-') {
        g_set_error (error,
                     G_OPTION_ERROR,
                     G_OPTION_ERROR_FAILED,
                     "Invalid option '%s'",
                     option);
        return FALSE;
    }
    option += 2;

    const GParamSpec *prop =
        g_object_class_find_property (G_OBJECT_GET_CLASS (object), option);
    if (!prop) {
        g_set_error (error,
                     G_OPTION_ERROR,
                     G_OPTION_ERROR_FAILED,
                     "Property '%s::%s' does not exist",
                     G_OBJECT_CLASS_NAME (G_OBJECT_GET_CLASS (object)),
                     option);
        return FALSE;
    }

    const GType prop_type = G_PARAM_SPEC_VALUE_TYPE (prop);
    switch (prop_type) {
        case G_TYPE_BOOLEAN: {
            const gboolean prop_value = !(value &&
                                          g_ascii_strcasecmp (value, "true") &&
                                          strcmp (value, "1"));
            g_object_set (object, option, prop_value, NULL);
            break;
        }
        case G_TYPE_DOUBLE:
        case G_TYPE_FLOAT: {
            errno = 0;
            char *end = NULL;
            double prop_value = g_ascii_strtod (value, &end);
            if (errno == ERANGE ||
                (prop_type == G_TYPE_FLOAT && (prop_value > G_MAXFLOAT || prop_value < G_MINFLOAT)))
            {
                g_set_error (error,
                             G_OPTION_ERROR,
                             G_OPTION_ERROR_BAD_VALUE,
                             "%s value '%s' for %s out of range",
                             prop_type == G_TYPE_FLOAT ? "Float" : "Double",
                             value,
                             option);
                return FALSE;
            }
            if (errno || value == end) {
                g_set_error (error,
                             G_OPTION_ERROR,
                             G_OPTION_ERROR_BAD_VALUE,
                             "Cannot parse %s value '%s' for %s",
                             prop_type == G_TYPE_FLOAT ? "float" : "double",
                             value,
                             option);
                return FALSE;
            }
            if (prop_type == G_TYPE_FLOAT)
                g_object_set (object, option, (float) prop_value, NULL);
            else
                g_object_set (object, option, prop_value, NULL);
            break;
        }
        case G_TYPE_INT64:
        case G_TYPE_INT:
        case G_TYPE_LONG: {
            errno = 0;
            char *end = NULL;
            int64_t prop_value = g_ascii_strtoll (value, &end, 0);
            if (errno == ERANGE ||
                (prop_type == G_TYPE_INT && (prop_value > INT_MAX || prop_value < INT_MIN)) ||
                (prop_type == G_TYPE_LONG && (prop_value > LONG_MAX || prop_value < LONG_MIN)))
            {
                g_set_error (error,
                             G_OPTION_ERROR,
                             G_OPTION_ERROR_BAD_VALUE,
                             "%s value '%s' for %s out of range",
                             prop_type == G_TYPE_INT64 ? "int64" :
                                (prop_type == G_TYPE_INT ? "int" : "long"),
                            value,
                            option);
                return FALSE;
            }
            if (errno || value == end) {
                g_set_error (error,
                             G_OPTION_ERROR,
                             G_OPTION_ERROR_BAD_VALUE,
                             "Cannot parse %s value '%s' for %s",
                             prop_type == G_TYPE_INT64 ? "int64" :
                                (prop_type == G_TYPE_INT ? "int" : "long"),
                            value,
                            option);
                return FALSE;
            }
            if (prop_type == G_TYPE_INT)
                g_object_set (object, option, (int) prop_value, NULL);
            else if (prop_type == G_TYPE_LONG)
                g_object_set (object, option, (long) prop_value, NULL);
            else
                g_object_set (object, option, prop_value, NULL);
            break;
        }
        case G_TYPE_STRING:
            g_object_set (object, option, value, NULL);
            break;
        case G_TYPE_UINT:
        case G_TYPE_UINT64:
        case G_TYPE_ULONG: {
            errno = 0;
            char *end = NULL;
            guint64 prop_value = g_ascii_strtoull (value, &end, 0);
            if (errno == ERANGE ||
                ((prop_type == G_TYPE_UINT && prop_value > UINT_MAX) ||
                 (prop_type == G_TYPE_ULONG && prop_value > ULONG_MAX)))
            {
                g_set_error (error,
                             G_OPTION_ERROR,
                             G_OPTION_ERROR_BAD_VALUE,
                             "%s value '%s' for %s out of range",
                             prop_type == G_TYPE_UINT64 ? "uint64" :
                                (prop_type == G_TYPE_UINT ? "uint" : "ulong"),
                            value,
                            option);
                return FALSE;
            }
            if (errno || value == end) {
                g_set_error (error,
                             G_OPTION_ERROR,
                             G_OPTION_ERROR_BAD_VALUE,
                             "Cannot parse %s value '%s' for %s",
                             prop_type == G_TYPE_UINT64 ? "uint64" :
                                (prop_type == G_TYPE_UINT ? "uint" : "ulong"),
                            value,
                            option);
                return FALSE;
            }
            if (prop_type == G_TYPE_UINT)
                g_object_set (object, option, (unsigned int) prop_value, NULL);
            else if (prop_type == G_TYPE_ULONG)
                g_object_set (object, option, (unsigned long) prop_value, NULL);
            else
                g_object_set (object, option, prop_value, NULL);
            break;
        }
        default:
            g_assert_not_reached ();
    }

    return TRUE;
}

int
entry_comparator (const void *p1, const void *p2)
{
    GOptionEntry *e1 = (GOptionEntry *) p1;
    GOptionEntry *e2 = (GOptionEntry *) p2;
    return g_strcmp0 (e1->long_name, e2->long_name);
}

GOptionEntry*
cog_option_entries_from_class (GObjectClass *klass)
{
    g_return_val_if_fail (klass != NULL, NULL);

    unsigned n_properties = 0;
    g_autofree GParamSpec **properties =
        g_object_class_list_properties (klass, &n_properties);

    if (!properties || n_properties == 0)
        return NULL;

    g_autofree GOptionEntry *entries = g_new0 (GOptionEntry, n_properties + 1);

    unsigned e = 0;
    for (unsigned i = 0; i < n_properties; i++) {
        GParamSpec *prop = properties[i];

        // Pick only writable properties.
        if (!prop || !(prop->flags & G_PARAM_WRITABLE) || (prop->flags & G_PARAM_CONSTRUCT_ONLY))
            continue;

        // Pick only properties of basic types we know how to convert.
        const GType prop_type = G_PARAM_SPEC_VALUE_TYPE (prop);
        const char *type_name = NULL;
        switch (prop_type) {
            case G_TYPE_BOOLEAN: type_name = "BOOL"; break;
            case G_TYPE_DOUBLE:
            case G_TYPE_FLOAT: type_name = "FLOAT"; break;
            case G_TYPE_INT64:
            case G_TYPE_INT:
            case G_TYPE_LONG: type_name = "INTEGER"; break;
            case G_TYPE_STRING: type_name = "STRING"; break;
            case G_TYPE_UINT:
            case G_TYPE_UINT64:
            case G_TYPE_ULONG: type_name = "UNSIGNED"; break;
            default:
                continue;
        }

        // Fill entry.
        GOptionEntry *entry = &entries[e++];
        entry->long_name = g_param_spec_get_name (prop);
        entry->arg = G_OPTION_ARG_CALLBACK;
        entry->arg_data = option_entry_parse_to_property;
        entry->description = g_param_spec_get_blurb (prop);
        entry->arg_description = type_name;
        if (prop_type == G_TYPE_BOOLEAN && g_str_has_prefix (entry->long_name, "enable-"))
            entry->flags |= G_OPTION_FLAG_OPTIONAL_ARG;
    }

    // Sort entries by long name.
    qsort (entries, e, sizeof (GOptionEntry), entry_comparator);

    return g_steal_pointer (&entries);
}
