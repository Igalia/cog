/*
 * cog-minimal-shell.c
 * Copyright (C) 2019 Adrian Perez de Castro <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#define COG_INTERNAL_COG__ 1

#include "cog-shell.h"
#include "cog-modules.h"
#include <wpe/wpe.h>


G_DECLARE_FINAL_TYPE (CogMinimalShell, cog_minimal_shell, COG, MINIMAL_SHELL, CogShell)
G_DECLARE_FINAL_TYPE (CogMinimalView, cog_minimal_view, COG, MINIMAL_VIEW, CogView)


struct _CogMinimalShellClass {
    CogShellClass parent_class;
};

struct _CogMinimalShell {
    CogShell parent;
    char    *backend_name;
};

enum {
    PROP_0,
    PROP_BACKEND_NAME,
    N_PROPERTIES,
};

static GParamSpec *s_properties[N_PROPERTIES] = { NULL, };


struct _CogMinimalViewClass {
    CogViewClass parent_class;
};

struct _CogMinimalView {
    CogView parent;
};


static gboolean
cog_minimal_shell_initable_init (GInitable    *initable,
                                 GCancellable *cancellable,
                                 GError      **error)
{
    CogMinimalShell *self = COG_MINIMAL_SHELL (initable);

    /*
     * TODO: Un-hardcode API version and loadable library suffix.
     */
    static const char *try_formats[] = {
        "libWPEBackend-%s-1.0.so",
        "libWPEBackend-%s.so",
        "%s",
    };

    for (unsigned i = 0; i < G_N_ELEMENTS (try_formats); i++) {
        g_autofree char *name = g_strdup_printf (try_formats[i],
                                                 self->backend_name);
        g_debug ("%s: Trying backend %s.", G_STRFUNC, name);
        if (wpe_loader_init (name)) {
            g_info ("%s: Backend initialized.", G_STRFUNC);
            g_clear_pointer (&self->backend_name, g_free);
            self->backend_name = g_steal_pointer (&name);
            return TRUE;
        }
    }

    g_critical ("%s: Could not find backend.", G_STRFUNC);
    g_set_error (error,
                 G_FILE_ERROR,
                 G_FILE_ERROR_NOENT,
                 "Backend '%s' not found.",
                 self->backend_name);
    return FALSE;
}


static void
cog_minimal_shell_initable_iface_init (GInitableIface *iface)
{
    iface->init = cog_minimal_shell_initable_init;
}


G_DEFINE_TYPE_WITH_CODE (CogMinimalShell, cog_minimal_shell, COG_TYPE_SHELL,
    _cog_modules_ensure_extension_points ();
    g_io_extension_point_implement (COG_MODULES_SHELL_EXTENSION_POINT,
                                    g_define_type_id, "minimal", 0);
    G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE, cog_minimal_shell_initable_iface_init))

G_DEFINE_TYPE (CogMinimalView, cog_minimal_view, COG_TYPE_VIEW)


static gboolean
cog_minimal_shell_is_supported (void)
{
    /* The minimal shell is always supported. */
    return TRUE;
}


static void
cog_minimal_shell_get_property (GObject    *object,
                                unsigned    prop_id,
                                GValue     *value,
                                GParamSpec *pspec)
{
    CogMinimalShell *self = COG_MINIMAL_SHELL (object);
    switch (prop_id) {
        case PROP_BACKEND_NAME:
            g_value_set_string (value, self->backend_name);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}


static void
cog_minimal_shell_set_property (GObject      *object,
                                unsigned      prop_id,
                                const GValue *value,
                                GParamSpec   *pspec)
{
    CogMinimalShell *self = COG_MINIMAL_SHELL (object);
    switch (prop_id) {
        case PROP_BACKEND_NAME:
            g_clear_pointer (&self->backend_name, g_free);
            self->backend_name = g_strdup (g_value_get_string (value));
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}


static void
cog_minimal_shell_dispose (GObject *object)
{
    CogMinimalShell *self = COG_MINIMAL_SHELL (object);

    g_clear_pointer (&self->backend_name, g_free);

    G_OBJECT_CLASS (cog_minimal_shell_parent_class)->dispose (object);
}


static void
cog_minimal_shell_class_init (CogMinimalShellClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    object_class->get_property = cog_minimal_shell_get_property;
    object_class->set_property = cog_minimal_shell_set_property;
    object_class->dispose = cog_minimal_shell_dispose;

    CogShellClass *shell_class = COG_SHELL_CLASS (klass);
    shell_class->is_supported = cog_minimal_shell_is_supported;

    s_properties[PROP_BACKEND_NAME] =
        g_param_spec_string ("backend-name",
                             "Backend name",
                             "WPE backend library name or path",
                             "default",
                             G_PARAM_READWRITE |
                             G_PARAM_CONSTRUCT_ONLY |
                             G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class,
                                       N_PROPERTIES,
                                       s_properties);
}

static void
cog_minimal_shell_init (CogMinimalShell *self)
{
}


static void
cog_minimal_view_setup (CogView *view)
{
    WebKitWebViewBackend *backend =
        webkit_web_view_backend_new (wpe_view_backend_create (), NULL, NULL);
    g_object_set (view, "backend", backend, NULL);
    g_boxed_free (WEBKIT_TYPE_WEB_VIEW_BACKEND, backend);
}

static void
cog_minimal_view_class_init (CogMinimalViewClass *klass)
{
    CogViewClass *view_class = COG_VIEW_CLASS (klass);
    view_class->setup = cog_minimal_view_setup;
}

static void
cog_minimal_view_init (CogMinimalView *self G_GNUC_UNUSED)
{
}
