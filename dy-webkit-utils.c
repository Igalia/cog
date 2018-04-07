/*
 * dy-webkit-utils.c
 * Copyright (C) 2018 Adrian Perez <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#include "dy-webkit-utils.h"
#include <stdlib.h>
#include <string.h>


static const char error_message_template[] =
    "<!DOCTYPE html><html><head><title>%s</title><style type='text/css'>\n"
    "html { background: #fffafa; color: #0f0f0f; }\n"
    "h3 { font-weight: 600; color: #fffafa; background: #555;\n"
    "     border-radius: 3px; padding: 0.15em 0.5em; margin-bottom: 0.25em }\n"
    "p { margin-left: 0.5em; margin-right: 0.5em }\n"
    "p.uri { size: 70%; font-family: monospace; color: #888;\n"
    "        margin-left: 0.75em; margin-top: 0 }\n"
    "</style></head><body>\n"
    "  <h3>%s</h3>\n"
    "  <p class='uri'>%s</p>\n"
    "  <p>%s</p>\n"
    "</body></html>";


gboolean
load_error_page (WebKitWebView *web_view,
                 const char    *failing_uri,
                 const char    *title,
                 const char    *message)
{
    g_warning ("<%s> %s: %s", failing_uri, title, message);

    g_autofree char *html = g_strdup_printf (error_message_template,
                                             title,
                                             title,
                                             failing_uri,
                                             message);
    webkit_web_view_load_alternate_html (web_view,
                                         html,
                                         failing_uri,
                                         NULL);
    return TRUE;
}


gboolean
dy_handle_web_view_load_failed (WebKitWebView  *web_view,
                                WebKitLoadEvent load_event,
                                char           *failing_uri,
                                GError         *error,
                                void           *userdata)
{
    return load_error_page (web_view,
                            failing_uri,
                            "Page load error",
                            error ? error->message : "No error message");
}


static char*
format_tls_error (GTlsCertificateFlags errors)
{
    static const char prefix[] = "TLS certificate ";
    GString *str = g_string_new (prefix);

    if (errors & G_TLS_CERTIFICATE_UNKNOWN_CA)
        g_string_append (str, " has unknown CA,");
    if (errors & G_TLS_CERTIFICATE_BAD_IDENTITY)
        g_string_append (str, " identity mismatch,");
    if (errors & G_TLS_CERTIFICATE_NOT_ACTIVATED)
        g_string_append (str, " has activation time in the future,");
    if (errors & G_TLS_CERTIFICATE_EXPIRED)
        g_string_append (str, " is expired,");
    if (errors & G_TLS_CERTIFICATE_REVOKED)
        g_string_append (str, " is revoked,");
    if (errors & G_TLS_CERTIFICATE_INSECURE)
        g_string_append (str, " uses insecure algorithm,");
    if (errors & G_TLS_CERTIFICATE_GENERIC_ERROR)
        g_string_append (str, " cannot be validated,");

    if (strcmp (str->str, prefix)) {
        g_string_truncate (str, str->len - 1);
        g_string_append_c (str, '.');
    } else {
        g_string_append (str, " unknown error.");
    }

    return g_string_free (str, FALSE);
}


gboolean
dy_handle_web_view_load_failed_with_tls_errors (WebKitWebView       *web_view,
                                                char                *failing_uri,
                                                GTlsCertificate     *certificate,
                                                GTlsCertificateFlags errors,
                                                void                *user_data)
{
    g_autofree char *error_string = format_tls_error (errors);
    return load_error_page (web_view,
                            failing_uri,
                            "TLS Error",
                            error_string);
}


gboolean
dy_handle_web_view_web_process_crashed (WebKitWebView *web_view,
                                        void          *userdata)
{
    static const char *message = 
        "The renderer process crashed. Reloading the page may fix"
        " intermittent failures.";
    return load_error_page (web_view,
                            webkit_web_view_get_uri (web_view),
                            "Crash!",
                            message);
}


gboolean
dy_handle_web_view_web_process_crashed_exit (WebKitWebView *web_view,
                                             void          *userdata)
{
    g_critical ("The rendered process crashed, exiting...");
    exit (GPOINTER_TO_INT (userdata));
}


struct RestartData {
    unsigned tries;
    unsigned max_tries;
    unsigned try_window_ms;
    unsigned tries_timeout_id;
};


static gboolean
reset_recovery_tries (struct RestartData *restart)
{
    restart->tries = 0;
    restart->tries_timeout_id = 0;
    return G_SOURCE_REMOVE;
}


static gboolean
on_web_process_crashed_restart (WebKitWebView *web_view, void *userdata)
{
    struct RestartData *restart = userdata;

#if GLIB_CHECK_VERSION(2, 56, 0)
    g_clear_handle_id (&restart->tries_timeout_id, g_source_remove);
#else
    if (restart->tries_timeout_id) {
        g_source_remove (restart->tries_timeout_id);
        restart->tries_timeout_id = 0;
    }
#endif // GLIB_CHECK_VERSION

    if (++restart->tries >= restart->max_tries) {
        g_critical ("Renderer process crashed and failed to recover within %lums",
                    restart->try_window_ms);
        // Chain up to the handler that renders an error page.
        return dy_handle_web_view_web_process_crashed (web_view, NULL);
    }

    g_warning ("Renderer process crashed, restarting (attempt %u/%u).",
               restart->tries, restart->max_tries);
    webkit_web_view_reload (web_view);

    // Reset the count of attempts if the Web process does not crash again
    // during the configure time window.
    restart->tries_timeout_id = g_timeout_add (restart->try_window_ms,
                                               (GSourceFunc) reset_recovery_tries,
                                               restart);
    return TRUE;
}


static void
free_restart_data (void *restart, G_GNUC_UNUSED GClosure *closure)
{
    g_slice_free (struct RestartData, restart);
}


gulong
dy_web_view_connect_web_process_crashed_restart_handler (WebKitWebView *web_view,
                                                         unsigned       max_tries,
                                                         unsigned       try_window_ms)
{
    g_return_val_if_fail (WEBKIT_IS_WEB_VIEW (web_view), 0);
    g_return_val_if_fail (max_tries > 0, 0);

    struct RestartData *restart = g_slice_new0 (struct RestartData);
    restart->max_tries = max_tries;
    restart->try_window_ms = try_window_ms;

    return g_signal_connect_data (web_view,
                                  "web-process-crashed",
                                  G_CALLBACK (on_web_process_crashed_restart),
                                  restart,
                                  free_restart_data,
                                  0);
}


void
dy_web_view_connect_default_error_handlers (WebKitWebView *web_view)
{
    static const struct {
        const char *sig;
        const void *hnd;
    } handlers[] = {
        { "load-failed",
            dy_handle_web_view_load_failed },
        { "load-failed-with-tls-errors",
            dy_handle_web_view_load_failed_with_tls_errors },
        { "web-process-crashed",
            dy_handle_web_view_web_process_crashed },
    };
    
    for (unsigned i = 0; i < G_N_ELEMENTS (handlers); i++)
        g_signal_connect (web_view, handlers[i].sig, handlers[i].hnd, NULL);
}


void
dy_handle_web_view_load_changed (WebKitWebView  *web_view,
                                 WebKitLoadEvent load_event,
                                 void           *userdata)
{
    const char *info = NULL;
    switch (load_event) {
        case WEBKIT_LOAD_STARTED:
            info = "Load started.";
            break;
        case WEBKIT_LOAD_REDIRECTED:
            info = "Redirected.";
            break;
        case WEBKIT_LOAD_COMMITTED:
            info = "Loading...";
            break;
        case WEBKIT_LOAD_FINISHED:
            info = "Loaded successfully.";
            break;
    }

    g_message ("<%s> %s", webkit_web_view_get_uri (web_view), info);
}


void
dy_web_view_connect_default_progress_handlers (WebKitWebView *web_view)
{
    static const struct {
        const char *sig;
        const void *hnd;
    } handlers[] = {
        { "load-changed", dy_handle_web_view_load_changed },
    };

    for (unsigned i = 0; i < G_N_ELEMENTS (handlers); i++)
        g_signal_connect (web_view, handlers[i].sig, handlers[i].hnd, NULL);
}
