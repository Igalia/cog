/*
 * cog-platform.c
 * Copyright (C) 2018 Adrian Perez <aperez@igalia.com>
 * Copyright (C) 2018 Eduardo Lima <elima@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#include "cog-platform.h"
#include "cog-modules.h"

G_DEFINE_QUARK(COG_PLATFORM_ERROR, cog_platform_error)
G_DEFINE_QUARK(COG_PLATFORM_EGL_ERROR, cog_platform_egl_error)
G_DEFINE_QUARK(COG_PLATFORM_WPE_ERROR, cog_platform_wpe_error)

G_DEFINE_ABSTRACT_TYPE(CogPlatform, cog_platform, G_TYPE_OBJECT)

static void
cog_platform_constructed(GObject *object)
{
    CogPlatform *platform = COG_PLATFORM(object);

    if (cog_platform_get_default() == NULL)
        cog_platform_set_default(platform);
}

static void
cog_platform_finalize(GObject *object)
{
    CogPlatform *platform = COG_PLATFORM(object);

    if (cog_platform_get_default() == platform)
        cog_platform_set_default(NULL);

    G_OBJECT_CLASS(cog_platform_parent_class)->finalize(object);
}

static gboolean
cog_platform_is_supported(void)
{
    return TRUE;
}

static void
cog_platform_class_init(CogPlatformClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->constructed = cog_platform_constructed;
    object_class->finalize = cog_platform_finalize;

    klass->is_supported = cog_platform_is_supported;
}

static void
cog_platform_init(CogPlatform *platform)
{
}

static CogPlatform *default_platform = NULL;

void
cog_platform_set_default(CogPlatform *platform)
{
    default_platform = platform;
}

CogPlatform *
cog_platform_get_default(void)
{
    return default_platform;
}

CogPlatform *
cog_platform_new(const char *name, GError **error)
{
    GType platform_type =
        cog_modules_get_preferred(COG_MODULES_PLATFORM, name, G_STRUCT_OFFSET(CogPlatformClass, is_supported));

    if (platform_type == G_TYPE_INVALID) {
        g_set_error_literal(error, COG_PLATFORM_ERROR, COG_PLATFORM_ERROR_NO_MODULE,
                            "Could not find an usable platform module");
        return NULL;
    }

    g_autoptr(CogPlatform) self = g_object_new(platform_type, NULL);
    if (G_IS_INITABLE(self)) {
        if (!g_initable_init(G_INITABLE(self),
                             NULL, /* cancellable */
                             error))
            return NULL;
    }

    return g_steal_pointer(&self);
}

gboolean
cog_platform_setup (CogPlatform *platform,
                    CogShell    *shell,
                    const char  *params,
                    GError     **error)
{
    g_return_val_if_fail(COG_IS_PLATFORM(platform), FALSE);
    g_return_val_if_fail (COG_IS_SHELL (shell), FALSE);

    return COG_PLATFORM_GET_CLASS(platform)->setup(platform, shell, params, error);
}

WebKitWebViewBackend*
cog_platform_get_view_backend (CogPlatform   *platform,
                               WebKitWebView *related_view,
                               GError       **error)
{
    g_return_val_if_fail(COG_IS_PLATFORM(platform), NULL);

    CogPlatformClass *klass = COG_PLATFORM_GET_CLASS(platform);
    return klass->get_view_backend(platform, related_view, error);
}

void
cog_platform_init_web_view (CogPlatform   *platform,
                            WebKitWebView *view)
{
    g_return_if_fail(COG_IS_PLATFORM(platform));

    CogPlatformClass *klass = COG_PLATFORM_GET_CLASS(platform);
    if (klass->init_web_view)
        klass->init_web_view(platform, view);
}

WebKitInputMethodContext *
cog_platform_create_im_context(CogPlatform *platform)
{
    g_return_val_if_fail(COG_IS_PLATFORM(platform), NULL);

    CogPlatformClass *klass = COG_PLATFORM_GET_CLASS(platform);
    if (klass->create_im_context)
        return klass->create_im_context(platform);

    return NULL;
}
