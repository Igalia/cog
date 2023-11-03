/*
 * cog-viewport-wl.c
 * Copyright (C) 2023 Pablo Saavedra <psaavedra@igalia.com>
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../core/cog.h"

#include "cog-viewport-wl.h"

G_DEFINE_DYNAMIC_TYPE(CogWlViewport, cog_wl_viewport, COG_TYPE_VIEWPORT)

static void cog_wl_viewport_dispose(GObject *);

/*
 * CogWlViewport instantiation.
 */

static void
cog_wl_viewport_class_init(CogWlViewportClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = cog_wl_viewport_dispose;
}

static void
cog_wl_viewport_init(CogWlViewport *self)
{
}

/*
 * CogWlViewport deinstantiation.
 */

static void
cog_wl_viewport_class_finalize(CogWlViewportClass *klass G_GNUC_UNUSED)
{
}

static void
cog_wl_viewport_dispose(GObject *object)
{
    G_OBJECT_CLASS(cog_wl_viewport_parent_class)->dispose(object);
}

/*
 * Method definitions.
 */

/*
 * CogWlViewport register type method.
 */

void
cog_wl_viewport_register_type_exported(GTypeModule *type_module)
{
    cog_wl_viewport_register_type(type_module);
}
