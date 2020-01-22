/*
 * pwl.h
 * Copyright (C) 2019 Adrian Perez de Castro <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#pragma once

#include "cog-config.h"

#include <stdbool.h>

#include <glib.h>
#include <wayland-client.h>
#include <wayland-egl.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-compose.h>
#include <locale.h>

#include "xdg-shell-client.h"
#include "fullscreen-shell-unstable-v1-client.h"

#define DEFAULT_WIDTH  1024
#define DEFAULT_HEIGHT  768

#if defined(COG_DEVICE_SCALING) && COG_DEVICE_SCALING
# define HAVE_DEVICE_SCALING 1
#else
# define HAVE_DEVICE_SCALING 0
#endif /* COG_DEVICE_SCALING */

G_BEGIN_DECLS

#if HAVE_DEVICE_SCALING
typedef struct output_metrics {
  struct wl_output *output;
  int32_t name;
  int32_t scale;
} output_metrics;
#endif /* HAVE_DEVICE_SCALING */

typedef struct {
    struct wl_keyboard *obj;

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

    struct {
        uint32_t keysym;
        uint32_t unicode;
        uint32_t state;
        uint8_t modifiers;
        uint32_t timestamp;
    } event;

    uint32_t serial;
} PwlKeyboard;

typedef struct {
    struct wl_pointer *obj;
    uint32_t time;
    uint32_t axis;
    int32_t x;
    int32_t y;
    uint32_t button;
    uint32_t state;
    wl_fixed_t value;
    void *data;
} PwlPointer;

typedef struct {
    struct wl_touch *obj;
    int32_t id;
    uint32_t time;
    wl_fixed_t x;
    wl_fixed_t y;
} PwlTouch;

typedef struct {
    struct xkb_context* context;
    struct xkb_keymap* keymap;
    struct xkb_state* state;

    struct xkb_compose_table* compose_table;
    struct xkb_compose_state* compose_state;

    struct {
        xkb_mod_index_t control;
        xkb_mod_index_t alt;
        xkb_mod_index_t shift;
    } indexes;

    struct {
        xkb_mod_index_t control;
        xkb_mod_index_t alt;
        xkb_mod_index_t shift;
    } modifier;

    uint8_t modifiers;
} PwlXKBData;

typedef struct _PwlDisplay PwlDisplay;

struct _PwlDisplay {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_seat *seat;
    struct egl_display *egl_display;
    EGLContext egl_context;
    EGLConfig egl_config;

    PwlKeyboard keyboard;
    PwlPointer pointer;
    PwlTouch touch;

    PwlXKBData xkb_data;

    struct xdg_wm_base *xdg_shell;
    struct zwp_fullscreen_shell_v1 *fshell;
    struct wl_shell *shell;

#if HAVE_DEVICE_SCALING
    struct output_metrics metrics[16];
#endif /* HAVE_DEVICE_SCALING */

    struct {
        int32_t scale;
    } current_output;

    GSource *event_src;

    void (*on_surface_enter) (PwlDisplay*, void *userdata);
    void *on_surface_enter_userdata;
    void (*on_pointer_on_motion) (PwlDisplay*, void *userdata);
    void *on_pointer_on_motion_userdata;
    void (*on_pointer_on_button) (PwlDisplay*, void *userdata);
    void *on_pointer_on_button_userdata;
    void (*on_pointer_on_axis) (PwlDisplay*, void *userdata);
    void *on_pointer_on_axis_userdata;

    void (*on_touch_on_down)     (PwlDisplay*, void *userdata);
    void *on_touch_on_down_userdata;
    void (*on_touch_on_up)       (PwlDisplay*, void *userdata);
    void *on_touch_on_up_userdata;
    void (*on_touch_on_motion)   (PwlDisplay*, void *userdata);
    void *on_touch_on_motion_userdata;

    void (*on_key_event) (PwlDisplay*, void *userdata);
    void *on_key_event_userdata;
    bool (*on_capture_app_key) (PwlDisplay*, void *userdata);
    void *on_capture_app_key_userdata;
};

PwlDisplay* pwl_display_connect (const char *name, GError**);
void        pwl_display_destroy (PwlDisplay*);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (PwlDisplay, pwl_display_destroy)

#ifndef EGL_WL_create_wayland_buffer_from_image
typedef struct wl_buffer * (EGLAPIENTRYP PFNEGLCREATEWAYLANDBUFFERFROMIMAGEWL) (EGLDisplay dpy, EGLImageKHR image);
#endif

struct pwl_event_source {
    GSource source;
    GPollFD pfd;
    struct wl_display* display;
};

typedef struct _PwlWindow PwlWindow;

GSource *
setup_wayland_event_source (GMainContext *main_context,
                            PwlDisplay *display);

gboolean pwl_display_egl_init (PwlDisplay*, GError **error);
void pwl_display_egl_deinit (PwlDisplay*);

gboolean init_wayland (PwlDisplay*, GError **error);

PwlWindow* pwl_window_create (PwlDisplay*);
void pwl_window_destroy (PwlWindow*);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (PwlWindow, pwl_window_destroy)

struct wl_surface* pwl_window_get_surface (const PwlWindow*);

void pwl_window_get_size (const PwlWindow*, uint32_t *w, uint32_t *h);

bool pwl_window_is_fullscreen (const PwlWindow*);
void pwl_window_set_fullscreen (PwlWindow*, bool fullscreen);

bool pwl_window_is_maximized (const PwlWindow*);
void pwl_window_set_maximized (PwlWindow*, bool maximized);

void pwl_window_set_opaque_region (const PwlWindow*,
                                   uint32_t x, uint32_t y,
                                   uint32_t w, uint32_t h);
void pwl_window_unset_opaque_region (const PwlWindow*);

void pwl_window_notify_resize (PwlWindow*,
                               void (*callback) (PwlWindow*, uint32_t w, uint32_t h, void*),
                               void *userdata);

gboolean init_input (PwlDisplay*, GError **error);
void clear_input (PwlDisplay*);

G_END_DECLS
