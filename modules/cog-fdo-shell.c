/*
 * cog-fdo-shell.c
 * Copyright (C) 2018-2019 Adrian Perez de Castro <aperez@igalia.com>
 * Copyright (C) 2018 Eduardo Lima <elima@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#include <cog.h>
#include <wayland-egl.h>
#include <wpe/wpe.h>
#include "../platform/pwl.h"

#if 1
# define TRACE(fmt, ...) g_debug ("%s: " fmt, G_STRFUNC, ##__VA_ARGS__)
#else
# define TRACE(fmt, ...) ((void) 0)
#endif

static PwlDisplay *s_pdisplay = NULL;
static bool s_support_checked = false;
G_LOCK_DEFINE_STATIC (s_globals);


typedef struct {
    CogShellClass parent_class;
} CogFdoShellClass;

typedef struct {
    CogShell parent;
} CogFdoShell;

static void cog_fdo_shell_initable_iface_init (GInitableIface *iface);

G_DEFINE_DYNAMIC_TYPE_EXTENDED (CogFdoShell, cog_fdo_shell, COG_TYPE_SHELL, 0,
    g_io_extension_point_implement (COG_MODULES_SHELL_EXTENSION_POINT,
                                    g_define_type_id, "fdo", 100);
    G_IMPLEMENT_INTERFACE_DYNAMIC (G_TYPE_INITABLE, cog_fdo_shell_initable_iface_init))

G_MODULE_EXPORT
void
g_io_fdo_shell_load (GIOModule *module)
{
    TRACE ("");
    cog_fdo_shell_register_type (G_TYPE_MODULE (module));
}

G_MODULE_EXPORT
void
g_io_fdo_shell_unload (GIOModule *module G_GNUC_UNUSED)
{
    TRACE ("");

    G_LOCK (s_globals);
    g_clear_pointer (&s_pdisplay, pwl_display_destroy);
    G_UNLOCK (s_globals);
}

static gboolean
cog_fdo_shell_is_supported (void)
{
    gboolean result;

    G_LOCK (s_globals);

    if (!s_support_checked) {
        g_autoptr(GError) error = NULL;
        s_pdisplay = pwl_display_connect (NULL, &error);
        if (!s_pdisplay)
            g_debug ("%s: %s", G_STRFUNC, error->message);
        s_support_checked = true;
    }
    TRACE ("PwlDisplay @ %p", s_pdisplay);
    result = s_pdisplay ? TRUE : FALSE;

    G_UNLOCK (s_globals);

    return result;
}

static void
cog_fdo_shell_class_init (CogFdoShellClass *klass)
{
    TRACE ("");
    CogShellClass *shell_class = COG_SHELL_CLASS (klass);
    shell_class->is_supported = cog_fdo_shell_is_supported;
}

static void
cog_fdo_shell_class_finalize (CogFdoShellClass *klass)
{
    TRACE ("");
}

static void
cog_fdo_shell_init (CogFdoShell *shell)
{
    TRACE ("");
}

gboolean
cog_fdo_shell_initable_init (GInitable *initable,
                             GCancellable *cancellable,
                             GError **error)
{
    if (cancellable != NULL) {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                             "Cancellable initialization not supported");
        return FALSE;
    }

    if (!wpe_loader_init ("libWPEBackend-fdo-1.0.so")) {
        g_debug ("%s: Could not initialize libwpe.", G_STRFUNC);
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                             "Couldn't initialize the FDO backend.");
        return FALSE;
    }

    return TRUE;
}

static void cog_fdo_shell_initable_iface_init (GInitableIface *iface)
{
    iface->init = cog_fdo_shell_initable_init;
}
