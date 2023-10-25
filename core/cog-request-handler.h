/*
 * cog-request-handler.h
 * Copyright (C) 2017-2018 Adrian Perez <aperez@igalia.com>
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef COG_REQUEST_HANDLER_H
#define COG_REQUEST_HANDLER_H

#if !(defined(COG_INSIDE_COG__) && COG_INSIDE_COG__)
# error "Do not include this header directly, use <cog.h> instead"
#endif

#include "cog-webkit-utils.h"

G_BEGIN_DECLS

#define COG_TYPE_REQUEST_HANDLER  (cog_request_handler_get_type ())

G_DECLARE_INTERFACE (CogRequestHandler, cog_request_handler, COG, REQUEST_HANDLER, GObject)

struct _CogRequestHandlerInterface {
    GTypeInterface g_iface;

    /*< public >*/
    void (*run) (CogRequestHandler *handler, WebKitURISchemeRequest *request);
};


void cog_request_handler_run (CogRequestHandler *handler, WebKitURISchemeRequest *request);


G_END_DECLS

#endif /* !COG_REQUEST_HANDLER_H */
