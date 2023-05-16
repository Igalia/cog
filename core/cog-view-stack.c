/*
 * cog-view-stack.c
 * Copyright (C) 2023 Igalia S.L.
 *
 * SPDX-License-Identifier: MIT
 */

#include "cog-view-stack.h"
#include "cog-view.h"

/**
 * CogViewStack:
 *
 * Group of views which tracks a visible view.
 *
 * Since: 0.18
 */

struct _CogViewStack {
    CogViewGroup parent;
    CogView     *visible_view;
    gulong       add_handler;
    gulong       remove_handler;
};

G_DEFINE_TYPE(CogViewStack, cog_view_stack, COG_TYPE_VIEW_GROUP)

enum {
    PROP_0,
    PROP_VISIBLE_VIEW,
    N_PROPERTIES,
};

static GParamSpec *s_properties[N_PROPERTIES] = {
    NULL,
};

static void
cog_view_stack_get_property(GObject *obj, unsigned prop_id, GValue *value, GParamSpec *pspec)
{
    CogViewStack *self = COG_VIEW_STACK(obj);
    switch (prop_id) {
    case PROP_VISIBLE_VIEW:
        g_value_set_object(value, cog_view_stack_get_visible_view(self));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, prop_id, pspec);
    }
}

static void
cog_view_stack_set_property(GObject *obj, unsigned prop_id, const GValue *value, GParamSpec *pspec)
{
    CogViewStack *self = COG_VIEW_STACK(obj);
    switch (prop_id) {
    case PROP_VISIBLE_VIEW:
        cog_view_stack_set_visible_view(self, g_value_get_object(value));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, prop_id, pspec);
    }
}

static void
cog_view_stack_dispose(GObject *obj)
{
    CogViewStack *self = COG_VIEW_STACK(obj);

    if (self->add_handler) {
        g_signal_handler_disconnect(self, self->add_handler);
        self->add_handler = 0;
    }
    if (self->remove_handler) {
        g_signal_handler_disconnect(self, self->remove_handler);
        self->remove_handler = 0;
    }

    G_OBJECT_CLASS(cog_view_stack_parent_class)->dispose(obj);
}

static void
cog_view_stack_class_init(CogViewStackClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->set_property = cog_view_stack_set_property;
    object_class->get_property = cog_view_stack_get_property;
    object_class->dispose = cog_view_stack_dispose;

    /**
     * CogViewStack:visible-view: (attributes org.gtk.Property.get=cog_view_stack_get_visible_view org.gtk.Property.set=cog_view_stack_set_visible_view) (setter get_visible_view) (getter get_visible_view)
     *
     * The web view currently visible from the stacked group.
     *
     * Since: 0.18
     */
    s_properties[PROP_VISIBLE_VIEW] = g_param_spec_object(
        "visible-view", NULL, NULL, COG_TYPE_VIEW, G_PARAM_READWRITE | G_PARAM_STATIC_NAME | G_PARAM_EXPLICIT_NOTIFY);

    g_object_class_install_properties(object_class, N_PROPERTIES, s_properties);
}

static void
cog_view_stack_set_visible_view_internal(CogViewStack *self, CogView *view)
{
    if (self->visible_view == view)
        return;

    if (self->visible_view) {
        struct wpe_view_backend *backend = cog_view_get_backend(self->visible_view);
        wpe_view_backend_remove_activity_state(backend, wpe_view_activity_state_visible);
        wpe_view_backend_remove_activity_state(backend, wpe_view_activity_state_focused);
    }

    self->visible_view = view;
    g_object_notify_by_pspec(G_OBJECT(self), s_properties[PROP_VISIBLE_VIEW]);

    if (self->visible_view) {
        struct wpe_view_backend *backend = cog_view_get_backend(self->visible_view);
        wpe_view_backend_add_activity_state(backend, wpe_view_activity_state_visible);
        wpe_view_backend_add_activity_state(backend, wpe_view_activity_state_focused);
    }
}

static void
cog_view_stack_handle_add(CogViewStack *self, CogView *view)
{
    if (!self->visible_view) {
        g_debug("%s<%p>: adding view %p as visible", G_STRFUNC, self, view);
        cog_view_stack_set_visible_view_internal(self, view);
    } else {
        g_debug("%s<%p>: adding view %p as invisible", G_STRFUNC, self, view);
        struct wpe_view_backend *backend = cog_view_get_backend(view);
        wpe_view_backend_remove_activity_state(backend, wpe_view_activity_state_visible);
        wpe_view_backend_remove_activity_state(backend, wpe_view_activity_state_focused);
    }
}

static void
cog_view_stack_handle_remove(CogViewStack *self, CogView *view)
{
    if (self->visible_view == view) {
        CogView *visible = cog_view_group_get_n_views(COG_VIEW_GROUP(self))
                               ? COG_VIEW(cog_view_group_get_nth_view(COG_VIEW_GROUP(self), 0))
                               : NULL;
        g_debug("%s: self %p, view %p, now visible %p", G_STRFUNC, self, view, visible);
        cog_view_stack_set_visible_view_internal(self, visible);
    } else {
        g_debug("%s: self %p, view %p", G_STRFUNC, self, view);
    }
}

static void
cog_view_stack_init(CogViewStack *self)
{
    self->add_handler = g_signal_connect(self, "add", G_CALLBACK(cog_view_stack_handle_add), NULL);
    self->remove_handler = g_signal_connect(self, "remove", G_CALLBACK(cog_view_stack_handle_remove), NULL);
}

/**
 * cog_view_stack_set_visible_view: (set-property visible-view)
 * @self: View stack.
 * @view: The view to set as visible.
 *
 * Sets the visible view for the stack.
 *
 * Since: 0.18
 */
void
cog_view_stack_set_visible_view(CogViewStack *self, CogView *view)
{
    g_return_if_fail(COG_IS_VIEW_STACK(self));
    g_return_if_fail(COG_IS_VIEW(view));
    g_return_if_fail(cog_view_group_contains(COG_VIEW_GROUP(self), view));

    cog_view_stack_set_visible_view_internal(self, view);
}

/**
 * cog_view_stack_get_visible_view: (get-property visible-view)
 * @self: A view stack.
 *
 * Gets the visible view.
 *
 * Note that there is no visible view when the stack is empty. In this
 * case %NULL is returned.
 *
 * Returns: (transfer none) (nullable): Visible view, or %NULL.
 *
 * Since: 0.18
 */
CogView *
cog_view_stack_get_visible_view(CogViewStack *self)
{
    g_return_val_if_fail(COG_IS_VIEW_STACK(self), NULL);
    return self->visible_view;
}

/**
 * cog_view_stack_new: (constructor)
 *
 * Creates a new view stack.
 *
 * Returns: (transfer full): A new view stack.
 *
 * Since: 0.18
 */
CogViewStack *
cog_view_stack_new(void)
{
    return g_object_new(COG_TYPE_VIEW_STACK, NULL);
}
