/*
 * dy-uri-handler.h
 * Copyright (C) 2017 Adrian Perez <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#ifndef DY_URI_HANDLER_H
#define DY_URI_HANDLER_H

#if !(defined(DY_INSIDE_DINGHY__) && DY_INSIDE_DINGHY__)
# error "Do not include this header directly, use <dinghy.h> instead"
#endif

#include "dy-webkit-utils.h"

G_BEGIN_DECLS

#define DY_TYPE_URI_HANDLER  (dy_uri_handler_get_type ())

G_DECLARE_FINAL_TYPE (DyURIHandler, dy_uri_handler, DY, URI_HANDLER, GObject)

typedef struct _DyLauncher          DyLauncher;
typedef struct _DyURIHandlerRequest DyURIHandlerRequest;


typedef void (*DyURIHandlerCallback)  (DyURIHandlerRequest *request,
                                       void                *user_data);


DyURIHandler *dy_uri_handler_new        (const char          *scheme);
const char   *dy_uri_handler_get_scheme (DyURIHandler        *handler);
gboolean      dy_uri_handler_register   (DyURIHandler        *handler,
                                         const char          *prefix,
                                         DyURIHandlerCallback callback,
                                         void                *user_data);
gboolean      dy_uri_handler_unregister (DyURIHandler        *handler,
                                         const char          *prefix);
void          dy_uri_handler_attach     (DyURIHandler        *handler,
                                         DyLauncher          *launcher);

DyURIHandler *dy_uri_handler_request_get_handler   (DyURIHandlerRequest *request);
const char   *dy_uri_handler_request_get_scheme    (DyURIHandlerRequest *request);
const char   *dy_uri_handler_request_get_prefix    (DyURIHandlerRequest *request);
const char   *dy_uri_handler_request_get_path      (DyURIHandlerRequest *request);
void          dy_uri_handler_request_load_bytes    (DyURIHandlerRequest *request,
                                                    const char          *mime_type,
                                                    GBytes              *bytes);
void          dy_uri_handler_request_load_string   (DyURIHandlerRequest *request,
                                                    const char          *mime_type,
                                                    const char          *data,
                                                    gssize               len);
void          dy_uri_handler_request_error         (DyURIHandlerRequest *request,
                                                    const char          *format,
                                                    ...);

G_END_DECLS

#endif /* !DY_URI_HANDLER_H */
