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

typedef struct {
    char    *name;
    GList   *views;
    CogView *focused_view;
} CogShellPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (CogShell, cog_shell, G_TYPE_OBJECT)

enum {
    PROP_0,
    PROP_NAME,
    PROP_FOCUSED_VIEW,
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
        case PROP_FOCUSED_VIEW:
            g_value_set_object (value, cog_shell_get_focused_view (shell));
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
    CogShell *self = COG_SHELL (object);
    CogShellPrivate *priv = cog_shell_get_instance_private (self);
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

    s_properties[PROP_FOCUSED_VIEW] =
        g_param_spec_object ("focused-view",
                             "Focused view",
                             "CogView instance with focus",
                             COG_TYPE_VIEW,
                             G_PARAM_READABLE |
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
                       unsigned     n_properties)
{
    if (G_UNLIKELY (pspec == NULL)) {
        g_critical ("%s: object class '%s' has no property named '%s'",
                    func, g_type_name (object_type), propname);
        return FALSE;
    }

    if (G_UNLIKELY (!(pspec->flags & G_PARAM_READWRITE))) {
        g_critical ("%s: property '%s' of object class '%s' is not writable",
                    func, pspec->name, g_type_name (object_type));
        return FALSE;
    }

    return TRUE;
}

static gboolean
string_array_contains (const char *haystack[],
                       unsigned    haystack_size,
                       const char *needle)
{
    for (unsigned i = 0; i < haystack_size; i++)
        if (strcmp (needle, haystack[i]) == 0)
            return TRUE;

    return FALSE;
}

static CogShell*
cog_shell_new_internal (GError    **error,
                        const char *shell_name,
                        const char *module_name,
                        const char *prop_name,
                        va_list     args)
{
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

    g_autoptr(CogShell) shell = NULL;
    if (prop_name) {
        GObjectClass *unref_class = NULL;
        GObjectClass *shell_class = g_type_class_peek_static (shell_type);
        if (!shell_class)
            shell_class = unref_class = g_type_class_ref (shell_type);

        unsigned n_properties = 1;
        g_autofree const char **property_names = g_new (const char*, n_properties);
        g_autofree GValue *property_values = g_new (GValue, n_properties);

        property_names[0] = "name";
        property_values[0] = (GValue) G_VALUE_INIT;
        g_value_init (&property_values[0], G_TYPE_STRING);
        g_value_set_string (&property_values[0], shell_name);

        do {
            GParamSpec *pspec = g_object_class_find_property (shell_class, prop_name);
            if (!is_construct_property (G_STRFUNC,
                                        shell_type,
                                        prop_name,
                                        pspec,
                                        n_properties))
                break;

            if (string_array_contains (property_names, n_properties, prop_name)) {
                g_critical ("%s: property '%s' of object class '%s' cannot be "
                            "set twice.", G_STRFUNC, pspec->name,
                            g_type_name (shell_type));
                break;
            }

            property_names = g_renew (const char*, property_names, n_properties + 1);
            property_values = g_renew (GValue, property_values, n_properties + 1);

            property_names[n_properties] = prop_name;

            g_autofree char *error = NULL;
            G_VALUE_COLLECT_INIT (&property_values[n_properties],
                                  pspec->value_type, args,
                                  G_VALUE_NOCOPY_CONTENTS, &error);
            if (error) {
                g_critical ("%s: %s", G_STRFUNC, error);
                g_value_unset (&property_values[n_properties]);
                break;
            }

            n_properties++;
        } while ((prop_name = va_arg (args, const char*)));

        g_clear_pointer (&unref_class, g_type_class_unref);    

        shell = (CogShell*) g_object_new_with_properties (shell_type,
                                                          n_properties,
                                                          property_names,
                                                          property_values);
    } else {
        /* Fast case: No additional properties specified. */
        shell = (CogShell*) g_object_new (shell_type,
                                          "name", shell_name,
                                          NULL);
    }

    if (G_IS_INITABLE (shell)) {
        if (!g_initable_init ((GInitable*) shell,
                              NULL,  /* cancellable */
                              error))
            return NULL;
    }

    va_end (args);
    return g_steal_pointer (&shell);
}

/**
 * cog_shell_new:
 * @name:
 *
 * Returns: (transfer full): A new #CogShell.
 */
CogShell*
cog_shell_new (GError    **error,
               const char *name,
               const char *prop_name,
               ...)
{
    va_list args;
    va_start (args, prop_name);
    CogShell *self = cog_shell_new_internal (error,
                                             name,
                                             NULL,
                                             prop_name,
                                             args);
    va_end (args);
    return self;
}

CogShell*
cog_shell_new_from_module (GError    **error,
                           const char *name,
                           const char *module_name,
                           const char *prop_name,
                           ...)
{
    g_return_val_if_fail (name != NULL, NULL);

    va_list args;
    va_start (args, prop_name);
    CogShell *self = cog_shell_new_internal (error,
                                             name,
                                             module_name,
                                             prop_name,
                                             args);
    va_end (args);
    return self;
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


static void
shell_on_notify_view_focused (CogView    *view,
                              GParamSpec *pspec,
                              CogShell   *shell)
{
    CogShellPrivate *priv = cog_shell_get_instance_private (shell);
    if (cog_view_get_focused (view) && view != priv->focused_view) {
        for (GList *item = g_list_first (priv->views); item; item = g_list_next (item)) {
            if (item->data != view)
                cog_view_set_focused (item->data, FALSE);
        }

        priv->focused_view = view;
        g_object_notify_by_pspec (G_OBJECT (shell),
                                  s_properties[PROP_FOCUSED_VIEW]);
    }
}


static void
shell_connect_view (CogShell *shell,
                    CogView  *view)
{
    CogShellPrivate *priv = cog_shell_get_instance_private (shell);

    const gboolean is_first_view = (!priv->views);
    priv->views = g_list_prepend (priv->views, g_object_ref_sink (view));

    if (is_first_view) {
        cog_view_set_focused (view, TRUE);
        priv->focused_view = view;
        g_object_notify_by_pspec (G_OBJECT (shell),
                                  s_properties[PROP_FOCUSED_VIEW]);
    }

    g_signal_connect_object (view,
                             "notify::focused",
                             G_CALLBACK (shell_on_notify_view_focused),
                             shell,
                             0);
}


static void
shell_disconnect_view (CogShell *shell,
                       CogView  *view)
{
    g_signal_handlers_disconnect_by_func (view,
                                          shell_on_notify_view_focused,
                                          shell);
}


/**
 * cog_shell_add_view:
 * @shell: A #CogShell
 * @view_name: Name for the new view.
 * @...: Properties to pass to the constructor.
 *
 * Returns: (transfer none): A new #CogView of the type needed by the shell.
 */
CogView*
cog_shell_add_view (CogShell   *shell,
                    const char *view_name,
                    const char *prop_name,
                    ...)
{
    g_return_val_if_fail (COG_IS_SHELL (shell), NULL);
    g_return_val_if_fail (view_name != NULL, NULL);

    GType view_type = cog_shell_get_view_class (shell);
    va_list varargs;
    va_start (varargs, prop_name);

    g_autoptr(CogView) view = NULL;
    if (prop_name) {
        /*
         * Gather parameters, and add our own (shell, name) to be used with
         * g_object_new_with_properties()
         */
        GObjectClass *unref_class = NULL;
        GObjectClass *view_class = g_type_class_peek_static (view_type);
        if (!view_class)
            view_class = unref_class = g_type_class_ref (view_type);

        unsigned n_properties = 2;
        g_autofree const char **property_names = g_new (const char*, n_properties);
        g_autofree GValue *property_values = g_new (GValue, n_properties);

        property_names[0] = "shell";
        property_values[0] = (GValue) G_VALUE_INIT;
        g_value_init (&property_values[0], G_TYPE_OBJECT);
        g_value_set_object (&property_values[0], shell);

        property_names[1] = "name";
        property_values[1] = (GValue) G_VALUE_INIT;
        g_value_init (&property_values[1], G_TYPE_STRING);
        g_value_set_string (&property_values[1], view_name);

        do {
            GParamSpec *pspec = g_object_class_find_property (view_class,
                                                              prop_name);
            if (!is_construct_property (G_STRFUNC,
                                        view_type,
                                        prop_name,
                                        pspec,
                                        n_properties))
                break;

            if (string_array_contains (property_names, n_properties, prop_name)) {
                g_critical ("%s: property '%s' of object class '%s' cannot be "
                            "set twice.", G_STRFUNC, pspec->name,
                            g_type_name (view_type));
                break;
            }

            property_names = g_renew (const char*, property_names, n_properties + 1);
            property_values = g_renew (GValue, property_values, n_properties + 1);

            property_names[n_properties] = prop_name;

            g_autofree char *error = NULL;
            G_VALUE_COLLECT_INIT (&property_values[n_properties],
                                  pspec->value_type, varargs,
                                  G_VALUE_NOCOPY_CONTENTS, &error);
            if (error) {
                g_critical ("%s: %s", G_STRFUNC, error);
                g_value_unset (&property_values[n_properties]);
                break;
            }

            n_properties++;
        } while ((prop_name = va_arg (varargs, const char*)));

        g_clear_pointer (&unref_class, g_type_class_unref);

        view = (CogView*) g_object_new_with_properties (view_type,
                                                        n_properties,
                                                        property_names,
                                                        property_values);
    } else {
        /* Fast case: No additional properties specified. */
        view = (CogView*) g_object_new (view_type,
                                        "shell", shell,
                                        "name", view_name,
                                        NULL);
    }

    va_end (varargs);

    shell_connect_view (shell, view);

    return g_steal_pointer (&view);
}


/**
 * cog_shell_remove_view:
 * @shell: A #CogShell
 * @name: Name of the view to remove.
 */
void
cog_shell_remove_view (CogShell   *shell,
                       const char *name)
{
    g_return_if_fail (COG_IS_SHELL (shell));
    g_return_if_fail (shell != NULL);

    CogShellPrivate *priv = cog_shell_get_instance_private (shell);
    CogView *focused_view = cog_shell_get_focused_view (shell);

    g_autoptr(CogView) view = NULL;
    GList *item;
    for (item = g_list_first (priv->views); item; item = g_list_next (item)) {
        if (strcmp (name, cog_view_get_name (item->data)) == 0) {
            view = g_steal_pointer (&item->data);
            break;
        }
    }
    g_return_if_fail (view != NULL);

    g_assert (item && !item->data);

    priv->views = g_list_remove_link (priv->views, item);
    g_clear_pointer (&item, g_list_free);

    /* Mark some other view as focused. */
    if (view == focused_view && (item = g_list_first (priv->views)))
        cog_view_set_focused ((CogView*) item->data, TRUE);

    shell_disconnect_view (shell, view);
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
        if (strcmp (name, cog_view_get_name (view)) == 0)
            return view;
    }

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

/**
 * cog_shell_get_focused_view:
 * @shell: A #CogShell
 *
 * Returns: (transfer none): The view with focus.
 */
CogView*
cog_shell_get_focused_view (CogShell *shell)
{
    g_return_val_if_fail (COG_IS_SHELL (shell), NULL);

    CogShellPrivate *priv = cog_shell_get_instance_private (shell);

    for (GList *item = g_list_first (priv->views); item; item = g_list_next (item)) {
        if (cog_view_get_focused (item->data))
            return item->data;
    }

    return NULL;
}
