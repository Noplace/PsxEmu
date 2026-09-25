/*****************************************************************************************************************
* Copyright (c) 2012 Khalid Ali Al-Kooheji                                                                       *
*                                                                                                                *
* Permission is hereby granted, free of charge, to any person obtaining a copy of this software and              *
* associated documentation files (the "Software"), to deal in the Software without restriction, including        *
* without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell        *
* copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the       *
* following conditions:                                                                                          *
*                                                                                                                *
* The above copyright notice and this permission notice shall be included in all copies or substantial           *
* portions of the Software.                                                                                      *
*                                                                                                                *
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT          *
* LIMITED TO THE WARRANTIES OF MERCHANTABILITY, * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.          *
* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, * DAMAGES OR OTHER LIABILITY,      *
* WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE            *
* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                                                         *
*****************************************************************************************************************/
#pragma once

// The OpenGL the engine uses beyond what opengl32.dll exports.
//
// Windows' own gl.h stops at OpenGL 1.1; everything newer - shaders, framebuffer objects, sampler
// objects, the WGL calls that make a 3.3 core context - has to be asked of the driver by name once
// a context exists. glext.h and wglext.h are the usual way to spell those, but the Windows SDK does
// not ship them, so this declares just the few dozen the engine calls, with the values the
// Khronos registry gives, rather than vendoring a loader for a thousand functions it never uses.

#include "app/framework.h"

#include <GL/gl.h>

#pragma comment(lib, "opengl32.lib")

namespace psxemu {

    // Types gl.h (1.1) does not have.
    typedef char GLchar;

    // Enumerants past 1.1.
    inline constexpr GLenum kGlFragmentShader = 0x8B30;
    inline constexpr GLenum kGlVertexShader = 0x8B31;
    inline constexpr GLenum kGlCompileStatus = 0x8B81;
    inline constexpr GLenum kGlLinkStatus = 0x8B82;
    inline constexpr GLenum kGlInfoLogLength = 0x8B84;
    inline constexpr GLenum kGlTexture0 = 0x84C0;
    inline constexpr GLenum kGlClampToEdge = 0x812F;
    inline constexpr GLenum kGlBgra = 0x80E1;
    inline constexpr GLenum kGlTextureMaxLevel = 0x813D;
    inline constexpr GLenum kGlFramebuffer = 0x8D40;
    inline constexpr GLenum kGlColorAttachment0 = 0x8CE0;
    inline constexpr GLenum kGlFramebufferComplete = 0x8CD5;

    // WGL_ARB_create_context and _profile.
    inline constexpr int kWglContextMajorVersion = 0x2091;
    inline constexpr int kWglContextMinorVersion = 0x2092;
    inline constexpr int kWglContextFlags = 0x2094;
    inline constexpr int kWglContextProfileMask = 0x9126;
    inline constexpr int kWglContextCoreProfileBit = 0x0001;
    inline constexpr int kWglContextForwardCompatibleBit = 0x0002;

    // Each entry: return type, name without its gl/wgl prefix, parameter list. The prefix is added
    // back when the driver is asked for it.
#define PSXEMU_GL_FUNCTIONS(X)                                                                     \
    X(GLuint, CreateShader, (GLenum type))                                                         \
    X(void, ShaderSource, (GLuint shader, GLsizei count, const GLchar* const* string,              \
                           const GLint* length))                                                   \
    X(void, CompileShader, (GLuint shader))                                                        \
    X(void, GetShaderiv, (GLuint shader, GLenum name, GLint* params))                              \
    X(void, GetShaderInfoLog, (GLuint shader, GLsizei size, GLsizei* length, GLchar* log))         \
    X(void, DeleteShader, (GLuint shader))                                                         \
    X(GLuint, CreateProgram, (void))                                                               \
    X(void, AttachShader, (GLuint program, GLuint shader))                                         \
    X(void, LinkProgram, (GLuint program))                                                         \
    X(void, GetProgramiv, (GLuint program, GLenum name, GLint* params))                            \
    X(void, GetProgramInfoLog, (GLuint program, GLsizei size, GLsizei* length, GLchar* log))       \
    X(void, DeleteProgram, (GLuint program))                                                       \
    X(void, UseProgram, (GLuint program))                                                          \
    X(GLint, GetUniformLocation, (GLuint program, const GLchar* name))                             \
    X(void, Uniform1i, (GLint location, GLint v0))                                                 \
    X(void, Uniform1f, (GLint location, GLfloat v0))                                               \
    X(void, Uniform2f, (GLint location, GLfloat v0, GLfloat v1))                                   \
    X(void, Uniform4f, (GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3))           \
    X(void, GenVertexArrays, (GLsizei n, GLuint* arrays))                                          \
    X(void, BindVertexArray, (GLuint array))                                                       \
    X(void, DeleteVertexArrays, (GLsizei n, const GLuint* arrays))                                 \
    X(void, ActiveTexture, (GLenum texture))                                                       \
    X(void, GenSamplers, (GLsizei n, GLuint* samplers))                                            \
    X(void, DeleteSamplers, (GLsizei n, const GLuint* samplers))                                   \
    X(void, BindSampler, (GLuint unit, GLuint sampler))                                            \
    X(void, SamplerParameteri, (GLuint sampler, GLenum name, GLint param))                         \
    X(void, GenFramebuffers, (GLsizei n, GLuint* framebuffers))                                    \
    X(void, DeleteFramebuffers, (GLsizei n, const GLuint* framebuffers))                           \
    X(void, BindFramebuffer, (GLenum target, GLuint framebuffer))                                  \
    X(void, FramebufferTexture2D, (GLenum target, GLenum attachment, GLenum textarget,             \
                                   GLuint texture, GLint level))                                   \
    X(GLenum, CheckFramebufferStatus, (GLenum target))

    struct GlFunctions {
#define PSXEMU_GL_DECLARE(ret, name, params)                                                       \
    typedef ret(APIENTRY* name##Proc) params;                                                      \
    name##Proc name = nullptr;
        PSXEMU_GL_FUNCTIONS(PSXEMU_GL_DECLARE)
#undef PSXEMU_GL_DECLARE

        // WGL, asked for separately: they are what gets the 3.3 context in the first place.
        typedef HGLRC(WINAPI* CreateContextAttribsProc)(HDC dc, HGLRC share, const int* attributes);
        typedef BOOL(WINAPI* SwapIntervalProc)(int interval);
        CreateContextAttribsProc CreateContextAttribs = nullptr;
        SwapIntervalProc SwapInterval = nullptr;   // WGL_EXT_swap_control; may be absent

        // Needs a current context. False if any of the GL functions is missing - the driver is
        // older than 3.3, and the engine gives up so the factory can fall back.
        bool Load() {
            bool all = true;
#define PSXEMU_GL_LOAD(ret, name, params)                                                          \
    name = reinterpret_cast<name##Proc>(Find("gl" #name));                                         \
    all = all && name != nullptr;
            PSXEMU_GL_FUNCTIONS(PSXEMU_GL_LOAD)
#undef PSXEMU_GL_LOAD
            SwapInterval = reinterpret_cast<SwapIntervalProc>(Find("wglSwapIntervalEXT"));
            return all;
        }

        void LoadCreateContext() {
            CreateContextAttribs =
                reinterpret_cast<CreateContextAttribsProc>(Find("wglCreateContextAttribsARB"));
        }

     private:
        // wglGetProcAddress answers for extensions and anything past 1.1; some drivers return
        // small integers rather than null for a name they do not have, which count as missing.
        static PROC Find(const char* name) {
            PROC proc = wglGetProcAddress(name);
            const intptr_t value = reinterpret_cast<intptr_t>(proc);
            if (value == 0 || value == 1 || value == 2 || value == 3 || value == -1)
                return nullptr;
            return proc;
        }
    };

}   // namespace psxemu
