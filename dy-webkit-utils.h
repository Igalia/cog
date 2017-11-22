/*
 * dy-webkit-utils.h
 * Copyright (C) 2017 Adrian Perez <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#ifndef DY_WEBKIT_UTILS_H
#define DY_WEBKIT_UTILS_H

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
G_DEFINE_AUTOPTR_CLEANUP_FUNC (WebKitWebsiteDataManager, g_object_unref)

#endif /* DY_USE_WEBKITGTK */

G_END_DECLS

#endif /* !DY_WEBKIT_UTILS_H */
