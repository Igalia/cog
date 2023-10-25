/*
 * cog-gl-utils.h
 * Copyright (C) 2021-2022 Igalia S.L.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <epoxy/egl.h>
#include <epoxy/gl.h>
#include <glib.h>

G_BEGIN_DECLS

typedef GLuint CogGLShaderId;

void cog_gl_shader_id_destroy(CogGLShaderId *shader_id);
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(CogGLShaderId, cog_gl_shader_id_destroy)

CogGLShaderId cog_gl_shader_id_steal(CogGLShaderId *shader_id) G_GNUC_WARN_UNUSED_RESULT;

CogGLShaderId cog_gl_load_shader(const char *source, GLenum kind, GError **) G_GNUC_WARN_UNUSED_RESULT;

bool cog_gl_link_program(GLuint program, GError **);

/*
 * CogGLRenderer wraps an EGLImage (for example provided by wpebackend-fdo)
 * into a texture, then paints a quad which covers the current viewport and
 * samples said texture.
 *
 * A simple GLSL shader program uses the "position" attribute to fetch the
 * coordinates of the quad, and the "texture" attribute the UV mapping
 * coordinates for the texture. Rotation is achieved by changing the UV
 * mapping. The "texture" uniform is used to reference the texture unit
 * where the frame textures get loaded.
 *
 * By itself the renderer only knows how to prepare the shader program
 * and how to use it to paint a textured quad with the given EGLImage as
 * its texture. The rest of EGL/GL/GLES handling is left out intentionally.
 *
 * To use the renderer:
 *
 * - Setup:
 *   - Embed a CogGLRenderer somewhere. Typically this will be a Cog
 *     platform implementation.
 *   - Initialize EGL, ensure eglBindAPI(EGL_OPENGL_ES_API) is called,
 *     create an EGLContext suitable for rendering.
 *   - With the EGLContext active, call cog_gl_renderer_initialize().
 *
 * - Painting:
 *   - Activate the EGLContext.
 *   - Optionally, paint before using the renderer (e.g. some solid color
 *     background below the image, shows if the web view and its content
 *     have transparency).
 *   - Use glViewport() to set the region to be painted on, then
 *     call cog_gl_renderer_paint() to cover the region with the image.
 *   - Optionally, paint afterwards (e.g. some user interface shown over
 *     or around the image).
 *
 * - Shutdown:
 *   - Call cog_gl_renderer_finalize() to dispose of the shader program
 *     and texture used for painting.
 */

typedef struct {
    GLuint vao;
    GLuint program;
    GLuint texture;
    GLuint buffer_vertex;
    GLint  attrib_position;
    GLint  attrib_texture;
    GLint  uniform_texture;
} CogGLRenderer;

typedef enum {
    COG_GL_RENDERER_ROTATION_0 = 0,
    COG_GL_RENDERER_ROTATION_90 = 1,
    COG_GL_RENDERER_ROTATION_180 = 2,
    COG_GL_RENDERER_ROTATION_270 = 3,
} CogGLRendererRotation;

bool cog_gl_renderer_initialize(CogGLRenderer *self, GError **error);
void cog_gl_renderer_finalize(CogGLRenderer *self);
void cog_gl_renderer_paint(CogGLRenderer *self, EGLImage *image, CogGLRendererRotation rotation);

G_END_DECLS
