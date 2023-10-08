/*
 * cog-window-wl.c
 * Copyright (C) 2023 Pablo Saavedra <psaavedra@igalia.com>
 * Copyright (C) 2023 Adrian Perez de Castro <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#include "../../core/cog.h"

#include "xdg-shell-client.h"

#include "cog-window-wl.h"

G_DEFINE_TYPE(CogWlWindow, cog_wl_window, G_TYPE_OBJECT)

/*
 * CogWlWindow instantiation.
 */

static void
cog_wl_window_class_init(CogWlWindowClass *klass)
{
}

static void
cog_wl_window_init(CogWlWindow *self)
{
}

/*
 * Method definitions.
 */

CogWlWindow *
cog_wl_window_new(void)
{
    g_autoptr(CogWlWindow) self = g_object_new(COG_WL_WINDOW_TYPE, NULL);
    return g_steal_pointer(&self);
}

void
cog_wl_window_popup_destroy(CogWlWindow *window)
{
    if (window->popup_data.option_menu == NULL)
        return;

    webkit_option_menu_close(window->popup_data.option_menu);
    g_clear_pointer(&window->popup_data.popup_menu, cog_popup_menu_destroy);
    g_clear_object(&window->popup_data.option_menu);

    g_clear_pointer(&window->popup_data.xdg_popup, xdg_popup_destroy);
    g_clear_pointer(&window->popup_data.xdg_surface, xdg_surface_destroy);
    g_clear_pointer(&window->popup_data.xdg_positioner, xdg_positioner_destroy);
    g_clear_pointer(&window->popup_data.shell_surface, wl_shell_surface_destroy);
    g_clear_pointer(&window->popup_data.wl_surface, wl_surface_destroy);

    window->popup_data.configured = false;
}

void
cog_wl_window_popup_display(CogWlWindow *window)
{
    struct wl_buffer *buffer = cog_popup_menu_get_buffer(window->popup_data.popup_menu);
    wl_surface_attach(window->popup_data.wl_surface, buffer, 0, 0);
    wl_surface_damage(window->popup_data.wl_surface, 0, 0, INT32_MAX, INT32_MAX);
    wl_surface_commit(window->popup_data.wl_surface);
}

void
cog_wl_window_popup_update(CogWlWindow *window)
{
    int  selected_index;
    bool has_final_selection = cog_popup_menu_has_final_selection(window->popup_data.popup_menu, &selected_index);
    if (has_final_selection) {
        if (selected_index != -1)
            webkit_option_menu_activate_item(window->popup_data.option_menu, selected_index);
        cog_wl_window_popup_destroy(window);
        return;
    }

    struct wl_buffer *buffer = cog_popup_menu_get_buffer(window->popup_data.popup_menu);
    wl_surface_attach(window->popup_data.wl_surface, buffer, 0, 0);
    wl_surface_damage(window->popup_data.wl_surface, 0, 0, INT32_MAX, INT32_MAX);
    wl_surface_commit(window->popup_data.wl_surface);
}

void
cog_wl_window_xdg_popup_on_configure(void             *data,
                                     struct xdg_popup *xdg_popup,
                                     int32_t           x,
                                     int32_t           y,
                                     int32_t           width,
                                     int32_t           height)
{
}

void
cog_wl_window_xdg_popup_on_done(void *data, struct xdg_popup *xdg_popup)
{
    CogWlWindow *window = data;
    cog_wl_window_popup_destroy(window);
}
