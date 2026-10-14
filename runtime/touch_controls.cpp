// On-screen touch controls for phones and tablets, drawn over the game in the final pass.
//
// They act as a virtual gamepad (pad::touch, combined with any real controller in gamepad.cpp),
// so the game's joystick bindings apply; swiping is mouse look.
//  playing:  left half = floating move stick, right half = look; FIRE JUMP DUCK USE RELOAD,
//            WPN- WPN+ and MENU buttons
//  menus:    D-pad, OK and BACK (the game's menus are keyboard driven)
// Hidden while a game controller is in use (until the screen is touched again). KP_TOUCH_LOOK scales look speed (default 1).
// KP_TOUCH_SCRIPT="t:x:y:x2:y2:dur,..." plays touches for tests (screen fractions; a press at x,y
// that moves to x2,y2 over dur seconds, then lifts).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "gfx/gl_native.h"
#include "platform/platform.h"
#include "win32/hle.h"

bool g_introPlaying = false; // platform/app.cpp

namespace
{
    enum Action
    {
        kA, kB, kX, kY, kLB, kRB, kLT, kRT, // pad button bits 0..7
        kStart = 100, kUp, kDown, kLeft, kRight,
    };

    struct Button
    {
        const char* label;
        float cx, cy, r; // in units of screen height / 10, from the anchor corner
        int anchor;      // 0 top left, 1 top right, 2 bottom left, 3 bottom right
        int action;
    };

    const Button kGameButtons[] = {
        { "FIRE", 2.3f, 2.4f, 1.15f, 3, kRT },  { "JUMP", 4.6f, 1.3f, 0.75f, 3, kA },
        { "DUCK", 5.0f, 3.2f, 0.65f, 3, kB },   { "USE", 4.0f, 4.9f, 0.65f, 3, kX },
        { "RELOAD", 1.9f, 5.1f, 0.65f, 3, kY }, { "WPN-", 4.4f, 1.1f, 0.6f, 1, kLB },
        { "WPN+", 2.6f, 1.1f, 0.6f, 1, kRB },   { "MENU", 2.0f, 1.0f, 0.55f, 0, kStart },
    };
    const Button kMenuButtons[] = {
        { "^", 3.2f, 4.4f, 0.75f, 2, kUp },    { "v", 3.2f, 1.6f, 0.75f, 2, kDown },
        { "<", 1.8f, 3.0f, 0.75f, 2, kLeft },  { ">", 4.6f, 3.0f, 0.75f, 2, kRight },
        { "OK", 2.2f, 2.2f, 1.0f, 3, kA },     { "BACK", 4.6f, 1.4f, 0.75f, 3, kB },
    };

    enum class Role { None, Button, Stick, Look };
    struct Touch
    {
        Role role = Role::None;
        int button = -1;          // index into the active button list
        float x0 = 0, y0 = 0;     // stick origin / last look position
        float x = 0, y = 0;
        std::chrono::steady_clock::time_point start, releaseAt; // releaseAt set: lifted, held briefly
    };
    constexpr auto kMinPress = std::chrono::milliseconds(100); // quick taps still reach the game

    std::mutex g_m;
    std::map<intptr_t, Touch> g_touches;
    int g_w = 1, g_h = 1;
    bool g_menuMode = false;
    float g_lookRemX = 0, g_lookRemY = 0;

    float unit() { return g_h / 10.0f; }
    std::chrono::steady_clock::time_point g_lastTouch;
    bool visible() { return touch::enabled() && (!pad::controllerConnected() || g_lastTouch >= pad::lastUsed()); }

    std::vector<Button> activeButtons()
    {
        return g_menuMode ? std::vector<Button>(std::begin(kMenuButtons), std::end(kMenuButtons))
                          : std::vector<Button>(std::begin(kGameButtons), std::end(kGameButtons));
    }

    void center(const Button& b, float& x, float& y)
    {
        const float u = unit();
        x = (b.anchor & 1) ? g_w - b.cx * u : b.cx * u;
        y = (b.anchor & 2) ? g_h - b.cy * u : b.cy * u;
    }

    // Keeps the current mode in step with the game; a mode change drops every touch.
    void syncMode()
    {
        const bool menu = pad::keyDest() != 0;
        if (menu != g_menuMode)
        {
            g_menuMode = menu;
            g_touches.clear();
        }
    }

    // Recomputes the virtual pad from the active touches.
    void publish()
    {
        pad::State s;
        const auto buttons = activeButtons();
        for (const auto& [id, t] : g_touches)
        {
            if (t.role == Role::Button)
            {
                const int a = buttons[t.button].action;
                if (a < 8)
                    s.buttons |= 1u << a;
                if (a == kRT)
                    s.rt = 1;
                if (a == kStart) s.start = true;
                if (a == kUp) s.up = true;
                if (a == kDown) s.down = true;
                if (a == kLeft) s.left = true;
                if (a == kRight) s.right = true;
            }
            else if (t.role == Role::Stick)
            {
                const float range = 1.2f * unit();
                float dx = (t.x - t.x0) / range, dy = -(t.y - t.y0) / range;
                const float len = std::sqrt(dx * dx + dy * dy);
                if (len > 1)
                {
                    dx /= len;
                    dy /= len;
                }
                s.lx = dx;
                s.ly = dy;
            }
        }
        pad::touch(s);
    }

    // ---- drawing: discs (signed distance) and label pixels, one batch of triangles
    struct Vtx
    {
        float x, y, u, v, r, g, b, a, mode;
    };

    const uint8_t* glyph(char ch)
    {
        static const std::map<char, std::vector<uint8_t>> font = {
            { 'A', { 14, 17, 17, 31, 17, 17, 17 } }, { 'B', { 30, 17, 17, 30, 17, 17, 30 } },
            { 'C', { 14, 17, 16, 16, 16, 17, 14 } }, { 'D', { 30, 17, 17, 17, 17, 17, 30 } },
            { 'E', { 31, 16, 16, 30, 16, 16, 31 } }, { 'F', { 31, 16, 16, 30, 16, 16, 16 } },
            { 'I', { 14, 4, 4, 4, 4, 4, 14 } },      { 'J', { 7, 2, 2, 2, 2, 18, 12 } },
            { 'K', { 17, 18, 20, 24, 20, 18, 17 } }, { 'L', { 16, 16, 16, 16, 16, 16, 31 } },
            { 'M', { 17, 27, 21, 21, 17, 17, 17 } }, { 'N', { 17, 17, 25, 21, 19, 17, 17 } },
            { 'O', { 14, 17, 17, 17, 17, 17, 14 } }, { 'P', { 30, 17, 17, 30, 16, 16, 16 } },
            { 'R', { 30, 17, 17, 30, 20, 18, 17 } }, { 'S', { 15, 16, 16, 14, 1, 1, 30 } },
            { 'U', { 17, 17, 17, 17, 17, 17, 14 } }, { 'V', { 17, 17, 17, 17, 17, 10, 4 } },
            { 'W', { 17, 17, 17, 21, 21, 21, 10 } }, { '+', { 0, 4, 4, 31, 4, 4, 0 } },
            { '-', { 0, 0, 0, 31, 0, 0, 0 } },       { '<', { 2, 4, 8, 16, 8, 4, 2 } },
            { '>', { 8, 4, 2, 1, 2, 4, 8 } },        { '^', { 4, 14, 21, 4, 4, 4, 4 } },        { 'v', { 4, 4, 4, 4, 21, 14, 4 } },
        };
        auto it = font.find(ch);
        return it == font.end() ? nullptr : it->second.data();
    }

    void quad(std::vector<Vtx>& out, float x0, float y0, float x1, float y1, float mode, const float* col)
    {
        const auto ndc = [](float x, float y, float& nx, float& ny) {
            nx = x / g_w * 2 - 1;
            ny = 1 - y / g_h * 2;
        };
        Vtx v[4];
        const float px[4] = { x0, x1, x1, x0 }, py[4] = { y0, y0, y1, y1 }, pu[4] = { -1, 1, 1, -1 }, pv[4] = { -1, -1, 1, 1 };
        for (int k = 0; k < 4; k++)
        {
            ndc(px[k], py[k], v[k].x, v[k].y);
            v[k].u = pu[k];
            v[k].v = pv[k];
            v[k].r = col[0];
            v[k].g = col[1];
            v[k].b = col[2];
            v[k].a = col[3];
            v[k].mode = mode;
        }
        for (int k : { 0, 1, 2, 0, 2, 3 })
            out.push_back(v[k]);
    }

    void disc(std::vector<Vtx>& out, float cx, float cy, float r, const float* col) { quad(out, cx - r, cy - r, cx + r, cy + r, 0, col); }

    void text(std::vector<Vtx>& out, const char* s, float cx, float cy, float px, const float* col)
    {
        const int n = (int)strlen(s);
        const float w = n * 6 * px - px;
        float x = cx - w / 2;
        const float y = cy - 3.5f * px;
        for (int i = 0; i < n; i++, x += 6 * px)
            if (const uint8_t* g = glyph(s[i]))
                for (int row = 0; row < 7; row++)
                    for (int c = 0; c < 5; c++)
                        if (g[row] & (16 >> c))
                            quad(out, x + c * px, y + row * px, x + (c + 1) * px, y + (row + 1) * px, 1, col);
    }

    GLuint g_prog, g_vao, g_vbo;

    void ensureGl()
    {
        if (g_prog)
            return;
        const char* vs = KP_GLSL_VERSION R"(
in vec2 pos; in vec2 uv; in vec4 col; in float mode;
out vec2 vUv; out vec4 vCol; out float vMode;
void main() { vUv = uv; vCol = col; vMode = mode; gl_Position = vec4(pos, 0.0, 1.0); })";
        const char* fs = KP_GLSL_VERSION R"(
in vec2 vUv; in vec4 vCol; in float vMode; out vec4 fragColor;
void main() {
  if (vMode > 0.5) { fragColor = vCol; return; }
  float d = length(vUv), aa = fwidth(d);
  float fill = 1.0 - smoothstep(1.0 - aa, 1.0, d);
  float ring = smoothstep(0.88 - aa, 0.88, d) * fill;
  fragColor = vec4(mix(vCol.rgb, vec3(1.0), ring), max(vCol.a * fill, ring * 0.55));
})";
        const auto compile = [](GLenum type, const char* src) {
            const GLuint sh = glCreateShader(type);
            glShaderSource(sh, 1, &src, nullptr);
            glCompileShader(sh);
            GLint ok = 0;
            glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
            if (!ok)
            {
                char log[1024];
                glGetShaderInfoLog(sh, sizeof log, nullptr, log);
                logf("[touch] shader: %s", log);
            }
            return sh;
        };
        g_prog = glCreateProgram();
        glAttachShader(g_prog, compile(GL_VERTEX_SHADER, vs));
        glAttachShader(g_prog, compile(GL_FRAGMENT_SHADER, fs));
        glBindAttribLocation(g_prog, 0, "pos");
        glBindAttribLocation(g_prog, 1, "uv");
        glBindAttribLocation(g_prog, 2, "col");
        glBindAttribLocation(g_prog, 3, "mode");
        glLinkProgram(g_prog);
        glGenVertexArrays(1, &g_vao);
        glGenBuffers(1, &g_vbo);
        glBindVertexArray(g_vao);
        glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
        const GLsizei stride = sizeof(Vtx);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(Vtx, x));
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(Vtx, u));
        glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(Vtx, r));
        glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(Vtx, mode));
        for (int k = 0; k < 4; k++)
            glEnableVertexAttribArray(k);
        glBindVertexArray(0);
    }
}

namespace touch
{
    bool enabled()
    {
#if defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
        static const bool on = !getenv("KP_TOUCH") || atoi(getenv("KP_TOUCH")) != 0;
#else
        static const bool on = getenv("KP_TOUCH") && atoi(getenv("KP_TOUCH")) != 0;
#endif
        return on;
    }

    void begin(intptr_t id, float x, float y)
    {
        if (!enabled())
            return;
        if (g_introPlaying) // a tap skips the intro movie (Escape)
        {
            input::key(0x01, true, 0x1B);
            input::key(0x01, false, 0x1B);
            return;
        }
        std::lock_guard lock(g_m);
        g_lastTouch = std::chrono::steady_clock::now();
        g_touches.erase(id); // a touch id reused while its last press is still held
        syncMode();
        Touch t;
        t.start = std::chrono::steady_clock::now();
        t.x0 = t.x = x;
        t.y0 = t.y = y;
        const auto buttons = activeButtons();
        float best = 1e9f;
        for (size_t i = 0; i < buttons.size(); i++)
        {
            float cx, cy;
            center(buttons[i], cx, cy);
            const float d = std::hypot(x - cx, y - cy) / (buttons[i].r * unit());
            if (d < 1.25f && d < best)
            {
                best = d;
                t.role = Role::Button;
                t.button = (int)i;
            }
        }
        if (t.role == Role::None && !g_menuMode)
            t.role = x < g_w * 0.45f ? Role::Stick : Role::Look;
        g_touches[id] = t;
        publish();
    }

    void move(intptr_t id, float x, float y)
    {
        std::lock_guard lock(g_m);
        auto it = g_touches.find(id);
        if (it == g_touches.end())
            return;
        Touch& t = it->second;
        if (t.role == Role::Look)
        {
            // A swipe across the screen's height is 1000 mouse counts: about 120 degrees at
            // sensitivity 5.5, 65 at the game's default 3.
            static const float speed = getenv("KP_TOUCH_LOOK") ? (float)atof(getenv("KP_TOUCH_LOOK")) : 1.0f;
            const float k = speed * 1000.0f / g_h;
            g_lookRemX += (x - t.x) * k;
            g_lookRemY += (y - t.y) * k;
            const int dx = (int)g_lookRemX, dy = (int)g_lookRemY;
            g_lookRemX -= dx;
            g_lookRemY -= dy;
            if (dx || dy)
                input::mouseDelta(dx, dy);
        }
        t.x = x;
        t.y = y;
        if (t.role == Role::Stick)
            publish();
    }

    void end(intptr_t id)
    {
        std::lock_guard lock(g_m);
        auto it = g_touches.find(id);
        if (it == g_touches.end())
            return;
        const auto now = std::chrono::steady_clock::now();
        if (it->second.role == Role::Button && now - it->second.start < kMinPress)
        {
            it->second.releaseAt = it->second.start + kMinPress; // draw() lets go
            return;
        }
        g_touches.erase(it);
        publish();
    }

    void scriptStart()
    {
        const char* e = getenv("KP_TOUCH_SCRIPT");
        if (!e || !enabled())
            return;
        struct Ev { double t; float x, y, x2, y2; double dur; };
        std::vector<Ev> evs;
        std::string spec = e;
        for (size_t pos = 0; pos < spec.size();)
        {
            size_t end = spec.find(',', pos);
            if (end == std::string::npos)
                end = spec.size();
            Ev ev{};
            if (sscanf(spec.substr(pos, end - pos).c_str(), "%lf:%f:%f:%f:%f:%lf", &ev.t, &ev.x, &ev.y, &ev.x2, &ev.y2, &ev.dur) == 6)
                evs.push_back(ev);
            pos = end + 1;
        }
        const auto t0 = std::chrono::steady_clock::now();
        intptr_t id = 1000;
        for (const Ev& ev : evs) // each touch on its own thread, so they can overlap
            std::thread([ev, t0, id = ++id] {
                std::this_thread::sleep_until(t0 + std::chrono::duration<double>(ev.t));
                logf("[touch] script: %.2f,%.2f -> %.2f,%.2f over %.2fs", ev.x, ev.y, ev.x2, ev.y2, ev.dur);
                const float w = (float)g_w, h = (float)g_h;
                begin(id, ev.x * w, ev.y * h);
                const int steps = std::max(1, (int)(ev.dur * 60));
                for (int k = 1; k <= steps; k++)
                {
                    std::this_thread::sleep_for(std::chrono::duration<double>(ev.dur / steps));
                    const float f = (float)k / steps;
                    move(id, (ev.x + (ev.x2 - ev.x) * f) * w, (ev.y + (ev.y2 - ev.y) * f) * h);
                }
                end(id);
            }).detach();
    }

    void draw(int fbW, int fbH)
    {
        if (!visible())
            return;
        std::vector<Vtx> v;
        {
            std::lock_guard lock(g_m);
            if (fbW != g_w || fbH != g_h)
            {
                g_w = std::max(1, fbW);
                g_h = std::max(1, fbH);
                g_touches.clear();
            }
            const bool wasMenu = g_menuMode;
            syncMode();
            bool changed = wasMenu != g_menuMode;
            const auto now = std::chrono::steady_clock::now();
            for (auto it = g_touches.begin(); it != g_touches.end();)
                if (it->second.releaseAt.time_since_epoch().count() && now >= it->second.releaseAt)
                {
                    it = g_touches.erase(it);
                    changed = true;
                }
                else
                    ++it;
            if (changed)
                publish();
            const auto buttons = activeButtons();
            const float u = unit();
            const float idle[4] = { 0.08f, 0.08f, 0.08f, 0.28f }, held[4] = { 0.55f, 0.45f, 0.25f, 0.55f },
                        label[4] = { 1, 1, 1, 0.75f };
            for (size_t i = 0; i < buttons.size(); i++)
            {
                bool pressed = false;
                for (const auto& [id, t] : g_touches)
                    pressed |= t.role == Role::Button && t.button == (int)i;
                float cx, cy;
                center(buttons[i], cx, cy);
                disc(v, cx, cy, buttons[i].r * u, pressed ? held : idle);
                const float n = (float)strlen(buttons[i].label);
                const float px = std::max(1.0f, std::floor(std::min(0.045f * u, 1.3f * buttons[i].r * u / (6 * n - 1))));
                text(v, buttons[i].label, cx, cy, px, label);
            }
            if (!g_menuMode)
            {
                bool stick = false;
                for (const auto& [id, t] : g_touches)
                    if (t.role == Role::Stick)
                    {
                        stick = true;
                        const float base[4] = { 0.1f, 0.1f, 0.1f, 0.3f }, knob[4] = { 0.6f, 0.6f, 0.6f, 0.5f };
                        disc(v, t.x0, t.y0, 1.2f * u, base);
                        const float range = 1.2f * u, dx = t.x - t.x0, dy = t.y - t.y0;
                        const float len = std::max(range, std::hypot(dx, dy));
                        disc(v, t.x0 + dx * range / len, t.y0 + dy * range / len, 0.5f * u, knob);
                    }
                if (!stick) // where the stick usually sits
                {
                    const float hint[4] = { 0.1f, 0.1f, 0.1f, 0.15f };
                    disc(v, 2.8f * u, g_h - 2.8f * u, 1.2f * u, hint);
                }
            }
        }
        ensureGl();
        glViewport(0, 0, fbW, fbH);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glDisable(GL_SCISSOR_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glUseProgram(g_prog);
        glBindVertexArray(g_vao);
        glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(v.size() * sizeof(Vtx)), v.data(), GL_STREAM_DRAW);
        glDrawArrays(GL_TRIANGLES, 0, (GLsizei)v.size());
        glBindVertexArray(0);
        glDisable(GL_BLEND);
    }
}
