/*
 * dy-webkit-utils.h
 * Copyright (C) 2017 Adrian Perez <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#pragma once

#if !(defined(DY_INSIDE_DINGHY__) && DY_INSIDE_DINGHY__)
# error "Do not include this header directly, use <dinghy.h> instead"
#endif

#include "dy-config.h"

#if DY_USE_WEBKITGTK
# include <webkit2/webkit2.h>
#else
# include <glib.h>
# include <wpe/webkit.h>

G_BEGIN_DECLS

/* Define cleanup functions to enable using g_auto* with WebKit types. */

G_DEFINE_AUTOPTR_CLEANUP_FUNC (WebKitWebContext, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (WebKitWebView, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (WebKitSettings, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (WebKitURISchemeRequest, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (WebKitWebsiteDataManager, g_object_unref)

#endif /* DY_USE_WEBKITGTK */


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
