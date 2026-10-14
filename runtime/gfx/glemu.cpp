// See glemu.h.
#include "glemu.h"
#include "../platform/platform.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

void logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

namespace
{
    struct Vtx
    {
        float x, y, z;
        float r, g, b, a;
        float s0, t0, s1, t1;
    };

    struct Mat
    {
        float m[16];
        static Mat identity()
        {
            Mat r{};
            r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1;
            return r;
        }
        Mat operator*(const Mat& b) const
        {
            Mat r{};
            for (int c = 0; c < 4; c++)
                for (int rr = 0; rr < 4; rr++)
                {
                    float s = 0;
                    for (int k = 0; k < 4; k++)
                        s += m[k * 4 + rr] * b.m[c * 4 + k];
                    r.m[c * 4 + rr] = s;
                }
            return r;
        }
    };

    // ---- emulated state
    std::vector<Mat> g_stack[3] = { { Mat::identity() }, { Mat::identity() }, { Mat::identity() } };
    int g_matrixMode = 0;
    float g_color[4] = { 1, 1, 1, 1 };
    float g_tc[2][2] = {};
    int g_unit = 0;
    bool g_tex2D[2] = {};
    uint32_t g_env[2] = { glenum::MODULATE, glenum::MODULATE };
    bool g_alphaTest = false;
    uint32_t g_alphaFunc = GL_ALWAYS;
    float g_alphaRef = 0;
    bool g_fog = false;
    uint32_t g_fogMode = glenum::EXP;
    float g_fogDensity = 1, g_fogStart = 0, g_fogEnd = 1, g_fogColor[4] = {};
    int g_unpackAlign = 4;
    std::unordered_map<uint32_t, bool> g_genMipmap; // texture -> GL_GENERATE_MIPMAP

    // ---- immediate mode and batching
    bool g_inBegin = false;
    uint32_t g_mode = 0;
    std::vector<Vtx> g_prim;   // vertices of the current glBegin
    std::vector<Vtx> g_batch;  // triangles waiting to be drawn
    uint32_t g_batchMode = 0;  // GL_TRIANGLES / GL_LINES / GL_POINTS

    // ---- native objects
    GLuint g_prog, g_vao, g_vbo;
    // How batched vertices reach the GPU (KP_GL_STREAM=map|subdata|client): a ring buffer mapped
    // unsynchronized (desktop GL), glBufferSubData into the ring, or client-side arrays.
    enum class Stream { Map, SubData, Client } g_stream = Stream::Map;
    size_t g_vboSize = 32u << 20, g_vboPos = 0;
    bool g_ready = false, g_bound = false;
    struct Uniforms
    {
        GLint mvp, mv, tex, env, alpha, alphaRef, fog, fogParams, fogColor, s0, s1;
    } g_u;
    struct UniformCache
    {
        Mat mvp, mv;
        int tex[2] = { -1, -1 }, env[2] = { -1, -1 }, alpha = -1, fog = -1;
        float alphaRef = -1, fogParams[3] = { -1, -1, -1 }, fogColor[4] = { -1, -1, -1, -1 };
        bool valid = false;
    } g_uc;

    const char* kVS = R"(
in vec3 aPos; in vec4 aColor; in vec2 aTc0; in vec2 aTc1;
uniform mat4 uMVP; uniform mat4 uMV;
out vec4 vColor; out vec2 vTc0; out vec2 vTc1; out float vEyeZ;
void main() {
  vColor = aColor; vTc0 = aTc0; vTc1 = aTc1;
  vEyeZ = -(uMV * vec4(aPos, 1.0)).z;
  gl_Position = uMVP * vec4(aPos, 1.0);
  gl_PointSize = 1.0;
})";

    const char* kFS = R"(
in vec4 vColor; in vec2 vTc0; in vec2 vTc1; in float vEyeZ;
uniform sampler2D uS0; uniform sampler2D uS1;
uniform ivec2 uTex; uniform ivec2 uEnv; uniform int uAlpha; uniform float uAlphaRef;
uniform int uFog; uniform vec3 uFogParams; uniform vec4 uFogColor;
out vec4 fragColor;
vec4 env(vec4 f, vec4 t, int mode) {
  if (mode == 1) return t;                                         // replace
  if (mode == 2) return vec4(mix(f.rgb, t.rgb, t.a), f.a);         // decal
  if (mode == 3) return vec4(f.rgb + t.rgb, f.a * t.a);            // add
  return f * t;                                                    // modulate
}
void main() {
  vec4 c = vColor;
  if (uTex.x != 0) c = env(c, texture(uS0, vTc0), uEnv.x);
  if (uTex.y != 0) c = env(c, texture(uS1, vTc1), uEnv.y);
  if (uAlpha != 0) {
    bool pass = true;
    if (uAlpha == 1) pass = false;
    else if (uAlpha == 2) pass = c.a < uAlphaRef;
    else if (uAlpha == 3) pass = c.a == uAlphaRef;
    else if (uAlpha == 4) pass = c.a <= uAlphaRef;
    else if (uAlpha == 5) pass = c.a > uAlphaRef;
    else if (uAlpha == 6) pass = c.a != uAlphaRef;
    else if (uAlpha == 7) pass = c.a >= uAlphaRef;
    if (!pass) discard;
  }
  if (uFog != 0) {
    float z = abs(vEyeZ), f;
    if (uFog == 1) f = (uFogParams.y - z) / (uFogParams.y - uFogParams.x);
    else if (uFog == 2) f = exp(-uFogParams.z * z);
    else { float d = uFogParams.z * z; f = exp(-d * d); }
    c.rgb = mix(uFogColor.rgb, c.rgb, clamp(f, 0.0, 1.0));
  }
  fragColor = c;
})";

    GLuint compile(GLenum type, const char* body)
    {
        const std::string src = std::string(KP_GLSL_VERSION) + body;
        const char* p = src.c_str();
        const GLuint sh = ::glCreateShader(type);
        ::glShaderSource(sh, 1, &p, nullptr);
        ::glCompileShader(sh);
        GLint ok = 0;
        ::glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
        if (!ok)
        {
            char log[2048];
            ::glGetShaderInfoLog(sh, sizeof log, nullptr, log);
            logf("[glemu] shader error: %s", log);
        }
        return sh;
    }

    Mat& top() { return g_stack[g_matrixMode].back(); }

    int envIndex(uint32_t mode)
    {
        switch (mode)
        {
        case glenum::REPLACE: return 1;
        case glenum::DECAL: return 2;
        case glenum::ADD: return 3;
        default: return 0;
        }
    }

    void bindObjects()
    {
        ::glUseProgram(g_prog);
        if (g_stream == Stream::Client)
        {
            ::glBindVertexArray(0); // client arrays live in the default vertex array
            ::glBindBuffer(GL_ARRAY_BUFFER, 0);
            for (GLuint a = 0; a < 4; a++)
                ::glEnableVertexAttribArray(a);
        }
        else
        {
            ::glBindVertexArray(g_vao);
            ::glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
        }
        g_bound = true;
    }

    void applyUniforms()
    {
        const Mat mv = g_stack[0].back(), mvp = g_stack[1].back() * mv;
        UniformCache& u = g_uc;
        if (!u.valid || memcmp(u.mvp.m, mvp.m, sizeof mvp.m))
        {
            ::glUniformMatrix4fv(g_u.mvp, 1, GL_FALSE, mvp.m);
            u.mvp = mvp;
        }
        if (g_fog && (!u.valid || memcmp(u.mv.m, mv.m, sizeof mv.m)))
        {
            ::glUniformMatrix4fv(g_u.mv, 1, GL_FALSE, mv.m);
            u.mv = mv;
        }
        const int tex0 = g_tex2D[0], tex1 = g_tex2D[1];
        if (!u.valid || u.tex[0] != tex0 || u.tex[1] != tex1)
        {
            ::glUniform2i(g_u.tex, tex0, tex1);
            u.tex[0] = tex0;
            u.tex[1] = tex1;
        }
        const int e0 = envIndex(g_env[0]), e1 = envIndex(g_env[1]);
        if (!u.valid || u.env[0] != e0 || u.env[1] != e1)
        {
            ::glUniform2i(g_u.env, e0, e1);
            u.env[0] = e0;
            u.env[1] = e1;
        }
        const int alpha = g_alphaTest ? (int)(g_alphaFunc - GL_NEVER) + 1 : 0; // NEVER..ALWAYS -> 1..8
        const int alphaMode = alpha == 8 ? 0 : alpha;
        if (!u.valid || u.alpha != alphaMode)
        {
            ::glUniform1i(g_u.alpha, alphaMode);
            u.alpha = alphaMode;
        }
        if (!u.valid || u.alphaRef != g_alphaRef)
        {
            ::glUniform1f(g_u.alphaRef, g_alphaRef);
            u.alphaRef = g_alphaRef;
        }
        const int fog = !g_fog ? 0 : g_fogMode == glenum::LINEAR_FOG ? 1 : g_fogMode == glenum::EXP ? 2 : 3;
        if (!u.valid || u.fog != fog)
        {
            ::glUniform1i(g_u.fog, fog);
            u.fog = fog;
        }
        if (fog)
        {
            const float params[3] = { g_fogStart, g_fogEnd, g_fogDensity };
            if (!u.valid || memcmp(u.fogParams, params, sizeof params))
            {
                ::glUniform3fv(g_u.fogParams, 1, params);
                memcpy(u.fogParams, params, sizeof params);
            }
            if (!u.valid || memcmp(u.fogColor, g_fogColor, sizeof g_fogColor))
            {
                ::glUniform4fv(g_u.fogColor, 1, g_fogColor);
                memcpy(u.fogColor, g_fogColor, sizeof g_fogColor);
            }
        }
        u.valid = true;
    }

    // Appends the current primitive to the batch as triangles / lines / points.
    void emitPrimitive()
    {
        const std::vector<Vtx>& v = g_prim;
        const size_t n = v.size();
        uint32_t mode = GL_TRIANGLES;
        std::vector<Vtx> out;
        switch (g_mode)
        {
        case GL_TRIANGLES:
            out.assign(v.begin(), v.begin() + (n / 3) * 3);
            break;
        case GL_TRIANGLE_STRIP:
        case glenum::QUAD_STRIP:
            for (size_t i = 2; i < n; i++)
            {
                if ((i & 1) == 0)
                    out.insert(out.end(), { v[i - 2], v[i - 1], v[i] });
                else
                    out.insert(out.end(), { v[i - 1], v[i - 2], v[i] });
            }
            break;
        case GL_TRIANGLE_FAN:
        case glenum::POLYGON:
            for (size_t i = 2; i < n; i++)
                out.insert(out.end(), { v[0], v[i - 1], v[i] });
            break;
        case glenum::QUADS:
            for (size_t i = 0; i + 3 < n; i += 4)
                out.insert(out.end(), { v[i], v[i + 1], v[i + 2], v[i], v[i + 2], v[i + 3] });
            break;
        case GL_LINES:
            mode = GL_LINES;
            out.assign(v.begin(), v.begin() + (n / 2) * 2);
            break;
        case GL_LINE_STRIP:
        case GL_LINE_LOOP:
            mode = GL_LINES;
            for (size_t i = 1; i < n; i++)
                out.insert(out.end(), { v[i - 1], v[i] });
            if (g_mode == GL_LINE_LOOP && n > 2)
                out.insert(out.end(), { v[n - 1], v[0] });
            break;
        case GL_POINTS:
            mode = GL_POINTS;
            out = v;
            break;
        default:
            return;
        }
        if (out.empty())
            return;
        if (!g_batch.empty() && g_batchMode != mode)
            emu::flush();
        g_batchMode = mode;
        g_batch.insert(g_batch.end(), out.begin(), out.end());
    }

    void pushVertex(float x, float y, float z)
    {
        g_prim.push_back({ x, y, z, g_color[0], g_color[1], g_color[2], g_color[3], g_tc[0][0], g_tc[0][1], g_tc[1][0],
                           g_tc[1][1] });
    }

    // Converts an upload to tightly packed RGBA8.
    const uint8_t* toRGBA(uint32_t format, int w, int h, const void* pixels, std::vector<uint8_t>& buf)
    {
        if (!pixels)
            return nullptr;
        int bpp;
        switch (format)
        {
        case GL_RGBA: case glenum::BGRA: bpp = 4; break;
        case GL_RGB: case glenum::BGR: bpp = 3; break;
        case glenum::LUMINANCE_ALPHA: bpp = 2; break;
        default: bpp = 1; break;
        }
        const size_t rowIn = ((size_t)w * bpp + g_unpackAlign - 1) / g_unpackAlign * g_unpackAlign;
        if (format == GL_RGBA && rowIn == (size_t)w * 4)
            return static_cast<const uint8_t*>(pixels);
        buf.resize((size_t)w * h * 4);
        const uint8_t* src = static_cast<const uint8_t*>(pixels);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
            {
                const uint8_t* p = src + y * rowIn + x * bpp;
                uint8_t* o = &buf[((size_t)y * w + x) * 4];
                switch (format)
                {
                case GL_RGBA: o[0] = p[0]; o[1] = p[1]; o[2] = p[2]; o[3] = p[3]; break;
                case glenum::BGRA: o[0] = p[2]; o[1] = p[1]; o[2] = p[0]; o[3] = p[3]; break;
                case GL_RGB: o[0] = p[0]; o[1] = p[1]; o[2] = p[2]; o[3] = 255; break;
                case glenum::BGR: o[0] = p[2]; o[1] = p[1]; o[2] = p[0]; o[3] = 255; break;
                case glenum::LUMINANCE_ALPHA: o[0] = o[1] = o[2] = p[0]; o[3] = p[1]; break;
                case glenum::ALPHA: o[0] = o[1] = o[2] = 255; o[3] = p[0]; break;
                default: o[0] = o[1] = o[2] = p[0]; o[3] = 255; break; // luminance / intensity
                }
            }
        return buf.data();
    }

    GLuint boundTexture()
    {
        GLint t = 0;
        ::glGetIntegerv(GL_TEXTURE_BINDING_2D, &t);
        return (GLuint)t;
    }
}

namespace emu
{
    void init()
    {
        if (g_ready)
            return;
        g_prog = ::glCreateProgram();
        ::glAttachShader(g_prog, compile(GL_VERTEX_SHADER, kVS));
        ::glAttachShader(g_prog, compile(GL_FRAGMENT_SHADER, kFS));
        ::glBindAttribLocation(g_prog, 0, "aPos");
        ::glBindAttribLocation(g_prog, 1, "aColor");
        ::glBindAttribLocation(g_prog, 2, "aTc0");
        ::glBindAttribLocation(g_prog, 3, "aTc1");
        ::glLinkProgram(g_prog);
        GLint ok = 0;
        ::glGetProgramiv(g_prog, GL_LINK_STATUS, &ok);
        if (!ok)
        {
            char log[2048];
            ::glGetProgramInfoLog(g_prog, sizeof log, nullptr, log);
            logf("[glemu] link error: %s", log);
        }
        g_u = { ::glGetUniformLocation(g_prog, "uMVP"), ::glGetUniformLocation(g_prog, "uMV"),
                ::glGetUniformLocation(g_prog, "uTex"), ::glGetUniformLocation(g_prog, "uEnv"),
                ::glGetUniformLocation(g_prog, "uAlpha"), ::glGetUniformLocation(g_prog, "uAlphaRef"),
                ::glGetUniformLocation(g_prog, "uFog"), ::glGetUniformLocation(g_prog, "uFogParams"),
                ::glGetUniformLocation(g_prog, "uFogColor"), ::glGetUniformLocation(g_prog, "uS0"),
                ::glGetUniformLocation(g_prog, "uS1") };
        ::glUseProgram(g_prog);
        ::glUniform1i(g_u.s0, 0);
        ::glUniform1i(g_u.s1, 1);
        ::glGenVertexArrays(1, &g_vao);
        ::glBindVertexArray(g_vao);
        ::glGenBuffers(1, &g_vbo);
        ::glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
        ::glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)g_vboSize, nullptr, GL_STREAM_DRAW);
        const GLsizei stride = sizeof(Vtx);
        ::glEnableVertexAttribArray(0);
        ::glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(Vtx, x));
        ::glEnableVertexAttribArray(1);
        ::glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(Vtx, r));
        ::glEnableVertexAttribArray(2);
        ::glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(Vtx, s0));
        ::glEnableVertexAttribArray(3);
        ::glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(Vtx, s1));
        const char* stream = getenv("KP_GL_STREAM");
        g_stream = stream ? (!strcmp(stream, "client") ? Stream::Client : !strcmp(stream, "subdata") ? Stream::SubData : Stream::Map)
                          : (KP_GLES ? Stream::Client : Stream::Map);
        g_ready = true;
        g_bound = false;
        logf("[glemu] fixed-function emulation ready (%s), vertex streaming: %s", (const char*)::glGetString(GL_VERSION),
             g_stream == Stream::Client ? "client arrays" : g_stream == Stream::SubData ? "glBufferSubData" : "mapped ring");
    }

    void invalidate()
    {
        g_bound = false;
        g_uc.valid = false;
    }

    uint64_t g_drawCount = 0;
    uint64_t drawCount() { return g_drawCount; }

    void flush()
    {
        if (g_batch.empty() || !g_ready)
        {
            g_batch.clear();
            return;
        }
        ProfScope prof(kProfDraw);
        if (!g_bound)
            bindObjects();
        applyUniforms();
        if (g_stream == Stream::Client)
        {
            const GLsizei stride = sizeof(Vtx);
            const char* base = reinterpret_cast<const char*>(g_batch.data());
            ::glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, base + offsetof(Vtx, x));
            ::glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, stride, base + offsetof(Vtx, r));
            ::glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, base + offsetof(Vtx, s0));
            ::glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, stride, base + offsetof(Vtx, s1));
            ::glDrawArrays(g_batchMode, 0, (GLsizei)g_batch.size());
            g_drawCount++;
            g_batch.clear();
            return;
        }
        const size_t bytes = g_batch.size() * sizeof(Vtx);
        if (bytes > g_vboSize)
        {
            g_vboSize = bytes * 2;
            ::glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)g_vboSize, nullptr, GL_STREAM_DRAW);
            g_vboPos = 0;
        }
        if (g_vboPos + bytes > g_vboSize)
        {
            ::glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)g_vboSize, nullptr, GL_STREAM_DRAW); // orphan
            g_vboPos = 0;
        }
        // Ring buffer: a fresh range is never in use by the GPU (the buffer is orphaned on wrap), so
        // it can be written without synchronisation.
        if (g_stream == Stream::SubData)
            ::glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)g_vboPos, (GLsizeiptr)bytes, g_batch.data());
        else if (void* dst = ::glMapBufferRange(GL_ARRAY_BUFFER, (GLintptr)g_vboPos, (GLsizeiptr)bytes,
                                                GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT | GL_MAP_INVALIDATE_RANGE_BIT))
        {
            memcpy(dst, g_batch.data(), bytes);
            ::glUnmapBuffer(GL_ARRAY_BUFFER);
        }
        ::glDrawArrays(g_batchMode, (GLint)(g_vboPos / sizeof(Vtx)), (GLsizei)g_batch.size());
        g_drawCount++;
        g_vboPos += bytes;
        g_batch.clear();
    }

    // ---- immediate mode
    void glBegin(uint32_t mode)
    {
        g_inBegin = true;
        g_mode = mode;
        g_prim.clear();
    }
    void glEnd()
    {
        g_inBegin = false;
        emitPrimitive();
    }
    void glVertex2f(float x, float y) { pushVertex(x, y, 0); }
    void glVertex3f(float x, float y, float z) { pushVertex(x, y, z); }
    void glVertex3fv(const float* v) { pushVertex(v[0], v[1], v[2]); }
    void glVertex2fv(const float* v) { pushVertex(v[0], v[1], 0); }
    void glVertex2i(int32_t x, int32_t y) { pushVertex((float)x, (float)y, 0); }
    void glTexCoord2f(float s, float t)
    {
        g_tc[0][0] = s;
        g_tc[0][1] = t;
    }
    void glTexCoord2fv(const float* v) { glTexCoord2f(v[0], v[1]); }
    void glMultiTexCoord2f(uint32_t unit, float s, float t)
    {
        const int u = (int)(unit - GL_TEXTURE0) & 1;
        g_tc[u][0] = s;
        g_tc[u][1] = t;
    }
    void glMultiTexCoord2fv(uint32_t unit, const float* v) { glMultiTexCoord2f(unit, v[0], v[1]); }
    void glColor4f(float r, float g, float b, float a)
    {
        g_color[0] = r;
        g_color[1] = g;
        g_color[2] = b;
        g_color[3] = a;
    }
    void glColor3f(float r, float g, float b) { glColor4f(r, g, b, 1); }
    void glColor3fv(const float* v) { glColor4f(v[0], v[1], v[2], 1); }
    void glColor4fv(const float* v) { glColor4f(v[0], v[1], v[2], v[3]); }
    void glColor4ub(uint8_t r, uint8_t g, uint8_t b, uint8_t a) { glColor4f(r / 255.f, g / 255.f, b / 255.f, a / 255.f); }
    void glColor3ub(uint8_t r, uint8_t g, uint8_t b) { glColor4ub(r, g, b, 255); }
    void glColor4ubv(const uint8_t* v) { glColor4ub(v[0], v[1], v[2], v[3]); }
    void glColor3ubv(const uint8_t* v) { glColor4ub(v[0], v[1], v[2], 255); }

    // ---- matrices (column-major, like GL)
    void glMatrixMode(uint32_t mode)
    {
        g_matrixMode = mode == glenum::PROJECTION ? 1 : mode == glenum::TEXTURE_MATRIX ? 2 : 0;
    }
    void glLoadIdentity()
    {
        flush();
        top() = Mat::identity();
    }
    void glLoadMatrixf(const float* m)
    {
        flush();
        memcpy(top().m, m, sizeof top().m);
    }
    void glMultMatrixf(const float* m)
    {
        flush();
        Mat b;
        memcpy(b.m, m, sizeof b.m);
        top() = top() * b;
    }
    void glPushMatrix() { g_stack[g_matrixMode].push_back(top()); }
    void glPopMatrix()
    {
        flush();
        if (g_stack[g_matrixMode].size() > 1)
            g_stack[g_matrixMode].pop_back();
    }
    void glRotatef(float angle, float x, float y, float z)
    {
        const float len = std::sqrt(x * x + y * y + z * z);
        if (len == 0)
            return;
        x /= len;
        y /= len;
        z /= len;
        const float a = angle * (float)M_PI / 180.0f, c = std::cos(a), s = std::sin(a), t = 1 - c;
        Mat r = Mat::identity();
        r.m[0] = x * x * t + c;
        r.m[1] = y * x * t + z * s;
        r.m[2] = x * z * t - y * s;
        r.m[4] = x * y * t - z * s;
        r.m[5] = y * y * t + c;
        r.m[6] = y * z * t + x * s;
        r.m[8] = x * z * t + y * s;
        r.m[9] = y * z * t - x * s;
        r.m[10] = z * z * t + c;
        glMultMatrixf(r.m);
    }
    void glTranslatef(float x, float y, float z)
    {
        Mat r = Mat::identity();
        r.m[12] = x;
        r.m[13] = y;
        r.m[14] = z;
        glMultMatrixf(r.m);
    }
    void glScalef(float x, float y, float z)
    {
        Mat r = Mat::identity();
        r.m[0] = x;
        r.m[5] = y;
        r.m[10] = z;
        glMultMatrixf(r.m);
    }
    void glOrtho(double l, double r, double b, double t, double n, double f)
    {
        Mat m{};
        m.m[0] = (float)(2 / (r - l));
        m.m[5] = (float)(2 / (t - b));
        m.m[10] = (float)(-2 / (f - n));
        m.m[12] = (float)(-(r + l) / (r - l));
        m.m[13] = (float)(-(t + b) / (t - b));
        m.m[14] = (float)(-(f + n) / (f - n));
        m.m[15] = 1;
        glMultMatrixf(m.m);
    }
    void glFrustum(double l, double r, double b, double t, double n, double f)
    {
        Mat m{};
        m.m[0] = (float)(2 * n / (r - l));
        m.m[5] = (float)(2 * n / (t - b));
        m.m[8] = (float)((r + l) / (r - l));
        m.m[9] = (float)((t + b) / (t - b));
        m.m[10] = (float)(-(f + n) / (f - n));
        m.m[11] = -1;
        m.m[14] = (float)(-2 * f * n / (f - n));
        glMultMatrixf(m.m);
    }

    // ---- state
    void glEnable(uint32_t cap)
    {
        flush();
        switch (cap)
        {
        case glenum::TEXTURE_2D: g_tex2D[g_unit] = true; return;
        case glenum::ALPHA_TEST: g_alphaTest = true; return;
        case glenum::FOG: g_fog = true; return;
        case GL_BLEND: case GL_DEPTH_TEST: case GL_CULL_FACE: case GL_POLYGON_OFFSET_FILL: case GL_SCISSOR_TEST:
        case GL_STENCIL_TEST: case GL_DITHER:
            ::glEnable(cap);
            return;
        default: return; // lighting, smoothing, texgen...: not used for drawing here
        }
    }
    void glDisable(uint32_t cap)
    {
        flush();
        switch (cap)
        {
        case glenum::TEXTURE_2D: g_tex2D[g_unit] = false; return;
        case glenum::ALPHA_TEST: g_alphaTest = false; return;
        case glenum::FOG: g_fog = false; return;
        case GL_BLEND: case GL_DEPTH_TEST: case GL_CULL_FACE: case GL_POLYGON_OFFSET_FILL: case GL_SCISSOR_TEST:
        case GL_STENCIL_TEST: case GL_DITHER:
            ::glDisable(cap);
            return;
        default: return;
        }
    }
    uint8_t glIsEnabled(uint32_t cap)
    {
        switch (cap)
        {
        case glenum::TEXTURE_2D: return g_tex2D[g_unit];
        case glenum::ALPHA_TEST: return g_alphaTest;
        case glenum::FOG: return g_fog;
        default: return ::glIsEnabled(cap);
        }
    }
    void glActiveTexture(uint32_t unit)
    {
        flush();
        g_unit = (int)(unit - GL_TEXTURE0) & 1;
        ::glActiveTexture(unit);
    }
    void glClientActiveTexture(uint32_t) {}
    void glBindTexture(uint32_t target, uint32_t tex)
    {
        flush();
        ::glBindTexture(target == glenum::TEXTURE_2D ? GL_TEXTURE_2D : target, tex);
    }
    void glTexEnvi(uint32_t target, uint32_t pname, int32_t v)
    {
        if (target == glenum::TEXTURE_ENV && pname == glenum::TEXTURE_ENV_MODE)
        {
            flush();
            g_env[g_unit] = (uint32_t)v;
        }
    }
    void glTexEnvf(uint32_t target, uint32_t pname, float v) { glTexEnvi(target, pname, (int32_t)v); }
    void glAlphaFunc(uint32_t func, float ref)
    {
        flush();
        g_alphaFunc = func;
        g_alphaRef = ref;
    }
    void glFogf(uint32_t pname, float v)
    {
        flush();
        switch (pname)
        {
        case glenum::FOG_MODE: g_fogMode = (uint32_t)v; break;
        case glenum::FOG_DENSITY: g_fogDensity = v; break;
        case glenum::FOG_START: g_fogStart = v; break;
        case glenum::FOG_END: g_fogEnd = v; break;
        default: break;
        }
    }
    void glFogi(uint32_t pname, int32_t v) { glFogf(pname, (float)v); }
    void glFogfv(uint32_t pname, const float* v)
    {
        flush();
        if (pname == glenum::FOG_COLOR)
            memcpy(g_fogColor, v, sizeof g_fogColor);
        else
            glFogf(pname, v[0]);
    }
    void glShadeModel(uint32_t) {}
    void glBlendFunc(uint32_t s, uint32_t d)
    {
        flush();
        ::glBlendFunc(s, d);
    }
    void glDepthFunc(uint32_t f)
    {
        flush();
        ::glDepthFunc(f);
    }
    void glDepthMask(uint8_t m)
    {
        flush();
        ::glDepthMask(m);
    }
    void glDepthRange(double n, double f)
    {
        flush();
#if KP_GLES
        ::glDepthRangef((float)n, (float)f);
#else
        ::glDepthRange(n, f);
#endif
    }
    void glCullFace(uint32_t m)
    {
        flush();
        ::glCullFace(m);
    }
    void glFrontFace(uint32_t m)
    {
        flush();
        ::glFrontFace(m);
    }
    void glColorMask(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
    {
        flush();
        ::glColorMask(r, g, b, a);
    }
    void glPolygonMode(uint32_t, uint32_t) {}
    void glPolygonOffset(float factor, float units)
    {
        flush();
        ::glPolygonOffset(factor, units);
    }
    void glScissor(int32_t x, int32_t y, int32_t w, int32_t h)
    {
        flush();
        ::glScissor(x, y, w, h);
    }
    void glViewport(int32_t x, int32_t y, int32_t w, int32_t h)
    {
        flush();
        ::glViewport(x, y, w, h);
    }
    void glStencilFunc(uint32_t f, int32_t ref, uint32_t mask)
    {
        flush();
        ::glStencilFunc(f, ref, mask);
    }
    void glStencilOp(uint32_t a, uint32_t b, uint32_t c)
    {
        flush();
        ::glStencilOp(a, b, c);
    }
    void glStencilMask(uint32_t m)
    {
        flush();
        ::glStencilMask(m);
    }
    void glHint(uint32_t, uint32_t) {}
    void glLineWidth(float) {}
    void glPointSize(float) {}
    void glDrawBuffer(uint32_t) {}
    void glReadBuffer(uint32_t) {}

    void glClear(uint32_t mask)
    {
        flush();
        ::glClear(mask & (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT));
    }
    void glClearColor(float r, float g, float b, float a) { ::glClearColor(r, g, b, a); }
    void glClearDepth(double d)
    {
#if KP_GLES
        ::glClearDepthf((float)d);
#else
        ::glClearDepth(d);
#endif
    }
    void glClearStencil(int32_t s) { ::glClearStencil(s); }
    void glFinish()
    {
        flush();
        ::glFinish();
    }
    void glFlush()
    {
        flush();
        ::glFlush();
    }
    uint32_t glGetError()
    {
        while (::glGetError() != GL_NO_ERROR) {} // keep the emulator's own errors from the game
        return GL_NO_ERROR;
    }
    void glGetFloatv(uint32_t pname, float* v)
    {
        switch (pname)
        {
        case glenum::MODELVIEW_MATRIX: memcpy(v, g_stack[0].back().m, 64); return;
        case glenum::PROJECTION_MATRIX: memcpy(v, g_stack[1].back().m, 64); return;
        default: ::glGetFloatv(pname, v); return;
        }
    }
    void glGetIntegerv(uint32_t pname, int32_t* v)
    {
        switch (pname)
        {
        case glenum::MATRIX_MODE: *v = (int32_t)(glenum::MODELVIEW + g_matrixMode); return;
        case GL_UNPACK_ALIGNMENT: *v = g_unpackAlign; return;
        default: ::glGetIntegerv(pname, v); return;
        }
    }
    void glReadPixels(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t format, uint32_t type, void* pixels)
    {
        flush();
        if (format == GL_RGBA && type == GL_UNSIGNED_BYTE)
        {
            ::glReadPixels(x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
            return;
        }
        // GL_RGB (screenshots): read RGBA and pack.
        std::vector<uint8_t> tmp((size_t)w * h * 4);
        ::glReadPixels(x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, tmp.data());
        uint8_t* out = static_cast<uint8_t*>(pixels);
        for (size_t i = 0; i < (size_t)w * h; i++)
            for (int c = 0; c < 3; c++)
                out[i * 3 + c] = tmp[i * 4 + c];
    }
    void glPixelStorei(uint32_t pname, int32_t v)
    {
        if (pname == GL_UNPACK_ALIGNMENT)
            g_unpackAlign = v;
        else
            ::glPixelStorei(pname, v);
    }

    // ---- textures
    void glGenTextures(int32_t n, uint32_t* out) { ::glGenTextures(n, out); }
    void glDeleteTextures(int32_t n, const uint32_t* names)
    {
        flush();
        for (int32_t k = 0; k < n; k++)
            g_genMipmap.erase(names[k]);
        ::glDeleteTextures(n, names);
    }
    void glTexImage2D(uint32_t target, int32_t level, int32_t, int32_t w, int32_t h, int32_t, uint32_t format, uint32_t type,
                      const void* pixels)
    {
        flush();
        if (type != GL_UNSIGNED_BYTE)
            return;
        std::vector<uint8_t> buf;
        const uint8_t* rgba = toRGBA(format, w, h, pixels, buf);
        ::glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        ::glTexImage2D(target == glenum::TEXTURE_2D ? GL_TEXTURE_2D : target, level, GL_RGBA8, w, h, 0, GL_RGBA,
                       GL_UNSIGNED_BYTE, rgba);
        ::glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        if (level == 0)
        {
            auto it = g_genMipmap.find(boundTexture());
            if (it != g_genMipmap.end() && it->second)
                ::glGenerateMipmap(GL_TEXTURE_2D);
        }
    }
    void glTexSubImage2D(uint32_t target, int32_t level, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t format,
                         uint32_t type, const void* pixels)
    {
        flush();
        if (type != GL_UNSIGNED_BYTE)
            return;
        std::vector<uint8_t> buf;
        const uint8_t* rgba = toRGBA(format, w, h, pixels, buf);
        ::glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        ::glTexSubImage2D(target == glenum::TEXTURE_2D ? GL_TEXTURE_2D : target, level, x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE,
                          rgba);
        ::glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    }
    void glTexParameteri(uint32_t target, uint32_t pname, int32_t v)
    {
        flush();
        if (pname == glenum::GENERATE_MIPMAP)
        {
            g_genMipmap[boundTexture()] = v != 0;
            return;
        }
        if ((pname == GL_TEXTURE_WRAP_S || pname == GL_TEXTURE_WRAP_T) && (uint32_t)v == glenum::CLAMP)
            v = GL_CLAMP_TO_EDGE;
        ::glTexParameteri(target == glenum::TEXTURE_2D ? GL_TEXTURE_2D : target, pname, v);
    }
    void glTexParameterf(uint32_t target, uint32_t pname, float v)
    {
        if (pname == GL_TEXTURE_MAX_ANISOTROPY_EXT)
        {
            flush();
            ::glTexParameterf(GL_TEXTURE_2D, pname, v);
            return;
        }
        glTexParameteri(target, pname, (int32_t)v);
    }
    void glGetTexParameteriv(uint32_t target, uint32_t pname, int32_t* v) { ::glGetTexParameteriv(target, pname, v); }

    void readTexture(uint32_t tex, int32_t w, int32_t h, void* rgba)
    {
        flush();
        GLint prevFbo = 0;
        ::glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
        GLuint fbo;
        ::glGenFramebuffers(1, &fbo);
        ::glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        ::glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        ::glPixelStorei(GL_PACK_ALIGNMENT, 1);
        ::glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        ::glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFbo);
        ::glDeleteFramebuffers(1, &fbo);
    }
}
