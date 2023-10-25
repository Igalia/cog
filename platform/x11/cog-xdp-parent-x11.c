/*
 * cog-xdp-parent-x11.c
 * Copyright (C) 2023 SUSE Software Solutions Germany GmbH
 *
 * SPDX-License-Identifier: MIT
 */

#include "cog-xdp-parent-x11.h"

#include "../common/xdp-parent-private.h"

static gboolean
xdp_parent_export_x11(XdpParent *parent, XdpParentExported callback, gpointer user_data)
{
    xcb_window_t    *window_id = (xcb_window_t *) parent->data;
    guint32          xid = (guint32) *window_id;
    g_autofree char *handle = g_strdup_printf("x11:%x", xid);
    callback(parent, handle, user_data);
    return TRUE;
}

static void
xdp_parent_unexport_x11(XdpParent *parent)
{
}

XdpParent *
xdp_parent_new_x11(xcb_window_t *window_id)
{
    XdpParent *parent = g_new0(XdpParent, 1);
    parent->parent_export = xdp_parent_export_x11;
    parent->parent_unexport = xdp_parent_unexport_x11;
    parent->data = (gpointer) window_id;
    return parent;
}
