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
    PROP_VISIBLE,
    PROP_FOCUSED,
    PROP_IN_WINDOW,
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
        case PROP_VISIBLE:
            g_value_set_boolean (value, cog_view_get_visible (view));
            break;
        case PROP_FOCUSED:
            g_value_set_boolean (value, cog_view_get_focused (view));
            break;
        case PROP_IN_WINDOW:
            g_value_set_boolean (value, cog_view_get_in_window (view));
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
    CogView *self = COG_VIEW (object);
    CogViewPrivate *priv = cog_view_get_instance_private (self);
    switch (prop_id) {
        case PROP_NAME:
            priv->name = g_value_dup_string (value);
            break;
        case PROP_SHELL:
            priv->shell = g_value_dup_object (value);
            break;
        case PROP_VISIBLE:
            cog_view_set_visible (self, g_value_get_boolean (value));
            break;
        case PROP_FOCUSED:
            cog_view_set_focused (self, g_value_get_boolean (value));
            break;
        case PROP_IN_WINDOW:
            cog_view_set_in_window (self, g_value_get_boolean (value));
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static GObject*
cog_view_constructor (GType                  type,
                      unsigned               n_properties,
                      GObjectConstructParam *properties)
{
    GObject *obj = G_OBJECT_CLASS (cog_view_parent_class)->constructor (type,
                                                                        n_properties,
                                                                        properties);
    CogViewClass *view_class = COG_VIEW_GET_CLASS (obj);
    if (view_class->setup)
        view_class->setup (COG_VIEW (obj));

    return obj;
}

static void
cog_view_constructed (GObject *object)
{
    G_OBJECT_CLASS (cog_view_parent_class)->constructed (object);

    webkit_web_view_load_html (WEBKIT_WEB_VIEW (object),
                               "<!DOCTYPE html><html><body></body></html>",
                               "about:blank");
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
    object_class->constructor = cog_view_constructor;
    object_class->constructed = cog_view_constructed;
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

    s_properties[PROP_VISIBLE] =
        g_param_spec_boolean ("visible",
                              "View is visible",
                              "The view is abeing displayed",
                              FALSE,
                              G_PARAM_READWRITE |
                              G_PARAM_STATIC_STRINGS);

    s_properties[PROP_FOCUSED] =
        g_param_spec_boolean ("focused",
                              "View is focused",
                              "The view has the input focus",
                              FALSE,
                              G_PARAM_READWRITE |
                              G_PARAM_STATIC_STRINGS);

    s_properties[PROP_IN_WINDOW] =
        g_param_spec_boolean ("in-window",
                              "View is inside a window",
                              "The view is inside an application window",
                              FALSE,
                              G_PARAM_READWRITE |
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
    CogViewPrivate *priv = cog_view_get_instance_private (view);
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
    CogViewPrivate *priv = cog_view_get_instance_private (view);
    return priv->shell;
}

#define COG_MAKE_ACTIVITY_STATE_GETTER(state_name)                 \
    g_return_val_if_fail (COG_IS_VIEW (view), FALSE);              \
    struct wpe_view_backend *backend =                             \
        webkit_web_view_backend_get_wpe_backend (                  \
            webkit_web_view_get_backend (WEBKIT_WEB_VIEW (view))); \
    return (wpe_view_backend_get_activity_state (backend)          \
            & wpe_view_activity_state_ ## state_name)

#define COG_MAKE_ACTIVITY_STATE_SETTER(state_name, caps_state_name)                      \
    g_return_if_fail (COG_IS_VIEW (view));                                               \
    struct wpe_view_backend *backend =                                                   \
        webkit_web_view_backend_get_wpe_backend (                                        \
            webkit_web_view_get_backend (WEBKIT_WEB_VIEW (view)));                       \
    const bool was_ ## state_name = (wpe_view_backend_get_activity_state (backend)       \
                                     & wpe_view_activity_state_ ## state_name);          \
    if (was_ ## state_name == state_name)                                                \
        return;                                                                          \
    if (state_name) {                                                                    \
        wpe_view_backend_add_activity_state (backend,                                    \
                                             wpe_view_activity_state_ ## state_name);    \
    } else {                                                                             \
        wpe_view_backend_remove_activity_state (backend,                                 \
                                                wpe_view_activity_state_ ## state_name); \
    }                                                                                    \
    g_object_notify_by_pspec (G_OBJECT (view), s_properties[PROP_ ## caps_state_name])

/**
 * cog_view_get_active:
 * @view: A #CogView
 *
 * Returns: Whether the view is being actively displayed.
 */
gboolean
cog_view_get_visible (CogView *view)
{
    COG_MAKE_ACTIVITY_STATE_GETTER (visible);
}

/**
 * cog_view_set_active:
 * @view: A #CogView
 * @visible: Whether the view should is visible.
 */
void
cog_view_set_visible (CogView *view,
                      gboolean visible)
{
    COG_MAKE_ACTIVITY_STATE_SETTER (visible, VISIBLE);
}

/**
 * cog_view_get_focused:
 * @view: A #CogView
 *
 * Returns: Whether the view has the input focus.
 */
gboolean
cog_view_get_focused (CogView *view)
{
    COG_MAKE_ACTIVITY_STATE_GETTER (focused);
}

/**
 * cog_view_set_focused:
 * @view: A #CogView
 * @focused: Whether to give focus to the view.
 */
void
cog_view_set_focused (CogView *view,
                      gboolean focused)
{
    /* XXX: Should only one single view be forced as focused? */
    COG_MAKE_ACTIVITY_STATE_SETTER (focused, FOCUSED);
}

/**
 * cog_view_get_in_window:
 * @view: A #CogView
 * 
 * Returns: Whether the view is inside an application window.
 */
gboolean
cog_view_get_in_window (CogView *view)
{
    COG_MAKE_ACTIVITY_STATE_GETTER (in_window);
}

/**
 * cog_view_set_in_window:
 * @view: A #CogView
 * @in_window: Whether the view is inside an application window.
 */
void
cog_view_set_in_window (CogView *view,
                        gboolean in_window)
{
    COG_MAKE_ACTIVITY_STATE_SETTER (in_window, IN_WINDOW);
}

#undef COG_MAKE_ACTIVITY_STATE_SETTER
#undef COG_MAKE_ACTIVITY_STATE_GETTER

/**
 * cog_view_get_backend:
 * @view: A #CogView
 *
 * Returns: The WPE view backend for the view.
 */
struct wpe_view_backend*
cog_view_get_backend (CogView *self)
{
    g_return_val_if_fail (COG_IS_VIEW (self), NULL);
    return webkit_web_view_backend_get_wpe_backend (webkit_web_view_get_backend ((WebKitWebView*) self));
}
