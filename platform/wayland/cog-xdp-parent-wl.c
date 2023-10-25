/*
 * cog-xdp-parent-wl.c
 * Copyright (C) 2023 SUSE Software Solutions Germany GmbH
 *
 * SPDX-License-Identifier: MIT
 */

#include "cog-xdp-parent-wl.h"

#include "../common/xdp-parent-private.h"

static void
handle_exported(void *data, struct zxdg_exported_v2 *zxdg_exported_v2, const char *handle)
{
    XdpParent                 *parent = data;
    struct xdp_parent_wl_data *wl_data = (struct xdp_parent_wl_data *) parent->data;
    g_autofree char           *handle_str = g_strdup_printf("wayland:%s", handle);

    parent->callback(parent, handle_str, wl_data->user_data);
}

static const struct zxdg_exported_v2_listener zxdg_exported_listener = {
    .handle = handle_exported,
};

static gboolean
xdp_parent_export_wl(XdpParent *parent, XdpParentExported callback, gpointer user_data)
{
    struct xdp_parent_wl_data *wl_data = (struct xdp_parent_wl_data *) parent->data;

    parent->callback = callback;
    wl_data->user_data = user_data;
    wl_data->zxdg_exported = zxdg_exporter_v2_export_toplevel(wl_data->zxdg_exporter, wl_data->wl_surface);

    return zxdg_exported_v2_add_listener(wl_data->zxdg_exported, &zxdg_exported_listener, parent);
}

static void
xdp_parent_unexport_wl(XdpParent *parent)
{
    struct xdp_parent_wl_data *wl_data = (struct xdp_parent_wl_data *) parent->data;

    zxdg_exported_v2_destroy(wl_data->zxdg_exported);
}

XdpParent *
xdp_parent_new_wl(struct xdp_parent_wl_data *wl_data)
{
    XdpParent *parent = g_new0(XdpParent, 1);
    parent->parent_export = xdp_parent_export_wl;
    parent->parent_unexport = xdp_parent_unexport_wl;
    parent->data = (gpointer) wl_data;
    return parent;
}
