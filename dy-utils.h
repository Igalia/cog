/*
 * dy-utils.h
 * Copyright (C) 2018 Adrian Perez <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#pragma once

#if !(defined(DY_INSIDE_DINGHY__) && DY_INSIDE_DINGHY__)
# error "Do not include this header directly, use <dinghy.h> instead"
#endif

#include "dy-webkit-utils.h"

G_BEGIN_DECLS

gboolean dy_handle_web_view_load_failed (WebKitWebView  *web_view,
                                         WebKitLoadEvent load_event,
                                         char           *failing_uri,
                                         GError         *error,
                                         void           *userdata);

gboolean dy_handle_web_view_load_failed_with_tls_errors (WebKitWebView       *web_view,
                                                         char                *failing_uri,
                                                         GTlsCertificate     *certificate,
                                                         GTlsCertificateFlags errors,
                                                         void                *user_data);

gboolean dy_handle_web_view_web_process_crashed (WebKitWebView *web_view,
                                                 void          *userdata);

void dy_web_view_connect_default_error_handlers (WebKitWebView *web_view);


void dy_handle_web_view_load_changed (WebKitWebView  *web_view,
                                      WebKitLoadEvent load_event,
                                      void           *userdata);

void dy_web_view_connect_default_progress_handlers (WebKitWebView *web_view);

G_END_DECLS
