// Android platform layer: NativeActivity (android_native_app_glue), EGL + OpenGL ES 3, AAudio,
// touch controls (runtime/touch_controls.cpp), game controllers, AImageDecoder.
//
// Game data: the app's external files directory (/sdcard/Android/data/<package>/files, writable
// with adb push) holds the installed game (kingpin.exe, ref_gl.dll, main/...), the disc's data
// track image track1.iso and optionally cd/<name>.cue with the audio track .bin files for music.
// kp_env.txt there sets KP_* options (KEY=VALUE per line; ARGS=... adds game arguments).
#include <EGL/egl.h>
#include <aaudio/AAudio.h>
#include <android/bitmap.h>
#include <android/imagedecoder.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android_native_app_glue.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>

#include "../gfx/gl_native.h"
#include "../win32/hle.h"
#include "app.h"
#include "platform.h"

void glBlitRgba(const uint32_t* pixels, int w, int h, int viewW, int viewH); // gl_post.cpp

namespace
{
    android_app* g_app;

    // The native window comes and goes with the activity (backgrounding). The guest thread owns
    // the EGL surface: the UI thread asks it to drop the surface and waits until it has.
    std::mutex g_winMutex;
    std::condition_variable g_winCv;
    ANativeWindow* g_window = nullptr;   // current window (null while there is none)
    bool g_surfaceReleased = true;       // the guest thread holds no surface for g_window
    bool g_windowChanged = false;

    EGLDisplay g_display = EGL_NO_DISPLAY;
    EGLConfig g_config;
    EGLContext g_context = EGL_NO_CONTEXT;
    EGLSurface g_surface = EGL_NO_SURFACE;
    int g_renderW = 0, g_renderH = 0;

    float renderScale()
    {
        const char* s = getenv("KP_RENDER_SCALE");
        return s ? std::clamp((float)atof(s), 0.25f, 1.0f) : 1.0f;
    }

    // Window size scaled by KP_RENDER_SCALE: the buffers the game renders at.
    void windowRenderSize(ANativeWindow* w, int& rw, int& rh)
    {
        const float k = renderScale();
        rw = (int)(ANativeWindow_getWidth(w) * k) & ~1;
        rh = (int)(ANativeWindow_getHeight(w) * k) & ~1;
    }

    // Guest thread: (re)creates the surface for the current window; blocks while there is none.
    void ensureSurface()
    {
        std::unique_lock lock(g_winMutex);
        if (g_surface != EGL_NO_SURFACE && !g_windowChanged)
            return;
        if (g_surface != EGL_NO_SURFACE)
        {
            eglMakeCurrent(g_display, EGL_NO_SURFACE, EGL_NO_SURFACE, g_context);
            eglDestroySurface(g_display, g_surface);
            g_surface = EGL_NO_SURFACE;
        }
        g_surfaceReleased = true;
        g_winCv.notify_all();
        g_winCv.wait(lock, [] { return g_window != nullptr; });
        g_windowChanged = false;
        int rw, rh;
        windowRenderSize(g_window, rw, rh);
        ANativeWindow_setBuffersGeometry(g_window, rw, rh, 0);
        // Up to 120 Hz on high refresh rate screens (the game's frame cap is 125; KP_HZ overrides).
        const float hz = getenv("KP_HZ") ? (float)atof(getenv("KP_HZ")) : 120.0f;
        ANativeWindow_setFrameRate(g_window, hz, ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE);
        g_surface = eglCreateWindowSurface(g_display, g_config, g_window, nullptr);
        g_surfaceReleased = false;
        eglMakeCurrent(g_display, g_surface, g_surface, g_context);
        eglSwapInterval(g_display, 1); // one frame per display refresh
        EGLint w = 0, h = 0;
        eglQuerySurface(g_display, g_surface, EGL_WIDTH, &w);
        eglQuerySurface(g_display, g_surface, EGL_HEIGHT, &h);
        g_renderW = w;
        g_renderH = h;
        logf("[gfx] window surface %dx%d", w, h);
    }

    // ---- input

    // Android key code -> PC set-1 scan code (| 0x100 extended) and Windows virtual key.
    bool keyToScan(int32_t k, uint32_t& sc, uint32_t& vk)
    {
        vk = 0;
        if (k >= AKEYCODE_A && k <= AKEYCODE_Z)
        {
            static const uint8_t letters[26] = { 0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
                                                 0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C };
            sc = letters[k - AKEYCODE_A];
            vk = 'A' + (k - AKEYCODE_A);
            return true;
        }
        if (k >= AKEYCODE_1 && k <= AKEYCODE_9)
        {
            sc = 0x02 + (k - AKEYCODE_1);
            vk = '1' + (k - AKEYCODE_1);
            return true;
        }
        switch (k)
        {
        case AKEYCODE_0: sc = 0x0B; vk = '0'; return true;
        case AKEYCODE_ESCAPE: case AKEYCODE_BACK: sc = 0x01; vk = 0x1B; return true;
        case AKEYCODE_ENTER: sc = 0x1C; vk = 0x0D; return true;
        case AKEYCODE_SPACE: sc = 0x39; vk = 0x20; return true;
        case AKEYCODE_TAB: sc = 0x0F; vk = 0x09; return true;
        case AKEYCODE_DEL: sc = 0x0E; vk = 0x08; return true;
        case AKEYCODE_GRAVE: sc = 0x29; return true;
        case AKEYCODE_MINUS: sc = 0x0C; return true;
        case AKEYCODE_EQUALS: sc = 0x0D; return true;
        case AKEYCODE_PERIOD: sc = 0x34; return true;
        case AKEYCODE_SLASH: sc = 0x35; return true;
        case AKEYCODE_SHIFT_LEFT: sc = 0x2A; vk = 0x10; return true;
        case AKEYCODE_SHIFT_RIGHT: sc = 0x36; vk = 0x10; return true;
        case AKEYCODE_CTRL_LEFT: sc = 0x1D; vk = 0x11; return true;
        case AKEYCODE_ALT_LEFT: sc = 0x38; vk = 0x12; return true;
        case AKEYCODE_DPAD_UP: sc = 0x148; vk = 0x26; return true;
        case AKEYCODE_DPAD_DOWN: sc = 0x150; vk = 0x28; return true;
        case AKEYCODE_DPAD_LEFT: sc = 0x14B; vk = 0x25; return true;
        case AKEYCODE_DPAD_RIGHT: sc = 0x14D; vk = 0x27; return true;
        default: break;
        }
        if (k >= AKEYCODE_F1 && k <= AKEYCODE_F10)
        {
            sc = 0x3B + (k - AKEYCODE_F1);
            return true;
        }
        return false;
    }

    // Game controller state (buttons from key events, sticks and triggers from motion events).
    pad::State g_pad;
    bool g_padSeen = false;

    int padBit(int32_t k)
    {
        switch (k)
        {
        case AKEYCODE_BUTTON_A: return 0;
        case AKEYCODE_BUTTON_B: return 1;
        case AKEYCODE_BUTTON_X: return 2;
        case AKEYCODE_BUTTON_Y: return 3;
        case AKEYCODE_BUTTON_L1: return 4;
        case AKEYCODE_BUTTON_R1: return 5;
        case AKEYCODE_BUTTON_L2: return 6;
        case AKEYCODE_BUTTON_R2: return 7;
        case AKEYCODE_BUTTON_THUMBL: return 8;
        case AKEYCODE_BUTTON_THUMBR: return 9;
        case AKEYCODE_BUTTON_SELECT: return 10;
        default: return -1;
        }
    }

    bool fromController(int32_t source) { return (source & (AINPUT_SOURCE_GAMEPAD | AINPUT_SOURCE_JOYSTICK)) != 0; }

    int32_t onInput(android_app*, AInputEvent* e)
    {
        const int32_t source = AInputEvent_getSource(e);
        if (AInputEvent_getType(e) == AINPUT_EVENT_TYPE_KEY)
        {
            const int32_t k = AKeyEvent_getKeyCode(e), action = AKeyEvent_getAction(e);
            if (action == AKEY_EVENT_ACTION_MULTIPLE)
                return 0;
            const bool down = action == AKEY_EVENT_ACTION_DOWN;
            const int bit = padBit(k);
            if (bit >= 0 || k == AKEYCODE_BUTTON_START || (fromController(source) && k >= AKEYCODE_DPAD_UP && k <= AKEYCODE_DPAD_RIGHT))
            {
                if (bit >= 0)
                    g_pad.buttons = down ? g_pad.buttons | 1u << bit : g_pad.buttons & ~(1u << bit);
                if (k == AKEYCODE_BUTTON_START) g_pad.start = down;
                if (k == AKEYCODE_DPAD_UP) g_pad.up = down;
                if (k == AKEYCODE_DPAD_DOWN) g_pad.down = down;
                if (k == AKEYCODE_DPAD_LEFT) g_pad.left = down;
                if (k == AKEYCODE_DPAD_RIGHT) g_pad.right = down;
                if (!g_padSeen)
                    logf("[pad] controller in use");
                g_padSeen = true;
                pad::update(g_pad);
                return 1;
            }
            if (AKeyEvent_getRepeatCount(e) > 0)
                return 1;
            uint32_t sc, vk;
            if (k == AKEYCODE_VOLUME_UP || k == AKEYCODE_VOLUME_DOWN || !keyToScan(k, sc, vk))
                return 0; // volume keys stay with the system
            input::key(sc, down, vk);
            return 1;
        }
        if (AInputEvent_getType(e) != AINPUT_EVENT_TYPE_MOTION)
            return 0;
        if (fromController(source))
        {
            const auto axis = [&](int32_t a) { return AMotionEvent_getAxisValue(e, a, 0); };
            g_pad.lx = axis(AMOTION_EVENT_AXIS_X);
            g_pad.ly = -axis(AMOTION_EVENT_AXIS_Y);
            g_pad.rx = axis(AMOTION_EVENT_AXIS_Z);
            g_pad.ry = -axis(AMOTION_EVENT_AXIS_RZ);
            g_pad.lt = std::max(axis(AMOTION_EVENT_AXIS_LTRIGGER), axis(AMOTION_EVENT_AXIS_BRAKE));
            g_pad.rt = std::max(axis(AMOTION_EVENT_AXIS_RTRIGGER), axis(AMOTION_EVENT_AXIS_GAS));
            const float hx = axis(AMOTION_EVENT_AXIS_HAT_X), hy = axis(AMOTION_EVENT_AXIS_HAT_Y);
            g_pad.left = hx < -0.5f;
            g_pad.right = hx > 0.5f;
            g_pad.up = hy < -0.5f;
            g_pad.down = hy > 0.5f;
            g_pad.buttons = (g_pad.buttons & ~0xC0u) | (g_pad.lt > 0.4f ? 0x40u : 0) | (g_pad.rt > 0.4f ? 0x80u : 0);
            g_padSeen = true;
            pad::update(g_pad);
            return 1;
        }
        // Touch: positions in window pixels -> render buffer pixels.
        float sx = 1, sy = 1;
        {
            std::lock_guard lock(g_winMutex);
            if (g_window && ANativeWindow_getWidth(g_window) > 0)
            {
                sx = (float)g_renderW / ANativeWindow_getWidth(g_window);
                sy = (float)g_renderH / ANativeWindow_getHeight(g_window);
            }
        }
        const int32_t action = AMotionEvent_getAction(e), kind = action & AMOTION_EVENT_ACTION_MASK;
        const size_t index = (size_t)((action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
        const auto id = [&](size_t i) { return (intptr_t)AMotionEvent_getPointerId(e, i) + 1; };
        switch (kind)
        {
        case AMOTION_EVENT_ACTION_DOWN:
        case AMOTION_EVENT_ACTION_POINTER_DOWN:
            touch::begin(id(index), AMotionEvent_getX(e, index) * sx, AMotionEvent_getY(e, index) * sy);
            break;
        case AMOTION_EVENT_ACTION_MOVE:
            for (size_t i = 0; i < AMotionEvent_getPointerCount(e); i++)
                touch::move(id(i), AMotionEvent_getX(e, i) * sx, AMotionEvent_getY(e, i) * sy);
            break;
        case AMOTION_EVENT_ACTION_UP:
        case AMOTION_EVENT_ACTION_POINTER_UP:
            touch::end(id(index));
            break;
        case AMOTION_EVENT_ACTION_CANCEL:
            for (size_t i = 0; i < AMotionEvent_getPointerCount(e); i++)
                touch::end(id(i));
            break;
        default: break;
        }
        return 1;
    }

    // ---- start-up

    std::string g_dataDir;
    bool g_started = false;

    void loadEnvFile(std::vector<std::string>& args)
    {
        std::ifstream f(g_dataDir + "/kp_env.txt");
        std::string line;
        while (std::getline(f, line))
        {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                line.pop_back();
            const size_t eq = line.find('=');
            if (line.empty() || line[0] == '#' || eq == std::string::npos)
                continue;
            const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
            if (k == "ARGS")
            {
                std::istringstream ss(v);
                for (std::string a; ss >> a;)
                    args.push_back(a);
            }
            else
                setenv(k.c_str(), v.c_str(), 1);
        }
    }

    void startGame()
    {
        if (g_started)
            return;
        g_started = true;
        AppConfig cfg;
        cfg.hostExeDir = g_dataDir;
        cfg.gameDir = g_dataDir;
        cfg.discDir = access((g_dataDir + "/track1.iso").c_str(), R_OK) == 0 ? g_dataDir + "/track1.iso" : g_dataDir + "/disc";
        if (DIR* d = opendir((g_dataDir + "/cd").c_str()))
        {
            while (dirent* de = readdir(d))
            {
                const std::string n = de->d_name;
                if (n.size() > 4 && strcasecmp(n.c_str() + n.size() - 4, ".cue") == 0)
                    cfg.cuePath = g_dataDir + "/cd/" + n;
            }
            closedir(d);
        }
        loadEnvFile(cfg.args);
        if (access((g_dataDir + "/kingpin.exe").c_str(), R_OK) != 0)
        {
            logf("[main] no game files in %s: copy kingpin.exe, ref_gl.dll, main/ and track1.iso there", g_dataDir.c_str());
            return;
        }
        if (appInit(cfg))
            appStartGuest();
    }

    void onCmd(android_app* app, int32_t cmd)
    {
        switch (cmd)
        {
        case APP_CMD_INIT_WINDOW:
        {
            std::lock_guard lock(g_winMutex);
            g_window = app->window;
            g_windowChanged = true;
            g_winCv.notify_all();
            break;
        }
        case APP_CMD_TERM_WINDOW:
        {
            // The surface must be gone before the window is: wait for the guest thread to drop it.
            std::unique_lock lock(g_winMutex);
            g_window = nullptr;
            g_windowChanged = true;
            g_winCv.wait_for(lock, std::chrono::seconds(2), [] { return g_surfaceReleased; });
            break;
        }
        case APP_CMD_GAINED_FOCUS: input::activate(true); break;
        case APP_CMD_LOST_FOCUS: input::activate(false); break;
        default: break;
        }
    }
}

// ---------------------------------------------------------------- platform interface

namespace platform
{
    void showWindow(int, int, const char*) {}
    void hideWindow() {}
    void setFullscreen(bool) {}
    void setMouseCaptured(bool) {}

    void screenSize(int& w, int& h)
    {
        std::unique_lock lock(g_winMutex);
        g_winCv.wait(lock, [] { return g_window != nullptr; });
        windowRenderSize(g_window, w, h);
        if (w < h)
            std::swap(w, h); // landscape (the activity may still be rotating)
    }

    bool glCreate()
    {
        if (g_context != EGL_NO_CONTEXT)
            return true; // already made (for the intro, or before a video restart)
        g_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        eglInitialize(g_display, nullptr, nullptr);
        const EGLint attrs[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RED_SIZE, 8,
                                 EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE };
        EGLint n = 0;
        if (!eglChooseConfig(g_display, attrs, &g_config, 1, &n) || n < 1)
            return false;
        const EGLint ctxAttrs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
        g_context = eglCreateContext(g_display, g_config, EGL_NO_CONTEXT, ctxAttrs);
        return g_context != EGL_NO_CONTEXT;
    }

    void glMakeCurrent(int, int)
    {
        // Wait (briefly) for the landscape window, then attach.
        for (int i = 0; i < 100; i++)
        {
            {
                std::lock_guard lock(g_winMutex);
                if (g_window && ANativeWindow_getWidth(g_window) > ANativeWindow_getHeight(g_window))
                    break;
            }
            usleep(20000);
        }
        {
            std::lock_guard lock(g_winMutex);
            g_windowChanged = true;
        }
        ensureSurface();
    }

    // The context itself is kept for the life of the process (see mac.mm); the surface stays too.
    void glRelease() {}

    void glPresent()
    {
        eglSwapBuffers(g_display, g_surface);
        ensureSurface(); // follows window changes (and waits while the app is in the background)
    }

    void glWaitVblank() {} // eglSwapInterval(1) paces

    uint32_t glWindowFramebuffer() { return 0; }

    void glDrawableSize(int& w, int& h)
    {
        w = g_renderW;
        h = g_renderH;
    }

    // The intro movie's frames (DirectDraw), before the game has set up GL.
    void presentRgba(const uint32_t* pixels, int w, int h)
    {
        if (g_context == EGL_NO_CONTEXT)
        {
            glCreate();
            glMakeCurrent(w, h);
        }
        glBlitRgba(pixels, w, h, g_renderW, g_renderH);
        glPresent();
    }

    // ---- audio (AAudio)
    namespace
    {
        void (*g_render)(float*, uint32_t);
        aaudio_data_callback_result_t audioCallback(AAudioStream*, void*, void* data, int32_t frames)
        {
            g_render(static_cast<float*>(data), (uint32_t)frames);
            return AAUDIO_CALLBACK_RESULT_CONTINUE;
        }
    }

    void audioStart(void (*render)(float* out, uint32_t frames))
    {
        g_render = render;
        AAudioStreamBuilder* b = nullptr;
        if (AAudio_createStreamBuilder(&b) != AAUDIO_OK)
            return;
        AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_FLOAT);
        AAudioStreamBuilder_setChannelCount(b, 2);
        AAudioStreamBuilder_setSampleRate(b, 44100);
        AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
        AAudioStreamBuilder_setDataCallback(b, audioCallback, nullptr);
        AAudioStream* s = nullptr;
        const aaudio_result_t r = AAudioStreamBuilder_openStream(b, &s);
        AAudioStreamBuilder_delete(b);
        if (r != AAUDIO_OK || AAudioStream_requestStart(s) != AAUDIO_OK)
        {
            logf("[audio] could not start AAudio output (%d)", r);
            return;
        }
        logf("[audio] AAudio output started (%d Hz stereo)", AAudioStream_getSampleRate(s));
    }

    // ---- images
    bool decodeImage(const std::string& path, std::vector<uint8_t>& rgba, int& w, int& h)
    {
        const int fd = open(path.c_str(), O_RDONLY);
        if (fd < 0)
            return false;
        AImageDecoder* dec = nullptr;
        bool ok = false;
        if (AImageDecoder_createFromFd(fd, &dec) == ANDROID_IMAGE_DECODER_SUCCESS)
        {
            AImageDecoder_setAndroidBitmapFormat(dec, ANDROID_BITMAP_FORMAT_RGBA_8888);
            AImageDecoder_setUnpremultipliedRequired(dec, true);
            const AImageDecoderHeaderInfo* info = AImageDecoder_getHeaderInfo(dec);
            w = AImageDecoderHeaderInfo_getWidth(info);
            h = AImageDecoderHeaderInfo_getHeight(info);
            const size_t stride = AImageDecoder_getMinimumStride(dec);
            std::vector<uint8_t> buf(stride * h);
            if (AImageDecoder_decodeImage(dec, buf.data(), stride, buf.size()) == ANDROID_IMAGE_DECODER_SUCCESS)
            {
                rgba.resize((size_t)w * h * 4);
                for (int y = 0; y < h; y++)
                    memcpy(&rgba[(size_t)y * w * 4], &buf[y * stride], (size_t)w * 4);
                ok = true;
            }
            AImageDecoder_delete(dec);
        }
        close(fd);
        return ok;
    }

    bool writePng(const std::string& path, const uint8_t* rgba, int w, int h)
    {
        AndroidBitmapInfo info{};
        info.width = (uint32_t)w;
        info.height = (uint32_t)h;
        info.stride = (uint32_t)w * 4;
        info.format = ANDROID_BITMAP_FORMAT_RGBA_8888;
        info.flags = ANDROID_BITMAP_FLAGS_ALPHA_OPAQUE;
        FILE* f = fopen(path.c_str(), "wb");
        if (!f)
            return false;
        const int r = AndroidBitmap_compress(&info, ADataSpace::ADATASPACE_SRGB, rgba, ANDROID_BITMAP_COMPRESS_FORMAT_PNG, 100, f,
                                             [](void* ctx, const void* data, size_t size) {
                                                 return fwrite(data, 1, size, static_cast<FILE*>(ctx)) == size;
                                             });
        fclose(f);
        return r == ANDROID_BITMAP_RESULT_SUCCESS;
    }

    void gamepadStart() {} // controllers arrive as input events (onInput)
}

// ---------------------------------------------------------------- entry

void android_main(android_app* app)
{
    g_app = app;
    app->onAppCmd = onCmd;
    app->onInputEvent = onInput;
    g_dataDir = app->activity->externalDataPath ? app->activity->externalDataPath : app->activity->internalDataPath;
    logf("[main] data directory %s", g_dataDir.c_str());
    while (true)
    {
        int events;
        android_poll_source* source;
        while (ALooper_pollOnce(-1, nullptr, &events, (void**)&source) >= 0)
        {
            if (source)
                source->process(app, source);
            if (app->window && !g_started)
                startGame();
            if (app->destroyRequested)
                hostExit(0);
        }
    }
}
