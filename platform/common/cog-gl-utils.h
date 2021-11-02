/*
 * cog-gl-utils.h
 * Copyright (C) 2021 Igalia S.L.
 *
 * Distributed under terms of the MIT license.
 */

#pragma once

#include <epoxy/gl.h>
#include <glib.h>

G_BEGIN_DECLS

typedef GLuint CogGLShaderId;

void cog_gl_shader_id_destroy(CogGLShaderId *shader_id);
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(CogGLShaderId, cog_gl_shader_id_destroy)

CogGLShaderId cog_gl_shader_id_steal(CogGLShaderId *shader_id) G_GNUC_WARN_UNUSED_RESULT;

CogGLShaderId cog_gl_load_shader(const char *source, GLenum kind, GError **) G_GNUC_WARN_UNUSED_RESULT;

bool cog_gl_link_program(GLuint program, GError **);

G_END_DECLS
