/*
 * cog-fallback-platform.c
 * Copyright (C) 2023 Igalia S.L.
 *
 * SPDX-License-Identifier: MIT
 */

#ifdef G_LOG_DOMAIN
#    undef G_LOG_DOMAIN
#endif /* G_LOG_DOMAIN */

#define G_LOG_DOMAIN "Cog-Fallback"

#include "cog-fallback-platform.h"
#include "cog-modules.h"
#include "cog-platform.h"

struct _CogFallbackPlatformClass {
    CogPlatformClass parent_class;
};

struct _CogFallbackPlatform {
    CogPlatform parent;
};

G_DECLARE_FINAL_TYPE(CogFallbackPlatform, cog_fallback_platform, COG, FALLBACK_PLATFORM, CogPlatform)
G_DEFINE_TYPE_WITH_CODE(
    CogFallbackPlatform, cog_fallback_platform, COG_TYPE_PLATFORM, (void) cog_modules_get_platform_extension_point();
    g_io_extension_point_implement(COG_MODULES_PLATFORM_EXTENSION_POINT, g_define_type_id, "fallback", 0))

static gboolean
cog_fallback_platform_is_supported(void)
{
    /* The fallback platform is always supported. */
    return TRUE;
}

static gboolean
cog_fallback_platform_setup(CogPlatform *platform, CogShell *shell G_GNUC_UNUSED, const char *params, GError **error)
{
    if (params && *params) {
        static const char *try_formats[] = {
            "libWPEBackend-%s-1.0.so.1",
            "libWPEBackend-%s-1.0.so",
            "libWPEBackend-%s.so.1",
            "libWPEBackend-%s.so",
        };

        for (unsigned i = 0; i < G_N_ELEMENTS(try_formats); i++) {
            g_autofree char *name = g_strdup_printf(try_formats[i], params);
            if (wpe_loader_init(name)) {
                g_debug("%s: Backend implementation '%s' loaded.", G_STRFUNC, name);
                return TRUE;
            }
        }

        g_critical("%s: Could not find a suitable backend.", G_STRFUNC);
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_NOENT, "Backend '%s' not found.", params);
        return FALSE;
    }

    g_debug("%s: backend implementation library name not specified, using libwpe defaults.", G_STRFUNC);
    return TRUE;
}

static WebKitWebViewBackend *
cog_fallback_platform_get_view_backend(CogPlatform *platform, WebKitWebView *related_view, GError **error)
{
    return webkit_web_view_backend_new(wpe_view_backend_create(), NULL, NULL);
}

static void
cog_fallback_platform_class_init(CogFallbackPlatformClass *klass)
{
    CogPlatformClass *platform_class = COG_PLATFORM_CLASS(klass);
    platform_class->is_supported = cog_fallback_platform_is_supported;
    platform_class->setup = cog_fallback_platform_setup;
    platform_class->get_view_backend = cog_fallback_platform_get_view_backend;
}

static void
cog_fallback_platform_init(CogFallbackPlatform *self)
{
}
