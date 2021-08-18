/*
 * cog-platform-wl-v1.h
 * Copyright (C) 2020 Igalia S.L.
 *
 * Distributed under terms of the MIT license.
 */

#pragma once

#include <wpe/webkit.h>

#include "text-input-unstable-v1-client.h"

G_BEGIN_DECLS

#define COG_TYPE_IM_CONTEXT_WL_V1  (cog_im_context_wl_v1_get_type ())

G_DECLARE_DERIVABLE_TYPE (CogIMContextWlV1, cog_im_context_wl_v1, COG, IM_CONTEXT_WL_V1, WebKitInputMethodContext)


struct _CogIMContextWlV1Class {
    WebKitInputMethodContextClass parent_class;
};

void cog_im_context_wl_v1_set_text_input   (struct zwp_text_input_v1 *text_input,
                                             struct wl_seat           *seat,
                                             struct wl_surface        *surface);
void cog_im_context_wl_v1_set_view_backend (struct wpe_view_backend  *backend);


WebKitInputMethodContext *cog_im_context_wl_v1_new (void);

G_END_DECLS
