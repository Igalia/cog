/*
 * cog-platform.c
 * Copyright (C) 2018 Adrian Perez <aperez@igalia.com>
 * Copyright (C) 2018 Eduardo Lima <elima@igalia.com>
 *
 * SPDX-License-Identifier: MIT
 */

#include "cog-platform.h"
#include "cog-modules.h"

G_DEFINE_QUARK(COG_PLATFORM_ERROR, cog_platform_error)
G_DEFINE_QUARK(COG_PLATFORM_EGL_ERROR, cog_platform_egl_error)
G_DEFINE_QUARK(COG_PLATFORM_WPE_ERROR, cog_platform_wpe_error)

G_DEFINE_ABSTRACT_TYPE(CogPlatform, cog_platform, G_TYPE_OBJECT)

static CogPlatform *platform_singleton = NULL; // (owned) (atomic)

static gboolean
cog_platform_is_supported(void)
{
    return TRUE;
}

static void
cog_platform_class_init(CogPlatformClass *klass)
{
    klass->is_supported = cog_platform_is_supported;
}

static void
cog_platform_init(CogPlatform *platform)
{
}

static inline gboolean
cog_platform_ensure_singleton(const char *name)
{
    if (g_once_init_enter(&platform_singleton)) {
        cog_modules_add_directory(NULL);

        GType platform_type =
            cog_modules_get_preferred(COG_MODULES_PLATFORM, name, G_STRUCT_OFFSET(CogPlatformClass, is_supported));

        if (platform_type == G_TYPE_INVALID) {
            if (name)
                g_error("Requested platform '%s' not usable.", name);
            else
                g_error("Could not find an usable platform.");
        }

        g_debug("%s: %s requested, %s chosen.", G_STRFUNC, name ?: "None", g_type_name(platform_type));
        g_once_init_leave(&platform_singleton, g_object_new(platform_type, NULL));
        return FALSE;
    }

    return TRUE;
}

/**
 * cog_init:
 * @platform_name: (nullable): The name of the platform module to use.
 * @module_path: (nullable): The directory to scan for modules.
 *
 * Initialize the library, optionally indicating options.
 *
 * This function ensures the creation of the single [class@CogPlatform]
 * instance, and optionally allows indicating which platform module to use
 * and from which directory to load modules.
 *
 * If the @platform_name passed is %NULL, the value of the
 * `COG_PLATFORM_NAME` environment variable will be used. If the environment
 * variable is undefined, the most suitable platform module will be determined
 * automatically.
 *
 * If the @module_path passed is %NULL, the value of the `COG_MODULEDIR`
 * environment variable will be used. If the environment variable is
 * undefined, the default module path chosen at build time will be used.
 *
 * Note that it is **not required** to use this function. It is possible
 * to use [id@cog_platform_get] if the default behaviour of using the
 * environment variables (if defined) and automatic detection as fallback
 * with the default module path is acceptable.
 *
 * This function is thread-safe.
 *
 * Since: 0.20
 */
void
cog_init(const char *platform_name, const char *module_path)
{
    cog_modules_add_directory(module_path);
    gboolean already_initialized = cog_platform_ensure_singleton(platform_name ?: g_getenv("COG_PLATFORM_NAME"));
    g_return_if_fail(!already_initialized);
}

/**
 * cog_platform_get:
 *
 * Gets the platform instance.
 *
 * The platform module instance is a singleton. The instance will be created
 * if needed, but [id@cog_init] can be used to control at which point it shall
 * be created and which particular module to use.
 *
 * This function is thread-safe.
 *
 * Returns: (transfer none): The platform instance.
 *
 * Since: 0.20
 */
CogPlatform *
cog_platform_get(void)
{
    cog_platform_ensure_singleton(NULL);
    return platform_singleton;
}

/**
 * cog_platform_setup:
 * @platform: Platform instance.
 * @shell: The shell being configured.
 * @params: (nullable): String with parameters for the platform plug-in.
 * @error: (nullable): Location where to store an error on failure.
 *
 * Configure the platform plug-in module.
 *
 * If the @params string is %NULL or empty, the value of the
 * `COG_PLATFORM_PARAMS` environment variable will be used, if defined.
 * Each platform module may have its own syntax for the parameters string,
 * but typically they accept a list of comma-separated `variable=value`
 * assignments.
 *
 * The documentation for each platform plug-in details the available
 * parameters and deviations of the parameter string syntax, if applicable.
 *
 * Returns: Whether the platform was configured successfully.
 */
gboolean
cog_platform_setup(CogPlatform *platform, CogShell *shell, const char *params, GError **error)
{
    g_return_val_if_fail(COG_IS_PLATFORM(platform), FALSE);
    g_return_val_if_fail(COG_IS_SHELL(shell), FALSE);

    if (G_IS_INITABLE(platform) && !g_initable_init(G_INITABLE(platform), NULL /* cancellable */, error))
        return FALSE;

    if (!params || params[0] == '\0')
        params = g_getenv("COG_PLATFORM_PARAMS") ?: "";

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
    if (klass->init_web_view) {
        if (klass->get_view_type) {
#if GLIB_CHECK_VERSION(2, 63, 1)
            g_warning_once("%s: class %s defines both .get_view_type and .init_web_view, "
                           "the latter should be removed.",
                           G_STRFUNC, G_OBJECT_CLASS_NAME(platform));
#else
            static bool warned = false;
            if (!warned) {
                warned = true;
                g_warning("%s: class %s defines both .get_view_type and .init_web_view, "
                          "the latter should be removed.",
                          G_STRFUNC, G_OBJECT_CLASS_NAME(platform));
            }
#endif
            return;
        }

        klass->init_web_view(platform, view);
    }
}

WebKitInputMethodContext *
cog_platform_create_im_context(CogViewport *viewport)
{
    CogPlatform *platform = cog_platform_get();
    g_return_val_if_fail(COG_IS_PLATFORM(platform), NULL);

    CogPlatformClass *klass = COG_PLATFORM_GET_CLASS(platform);
    if (klass->create_im_context)
        return klass->create_im_context(viewport);

    return NULL;
}
