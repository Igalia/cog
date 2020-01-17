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

typedef struct {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
} PwlDisplay;

typedef struct {
    struct egl_display *display;
    EGLContext context;
    EGLConfig egl_config;
} PwlEGLData;

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

#if HAVE_DEVICE_SCALING
typedef struct output_metrics {
  struct wl_output *output;
  int32_t name;
  int32_t scale;
} output_metrics;
#endif /* HAVE_DEVICE_SCALING */

typedef struct {
    struct wl_registry *registry;
    struct wl_compositor *compositor;

    struct xdg_wm_base *xdg_shell;
    struct zwp_fullscreen_shell_v1 *fshell;
    struct wl_shell *shell;

    struct wl_seat *seat;

#if HAVE_DEVICE_SCALING
    struct output_metrics metrics[16];
#endif /* HAVE_DEVICE_SCALING */

    struct {
        int32_t scale;
    } current_output;

    struct {
        struct wl_pointer *obj;
        int32_t x;
        int32_t y;
        uint32_t button;
        uint32_t state;
        struct wl_pointer_listener listener;
    } pointer;

    struct {
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

        uint32_t serial;
        struct wl_keyboard_listener listener;
    } keyboard;

    struct {
        struct wl_touch *obj;
        struct wl_touch_listener listener;
    } touch;

    GSource *event_src;

    void (*resize_window)(void* data);
    void (*handle_key_event)(void* data, uint32_t key, uint32_t state, uint32_t time);

    struct wl_output_listener output_listener;
} PwlData;

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

typedef struct {
    struct wl_surface *wl_surface;
    struct wl_surface_listener surface_listener;
    struct wl_egl_window *egl_window;
    EGLSurface egl_surface;

    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *xdg_toplevel;
    struct wl_shell_surface *shell_surface;

    uint32_t width;
    uint32_t height;

    bool is_fullscreen;
    bool is_maximized;
} PwlWinData;

GSource *
setup_wayland_event_source (GMainContext *main_context,
                            PwlDisplay *display);

gboolean init_egl (PwlDisplay*, GError **error);
void clear_egl (void);

gboolean init_wayland (PwlDisplay*, GError **error);

gboolean create_window (void *data, GError **error);
void destroy_window (void);

gboolean init_input (void *data, GError **error);
void clear_input (void);

/* Keyboard */
void
keyboard_on_keymap (void *data,
                    struct wl_keyboard *wl_keyboard,
                    uint32_t format,
                    int32_t fd,
                    uint32_t size);
void
keyboard_on_enter (void *data,
                   struct wl_keyboard *wl_keyboard,
                   uint32_t serial,
                   struct wl_surface *surface,
                   struct wl_array *keys);

void
keyboard_on_leave (void *data,
                   struct wl_keyboard *wl_keyboard,
                   uint32_t serial,
                   struct wl_surface *surface);

void
keyboard_on_key (void *data,
                 struct wl_keyboard *wl_keyboard,
                 uint32_t serial,
                 uint32_t time,
                 uint32_t key,
                 uint32_t state);

void
keyboard_on_modifiers (void *data,
                       struct wl_keyboard *wl_keyboard,
                       uint32_t serial,
                       uint32_t mods_depressed,
                       uint32_t mods_latched,
                       uint32_t mods_locked,
                       uint32_t group);

void
keyboard_on_repeat_info (void *data,
                         struct wl_keyboard *wl_keyboard,
                         int32_t rate,
                         int32_t delay);

G_END_DECLS
