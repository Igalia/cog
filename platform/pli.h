/*
 * pli.h
 * Copyright (C) 2020 Adrian Perez de Castro <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#pragma once

#include <glib.h>
#include <libinput.h>
#include <libudev.h>
#include <wpe/wpe.h>

G_BEGIN_DECLS

typedef struct _PliContext PliContext;

typedef struct wpe_input_keyboard_event PliKeyEvent;
typedef struct wpe_input_touch_event    PliTouchEvent;


PliContext* pli_context_create         (GError**);
void        pli_context_destroy        (PliContext*);
void        pli_context_set_touch_size (PliContext*, uint32_t w, uint32_t h);
void        pli_context_attach_sources (PliContext*, GMainContext*);

void        pli_context_notify_key     (PliContext*,
                                        void (*callback) (PliContext*, PliKeyEvent*, void*),
                                        void *userdata);
void        pli_context_notify_touch   (PliContext*,
                                        void (*callback) (PliContext*, PliTouchEvent*, void*),
                                        void  *userdata);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (PliContext, pli_context_destroy)

G_END_DECLS
