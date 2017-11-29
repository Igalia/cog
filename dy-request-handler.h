/*
 * dy-request-handler.h
 * Copyright (C) 2017 Adrian Perez <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#ifndef DY_REQUEST_HANDLER_H
#define DY_REQUEST_HANDLER_H

#if !(defined(DY_INSIDE_DINGHY__) && DY_INSIDE_DINGHY__)
# error "Do not include this header directly, use <dinghy.h> instead"
#endif

#include "dy-webkit-utils.h"

G_BEGIN_DECLS

#define DY_TYPE_REQUEST_HANDLER  (dy_request_handler_get_type ())

G_DECLARE_INTERFACE (DyRequestHandler, dy_request_handler, DY, REQUEST_HANDLER, GObject)


struct _DyRequestHandlerInterface {
    GTypeInterface g_iface;

    void (*run) (DyRequestHandler *handler, WebKitURISchemeRequest *request);
};


void dy_request_handler_run (DyRequestHandler *handler, WebKitURISchemeRequest *request);


G_END_DECLS

#endif /* !DY_REQUEST_HANDLER_H */
