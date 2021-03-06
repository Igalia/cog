/*
 * cog-platform-fdo.h
 * Copyright (C) 2020 Igalia S.L.
 *
 * Distributed under terms of the MIT license.
 */

#pragma once

#include <wpe/webkit.h>

#include "text-input-unstable-v3-client.h"

G_BEGIN_DECLS

#define COG_TYPE_IM_CONTEXT_FDO  (cog_im_context_fdo_get_type ())

G_DECLARE_DERIVABLE_TYPE (CogIMContextFdo, cog_im_context_fdo, COG, IM_CONTEXT_FDO, WebKitInputMethodContext)


struct _CogIMContextFdoClass {
    WebKitInputMethodContextClass parent_class;
};

void cog_im_context_fdo_set_text_input (struct zwp_text_input_v3 *text_input);

WebKitInputMethodContext *cog_im_context_fdo_new (void);

G_END_DECLS
