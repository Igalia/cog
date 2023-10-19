/*
 * cog-utils-wl.h
 * Copyright (C) 2023 Pablo Saavedra <psaavedra@igalia.com>
 * Copyright (C) 2023 Adrian Perez de Castro <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#ifndef COG_PLATFORM_WL_UTILS_H
#define COG_PLATFORM_WL_UTILS_H

#include "../../core/cog.h"

#include <wayland-util.h>
#include <xkbcommon/xkbcommon.h>

#define DEFAULT_HEIGHT 768
#define DEFAULT_WIDTH  1024

#if defined(WPE_CHECK_VERSION)
#    define HAVE_REFRESH_RATE_HANDLING WPE_CHECK_VERSION(1, 13, 2)
#else
#    define HAVE_REFRESH_RATE_HANDLING 0
#endif

#if defined(WPE_FDO_CHECK_VERSION)
#    define HAVE_SHM_EXPORTED_BUFFER WPE_FDO_CHECK_VERSION(1, 9, 0)
#    define HAVE_FULLSCREEN_HANDLING WPE_FDO_CHECK_VERSION(1, 11, 1)
#else
#    define HAVE_SHM_EXPORTED_BUFFER 0
#    define HAVE_FULLSCREEN_HANDLING 0
#endif

typedef struct _CogWlAxis     CogWlAxis;
typedef struct _CogWlDisplay  CogWlDisplay;
typedef struct _CogWlKeyboard CogWlKeyboard;
typedef struct _CogWlOutput   CogWlOutput;
typedef struct _CogWlPointer  CogWlPointer;
typedef struct _CogWlTouch    CogWlTouch;
typedef struct _CogWlXkb      CogWlXkb;

struct _CogWlAxis {
    bool       has_delta;
    uint32_t   time;
    wl_fixed_t x_delta;
    wl_fixed_t y_delta;
};

struct _CogWlKeyboard {
    struct {
        int32_t rate;
        int32_t delay;
    } repeat_info;

    struct {
        uint32_t key;
        uint32_t time;
        uint32_t state;
        uint32_t event_source;
    } repeat_data;

    uint32_t serial;
};

struct _CogWlOutput {
    struct wl_output *output;
    int32_t           name;
    int32_t           scale;
    int32_t           width;
    int32_t           height;
    int32_t           refresh;
    struct wl_list    link;
};

struct _CogWlPointer {
    struct wl_surface *surface;
    int32_t            x;
    int32_t            y;
    uint32_t           button;
    uint32_t           state;
};

struct _CogWlTouch {
    struct wl_surface               *surface;
    struct wpe_input_touch_event_raw points[10];
};

struct _CogWlXkb {
    struct xkb_context *context;
    struct xkb_keymap  *keymap;
    struct xkb_state   *state;

    struct xkb_compose_table *compose_table;
    struct xkb_compose_state *compose_state;

    struct {
        xkb_mod_index_t control;
        xkb_mod_index_t alt;
        xkb_mod_index_t shift;
    } indexes;

    uint8_t modifiers;
};

#if HAVE_SHM_EXPORTED_BUFFER
struct shm_buffer {
    struct wl_list     link;
    struct wl_listener destroy_listener;

    struct wl_resource                 *buffer_resource;
    struct wpe_fdo_shm_exported_buffer *exported_buffer;

    struct wl_shm_pool *shm_pool;
    void               *data;
    size_t              size;
    struct wl_buffer   *buffer;
};
#endif

#if COG_ENABLE_WESTON_DIRECT_DISPLAY
#    define VIDEO_BUFFER_FORMAT DRM_FORMAT_YUYV
struct video_buffer {
    struct wl_buffer *buffer;

    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
    int     fd;

    struct wpe_video_plane_display_dmabuf_export *dmabuf_export;
};

struct video_surface {
#    if COG_ENABLE_WESTON_CONTENT_PROTECTION
    struct weston_protected_surface *protected_surface;
#    endif
    struct wl_surface    *wl_surface;
    struct wl_subsurface *wl_subsurface;
};
#endif

struct wl_event_source {
    GSource            source;
    GPollFD            pfd;
    struct wl_display *display;
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

    struct wl_seat *seat;
    uint32_t        pointer_on_enter_event_serial;

#if COG_ENABLE_WESTON_DIRECT_DISPLAY
    struct zwp_linux_dmabuf_v1      *dmabuf;
    struct weston_direct_display_v1 *direct_display;
#endif

#if COG_ENABLE_WESTON_CONTENT_PROTECTION
    struct weston_content_protection *protection;
#endif

#ifdef COG_USE_WAYLAND_CURSOR
    struct wl_cursor_theme *cursor_theme;
    struct wl_surface      *cursor_surface;
#endif /* COG_USE_WAYLAND_CURSOR */

    CogWlOutput  metrics[16];
    CogWlOutput *current_output;

    struct zwp_text_input_manager_v3 *text_input_manager;
    struct zwp_text_input_manager_v1 *text_input_manager_v1;
    struct zxdg_exporter_v2          *zxdg_exporter;

    struct wp_presentation *presentation;

    CogWlAxis axis;

    CogWlKeyboard       keyboard;
    struct wl_keyboard *keyboard_obj;

    CogWlPointer       pointer;
    struct wl_pointer *pointer_obj;

    CogWlTouch       touch;
    struct wl_touch *touch_obj;

    GSource *event_src;

    struct wl_list shm_buffer_list;
};

#endif /* !COG_PLATFORM_WL_UTILS_H */
