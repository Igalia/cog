/*
 * cog-view.c
 * Copyright (C) 2023 Igalia S.L.
 *
 * SPDX-License-Identifier: MIT
 */

#include "cog-view.h"
#include "cog-platform.h"

/**
 * CogView:
 *
 * Convenience base class for web views.
 *
 * Most of the functionality dealing with web views when using the
 * Cog core library uses #CogView instead of [class@WPEWebKit.WebView].
 * The base class is extended to delegate the creation of its
 * [struct@WPEWebKit.WebViewBackend] to the current [class@CogPlatform]
 * implementation. Optionally, platform plug-in implementations can
 * provide their own view class implementation by overriding
 * [vfunc@CogPlatform.get_view_type].
 *
 * A number of utility functions are also provided.
 */

G_DEFINE_ABSTRACT_TYPE(CogView, cog_view, WEBKIT_TYPE_WEB_VIEW)

struct _CogCoreViewClass {
    CogViewClass parent_class;
};

G_DECLARE_DERIVABLE_TYPE(CogCoreView, cog_core_view, COG, CORE_VIEW, CogView)
G_DEFINE_TYPE(CogCoreView, cog_core_view, COG_TYPE_VIEW)

static GObject *
cog_view_constructor(GType type, unsigned n_properties, GObjectConstructParam *properties)
{
    GObject *object = G_OBJECT_CLASS(cog_view_parent_class)->constructor(type, n_properties, properties);

    CogViewClass *view_class = COG_VIEW_GET_CLASS(object);
    if (view_class->create_backend) {
        WebKitWebViewBackend *backend = (*view_class->create_backend)((CogView *) object);

        GValue backend_value = G_VALUE_INIT;
        g_value_take_boxed(g_value_init(&backend_value, WEBKIT_TYPE_WEB_VIEW_BACKEND), backend);
        g_object_set_property(object, "backend", &backend_value);
    } else {
        g_error("Type %s did not define the create_backend vfunc", g_type_name(type));
    }

    return object;
}

static void
cog_view_class_init(CogViewClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->constructor = cog_view_constructor;
}

static void
cog_view_init(CogView *self)
{
}

/**
 * cog_view_get_backend:
 * @self: A view.
 *
 * Get the backend for the view.
 *
 * Returns: (transfer none) (not nullable): The WPE view backend for the view.
 */
struct wpe_view_backend *
cog_view_get_backend(CogView *self)
{
    g_return_val_if_fail(COG_IS_VIEW(self), NULL);

    WebKitWebViewBackend *backend = webkit_web_view_get_backend(WEBKIT_WEB_VIEW(self));
    return webkit_web_view_backend_get_wpe_backend(backend);
}

static void *
_cog_view_get_impl_type_init(GType *type)
{
    *type = cog_core_view_get_type();

    CogPlatform *platform = cog_platform_get_default();
    if (platform) {
        CogPlatformClass *platform_class = COG_PLATFORM_GET_CLASS(platform);
        if (platform_class->get_view_type) {
            GType view_type = (*platform_class->get_view_type)(platform);
            *type = view_type;
        }
    }

    g_debug("%s: using %s", G_STRFUNC, g_type_name(*type));
    return NULL;
}

/**
 * cog_view_get_impl_type:
 *
 * Get the type of the web view implementation in use.
 *
 * This function always returns a valid type. If the If the active
 * [class@CogPlatform] does not provide a custom view type, the default
 * built-in type included as part of the Cog core library is returned.
 *
 * When creating web views, this function must be used to retrieve the
 * type to use, e.g.:
 *
 * ```c
 * WebKitWebView *view = g_object_new(cog_view_get_impl_type(), NULL);
 * ```
 *
 * In most cases it should be possible to use the convenience function
 * [ctor@View.new], which uses this function internally.
 *
 * Returns: Type of the view implementation in use.
 */
GType
cog_view_get_impl_type(void)
{
    static GType view_type;
    static GOnce once = G_ONCE_INIT;

    g_once(&once, (GThreadFunc) _cog_view_get_impl_type_init, &view_type);

    return view_type;
}

/**
 * cog_view_new: (constructor)
 * @first_property_name: Name of the first property.
 * @...: Value of the first property, followed optionally by more name/value
 *    pairs, followed by %NULL.
 *
 * Creates a new instance of a view implementation and sets its properties.
 *
 * Returns: (transfer full): A new web view.
 */
CogView *
cog_view_new(const char *first_property_name, ...)
{
    va_list args;
    va_start(args, first_property_name);
    void *view = g_object_new_valist(cog_view_get_impl_type(), first_property_name, args);
    va_end(args);
    return view;
}

static WebKitWebViewBackend *
cog_core_view_create_backend(CogView *view G_GNUC_UNUSED)
{
    g_autoptr(GError) error = NULL;

    WebKitWebViewBackend *backend = cog_platform_get_view_backend(cog_platform_get_default(), NULL, &error);
    if (!backend)
        g_error("%s: Could not create view backend, %s", G_STRFUNC, error->message);

    g_debug("%s: backend %p", G_STRFUNC, backend);
    return backend;
}

static void
cog_core_view_class_init(CogCoreViewClass *klass)
{
    CogViewClass *view_class = COG_VIEW_CLASS(klass);
    view_class->create_backend = cog_core_view_create_backend;
}

static void
cog_core_view_init(CogCoreView *self)
{
}
