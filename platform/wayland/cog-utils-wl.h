/*
 * cog-utils-wl.h
 * Copyright (C) 2023 Pablo Saavedra <psaavedra@igalia.com>
 * Copyright (C) 2023 Adrian Perez de Castro <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#include "../../core/cog.h"

#if defined(WPE_FDO_CHECK_VERSION) && 0
#    define HAVE_SHM_EXPORTED_BUFFER WPE_FDO_CHECK_VERSION(1, 9, 0)
#    define HAVE_FULLSCREEN_HANDLING WPE_FDO_CHECK_VERSION(1, 11, 1)
#else
#    define HAVE_SHM_EXPORTED_BUFFER 0
#    define HAVE_FULLSCREEN_HANDLING 0
#endif

typedef struct _CogWlAxis     CogWlAxis;
typedef struct _CogWlKeyboard CogWlKeyboard;
typedef struct _CogWlOutput   CogWlOutput;
typedef struct _CogWlPointer  CogWlPointer;
typedef struct _CogWlTouch    CogWlTouch;
typedef struct _CogWlXkb      CogWlXkb;

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
