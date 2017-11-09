/*
 * dy-webkit-utils.h
 * Copyright (C) 2017 Adrian Perez <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#ifndef DY_WEBKIT_UTILS_H
#define DY_WEBKIT_UTILS_H

#include <glib.h>

#if DY_WEBKIT_GTK
# include <webkit2/webkit2.h>
#else
# include <wpe/webkit.h>

G_BEGIN_DECLS

/* Define cleanup functions to enable using g_auto* with WebKit types. */

G_DEFINE_AUTOPTR_CLEANUP_FUNC (WebKitWebContext, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (WebKitWebView, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (WebKitSettings, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (WebKitWebsiteDataManager, g_object_unref)

#endif

G_END_DECLS

#endif /* !DY_WEBKIT_UTILS_H */
