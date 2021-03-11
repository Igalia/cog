/*
 * headless-backend.cpp
 * Copyright (C) 2021 Igalia S.L
 *
 * Distributed under terms of the MIT license.
 */

#include <EGL/eglplatform.h>
#include <glib.h>
#include <wpe/wpe-egl.h>
#include <wpe/wpe.h>

extern "C" {

struct wpe_renderer_host_interface cog_headless_renderer_host = {
    // create
    []() -> void* {
        return nullptr;
    },
    // destroy
    [](void* data) {
    },
    // create_client
    [](void* data) -> int {
        return 0;
    },
};

struct wpe_renderer_backend_egl_interface cog_headless_renderer_backend_egl = {
    // create
    [](int host_fd) -> void* {
        return nullptr;
    },
    // destroy
    [](void* data) {
    },
    // get_native_display
    [](void* data) -> EGLNativeDisplayType {
        return 0;
    },
};

struct wpe_renderer_backend_egl_target_interface
    cog_headless_renderer_backend_egl_target
    = {
          // create
          [](struct wpe_renderer_backend_egl_target* target,
              int host_fd) -> void* {
              return nullptr;
          },
          // destroy
          [](void* data) {
          },
          // initialize
          [](void* data, void* backend_data, uint32_t width, uint32_t height) {
          },
          // get_native_window
          [](void* data) -> EGLNativeWindowType {
              return 0;
          },
          // resize
          [](void* data, uint32_t width, uint32_t height) {
          },
          // frame_will_render
          [](void* data) {
          },
          // frame_rendered
          [](void* data) {
          },
      };

__attribute__((visibility(
    "default"))) struct wpe_loader_interface _wpe_loader_interface
    = {
          [](const char* object_name) -> void* {
              if (!strcmp(object_name, "_wpe_renderer_host_interface"))
                  return &cog_headless_renderer_host;

              if (!strcmp(object_name, "_wpe_renderer_backend_egl_interface"))
                  return &cog_headless_renderer_backend_egl;
              if (!strcmp(object_name, "_wpe_renderer_backend_egl_target_interface"))
                  return &cog_headless_renderer_backend_egl_target;

              return nullptr;
          },
      };
}
