// user32 / gdi32 / wgl: windows, messages, input and the GL context, on the platform layer.
//
// Input events become Win32 messages in a queue the game pumps with PeekMessage/DispatchMessage.
// Keys carry PC set-1 scan codes in lParam (Quake 2's MapKey reads those). The mouse cursor seen
// by the game is virtual: raw motion moves it and SetCursorPos re-centres it, which is how Quake 2's
// mouse look works; while the game clips the cursor the platform captures the real mouse.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>

#include "../gfx/glemu.h"
#include "../platform/platform.h"
#include "hle.h"

void glPostInit(int w, int h);
void glPostPresent(float gamma, int viewW, int viewH, void (*whileWindowBound)());
void glPostAfterSwap();
void glTexturesEndFrame();
void hostExit(int code);
void winmmPoll();

namespace
{
    struct Msg
    {
        uint32_t hwnd, message, wParam, lParam;
    };

    std::mutex g_qMutex;
    std::condition_variable g_qCv;
    std::deque<Msg> g_queue;

    struct Window
    {
        uint32_t wndproc = 0;
        int x = 0, y = 0, w = 640, h = 480;
        bool shown = false;
        std::map<int32_t, uint32_t> longs;
    };
    std::mutex g_winMutex;
    std::map<uint32_t, Window> g_windows;
    std::map<std::string, uint32_t> g_classes; // class name -> wndproc
    uint32_t g_nextHwnd = 0x00010010;
    uint32_t g_mainHwnd = 0;

    std::atomic<int> g_cursorX{ 320 }, g_cursorY{ 240 };
    std::atomic<bool> g_clipped{ false };
    int g_showCount = 0;
    uint32_t g_buttons = 0;
    std::atomic<bool> g_wantFullscreen{ false };

    void post(uint32_t message, uint32_t wParam, uint32_t lParam)
    {
        std::lock_guard lock(g_qMutex);
        g_queue.push_back({ g_mainHwnd, message, wParam, lParam });
        g_qCv.notify_all();
    }

    uint32_t cursorLParam() { return (uint32_t)(g_cursorY << 16 | (g_cursorX & 0xFFFF)); }

    // Guest window procedure call for a message.
    uint32_t sendToGuest(Cpu& c, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp)
    {
        uint32_t proc;
        {
            std::lock_guard lock(g_winMutex);
            auto it = g_windows.find(hwnd);
            if (it == g_windows.end() || !it->second.wndproc)
                return 0;
            proc = it->second.wndproc;
        }
        return guestCall(c, proc, { hwnd, msg, wp, lp });
    }

    void applyFullscreen()
    {
        static const bool forceWindowed = getenv("KP_WINDOWED") && atoi(getenv("KP_WINDOWED")) != 0;
        platform::setFullscreen(g_wantFullscreen && !forceWindowed);
    }

    void showMainWindow(int w, int h, const char* title)
    {
        platform::showWindow(w, h, title);
        applyFullscreen();
    }
}

// ---------------------------------------------------------------- input from the platform

namespace input
{
    void key(uint32_t sc, bool down, uint32_t vk)
    {
        if (!sc)
            return;
        const bool alt = (sc & 0xFF) == 0x38;
        uint32_t lParam = 1 | (sc & 0xFF) << 16 | ((sc & 0x100) ? 1u << 24 : 0);
        if (!down)
            lParam |= 3u << 30;
        post(alt ? (down ? 0x104 : 0x105) : (down ? 0x100 : 0x101), vk, lParam);
    }
    void character(uint32_t ch) { post(0x0102, ch, 1); } // WM_CHAR
    void mouseDelta(int dx, int dy)
    {
        g_cursorX += dx;
        g_cursorY += dy;
        post(0x200, g_buttons, cursorLParam());
    }
    void mouseAbsolute(double x01, double y01)
    {
        if (g_clipped)
            return; // captured: only relative motion counts
        Window w;
        {
            std::lock_guard lock(g_winMutex);
            w = g_windows[g_mainHwnd];
        }
        g_cursorX = w.x + (int)(x01 * w.w);
        g_cursorY = w.y + (int)(y01 * w.h);
        post(0x200, g_buttons, cursorLParam());
    }
    void mouseButton(int button, bool down)
    {
        static const uint32_t bits[3] = { 1, 2, 0x10 };
        static const uint32_t downMsg[3] = { 0x201, 0x204, 0x207 };
        if (button < 0 || button > 2)
            return;
        if (down)
            g_buttons |= bits[button];
        else
            g_buttons &= ~bits[button];
        post(downMsg[button] + (down ? 0 : 1), g_buttons, cursorLParam());
    }
    void wheel(int dir) { post(0x020A, (uint32_t)((int16_t)(dir > 0 ? 120 : -120)) << 16, 0); }
    void activate(bool active)
    {
        if (active)
            post(0x001C, 1, 0); // WM_ACTIVATEAPP
        post(0x0006, active ? 1 : 0, 0); // WM_ACTIVATE
    }
    void close() { post(0x0010, 0, 0); } // WM_CLOSE
}

// Relative mouse motion from a script (same path as real motion).
void injectMouseDelta(int dx, int dy) { input::mouseDelta(dx, dy); }

// ---------------------------------------------------------------- user32: windows

WIN32API(user32, RegisterClassA, 1)
{
    const uint32_t wc = ARG(0);
    const std::string name = gstr(rd32(wc + 36));
    std::lock_guard lock(g_winMutex);
    g_classes[name] = rd32(wc + 4);
    RET(0xC000 + g_classes.size());
}
WIN32API(user32, UnregisterClassA, 2) { RET(1); }

WIN32API(user32, CreateWindowExA, 12)
{
    const std::string cls = ARG(1) > 0xFFFF ? gstr(ARG(1)) : "";
    const char* title = ARG(2) ? gstr(ARG(2)) : "";
    const int w = (int)ARG(6), h = (int)ARG(7);
    uint32_t hwnd;
    {
        std::lock_guard lock(g_winMutex);
        hwnd = g_nextHwnd += 0x10;
        Window win;
        win.wndproc = g_classes.count(cls) ? g_classes[cls] : 0;
        win.x = (int)ARG(4) == (int)0x80000000 ? 0 : (int)ARG(4);
        win.y = (int)ARG(5) == (int)0x80000000 ? 0 : (int)ARG(5);
        win.w = w > 0 ? w : 640;
        win.h = h > 0 ? h : 480;
        g_windows[hwnd] = win;
        g_mainHwnd = hwnd;
        g_cursorX = win.x + win.w / 2;
        g_cursorY = win.y + win.h / 2;
    }
    logf("[user32] CreateWindowEx(%s, \"%s\", %dx%d at %d,%d)", cls.c_str(), title, w, h, (int)ARG(4), (int)ARG(5));
    showMainWindow(w > 0 ? w : 640, h > 0 ? h : 480, title);
    const uint32_t cs = heapAlloc(48, true);
    wr32(cs + 0, ARG(11));
    wr32(cs + 4, ARG(10));
    wr32(cs + 24, (uint32_t)h);
    wr32(cs + 28, (uint32_t)w);
    sendToGuest(c, hwnd, 0x0001, 0, cs); // WM_CREATE
    heapFree(cs);
    RET(hwnd);
}

WIN32API(user32, DestroyWindow, 1)
{
    const uint32_t hwnd = ARG(0);
    sendToGuest(c, hwnd, 0x0002, 0, 0); // WM_DESTROY
    bool main;
    {
        std::lock_guard lock(g_winMutex);
        g_windows.erase(hwnd);
        main = hwnd == g_mainHwnd;
    }
    if (main)
        platform::hideWindow();
    RET(1);
}

WIN32API(user32, ShowWindow, 2)
{
    // Like Windows, only a change of state sends messages (the game's WM_ACTIVATE handler calls
    // ShowWindow(SW_RESTORE) itself).
    const uint32_t cmd = ARG(1);
    const bool show = !(cmd == 0 || cmd == 2 || cmd == 6 || cmd == 7); // HIDE, SHOWMINIMIZED, MINIMIZE, SHOWMINNOACTIVE
    Window w;
    bool was;
    {
        std::lock_guard lock(g_winMutex);
        auto& win = g_windows[ARG(0)];
        was = win.shown;
        win.shown = show;
        w = win;
    }
    if (show && !was)
    {
        sendToGuest(c, ARG(0), 0x0003, 0, (uint32_t)(w.y << 16 | (w.x & 0xFFFF))); // WM_MOVE
        sendToGuest(c, ARG(0), 0x001C, 1, 0);                                        // WM_ACTIVATEAPP
        sendToGuest(c, ARG(0), 0x0006, 1, 0);                                        // WM_ACTIVATE
    }
    RET(was ? 1 : 0);
}
WIN32API(user32, UpdateWindow, 1) { RET(1); }
WIN32API(user32, SetForegroundWindow, 1) { RET(1); }
WIN32API(user32, SetFocus, 1) { RET(g_mainHwnd); }
WIN32API(user32, GetForegroundWindow, 0) { RET(g_mainHwnd); }
WIN32API(user32, GetActiveWindow, 0) { RET(g_mainHwnd); }
WIN32API(user32, LoadCursorA, 2) { RET(0x00020001); }
WIN32API(user32, LoadIconA, 2) { RET(0x00020002); }
WIN32API(user32, SetCursor, 1) { RET(0); }

WIN32API(user32, MoveWindow, 6)
{
    std::lock_guard lock(g_winMutex);
    auto& w = g_windows[ARG(0)];
    w.x = (int)ARG(1);
    w.y = (int)ARG(2);
    RET(1);
}

WIN32API(user32, GetWindowRect, 2)
{
    Window w;
    {
        std::lock_guard lock(g_winMutex);
        w = g_windows[ARG(0)];
    }
    const uint32_t r = ARG(1);
    wr32(r, (uint32_t)w.x);
    wr32(r + 4, (uint32_t)w.y);
    wr32(r + 8, (uint32_t)(w.x + w.w));
    wr32(r + 12, (uint32_t)(w.y + w.h));
    RET(1);
}
WIN32API(user32, GetClientRect, 2)
{
    Window w;
    {
        std::lock_guard lock(g_winMutex);
        w = g_windows[ARG(0)];
    }
    const uint32_t r = ARG(1);
    wr32(r, 0);
    wr32(r + 4, 0);
    wr32(r + 8, (uint32_t)w.w);
    wr32(r + 12, (uint32_t)w.h);
    RET(1);
}
WIN32API(user32, AdjustWindowRect, 3) { RET(1); } // windows have no frame in the guest's view
WIN32API(user32, AdjustWindowRectEx, 4) { RET(1); }

WIN32API(user32, SetWindowLongA, 3)
{
    std::lock_guard lock(g_winMutex);
    auto& w = g_windows[ARG(0)];
    const int32_t idx = (int32_t)ARG(1);
    uint32_t old = w.longs[idx];
    if (idx == -4) // GWL_WNDPROC
    {
        old = w.wndproc;
        w.wndproc = ARG(2);
    }
    w.longs[idx] = ARG(2);
    RET(old);
}
WIN32API(user32, GetWindowLongA, 2)
{
    std::lock_guard lock(g_winMutex);
    auto& w = g_windows[ARG(0)];
    const int32_t idx = (int32_t)ARG(1);
    RET(idx == -4 ? w.wndproc : w.longs[idx]);
}

WIN32API(user32, DefWindowProcA, 4)
{
    if (ARG(1) == 0x0010) // WM_CLOSE
    {
        logf("[user32] WM_CLOSE reached DefWindowProc: exiting");
        hostExit(0);
    }
    RET(0);
}

WIN32API(user32, CallWindowProcA, 5) { RET(guestCall(c, ARG(0), { ARG(1), ARG(2), ARG(3), ARG(4) })); }
WIN32API(user32, SendMessageA, 4) { RET(sendToGuest(c, ARG(0), ARG(1), ARG(2), ARG(3))); }
WIN32API(user32, PostMessageA, 4)
{
    std::lock_guard lock(g_qMutex);
    g_queue.push_back({ ARG(0), ARG(1), ARG(2), ARG(3) });
    g_qCv.notify_all();
    RET(1);
}

WIN32API(user32, RegisterWindowMessageA, 1)
{
    static std::map<std::string, uint32_t> msgs;
    std::lock_guard lock(g_winMutex);
    auto& v = msgs[gstr(ARG(0))];
    if (!v)
        v = 0xC100 + (uint32_t)msgs.size();
    RET(v);
}

// ---------------------------------------------------------------- user32: messages

// Forgets the previous program's windows and messages.
void user32Reset()
{
    {
        std::lock_guard lock(g_qMutex);
        g_queue.clear();
    }
    std::lock_guard lock(g_winMutex);
    g_windows.clear();
    g_classes.clear();
    g_mainHwnd = 0;
}

void postMessageHost(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp)
{
    std::lock_guard lock(g_qMutex);
    g_queue.push_back({ hwnd ? hwnd : g_mainHwnd, msg, wp, lp });
    g_qCv.notify_all();
}

static void writeMsg(uint32_t p, const Msg& m)
{
    wr32(p, m.hwnd);
    wr32(p + 4, m.message);
    wr32(p + 8, m.wParam);
    wr32(p + 12, m.lParam);
    wr32(p + 16, 0);
    wr32(p + 20, (uint32_t)g_cursorX);
    wr32(p + 24, (uint32_t)g_cursorY);
}

WIN32API(user32, PeekMessageA, 5)
{
    winmmPoll();
    std::lock_guard lock(g_qMutex);
    if (g_queue.empty())
        RET(0);
    writeMsg(ARG(0), g_queue.front());
    if (ARG(4) & 1) // PM_REMOVE
        g_queue.pop_front();
    RET(1);
}

WIN32API(user32, GetMessageA, 4)
{
    std::unique_lock lock(g_qMutex);
    g_qCv.wait(lock, [] { return !g_queue.empty(); });
    const Msg m = g_queue.front();
    g_queue.pop_front();
    writeMsg(ARG(0), m);
    RET(m.message == 0x0012 ? 0 : 1); // WM_QUIT
}

WIN32API(user32, TranslateMessage, 1) { RET(0); }
WIN32API(user32, DispatchMessageA, 1)
{
    const uint32_t p = ARG(0);
    RET(sendToGuest(c, rd32(p), rd32(p + 4), rd32(p + 8), rd32(p + 12)));
}
WIN32API(user32, PostQuitMessage, 1)
{
    post(0x0012, ARG(0), 0);
    RET(0);
}

// ---------------------------------------------------------------- user32: cursor and misc

WIN32API(user32, GetCursorPos, 1)
{
    wr32(ARG(0), (uint32_t)g_cursorX.load());
    wr32(ARG(0) + 4, (uint32_t)g_cursorY.load());
    RET(1);
}
WIN32API(user32, SetCursorPos, 2)
{
    g_cursorX = (int)ARG(0);
    g_cursorY = (int)ARG(1);
    RET(1);
}
WIN32API(user32, ShowCursor, 1)
{
    g_showCount += ARG(0) ? 1 : -1;
    RET((uint32_t)g_showCount);
}
WIN32API(user32, ClipCursor, 1)
{
    g_clipped = ARG(0) != 0;
    platform::setMouseCaptured(g_clipped);
    RET(1);
}
WIN32API(user32, SetCapture, 1) { RET(0); }
WIN32API(user32, ReleaseCapture, 0) { RET(1); }

WIN32API(user32, GetSystemMetrics, 1)
{
    int w, h;
    platform::screenSize(w, h);
    switch (ARG(0))
    {
    case 0: RET(w); // SM_CXSCREEN
    case 1: RET(h);
    case 19: RET(1); // SM_MOUSEPRESENT
    case 43: RET(3); // SM_CMOUSEBUTTONS
    case 75: RET(1); // SM_MOUSEWHEELPRESENT
    default: RET(0);
    }
}

WIN32API(user32, SystemParametersInfoA, 4)
{
    if (ARG(0) == 3 && ARG(2)) // SPI_GETMOUSE
    {
        wr32(ARG(2), 0);
        wr32(ARG(2) + 4, 0);
        wr32(ARG(2) + 8, 0);
    }
    RET(1);
}

WIN32API(user32, MessageBoxA, 4)
{
    logf("[user32] MessageBox \"%s\": %s", ARG(2) ? gstr(ARG(2)) : "", ARG(1) ? gstr(ARG(1)) : "");
    RET(1); // IDOK
}

WIN32API(user32, RegisterHotKey, 4) { RET(1); }
WIN32API(user32, UnregisterHotKey, 2) { RET(1); }
WIN32API(user32, OpenClipboard, 1) { RET(0); }
WIN32API(user32, CloseClipboard, 0) { RET(1); }
WIN32API(user32, GetClipboardData, 1) { RET(0); }
WIN32API(user32, GetKeyState, 1) { RET(0); }
WIN32API(user32, GetAsyncKeyState, 1) { RET(0); }
WIN32API(user32, ClientToScreen, 2) { RET(1); } // windows sit at the origin of the guest's screen
WIN32API(user32, ScreenToClient, 2) { RET(1); }
WIN32API(user32, OffsetRect, 3)
{
    const uint32_t r = ARG(0);
    wr32(r, rd32(r) + ARG(1));
    wr32(r + 4, rd32(r + 4) + ARG(2));
    wr32(r + 8, rd32(r + 8) + ARG(1));
    wr32(r + 12, rd32(r + 12) + ARG(2));
    RET(1);
}

WIN32API(user32, ChangeDisplaySettingsA, 2)
{
    // A mode (with CDS_FULLSCREEN) means the game wants fullscreen; NULL restores the desktop.
    if (ARG(0))
        logf("[user32] ChangeDisplaySettings %ux%u: fullscreen", rd32(ARG(0) + 108), rd32(ARG(0) + 112));
    g_wantFullscreen = ARG(0) != 0;
    RET(0); // DISP_CHANGE_SUCCESSFUL
}
WIN32API(user32, EnumDisplaySettingsA, 3) { RET(0); }

WIN32API(user32, GetDC, 1) { RET(0xDC000000u | ARG(0)); }
WIN32API(user32, ReleaseDC, 2) { RET(1); }

// DirectDraw (the intro): exclusive mode and display mode changes.
void requestFullscreen(bool on)
{
    g_wantFullscreen = on;
    applyFullscreen();
}
void setDisplayModeWindow(int w, int h)
{
    {
        std::lock_guard lock(g_winMutex);
        Window& win = g_windows[g_mainHwnd];
        win.w = w;
        win.h = h;
    }
    showMainWindow(w, h, "Kingpin");
}
void presentRgba(const uint32_t* pixels, int w, int h) { platform::presentRgba(pixels, w, h); }

// ---------------------------------------------------------------- gdi32 / pixel formats

static void describePixelFormat(uint32_t p)
{
    memset(g_mem + p, 0, 40);
    wr16(p, 40);
    wr16(p + 2, 1);
    wr32(p + 4, 0x25); // DRAW_TO_WINDOW | SUPPORT_OPENGL | DOUBLEBUFFER
    wr8(p + 9, 32);
    wr8(p + 10, 8);
    wr8(p + 11, 16);
    wr8(p + 12, 8);
    wr8(p + 13, 8);
    wr8(p + 14, 8);
    wr8(p + 16, 8);
    wr8(p + 17, 24);
    wr8(p + 23, 24); // depth
    wr8(p + 24, 8);  // stencil
}

WIN32API(gdi32, ChoosePixelFormat, 2) { RET(1); }
WIN32API(gdi32, SetPixelFormat, 3) { RET(1); }
WIN32API(gdi32, GetPixelFormat, 1) { RET(1); }
WIN32API(gdi32, DescribePixelFormat, 4)
{
    if (ARG(3))
        describePixelFormat(ARG(3));
    RET(1);
}
WIN32API(gdi32, GetDeviceCaps, 2)
{
    int w, h;
    platform::screenSize(w, h);
    switch (ARG(1))
    {
    case 8: RET(w);
    case 10: RET(h);
    case 12: RET(32);
    case 116: RET(60);
    default: RET(0);
    }
}
WIN32API(gdi32, SetDeviceGammaRamp, 2) { RET(1); }
WIN32API(gdi32, GetDeviceGammaRamp, 2)
{
    for (int ch = 0; ch < 3; ch++)
        for (int i = 0; i < 256; i++)
            wr16(ARG(1) + 2 * (ch * 256 + i), (uint16_t)(i * 257));
    RET(1);
}
WIN32API(gdi32, GetSystemPaletteEntries, 4)
{
    // A grey ramp stands in for the Windows system palette.
    for (uint32_t i = 0; i < ARG(2); i++)
    {
        const uint8_t v = (uint8_t)(ARG(1) + i);
        wr8(ARG(3) + 4 * i, v);
        wr8(ARG(3) + 4 * i + 1, v);
        wr8(ARG(3) + 4 * i + 2, v);
        wr8(ARG(3) + 4 * i + 3, 0);
    }
    RET(ARG(2));
}

// ---------------------------------------------------------------- brightness (gamma)
//
// Kingpin is very dark on modern displays; the final pass applies a gamma curve (lifts shadows,
// keeps black and white). gammaAdjust() changes it live (Cmd+= / Cmd+- on the Mac); the value is
// kept in main/kpmac_gamma.txt (KP_GAMMA overrides).

namespace
{
    std::atomic<float> g_gamma{ 1.3f };
    std::string gammaFile() { return g_gameDir + "/main/kpmac_gamma.txt"; }
    void loadGamma()
    {
        float v = 1.3f;
        if (FILE* f = fopen(gammaFile().c_str(), "r"))
        {
            if (fscanf(f, "%f", &v) != 1)
                v = 1.3f;
            fclose(f);
        }
        if (const char* e = getenv("KP_GAMMA"))
            v = (float)atof(e);
        g_gamma = std::clamp(v, 0.5f, 3.0f);
    }
}

void gammaAdjust(float delta)
{
    const float v = std::clamp(g_gamma.load() + delta, 0.5f, 3.0f);
    g_gamma = v;
    if (FILE* f = fopen(gammaFile().c_str(), "w"))
    {
        fprintf(f, "%.2f\n", v);
        fclose(f);
    }
    logf("[gfx] brightness (gamma) %.2f", v);
}

// ---------------------------------------------------------------- wgl

namespace
{
    bool g_haveContext = false;
    int g_glW = 640, g_glH = 480;
    std::map<uint32_t, uint32_t> g_stringCache;
}

WIN32API(opengl32, wglChoosePixelFormat, 2) { RET(1); }
WIN32API(opengl32, wglSetPixelFormat, 3) { RET(1); }
WIN32API(opengl32, wglGetPixelFormat, 1) { RET(1); }
WIN32API(opengl32, wglDescribePixelFormat, 4)
{
    if (ARG(3))
        describePixelFormat(ARG(3));
    RET(1);
}

WIN32API(opengl32, wglCreateContext, 1)
{
    g_haveContext = platform::glCreate();
    logf("[wgl] context %s", g_haveContext ? "created" : "creation failed");
    RET(g_haveContext ? 0x6C000001 : 0);
}

WIN32API(opengl32, wglMakeCurrent, 2)
{
    if (!ARG(1))
        RET(1);
    {
        std::lock_guard lock(g_winMutex);
        const Window& w = g_windows[g_mainHwnd];
        g_glW = w.w;
        g_glH = w.h;
    }
    platform::glMakeCurrent(g_glW, g_glH);
    glPostInit(g_glW, g_glH);
    loadGamma();
    logf("[wgl] make current %dx%d: %s / %s", g_glW, g_glH, (const char*)glGetString(GL_RENDERER),
         (const char*)glGetString(GL_VERSION));
    RET(1);
}

WIN32API(opengl32, wglDeleteContext, 1)
{
    platform::glRelease();
    g_haveContext = false;
    RET(1);
}
WIN32API(opengl32, wglGetCurrentContext, 0) { RET(g_haveContext ? 0x6C000001 : 0); }
WIN32API(opengl32, wglGetCurrentDC, 0) { RET(0xDC000000u | g_mainHwnd); }
WIN32API(opengl32, wglShareLists, 2) { RET(1); }
WIN32API(opengl32, wglCopyContext, 3) { RET(1); }
WIN32API(opengl32, wglCreateLayerContext, 2) { RET(0); }
WIN32API(opengl32, wglDescribeLayerPlane, 5) { RET(0); }
WIN32API(opengl32, wglGetLayerPaletteEntries, 5) { RET(0); }
WIN32API(opengl32, wglSetLayerPaletteEntries, 5) { RET(0); }
WIN32API(opengl32, wglRealizeLayerPalette, 3) { RET(0); }
WIN32API(opengl32, wglSwapLayerBuffers, 2) { RET(1); }
WIN32API(opengl32, wglUseFontBitmapsA, 4) { RET(0); }

// Shows the frame: final pass into the window framebuffer, present, then wait for the next
// display refresh (one game frame per refresh).
static void swapBuffers()
{
    int dw, dh;
    platform::glDrawableSize(dw, dh);
    {
        ProfScope p(kProfFinal);
        glPostPresent(g_gamma, dw, dh, [] {
            int w, h;
            platform::glDrawableSize(w, h);
            touch::draw(w, h); // on-screen controls (touch platforms)
            frameHook();
        });
        glTexturesEndFrame();
    }
    {
        ProfScope p(kProfSwap);
        platform::glPresent();
    }
    glPostAfterSwap();
    {
        ProfScope p(kProfVblank);
        platform::glWaitVblank();
    }
    static const bool check = getenv("KP_GL_ERRORS") != nullptr;
    if (check)
        for (GLenum e; (e = glGetError()) != GL_NO_ERROR;)
            logf("[gl] error 0x%04X", e);
}
WIN32API(opengl32, wglSwapBuffers, 1)
{
    swapBuffers();
    RET(1);
}
WIN32API(gdi32, SwapBuffers, 1)
{
    swapBuffers();
    RET(1);
}
WIN32API(opengl32, wglSwapIntervalEXT, 1) { RET(1); } // the platform paces frames itself

// SGIS multitexture in terms of the GL 1.3 API (targets 0x835E.. -> GL_TEXTURE0..)
WIN32API(opengl32, glSelectTextureSGIS, 1) { emu::glActiveTexture(GL_TEXTURE0 + (ARG(0) - 0x835E)); }
WIN32API(opengl32, glMTexCoord2fSGIS, 3) { emu::glMultiTexCoord2f(GL_TEXTURE0 + (ARG(0) - 0x835E), ARGF(1), ARGF(2)); }
WIN32API(opengl32, glLockArraysEXT, 2) {}
WIN32API(opengl32, glUnlockArraysEXT, 0) {}

WIN32API(opengl32, wglGetProcAddress, 1)
{
    std::string n = gstr(ARG(0));
    static const std::map<std::string, std::string> alias = {
        { "glActiveTextureARB", "glActiveTexture" },
        { "glClientActiveTextureARB", "glClientActiveTexture" },
        { "glMultiTexCoord2fARB", "glMultiTexCoord2f" },
        { "glMultiTexCoord2fvARB", "glMultiTexCoord2fv" },
    };
    if (auto it = alias.find(n); it != alias.end())
        n = it->second;
    static const char* known[] = { "glActiveTexture", "glClientActiveTexture", "glMultiTexCoord2f", "glMultiTexCoord2fv",
                                   "glSelectTextureSGIS", "glMTexCoord2fSGIS", "glLockArraysEXT", "glUnlockArraysEXT",
                                   "wglSwapIntervalEXT" };
    for (const char* k : known)
        if (n == k)
            RET(hleThunk("opengl32", n));
    logf("[wgl] wglGetProcAddress(%s): not provided", n.c_str());
    RET(0);
}

WIN32API(opengl32, glGetString, 1)
{
    const uint32_t name = ARG(0);
    auto it = g_stringCache.find(name);
    if (it != g_stringCache.end())
        RET(it->second);
    std::string s;
    if (name == GL_EXTENSIONS)
        s = "GL_ARB_multitexture GL_EXT_compiled_vertex_array WGL_EXT_swap_control";
    else if (name == GL_VERSION)
        s = "1.3.0";
    else
    {
        const GLubyte* v = glGetString(name);
        s = v ? (const char*)v : "";
    }
    const uint32_t a = guestStrdup(s);
    g_stringCache[name] = a;
    RET(a);
}

WIN32API(opengl32, glGetPointerv, 2) { wr32(ARG(1), 0); } // no client arrays in use
