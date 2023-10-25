/*
 * egl-proc-address.h
 * Copyright (C) 2020 Igalia S.L.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#define __USE_GNU
#include <dlfcn.h>

#include <EGL/egl.h>

static inline void *
load_egl_proc_address(const char *name)
{
    void *proc_address = eglGetProcAddress (name);
    if (!proc_address)
        proc_address = dlsym (RTLD_NEXT, name);
    return proc_address;
}
