/*
 * cog-gl-utils.c
 * Copyright (C) 2021-2022 Igalia S.L.
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

bool
cog_gl_renderer_initialize(CogGLRenderer *self, GError **error)
{
    g_assert(self);
    g_assert(!self->program);
    g_assert(eglGetCurrentContext() != EGL_NO_CONTEXT);

    static const char *required_gl_extensions[] = {
        "GL_OES_EGL_image",
    };
    for (unsigned i = 0; i < G_N_ELEMENTS(required_gl_extensions); i++) {
        if (!epoxy_has_gl_extension(required_gl_extensions[i])) {
            g_set_error(error, COG_PLATFORM_WPE_ERROR, COG_PLATFORM_WPE_ERROR_INIT, "GL extension %s missing",
                        required_gl_extensions[i]);
            return false;
        }
    }

    static const char vertex_shader_source[] = "#version 100\n"
                                               "attribute vec2 position;\n"
                                               "attribute vec2 texture;\n"
                                               "varying vec2 v_texture;\n"
                                               "void main() {\n"
                                               "  v_texture = texture;\n"
                                               "  gl_Position = vec4(position, 0, 1);\n"
                                               "}\n";
    static const char fragment_shader_source[] = "#version 100\n"
                                                 "precision mediump float;\n"
                                                 "uniform sampler2D u_texture;\n"
                                                 "varying vec2 v_texture;\n"
                                                 "void main() {\n"
                                                 "  gl_FragColor = texture2D(u_texture, v_texture);\n"
                                                 "}\n";

    g_auto(CogGLShaderId) vertex_shader = cog_gl_load_shader(vertex_shader_source, GL_VERTEX_SHADER, error);
    if (!vertex_shader)
        return false;

    g_auto(CogGLShaderId) fragment_shader = cog_gl_load_shader(fragment_shader_source, GL_FRAGMENT_SHADER, error);
    if (!fragment_shader)
        return false;

    if (!(self->program = glCreateProgram())) {
        g_set_error_literal(error, COG_PLATFORM_EGL_ERROR, glGetError(), "Cannot create shader program");
        return false;
    }

    glAttachShader(self->program, vertex_shader);
    glAttachShader(self->program, fragment_shader);
    glBindAttribLocation(self->program, 0, "position");
    glBindAttribLocation(self->program, 1, "texture");

    if (!cog_gl_link_program(self->program, error)) {
        glDeleteProgram(self->program);
        self->program = 0;
        return false;
    }

    self->attrib_position = glGetAttribLocation(self->program, "position");
    self->attrib_texture = glGetAttribLocation(self->program, "texture");

    g_assert(self->attrib_position >= 0 && self->attrib_texture >= 0 && self->uniform_texture >= 0);

    /* Create texture. */
    glGenTextures(1, &self->texture);
    glBindTexture(GL_TEXTURE_2D, self->texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);

    /* Create vertex buffer */
    if (epoxy_is_desktop_gl() || epoxy_gl_version() >= 30) {
        glGenVertexArrays(1, &self->vao);
        glBindVertexArray(self->vao);
    } else {
        self->vao = 0;
    }

    /* clang-format off */
    static const GLfloat vertices[] = {
        /* position */
        -1.0f,  1.0f, 1.0f,  1.0f,
        -1.0f, -1.0f, 1.0f, -1.0f,
        /* texture */
        /* COG_GL_RENDERER_ROTATION_0 */
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 1.0f, 1.0f, 1.0f,
        /* COG_GL_RENDERER_ROTATION_90 */
        1.0f, 0.0f, 1.0f, 1.0f,
        0.0f, 0.0f, 0.0f, 1.0f,
        /* COG_GL_RENDERER_ROTATION_180 */
        1.0f, 1.0f, 0.0f, 1.0f,
        1.0f, 0.0f, 0.0f, 0.0f,
        /* COG_GL_RENDERER_ROTATION_270 */
        0.0f, 1.0f, 0.0f, 0.0f,
        1.0f, 1.0f, 1.0f, 0.0f,
    };
    /* clang-format on */

    glGenBuffers(1, &self->buffer_vertex);
    glBindBuffer(GL_ARRAY_BUFFER, self->buffer_vertex);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    if (self->vao > 0)
        glBindVertexArray(0);

    return true;
}

void
cog_gl_renderer_finalize(CogGLRenderer *self)
{
    g_assert(self);

    if (self->texture) {
        glDeleteTextures(1, &self->texture);
        self->texture = 0;
    }

    if (self->program) {
        glDeleteProgram(self->program);
        self->program = 0;
    }

    if (self->vao > 0) {
        glDeleteVertexArrays(1, &self->vao);
        self->vao = 0;
    }

    if (self->buffer_vertex) {
        glDeleteBuffers(1, &self->buffer_vertex);
        self->buffer_vertex = 0;
    }

    self->attrib_position = 0;
    self->attrib_texture = 0;
    self->uniform_texture = 0;
}

void
cog_gl_renderer_paint(CogGLRenderer *self, EGLImage *image, CogGLRendererRotation rotation)
{
    g_assert(self);
    g_assert(image != EGL_NO_IMAGE);
    g_assert(eglGetCurrentContext() != EGL_NO_CONTEXT);
    g_assert(rotation == COG_GL_RENDERER_ROTATION_0 || rotation == COG_GL_RENDERER_ROTATION_90 ||
             rotation == COG_GL_RENDERER_ROTATION_180 || rotation <= COG_GL_RENDERER_ROTATION_270);

    glUseProgram(self->program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, self->texture);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image);
    glUniform1i(self->uniform_texture, 0);

    if (self->vao > 0)
        glBindVertexArray(self->vao);

    glBindBuffer(GL_ARRAY_BUFFER, self->buffer_vertex);

    glVertexAttribPointer(self->attrib_position, 2, GL_FLOAT, GL_FALSE, 0, (void *) 0);
    glVertexAttribPointer(self->attrib_texture, 2, GL_FLOAT, GL_FALSE, 0,
                          (void *) ((rotation + 1) * 2 * 4 * sizeof(GLfloat)));

    glEnableVertexAttribArray(self->attrib_position);
    glEnableVertexAttribArray(self->attrib_texture);

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glBindBuffer(GL_ARRAY_BUFFER, 0);

    glDisableVertexAttribArray(self->attrib_position);
    glDisableVertexAttribArray(self->attrib_texture);

    if (self->vao > 0)
        glBindVertexArray(0);
}
