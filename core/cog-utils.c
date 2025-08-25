/*
 * cog-utils.c
 * Copyright (C) 2018 Adrian Perez <aperez@igalia.com>
 *
 * SPDX-License-Identifier: MIT
 */

#include "cog-utils.h"
#include <errno.h>
#include <gio/gio.h>
#include <libsoup/soup.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

char *
cog_appid_to_dbus_object_path(const char *appid)
{
    g_return_val_if_fail(appid != NULL, NULL);

    GString *s = g_string_new("/");
    for (; *appid; appid++) {
        g_string_append_c(s, (*appid == '.') ? '/' : *appid);
    }
    return g_string_free(s, FALSE);
}

static char *
cog_uri_guess_internal(const char *utf8_uri_like)
{
#if COG_USE_SOUP2
    g_autoptr(SoupURI) uri = soup_uri_new(utf8_uri_like);
    if (uri) {
        if (uri->scheme == SOUP_URI_SCHEME_HTTP || uri->scheme == SOUP_URI_SCHEME_HTTPS ||
            uri->scheme == SOUP_URI_SCHEME_FTP || uri->scheme == SOUP_URI_SCHEME_WS ||
            uri->scheme == SOUP_URI_SCHEME_WSS) {
            // Use the input URI directly without further guessing.
            return g_strdup(utf8_uri_like);
        }

        // We want to allow passing relative paths, but URIs must use full
        // paths. GFile does not handle query strings nor fragments, so use it
        // only to obtain the absolute path and modify the SoupURI in place.
        g_autofree char *relpath = g_strconcat(uri->host ? uri->host : "", uri->path ? uri->path : "", NULL);
        if (uri->scheme == SOUP_URI_SCHEME_FILE && relpath && *relpath != '\0') {
            g_autoptr(GFile) file = g_file_new_for_path(relpath);
            g_autofree char *path = g_file_get_path(file);
            if (path) {
                soup_uri_set_path(uri, path);
                soup_uri_set_host(uri, "");
            }
        }

        // Allow "scheme:" to be a shorthand for "scheme:/", which
        // is handy when using custom URI scheme handlers.
        if (!uri->path || *uri->path == '\0')
            soup_uri_set_path(uri, "/");
        return soup_uri_to_string(uri, FALSE);
    }
#else
    const char *scheme = g_uri_peek_scheme(utf8_uri_like);
    if (scheme) {
        if (strcmp(scheme, "http") == 0 || strcmp(scheme, "https") == 0 || strcmp(scheme, "ftp") == 0 ||
            strcmp(scheme, "ws") == 0 || strcmp(scheme, "wss") == 0) {
            // Use the input URI directly without further guessing.
            return g_strdup(utf8_uri_like);
        }

        g_autoptr(GUri) uri = g_uri_parse(utf8_uri_like, G_URI_FLAGS_ENCODED, NULL);
        if (uri) {
            // We want to allow passing relative paths, but URIs must use full
            // paths. GFile does not handle query strings nor fragments, so use it
            // only to obtain the absolute path and modify the SoupURI in place.
            g_autofree char *relpath =
                g_strconcat(g_uri_get_host(uri) ? g_uri_get_host(uri) : "", g_uri_get_path(uri), NULL);
            if (strcmp(scheme, "file") == 0 && relpath && *relpath != '\0') {
                g_autoptr(GFile) file = g_file_new_for_path(relpath);
                g_autofree char *path = g_file_get_path(file);
                if (path) {
                    GUri *copy = soup_uri_copy(uri, SOUP_URI_HOST, NULL, SOUP_URI_PATH, path, SOUP_URI_NONE);
                    g_uri_unref(g_steal_pointer(&uri));
                    uri = copy;
                }
            }

            // Allow "scheme:" to be a shorthand for "scheme:/", which
            // is handy when using custom URI scheme handlers.
            if (*g_uri_get_path(uri) == '\0') {
                GUri *copy = soup_uri_copy(uri, SOUP_URI_PATH, "/", SOUP_URI_NONE);
                g_uri_unref(g_steal_pointer(&uri));
                uri = copy;
            }
            return g_uri_to_string(uri);
        }
    }
#endif
    return NULL;
}

/**
 * cog_uri_guess_from_user_input:
 * @uri_like: String containing a URI-like value.
 * @is_cli_arg: Whether the URI-like string is from a command line option.
 * @error: (out) (nullable): Location where to store an error, if any.
 *
 * Tries to assemble a valid URI from input that resembles a URI.
 *
 * First, if `is_cli_arg` is set, the input string is converted to UTF-8.
 * Then, the following heuristics may applied:
 *
 * - If the input is already a valid URI with a known scheme, return it as-is.
 * - If the input is a relative path, or resembles a local file path, try to
 *   resolve it to a full path and return a `file://` URI.
 * - If a URI does not have any path, set `/` as the path.
 * - As a last resort, try to prepend the `http://` scheme.
 *
 * The main use case for this function is turning some “simpler” version
 * of a URI, as typically entered by a user in a browser URL entry
 * (e.g. `wpewebkit.org/release`) and turn it into an actual
 * URI (`http://wpewebkit.org/release/`) which can be then passed to
 * [method@WebKit.WebView.load_uri].
 *
 * Returns: A valid, full URI as a string. If `NULL` is returned, the
 *    `error` will also be set.
 */
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
    g_autofree char *guessed_uri = cog_uri_guess_internal(utf8_uri_like);
    if (guessed_uri) {
        return g_steal_pointer(&guessed_uri);
    }

    // At this point we know that we have been given a shorthand without an
    // URI scheme, or something that cannot be parsed as a URI: try to find
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

/**
 * cog_key_file_parse_params_string:
 * @key_file: A key file
 * @group_name: Name of a key file group
 * @params_string: Input string
 * @error: Where to store an error, if any
 *
 * Parse parameters string storing values in a key file.
 *
 * Since: 0.18
 */
void
cog_key_file_parse_params_string(GKeyFile *key_file, const char *group_name, const char *params_string)
{
    g_return_if_fail(key_file);
    g_return_if_fail(group_name);
    g_return_if_fail(params_string);

    g_auto(GStrv) params = g_strsplit(params_string, ",", 0);
    for (unsigned i = 0; params[i]; i++) {
        g_auto(GStrv) kv = g_strsplit(params[i], "=", 2);
        if (g_strv_length(kv) != 2) {
            g_warning("%s: Invalid parameter syntax '%s'.", __func__, params[i]);
            continue;
        }

        const char *k = g_strstrip(kv[0]);
        const char *v = g_strstrip(kv[1]);

        g_key_file_set_value(key_file, group_name, k, v);
    }
}

/**
 * cog_apply_properties_from_key_file:
 * @object: An object to set properties on
 * @key_file: A key file
 * @group_name: Name of a key file group
 * @error: Where to store an error, if any
 *
 * Set properties on an object from values stored in a #GKeyFile.
 *
 * For each writable property of the object's class, looks up key with
 * the same name as the property in the specified group of the key file.
 * If present, it will be set as the new value of the property.
 *
 * If the specified group name does not exist in the key file, the
 * function returns `TRUE` without applying any changes to the object.
 * The group may contain keys with names other than those of the object
 * properties; these keys will be ignored.
 *
 * Properties of the following types are supported:
 *
 * - `G_TYPE_BOOLEAN`
 * - `G_TYPE_DOUBLE`
 * - `G_TYPE_FLOAT`
 * - `G_TYPE_STRING`
 *
 * Returns: Whether the configuration file values have been successfully
 *    applied to the corresponding object properties.
 *
 * Since: 0.18
 */
gboolean
cog_apply_properties_from_key_file(GObject *object, GKeyFile *key_file, const char *group_name, GError **error)
{
    g_return_val_if_fail(G_IS_OBJECT(object), FALSE);
    g_return_val_if_fail(key_file != NULL, FALSE);
    g_return_val_if_fail(group_name != NULL, FALSE);

    if (!g_key_file_has_group(key_file, group_name))
        return TRUE;

    GObjectClass *object_class = G_OBJECT_GET_CLASS(object);

    unsigned n_properties = 0;

    g_autofree GParamSpec **properties = g_object_class_list_properties(object_class, &n_properties);

    if (!properties || n_properties == 0)
        return TRUE;

    for (unsigned i = 0; i < n_properties; i++) {
        GParamSpec *pspec = properties[i];

        /* Pick only writable properties. */
        if (!pspec || !(pspec->flags & G_PARAM_WRITABLE) || (pspec->flags & G_PARAM_CONSTRUCT_ONLY))
            continue;

        g_autoptr(GError) item_error = NULL;
        g_auto(GValue)    prop_value = G_VALUE_INIT;
        const GType       prop_type = G_PARAM_SPEC_VALUE_TYPE(pspec);
        const char       *prop_name = g_param_spec_get_name(pspec);

        switch (prop_type) {
        case G_TYPE_BOOLEAN: {
            gboolean value = g_key_file_get_boolean(key_file, group_name, prop_name, &item_error);
            if (!value && item_error) {
                if (g_error_matches(item_error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_KEY_NOT_FOUND))
                    continue;
                g_propagate_error(error, g_steal_pointer(&item_error));
                return FALSE;
            }
            g_value_set_boolean(g_value_init(&prop_value, G_TYPE_BOOLEAN), value);
            break;
        }

        case G_TYPE_FLOAT:
        case G_TYPE_DOUBLE: {
            double value = g_key_file_get_double(key_file, group_name, prop_name, &item_error);
            if (g_error_matches(item_error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_KEY_NOT_FOUND))
                continue;
            if (item_error) {
                g_propagate_error(error, g_steal_pointer(&item_error));
                return FALSE;
            }
            g_value_set_double(g_value_init(&prop_value, G_TYPE_DOUBLE), value);
            break;
        }

        case G_TYPE_STRING: {
            g_autofree char *value = g_key_file_get_string(key_file, group_name, prop_name, &item_error);
            if (g_error_matches(item_error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_NOT_FOUND))
                continue;
            if (item_error) {
                g_propagate_error(error, g_steal_pointer(&item_error));
                return FALSE;
            }
            g_value_take_string(g_value_init(&prop_value, G_TYPE_STRING), g_steal_pointer(&value));
            break;
        }

        default:
            /* Skip properties of unsupported types. */
            continue;
        }

        g_assert(G_IS_VALUE(&prop_value));
        g_object_set_property(object, prop_name, &prop_value);

        g_auto(GValue) prop_str_value = G_VALUE_INIT;
        g_value_init(&prop_str_value, G_TYPE_STRING);
        if (g_value_transform(&prop_value, &prop_str_value)) {
            g_debug("%s: Setting %s<%p>.%s = <%s>", __func__, G_OBJECT_CLASS_NAME(object_class), object, prop_name,
                    g_value_get_string(&prop_str_value));
        } else {
            g_debug("%s: Setting %s<%p>.%s = <...>", __func__, G_OBJECT_CLASS_NAME(object_class), object, prop_name);
        }
    }

    return TRUE;
}
