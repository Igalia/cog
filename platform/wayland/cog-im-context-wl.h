/*
 * cog-platform-wl.h
 * Copyright (C) 2020 Igalia S.L.
 *
 * Distributed under terms of the MIT license.
 */

#pragma once

#include <wpe/webkit.h>

#include "text-input-unstable-v3-client.h"

G_BEGIN_DECLS

#define COG_TYPE_IM_CONTEXT_WL  (cog_im_context_wl_get_type ())

G_DECLARE_DERIVABLE_TYPE (CogIMContextWl, cog_im_context_wl, COG, IM_CONTEXT_WL, WebKitInputMethodContext)


struct _CogIMContextWlClass {
    WebKitInputMethodContextClass parent_class;
};

void cog_im_context_wl_set_text_input (struct zwp_text_input_v3 *text_input);

WebKitInputMethodContext *cog_im_context_wl_new (void);

G_END_DECLS
