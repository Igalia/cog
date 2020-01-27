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

#include "xdg-shell-unstable-v6-client.h"
#include "fullscreen-shell-unstable-v1-client.h"

#define DEFAULT_WIDTH  1024
#define DEFAULT_HEIGHT  768

#if defined(COG_DEVICE_SCALING) && COG_DEVICE_SCALING
# define HAVE_DEVICE_SCALING 1
#else
# define HAVE_DEVICE_SCALING 0
#endif /* COG_DEVICE_SCALING */

G_BEGIN_DECLS

#define PWL_ERROR  (pwl_error_quark ())
GQuark pwl_error_get_quark (void);

typedef enum {
    PWL_ERROR_WAYLAND,
    PWL_ERROR_EGL,
} PwlError;


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


PwlDisplay* pwl_display_connect (const char *name, GError**);
void        pwl_display_destroy (PwlDisplay*);

void        pwl_display_set_default_application_id (PwlDisplay*,
                                                    const char *application_id);
void        pwl_display_set_default_window_title (PwlDisplay*,
                                                  const char *title);

bool        pwl_display_input_init (PwlDisplay*, GError **error);
void        pwl_display_input_deinit (PwlDisplay*);

gboolean    pwl_display_egl_init (PwlDisplay*, GError **error);
void        pwl_display_egl_deinit (PwlDisplay*);
EGLDisplay  pwl_display_egl_get_display (const PwlDisplay*);
struct wl_buffer*
            pwl_display_egl_create_buffer_from_image (const PwlDisplay*, EGLImage);

PwlXKBData* pwl_display_xkb_get_data (PwlDisplay*);

void        pwl_display_notify_pointer_motion (PwlDisplay*,
                                               void (*callback) (PwlDisplay*, const PwlPointer*, void*),
                                               void *userdata);
void        pwl_display_notify_pointer_button (PwlDisplay*,
                                               void (*callback) (PwlDisplay*, const PwlPointer*, void*),
                                               void *userdata);
void        pwl_display_notify_pointer_axis (PwlDisplay*,
                                             void (*callback) (PwlDisplay*, const PwlPointer*, void*),
                                             void *userdata);
void        pwl_display_notify_touch_down (PwlDisplay*,
                                           void (*callback) (PwlDisplay*, const PwlTouch*, void*),
                                           void *userdata);
void        pwl_display_notify_touch_up (PwlDisplay*,
                                         void (*callback) (PwlDisplay*, const PwlTouch*, void*),
                                         void *userdata);
void        pwl_display_notify_touch_motion (PwlDisplay*,
                                             void (*callback) (PwlDisplay*, const PwlTouch*, void*),
                                             void *userdata);
void        pwl_display_notify_key_event (PwlDisplay*,
                                          void (*callback) (PwlDisplay*, const PwlKeyboard*, void*),
                                          void *userdata);

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

G_END_DECLS
