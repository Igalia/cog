/*
 * cog-xdp-parent-wl.h
 * Copyright (C) 2023 SUSE Software Solutions Germany GmbH
 *
 * Distributed under terms of the MIT license.
 */

#pragma once

#include <libportal/types.h>

#include "xdg-foreign-unstable-v2-client.h"

G_BEGIN_DECLS

struct xdp_parent_wl_data {
    struct wl_surface       *wl_surface;
    struct zxdg_exporter_v2 *zxdg_exporter;
    struct zxdg_exported_v2 *zxdg_exported;
    gpointer                 user_data;
};

XdpParent *xdp_parent_new_wl(struct xdp_parent_wl_data *wl_data);

G_END_DECLS
