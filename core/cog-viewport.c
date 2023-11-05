/*
 * Copyright (C) 2023 Adrian Perez de Castro <aperez@igalia.com>
 *
 * SPDX-License-Identifier: MIT
 */

#include "cog-viewport.h"

#include "cog-platform.h"
#include "cog-view-private.h"
#include "cog-view.h"

/**
 * CogViewport:
 *
 * Observable set of views, one of which can be visible.
 *
 * Provides a container for [class@CogView] objects, which can be
 * observed for changes by means of the [signal@CogViewport::add]
 * and [signal@CogViewport::remove] signals.
 *
 * Each view associated with the viewport can be retrieved with
 * [id@cog_viewport_get_nth_view], and the number of views obtained
 * with [id@cog_viewport_get_n_views]. These can be used to iterate
 * over the views, but for convenience the [id@cog_viewport_foreach]
 * method is provided as well.
 *
 * The [property@CogViewport:visible-view] tracks the currently
 * visible view among the ones associated with the viewport, which
 * can be manipulated with [id@cog_viewport_set_visible_view] and
 * inspected with [id@cog_viewport_get_visible_view].
 *
 * Optionally, platform plug-in implementations can provide their own
 * viewport class implementation by overriding
 * the [func@CogPlatform.get_viewport_type] method.
 *
 * Since: 0.20
 */

typedef struct {
    GPtrArray *views;
    CogView   *visible_view;
} CogViewportPrivate;

enum {
    ADD,
    REMOVE,
    N_SIGNALS,
};

static unsigned s_signals[N_SIGNALS] = {
    0,
};

enum {
    PROP_0,
    PROP_VISIBLE_VIEW,
    N_PROPERTIES,
};

static GParamSpec *s_properties[N_PROPERTIES] = {
    NULL,
};

G_DEFINE_TYPE_WITH_PRIVATE(CogViewport, cog_viewport, G_TYPE_OBJECT)

#define PRIV(obj) ((CogViewportPrivate *) cog_viewport_get_instance_private((CogViewport *) obj))

static void *
_cog_viewport_get_impl_type_init(GType *type)
{
    *type = COG_TYPE_VIEWPORT;

    CogPlatform *platform = cog_platform_get();
    if (platform) {
        CogPlatformClass *platform_class = COG_PLATFORM_GET_CLASS(platform);
        if (platform_class->get_viewport_type) {
            GType viewport_type = (*platform_class->get_viewport_type)();
            *type = viewport_type;
        }
    }

    g_debug("%s: using %s", G_STRFUNC, g_type_name(*type));
    return NULL;
}

static void
cog_viewport_set_property(GObject *object, unsigned prop_id, const GValue *value, GParamSpec *pspec)
{
    CogViewport *self = COG_VIEWPORT(object);
    switch (prop_id) {
    case PROP_VISIBLE_VIEW:
        cog_viewport_set_visible_view(self, g_value_get_object(value));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    }
}

static void
cog_viewport_get_property(GObject *object, unsigned prop_id, GValue *value, GParamSpec *pspec)
{
    CogViewport *self = COG_VIEWPORT(object);
    switch (prop_id) {
    case PROP_VISIBLE_VIEW:
        g_value_set_object(value, cog_viewport_get_visible_view(self));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    }
}

/**
 * cog_viewport_get_impl_type:
 *
 * Get the type of the viewport implementation in use.
 *
 * This function always returns a valid type. If the active
 * [class@CogPlatform] does not provide a custom viewport type, the default
 * built-in type included as part of the Cog core library is returned.
 *
 * When creating viewports, this function must be used to retrieve the
 * type to use, e.g.:
 *
 * ```c
 * CogViewport *view = g_object_new(cog_viewport_get_impl_type(), NULL);
 * ```
 *
 * In most cases it should be possible to use the convenience function
 * [ctor@Viewport.new], which uses this function internally.
 *
 * Returns: Type of the view implementation in use.
 */
GType
cog_viewport_get_impl_type(void)
{
    static GType viewport_type;
    static GOnce once = G_ONCE_INIT;

    g_once(&once, (GThreadFunc) _cog_viewport_get_impl_type_init, &viewport_type);

    return viewport_type;
}

/**
 * cog_viewport_new: (constructor)
 *
 * Creates a new instance of a viewport implementation and sets its properties.
 *
 * Returns: (transfer full): A new web view.
 */
CogViewport *
cog_viewport_new(void)
{
    return g_object_new(cog_viewport_get_impl_type(), NULL);
}

static void
cog_viewport_dispose(GObject *object)
{
    g_ptr_array_set_size(PRIV(object)->views, 0);

    G_OBJECT_CLASS(cog_viewport_parent_class)->dispose(object);
}

static void
cog_viewport_finalize(GObject *object)
{
    g_ptr_array_free(PRIV(object)->views, TRUE);

    G_OBJECT_CLASS(cog_viewport_parent_class)->finalize(object);
}

static void
cog_viewport_class_init(CogViewportClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->set_property = cog_viewport_set_property;
    object_class->get_property = cog_viewport_get_property;
    object_class->dispose = cog_viewport_dispose;
    object_class->finalize = cog_viewport_finalize;

    /**
     * CogViewport::visible-view: (attributes org.gtk.Property.get=cog_viewport_get_visible_view org.gtk.Property.set=cog_viewport_set_visible_view) (setter set_visible_view) (getter get_visible_view):
     *
     * The web view currently visible in the viewport.
     */
    s_properties[PROP_VISIBLE_VIEW] =
        g_param_spec_object("visible-view", NULL, NULL, COG_TYPE_VIEW,
                            G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties(object_class, N_PROPERTIES, s_properties);

    /**
     * CogViewport::add:
     * @self: The viewport where the view was added.
     * @view: The view that was added to the viewport.
     * @user_data: User data.
     *
     * Emitted after a view has been added to the viewport.
     */
    s_signals[ADD] =
        g_signal_new("add", COG_TYPE_VIEWPORT, G_SIGNAL_RUN_FIRST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1, COG_TYPE_VIEW);

    /**
     * CogViewport::remove:
     * @self: The viewport from which the view was removed.
     * @view: The view that was removed from the viewport.
     * @user_data: User data.
     *
     * Emitted after a view has been removed from the viewport.
     */
    s_signals[REMOVE] = g_signal_new("remove", COG_TYPE_VIEWPORT, G_SIGNAL_RUN_FIRST, 0, NULL, NULL, NULL, G_TYPE_NONE,
                                     1, COG_TYPE_VIEW);
}

static void
cog_viewport_init(CogViewport *self)
{
    PRIV(self)->views = g_ptr_array_new_full(3, g_object_unref);
}

static void
cog_viewport_set_visible_view_internal(CogViewport *self, CogViewportPrivate *priv, CogView *view)
{
    if (priv->visible_view == view)
        return;

    if (priv->visible_view) {
        struct wpe_view_backend *backend = cog_view_get_backend(priv->visible_view);
        wpe_view_backend_remove_activity_state(backend, wpe_view_activity_state_visible);
        wpe_view_backend_remove_activity_state(backend, wpe_view_activity_state_focused);
    }

    priv->visible_view = view;
    g_object_notify_by_pspec(G_OBJECT(self), s_properties[PROP_VISIBLE_VIEW]);

    if (priv->visible_view) {
        struct wpe_view_backend *backend = cog_view_get_backend(priv->visible_view);
        wpe_view_backend_add_activity_state(backend, wpe_view_activity_state_visible);
    }
}

/**
 * cog_viewport_add:
 * @self: Viewport to add the view to.
 * @view: The view to add.
 *
 * Adds a view to a viewport.
 *
 * The next available index will be assigned to the view.
 */
void
cog_viewport_add(CogViewport *self, CogView *view)
{
    g_return_if_fail(COG_IS_VIEWPORT(self));
    g_return_if_fail(COG_IS_VIEW(view));

    CogViewportPrivate *priv = PRIV(self);
    g_return_if_fail(!g_ptr_array_find(priv->views, view, NULL));

    cog_view_set_viewport(view, self);

    g_ptr_array_add(priv->views, g_object_ref(view));
    g_signal_emit(self, s_signals[ADD], 0, view);

    struct wpe_view_backend *backend = cog_view_get_backend(view);
    wpe_view_backend_add_activity_state(backend, wpe_view_activity_state_in_window);

    g_autoptr(WebKitInputMethodContext) im_context = cog_platform_create_im_context(self);
    webkit_web_view_set_input_method_context(WEBKIT_WEB_VIEW(view), im_context);

    if (!priv->visible_view) {
        g_debug("%s<%p>: adding view %p as visible", G_STRFUNC, self, view);
        cog_viewport_set_visible_view_internal(self, priv, view);
    } else {
        g_debug("%s<%p>: adding view %p as invisible", G_STRFUNC, self, view);
        wpe_view_backend_remove_activity_state(backend, wpe_view_activity_state_visible);
        wpe_view_backend_remove_activity_state(backend, wpe_view_activity_state_focused);
    }
}

/**
 * cog_viewport_remove:
 * @self: Viewport to remove the view from.
 * @view: The view to remove.
 *
 * Removes a view from the viewport.
 *
 * Removing a view preserves the relative ordering of the rest of views in
 * the viewport. This also means that the index used to retrieve them may
 * change after removal.
 *
 * After removal, [property@CogViewport:visible-view] is updated if the
 * view being removed was visible. If there are other views left associated
 * with the viewport, the one at index zero will be set as visible; otherwise
 * if there are no views left, the property is set to %NULL.
 */
void
cog_viewport_remove(CogViewport *self, CogView *view)
{
    g_return_if_fail(COG_IS_VIEWPORT(self));
    g_return_if_fail(COG_IS_VIEW(view));

    CogViewportPrivate *priv = PRIV(self);

    unsigned index;
    if (!g_ptr_array_find(priv->views, view, &index)) {
        g_warning("Attempted to remove view %p, which was not in viewport %p.", view, self);
        return;
    }

    webkit_web_view_set_input_method_context(WEBKIT_WEB_VIEW(view), NULL);

    cog_view_set_viewport(view, NULL);

    g_object_ref(view);
    g_ptr_array_remove_index(priv->views, index);
    g_signal_emit(self, s_signals[REMOVE], 0, view);

    if (priv->visible_view == view) {
        CogView *new_visible = priv->views->len ? g_ptr_array_index(priv->views, 0) : NULL;
        g_debug("%s<%p>: view %p removed, %p now visible", G_STRFUNC, self, view, new_visible);
        cog_viewport_set_visible_view_internal(self, priv, new_visible);
    } else {
        g_debug("%s<%p>: view %p removed, none visible", G_STRFUNC, self, view);
    }

    struct wpe_view_backend *backend = cog_view_get_backend(view);
    wpe_view_backend_remove_activity_state(backend, wpe_view_activity_state_in_window);
    wpe_view_backend_remove_activity_state(backend, wpe_view_activity_state_focused);

    g_object_unref(view);
}

/**
 * cog_viewport_contains:
 * @self: Viewport.
 * @view: The view to check.
 *
 * Checks whether a viewport contains a given view.
 *
 * Returns: Whether the @view is contained in the viewport.
 */
gboolean
cog_viewport_contains(CogViewport *self, CogView *view)
{
    g_return_val_if_fail(COG_IS_VIEWPORT(self), FALSE);
    g_return_val_if_fail(COG_IS_VIEW(view), FALSE);

    return g_ptr_array_find(PRIV(self)->views, view, NULL);
}

/**
 * cog_viewport_foreach:
 * @self: Viewport.
 * @func: (scope call): Callback function.
 * @user_data: User data passed to the callback.
 *
 * Applies a function to each view in the viewport.
 */
void
cog_viewport_foreach(CogViewport *self, GFunc func, void *user_data)
{
    g_return_if_fail(COG_IS_VIEWPORT(self));
    g_return_if_fail(func != NULL);

    g_ptr_array_foreach(PRIV(self)->views, func, user_data);
}

/**
 * cog_viewport_get_n_views:
 * @self: Viewport.
 *
 * Gets the number of views in a viewport.
 *
 * Returns: Number of views.
 */
gsize
cog_viewport_get_n_views(CogViewport *self)
{
    g_return_val_if_fail(COG_IS_VIEWPORT(self), 0);

    return PRIV(self)->views->len;
}

/**
 * cog_viewport_get_nth_view:
 * @self: Viewport.
 * @index: Index of the view to get.
 *
 * Gets a view from the viewport given its index.
 *
 * This is typically used along [method@CogViewport.get_n_views] to iterate
 * over the views:
 *
 * ```c
 * CogViewport *viewport = get_viewport();
 * for (gsize i = 0; i < cog_viewport_get_n_views(viewport); i++)
 *     handle_view(cog_viewport_get_nth_view(viewport, i));
 * ```
 *
 * Returns: (transfer none): View at the given @index.
 */
CogView *
cog_viewport_get_nth_view(CogViewport *self, gsize index)
{
    g_return_val_if_fail(COG_IS_VIEWPORT(self), NULL);

    CogViewportPrivate *priv = PRIV(self);
    g_return_val_if_fail(index < priv->views->len, NULL);

    return g_ptr_array_index(priv->views, index);
}

/**
 * cog_viewport_get_visible_view: (get-property visible-view)
 * @self: Viewport.
 *
 * Gets the visible view.
 *
 * Note that there is no visible view when none is contained in the viewport.
 * In this case %NULL is returned.
 *
 * Returns: (transfer none) (nullable): Visible view, or %NULL.
 */
CogView *
cog_viewport_get_visible_view(CogViewport *self)
{
    g_return_val_if_fail(COG_IS_VIEWPORT(self), NULL);
    return PRIV(self)->visible_view;
}

/**
 * cog_viewport_set_visible_view: (set-property visible-view)
 * @self: Viewport.
 * @view: The view to set as visible.
 *
 * Sets the visible view for the viewport.
 *
 * The view set as currently visible will gets its state flags
 * %wpe_view_activity_state_visible set; and the previously visible
 * one (if any) will get the flag removed, as well as the
 * %wpe_view_activity_state_focused removed, too.
 *
 * Note that the %wpe_view_activity_state_focused flag **will not be
 * enabled** for the view that was made visible. The reasons is that
 * in theory only one view should have this flag at a time, and there
 * is no way to determine whether a focused view may already be present
 * in another viewport.
 */
void
cog_viewport_set_visible_view(CogViewport *self, CogView *view)
{
    g_return_if_fail(COG_IS_VIEWPORT(self));
    g_return_if_fail(COG_IS_VIEW(view));
    g_return_if_fail(cog_viewport_contains(self, view));

    cog_viewport_set_visible_view_internal(self, PRIV(self), view);
}
