// Post-processing and presentation (GL 3.2 core / GL ES 3.0; the game itself draws through the
// fixed-function emulation in runtime/gfx/glemu.cpp).
//
// The game renders into an off-screen multisampled framebuffer instead of the window.
//  * When it switches from its 3D view to 2D drawing (the 2D glOrtho), the 3D image is resolved
//    and gets screen-space ambient occlusion (from the depth buffer) and bloom; the HUD and menus
//    are then drawn on top, unaffected.
//  * At SwapBuffers the frame is drawn into the window framebuffer through a gamma curve
//    (brightness) and a light contrast-adaptive sharpen.
// Settings: KP_SSAO (default 0.8), KP_BLOOM (0.35), KP_SHARPEN (0.25), KP_MSAA (samples, 4),
// KP_POST=0 (no AO/bloom/sharpen), KP_POST_DEBUG=1 (show the AO buffer); Cmd+P toggles effects.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <string>

#include "../gfx/glemu.h"
#include "hle.h"

#include "../platform/platform.h"

namespace
{
    bool g_ready = false;
    int g_w = 0, g_h = 0, g_samples = 4;
    GLuint g_msFbo, g_msColor, g_msDepth;    // game renders here
    GLuint g_resFbo, g_resColor, g_resDepth; // resolved colour + depth (textures)
    GLuint g_aoFbo, g_aoTex;                 // half-resolution ambient occlusion
    GLuint g_bloomFbo[2], g_bloomTex[2];     // quarter-resolution bloom ping-pong
    GLuint g_vao;
    GLuint g_progAO, g_progBright, g_progBlur, g_progComposite, g_progFinal, g_progCopy;
    bool g_3dThisFrame = false, g_any3D = false;
    uint64_t g_drawsAtFrustum = 0; // emu draw count when the 3D view was set up
    GLint g_frustumViewport[4] = {}; // the 3D view's viewport
}

// Set by the game layer (kingpin_tweaks.cpp): true while AO/bloom must not run, e.g. while a
// full-screen console covers a 3D image that is not redrawn (bloom would pile up on stale pixels).
bool (*g_postSuppressed)() = nullptr;

namespace
{
    double g_near = 4, g_far = 4096, g_tanX = 1, g_tanY = 0.75;

    float envf(const char* name, float def)
    {
        const char* v = getenv(name);
        return v ? (float)atof(v) : def;
    }
    // Settings are read in glPostInit (platforms may set options after static initialisation).
    std::atomic<float> g_ssao{ 0.8f }, g_bloom{ 0.35f }, g_sharpen{ 0.25f };
    std::atomic<bool> g_effectsOn{ true };

    // Full-screen triangle; uv from the vertex id.
    const char* kVS = R"(
out vec2 uv;
void main() {
  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
  uv = p;
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
})";

    const char* kAO = R"(
in vec2 uv; out vec4 fragColor;
uniform sampler2D depthTex; uniform vec2 texel; uniform float zn, zf; uniform vec2 tanHalf;
float lin(float d) { return 2.0 * zn * zf / (zf + zn - (2.0 * d - 1.0) * (zf - zn)); }
vec3 viewPos(vec2 t) { float z = lin(texture(depthTex, t).r); return vec3((t * 2.0 - 1.0) * tanHalf * z, -z); }
void main() {
  float d = texture(depthTex, uv).r;
  if (d >= 0.99999) { fragColor = vec4(1.0); return; }
  vec3 p = viewPos(uv);
  vec3 n = normalize(cross(viewPos(uv + vec2(texel.x, 0.0)) - p, viewPos(uv + vec2(0.0, texel.y)) - p));
  float radius = 32.0;
  vec2 r = radius / (-p.z * tanHalf) * 0.5;
  float ao = 0.0;
  float a = fract(sin(dot(uv, vec2(12.9898, 78.233))) * 43758.5453) * 6.2831;
  for (int i = 0; i < 12; i++) {
    float t = (float(i) + 0.5) / 12.0;
    float ang = a + float(i) * 2.39996;
    vec3 q = viewPos(uv + vec2(cos(ang), sin(ang)) * r * sqrt(t));
    vec3 v = q - p;
    float l = length(v);
    ao += max(0.0, dot(n, v / max(l, 0.001)) - 0.15) * clamp(1.0 - l / (radius * 1.5), 0.0, 1.0);
  }
  fragColor = vec4(vec3(1.0 - clamp(ao / 12.0 * 2.6, 0.0, 0.85)), 1.0);
})";

    const char* kBright = R"(
in vec2 uv; out vec4 fragColor; uniform sampler2D src;
void main() {
  vec3 c = texture(src, uv).rgb;
  float l = max(c.r, max(c.g, c.b));
  fragColor = vec4(c * smoothstep(0.55, 0.95, l), 1.0);
})";

    const char* kBlur = R"(
in vec2 uv; out vec4 fragColor; uniform sampler2D src; uniform vec2 dir;
void main() {
  vec3 c = texture(src, uv).rgb * 0.227;
  c += (texture(src, uv + dir * 1.385).rgb + texture(src, uv - dir * 1.385).rgb) * 0.316;
  c += (texture(src, uv + dir * 3.231).rgb + texture(src, uv - dir * 3.231).rgb) * 0.070;
  fragColor = vec4(c, 1.0);
})";

    const char* kComposite = R"(
in vec2 uv; out vec4 fragColor;
uniform sampler2D colorTex, aoTex, bloomTex; uniform vec2 aoTexel; uniform float aoStrength, bloomStrength, debugAO;
void main() {
  float ao = 0.0;
  for (int y = -1; y <= 1; y++) for (int x = -1; x <= 1; x++) ao += texture(aoTex, uv + vec2(x, y) * aoTexel).r;
  ao = mix(1.0, ao / 9.0, aoStrength);
  vec3 c = texture(colorTex, uv).rgb * ao + texture(bloomTex, uv).rgb * bloomStrength;
  if (debugAO > 0.5) c = vec3(ao);
  fragColor = vec4(c, 1.0);
})";

    const char* kFinal = R"(
in vec2 uv; out vec4 fragColor; uniform sampler2D src; uniform vec2 texel; uniform float invGamma, sharpen;
void main() {
  vec3 c = texture(src, uv).rgb;
  vec3 n = texture(src, uv + vec2(0.0, texel.y)).rgb, s = texture(src, uv - vec2(0.0, texel.y)).rgb;
  vec3 e = texture(src, uv + vec2(texel.x, 0.0)).rgb, w = texture(src, uv - vec2(texel.x, 0.0)).rgb;
  vec3 mn = min(c, min(min(n, s), min(e, w))), mx = max(c, max(max(n, s), max(e, w)));
  vec3 amp = sqrt(clamp(min(mn, 1.0 - mx) / max(mx, 0.0001), 0.0, 1.0));
  vec3 wgt = -amp * sharpen * 0.2;
  c = clamp((c + (n + s + e + w) * wgt) / (1.0 + 4.0 * wgt), 0.0, 1.0);
  fragColor = vec4(pow(c, vec3(invGamma)), 1.0);
})";

    // Plain textured copy (intro frames), flipped vertically when asked.
    const char* kCopy = R"(
in vec2 uv; out vec4 fragColor; uniform sampler2D src; uniform float flipY;
void main() { fragColor = vec4(texture(src, vec2(uv.x, mix(uv.y, 1.0 - uv.y, flipY))).rgb, 1.0); })";

    GLuint compile(GLenum type, const char* body)
    {
        const std::string src = std::string(KP_GLSL_VERSION) + body;
        const char* p = src.c_str();
        const GLuint sh = glCreateShader(type);
        glShaderSource(sh, 1, &p, nullptr);
        glCompileShader(sh);
        GLint ok = 0;
        glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
        if (!ok)
        {
            char log[2048];
            glGetShaderInfoLog(sh, sizeof log, nullptr, log);
            logf("[post] shader error: %s", log);
        }
        return sh;
    }
    GLuint program(const char* fs)
    {
        const GLuint p = glCreateProgram();
        glAttachShader(p, compile(GL_VERTEX_SHADER, kVS));
        glAttachShader(p, compile(GL_FRAGMENT_SHADER, fs));
        glLinkProgram(p);
        return p;
    }
    GLint U(GLuint prog, const char* name) { return glGetUniformLocation(prog, name); }

    GLuint makeTex(int w, int h, GLint internal, GLenum format, GLenum type, GLint filter)
    {
        GLuint t;
        glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D, t);
        glTexImage2D(GL_TEXTURE_2D, 0, internal, w, h, 0, format, type, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        return t;
    }
    GLuint fboFor(GLuint colorTex)
    {
        GLuint f;
        glGenFramebuffers(1, &f);
        glBindFramebuffer(GL_FRAMEBUFFER, f);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorTex, 0);
        return f;
    }

    void ensurePrograms()
    {
        if (g_progFinal)
            return;
        g_progAO = program(kAO);
        g_progBright = program(kBright);
        g_progBlur = program(kBlur);
        g_progComposite = program(kComposite);
        g_progFinal = program(kFinal);
        g_progCopy = program(kCopy);
        glGenVertexArrays(1, &g_vao);
    }

    // Saves and restores the native state our passes change (the emulator re-applies its own).
    struct StateGuard
    {
        GLint fbo, viewport[4], activeTex, tex[3], vao, prog;
        GLboolean blend, depth, cull, scissor, stencil, depthMask, colorMask[4];
        bool restoreFbo = true;
        StateGuard()
        {
            emu::flush();
            glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
            glGetIntegerv(GL_VIEWPORT, viewport);
            glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTex);
            for (int u = 0; u < 3; u++)
            {
                glActiveTexture(GL_TEXTURE0 + u);
                glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex[u]);
            }
            glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
            glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
            blend = glIsEnabled(GL_BLEND);
            depth = glIsEnabled(GL_DEPTH_TEST);
            cull = glIsEnabled(GL_CULL_FACE);
            scissor = glIsEnabled(GL_SCISSOR_TEST);
            stencil = glIsEnabled(GL_STENCIL_TEST);
            glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
            glGetBooleanv(GL_COLOR_WRITEMASK, colorMask);
            glDisable(GL_BLEND);
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_CULL_FACE);
            glDisable(GL_SCISSOR_TEST);
            glDisable(GL_STENCIL_TEST);
            glDepthMask(GL_FALSE);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glBindVertexArray(g_vao);
        }
        ~StateGuard()
        {
            if (restoreFbo)
                glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)fbo);
            glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
            for (int u = 2; u >= 0; u--)
            {
                glActiveTexture(GL_TEXTURE0 + u);
                glBindTexture(GL_TEXTURE_2D, (GLuint)tex[u]);
            }
            glActiveTexture((GLenum)activeTex);
            glBindVertexArray((GLuint)vao);
            glUseProgram((GLuint)prog);
            auto set = [](GLenum cap, GLboolean on) { on ? glEnable(cap) : glDisable(cap); };
            set(GL_BLEND, blend);
            set(GL_DEPTH_TEST, depth);
            set(GL_CULL_FACE, cull);
            set(GL_SCISSOR_TEST, scissor);
            set(GL_STENCIL_TEST, stencil);
            glDepthMask(depthMask);
            glColorMask(colorMask[0], colorMask[1], colorMask[2], colorMask[3]);
            emu::invalidate();
        }
    };

    void quad() { glDrawArrays(GL_TRIANGLES, 0, 3); }
    void bindTex(int unit, GLuint t)
    {
        glActiveTexture(GL_TEXTURE0 + unit);
        glBindTexture(GL_TEXTURE_2D, t);
    }
    void resolve(bool depth)
    {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, g_msFbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_resFbo);
        glBlitFramebuffer(0, 0, g_w, g_h, 0, 0, g_w, g_h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        if (depth)
            glBlitFramebuffer(0, 0, g_w, g_h, 0, 0, g_w, g_h, GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT, GL_NEAREST);
    }
}

bool glPostEnabled() { return g_ready; }
bool glPostAny3D() { return g_any3D; }
void glPostToggleEffects()
{
    g_effectsOn = !g_effectsOn;
    logf("[post] ambient occlusion / bloom / sharpening %s", g_effectsOn ? "on" : "off");
}

// Called after the game's context is made current (and again after a video restart).
void glPostInit(int w, int h)
{
    emu::init();
    ensurePrograms();
    static bool settingsRead = false;
    if (!settingsRead)
    {
        settingsRead = true;
        g_ssao = envf("KP_SSAO", 0.8f);
        g_bloom = envf("KP_BLOOM", 0.35f);
        g_sharpen = envf("KP_SHARPEN", 0.25f);
        g_effectsOn = !(getenv("KP_POST") && atoi(getenv("KP_POST")) == 0);
    }
    g_w = w;
    g_h = h;
    g_samples = getenv("KP_MSAA") ? std::max(0, atoi(getenv("KP_MSAA"))) : 4;
    GLint maxSamples = 0;
    glGetIntegerv(GL_MAX_SAMPLES, &maxSamples);
    g_samples = std::min(g_samples, (int)maxSamples);

    glGenFramebuffers(1, &g_msFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g_msFbo);
    glGenRenderbuffers(1, &g_msColor);
    glBindRenderbuffer(GL_RENDERBUFFER, g_msColor);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, g_samples, GL_RGBA8, w, h);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, g_msColor);
    glGenRenderbuffers(1, &g_msDepth);
    glBindRenderbuffer(GL_RENDERBUFFER, g_msDepth);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, g_samples, GL_DEPTH24_STENCIL8, w, h);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, g_msDepth);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, g_msDepth);
    const GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);

    g_resColor = makeTex(w, h, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, GL_LINEAR);
    g_resDepth = makeTex(w, h, GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, GL_NEAREST);
    g_resFbo = fboFor(g_resColor);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, g_resDepth, 0);
    g_aoTex = makeTex(w / 2, h / 2, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, GL_LINEAR);
    g_aoFbo = fboFor(g_aoTex);
    for (int k = 0; k < 2; k++)
    {
        g_bloomTex[k] = makeTex(w / 4, h / 4, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, GL_LINEAR);
        g_bloomFbo[k] = fboFor(g_bloomTex[k]);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, g_msFbo);
    glBindTexture(GL_TEXTURE_2D, 0);
    glViewport(0, 0, w, h);
    g_ready = true;
    emu::invalidate();
    logf("[post] off-screen rendering %dx%d, %dx MSAA (status %04X)", w, h, g_samples, st);
}

void glPostFrustum(double l, double r, double b, double t, double n, double f)
{
    g_near = n;
    g_far = f;
    g_tanX = (r - l) / 2 / n;
    g_tanY = (t - b) / 2 / n;
    g_3dThisFrame = true;
    g_any3D = true;
    emu::flush();
    g_drawsAtFrustum = emu::drawCount();
    glGetIntegerv(GL_VIEWPORT, g_frustumViewport);
}

// The game starts 2D drawing: apply AO and bloom to the finished 3D image.
void glPostBegin2D()
{
    if (!g_ready || !g_3dThisFrame)
        return;
    g_3dThisFrame = false;
    if (g_postSuppressed && g_postSuppressed())
        return;
    ProfScope prof(kProfEffects);
    emu::flush();
    static const bool trace = getenv("KP_TRACE_POST") != nullptr;
    if (trace)
    {
        static int n = 0;
        if (n++ % 60 == 0)
        {
            GLint vp[4];
            glGetIntegerv(GL_VIEWPORT, vp);
            logf("[post] 3D pass: %llu draws, viewport %d,%d %dx%d of %dx%d", (unsigned long long)(emu::drawCount() - g_drawsAtFrustum),
                 g_frustumViewport[0], g_frustumViewport[1], g_frustumViewport[2], g_frustumViewport[3], g_w, g_h);
        }
    }
    const float ssao = g_effectsOn ? g_ssao.load() : 0.0f, bloom = g_effectsOn ? g_bloom.load() : 0.0f;
    if (ssao <= 0 && bloom <= 0)
        return;
    StateGuard guard;
    resolve(ssao > 0);
    if (ssao > 0)
    {
        glBindFramebuffer(GL_FRAMEBUFFER, g_aoFbo);
        glViewport(0, 0, g_w / 2, g_h / 2);
        glUseProgram(g_progAO);
        bindTex(0, g_resDepth);
        glUniform1i(U(g_progAO, "depthTex"), 0);
        glUniform2f(U(g_progAO, "texel"), 2.0f / g_w, 2.0f / g_h);
        glUniform1f(U(g_progAO, "zn"), (float)g_near);
        glUniform1f(U(g_progAO, "zf"), (float)g_far);
        glUniform2f(U(g_progAO, "tanHalf"), (float)g_tanX, (float)g_tanY);
        quad();
    }
    if (bloom > 0)
    {
        glViewport(0, 0, g_w / 4, g_h / 4);
        glBindFramebuffer(GL_FRAMEBUFFER, g_bloomFbo[0]);
        glUseProgram(g_progBright);
        bindTex(0, g_resColor);
        glUniform1i(U(g_progBright, "src"), 0);
        quad();
        glUseProgram(g_progBlur);
        glUniform1i(U(g_progBlur, "src"), 0);
        for (int pass = 0; pass < 2; pass++)
        {
            glBindFramebuffer(GL_FRAMEBUFFER, g_bloomFbo[1]);
            bindTex(0, g_bloomTex[0]);
            glUniform2f(U(g_progBlur, "dir"), 4.0f / g_w, 0.0f);
            quad();
            glBindFramebuffer(GL_FRAMEBUFFER, g_bloomFbo[0]);
            bindTex(0, g_bloomTex[1]);
            glUniform2f(U(g_progBlur, "dir"), 0.0f, 4.0f / g_h);
            quad();
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, g_msFbo);
    glViewport(0, 0, g_w, g_h);
    glUseProgram(g_progComposite);
    bindTex(0, g_resColor);
    bindTex(1, ssao > 0 ? g_aoTex : g_resColor);
    bindTex(2, g_bloomTex[0]);
    glUniform1i(U(g_progComposite, "colorTex"), 0);
    glUniform1i(U(g_progComposite, "aoTex"), 1);
    glUniform1i(U(g_progComposite, "bloomTex"), 2);
    glUniform2f(U(g_progComposite, "aoTexel"), 2.0f / g_w, 2.0f / g_h);
    glUniform1f(U(g_progComposite, "aoStrength"), ssao > 0 ? std::min(ssao, 1.0f) : 0.0f);
    glUniform1f(U(g_progComposite, "bloomStrength"), bloom);
    static const float debugAO = getenv("KP_POST_DEBUG") ? 1.0f : 0.0f;
    glUniform1f(U(g_progComposite, "debugAO"), debugAO);
    quad();
}

// SwapBuffers: draw the finished frame into the window framebuffer (gamma + sharpen), with the
// window framebuffer left bound for the platform's present; glPostAfterSwap() switches back.
void glPostPresent(float gamma, int viewW, int viewH, void (*whileWindowBound)())
{
    g_any3D = false;
    if (!g_ready)
        return;
    g_3dThisFrame = false;
    StateGuard guard;
    resolve(false);
    glBindFramebuffer(GL_FRAMEBUFFER, platform::glWindowFramebuffer());
    glViewport(0, 0, viewW > 0 ? viewW : g_w, viewH > 0 ? viewH : g_h);
    glUseProgram(g_progFinal);
    bindTex(0, g_resColor);
    glUniform1i(U(g_progFinal, "src"), 0);
    glUniform2f(U(g_progFinal, "texel"), 1.0f / g_w, 1.0f / g_h);
    glUniform1f(U(g_progFinal, "invGamma"), 1.0f / std::max(gamma, 0.1f));
    glUniform1f(U(g_progFinal, "sharpen"), g_effectsOn ? g_sharpen.load() : 0.0f);
    quad();
    if (whileWindowBound)
        whileWindowBound();
    guard.restoreFbo = false;
}

void glPostAfterSwap()
{
    if (g_ready)
        glBindFramebuffer(GL_FRAMEBUFFER, g_msFbo);
}

// Draws RGBA pixels (top row first) scaled to fit the window framebuffer, keeping their aspect
// ratio (black bars).
void glBlitRgba(const uint32_t* pixels, int w, int h, int viewW, int viewH)
{
    ensurePrograms();
    static GLuint tex;
    if (!tex)
        tex = makeTex(1, 1, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, platform::glWindowFramebuffer());
    glViewport(0, 0, viewW, viewH);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    const double scale = std::min((double)viewW / w, (double)viewH / h);
    const int fw = (int)lround(w * scale), fh = (int)lround(h * scale);
    glViewport((viewW - fw) / 2, (viewH - fh) / 2, fw, fh);
    glBindVertexArray(g_vao);
    glUseProgram(g_progCopy);
    bindTex(0, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glUniform1i(U(g_progCopy, "src"), 0);
    glUniform1f(U(g_progCopy, "flipY"), 1.0f);
    quad();
}

// ---------------------------------------------------------------- GL thunks routed through here

WIN32API(opengl32, glDrawBuffer, 1) {}
WIN32API(opengl32, glReadBuffer, 1) {}
WIN32API(opengl32, glFrustum, 12)
{
    double v[6];
    for (int k = 0; k < 6; k++)
        v[k] = rdf64(c.r[4] + 4 + 8 * k);
    glPostFrustum(v[0], v[1], v[2], v[3], v[4], v[5]);
    emu::glFrustum(v[0], v[1], v[2], v[3], v[4], v[5]);
}
