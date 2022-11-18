/*
 * cog-directory-files-handler.c
 * Copyright (C) 2017-2018 Adrian Perez <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#include "cog-directory-files-handler.h"
#include <gio/gio.h>

/**
 * CogDirectoryFilesHandler:
 *
 * Request handler implementation that loads content from files inside
 * a local directory.
 *
 * By default only the path component of requested URIs is taken into
 * account, and setting [property@Cog.DirectoryFilesHandler:use-host].
 * also uses the URI host component. If a resolved path points to a
 * local directory and it contains a file named `index.html`, it will
 * be used as the response.
 */

struct _CogDirectoryFilesHandler {
    GObject  parent;
    GFile   *base_path;
    gboolean use_host;
    unsigned strip_components;
};

enum {
    PROP_0,
    PROP_BASE_PATH,
    PROP_USE_HOST,
    PROP_STRIP_COMPONENTS,
    N_PROPERTIES,
};

static GParamSpec *s_properties[N_PROPERTIES] = { NULL, };

static const char s_file_query_attributes[] =
    G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE ","
    G_FILE_ATTRIBUTE_STANDARD_SIZE ","
    G_FILE_ATTRIBUTE_STANDARD_TYPE;

static void* s_request_resolving_index = (void*) 0xCAFECAFE;

G_DEFINE_QUARK (cog-directory-files-handler-data, cog_directory_files_handler);


static void
on_file_read_async_completed (GObject      *source_object,
                              GAsyncResult *result,
                              void         *user_data)
{
    g_autoptr(GFile) file = G_FILE (g_object_ref (source_object));
    g_autoptr(WebKitURISchemeRequest) request = WEBKIT_URI_SCHEME_REQUEST (user_data);

    g_autoptr(GError) error = NULL;
    g_autoptr(GFileInputStream) file_stream =
        g_file_read_finish (file, result, &error);

    if (file_stream) {
        g_autoptr(GInputStream) stream =
            g_buffered_input_stream_new (G_INPUT_STREAM (file_stream));
        g_autoptr(GFileInfo) info =
            G_FILE_INFO (g_object_steal_qdata (source_object, cog_directory_files_handler_quark ()));
        guint64 size =
            g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_STANDARD_SIZE);
        const char *mime_type =
            g_file_info_get_attribute_string (info, G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE);
        webkit_uri_scheme_request_finish (request, stream, size, mime_type);
    } else {
        /*
         * TODO: Generate a nicer error page.
         */
        g_assert (error);
        webkit_uri_scheme_request_finish_error (request, error);
    }
}


static void
on_file_query_info_async_completed (GObject      *source_object,
                                    GAsyncResult *result,
                                    void         *user_data)
{
    g_autoptr(GFile) file = G_FILE (g_object_ref (source_object));
    g_autoptr(WebKitURISchemeRequest) request = WEBKIT_URI_SCHEME_REQUEST (user_data);

    g_autoptr(GError) error = NULL;
    g_autoptr(GFileInfo) info = g_file_query_info_finish (file, result, &error);

    if (!info) {
        g_assert (error);
        webkit_uri_scheme_request_finish_error (request, error);
        return;
    }

    GFileType type =
        g_file_info_get_attribute_uint32 (info, G_FILE_ATTRIBUTE_STANDARD_TYPE);

    if (type == G_FILE_TYPE_REGULAR) {
        /*
         * The current file information will be retrieved by 
         * on_file_read_async_completed() to avoid querying again.
         */
        g_object_set_qdata_full (source_object,
                                 cog_directory_files_handler_quark (),
                                 g_steal_pointer (&info),
                                 g_object_unref);
        g_file_read_async (file,
                           G_PRIORITY_DEFAULT,
                           NULL,
                           on_file_read_async_completed,
                           g_steal_pointer (&request));
    } else if (type == G_FILE_TYPE_DIRECTORY) {
        /*
         * If the request has been marked, it means this function is being
         * called after having previously found a directory. In that case,
         * do not try to resolve "index.html" a second time and produce
         * an error instead.
         */
        if (s_request_resolving_index == g_object_get_qdata (G_OBJECT (request),
                                                             cog_directory_files_handler_quark ()))
        {
            g_autofree char *path = g_file_get_path (file);
            error = g_error_new (cog_directory_files_handler_error_quark (),
                                 COG_DIRECTORY_FILES_HANDLER_ERROR_CANNOT_RESOLVE,
                                 "Path '%s' does not represent a regular file",
                                 path);
            webkit_uri_scheme_request_finish_error (request, error);
        } else {
            /* Mark request as being resolved for its index. */
            g_object_set_qdata (G_OBJECT (request),
                                cog_directory_files_handler_quark (),
                                s_request_resolving_index);
            g_autoptr(GFile) index = g_file_get_child (file, "index.html");
            g_file_query_info_async (index,
                                     s_file_query_attributes,
                                     G_FILE_QUERY_INFO_NONE,
                                     G_PRIORITY_DEFAULT,
                                     NULL,
                                     on_file_query_info_async_completed,
                                     g_steal_pointer (&request));
        }
    } else {
        g_autofree char *path = g_file_get_path (file);
        error = g_error_new (cog_directory_files_handler_error_quark (),
                             COG_DIRECTORY_FILES_HANDLER_ERROR_CANNOT_RESOLVE,
                             "Path '%s' does not represent a regular file or directory",
                             path);
        webkit_uri_scheme_request_finish_error (request, error);
    }
}


static void
cog_directory_files_handler_run (CogRequestHandler      *request_handler,
                                 WebKitURISchemeRequest *request)
{
    CogDirectoryFilesHandler *handler = COG_DIRECTORY_FILES_HANDLER(request_handler);

#if COG_USE_SOUP2
    g_autoptr(SoupURI) uri = soup_uri_new(webkit_uri_scheme_request_get_uri(request));
#else
    g_autoptr(GUri) uri = g_uri_parse(webkit_uri_scheme_request_get_uri(request), SOUP_HTTP_URI_FLAGS, NULL);
#endif

    /*
     * If we get an empty path, redirect to the root resource "/", otherwise
     * subresources cannot load properly as there would be no base URI.
     */
#if COG_USE_SOUP2
    const char *path = soup_uri_get_path(uri);
#else
    const char *path = g_uri_get_path(uri);
#endif
    if (path[0] != '/') {
#if COG_USE_SOUP2
        soup_uri_set_path(uri, "/");
        g_autofree char *uri_string = soup_uri_to_string(uri, FALSE);
#else
        g_autoptr(GUri) new_uri = soup_uri_copy(uri, SOUP_URI_PATH, "/", SOUP_URI_NONE);
        g_autofree char *uri_string = g_uri_to_string(new_uri);
#endif
        webkit_web_view_load_uri(webkit_uri_scheme_request_get_web_view(request), uri_string);
        return;
    }

    g_autoptr(GFile) base_path = NULL;
    if (handler->use_host) {
#if COG_USE_SOUP2
        const char *host = soup_uri_get_host(uri);
#else
        const char *host = g_uri_get_host(uri);
#endif
        if (!host || host[0] == '\0') {
#if COG_USE_SOUP2
            g_autofree char *uri_string = soup_uri_to_string(uri, FALSE);
#else
            g_autofree char *uri_string = g_uri_to_string(uri);
#endif
            g_autoptr(GError) error = g_error_new(G_FILE_ERROR, G_FILE_ERROR_INVAL, "No host in URI: %s", uri_string);
            webkit_uri_scheme_request_finish_error (request, error);
            return;
        }
        base_path = g_file_get_child (handler->base_path, host);
    } else {
        base_path = g_object_ref (handler->base_path);
    }

    /*
     * Skip trailing forward slashes: If an absolute child path would
     * be passed to g_file_get_child() it will ignore the base path.
     */
    while (path[0] == '/')
        ++path;

    /*
     * Discard non-empty leading path components, ignoring empty path
     * components (empty component == two consecutive slashes). For
     * example with "strip-components" being 2, paths would get munged
     * as follows:
     *
     *   - /foo/bar/baz  -> /baz
     *   - /foo//bar/baz -> /baz
     *   - /foo/bar/     -> /
     *   - /foo/bar      -> (empty)
     *   - /foo          -> (empty)
     */
    for (unsigned i = 0; i < handler->strip_components && path[0] != '\0'; i++) {
        /* Skip up to the next slash. */
        while (path[0] != '/' && path[0] != '\0')
            ++path;
        /* Skip consecutive slashes. */
        while (path[0] == '/')
            ++path;
    }

    g_autoptr(GFile) file = g_file_get_child (base_path, path);

    /*
     * Check whether the resolved path is contained inside base_path,
     * to prevent URIs with ".." components from accessing resources
     * outside of base_path. The g_file_get_relative_path() method
     * returns NULL when the file path is NOT descendant of base_path,
     * or when both paths are the same.
     */
    if (!g_file_equal (base_path, file)) {
        g_autofree char *relative_path = g_file_get_relative_path (base_path, file);
        if (!relative_path) {
            g_autoptr(GError) error = g_error_new (G_FILE_ERROR,
                                                   G_FILE_ERROR_PERM,
                                                   "Resolved path '%s' not "
                                                   "contained in base path '%s'",
                                                   g_file_peek_path (file),
                                                   g_file_peek_path (base_path));
            return webkit_uri_scheme_request_finish_error (request, error);
        }
    }

    g_file_query_info_async (file,
                             s_file_query_attributes,
                             G_FILE_QUERY_INFO_NONE,
                             G_PRIORITY_DEFAULT,
                             NULL,
                             on_file_query_info_async_completed,
                             g_object_ref (request));
}

static void
cog_directory_files_handler_iface_init (CogRequestHandlerInterface *iface)
{
    iface->run = cog_directory_files_handler_run;
}


G_DEFINE_TYPE_WITH_CODE (CogDirectoryFilesHandler,
                         cog_directory_files_handler,
                         G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (COG_TYPE_REQUEST_HANDLER,
                                                cog_directory_files_handler_iface_init))


static void
cog_directory_files_handler_get_property (GObject    *object,
                                          unsigned    prop_id,
                                          GValue     *value,
                                          GParamSpec *pspec)
{
    CogDirectoryFilesHandler *handler = COG_DIRECTORY_FILES_HANDLER (object);
    switch (prop_id) {
        case PROP_BASE_PATH:
            g_value_set_object (value, handler->base_path);
            break;
        case PROP_USE_HOST:
            g_value_set_boolean (value, cog_directory_files_handler_get_use_host (handler));
            break;
        case PROP_STRIP_COMPONENTS:
            g_value_set_uint (value, cog_directory_files_handler_get_strip_components (handler));
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}


static void
cog_directory_files_handler_set_property (GObject      *object,
                                          unsigned      prop_id,
                                          const GValue *value,
                                          GParamSpec   *pspec)
{
    CogDirectoryFilesHandler *handler = COG_DIRECTORY_FILES_HANDLER (object);
    switch (prop_id) {
        case PROP_BASE_PATH:
            g_clear_object (&handler->base_path);
            handler->base_path = g_value_dup_object (value);
            break;
        case PROP_USE_HOST:
            cog_directory_files_handler_set_use_host (handler,
                                                      g_value_get_boolean (value));
            break;
        case PROP_STRIP_COMPONENTS:
            cog_directory_files_handler_set_strip_components (handler,
                                                              g_value_get_uint (value));
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}


static void
cog_directory_files_handler_constructed (GObject *object)
{
    G_OBJECT_CLASS (cog_directory_files_handler_parent_class)->constructed (object);

    CogDirectoryFilesHandler *handler = COG_DIRECTORY_FILES_HANDLER (object);

    g_return_if_fail (cog_directory_files_handler_is_suitable_path (handler->base_path, NULL));
}


static void
cog_directory_files_handler_dispose (GObject *object)
{
    CogDirectoryFilesHandler *handler = COG_DIRECTORY_FILES_HANDLER (object);

    g_clear_object (&handler->base_path);

    G_OBJECT_CLASS (cog_directory_files_handler_parent_class)->dispose (object);
}


static void
cog_directory_files_handler_class_init (CogDirectoryFilesHandlerClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    object_class->get_property = cog_directory_files_handler_get_property;
    object_class->set_property = cog_directory_files_handler_set_property;
    object_class->constructed = cog_directory_files_handler_constructed;
    object_class->dispose = cog_directory_files_handler_dispose;

    /**
     * CogDirectoryFilesHandler:base-path:
     *
     * Base directory where to search for files to “serve” as resources.
     */
    s_properties[PROP_BASE_PATH] =
        g_param_spec_object ("base-path",
                             "Base path",
                             "Path where to load files from",
                             G_TYPE_FILE,
                             G_PARAM_READWRITE |
                             G_PARAM_CONSTRUCT_ONLY |
                             G_PARAM_STATIC_STRINGS);

    /**
     * CogDirectoryFilesHandler:use-host: (attributes org.gtk.Property.get=cog_directory_files_handler_get_use_host org.gtk.Property.set=cog_directory_files_handler_set_use_host):
     *
     * Whether to use the URI host component as the first subdirectory name.
     *
     * For example, given a request for the URI `scheme://host/some/path`:
     *
     * - When enabled, a subdirectory named `host/some/path` will
     *   be searched for inside the `base-path`.
     * - When disabled, a subdirectory named `some/path` will be
     *   searched for inside the `base-path`.
     */
    s_properties[PROP_USE_HOST] =
        g_param_spec_boolean ("use-host",
                              "Use URI host",
                              "Use the host from the URI authority part as top-level subdirectory",
                              FALSE,
                              G_PARAM_READWRITE |
                              G_PARAM_STATIC_STRINGS);

    /**
     * CogDirectoryFilesHandler:strip-components: (attributes org.gtk.Property.get=cog_directory_files_handler_get_strip_components org.gtk.Property.set=cog_directory_files_handler_set_strip_components):
     *
     * Number of leading path components to strip (ignore) at the beginning
     * of request URIs.
     *
     * For example, when set to `2`, a  request for a URI with path
     * `/a/b/c/d.html` will ignore the `/a/b` prefix and search for `c/d.html`.
     */
    s_properties[PROP_STRIP_COMPONENTS] =
        g_param_spec_uint ("strip-components",
                           "Strip path components",
                           "Number of leading URI path components to ignore",
                           0, G_MAXUINT, 0,
                           G_PARAM_READWRITE |
                           G_PARAM_CONSTRUCT |
                           G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPERTIES, s_properties);
}


static void
cog_directory_files_handler_init (CogDirectoryFilesHandler *handler)
{
}


G_DEFINE_QUARK (CogDirectoryFilesHandlerError, cog_directory_files_handler_error)


gboolean
cog_directory_files_handler_is_suitable_path (GFile   *file,
                                              GError **error)
{
    g_return_val_if_fail (G_IS_FILE (file), FALSE);

    if (!g_file_is_native (file)) {
        g_autofree char *path = error ? g_file_get_path (file) : NULL;
        g_set_error (error,
                     cog_directory_files_handler_error_quark (),
                     COG_DIRECTORY_FILES_HANDLER_ERROR_PATH_NOT_NATIVE,
                     "Path '%s' is not native",
                     path);
        return FALSE;
    }

    if (g_file_query_file_type (file, G_FILE_QUERY_INFO_NONE, NULL) != G_FILE_TYPE_DIRECTORY) {
        g_autofree char *path = error ? g_file_get_path (file) : NULL;
        g_set_error (error,
                     cog_directory_files_handler_error_quark (),
                     COG_DIRECTORY_FILES_HANDLER_ERROR_PATH_NOT_DIRECTORY,
                     "Path '%s' is not a directory",
                     path);
        return FALSE;
    }

    return TRUE;
}

/**
 * cog_directory_files_handler_new: (constructor)
 * @base_path: Base directory where to search for files.
 *
 * Create a new handler.
 *
 * Returns: (transfer full): A directory files handler.
 */
CogRequestHandler*
cog_directory_files_handler_new (GFile *base_path)
{
    g_return_val_if_fail (cog_directory_files_handler_is_suitable_path (base_path, NULL), NULL);
    return g_object_new (COG_TYPE_DIRECTORY_FILES_HANDLER,
                         "base-path", base_path,
                         NULL);
}

/**
 * cog_directory_files_handler_get_use_host:
 *
 * Gets the value of the [property@Cog.DirectoryFilesHandler:use-host]
 * property.
 *
 * Returns: Whether to use the URI host part as a subdirectory name.
 */
gboolean
cog_directory_files_handler_get_use_host (CogDirectoryFilesHandler *self)
{
    g_return_val_if_fail (COG_IS_DIRECTORY_FILES_HANDLER (self), FALSE);
    return self->use_host;
}

/**
 * cog_directory_files_handler_set_use_host:
 * @use_host: Whether to use the URI host part as a subdirectory name.
 *
 * Sets the value of the [property@Cog.DirectoryFilesHandler:use-host]
 * property.
 */
void
cog_directory_files_handler_set_use_host (CogDirectoryFilesHandler *self,
                                          gboolean                  use_host)
{
    g_return_if_fail (COG_IS_DIRECTORY_FILES_HANDLER (self));

    use_host = use_host ? TRUE : FALSE;
    if (self->use_host == use_host)
        return;

    self->use_host = use_host;
    g_object_notify_by_pspec (G_OBJECT (self), s_properties[PROP_USE_HOST]);
}

/**
 * cog_directory_files_handler_get_strip_components:
 * @self: a #CogDirectoryFilesHandler
 *
 * Gets the value of the [property@Cog.DirectoryFilesHandler:strip-components]
 * property.
 *
 * Returns: Number of leading URI path components to ignore.
 */
unsigned
cog_directory_files_handler_get_strip_components (CogDirectoryFilesHandler *self)
{
    g_return_val_if_fail (COG_IS_DIRECTORY_FILES_HANDLER (self), 0);
    return self->strip_components;
}

/**
 * cog_directory_files_handler_set_strip_components:
 * @self: a #CogDirectoryFilesHandler
 * @count: Number of leading URI path components to ignore.
 *
 * Sets the value of the [property@Cog.DirectoryFilesHandler:strip-components]
 * property.
 */
void
cog_directory_files_handler_set_strip_components (CogDirectoryFilesHandler *self,
                                                  unsigned                  count)
{
    g_return_if_fail (COG_IS_DIRECTORY_FILES_HANDLER (self));

    if (self->strip_components == count)
        return;

    self->strip_components = count;
    g_object_notify_by_pspec (G_OBJECT (self), s_properties[PROP_STRIP_COMPONENTS]);
}
