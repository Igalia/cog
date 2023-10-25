/*
 * cog-host-routes-handler.c
 * Copyright (C) 2021 Igalia S.L.
 *
 * SPDX-License-Identifier: MIT
 */

#include "cog-host-routes-handler.h"
#include "cog-directory-files-handler.h"

/**
 * CogHostRoutesHandler:
 *
 * Direct custom URI scheme requests to different handlers.
 *
 * Handler for custom URI scheme requests that can route requests to
 * different handlers depending on the *host* component of the requested
 * URI.
 *
 * Optionally, if a “fallback” handler has been specified, it will be
 * used to serve requests which do not match any of the routed hosts.
 *
 * The set of available host routes entries can be configured using
 * [method@Cog.HostRoutesHandler.add] and
 * [method@Cog.HostRoutesHandler.remove]. For each request, route entries
 * are checked and the one that matches the URI *host* component
 * will handle the request.
 *
 * This handler is typically used in tandem with
 * [class@Cog.DirectoryFilesHandler], the latter being typically a
 * fallback, or as the handler for a routed host.
 *
 * The following configures a handler to route requests with their host
 * matching `example.host` to a certain directory, and all the rest of
 * hosts to another directory:
 *
 * ```c
 * GFile *fallback_dir = g_file_new_for_path ("data/default");
 * CogRequestHandler *fallback_handler = cog_directory_files_handler_new (fallback_dir);
 *
 * GFile *example_host_dir = g_file_new_for_path ("data/example");
 * CogRequestHandler *example_host_handler = cog_directory_files_handler_new (example_host_handler);
 *
 * CogRequestHandler *handler = cog_host_routes_handler_new (fallback_handler);
 * cog_host_routes_handler_add (COG_HOST_ROUTES_HANDLER (handler),
 *                             "example.host",
 *                             example_host_handler);
 * ```
 */

struct _CogHostRoutesHandler {
    GObject parent;

    CogRequestHandler *fallback;
    GHashTable *route; /* string -> CogRequestHandler */
};

enum {
    PROP_0,
    PROP_FALLBACK_HANDLER,
    N_PROPERTIES,
};

static GParamSpec *s_properties[N_PROPERTIES] = {
    NULL,
};

static void
cog_host_routes_handler_run_fallback(CogHostRoutesHandler *self, WebKitURISchemeRequest *request)
{
    if (self->fallback) {
        cog_request_handler_run(self->fallback, request);
    } else {
        g_autoptr(GError) error = g_error_new(G_FILE_ERROR,
                                              G_FILE_ERROR_NOENT,
                                              "No file for URI path: %s",
                                              webkit_uri_scheme_request_get_path(request));
        webkit_uri_scheme_request_finish_error(request, error);
    }
}

static void
cog_host_routes_handler_run(CogRequestHandler *request_handler, WebKitURISchemeRequest *request)
{
    CogHostRoutesHandler *self = COG_HOST_ROUTES_HANDLER(request_handler);

#if COG_USE_SOUP2
    g_autoptr(SoupURI) uri = soup_uri_new(webkit_uri_scheme_request_get_uri(request));
    const char *host = SOUP_URI_IS_VALID(uri) ? soup_uri_get_host(uri) : NULL;
#else
    g_autoptr(GUri) uri = g_uri_parse(webkit_uri_scheme_request_get_uri(request), G_URI_FLAGS_ENCODED, NULL);
    const char *host = uri ? g_uri_get_host(uri) : NULL;
#endif
    if (host) {
        CogRequestHandler *handler = g_hash_table_lookup(self->route, host);
        if (handler)
            return cog_request_handler_run(handler, request);
    }

    cog_host_routes_handler_run_fallback(self, request);
}

static void
cog_host_routes_handler_iface_init(CogRequestHandlerInterface *iface)
{
    iface->run = cog_host_routes_handler_run;
}

G_DEFINE_TYPE_WITH_CODE(CogHostRoutesHandler,
                        cog_host_routes_handler,
                        G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(COG_TYPE_REQUEST_HANDLER, cog_host_routes_handler_iface_init))

static void
cog_host_routes_handler_get_property(GObject *object, unsigned prop_id, GValue *value, GParamSpec *pspec)
{
    CogHostRoutesHandler *self = COG_HOST_ROUTES_HANDLER(object);
    switch (prop_id) {
    case PROP_FALLBACK_HANDLER:
        g_value_set_object(value, self->fallback);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    }
}

static void
cog_host_routes_handler_set_property(GObject *object, unsigned prop_id, const GValue *value, GParamSpec *pspec)
{
    CogHostRoutesHandler *self = COG_HOST_ROUTES_HANDLER(object);
    switch (prop_id) {
    case PROP_FALLBACK_HANDLER:
        g_clear_object(&self->fallback);
        self->fallback = g_value_dup_object(value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    }
}

static void
cog_host_routes_handler_dispose(GObject *object)
{
    CogHostRoutesHandler *self = COG_HOST_ROUTES_HANDLER(object);

    g_hash_table_remove_all(self->route);
    g_clear_pointer(&self->route, g_hash_table_unref);

    g_clear_object(&self->fallback);

    G_OBJECT_CLASS(cog_host_routes_handler_parent_class)->dispose(object);
}

static void
cog_host_routes_handler_class_init(CogHostRoutesHandlerClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->get_property = cog_host_routes_handler_get_property;
    object_class->set_property = cog_host_routes_handler_set_property;
    object_class->dispose = cog_host_routes_handler_dispose;

    /**
     * CogHostRoutesHandler:fallback-handler:
     *
     * Optional #CogRequestHandler used as fallback when no route entries match
     * the URI host.
     */
    s_properties[PROP_FALLBACK_HANDLER] =
        g_param_spec_object("fallback-handler",
                            "Fallback handler",
                            "Handler used as fallback for unhandled route entries",
                            COG_TYPE_REQUEST_HANDLER,
                            G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties(object_class, N_PROPERTIES, s_properties);
}

static void
cog_host_routes_handler_init(CogHostRoutesHandler *self)
{
    self->route = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
}

/**
 * cog_host_routes_handler_new: (constructor)
 * @fallback_handler: (nullable) (transfer none): a #CogRequestHandler
 *    to use as fallback
 *
 * Create a new handler.
 *
 * Returns: (transfer full): A handler with no route entries configured.
 */
CogRequestHandler *
cog_host_routes_handler_new(CogRequestHandler *fallback_handler)
{
    return g_object_new(COG_TYPE_HOST_ROUTES_HANDLER, "fallback-handler", fallback_handler, NULL);
}

/**
 * cog_host_routes_handler_contains:
 * @self: a #CogHostRoutesHandler
 * @host: URI host to match
 *
 * Check if there is already added a route for the @host.
 *
 * Returns: Whether the route already exists.
 */
gboolean
cog_host_routes_handler_contains(CogHostRoutesHandler *self, const char *host)
{
    g_return_val_if_fail(COG_IS_HOST_ROUTES_HANDLER(self), FALSE);
    g_return_val_if_fail(host != NULL, FALSE);

    if (g_hash_table_contains(self->route, host))
        return TRUE;

    return FALSE;
}

/**
 * cog_host_routes_handler_add:
 * @self: a #CogHostRoutesHandler
 * @host: URI host to match
 * @handler: (transfer none): a request handler for the matched host
 *
 * Adds a route to the handler.
 *
 * Configures a route which matches @host in URI, and dispatches
 * requests to a given @handler.
 *
 * Returns: Whether the route was successfully added.
 */
gboolean
cog_host_routes_handler_add(CogHostRoutesHandler *self, const char *host, CogRequestHandler *handler)
{
    g_return_val_if_fail(COG_IS_HOST_ROUTES_HANDLER(self), FALSE);
    g_return_val_if_fail(host != NULL, FALSE);
    g_return_val_if_fail(COG_IS_REQUEST_HANDLER(handler), FALSE);

    if (g_hash_table_contains(self->route, host))
        return FALSE;

    g_hash_table_insert(self->route, g_strdup(host), g_object_ref(handler));
    return TRUE;
}

/**
 * cog_host_routes_handler_remove:
 * @self: a #CogHostRoutesHandler
 * @host: URIhost for a configured route
 *
 * Removes a previously configured route.
 *
 * Removes a route that matches @host previously configured
 * using cog_host_routes_handler_add() or
 * cog_host_routes_handler_add_path().
 *
 * Returns: Whether the route was found and removed.
 */
gboolean
cog_host_routes_handler_remove(CogHostRoutesHandler *self, const char *host)
{
    g_return_val_if_fail(COG_IS_HOST_ROUTES_HANDLER(self), FALSE);
    g_return_val_if_fail(host != NULL, FALSE);

    return g_hash_table_remove(self->route, host);
}

/**
 * cog_host_routes_handler_add_path:
 * @self: a #CogHostRoutesHandler
 * @host: URI path host to match
 * @base_path: Path to a local directory
 *
 * Adds a route to the handler pointing to a directory.
 *
 * This is a convenience method which configures a route
 * matching @host in URI, and creates a new
 * [class@Cog.DirectoryFilesHandler] for @base_path to handle requests
 * for the route.
 *
 * Returns: Whether the route was successfully added.
 */
gboolean
cog_host_routes_handler_add_path(CogHostRoutesHandler *self, const char *host, const char *base_path)
{
    g_return_val_if_fail(COG_IS_HOST_ROUTES_HANDLER(self), FALSE);
    g_return_val_if_fail(host != NULL, FALSE);
    g_return_val_if_fail(base_path != NULL, FALSE);

    g_autoptr(GFile) path_file = g_file_new_for_path(base_path);
    g_return_val_if_fail(cog_directory_files_handler_is_suitable_path(path_file, NULL), FALSE);

    g_autoptr(CogRequestHandler) handler = cog_directory_files_handler_new(path_file);
    return cog_host_routes_handler_add(self, host, handler);
}
