/*
 * cog-view-group.c
 * Copyright (C) 2023 Igalia S.L.
 *
 * SPDX-License-Identifier: MIT
 */

#include "cog-view-group.h"
#include "cog-view.h"

/**
 * CogViewGroup:
 *
 * Convenience observable container for a set of views.
 *
 * Provides a container for [class@CogView] objects, which can be
 * observed for changes by means of the [signal@CogViewGroup::add]
 * and [signal@CogViewGroup::remove] signals.
 *
 * Each view can be retrieved with [id@cog_view_group_get_nth_view],
 * and the number of views obtained with [id@cog_view_group_get_n_views].
 * These can be used to iterate over the views in a group, but for
 * convenience the [id@cog_view_group_foreach] method is provided as well.
 *
 * Since: 0.18
 */

typedef struct {
    GObject    parent_instance;
    GPtrArray *views;
} CogViewGroupPrivate;

G_DEFINE_TYPE_WITH_PRIVATE(CogViewGroup, cog_view_group, G_TYPE_OBJECT)

#define PRIV(obj) ((CogViewGroupPrivate *) cog_view_group_get_instance_private(COG_VIEW_GROUP(obj)))

enum {
    ADD,
    REMOVE,
    N_SIGNALS,
};

static unsigned s_signals[N_SIGNALS] = {
    0,
};

static void
cog_view_group_dispose(GObject *obj)
{
    g_ptr_array_set_size(PRIV(obj)->views, 0);

    G_OBJECT_CLASS(cog_view_group_parent_class)->dispose(obj);
}

static void
cog_view_group_finalize(GObject *obj)
{
    g_ptr_array_free(PRIV(obj)->views, TRUE);

    G_OBJECT_CLASS(cog_view_group_parent_class)->finalize(obj);
}

static void
cog_view_group_class_init(CogViewGroupClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = cog_view_group_dispose;
    object_class->finalize = cog_view_group_finalize;

    /**
     * CogViewGroup::add:
     * @self: The group where the view was added.
     * @view: The web view that was added to the group.
     * @user_data: User data.
     *
     * Emitted after a view has been added to the group.
     *
     * Since: 0.18
     */
    s_signals[ADD] = g_signal_new("add", COG_TYPE_VIEW_GROUP, G_SIGNAL_RUN_FIRST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1,
                                  WEBKIT_TYPE_WEB_VIEW);

    /**
     * CogViewGroup::remove:
     * @self: The group where the view was removed.
     * @view: The web view that was removed from the group.
     * @user_data: User data.
     *
     * Emitted after a view was removed from the group.
     *
     * Since: 0.18
     */
    s_signals[REMOVE] = g_signal_new("remove", COG_TYPE_VIEW_GROUP, G_SIGNAL_RUN_FIRST, 0, NULL, NULL, NULL,
                                     G_TYPE_NONE, 1, WEBKIT_TYPE_WEB_VIEW);
}

static void
cog_view_group_init(CogViewGroup *self)
{
    PRIV(self)->views = g_ptr_array_new_full(3, g_object_unref);
}

/**
 * cog_view_group_add:
 * @self: Group to add the view to.
 * @view: The view to add.
 *
 * Adds a view to a group.
 *
 * The next available index will be assigned to the view.
 *
 * Since: 0.18
 */
void
cog_view_group_add(CogViewGroup *self, CogView *view)
{
    g_return_if_fail(COG_IS_VIEW_GROUP(self));
    g_return_if_fail(COG_IS_VIEW(view));

    CogViewGroupPrivate *priv = PRIV(self);
    g_return_if_fail(!g_ptr_array_find(priv->views, view, NULL));

    g_ptr_array_add(priv->views, g_object_ref(view));
    g_signal_connect_swapped(view, "close", G_CALLBACK(cog_view_group_remove), self);
    g_signal_emit(self, s_signals[ADD], 0, view);
}

/**
 * cog_view_group_remove:
 * @self: Group to remove the view from.
 * @view: The view to remove.
 *
 * Removes a view from the group.
 *
 * Removing a view preserves the relative ordering of the rest of views
 * in the group. This also means that the index used to retrieve them
 * may change after a removal.
 *
 * Since: 0.18
 */
void
cog_view_group_remove(CogViewGroup *self, CogView *view)
{
    g_return_if_fail(COG_IS_VIEW_GROUP(self));
    g_return_if_fail(COG_IS_VIEW(view));

    CogViewGroupPrivate *priv = PRIV(self);

    unsigned index;
    if (!g_ptr_array_find(priv->views, view, &index)) {
        g_warning("Attempted to remove view %p, which was not in the group.", view);
        return;
    }

    g_ptr_array_remove_index(priv->views, index);
    g_signal_emit(self, s_signals[REMOVE], 0, view);
}

/**
 * cog_view_group_contains:
 * @self: Group of views.
 * @view: The view to check.
 *
 * Checks whether a group contains a given view.
 *
 * Returns: Whether the @view is contained in the group.
 *
 * Since: 0.18
 */
gboolean
cog_view_group_contains(CogViewGroup *self, CogView *view)
{
    g_return_val_if_fail(COG_IS_VIEW_GROUP(self), FALSE);
    g_return_val_if_fail(COG_IS_VIEW(view), FALSE);

    return g_ptr_array_find(PRIV(self)->views, view, NULL);
}

/**
 * cog_view_group_foreach:
 * @self: Group of views.
 * @func: (scope call): Callback function.
 * @userdata: User data passed to the callback.
 *
 * Applies a function to each view in the group.
 *
 * Since: 0.18
 */
void
cog_view_group_foreach(CogViewGroup *self, GFunc func, void *userdata)
{
    g_return_if_fail(COG_IS_VIEW_GROUP(self));
    g_return_if_fail(func);

    g_ptr_array_foreach(PRIV(self)->views, func, userdata);
}

/**
 * cog_view_group_get_n_views:
 * @self: A view group.
 *
 * Gets the number of views in a group.
 *
 * Returns: Number of views.
 *
 * Since: 0.18
 */
gsize
cog_view_group_get_n_views(CogViewGroup *self)
{
    g_return_val_if_fail(COG_IS_VIEW_GROUP(self), 0);

    return PRIV(self)->views->len;
}

/**
 * cog_view_group_get_nth_view:
 * @self: A view group.
 * @index: Index of the view.
 *
 * Gets a view from the group given its index.
 *
 * This is typically used along [method@CogViewGroup.get_n_views] to iterate
 * over the views:
 *
 * ```c
 * CogViewGroup *group = cog_shell_get_view_group(shell);
 * for (gsize i = 0; i < cog_view_group_get_n_views(group); i++)
 *     handle_view(cog_view_group_get_nth_view(group, i));
 * ```
 *
 * Returns: (transfer none): View at the given @index.
 *
 * Since: 0.18
 */
CogView *
cog_view_group_get_nth_view(CogViewGroup *self, gsize index)
{
    g_return_val_if_fail(COG_IS_VIEW_GROUP(self), NULL);

    CogViewGroupPrivate *priv = PRIV(self);
    g_return_val_if_fail(index < priv->views->len, NULL);

    return g_ptr_array_index(priv->views, index);
}

/**
 * cog_view_group_new: (constructor)
 *
 * Creates a new view group.
 *
 * Returns: (transfer full): A new view group.
 */
CogViewGroup *
cog_view_group_new(void)
{
    return g_object_new(COG_TYPE_VIEW_GROUP, NULL);
}
