/*
 * cog-platform-wl.h
 * Copyright (C) 2023 Pablo Saavedra <psaavedra@igalia.com>
 * Copyright (C) 2023 Adrian Perez de Castro <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#ifndef COG_PLATFORM_WL_PLATFORM_H
#define COG_PLATFORM_WL_PLATFORM_H

#include "../../core/cog.h"

#include "cog-view-wl.h"
#include "cog-window-wl.h"

G_BEGIN_DECLS

typedef struct _CogWlDisplay CogWlDisplay;
typedef struct _CogWlSeat    CogWlSeat;

struct _CogWlSeat {
    struct wl_seat *seat;
    uint32_t        seat_name;
    uint32_t        seat_version;

    struct wl_pointer *pointer_obj;
    CogWlWindow       *pointer_target; /* Current target of pointer events. */

    struct wl_touch *touch_obj;
    CogWlWindow     *touch_target; /* Current target of touch events. */

    CogWlKeyboard       keyboard;
    struct wl_keyboard *keyboard_obj;
    CogWlWindow        *keyboard_target; /* Current target of keyboard events. */

    CogWlXkb xkb;

    struct wl_list link;
};

struct _CogWlDisplay {
    struct egl_display *egl_display;

    struct wl_display    *display;
    struct wl_registry   *registry;
    struct wl_compositor *compositor;

    struct wl_subcompositor *subcompositor;
    struct wl_shm           *shm;

    struct xdg_wm_base             *xdg_shell;
    struct zwp_fullscreen_shell_v1 *fshell;
    struct wl_shell                *shell;

#if COG_ENABLE_WESTON_DIRECT_DISPLAY
    struct zwp_linux_dmabuf_v1      *dmabuf;
    struct weston_direct_display_v1 *direct_display;

    GHashTable *video_surfaces;
#endif

#if COG_ENABLE_WESTON_CONTENT_PROTECTION
    struct weston_content_protection *protection;
#endif

#ifdef COG_USE_WAYLAND_CURSOR
    struct wl_cursor_theme *cursor_theme;
    struct wl_cursor       *cursor_left_ptr;
    struct wl_surface      *cursor_left_ptr_surface;
#endif /* COG_USE_WAYLAND_CURSOR */

    struct zwp_text_input_manager_v3 *text_input_manager;
    struct zwp_text_input_manager_v1 *text_input_manager_v1;

    struct wp_presentation *presentation;

    GSource *event_src;

    struct wl_list shm_buffer_list;

    CogWlSeat     *seat_default;
    struct wl_list seats; /* wl_list<CogWlSeat> */
};

G_DECLARE_FINAL_TYPE(CogWlPlatform, cog_wl_platform, COG, WL_PLATFORM, CogPlatform)
G_END_DECLS

struct _CogWlPlatformClass {
    CogPlatformClass parent_class;
};

struct _CogWlPlatform {
    CogPlatform    parent;
    CogWlDisplay  *display;
    CogViewStack  *views;
    GHashTable    *windows;
    CogWlOutput   *current_output;
    struct wl_list outputs; /* wl_list<CogWlOutput> */
};

#endif /* !COG_PLATFORM_WL_PLATFORM_H */
