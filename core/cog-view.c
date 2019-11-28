/*
 * cog-view.c
 * Copyright (C) 2019 Adrian Perez de Castro <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#include "cog-view.h"
#include "cog-shell.h"

typedef struct {
    char     *name;
    CogShell *shell;
} CogViewPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (CogView, cog_view, WEBKIT_TYPE_WEB_VIEW)

enum {
    PROP_0,
    PROP_NAME,
    PROP_SHELL,
    N_PROPERTIES,
};

static GParamSpec *s_properties[N_PROPERTIES] = { NULL, };

static void
cog_view_get_property (GObject    *object,
                       unsigned    prop_id,
                       GValue     *value,
                       GParamSpec *pspec)
{
    CogView *view = COG_VIEW (object);
    switch (prop_id) {
        case PROP_NAME:
            g_value_set_string (value, cog_view_get_name (view));
            break;
        case PROP_SHELL:
            g_value_set_object (value, cog_view_get_shell (view));
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
cog_view_set_property (GObject      *object,
                       unsigned      prop_id,
                       const GValue *value,
                       GParamSpec   *pspec)
{
    CogViewPrivate *priv = cog_view_get_instance_private (COG_VIEW (object));
    switch (prop_id) {
        case PROP_NAME:
            priv->name = g_value_dup_string (value);
            break;
        case PROP_SHELL:
            priv->shell = g_value_dup_object (value);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
cog_view_dispose (GObject *object)
{
    CogViewPrivate *priv = cog_view_get_instance_private (COG_VIEW (object));

    g_clear_pointer (&priv->name, g_free);

    G_OBJECT_CLASS (cog_view_parent_class)->dispose (object);
}

static void
cog_view_class_init (CogViewClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    object_class->get_property = cog_view_get_property;
    object_class->set_property = cog_view_set_property;
    object_class->dispose = cog_view_dispose;

    s_properties[PROP_NAME] =
        g_param_spec_string ("name",
                             "Name",
                             "Name of the CogView instance",
                             NULL,
                             G_PARAM_READWRITE |
                             G_PARAM_CONSTRUCT_ONLY |
                             G_PARAM_STATIC_STRINGS);

    s_properties[PROP_SHELL] =
        g_param_spec_object ("shell",
                             "Shell",
                             "The CogShell which created the CogView",
                             COG_TYPE_SHELL,
                             G_PARAM_READWRITE |
                             G_PARAM_CONSTRUCT_ONLY |
                             G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class,
                                       N_PROPERTIES,
                                       s_properties);
}

static void
cog_view_init (CogView *self)
{
}

/**
 * cog_view_get_name:
 * @view: A #CogView
 *
 * Returns: (transfer none): The name which identifies the view.
 */
const char*
cog_view_get_name (CogView *view)
{
    g_return_val_if_fail (COG_IS_VIEW (view), NULL);
    CogViewPrivate *priv = cog_view_get_instance_private (COG_VIEW (view));
    return priv->name;
}

/**
 * cog_view_get_shell:
 * @view: A #CogView
 *
 * Returns: (transfer none): The #CogShell which created the view.
 */
CogShell*
cog_view_get_shell (CogView *view)
{
    g_return_val_if_fail (COG_IS_VIEW (view), NULL);
    CogViewPrivate *priv = cog_view_get_instance_private (COG_VIEW (view));
    return priv->shell;
}
