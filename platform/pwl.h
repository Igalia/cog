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

G_BEGIN_DECLS

#define PWL_ERROR  (pwl_error_quark ())
GQuark pwl_error_get_quark (void);

typedef enum {
    PWL_ERROR_WAYLAND,
    PWL_ERROR_EGL,
    PWL_ERROR_UNAVAILABLE,
} PwlError;


enum {
    PWL_N_TOUCH_POINTS = 10,
};


typedef struct {
    uint32_t keysym;
    uint32_t unicode;
    uint32_t state;
    uint8_t modifiers;
    uint32_t timestamp;
    uint32_t serial;
} PwlKeyboard;

typedef struct {
    uint32_t   timestamp;
    int32_t    x;
    int32_t    y;
    uint32_t   button;
    uint32_t   state;
    wl_fixed_t value;
    uint32_t   axis;
    uint32_t   axis_timestamp;
    wl_fixed_t axis_x_delta;
    wl_fixed_t axis_y_delta;
    bool       axis_x_discrete;
    bool       axis_y_discrete;
} PwlPointer;

typedef struct {
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


PwlDisplay* pwl_display_connect (const char *name, GError**);
void        pwl_display_destroy (PwlDisplay*);

void        pwl_display_set_default_application_id (PwlDisplay*,
                                                    const char *application_id);
void        pwl_display_set_default_window_title (PwlDisplay*,
                                                  const char *title);

gboolean    pwl_display_egl_init (PwlDisplay*, GError **error);
void        pwl_display_egl_deinit (PwlDisplay*);
EGLDisplay  pwl_display_egl_get_display (const PwlDisplay*);
struct wl_buffer*
            pwl_display_egl_create_buffer_from_image (const PwlDisplay*, EGLImage);

PwlXKBData* pwl_display_xkb_get_data (PwlDisplay*);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (PwlDisplay, pwl_display_destroy)


typedef struct _PwlWindow PwlWindow;

void setup_wayland_event_source (GMainContext *main_context,
                                 PwlDisplay *display);

PwlWindow* pwl_window_create (PwlDisplay*);
void pwl_window_destroy (PwlWindow*);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (PwlWindow, pwl_window_destroy)

void pwl_window_set_application_id (PwlWindow*, const char *application_id);
void pwl_window_set_title (PwlWindow*, const char *title);

struct wl_surface* pwl_window_get_surface (const PwlWindow*);

void pwl_window_get_size (const PwlWindow*, uint32_t *w, uint32_t *h);
uint32_t pwl_window_get_device_scale (const PwlWindow*);

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
void pwl_window_notify_device_scale (PwlWindow*,
                                     void (*callback) (PwlWindow*, uint32_t scale, void*),
                                     void *userdata);
void pwl_window_notify_pointer_motion (PwlWindow*,
                                       void (*callback) (PwlWindow*, const PwlPointer*, void*),
                                       void *userdata);
void pwl_window_notify_pointer_button (PwlWindow*,
                                       void (*callback) (PwlWindow*, const PwlPointer*, void*),
                                       void *userdata);
void pwl_window_notify_pointer_axis (PwlWindow*,
                                     void (*callback) (PwlWindow*, const PwlPointer*, void*),
                                     void *userdata);
void pwl_window_notify_touch_down (PwlWindow*,
                                   void (*callback) (PwlWindow*, const PwlTouch*, void*),
                                   void *userdata);
void pwl_window_notify_touch_up (PwlWindow*,
                                 void (*callback) (PwlWindow*, const PwlTouch*, void*),
                                 void *userdata);
void pwl_window_notify_touch_motion (PwlWindow*,
                                     void (*callback) (PwlWindow*, const PwlTouch*, void*),
                                     void *userdata);
void pwl_window_notify_keyboard (PwlWindow*,
                                 void (*callback) (PwlWindow*, const PwlKeyboard*, void*),
                                 void *userdata);

G_END_DECLS
