/*
 * cog-window-wl.h
 * Copyright (C) 2023 Pablo Saavedra <psaavedra@igalia.com>
 * Copyright (C) 2023 Adrian Perez de Castro <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#ifndef COG_PLATFORM_WL_WINDOW_H
#define COG_PLATFORM_WL_WINDOW_H

#include "../../core/cog.h"

#include "cog-popup-menu-wl.h"
#include "cog-utils-wl.h"

G_BEGIN_DECLS

/*
 * CogWlWindow type declaration.
 */

#define COG_WL_WINDOW_TYPE cog_wl_window_get_type()

struct _CogWlWindow {
    CogWindow parent;

    struct wl_surface *wl_surface;

    CogWlAxis    axis;
    CogWlPointer pointer;
    CogWlTouch   touch;

    struct xdg_surface      *xdg_surface;
    struct xdg_toplevel     *xdg_toplevel;
    struct wl_shell_surface *shell_surface;

    bool should_resize_to_largest_output;

    struct {
        struct wl_surface *wl_surface;

        struct xdg_positioner *xdg_positioner;
        struct xdg_surface    *xdg_surface;
        struct xdg_popup      *xdg_popup;

        struct wl_shell_surface *shell_surface;

        uint32_t width;
        uint32_t height;

        CogPopupMenu     *popup_menu;
        WebKitOptionMenu *option_menu;

        bool configured;
    } popup_data;
};

G_DECLARE_FINAL_TYPE(CogWlWindow, cog_wl_window, COG, WL_WINDOW, CogWindow)

/*
 * Method definitions.
 */

CogWlWindow *cog_wl_window_new(void);

void cog_wl_window_popup_destroy(CogWlWindow *);
void cog_wl_window_popup_display(CogWlWindow *);
void cog_wl_window_popup_update(CogWlWindow *);

void cog_wl_window_xdg_popup_on_configure(void *, struct xdg_popup *, int32_t, int32_t, int32_t, int32_t);
void cog_wl_window_xdg_popup_on_done(void *, struct xdg_popup *);

G_END_DECLS

#endif /* !COG_PLATFORM_WL_WINDOW_H */
