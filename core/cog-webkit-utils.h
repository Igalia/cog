/*
 * cog-webkit-utils.h
 * Copyright (C) 2021 Igalia S.L.
 * Copyright (C) 2017-2018 Adrian Perez <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#pragma once

#if !(defined(COG_INSIDE_COG__) && COG_INSIDE_COG__)
# error "Do not include this header directly, use <cog.h> instead"
#endif

#include "cog-config.h"

#include <glib.h>
#include <wpe/webkit.h>

G_BEGIN_DECLS

void cog_set_auto_retry_on_failure(bool value);

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

gboolean cog_handle_web_view_web_process_terminated (WebKitWebView                     *web_view,
                                                     WebKitWebProcessTerminationReason  reason,
                                                     void                              *userdata);
gboolean cog_handle_web_view_web_process_terminated_exit (WebKitWebView                     *web_view,
                                                          WebKitWebProcessTerminationReason  reason,
                                                          void                              *userdata);

static inline gulong
cog_web_view_connect_web_process_terminated_exit_handler (WebKitWebView* web_view,
                                                          int            exit_code)
{
    g_return_val_if_fail (WEBKIT_IS_WEB_VIEW (web_view), 0);
    return g_signal_connect (web_view,
                             "web-process-terminated",
                             G_CALLBACK (cog_handle_web_view_web_process_terminated_exit),
                             GINT_TO_POINTER (exit_code));
}

gulong cog_web_view_connect_web_process_terminated_restart_handler (WebKitWebView *web_view,
                                                                    unsigned       max_tries,
                                                                    unsigned       try_window_ms);

void cog_web_view_connect_default_error_handlers (WebKitWebView *web_view);


void cog_handle_web_view_load_changed (WebKitWebView  *web_view,
                                       WebKitLoadEvent load_event,
                                       void           *userdata);

void cog_web_view_connect_default_progress_handlers (WebKitWebView *web_view);


gboolean cog_webkit_settings_apply_from_key_file (WebKitSettings *settings,
                                                  GKeyFile       *key_file,
                                                  const char     *group,
                                                  GError        **error);

G_END_DECLS
