/*
 * cog-modules.c
 * Copyright (C) 2019 Adrian Perez de Castro <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#define COG_INTERNAL_COG__ 1
#include "cog-modules.h"

static void*
ensure_extension_points (void *func)
{
    static _CogExtensionPoints ep = { NULL, };
    ep.shell = g_io_extension_point_register (COG_MODULES_SHELL_EXTENSION_POINT);

    g_debug ("%s: Extension points registered.", (const char*) func);
    return &ep;
}

const _CogExtensionPoints*
_cog_modules_ensure_extension_points (void)
{
    static GOnce once = G_ONCE_INIT;
    g_once (&once, ensure_extension_points, (void*) G_STRFUNC);
    return once.retval;
}

static void*
ensure_builtin_types (void *func)
{
    _cog_modules_ensure_extension_points ();

    /* Initialize types from built-in "modules". */
    g_type_ensure (_cog_minimal_shell_get_type ());

    g_debug ("%s: Built-in module types initialized.", (const char*) func);
    return NULL;
}

void
_cog_modules_ensure_loaded (void)
{
    static GOnce once = G_ONCE_INIT;
    g_once (&once, ensure_builtin_types, (void*) G_STRFUNC);
}

static gboolean
can_use_extension (GIOExtension *extension,
                   size_t        is_supported_offset)
{
    if (G_UNLIKELY (extension == NULL))
        return FALSE;

    if (is_supported_offset == 0)
        return TRUE;

    typedef gboolean (*VerifyFunction)(void);

    GType type = g_io_extension_get_type (extension);
    g_autoptr(GTypeClass) type_class = g_type_class_ref (type);
    return (*G_STRUCT_MEMBER (VerifyFunction, type_class, is_supported_offset)) ();
}

GType
_cog_modules_get_preferred_internal (const char        *func,
                                     GIOExtensionPoint *ep,
                                     const char        *preferred_module,
                                     size_t             is_supported_offset)
{
    g_assert (ep != NULL);

    _cog_modules_ensure_loaded ();

    GIOExtension *extension, *chosen = NULL;
    if (preferred_module) {
        extension = g_io_extension_point_get_extension_by_name (ep, preferred_module);
        if (can_use_extension (extension, is_supported_offset))
            chosen = extension;
        else if (!extension)
            g_warning ("%s: cannot find module '%s'", func, preferred_module);
    }

    for (GList *item = g_list_first (g_io_extension_point_get_extensions (ep));
         !chosen && item;
         item = g_list_next (item))
    {
        extension = item->data;
        if (can_use_extension (extension, is_supported_offset))
            chosen = extension;
    }

    return chosen ? g_io_extension_get_type (chosen) : G_TYPE_INVALID;
}

/**
 * cog_modules_get_preferred:
 * @extension_point: Name of the extension point for which to obtain a module.
 * @preferred_module: (nullable): Name of the preferred module, which
 *    overrides the default preferred one.
 * @is_supported_offset: A vtable offset, or zero.
 *
 * Retrieves the default class which implements @extension_point.
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
cog_modules_get_preferred (const char *extension_point,
                           const char *preferred_module,
                           size_t      is_supported_offset)
{
    g_return_val_if_fail (extension_point != NULL, G_TYPE_INVALID);

    GIOExtensionPoint *ep = g_io_extension_point_lookup (extension_point);

    if (!ep) {
        g_critical ("%s: invalid extension point '%s'",
                    G_STRFUNC, extension_point);
        return G_TYPE_INVALID;
    }

    return _cog_modules_get_preferred_internal (G_STRFUNC,
                                                ep,
                                                preferred_module,
                                                is_supported_offset);
}

/**
 * cog_modules_foreach:
 * @extension_point: Name of an extension point.
 * @callback: Callback function.
 * @userdata: User data passed to @callback.
 *
 * Invokes @callback for each module which implements the @extension_point.
 */
void
cog_modules_foreach (const char *extension_point,
                     void (*callback) (GIOExtension*, void*),
                     void *userdata)
{
    g_return_if_fail (extension_point != NULL);
    g_return_if_fail (callback != NULL);

    _cog_modules_ensure_loaded ();

    GIOExtensionPoint *ep = g_io_extension_point_lookup (extension_point);

    if (!ep) {
        g_critical ("%s: invalid extension point '%s'",
                    G_STRFUNC, extension_point);
        return;
    }

    for (GList *item = g_list_first (g_io_extension_point_get_extensions (ep));
         item;
         item = g_list_next (item))
    {
        (*callback) (item->data, userdata);
    }
}

/**
 * cog_modules_add_directory:
 * @directory_path: Directory to scan for loadable modules.
 *
 * Scans @directory_path for loadable modules and registers them with the
 * extension points they implement.
 */
void
cog_modules_add_directory (const char *directory_path)
{
    g_return_if_fail (directory_path != NULL);

    _cog_modules_ensure_extension_points ();

    G_LOCK_DEFINE_STATIC (module_scan);
    G_LOCK (module_scan);

    g_debug ("%s: Scanning '%s'", G_STRFUNC, directory_path);
    g_io_modules_scan_all_in_directory (directory_path);

    G_UNLOCK (module_scan);
}
