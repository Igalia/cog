/*
 * cog-modules.c
 * Copyright (C) 2021 Adrian Perez de Castro <aperez@igalia.com>
 *
 * SPDX-License-Identifier: MIT
 */

#include "cog-modules.h"

#include "cog-config.h"
#include "cog-fallback-platform.h"

struct ExtensionPoints {
    GIOExtensionPoint *platform;
};

static void *
ensure_extension_points_internal(void *func)
{
    static struct ExtensionPoints ep = {};
    ep.platform = g_io_extension_point_register(COG_MODULES_PLATFORM_EXTENSION_POINT);
    g_debug("%s: Extension points registered.", (const char *) func);
    return &ep;
}

const struct ExtensionPoints *
ensure_extension_points(void)
{
    static GOnce once = G_ONCE_INIT;
    g_once(&once, ensure_extension_points_internal, (void *) G_STRFUNC);
    return once.retval;
}

static void *
ensure_builtin_types_internal(void *func)
{
    ensure_extension_points();

    g_type_ensure(cog_fallback_platform_get_type());

    g_debug("%s: Built-in platform types initialized.", (const char *) func);
    return NULL;
}

void
ensure_builtin_types(void)
{
    static GOnce once = G_ONCE_INIT;
    g_once(&once, ensure_builtin_types_internal, (void *) G_STRFUNC);
}

/**
 * cog_modules_get_platform_extension_point:
 *
 * Obtain the extension point for platform modules.
 *
 * Returns: (transfer none): Extension point.
 */
GIOExtensionPoint *
cog_modules_get_platform_extension_point(void)
{
    return ensure_extension_points()->platform;
}

static gboolean
can_use_extension(GIOExtension *extension, size_t is_supported_offset)
{
    if (extension == NULL)
        return FALSE;

    if (is_supported_offset == 0)
        return TRUE;

    typedef gboolean (*VerifyFunction)(void);

    GType type = g_io_extension_get_type(extension);
    g_autoptr(GTypeClass) type_class = g_type_class_ref(type);
    return (*G_STRUCT_MEMBER(VerifyFunction, type_class, is_supported_offset))();
}

/**
 * cog_modules_get_preferred:
 * @extension_point: Name of the extension point for which to obtain a module.
 * @preferred_module: (nullable): Name of the preferred module, which
 *    overrides the default preferred one.
 * @is_supported_offset: A vtable offset, or zero.
 *
 * Retrieves the default class which implements a certain extension point.
 *
 * If @preferred_module is not %NULL, then the implementation named by it will
 * be tried first. After that, or if %NULL, all other implementations will be
 * tried in order of decreasing priority.
 *
 * If @is_supported_offset is non-zero, then it is the offset into the class
 * vtable at which there is a function which takes no arguments and returns
 * a boolean. This function will be called on each candidate implementation
 * to check whether it is usable or not.
 *
 * Returns: (transfer none): A type that can be instantiated to implement
 *    @extension_point, or %G_TYPE_INVALID if there are o usable
 *    implementations.
 */
GType
cog_modules_get_preferred(GIOExtensionPoint *extension_point, const char *preferred_module, size_t is_supported_offset)
{
    g_return_val_if_fail(extension_point != NULL, G_TYPE_INVALID);

    ensure_builtin_types();
    cog_modules_add_directory(NULL);

    if (extension_point == COG_MODULES_PLATFORM && g_strcmp0(preferred_module, "fdo") == 0) {
        g_warning("Platform module name 'fdo' is deprecated, please use 'wl' instead.");
        preferred_module = "wl";
    }

    GIOExtension *extension, *chosen = NULL;
    if (preferred_module) {
        extension = g_io_extension_point_get_extension_by_name(extension_point, preferred_module);
        if (can_use_extension(extension, is_supported_offset))
            chosen = extension;
        else if (!extension)
            g_warning("%s: cannot find module '%s'", G_STRFUNC, preferred_module);
        if (!chosen) {
            g_warning("%s: preferred module '%s' not supported", G_STRFUNC, preferred_module);
            return G_TYPE_INVALID;
        }
    }

    GList *item = g_list_first(g_io_extension_point_get_extensions(extension_point));
    while (!chosen && item) {
        if (can_use_extension(item->data, is_supported_offset))
            chosen = item->data;
        item = g_list_next(item);
    }

    return chosen ? g_io_extension_get_type(chosen) : G_TYPE_INVALID;
}

/**
 * cog_modules_foreach:
 * @extension_point: Name of an extension point.
 * @callback: Callback function.
 * @userdata: User data passed to @callback.
 *
 * Invokes a callback for each module which implements an extension point.
 */
void
cog_modules_foreach(GIOExtensionPoint *extension_point, void (*callback)(GIOExtension *, void *), void *userdata)
{
    g_return_if_fail(extension_point != NULL);
    g_return_if_fail(callback != NULL);

    ensure_builtin_types();
    cog_modules_add_directory(NULL);

    GList *item = g_list_first(g_io_extension_point_get_extensions(extension_point));
    while (item) {
        (*callback)(item->data, userdata);
        item = g_list_next(item);
    }
}

/**
 * cog_modules_add_directory:
 * @directory_path: (nullable): Directory to scan for loadable modules.
 *
 * Scans a directory for loadable modules and registers them with the
 * extension points they implement.
 *
 * Since version 0.20 the @directory_path parameter may be %NULL, which
 * results in the default module directory chosen at build time to be
 * scanned. Normally this function does not need to be done manually,
 * because the default module directory will be scanned automatically
 * the first time [id@cog_modules_get_preferred] or [id@cog_modules_foreach]
 * is used.
 *
 * Note that in versions 0.20 and newer this function will skip modules
 * with the same file base name as the ones previously scanned. If two
 * directories contain a module with the same file base name, the one
 * scanned first will be the one used. In particular, this means that
 * calling this function with the same path more than once is allowed.
 */
void
cog_modules_add_directory(const char *directory_path)
{
    if (!directory_path)
        directory_path = COG_MODULEDIR;

    ensure_extension_points();

    G_LOCK_DEFINE_STATIC(module_scan);
    G_LOCK(module_scan);

    static GIOModuleScope *scope = NULL;
    if (!scope)
        scope = g_io_module_scope_new(G_IO_MODULE_SCOPE_BLOCK_DUPLICATES);

    static gboolean default_path_added = FALSE;
    if (!strcmp(COG_MODULEDIR, directory_path)) {
        if (default_path_added) {
            g_debug("%s: Default path already added, skipping.", G_STRFUNC);
            goto out; // Ensure that the the lock is released.
        }
        default_path_added = TRUE;
    }

    g_debug("%s: Scanning '%s'", G_STRFUNC, directory_path);
    g_io_modules_scan_all_in_directory_with_scope(directory_path, scope);

out:
    G_UNLOCK(module_scan);
}
