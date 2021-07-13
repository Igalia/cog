/*
 * cog-host-routes-handler.h
 * Copyright (C) 2021 Igalia S.L.
 *
 * Distributed under terms of the MIT license.
 */

#pragma once

#if !(defined(COG_INSIDE_COG__) && COG_INSIDE_COG__)
#    error "Do not include this header directly, use <cog.h> instead"
#endif

#include "cog-request-handler.h"

G_BEGIN_DECLS

#define COG_TYPE_HOST_ROUTES_HANDLER (cog_host_routes_handler_get_type())

G_DECLARE_FINAL_TYPE(CogHostRoutesHandler, cog_host_routes_handler, COG, HOST_ROUTES_HANDLER, GObject)

struct _CogHostRoutesHandlerClass {
    GObjectClass parent_class;
};

CogRequestHandler *cog_host_routes_handler_new(CogRequestHandler *fallback_handler);

gboolean cog_host_routes_handler_contains(CogHostRoutesHandler *self, const char *host);

gboolean cog_host_routes_handler_add(CogHostRoutesHandler *self, const char *host, CogRequestHandler *handler);

gboolean cog_host_routes_handler_remove(CogHostRoutesHandler *self, const char *host);

gboolean cog_host_routes_handler_add_path(CogHostRoutesHandler *self, const char *host, const char *base_path);

G_END_DECLS
