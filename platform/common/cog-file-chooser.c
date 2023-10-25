/*
 * cog-file-chooser.c
 * Copyright (C) 2023 SUSE Software Solutions Germany GmbH
 *
 * SPDX-License-Identifier: MIT
 */

#include "cog-file-chooser.h"

static void
on_file_chooser_response(GObject *object, GAsyncResult *result, gpointer user_data)
{
    g_autoptr(GError)                   error = NULL;
    g_autoptr(XdpPortal)                xdp_portal = XDP_PORTAL(object);
    g_autoptr(WebKitFileChooserRequest) request = WEBKIT_FILE_CHOOSER_REQUEST(user_data);
    g_autoptr(GVariant)                 val = xdp_portal_open_file_finish(xdp_portal, result, &error);
    if (!val) {
        g_debug("File chooser failed: %s", error->message);
        webkit_file_chooser_request_cancel(request);
        return;
    }

    g_autofree const char **uris = NULL;
    g_variant_lookup(val, "uris", "^a&s", &uris);
    webkit_file_chooser_request_select_files(request, uris);
}

void
run_file_chooser(WebKitWebView *view, WebKitFileChooserRequest *request, XdpParent *xdp_parent)
{
    XdpPortal *xdp_portal = xdp_portal_new();
    if (!xdp_portal)
        return;

    GVariantBuilder all_filters_builder;
    g_variant_builder_init(&all_filters_builder, G_VARIANT_TYPE("a(sa(us))"));

    // Add "Supported files" filters from mime types
    const char *const *mime_types = webkit_file_chooser_request_get_mime_types(request);
    if (mime_types) {
        GVariantBuilder one_filter_builder;
        g_variant_builder_init(&one_filter_builder, G_VARIANT_TYPE("a(us)"));
        for (int i = 0; mime_types[i]; i++) {
            g_variant_builder_add(&one_filter_builder, "(us)", 1, mime_types[i]);
        }
        g_variant_builder_add(&all_filters_builder, "(s@a(us))", "Supported files",
                              g_variant_builder_end(&one_filter_builder));
    }

    // Add "All files" filter
    GVariantBuilder one_filter_builder;
    g_variant_builder_init(&one_filter_builder, G_VARIANT_TYPE("a(us)"));
    g_variant_builder_add(&one_filter_builder, "(us)", 0, "*");
    g_variant_builder_add(&all_filters_builder, "(s@a(us))", "All files", g_variant_builder_end(&one_filter_builder));

    gboolean select_multiple = webkit_file_chooser_request_get_select_multiple(request);
    xdp_portal_open_file(xdp_portal, xdp_parent, select_multiple ? "Select Files" : "Select File",
                         g_variant_builder_end(&all_filters_builder), NULL, NULL,
                         select_multiple ? XDP_OPEN_FILE_FLAG_MULTIPLE : XDP_OPEN_FILE_FLAG_NONE, NULL,
                         on_file_chooser_response, g_object_ref(request));
}
