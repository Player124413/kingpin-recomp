// Gamepads for the game's own joystick support (winmm joyGetPosEx: Quake 2's analog joystick code with joy_advanced axes).
//
// Joystick layout seen by the game:
//   X/Y = left stick, Z/R = right stick, U/V = left/right trigger
//   buttons: A=JOY1 B=JOY2 X=JOY3 Y=JOY4 LB=AUX5 RB=AUX6 LT=AUX7 RT=AUX8 L3=AUX9 R3=AUX10 Back=AUX11
//   (Quake 2 names button i >= 4 K_AUX1 + i; main/config.cfg binds them)
// While a note, the notebook or the inventory is up, B puts it away (Escape) instead of crouching.
//   D-pad = POV hat (AUX29-32)
// Start always sends Escape (menu). While a menu, the console or chat is up (client key_dest != 0)
// the pad drives the keyboard instead (stick/D-pad = arrows, A = Enter, B = Escape) and the game
// sees a centred joystick.
// The platform reports controller state with pad::update() (Apple: platform/gc_gamepad.mm).
// KP_PAD="t:control:value:duration,..." injects a fake pad for tests (controls: a b x y lb rb lt rt
// l3 r3 back start up down left right lx ly rx ry; values -1..1).
#include <pthread.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../platform/platform.h"
#include "hle.h"

void postMessageHost(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp);

namespace
{
    using Pad = pad::State;
    std::mutex g_padMutex;
    Pad g_real, g_fake;
    bool g_fakeActive = false;
    std::chrono::steady_clock::time_point g_lastUse; // last real controller input
    Pad g_touch; // on-screen touch controls (runtime/touch_controls.cpp), combined with the controller

    // Digital controls as bits: buttons 0-10, then up down left right start.
    uint32_t digital(const Pad& p)
    {
        return p.buttons | (uint32_t)p.up << 11 | (uint32_t)p.down << 12 | (uint32_t)p.left << 13 |
               (uint32_t)p.right << 14 | (uint32_t)p.start << 15;
    }
    // A press shorter than the polling interval (8 ms menu thread, one game frame) still counts:
    // each press is reported for at least kMinPress.
    constexpr auto kMinPress = std::chrono::milliseconds(50);
    std::chrono::steady_clock::time_point g_pressedAt[16];

    Pad current()
    {
        std::lock_guard lock(g_padMutex);
        Pad p = g_fakeActive ? g_fake : g_real;
        const auto now = std::chrono::steady_clock::now();
        uint32_t held = 0;
        for (int b = 0; b < 16; b++)
            if (now - g_pressedAt[b] < kMinPress)
                held |= 1u << b;
        p.buttons |= held & 0x7FF;
        p.up |= (held >> 11) & 1;
        p.down |= (held >> 12) & 1;
        p.left |= (held >> 13) & 1;
        p.right |= (held >> 14) & 1;
        p.start |= (held >> 15) & 1;
        const Pad& t = g_touch;
        p.lx = std::clamp(p.lx + t.lx, -1.0f, 1.0f);
        p.ly = std::clamp(p.ly + t.ly, -1.0f, 1.0f);
        p.rx = std::clamp(p.rx + t.rx, -1.0f, 1.0f);
        p.ry = std::clamp(p.ry + t.ry, -1.0f, 1.0f);
        p.lt = std::max(p.lt, t.lt);
        p.rt = std::max(p.rt, t.rt);
        p.buttons |= t.buttons;
        p.up |= t.up;
        p.down |= t.down;
        p.left |= t.left;
        p.right |= t.right;
        p.start |= t.start;
        return p;
    }

    uint32_t keyDest() { return rd32(0x0125DD04); } // kingpin.exe cls.key_dest: 0 game, 1 console, 2 chat, 3 menu
    // kingpin.exe cl.frame.playerstate.stats[STAT_LAYOUTS]: a note, the notebook or the inventory is
    // shown (Escape then sends "cmd putaway").
    bool layoutShown() { return rd16(0x0133325A) != 0; }
    // The Pawn-O-Matic shop (maps pawn_*: gamex86.dll level.mapname). Its menu is driven by the
    // arrow keys (the client sends "cmd uparrow" etc.), Enter (select / exit), Y (yes, buy) and
    // X (no); the pad maps onto those there.
    bool inShop()
    {
        const char* map = reinterpret_cast<const char*>(g_mem + 0x200F67C8);
        return keyDest() == 0 && strncmp(map, "pawn_", 5) == 0;
    }

    void key(uint32_t sc, uint32_t vk, bool down)
    {
        const uint32_t lp = 1 | (sc & 0xFF) << 16 | ((sc & 0x100) ? 1u << 24 : 0) | (down ? 0 : 3u << 30);
        postMessageHost(0, down ? 0x100 : 0x101, vk, lp);
    }

    // Menu/console mode: pad -> keyboard, with auto-repeat for directions.
    void menuThread()
    {
#ifdef __APPLE__
        pthread_setname_np("gamepad");
#else
        pthread_setname_np(pthread_self(), "gamepad");
#endif
        struct Dir
        {
            uint32_t sc, vk;
            bool held = false;
            std::chrono::steady_clock::time_point next;
        } dirs[4] = { { 0x148, 0x26 }, { 0x150, 0x28 }, { 0x14B, 0x25 }, { 0x14D, 0x27 } };
        bool aHeld = false, bHeld = false, xHeld = false, yHeld = false, startHeld = false;
        while (true)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
            const Pad p = current();
            const auto now = std::chrono::steady_clock::now();
            if (p.start != startHeld)
            {
                startHeld = p.start;
                key(0x01, 0x1B, p.start); // Escape
            }
            const bool shop = inShop();
            const bool menu = keyDest() != 0 || shop;
            const bool want[4] = { menu && (p.up || p.ly > 0.6f), menu && (p.down || p.ly < -0.6f),
                                   menu && (p.left || p.lx < -0.6f), menu && (p.right || p.lx > 0.6f) };
            for (int k = 0; k < 4; k++)
            {
                Dir& d = dirs[k];
                if (want[k] && (!d.held || now >= d.next))
                {
                    key(d.sc, d.vk, true);
                    key(d.sc, d.vk, false);
                    // (the shop ignores presses closer than 0.2 s)
                    d.next = now + std::chrono::milliseconds(d.held ? (shop ? 250 : 120) : 400);
                    d.held = true;
                }
                else if (!want[k])
                    d.held = false;
            }
            const bool a = menu && (p.buttons & 1), b = (menu || layoutShown()) && (p.buttons & 2);
            if (a != aHeld)
            {
                aHeld = a;
                key(0x1C, 0x0D, a); // Enter
            }
            if (b != bHeld)
            {
                bHeld = b;
                if (shop)
                    key(0x14B, 0x25, b); // Left: back to the categories (cancels a pending deal)
                else
                    key(0x01, 0x1B, b); // Escape
            }
            const bool x = shop && (p.buttons & 4), y = shop && (p.buttons & 8);
            if (x != xHeld)
            {
                xHeld = x;
                key(0x2D, 'X', x); // key3: "no deal"
            }
            if (y != yHeld)
            {
                yHeld = y;
                key(0x15, 'Y', y); // key1: "deal" (buy)
            }
        }
    }

    void fakeScript(std::string script)
    {
        struct Ev { double t; std::string control; float value; double dur; };
        std::vector<Ev> evs;
        size_t pos = 0;
        while (pos < script.size())
        {
            size_t e = script.find(',', pos);
            if (e == std::string::npos) e = script.size();
            std::string it = script.substr(pos, e - pos);
            pos = e + 1;
            char control[32] = {};
            double t = 0, dur = 0.2;
            float v = 1;
            if (sscanf(it.c_str(), "%lf:%31[^:]:%f:%lf", &t, control, &v, &dur) >= 2)
                evs.push_back({ t, control, v, dur });
        }
        {
            std::lock_guard lock(g_padMutex);
            g_fakeActive = true;
            g_fake.connected = true;
        }
        std::thread([evs] {
            const auto t0 = std::chrono::steady_clock::now();
            static const char* buttons[] = { "a", "b", "x", "y", "lb", "rb", "lt", "rt", "l3", "r3", "back" };
            for (const Ev& ev : evs)
            {
                std::this_thread::sleep_until(t0 + std::chrono::duration<double>(ev.t));
                logf("[pad] fake %s = %.2f for %.2fs", ev.control.c_str(), ev.value, ev.dur);
                auto set = [&](bool on) {
                    std::lock_guard lock(g_padMutex);
                    Pad& p = g_fake;
                    const float v = on ? ev.value : 0.0f;
                    const std::string& c = ev.control;
                    if (c == "lx") p.lx = v;
                    else if (c == "ly") p.ly = v;
                    else if (c == "rx") p.rx = v;
                    else if (c == "ry") p.ry = v;
                    else if (c == "up") p.up = on;
                    else if (c == "down") p.down = on;
                    else if (c == "left") p.left = on;
                    else if (c == "right") p.right = on;
                    else if (c == "start") p.start = on;
                    for (int k = 0; k < 11; k++)
                        if (c == buttons[k])
                        {
                            if (on) p.buttons |= 1u << k;
                            else p.buttons &= ~(1u << k);
                            if (k == 6) p.lt = on ? 1.0f : 0.0f;
                            if (k == 7) p.rt = on ? 1.0f : 0.0f;
                        }
                };
                set(true);
                std::this_thread::sleep_for(std::chrono::duration<double>(ev.dur));
                set(false);
            }
        }).detach();
    }
}

void gamepadInit()
{
    if (const char* s = getenv("KP_PAD"))
        fakeScript(s);
    platform::gamepadStart();
    std::thread(menuThread).detach();
}

namespace pad
{
    void update(const State& s)
    {
        std::lock_guard lock(g_padMutex);
        const uint32_t pressed = digital(s) & ~digital(g_real);
        const auto now = std::chrono::steady_clock::now();
        for (int b = 0; b < 16; b++)
            if (pressed & (1u << b))
                g_pressedAt[b] = now;
        g_real = s;
        g_real.connected = true;
        const bool used = s.buttons || s.up || s.down || s.left || s.right || s.start || std::fabs(s.lx) > 0.3f ||
                          std::fabs(s.ly) > 0.3f || std::fabs(s.rx) > 0.3f || std::fabs(s.ry) > 0.3f;
        if (used)
            g_lastUse = std::chrono::steady_clock::now();
    }
    std::chrono::steady_clock::time_point lastUsed()
    {
        std::lock_guard lock(g_padMutex);
        return g_lastUse;
    }
    void touch(const State& s)
    {
        std::lock_guard lock(g_padMutex);
        g_touch = s;
    }
    bool controllerConnected()
    {
        std::lock_guard lock(g_padMutex);
        return g_real.connected;
    }
    int keyDest() { return (int)::keyDest(); }
    bool inShop() { return ::inShop(); }
    void disconnected()
    {
        std::lock_guard lock(g_padMutex);
        g_real = State{};
    }
}

// ---------------------------------------------------------------- winmm joystick API

namespace
{
    uint32_t axis(float v) { return (uint32_t)std::clamp(32768.0f + v * 32767.0f, 0.0f, 65535.0f); }
}

// One joystick that is always present (centred when no controller is connected), so a
// controller plugged in after start-up works: the game only looks for joysticks at start-up.
WIN32API(winmm, joyGetNumDevs, 0) { RET(1); }

WIN32API(winmm, joyGetDevCapsA, 3)
{
    if (ARG(0) != 0)
        RET(165); // JOYERR_PARMS
    const uint32_t p = ARG(1);
    memset(g_mem + p, 0, std::min<uint32_t>(ARG(2), 404));
    wr16(p, 0x045E);
    wr16(p + 2, 0x028E);
    strcpy(gstr(p + 4), "Gamepad");
    for (uint32_t off : { 40u, 48u, 56u, 76u, 84u, 92u }) // axis maxima (minima are 0)
        wr32(p + off, 65535);
    wr32(p + 60, 11);                         // buttons
    wr32(p + 96, 0x01 | 0x02 | 0x04 | 0x08 | 0x10 | 0x20); // HASZ HASR HASU HASV HASPOV POV4DIR
    wr32(p + 100, 6);
    wr32(p + 104, 6);
    wr32(p + 108, 32);
    RET(0);
}

WIN32API(winmm, joyGetPosEx, 2)
{
    if (ARG(0) != 0)
        RET(167); // JOYERR_UNPLUGGED
    Pad p = current();
    if (keyDest() != 0 || inShop()) // menus, console, the shop: the pad drives the keyboard instead
        p = Pad{};
    if (layoutShown()) // B puts the note away (menu thread) instead of crouching
        p.buttons &= ~2u;
    const uint32_t j = ARG(1);
    wr32(j + 8, axis(p.lx));
    wr32(j + 12, axis(-p.ly)); // Windows joystick Y grows downwards
    wr32(j + 16, axis(p.rx));
    wr32(j + 20, axis(-p.ry));
    wr32(j + 24, axis(p.lt * 2 - 1));
    wr32(j + 28, axis(p.rt * 2 - 1));
    wr32(j + 32, p.buttons);
    wr32(j + 36, (uint32_t)__builtin_popcount(p.buttons));
    uint32_t pov = 0xFFFF; // JOY_POVCENTERED
    if (p.up) pov = 0;
    else if (p.right) pov = 9000;
    else if (p.down) pov = 18000;
    else if (p.left) pov = 27000;
    wr32(j + 40, pov);
    RET(0);
}
