/*
 * cog-prefix-routes-handler.c
 * Copyright (C) 2021 Igalia S.L.
 *
 * Distributed under terms of the MIT license.
 */

#include "cog-prefix-routes-handler.h"
#include "cog-directory-files-handler.h"

/**
 * CogPrefixRoutesHandler:
 *
 * Direct custom URI scheme requests to different handlers.
 *
 * Handler for custom URI scheme requests that can route requests to
 * different handlers depending on the prefix of the requested URI
 * *path* component.
 *
 * Optionally, if a “fallback” handler has been specified, it will be
 * used to serve requests which do not match any of the routed prefixes.
 *
 * The set of available prefix routes can be configured using
 * [method@Cog.PrefixRoutesHandler.mount] and
 * [method@Cog.PrefixRoutesHandler.unmount]. For each request, routes
 * are checked and the one that matches the most URI *path* components
 * will handle the request.
 *
 * This handler is typically used in tandem with
 * [class@Cog.DirectoryFilesHandler], the latter being typically a
 * fallback, or as the handler for a routed prefix.
 *
 * The following configures a handler to route requests with their path
 * starting with `/static/images` to a certain directory, and all the
 * rest of paths to another—note how the
 * [property@Cog.DirectoryFilesHandler:strip-components] property is
 * used to remove the matched path components for that route:
 *
 * ```c
 * GFile *content_dir = g_file_new_for_path ("data/content");
 * CogRequestHandler *fallback_handler = cog_directory_files_handler_new (fallback_dir);
 *
 * GFile *images_dir = g_file_new_for_path ("data/images");
 * CogRequestHandler *images_handler = g_object_new (COG_TYPE_DIRECTORY_FILES_HANDLER,
 *                                                   "base-path", images_dir,
 *                                                   "strip-components", 2,
 *                                                   NULL);
 *
 * CogRequestHandler *handler = cog_prefix_routes_handler_new (fallback_handler);
 * cog_prefix_routes_handler_mount (COG_PREFIX_ROUTES_HANDLER (handler),
 *                                  "/static/images",
 *                                  images_handler);
 * ```
 */

struct _CogPrefixRoutesHandler {
    GObject parent;

    CogRequestHandler *fallback;
    GHashTable        *routes;  /* string -> CogRequestHandler */
};

enum {
    PROP_0,
    PROP_FALLBACK_HANDLER,
    N_PROPERTIES,
};

static GParamSpec *s_properties[N_PROPERTIES] = { NULL, };



static void
cog_prefix_routes_handler_run_fallback (CogPrefixRoutesHandler *self,
                                        WebKitURISchemeRequest *request)
{
    if (self->fallback) {
        cog_request_handler_run (self->fallback, request);
    } else {
        g_autoptr(GError) error =
            g_error_new (G_FILE_ERROR,
                         G_FILE_ERROR_NOENT,
                         "No file for URI path: %s",
                         webkit_uri_scheme_request_get_path (request));
        webkit_uri_scheme_request_finish_error (request, error);
    }
}


static void
cog_prefix_routes_handler_run (CogRequestHandler      *request_handler,
                               WebKitURISchemeRequest *request)
{
    CogPrefixRoutesHandler *self = COG_PREFIX_ROUTES_HANDLER (request_handler);

    const char *uri_path = webkit_uri_scheme_request_get_path (request);
    if (!uri_path)
        return cog_prefix_routes_handler_run_fallback (self, request);

    /*
     * Try to find the longest path (up to a slash) for which there
     * is a route configured.
     */
    g_autoptr(GString) prefix = g_string_new (uri_path);

    while (prefix->len > 1) {
        /* Find the position of the latest slash. */
        size_t last_slash_pos = prefix->len - 1;
        while (prefix->str[last_slash_pos] != '/' && last_slash_pos > 0)
            --last_slash_pos;

        /* Position 0 is always the leading slash. */
        if (last_slash_pos == 0)
            break;

        g_string_erase (prefix, last_slash_pos, -1);

        CogRequestHandler *handler =
            g_hash_table_lookup (self->routes, prefix->str);
        if (handler)
            return cog_request_handler_run (handler, request);
    }

    cog_prefix_routes_handler_run_fallback (self, request);
}


static void
cog_prefix_routes_handler_iface_init (CogRequestHandlerInterface *iface)
{
    iface->run = cog_prefix_routes_handler_run;
}


G_DEFINE_TYPE_WITH_CODE (CogPrefixRoutesHandler,
                         cog_prefix_routes_handler,
                         G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (COG_TYPE_REQUEST_HANDLER,
                                                cog_prefix_routes_handler_iface_init))


static void
cog_prefix_routes_handler_get_property (GObject    *object,
                                        unsigned    prop_id,
                                        GValue     *value,
                                        GParamSpec *pspec)
{
    CogPrefixRoutesHandler *self = COG_PREFIX_ROUTES_HANDLER (object);
    switch (prop_id) {
        case PROP_FALLBACK_HANDLER:
            g_value_set_object (value, self->fallback);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}


static void
cog_prefix_routes_handler_set_property (GObject      *object,
                                        unsigned      prop_id,
                                        const GValue *value,
                                        GParamSpec   *pspec)
{
    CogPrefixRoutesHandler *self = COG_PREFIX_ROUTES_HANDLER (object);
    switch (prop_id) {
        case PROP_FALLBACK_HANDLER:
            g_clear_object (&self->fallback);
            self->fallback = g_value_dup_object (value);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}


static void
cog_prefix_routes_handler_dispose (GObject *object)
{
    CogPrefixRoutesHandler *self = COG_PREFIX_ROUTES_HANDLER (object);

    g_hash_table_remove_all (self->routes);
    g_clear_pointer (&self->routes, g_hash_table_unref);

    g_clear_object (&self->fallback);

    G_OBJECT_CLASS (cog_prefix_routes_handler_parent_class)->dispose (object);
}


static void
cog_prefix_routes_handler_class_init (CogPrefixRoutesHandlerClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    object_class->get_property = cog_prefix_routes_handler_get_property;
    object_class->set_property = cog_prefix_routes_handler_set_property;
    object_class->dispose = cog_prefix_routes_handler_dispose;

    /**
     * CogPrefixRoutesHandler:fallback-handler:
     *
     * Optional #CogRequestHandler used as fallback when no routes match
     * the URI path.
     */
    s_properties[PROP_FALLBACK_HANDLER] =
        g_param_spec_object ("fallback-handler",
                             "Fallback handler",
                             "Handler used as fallback for unhandled routes",
                             COG_TYPE_REQUEST_HANDLER,
                             G_PARAM_READWRITE |
                             G_PARAM_CONSTRUCT_ONLY |
                             G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPERTIES, s_properties);
}


static void
cog_prefix_routes_handler_init (CogPrefixRoutesHandler *self)
{
    self->routes = g_hash_table_new_full (g_str_hash,
                                          g_str_equal,
                                          g_free,
                                          g_object_unref);
}

/**
 * cog_prefix_routes_handler_new: (constructor)
 * @fallback_handler: (nullable) (transfer none): a #CogRequestHandler
 *    to use as fallback
 *
 * Create a new handler.
 *
 * Returns: (transfer full): A handler with no routes configured.
 */
CogRequestHandler*
cog_prefix_routes_handler_new (CogRequestHandler *fallback_handler)
{
    return g_object_new (COG_TYPE_PREFIX_ROUTES_HANDLER,
                         "fallback-handler", fallback_handler,
                         NULL);
}

/**
 * cog_prefix_routes_handler_mount:
 * @self: a #CogPrefixRoutesHandler
 * @path_prefix: URI path prefix to match
 * @handler: (transfer none): a request handler for the matched prefix
 *
 * Adds a route to the handler.
 *
 * Configures a route which matches @path_prefix in URI paths, and dispatches
 * requests to a given @handler. The @path_prefix must contain a leading
 * slash character (`/`).
 *
 * Returns: Whether the route was successfully added.
 */
gboolean
cog_prefix_routes_handler_mount (CogPrefixRoutesHandler *self,
                                 const char             *path_prefix,
                                 CogRequestHandler      *handler)
{
    g_return_val_if_fail (COG_IS_PREFIX_ROUTES_HANDLER (self), FALSE);
    g_return_val_if_fail (path_prefix != NULL, FALSE);
    g_return_val_if_fail (path_prefix[0] == '/', FALSE);
    g_return_val_if_fail (COG_IS_REQUEST_HANDLER (handler), FALSE);

    if (g_hash_table_contains (self->routes, path_prefix))
        return FALSE;

    g_hash_table_insert (self->routes,
                         g_strdup (path_prefix),
                         g_object_ref (handler));
    return TRUE;
}

/**
 * cog_prefix_routes_handler_unmount:
 * @self: a #CogPrefixRoutesHandler
 * @path_prefix: URI path prefix for a configured route
 *
 * Removes a previously configured route.
 *
 * Removes a route that matches @path_prefix previously configured
 * using cog_prefix_routes_handler_mount() or
 * cog_prefix_routes_handler_mount_path().
 *
 * Returns: Whether the route was found and removed.
 */
gboolean
cog_prefix_routes_handler_unmount (CogPrefixRoutesHandler *self,
                                   const char             *path_prefix)
{
    g_return_val_if_fail (COG_IS_PREFIX_ROUTES_HANDLER (self), FALSE);
    g_return_val_if_fail (path_prefix != NULL, FALSE);

    return g_hash_table_remove (self->routes, path_prefix);
}

/**
 * cog_prefix_routes_handler_mount_path:
 * @self: a #CogPrefixRoutesHandler
 * @path_prefix: URI path prefix to match
 * @base_path: Path to a local directory
 *
 * Adds a route to the handler pointing to a directory.
 *
 * This is a convenience method which convenienceonfigures a route
 * matching @path_prefix in URI paths, and creates a new
 * [class@Cog.DirectoryFilesHandler] for @base_path to handle requests
 * for the route. The @path_prefix must contain a leading
 * slash character (`/`).
 *
 * Returns: Whether the route was successfully added.
 */
gboolean
cog_prefix_routes_handler_mount_path (CogPrefixRoutesHandler *self,
                                      const char             *path_prefix,
                                      const char             *base_path)
{
    g_return_val_if_fail (COG_IS_PREFIX_ROUTES_HANDLER (self), FALSE);
    g_return_val_if_fail (path_prefix != NULL, FALSE);
    g_return_val_if_fail (base_path != NULL, FALSE);

    g_autoptr(GFile) path_file = g_file_new_for_path (base_path);
    g_return_val_if_fail (cog_directory_files_handler_is_suitable_path (path_file, NULL), FALSE);

    unsigned num_components = 0;
    for (size_t i = 0; path_prefix[i] != '\0'; i++)
        if (path_prefix[i] == '/')
            num_components++;

    g_autoptr(CogRequestHandler) handler =
        g_object_new (COG_TYPE_DIRECTORY_FILES_HANDLER,
                      "base-path", path_file,
                      "strip-components", num_components);

    return cog_prefix_routes_handler_mount (self,
                                            path_prefix,
                                            handler);
}
