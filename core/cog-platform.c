/*
 * cog-platform.c
 * Copyright (C) 2018 Adrian Perez <aperez@igalia.com>
 * Copyright (C) 2018 Eduardo Lima <elima@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#include "cog-platform.h"
#include <gmodule.h>


G_DEFINE_AUTOPTR_CLEANUP_FUNC (GModule, g_module_close)


G_DEFINE_QUARK (COG_PLATFORM_EGL_ERROR, cog_platform_egl_error)
G_DEFINE_QUARK (COG_PLATFORM_ERROR, cog_platform_error)


G_DEFINE_INTERFACE (CogPlatformView, cog_platform_view, G_TYPE_OBJECT)

static void
cog_platform_view_default_init (CogPlatformViewInterface *iface G_GNUC_UNUSED)
{
}


/* @FIXME: Move this implementation to use a GIO extension point. */

G_DEFINE_INTERFACE (CogPlatform, cog_platform, G_TYPE_OBJECT)


static gboolean
cog_platform_default_setup (CogPlatform *platform,
                            CogShell    *shell G_GNUC_UNUSED,
                            const char  *params G_GNUC_UNUSED,
                            GError     **error G_GNUC_UNUSED)
{
    g_return_val_if_fail (G_IS_OBJECT (platform), FALSE);

    g_warning ("Class %s does not override the CogPlatform.setup() method."
               " This is usually a programming error.",
               G_OBJECT_CLASS_NAME (G_OBJECT_GET_CLASS (platform)));
}

static void
cog_platform_default_teardown (CogPlatform *platform)
{
    g_return_if_fail (G_IS_OBJECT (platform));

    g_warning ("Class %s does not override the CogPlatform.setup() method."
               " This is usually a programming error.",
               G_OBJECT_CLASS_NAME (G_OBJECT_GET_CLASS (platform)));
}

static CogPlatformView*
cog_platform_default_create_view (CogPlatform    *platform,
                                  WebKitWebView  *related_view G_GNUC_UNUSED,
                                  GError        **error G_GNUC_UNUSED)
{
    g_return_val_if_fail (G_IS_OBJECT (platform), NULL);

    g_critical ("Class %s does not override the CogPlatform.create_view() method.",
                G_OBJECT_CLASS_NAME (G_OBJECT_GET_CLASS (platform)));

    return NULL;
}


static void
cog_platform_default_init (CogPlatformInterface *iface)
{
    iface->setup = cog_platform_default_setup;
    iface->teardown = cog_platform_default_teardown;
    iface->create_view = cog_platform_default_create_view;
}


CogPlatform*
cog_platform_load (const gchar  *soname,
                   GError      **error)
{
    g_return_val_if_fail (soname != NULL, FALSE);

    g_autoptr(GModule) module = g_module_open (soname, G_MODULE_BIND_LAZY);
    if (module) {
        CogPlatform* (*create_platform) (GError**) = NULL;
        if (g_module_symbol (module,
                             "cog_platform_create_instance",
                             ((gpointer*) &create_platform))) {
            g_module_make_resident (module);
            return (*create_platform) (error);
        }

        g_set_error (error,
                     COG_PLATFORM_ERROR,
                     COG_PLATFORM_ERROR_LOAD,
                     "Cannot instantiate platform from module '%s': %s.",
                     soname,
                     g_module_error ());
        return NULL;
    }

    g_set_error (error,
                 COG_PLATFORM_ERROR,
                 COG_PLATFORM_ERROR_LOAD,
                 "Cannot load module '%s': %s.",
                 soname,
                 g_module_error ());
    return NULL;
}


struct _CogSimplePlatformView {
    GObject               parent;
    WebKitWebViewBackend *backend;
};

static WebKitWebViewBackend*
cog_simple_platform_view_get_backend (CogPlatformView *platform_view)
{
    return COG_SIMPLE_PLATFORM_VIEW (platform_view)->backend;
}

static void
cog_simple_platform_view_iface_init (CogPlatformViewInterface *iface)
{
    iface->get_backend = cog_simple_platform_view_get_backend;
}


G_DEFINE_TYPE_WITH_CODE (CogSimplePlatformView,
                         cog_simple_platform_view,
                         G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (COG_TYPE_PLATFORM_VIEW,
                                                cog_simple_platform_view_iface_init))

static void
cog_simple_platform_view_class_init (CogSimplePlatformViewClass *klass)
{
}

static void
cog_simple_platform_view_init (CogSimplePlatformView *platform_view)
{
}

CogPlatformView*
cog_simple_platform_view_new (WebKitWebViewBackend *backend)
{
    g_return_val_if_fail (backend != NULL, NULL);
    CogSimplePlatformView *platform_view =
        g_object_new (COG_TYPE_SIMPLE_PLATFORM_VIEW, NULL);
    platform_view->backend = backend;
    return COG_PLATFORM_VIEW (platform_view);
}
