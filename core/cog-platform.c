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
                            "Could not find a usable platform module");
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
    if (klass->init_web_view) {
        if (klass->get_view_type) {
            g_warning_once("%s: class %s defines both .get_view_type and .init_web_view, "
                           "the latter should be removed.",
                           G_STRFUNC, G_OBJECT_CLASS_NAME(platform));
            return;
        }

        klass->init_web_view(platform, view);
    }
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

/**
 * cog_platform_configure: (constructor)
 * @name: (nullable): Name of the platform implementation to use.
 * @params: (nullable): Parameters passed to the platform implementation.
 * @env_prefix: (nullable): Prefix for environment variables to check.
 * @shell: The shell that the platform will be configured for.
 * @error: Location where to store errors, if any.
 *
 * Creates and configures a platform implementation.
 *
 * Search and create a platform implementation with the given @name,
 * configuring it with the passed @params for use with a @shell.
 *
 * If the @env_prefix is non-%NULL, then the environment variable
 * `<env_prefix>_PLATFORM_NAME` can be used to set the platform name
 * when @name is %NULL, and the variable `<env_prefix>_PLATFORM_PARAMS`
 * can be used to set the configuration parameters when @params is %NULL.
 * Environment variables will *not* be used if %NULL is passed as the
 * prefix.
 *
 * If both @name is %NULL and the `<env_prefix>_PLATFORM_NAME` variable
 * not defined, then the platform implementation will be chosen automatically
 * among the available ones.
 *
 * Note that [id@cog_modules_add_directory] may be used beforehand to
 * configure where to search for available platform plug-ins.
 *
 * Returns: (transfer full): The configured platform.
 *
 * Since: 0.18
 */
CogPlatform *
cog_platform_configure(const char *name, const char *params, const char *env_prefix, CogShell *shell, GError **error)
{
    g_return_val_if_fail(!default_platform, default_platform);

    g_autofree char *platform_name = g_strdup(name);
    g_autofree char *platform_params = g_strdup(params);

    if (env_prefix) {
        if (!platform_name) {
            g_autofree char *name_var = g_strconcat(env_prefix, "_PLATFORM_NAME", NULL);
            platform_name = g_strdup(g_getenv(name_var));
        }
        if (!params) {
            g_autofree char *params_var = g_strconcat(env_prefix, "_PLATFORM_PARAMS", NULL);
            platform_params = g_strdup(g_getenv(params_var));
        }
    }
    g_debug("%s: name '%s', params '%s'", G_STRFUNC, platform_name, platform_params);

    CogPlatform *platform = cog_platform_new(platform_name, error);
    if (!platform)
        return NULL;

    if (!cog_platform_setup(platform, shell, platform_params ?: "", error)) {
        g_object_unref(platform);
        g_assert(!default_platform);
        return NULL;
    }
    g_debug("%s: Configured %s @ %p", G_STRFUNC, G_OBJECT_TYPE_NAME(platform), platform);

    g_assert(platform == default_platform);
    return platform;
}
