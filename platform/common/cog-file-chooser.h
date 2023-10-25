/*
 * cog-file-chooser.h
 * Copyright (C) 2023 SUSE Software Solutions Germany GmbH
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <libportal/portal.h>
#include <wpe/webkit.h>

G_BEGIN_DECLS

void run_file_chooser(WebKitWebView *view, WebKitFileChooserRequest *request, XdpParent *xdp_parent);

G_END_DECLS
