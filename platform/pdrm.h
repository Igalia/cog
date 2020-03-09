/*
 * pdrm.h
 * Copyright (C) 2020 Adrian Perez de Castro <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#pragma once

#include <glib.h>
#include <stdint.h>
#include <EGL/egl.h>

G_BEGIN_DECLS

struct wl_resource;
struct wpe_view_backend_exportable_fdo_dmabuf_resource;

#define PDRM_ERROR  (pdrm_error_quark ())
GQuark pdrm_error_quark (void);

typedef enum {
    PDRM_ERROR_EGL,
    PDRM_ERROR_CONFIGURATION,
    PDRM_ERROR_UNAVAILABLE,
} PdrmError;


typedef struct _PdrmDisplay PdrmDisplay;
typedef struct _PdrmBuffer PdrmBuffer;

char*        pdrm_find_primary_devnode    (int *out_fd);

PdrmDisplay* pdrm_display_open            (const char *device_path, GError**);
void         pdrm_display_destroy         (PdrmDisplay*);
void         pdrm_display_attach_sources  (PdrmDisplay*, GMainContext*);
void         pdrm_display_get_size        (const PdrmDisplay*, uint32_t *w, uint32_t *h);
EGLDisplay   pdrm_display_get_egl_display (const PdrmDisplay*);
PdrmBuffer*  pdrm_display_import_resource (PdrmDisplay*, struct wl_resource*);
PdrmBuffer*  pdrm_display_import_dmabuf   (PdrmDisplay*, struct wpe_view_backend_exportable_fdo_dmabuf_resource*);

void         pdrm_buffer_destroy          (PdrmBuffer*);
void         pdrm_buffer_commit           (PdrmBuffer*,
                                           void (*callback) (PdrmBuffer*, void*),
                                           void  *userdata);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (PdrmDisplay, pdrm_display_destroy)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (PdrmBuffer, pdrm_buffer_destroy)

G_END_DECLS
