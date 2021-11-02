/*
 * cog-gl-utils.c
 * Copyright (C) 2021 Igalia S.L.
 *
 * Distributed under terms of the MIT license.
 */

#include "cog-gl-utils.h"

#include "../../core/cog.h"

void
cog_gl_shader_id_destroy(CogGLShaderId *shader_id)
{
    if (shader_id && *shader_id) {
        glDeleteShader(*shader_id);
        *shader_id = 0;
    }
}

CogGLShaderId
cog_gl_shader_id_steal(CogGLShaderId *shader_id)
{
    g_assert(shader_id);
    CogGLShaderId result = *shader_id;
    *shader_id = 0;
    return result;
}

CogGLShaderId
cog_gl_load_shader(const char *source, GLenum kind, GError **error)
{
    g_assert(source != NULL);
    g_assert(kind == GL_VERTEX_SHADER || kind == GL_FRAGMENT_SHADER);

    g_auto(CogGLShaderId) shader = glCreateShader(kind);
    glShaderSource(shader, 1, &source, NULL);

    GLenum err;
    if ((err = glGetError()) != GL_NO_ERROR) {
        g_set_error_literal(error, COG_PLATFORM_EGL_ERROR, err, "Cannot set shader source");
        return 0;
    }

    glCompileShader(shader);
    if ((err = glGetError()) != GL_NO_ERROR) {
        g_set_error_literal(error, COG_PLATFORM_EGL_ERROR, err, "Cannot compile shader");
        return 0;
    }

    GLint shaderCompiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &shaderCompiled);
    if (shaderCompiled == GL_TRUE)
        return cog_gl_shader_id_steal(&shader);

    GLint log_length = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_length);
    g_autofree char *log = g_new0(char, log_length + 1);
    glGetShaderInfoLog(shader, log_length, NULL, log);
    g_set_error(error, COG_PLATFORM_EGL_ERROR, 0, "Shader compilation: %s", log);
    return 0;
}

bool
cog_gl_link_program(GLuint program, GError **error)
{
    glLinkProgram(program);

    GLint status = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &status);
    if (status)
        return true;

    GLint log_length = 0;
    glGetProgramiv(program, GL_INFO_LOG_LENGTH, &log_length);
    g_autofree char *log = g_new0(char, log_length + 1);
    glGetProgramInfoLog(program, log_length, NULL, log);
    g_set_error(error, COG_PLATFORM_EGL_ERROR, 0, "Shader linking: %s", log);
    return false;
}
