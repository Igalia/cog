/*
 * dy-request-handler.c
 * Copyright (C) 2017 Adrian Perez <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#include "dy-request-handler.h"


G_DEFINE_INTERFACE (DyRequestHandler, dy_request_handler, G_TYPE_OBJECT);


static void
dy_request_handler_default_init (DyRequestHandlerInterface *iface)
{
}


void
dy_request_handler_run (DyRequestHandler *handler, WebKitURISchemeRequest *request)
{
    g_return_if_fail (DY_IS_REQUEST_HANDLER (handler));
    g_return_if_fail (WEBKIT_IS_URI_SCHEME_REQUEST (request));

    DyRequestHandlerInterface *iface = DY_REQUEST_HANDLER_GET_IFACE (handler);
    g_return_if_fail (iface->run != NULL);
    (*iface->run) (handler, request);
}
