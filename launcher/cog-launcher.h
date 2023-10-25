/*
 * cog-launcher.h
 * Copyright (C) 2021 Igalia S.L.
 * Copyright (C) 2017 Adrian Perez <aperez@igalia.com>
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "../core/cog.h"

G_BEGIN_DECLS

#define COG_TYPE_LAUNCHER (cog_launcher_get_type())

G_DECLARE_FINAL_TYPE(CogLauncher, cog_launcher, COG, LAUNCHER, GApplication)

struct _CogLauncherClass {
    GApplicationClass parent_class;
};

typedef enum {
    COG_SESSION_REGULAR,
    COG_SESSION_AUTOMATED,
} CogSessionType;

CogLauncher              *cog_launcher_new(CogSessionType session_type);
CogShell                 *cog_launcher_get_shell(CogLauncher *launcher);
gboolean                  cog_launcher_is_automated(CogLauncher *launcher);
WebKitSettings           *cog_launcher_get_webkit_settings(CogLauncher *launcher);
WebKitWebsiteDataManager *cog_launcher_get_web_data_manager(CogLauncher *launcher);

G_END_DECLS
