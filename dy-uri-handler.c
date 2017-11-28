/*
 * dy-uri-handler.c
 * Copyright (C) 2017 Adrian Perez <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#include "dy-launcher.h"
#include "dy-uri-handler.h"
#include <gio/gio.h>
#include <string.h>


struct _DyURIHandlerRequest {
    DyURIHandler *handler;
    WebKitURISchemeRequest *webkit_request;
    SoupURI *uri;
};


DyURIHandler*
dy_uri_handler_request_get_handler (DyURIHandlerRequest *request)
{
    g_return_val_if_fail (request, NULL);
    return request->handler;
}


const char*
dy_uri_handler_request_get_scheme (DyURIHandlerRequest *request)
{
    g_return_val_if_fail (request, NULL);
    return soup_uri_get_scheme (request->uri);
}


const char*
dy_uri_handler_request_get_prefix (DyURIHandlerRequest *request)
{
    g_return_val_if_fail (request, NULL);
    return soup_uri_get_host (request->uri);
}


const char*
dy_uri_handler_request_get_path (DyURIHandlerRequest *request)
{
    g_return_val_if_fail (request, NULL);
    return soup_uri_get_path (request->uri);
}


void
dy_uri_handler_request_load_bytes (DyURIHandlerRequest *request,
                                   const char          *mime_type,
                                   GBytes              *bytes)
{
    g_return_if_fail (request);
    g_return_if_fail (bytes);

    g_autoptr(GInputStream) stream = g_memory_input_stream_new_from_bytes (bytes);
    if (mime_type) {
        webkit_uri_scheme_request_finish (request->webkit_request,
                                          stream,
                                          g_bytes_get_size (bytes),
                                          mime_type);
    } else {
        gsize data_size = 0;
        const void *data = g_bytes_get_data (bytes, &data_size);
        g_autofree char *guessed_mime_type =
            g_content_type_guess (NULL, data, data_size, NULL);
        webkit_uri_scheme_request_finish (request->webkit_request,
                                          stream,
                                          data_size,
                                          guessed_mime_type);
    }
}


void
dy_uri_handler_request_load_string (DyURIHandlerRequest *request,
                                    const char          *mime_type,
                                    const char          *data,
                                    gssize               len)
{
    g_return_if_fail (request);
    g_return_if_fail (data);

    if (len < 0) len = strlen (data);
    g_autoptr(GInputStream) stream = g_memory_input_stream_new_from_data (data, len, NULL);
    if (mime_type) {
        webkit_uri_scheme_request_finish (request->webkit_request,
                                          stream,
                                          len,
                                          mime_type);
    } else {
        g_autofree char *guessed_mime_type =
            g_content_type_guess (NULL, data, len, NULL);
        webkit_uri_scheme_request_finish (request->webkit_request,
                                          stream,
                                          len,
                                          guessed_mime_type);
    }
}


void
dy_uri_handler_request_error (DyURIHandlerRequest *request,
                              const char          *format,
                              ...)
{
    va_list args;
    g_autoptr(GError) error = NULL;

    va_start (args, format);
    error = g_error_new_valist (g_io_error_quark (), G_IO_ERROR_FAILED, format, args);
    va_end (args);

    webkit_uri_scheme_request_finish_error (request->webkit_request, error);
}


typedef struct {
    DyURIHandlerCallback callback;
    void                *user_data;
} PrefixEntry;


static inline PrefixEntry*
prefix_entry_new (DyURIHandlerCallback callback,
                  void                *user_data)
{
    PrefixEntry *entry = g_slice_new (PrefixEntry);
    entry->callback = callback;
    entry->user_data = user_data;
    return entry;
}


static inline void
prefix_entry_free (void *entry)
{
    g_slice_free (PrefixEntry, entry);
}


struct _DyURIHandler
{
    GObject parent;

    PrefixEntry default_callback;
    GHashTable *prefix_map;
    char *scheme;
};

G_DEFINE_TYPE (DyURIHandler, dy_uri_handler, G_TYPE_OBJECT)


enum {
    PROP_0,
    PROP_SCHEME,
    N_PROPERTIES
};

static GParamSpec *s_properties[N_PROPERTIES] = { NULL, };


static void
dy_uri_handler_dispose (GObject *object)
{
    DyURIHandler *handler = DY_URI_HANDLER (object);

    g_clear_pointer (&handler->scheme, g_free);
    g_clear_pointer (&handler->prefix_map, g_hash_table_destroy);

    G_OBJECT_CLASS (dy_uri_handler_parent_class)->dispose (object);
}


static void
dy_uri_handler_get_property (GObject    *object,
                             unsigned    prop_id,
                             GValue     *value,
                             GParamSpec *pspec)
{
    DyURIHandler *handler = DY_URI_HANDLER (object);
    switch (prop_id) {
        case PROP_SCHEME:
            g_value_set_string (value, dy_uri_handler_get_scheme (handler));
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}


static void
dy_uri_handler_class_init (DyURIHandlerClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    object_class->dispose = dy_uri_handler_dispose;
    object_class->get_property = dy_uri_handler_get_property;

    s_properties[PROP_SCHEME] =
        g_param_spec_string ("scheme",
                             "URI Scheme",
                             "The scheme of the URIs handled",
                             NULL,
                             G_PARAM_READABLE |
                             G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class,
                                       N_PROPERTIES,
                                       s_properties);
}


static void
dy_uri_handler_init (DyURIHandler *handler)
{
}


DyURIHandler*
dy_uri_handler_new (const char *scheme)
{
    g_return_val_if_fail (scheme, NULL);

    DyURIHandler *handler = g_object_new (DY_TYPE_URI_HANDLER, NULL);
    handler->scheme = g_strdup (scheme);

    return handler;
}


const char
*dy_uri_handler_get_scheme (DyURIHandler *handler)
{
    g_return_val_if_fail (DY_IS_URI_HANDLER (handler), NULL);
    return handler->scheme;
}


gboolean
dy_uri_handler_register (DyURIHandler        *handler,
                         const char          *prefix,
                         DyURIHandlerCallback callback,
                         void                *user_data)
{
    g_return_val_if_fail (DY_IS_URI_HANDLER (handler), FALSE);
    g_return_val_if_fail (callback, FALSE);

    const char *replaced_prefix = NULL;

    if (prefix) {
        if (!handler->prefix_map)
            handler->prefix_map = g_hash_table_new_full (g_str_hash,
                                                         g_str_equal,
                                                         g_free,
                                                         prefix_entry_free);
        if (!g_hash_table_replace (handler->prefix_map,
                                   g_strdup (prefix),
                                   prefix_entry_new (callback, user_data)))
            replaced_prefix = prefix;
    } else {
        if (handler->default_callback.callback)
            replaced_prefix = "*";
        handler->default_callback.callback = callback;
        handler->default_callback.user_data = user_data;
    }

    if (replaced_prefix) {
        g_warning ("Handler for %s:%s replaced", handler->scheme, replaced_prefix);
        return TRUE;
    } else {
        return FALSE;
    }
}


gboolean
dy_uri_handler_unregister (DyURIHandler *handler,
                           const char   *prefix)
{
    g_return_val_if_fail (DY_IS_URI_HANDLER (handler), FALSE);

    gboolean found;

    if (!prefix) {
        found = !!handler->default_callback.callback;
        memset (&handler->default_callback, 0x00, sizeof (PrefixEntry));
    } else if (handler->prefix_map) {
        found = g_hash_table_remove (handler->prefix_map, prefix);
    }
    return found;
}


static SoupURI*
uri_from_webkit_uri_scheme_request (WebKitURISchemeRequest *request)
{
    SoupURI *uri = soup_uri_new (webkit_uri_scheme_request_get_uri (request));

    /*
     * If the URI failed to parse, we may still want to accept URIs of the
     * form "scheme:prefix[/path/to/resource]", so parse ourselves here.
     */
    if (!(SOUP_URI_IS_VALID (uri) && soup_uri_get_host (uri))) {
        g_clear_pointer (&uri, soup_uri_free);
        uri = soup_uri_new (NULL);
        soup_uri_set_scheme (uri, webkit_uri_scheme_request_get_scheme (request));

        g_autofree char *hostpath = g_strdup (webkit_uri_scheme_request_get_path (request));
        char *slash = strchr (hostpath, '/');
        char *host = hostpath;

        if (slash == hostpath) {
            /* Path prefix starts with a slash: ignore. */
            slash = strchr (slash + 1, '/');
            ++host;
        }

        if (slash) {
            soup_uri_set_path (uri, slash);
            *slash = '\0';
        } else {
            soup_uri_set_path (uri, "/");
        }
        soup_uri_set_host (uri, host);
    }

    g_assert (SOUP_URI_IS_VALID (uri));
    g_assert_nonnull (soup_uri_get_host (uri));
    const char *path = soup_uri_get_path (uri);
    if (!path || path[0] == '\0')
        soup_uri_set_path (uri, "/");

    return uri;
}


static void
dy_uri_handler_do_webkit_request (WebKitURISchemeRequest *webkit_request,
                                  void                   *user_data)
{
    DyURIHandlerRequest request = {
        .handler = g_object_ref (DY_URI_HANDLER (user_data)),
        .webkit_request = g_object_ref (webkit_request),
        .uri = uri_from_webkit_uri_scheme_request (webkit_request),
    };

    if (!request.uri) {
        g_warning ("Invalid URI: '%s'\n",
                   webkit_uri_scheme_request_get_uri (webkit_request));
        goto out;
    }

    PrefixEntry *entry = g_hash_table_lookup (request.handler->prefix_map,
                                              soup_uri_get_host (request.uri));
    if (!entry)
        entry = &request.handler->default_callback;

    if (entry) {
        (*entry->callback) (&request, entry->user_data);
    } else {
        g_autoptr(GError) error = g_error_new (g_io_error_quark (),
                                               G_IO_ERROR_NOT_FOUND,
                                               "No handler for '%s'",
                                               webkit_uri_scheme_request_get_uri (webkit_request));
        webkit_uri_scheme_request_finish_error (webkit_request, error);
        g_warning ("%s", error->message);
    }

out:
    g_clear_pointer (&request.uri, soup_uri_free);
    g_clear_object (&request.webkit_request);
    g_clear_object (&request.handler);
}


void
dy_uri_handler_attach (DyURIHandler *handler,
                       DyLauncher   *launcher)
{
    g_return_if_fail (DY_IS_URI_HANDLER (handler));
    g_return_if_fail (DY_IS_LAUNCHER (launcher));
    webkit_web_context_register_uri_scheme (dy_launcher_get_web_context (launcher),
                                            handler->scheme,
                                            dy_uri_handler_do_webkit_request,
                                            g_object_ref (handler),
                                            g_object_unref);
}
