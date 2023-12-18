/*
 * cog-platform-private.h
 * Copyright (C) 2023 Igalia S.L.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "cog-platform.h"

G_BEGIN_DECLS

typedef struct _CogViewport CogViewport;

void cog_platform_notify_viewport_created(CogViewport *viewport);
void cog_platform_notify_viewport_disposed(CogViewport *viewport);

G_END_DECLS
