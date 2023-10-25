/*
 * cog-xdp-parent-x11.h
 * Copyright (C) 2023 SUSE Software Solutions Germany GmbH
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <libportal/types.h>
#include <xcb/xproto.h>

G_BEGIN_DECLS

XdpParent *xdp_parent_new_x11(xcb_window_t *window);

G_END_DECLS
