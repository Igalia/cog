/*
 * cog-webkit-utils.h
 * Copyright (C) 2017-2018 Adrian Perez <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#pragma once

#if !(defined(COG_INSIDE_COG__) && COG_INSIDE_COG__)
# error "Do not include this header directly, use <cog.h> instead"
#endif

#include "cog-config.h"

#if COG_USE_WEBKITGTK
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

#endif /* COG_USE_WEBKITGTK */


gboolean cog_handle_web_view_load_failed (WebKitWebView  *web_view,
                                          WebKitLoadEvent load_event,
                                          char           *failing_uri,
                                          GError         *error,
                                          void           *userdata);

gboolean cog_handle_web_view_load_failed_with_tls_errors (WebKitWebView       *web_view,
                                                          char                *failing_uri,
                                                          GTlsCertificate     *certificate,
                                                          GTlsCertificateFlags errors,
                                                          void                *user_data);

gboolean cog_handle_web_view_web_process_crashed (WebKitWebView *web_view,
                                                  void          *userdata);
gboolean cog_handle_web_view_web_process_crashed_exit (WebKitWebView *web_view,
                                                       void          *userdata);

static inline gulong
cog_web_view_connect_web_process_crashed_exit_handler (WebKitWebView* web_view,
                                                       int exit_code)
{
    g_return_val_if_fail (WEBKIT_IS_WEB_VIEW (web_view), 0);
    return g_signal_connect (web_view,
                             "web-process-crashed",
                             G_CALLBACK (cog_handle_web_view_web_process_crashed_exit),
                             GINT_TO_POINTER (exit_code));
}

gulong cog_web_view_connect_web_process_crashed_restart_handler (WebKitWebView *web_view,
                                                                 unsigned       max_tries,
                                                                 unsigned       try_window_ms);

void cog_web_view_connect_default_error_handlers (WebKitWebView *web_view);


void cog_handle_web_view_load_changed (WebKitWebView  *web_view,
                                       WebKitLoadEvent load_event,
                                       void           *userdata);

void cog_web_view_connect_default_progress_handlers (WebKitWebView *web_view);


G_END_DECLS
