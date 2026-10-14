// Fixed-function OpenGL 1.x emulation on modern GL (GL 3.2 core on macOS, GL ES 3.0 on iOS and
// Android): the subset the Kingpin renderer uses.
//
//  * Immediate mode (glBegin/glEnd, quads, polygons, fans, strips) becomes triangles in a
//    streaming vertex buffer; consecutive draws with unchanged state are batched.
//  * Matrix stacks, the texture environment of two units (modulate/replace/decal/add), alpha
//    test and fog are emulated by one shader. Blend, depth, cull, scissor and stencil state is
//    passed through.
//  * Textures are uploaded as RGBA8 (luminance/alpha/RGB/BGRA converted), GL_CLAMP becomes
//    GL_CLAMP_TO_EDGE and GL_GENERATE_MIPMAP is honoured.
// Code that draws with native GL in between (post-processing) calls emu::invalidate() after.
#pragma once

#include <cstdint>

#include "gl_native.h"

// Legacy enums absent from core/ES headers.
namespace glenum
{
    constexpr uint32_t QUADS = 0x0007, QUAD_STRIP = 0x0008, POLYGON = 0x0009;
    constexpr uint32_t MODELVIEW = 0x1700, PROJECTION = 0x1701, TEXTURE_MATRIX = 0x1702;
    constexpr uint32_t MODELVIEW_MATRIX = 0x0BA6, PROJECTION_MATRIX = 0x0BA7, MATRIX_MODE = 0x0BA0;
    constexpr uint32_t TEXTURE_2D = 0x0DE1, ALPHA_TEST = 0x0BC0, FOG = 0x0B60, LIGHTING = 0x0B50;
    constexpr uint32_t TEXTURE_ENV = 0x2300, TEXTURE_ENV_MODE = 0x2200, MODULATE = 0x2100, DECAL = 0x2101,
                       REPLACE = 0x1E01, ADD = 0x0104, BLEND_ENV = 0x0BE2;
    constexpr uint32_t FOG_MODE = 0x0B65, FOG_DENSITY = 0x0B62, FOG_START = 0x0B63, FOG_END = 0x0B64,
                       FOG_COLOR = 0x0B66, LINEAR_FOG = 0x2601, EXP = 0x0800, EXP2 = 0x0801;
    constexpr uint32_t CLAMP = 0x2900, GENERATE_MIPMAP = 0x8191;
    constexpr uint32_t LUMINANCE = 0x1909, LUMINANCE_ALPHA = 0x190A, ALPHA = 0x1906, BGRA = 0x80E1, BGR = 0x80E0;
    constexpr uint32_t FLAT = 0x1D00;
}

namespace emu
{
    void init();       // after the context is current
    void invalidate(); // native GL state was changed behind the emulator's back
    void flush();      // draw any batched geometry
    uint64_t drawCount(); // draw calls issued so far

    // Immediate mode
    void glBegin(uint32_t mode);
    void glEnd();
    void glVertex2f(float x, float y);
    void glVertex3f(float x, float y, float z);
    void glVertex3fv(const float* v);
    void glVertex2fv(const float* v);
    void glVertex2i(int32_t x, int32_t y);
    void glTexCoord2f(float s, float t);
    void glTexCoord2fv(const float* v);
    void glMultiTexCoord2f(uint32_t unit, float s, float t);
    void glMultiTexCoord2fv(uint32_t unit, const float* v);
    void glColor3f(float r, float g, float b);
    void glColor4f(float r, float g, float b, float a);
    void glColor3fv(const float* v);
    void glColor4fv(const float* v);
    void glColor3ub(uint8_t r, uint8_t g, uint8_t b);
    void glColor4ub(uint8_t r, uint8_t g, uint8_t b, uint8_t a);
    void glColor4ubv(const uint8_t* v);
    void glColor3ubv(const uint8_t* v);

    // Matrices
    void glMatrixMode(uint32_t mode);
    void glLoadIdentity();
    void glLoadMatrixf(const float* m);
    void glMultMatrixf(const float* m);
    void glPushMatrix();
    void glPopMatrix();
    void glRotatef(float angle, float x, float y, float z);
    void glTranslatef(float x, float y, float z);
    void glScalef(float x, float y, float z);
    void glOrtho(double l, double r, double b, double t, double n, double f);
    void glFrustum(double l, double r, double b, double t, double n, double f);

    // State
    void glEnable(uint32_t cap);
    void glDisable(uint32_t cap);
    uint8_t glIsEnabled(uint32_t cap);
    void glActiveTexture(uint32_t unit);
    void glClientActiveTexture(uint32_t unit);
    void glBindTexture(uint32_t target, uint32_t tex);
    void glTexEnvf(uint32_t target, uint32_t pname, float v);
    void glTexEnvi(uint32_t target, uint32_t pname, int32_t v);
    void glAlphaFunc(uint32_t func, float ref);
    void glFogf(uint32_t pname, float v);
    void glFogi(uint32_t pname, int32_t v);
    void glFogfv(uint32_t pname, const float* v);
    void glShadeModel(uint32_t mode);
    void glBlendFunc(uint32_t s, uint32_t d);
    void glDepthFunc(uint32_t f);
    void glDepthMask(uint8_t m);
    void glDepthRange(double n, double f);
    void glCullFace(uint32_t m);
    void glFrontFace(uint32_t m);
    void glColorMask(uint8_t r, uint8_t g, uint8_t b, uint8_t a);
    void glPolygonMode(uint32_t face, uint32_t mode);
    void glPolygonOffset(float factor, float units);
    void glScissor(int32_t x, int32_t y, int32_t w, int32_t h);
    void glViewport(int32_t x, int32_t y, int32_t w, int32_t h);
    void glStencilFunc(uint32_t f, int32_t ref, uint32_t mask);
    void glStencilOp(uint32_t a, uint32_t b, uint32_t c);
    void glStencilMask(uint32_t m);
    void glHint(uint32_t target, uint32_t mode);
    void glLineWidth(float w);
    void glPointSize(float s);
    void glDrawBuffer(uint32_t mode);
    void glReadBuffer(uint32_t mode);

    // Clear / misc
    void glClear(uint32_t mask);
    void glClearColor(float r, float g, float b, float a);
    void glClearDepth(double d);
    void glClearStencil(int32_t s);
    void glFinish();
    void glFlush();
    uint32_t glGetError();
    void glGetFloatv(uint32_t pname, float* v);
    void glGetIntegerv(uint32_t pname, int32_t* v);
    void glReadPixels(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t format, uint32_t type, void* pixels);
    void glPixelStorei(uint32_t pname, int32_t v);

    // Textures
    void glGenTextures(int32_t n, uint32_t* out);
    void glDeleteTextures(int32_t n, const uint32_t* names);
    void glTexImage2D(uint32_t target, int32_t level, int32_t internal, int32_t w, int32_t h, int32_t border,
                      uint32_t format, uint32_t type, const void* pixels);
    void glTexSubImage2D(uint32_t target, int32_t level, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t format,
                         uint32_t type, const void* pixels);
    void glTexParameterf(uint32_t target, uint32_t pname, float v);
    void glTexParameteri(uint32_t target, uint32_t pname, int32_t v);
    void glGetTexParameteriv(uint32_t target, uint32_t pname, int32_t* v);
    // Reads level 0 of the bound 2D texture as RGBA8 (GL ES has no glGetTexImage).
    void readTexture(uint32_t tex, int32_t w, int32_t h, void* rgba);
}
