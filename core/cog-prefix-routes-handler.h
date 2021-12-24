/*
 * cog-prefix-routes-handler.h
 * Copyright (C) 2021 Igalia S.L.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#if !(defined(COG_INSIDE_COG__) && COG_INSIDE_COG__)
# error "Do not include this header directly, use <cog.h> instead"
#endif

#include "cog-request-handler.h"

G_BEGIN_DECLS

#define COG_TYPE_PREFIX_ROUTES_HANDLER  (cog_prefix_routes_handler_get_type ())

COG_API
G_DECLARE_FINAL_TYPE(CogPrefixRoutesHandler, cog_prefix_routes_handler, COG, PREFIX_ROUTES_HANDLER, GObject)

struct _CogPrefixRoutesHandlerClass {
    GObjectClass parent_class;
};

COG_API
CogRequestHandler* cog_prefix_routes_handler_new     (CogRequestHandler      *fallback_handler);

COG_API
gboolean           cog_prefix_routes_handler_mount   (CogPrefixRoutesHandler *self,
                                                      const char             *path_prefix,
                                                      CogRequestHandler      *handler);

COG_API
gboolean           cog_prefix_routes_handler_unmount (CogPrefixRoutesHandler *self,
                                                      const char             *path_prefix);

COG_API
gboolean           cog_prefix_routes_handler_mount_path
                                                     (CogPrefixRoutesHandler *self,
                                                      const char             *path_prefix,
                                                      const char             *base_path);

G_END_DECLS
