/*
 * cog-viewport-wl.c
 * Copyright (C) 2023 Pablo Saavedra <psaavedra@igalia.com>
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>

#include "../../core/cog.h"

#include "cog-utils-wl.h"
#include "cog-view-wl.h"
#include "cog-viewport-wl.h"

#include "fullscreen-shell-unstable-v1-client.h"
#include "xdg-shell-client.h"

G_DEFINE_DYNAMIC_TYPE(CogWlViewport, cog_wl_viewport, COG_TYPE_VIEWPORT)

static void cog_wl_viewport_dispose(GObject *);
static void cog_wl_viewport_on_add(CogWlViewport *, CogView *);
static void destroy_window(CogWlViewport *);
static void noop();

#if COG_ENABLE_WESTON_DIRECT_DISPLAY
static void
destroy_video_surface(gpointer data)
{
    struct video_surface *surface = (struct video_surface *) data;

#    if COG_ENABLE_WESTON_CONTENT_PROTECTION
    g_clear_pointer(&surface->protected_surface, weston_protected_surface_destroy);
#    endif
    g_clear_pointer(&surface->wl_subsurface, wl_subsurface_destroy);
    g_clear_pointer(&surface->wl_surface, wl_surface_destroy);
    g_slice_free(struct video_surface, surface);
}
#endif

static void
destroy_window(CogWlViewport *viewport)
{
    g_clear_pointer(&viewport->window.xdg_toplevel, xdg_toplevel_destroy);
    g_clear_pointer(&viewport->window.xdg_surface, xdg_surface_destroy);
    g_clear_pointer(&viewport->window.shell_surface, wl_shell_surface_destroy);
    g_clear_pointer(&viewport->window.wl_surface, wl_surface_destroy);

#if COG_ENABLE_WESTON_DIRECT_DISPLAY
    g_clear_pointer(&viewport->window.video_surfaces, g_hash_table_destroy);
#endif
}

static bool
getenv_uint32(const char *env_var_name, uint32_t *result)
{
    const char *env_string = g_getenv(env_var_name);
    if (!env_string)
        return false;

    char    *endptr = NULL;
    uint64_t value = g_ascii_strtoull(env_string, &endptr, 0);

    if (value == UINT64_MAX && errno == ERANGE)
        return false;
    if (value == 0 && errno == EINVAL)
        return false;
    if (value > UINT32_MAX)
        return false;
    if (*endptr != '\0')
        return false;

    *result = (uint32_t) value;
    return true;
}

static void
noop()
{
}

static void
shell_surface_on_ping(void *data, struct wl_shell_surface *shell_surface, uint32_t serial)
{
    wl_shell_surface_pong(shell_surface, serial);
}

static void
shell_surface_on_configure(void                    *data,
                           struct wl_shell_surface *shell_surface,
                           uint32_t                 edges,
                           int32_t                  width,
                           int32_t                  height)
{
    CogWlViewport *viewport = data;

    cog_wl_viewport_configure_geometry(viewport, width, height);

    g_debug("New wl_shell configuration: (%" PRIu32 ", %" PRIu32 ")", width, height);
}

static const struct wl_shell_surface_listener shell_surface_listener = {
    .ping = shell_surface_on_ping,
    .configure = shell_surface_on_configure,
};

static void
surface_on_enter(void *data, struct wl_surface *surface, struct wl_output *output)
{
    CogWlViewport *viewport = data;
    CogWlPlatform *platform = (CogWlPlatform *) cog_platform_get();
    CogWlDisplay  *display = platform->display;

    if (display->current_output->output != output) {
        g_debug("%s: Surface %p output changed %p -> %p", G_STRFUNC, surface, display->current_output->output, output);
        display->current_output = cog_wl_display_find_output(platform->display, output);
    }

#ifdef WL_SURFACE_SET_BUFFER_SCALE_SINCE_VERSION
    const bool can_set_surface_scale = wl_surface_get_version(surface) >= WL_SURFACE_SET_BUFFER_SCALE_SINCE_VERSION;
    if (can_set_surface_scale)
        wl_surface_set_buffer_scale(surface, display->current_output->scale);
    else
        g_debug("%s: Surface %p uses old protocol version, cannot set scale factor", G_STRFUNC, surface);
#endif /* WL_SURFACE_SET_BUFFER_SCALE_SINCE_VERSION */

    for (unsigned i = 0; i < cog_viewport_get_n_views(COG_VIEWPORT(viewport)); i++) {
        struct wpe_view_backend *backend = cog_view_get_backend(cog_viewport_get_nth_view(COG_VIEWPORT(viewport), i));

        wpe_view_backend_set_target_refresh_rate(backend, display->current_output->refresh);

#ifdef WL_SURFACE_SET_BUFFER_SCALE_SINCE_VERSION
        if (can_set_surface_scale)
            wpe_view_backend_dispatch_set_device_scale_factor(backend, display->current_output->scale);
#endif /* WL_SURFACE_SET_BUFFER_SCALE_SINCE_VERSION */
    }
}

static const struct wl_surface_listener surface_listener = {
    .enter = surface_on_enter,
    .leave = noop,
};

static void
xdg_surface_on_configure(void *data, struct xdg_surface *surface, uint32_t serial)
{
    xdg_surface_ack_configure(surface, serial);
}

static void
xdg_toplevel_on_configure(void                *data,
                          struct xdg_toplevel *toplevel,
                          int32_t              width,
                          int32_t              height,
                          struct wl_array     *states)
{
    CogWlViewport *viewport = data;

    if (width == 0 || height == 0) {
        g_debug("%s: Skipped toplevel configuration, size %" PRIi32 "x%" PRIi32, G_STRFUNC, width, height);
        width = viewport->window.width;
        height = viewport->window.height;
    }
    g_debug("%s: New toplevel configuration, size %" PRIu32 ", %" PRIu32, G_STRFUNC, width, height);
    cog_wl_viewport_configure_geometry(viewport, width, height);
}

static void
xdg_toplevel_on_close(void *data, struct xdg_toplevel *xdg_toplevel)
{
    g_application_quit(g_application_get_default());
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
    .configure = xdg_toplevel_on_configure,
    .close = xdg_toplevel_on_close,
};

/*
 * CogWlViewport instantiation.
 */

static void
cog_wl_viewport_class_init(CogWlViewportClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = cog_wl_viewport_dispose;
}

static void
cog_wl_viewport_init(CogWlViewport *viewport)
{
    if (!getenv_uint32("COG_PLATFORM_WL_VIEW_WIDTH", &viewport->window.width) || viewport->window.width == 0)
        viewport->window.width = DEFAULT_WIDTH;

    if (!getenv_uint32("COG_PLATFORM_WL_VIEW_HEIGHT", &viewport->window.height) || viewport->window.height == 0)
        viewport->window.height = DEFAULT_HEIGHT;

    g_debug("%s: Initial size is %" PRIu32 "x%" PRIu32, G_STRFUNC, viewport->window.width, viewport->window.height);

    viewport->window.width_before_fullscreen = viewport->window.width;
    viewport->window.height_before_fullscreen = viewport->window.height;

    g_signal_connect(COG_VIEWPORT(viewport), "add", G_CALLBACK(cog_wl_viewport_on_add), NULL);
}

/*
 * CogWlViewport deinstantiation.
 */

static void
cog_wl_viewport_class_finalize(CogWlViewportClass *klass G_GNUC_UNUSED)
{
}

static void
cog_wl_viewport_dispose(GObject *object)
{
    destroy_window(COG_WL_VIEWPORT(object));

    G_OBJECT_CLASS(cog_wl_viewport_parent_class)->dispose(object);
}

/*
 * Method definitions.
 */

void
cog_wl_viewport_configure_geometry(CogWlViewport *viewport, int32_t width, int32_t height)
{
    g_return_if_fail(width > 0);
    g_return_if_fail(height > 0);

    if (viewport->window.width != width || viewport->window.height != height) {
        g_debug("Configuring new size: %" PRId32 "x%" PRId32, width, height);
        viewport->window.width = width;
        viewport->window.height = height;

        cog_viewport_foreach(COG_VIEWPORT(viewport), (GFunc) cog_wl_view_resize, NULL);
    }
}

gboolean
cog_wl_viewport_create_window(CogWlViewport *viewport, GError **error)
{
    g_debug("Creating Wayland surface...");

    CogWlPlatform *platform = COG_WL_PLATFORM(cog_platform_get());
    CogWlDisplay  *display = platform->display;

    viewport->window.wl_surface = cog_wl_compositor_create_surface(display->compositor, viewport);

#if COG_ENABLE_WESTON_DIRECT_DISPLAY
    viewport->window.video_surfaces = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, destroy_video_surface);
#endif

    wl_surface_add_listener(viewport->window.wl_surface, &surface_listener, viewport);

    if (display->xdg_shell != NULL) {
        viewport->window.xdg_surface = xdg_wm_base_get_xdg_surface(display->xdg_shell, viewport->window.wl_surface);
        g_assert(viewport->window.xdg_surface);

        static const struct xdg_surface_listener xdg_surface_listener = {.configure = xdg_surface_on_configure};
        xdg_surface_add_listener(viewport->window.xdg_surface, &xdg_surface_listener, NULL);
        viewport->window.xdg_toplevel = xdg_surface_get_toplevel(viewport->window.xdg_surface);
        g_assert(viewport->window.xdg_toplevel);

        xdg_toplevel_add_listener(viewport->window.xdg_toplevel, &xdg_toplevel_listener, viewport);
        xdg_toplevel_set_title(viewport->window.xdg_toplevel, COG_DEFAULT_APPNAME);

        const char   *app_id = NULL;
        GApplication *app = g_application_get_default();
        if (app) {
            app_id = g_application_get_application_id(app);
        }
        if (!app_id) {
            app_id = COG_DEFAULT_APPID;
        }
        xdg_toplevel_set_app_id(viewport->window.xdg_toplevel, app_id);
        wl_surface_commit(viewport->window.wl_surface);
    } else if (display->fshell != NULL) {
        zwp_fullscreen_shell_v1_present_surface(display->fshell,
                                                viewport->window.wl_surface,
                                                ZWP_FULLSCREEN_SHELL_V1_PRESENT_METHOD_DEFAULT,
                                                NULL);

        /* The fullscreen shell needs an initial surface configuration. */
        cog_wl_viewport_configure_geometry(viewport, viewport->window.width, viewport->window.height);
    } else if (display->shell != NULL) {
        viewport->window.shell_surface = wl_shell_get_shell_surface(display->shell, viewport->window.wl_surface);
        g_assert(viewport->window.shell_surface);

        wl_shell_surface_add_listener(viewport->window.shell_surface, &shell_surface_listener, viewport);
        wl_shell_surface_set_toplevel(viewport->window.shell_surface);

        /* wl_shell needs an initial surface configuration. */
        cog_wl_viewport_configure_geometry(viewport, viewport->window.width, viewport->window.height);
    }

#if COG_HAVE_LIBPORTAL
    viewport->window.xdp_parent_wl_data.zxdg_exporter = display->zxdg_exporter;
    viewport->window.xdp_parent_wl_data.wl_surface = viewport->window.wl_surface;
#endif /* COG_HAVE_LIBPORTAL */

    const char *env_var;
    if ((env_var = g_getenv("COG_PLATFORM_WL_VIEW_FULLSCREEN")) && g_ascii_strtoll(env_var, NULL, 10) > 0) {
        viewport->window.is_maximized = false;
        cog_wl_viewport_set_fullscreen(viewport, true);
    } else if ((env_var = g_getenv("COG_PLATFORM_WL_VIEW_MAXIMIZE")) && g_ascii_strtoll(env_var, NULL, 10) > 0) {
        cog_wl_viewport_set_fullscreen(viewport, false);
        viewport->window.is_maximized = true;

        if (display->xdg_shell != NULL) {
            xdg_toplevel_set_maximized(viewport->window.xdg_toplevel);
        } else if (display->shell != NULL) {
            wl_shell_surface_set_maximized(viewport->window.shell_surface, NULL);
        } else {
            g_warning("No available shell capable of maximizing.");
            viewport->window.is_maximized = false;
        }
    }

    return TRUE;
}

static void
cog_wl_viewport_enter_fullscreen(CogWlViewport *viewport)
{
    CogWlPlatform *platform = (CogWlPlatform *) cog_platform_get();
    CogWlDisplay  *display = platform->display;
    CogWlWindow   *window = &viewport->window;

    // Resize window to the size of the screen.
    window->width_before_fullscreen = window->width;
    window->height_before_fullscreen = window->height;

    if (display->xdg_shell) {
        xdg_toplevel_set_fullscreen(window->xdg_toplevel, NULL);
    } else if (display->shell) {
        wl_shell_surface_set_fullscreen(window->shell_surface, WL_SHELL_SURFACE_FULLSCREEN_METHOD_SCALE, 0, NULL);
    } else if (display->fshell == NULL) {
        g_assert_not_reached();
    }

    cog_wl_viewport_resize_to_largest_output(viewport);

    if (!cog_viewport_get_n_views(COG_VIEWPORT(viewport))) {
        g_debug("%s: No views in viewport, will not fullscreen.", G_STRFUNC);
        return;
    }

    // XXX: Do we need to set all views as fullscreened?
    // Wait until a new exported image is received. See cog_wl_view_enter_fullscreen().
    cog_wl_view_enter_fullscreen(COG_WL_VIEW(cog_viewport_get_visible_view(COG_VIEWPORT(viewport))));
}

static void
cog_wl_viewport_exit_fullscreen(CogWlViewport *viewport)
{
    // The surface was not fullscreened if there were no views.
    g_assert(cog_viewport_get_n_views(COG_VIEWPORT(viewport)) > 0);

    CogWlPlatform *platform = (CogWlPlatform *) cog_platform_get();
    CogWlDisplay  *display = platform->display;
    CogWlWindow   *window = &viewport->window;

    if (display->xdg_shell != NULL) {
        xdg_toplevel_unset_fullscreen(window->xdg_toplevel);
    } else if (display->fshell != NULL) {
        noop();
    } else if (display->shell != NULL) {
        wl_shell_surface_set_toplevel(window->shell_surface);
    } else {
        g_assert_not_reached();
    }

    cog_wl_viewport_configure_geometry(viewport, window->width_before_fullscreen, window->height_before_fullscreen);

    if (window->was_fullscreen_requested_from_dom) {
        cog_wl_view_exit_fullscreen(COG_WL_VIEW(cog_viewport_get_visible_view(viewport)));
    }
    window->was_fullscreen_requested_from_dom = false;
}

static void
cog_wl_viewport_on_add(CogWlViewport *viewport G_GNUC_UNUSED, CogView *view)
{
    cog_wl_view_resize((CogWlView *) view);
}

void
cog_wl_viewport_resize_to_largest_output(CogWlViewport *viewport)
{
    CogWlPlatform *platform = (CogWlPlatform *) cog_platform_get();
    CogWlDisplay  *display = platform->display;

    /* Find the largest output and resize the surface to match */
    int32_t      width = 0;
    int32_t      height = 0;
    CogWlOutput *output;
    wl_list_for_each(output, &display->outputs, link) {
        if (output->width * output->height >= width * height) {
            width = output->width;
            height = output->height;
        }
    }
    if (width > 0 && height > 0)
        cog_wl_viewport_configure_geometry(viewport, width, height);
}

bool
cog_wl_viewport_set_fullscreen(CogWlViewport *viewport, bool fullscreen)
{
    if (viewport->window.is_fullscreen == fullscreen)
        return false;

    viewport->window.is_fullscreen = fullscreen;

    if (fullscreen)
        cog_wl_viewport_enter_fullscreen(viewport);
    else
        cog_wl_viewport_exit_fullscreen(viewport);

    return true;
}

/*
 * CogWlViewport register type method.
 */

void
cog_wl_viewport_register_type_exported(GTypeModule *type_module)
{
    cog_wl_viewport_register_type(type_module);
}
