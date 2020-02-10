/*
 * cog-shell.c
 * Copyright (C) 2019 Adrian Perez de Castro <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#define COG_INTERNAL_COG__ 1

#include "cog-shell.h"

#include "cog-modules.h"
#include "cog-request-handler.h"
#include <stdarg.h>
#include <string.h>

typedef struct {
    char  *name;
    GList *views;
} CogShellPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (CogShell, cog_shell, G_TYPE_OBJECT)

enum {
    PROP_0,
    PROP_NAME,
    N_PROPERTIES,
};

static GParamSpec *s_properties[N_PROPERTIES] = { NULL, };

static void
cog_shell_get_property (GObject    *object,
                        unsigned    prop_id,
                        GValue     *value,
                        GParamSpec *pspec)
{
    CogShell *shell = COG_SHELL (object);
    switch (prop_id) {
        case PROP_NAME:
            g_value_set_string (value, cog_shell_get_name (shell));
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
cog_shell_set_property (GObject      *object,
                        unsigned      prop_id,
                        const GValue *value,
                        GParamSpec   *pspec)
{
    CogShellPrivate *priv = cog_shell_get_instance_private (COG_SHELL (object));
    switch (prop_id) {
        case PROP_NAME:
            priv->name = g_value_dup_string (value);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
cog_shell_dispose (GObject *object)
{
    CogShellPrivate *priv = cog_shell_get_instance_private (COG_SHELL (object));

    if (priv->views) {
        g_list_free_full (priv->views, g_object_unref);
        priv->views = NULL;
    }
    g_clear_pointer (&priv->name, g_free);

    G_OBJECT_CLASS (cog_shell_parent_class)->dispose (object);
}

static gboolean
cog_shell_is_supported_default (void)
{
    return FALSE;
}

static GType
cog_shell_get_view_class_default (void)
{
    return COG_TYPE_VIEW;
}

static void
cog_shell_class_init (CogShellClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    object_class->get_property = cog_shell_get_property;
    object_class->set_property = cog_shell_set_property;
    object_class->dispose = cog_shell_dispose;

    klass->is_supported = cog_shell_is_supported_default;
    klass->get_view_class = cog_shell_get_view_class_default;

    s_properties[PROP_NAME] =
        g_param_spec_string ("name",
                             "Name",
                             "Name of the CogView instance",
                             NULL,
                             G_PARAM_READWRITE |
                             G_PARAM_CONSTRUCT_ONLY |
                             G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class,
                                       N_PROPERTIES,
                                       s_properties);
}

static void
cog_shell_init (CogShell *shell)
{
}

static GType
cog_shell_get_view_class (CogShell *shell)
{
    g_assert (COG_IS_SHELL (shell));
    CogShellClass *shell_class = COG_SHELL_GET_CLASS (shell);
    return (*shell_class->get_view_class) ();
}

static gboolean
is_construct_property (const char  *func,
                       GType        object_type,
                       const char  *propname,
                       GParamSpec  *pspec,
                       unsigned     n_properties,
                       GParamSpec **property_pspecs)
{
    if (G_UNLIKELY (pspec == NULL)) {
        g_critical ("%s: object class '%s' has no property named '%s'",
                    func, g_type_name (object_type), propname);
        return FALSE;
    }

    if (G_UNLIKELY (~pspec->flags & G_PARAM_READWRITE)) {
        g_critical ("%s: property '%s' of object class '%s' is not writable",
                    func, pspec->name, g_type_name (object_type));
        return FALSE;
    }

    if (G_UNLIKELY (pspec->flags & (G_PARAM_CONSTRUCT | G_PARAM_CONSTRUCT_ONLY))) {
        for (unsigned i = 0; i < n_properties; i++) {
            if (G_UNLIKELY (property_pspecs[i] == pspec)) {
                g_critical ("%s: property '%s' of object class '%s' cannot be set twice",
                            func, pspec->name, g_type_name (object_type));
                return FALSE;
            }
        }
    }

    return TRUE;
}

/**
 * cog_shell_new:
 * @name:
 *
 * Returns: (transfer full): A new #CogShell.
 */
CogShell*
cog_shell_new (const char *name)
{
    return cog_shell_new_from_module (name, NULL);
}

CogShell*
cog_shell_new_from_module (const char *name,
                           const char *module_name)
{
    g_return_val_if_fail (name != NULL, NULL);

    GType shell_type =
        _cog_modules_get_preferred_internal (G_STRFUNC,
                                             _cog_modules_get_shell_extension_point (),
                                             module_name,
                                             G_STRUCT_OFFSET (CogShellClass, is_supported));
    if (shell_type == G_TYPE_INVALID) {
        g_critical ("%s: cannot find any '%s' implementation",
                    G_STRFUNC, COG_MODULES_SHELL_EXTENSION_POINT);
        return NULL;
    }

    return g_initable_new (shell_type, NULL, NULL, "name", name, NULL);
}

/**
 * cog_shell_get_name:
 * @shell: A #CogShell
 *
 * Returns: (transfer none): The name which identifies the shell.
 */
const char*
cog_shell_get_name (CogShell *shell)
{
    g_return_val_if_fail (COG_IS_SHELL (shell), NULL);
    CogShellPrivate *priv = cog_shell_get_instance_private (COG_SHELL (shell));
    return priv->name;
}

/**
 * cog_shell_create_view:
 * @shell: A #CogShell
 * @name: Name for the created #CogView instance
 * @...: Properties to pass to the constructor.
 *
 * Returns: (transfer floating): A new #CogView of the type needed by the shell.
 */
CogView*
cog_shell_create_view (CogShell *shell, const char *name, const char *propname, ...)
{
    g_return_val_if_fail (COG_IS_SHELL (shell), NULL);
    g_return_val_if_fail (name != NULL, NULL);

    GType view_type = cog_shell_get_view_class (shell);
    va_list varargs;
    va_start (varargs, propname);

    g_autoptr(CogView) view = NULL;

    WebKitWebViewBackend *view_backend = NULL;
    view_backend = cog_shell_new_view_backend (shell);
    if (!view_backend) {
        g_warning ("Failed to get platform's view backend");
    }
    if (propname) {
        /*
         * Gather parameters, and add our own (shell, name) to be used with
         * g_object_new_with_properties()
         */
        GObjectClass *unref_class = NULL;
        GObjectClass *view_class = g_type_class_peek_static (view_type);
        if (!view_class)
            view_class = unref_class = g_type_class_ref (view_type);

        unsigned n_properties = 3;
        g_autofree const char **property_names = g_new (const char*, n_properties);
        g_autofree GValue *property_values = g_new (GValue, n_properties);
        g_autofree GParamSpec **property_pspecs = g_new (GParamSpec*, n_properties);

        property_names[0] = "shell";
        property_values[0] = (GValue) G_VALUE_INIT;
        g_value_init (&property_values[0], G_TYPE_OBJECT);
        g_value_set_object (&property_values[0], shell);
        property_names[1] = "name";
        property_values[1] = (GValue) G_VALUE_INIT;
        g_value_init (&property_values[1], G_TYPE_STRING);
        g_value_set_string (&property_values[1], name);
        property_names[2] = "backend";
        property_values[2] = (GValue) G_VALUE_INIT;
        g_value_init (&property_values[2], WEBKIT_TYPE_WEB_VIEW_BACKEND);
        g_value_set_boxed (&property_values[2], view_backend);

        do {
            GParamSpec *pspec = g_object_class_find_property (view_class, propname);
            if (!is_construct_property (G_STRFUNC,
                                        view_type,
                                        propname,
                                        pspec,
                                        n_properties,
                                        property_pspecs))
                break;

            property_names = g_renew(const char*, property_names, n_properties + 1);
            property_values = g_renew(GValue, property_values, n_properties + 1);
            property_pspecs = g_renew(GParamSpec*, property_pspecs, n_properties + 1);

            property_names[n_properties] = propname;
            property_pspecs[n_properties] = pspec;

            g_autofree char *error = NULL;
            G_VALUE_COLLECT_INIT (&property_values[n_properties],
                                  pspec->value_type, varargs, 0, &error);
            if (error) {
                g_critical ("%s: %s", G_STRFUNC, error);
                g_value_unset (&property_values[n_properties]);
                break;
            }

            n_properties++;
        } while ((propname = va_arg (varargs, const char*)));

        g_clear_pointer (&unref_class, g_type_class_unref);

        view = (CogView*) g_object_new_with_properties (view_type,
                                                        n_properties,
                                                        property_names,
                                                        property_values);
    } else {
        /* Fast case: No additional properties specified. */
        view = (CogView*) g_object_new (view_type,
                                        "name", name,
                                        "shell", shell,
                                        "backend", view_backend,
                                        NULL);
    }

    va_end (varargs);
    return g_steal_pointer (&view);
}

/**
 * cog_shell_add_view:
 * @shell: A #CogShell
 * @view: A #CogView
 */
void
cog_shell_add_view (CogShell *shell,
                    CogView  *view)
{
    g_return_if_fail (COG_IS_SHELL (shell));
    g_return_if_fail (COG_IS_VIEW (view));
    g_return_if_fail (G_TYPE_CHECK_INSTANCE_TYPE (view, cog_shell_get_view_class (shell)));
    g_return_if_fail (cog_shell_get_view (shell, cog_view_get_name (view)) == NULL);

    CogShellPrivate *priv = cog_shell_get_instance_private (shell);
    priv->views = g_list_prepend (priv->views, g_object_ref_sink (view));
}

/**
 * cog_shell_get_view:
 * @shell: A #CogShell
 * @name: Name of the view to obtain.
 *
 * Returns: (transfer none): The corresponding #CogView
 */
CogView*
cog_shell_get_view (CogShell   *shell,
                    const char *name)
{
    g_return_val_if_fail (COG_IS_SHELL (shell), NULL);
    g_return_val_if_fail (name != NULL, NULL);

    CogShellPrivate *priv = cog_shell_get_instance_private (shell);
    for (GList *item = g_list_first (priv->views); item; item = g_list_next (item)) {
        CogView *view = item->data;
        if (strcmp (name, cog_view_get_name (view)) == 0) {
            g_debug("cog_shell_get_view - view_name: %s - found", cog_view_get_name (view));
            return view;
        }
    }

    g_debug("cog_shell_get_view - not found");
    return NULL;
}

/**
 * cog_shell_get_views:
 * @shell: A #CogShell.
 *
 * Returns: (transfer none, element-type=CogView): A list of views.
 */
GList*
cog_shell_get_views (CogShell *shell)
{
    g_return_val_if_fail (COG_IS_SHELL (shell), NULL);

    CogShellPrivate *priv = cog_shell_get_instance_private (shell);
    return priv->views;
}

CogView*
cog_shell_get_active_view (CogShell *shell)
{
    g_return_val_if_fail (COG_IS_SHELL (shell), NULL);

    CogShellPrivate *priv = cog_shell_get_instance_private (shell);

    for (GList *item = g_list_first (priv->views); item; item = g_list_next (item)) {
        CogView *view = item->data;
        WebKitWebViewBackend *view_backend = webkit_web_view_get_backend (WEBKIT_WEB_VIEW(view));
        struct wpe_view_backend *backend = webkit_web_view_backend_get_wpe_backend (view_backend);
        if (wpe_view_backend_get_activity_state (backend) & wpe_view_activity_state_visible) {
            g_debug ("cog_shell_active_view - view_name: %s - found", cog_view_get_name (view));
            return view;
        }
    }

    g_debug ("cog_shell_get_active_view - not found");
    return NULL;
}

void
cog_shell_set_active_view (CogShell *shell, CogView *view)
{
    g_return_if_fail (COG_IS_SHELL (shell));
    g_return_if_fail (COG_IS_VIEW (view));

    CogShellPrivate *priv = cog_shell_get_instance_private (shell);

    for (GList *item = g_list_first (priv->views); item; item = g_list_next (item)) {
        CogView *view_iter = item->data;
        WebKitWebViewBackend *view_backend = webkit_web_view_get_backend (WEBKIT_WEB_VIEW(view_iter));
        struct wpe_view_backend *backend = webkit_web_view_backend_get_wpe_backend (view_backend);
        if (strcmp (cog_view_get_name (view_iter), cog_view_get_name (view)) == 0) {
            wpe_view_backend_add_activity_state (backend, wpe_view_activity_state_visible);
            g_debug ("cog_shell_set_active_view - view_name: %s - set active", cog_view_get_name (view));
        } else {
            wpe_view_backend_remove_activity_state (backend, wpe_view_activity_state_visible);
            g_debug ("cog_shell_set_active_view - view_name: %s - set deactive", cog_view_get_name (view_iter));
        }
    }
    cog_shell_resume_active_views(shell);
}

WebKitWebViewBackend*
cog_shell_new_view_backend (CogShell *shell)
{
    CogShellClass *klass;

    g_return_val_if_fail (COG_IS_SHELL (shell), NULL);

    klass = COG_SHELL_GET_CLASS (shell);

    g_return_val_if_fail (klass->cog_shell_new_view_backend != NULL, NULL);

    return klass->cog_shell_new_view_backend (shell);
}

WebKitWebViewBackend*
cog_shell_resume_active_views (CogShell *shell)
{
    CogShellClass *klass;

    g_return_val_if_fail (COG_IS_SHELL (shell), NULL);

    klass = COG_SHELL_GET_CLASS (shell);

    g_return_val_if_fail (klass->cog_shell_new_view_backend != NULL, NULL);

    return klass->cog_shell_resume_active_views (shell);
}
