/*
 * cog-request-handler.c
 * Copyright (C) 2017-2018 Adrian Perez <aperez@igalia.com>
 *
 * SPDX-License-Identifier: MIT
 */

#include "cog-request-handler.h"

/**
 * CogRequestHandler:
 *
 * Convenience interface which allows implementing custom URI scheme handlers.
 *
 * Any object that implements this interface can be passed to
 * [method@Cog.Shell.set_request_handler]. An advantage of using this
 * interface instead of [method@WebKit.WebContext.register_uri_scheme]
 * directly is that it allows for extending handlers (by subclassing) and
 * for more easily combining different handlers in an aggregate one (like
 * [class@Cog.PrefixRoutesHandler] is that it allows for extending handlers
 * (by subclassing), supports combining different handlers in an aggregate
 * one (like [class@Cog.PrefixRoutesHandler]) more easily, and handler
 * implementations can keep their state in the object instances.
 *
 * ### Implementations
 *
 * - [class@Cog.DirectoryFilesHandler]
 * - [class@Cog.PrefixRoutesHandler]
 */

G_DEFINE_INTERFACE (CogRequestHandler, cog_request_handler, G_TYPE_OBJECT);

static void
cog_request_handler_default_init (CogRequestHandlerInterface *iface)
{
}

/**
 * cog_request_handler_run: (virtual run)
 * @request: A request to handle.
 *
 * Handle a single custom URI scheme request.
 */
void
cog_request_handler_run (CogRequestHandler *handler, WebKitURISchemeRequest *request)
{
    g_return_if_fail (COG_IS_REQUEST_HANDLER (handler));
    g_return_if_fail (WEBKIT_IS_URI_SCHEME_REQUEST (request));

    CogRequestHandlerInterface *iface = COG_REQUEST_HANDLER_GET_IFACE (handler);
    g_return_if_fail (iface->run != NULL);
    (*iface->run) (handler, request);
}
