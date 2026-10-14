// What each host OS provides to the portable runtime (macOS: platform/mac.mm, iOS:
// platform/ios.mm, Android: platform/android.cpp).
//
// The guest runs on its own thread; GL calls happen on it. Window and UI work is marshalled to the
// platform's UI thread by the implementation.
#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace platform
{
    // ---- window
    void showWindow(int w, int h, const char* title); // create or resize for a w x h video mode
    void hideWindow();
    void setFullscreen(bool on);           // the game's fullscreen request (desktop platforms)
    void screenSize(int& w, int& h);       // size of the native video mode
    void setMouseCaptured(bool captured);  // relative mouse, hidden cursor (while focused)

    // ---- GL (GL 3.2 core / GL ES 3.0), on the guest thread
    bool glCreate();                       // the game's context
    void glMakeCurrent(int renderW, int renderH);
    void glRelease();
    void glPresent();                      // show the window framebuffer
    void glWaitVblank();                   // pace one frame per display refresh
    uint32_t glWindowFramebuffer();        // framebuffer object of the window (0 on most platforms)
    void glDrawableSize(int& w, int& h);   // window framebuffer size in pixels
    void presentRgba(const uint32_t* pixels, int w, int h); // the intro's DirectDraw frames

    // ---- audio: a 44.1 kHz stereo float output pulling from render()
    void audioStart(void (*render)(float* out, uint32_t frames));

    // ---- gamepads: the platform reports controller state through pad::update()
    void gamepadStart();

    // ---- images
    bool decodeImage(const std::string& path, std::vector<uint8_t>& rgba, int& w, int& h);
    bool writePng(const std::string& path, const uint8_t* rgba, int w, int h); // rows top first
}

// Input from the platform (implemented by the portable user32 layer).
namespace input
{
    void key(uint32_t scancode, bool down, uint32_t vk); // PC set-1 scan code, | 0x100 = extended
    void character(uint32_t ch);
    void mouseDelta(int dx, int dy);
    void mouseAbsolute(double x01, double y01); // pointer position in the game view (uncaptured)
    void mouseButton(int button, bool down);    // 0 left, 1 right, 2 middle
    void wheel(int dir);
    void activate(bool active);
    void close();
}

// Gamepad state from the platform (implemented by win32/gamepad.cpp).
namespace pad
{
    struct State
    {
        float lx = 0, ly = 0, rx = 0, ry = 0, lt = 0, rt = 0; // sticks -1..1 (y up = +1), triggers 0..1
        uint32_t buttons = 0; // A B X Y LB RB LT RT L3 R3 Back (bit 0..10)
        bool up = false, down = false, left = false, right = false, start = false;
        bool connected = false;
    };
    void update(const State& s); // a game controller
    void disconnected();
    void touch(const State& s);  // the on-screen touch controls
    bool controllerConnected();
    std::chrono::steady_clock::time_point lastUsed(); // last input from a controller
    int keyDest(); // the game's input focus: 0 playing, 1 console, 2 chat, 3 menu
}

// On-screen touch controls (runtime/touch_controls.cpp). Positions in window framebuffer pixels,
// origin top left. Called on the platform's UI thread; draw() on the guest thread.
namespace touch
{
    bool enabled(); // touch platforms, or KP_TOUCH=1 (mouse as a finger) on the Mac
    void begin(intptr_t id, float x, float y);
    void move(intptr_t id, float x, float y);
    void end(intptr_t id);
    void draw(int fbW, int fbH); // into the bound window framebuffer
    void scriptStart();          // KP_TOUCH_SCRIPT (tests)
}

// Frame profile (KP_FRAMETIMES): milliseconds per section, summed until the next report.
enum ProfSection { kProfEffects, kProfFinal, kProfSwap, kProfVblank, kProfDraw, kProfCount };
extern double g_profMs[kProfCount];
struct ProfScope
{
    ProfSection s;
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    explicit ProfScope(ProfSection sec) : s(sec) {}
    ~ProfScope() { g_profMs[s] += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); }
};

// Frame hook (app.cpp): frame dumps, statistics; called with the finished frame bound.
void frameHook();
// Brightness (user32.cpp)
void gammaAdjust(float delta);
void glPostToggleEffects();
