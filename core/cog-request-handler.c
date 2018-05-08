/*
 * cog-request-handler.c
 * Copyright (C) 2017-2018 Adrian Perez <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#include "cog-request-handler.h"


G_DEFINE_INTERFACE (CogRequestHandler, cog_request_handler, G_TYPE_OBJECT);


static void
cog_request_handler_default_init (CogRequestHandlerInterface *iface)
{
}


void
cog_request_handler_run (CogRequestHandler *handler, WebKitURISchemeRequest *request)
{
    g_return_if_fail (COG_IS_REQUEST_HANDLER (handler));
    g_return_if_fail (WEBKIT_IS_URI_SCHEME_REQUEST (request));

    CogRequestHandlerInterface *iface = COG_REQUEST_HANDLER_GET_IFACE (handler);
    g_return_if_fail (iface->run != NULL);
    (*iface->run) (handler, request);
}
