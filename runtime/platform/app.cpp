// Portable process start-up, the guest main thread and per-frame host work.
//
// arguments: [--game dir] [--res WxH] [--seconds N] [game arguments, e.g. +map kpcut1]
// environment:
//   KP_FRAME_DUMP=dir[,N]   write a PNG of every Nth frame (default 60) into dir
//   KP_FRAMETIMES=1         frame pacing statistics every 2 s
//   KP_INPUT=script         scripted key presses "seconds:key:duration,..." (keys: a-z 0-9
//                           enter escape space tab up down left right ctrl shift alt f1-f12 mouse1,
//                           "type text", lookN)
//   KP_MEMSNAP=dir,t1,...   kingpin.exe data snapshots
//   KP_DISC=path            the CD (D:\): a directory or the data track's .iso image
//   KP_TRACE_HLE / KP_TRACE_GPA / KP_GL_ERRORS / KP_NO_AUDIO / KP_CUE
#include "app.h"

#include <pthread.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "../gfx/gl_native.h"
#include "../win32/kobject.h"
#include "platform.h"

void memInit();
void crashHandlerInstall();
void funcTableInit();
void funcTableInit(const X86Func* funcs, size_t count);
void modulesReset();
void user32Reset();
void dsoundReset();
void ddrawReset();
void modulesInit(const std::string& exePath);
void kingpinTweaksInit();
extern std::string g_resolutionArg;
uint32_t exeEntry();
void gamepadInit();
void postMessageHost(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp);
void injectMouseDelta(int dx, int dy);
extern std::string g_cuePath;
extern std::string g_hostExeDir;

void hostExit(int code)
{
    fflush(stdout);
    fflush(stderr);
    _exit(code);
}

// ---------------------------------------------------------------- frame hook

namespace
{
    std::string g_dumpDir;
    int g_dumpEvery = 60;
    uint64_t g_frames = 0;
    auto g_fpsT0 = std::chrono::steady_clock::now();

    // The window framebuffer (bound by the caller) as a PNG.
    void dumpFrame(const std::string& path)
    {
        int w, h;
        platform::glDrawableSize(w, h);
        if (w <= 0 || h <= 0)
            return;
        std::vector<uint8_t> px((size_t)w * h * 4), flipped(px.size());
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        for (int y = 0; y < h; y++)
            memcpy(&flipped[(size_t)y * w * 4], &px[(size_t)(h - 1 - y) * w * 4], (size_t)w * 4);
        for (size_t i = 3; i < flipped.size(); i += 4)
            flipped[i] = 255;
        platform::writePng(path, flipped.data(), w, h);
    }
}

double g_profMs[kProfCount];

void frameHook()
{
    g_frames++;
    // Frame pacing statistics every 2 s (KP_FRAMETIMES=1): worst / 99th percentile / hitches.
    static const bool frameTimes = getenv("KP_FRAMETIMES") != nullptr;
    if (frameTimes)
    {
        static auto last = std::chrono::steady_clock::now();
        static auto windowStart = last;
        static std::vector<double> times;
        const auto now = std::chrono::steady_clock::now();
        times.push_back(std::chrono::duration<double, std::milli>(now - last).count());
        last = now;
        if (std::chrono::duration<double>(now - windowStart).count() >= 2.0)
        {
            std::vector<double> t = times;
            std::sort(t.begin(), t.end());
            double sum = 0;
            int over12 = 0, over25 = 0;
            for (double v : t)
            {
                sum += v;
                over12 += v > 12.0;
                over25 += v > 25.0;
            }
            int bins[4] = {};
            for (double v : t)
                bins[v < 7 ? 0 : v < 9.6 ? 1 : v < 12 ? 2 : 3]++;
            logf("[frametime] n=%zu avg %.2f ms, p1 %.2f, p99 %.2f, max %.2f, >12ms %d, >25ms %d | <7:%d 7-9.6:%d 9.6-12:%d >12:%d",
                 t.size(), sum / t.size(), t[t.size() / 100], t[t.size() * 99 / 100], t.back(), over12, over25, bins[0],
                 bins[1], bins[2], bins[3]);
            const double n = (double)t.size();
            logf("[frametime] per frame: effects %.2f, final pass %.2f, swap %.2f, vblank wait %.2f, game draw calls %.2f ms",
                 g_profMs[kProfEffects] / n, g_profMs[kProfFinal] / n, g_profMs[kProfSwap] / n, g_profMs[kProfVblank] / n,
                 g_profMs[kProfDraw] / n);
            for (double& v : g_profMs)
                v = 0;
            times.clear();
            windowStart = now;
        }
    }
    if (!g_dumpDir.empty() && g_frames % g_dumpEvery == 0)
    {
        char name[64];
        snprintf(name, sizeof name, "/frame_%06llu.png", (unsigned long long)g_frames);
        dumpFrame(g_dumpDir + name);
    }
    if (g_frames % 120 == 0)
    {
        const auto now = std::chrono::steady_clock::now();
        const double dt = std::chrono::duration<double>(now - g_fpsT0).count();
        g_fpsT0 = now;
        logf("[gfx] frame %llu: %.1f fps", (unsigned long long)g_frames, 120.0 / dt);
    }
}

// ---------------------------------------------------------------- scripted input

namespace
{
    uint32_t scanForName(const std::string& k)
    {
        static const char* row1 = "1234567890";
        static const char* qwerty = "qwertyuiop";
        static const char* asdf = "asdfghjkl";
        static const char* zxcv = "zxcvbnm";
        if (k.size() == 1)
        {
            const char ch = k[0];
            if (const char* p = strchr(row1, ch)) return 0x02 + (uint32_t)(p - row1);
            if (const char* p = strchr(qwerty, ch)) return 0x10 + (uint32_t)(p - qwerty);
            if (const char* p = strchr(asdf, ch)) return 0x1E + (uint32_t)(p - asdf);
            if (const char* p = strchr(zxcv, ch)) return 0x2C + (uint32_t)(p - zxcv);
            if (ch == '`') return 0x29;
        }
        if (k == "enter") return 0x1C;
        if (k == "escape") return 0x01;
        if (k == "space") return 0x39;
        if (k == "tab") return 0x0F;
        if (k == "up") return 0x148;
        if (k == "down") return 0x150;
        if (k == "left") return 0x14B;
        if (k == "right") return 0x14D;
        if (k == "ctrl") return 0x1D;
        if (k == "shift") return 0x2A;
        if (k == "alt") return 0x38;
        if (k.size() >= 2 && k[0] == 'f' && isdigit(k[1]))
        {
            const int n = atoi(k.c_str() + 1);
            return n <= 10 ? 0x3A + (uint32_t)n : n == 11 ? 0x57 : 0x58;
        }
        return 0;
    }

    void inputScript(std::string script)
    {
        struct Ev { double t; std::string key; double dur; };
        std::vector<Ev> evs;
        size_t pos = 0;
        while (pos < script.size())
        {
            size_t e = script.find(',', pos);
            if (e == std::string::npos) e = script.size();
            const std::string item = script.substr(pos, e - pos);
            pos = e + 1;
            const size_t a = item.find(':'), b = item.find(':', a + 1);
            if (a == std::string::npos) continue;
            evs.push_back({ atof(item.c_str()), item.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1),
                            b == std::string::npos ? 0.1 : atof(item.c_str() + b + 1) });
        }
        std::thread([evs] {
            const auto t0 = std::chrono::steady_clock::now();
            for (const auto& ev : evs)
            {
                std::this_thread::sleep_until(t0 + std::chrono::duration<double>(ev.t));
                logf("[input] script: %s for %.2fs", ev.key.c_str(), ev.dur);
                if (ev.key.rfind("type ", 0) == 0) // type a console line (lower case, digits, space, _ . /)
                {
                    for (char ch : ev.key.substr(5))
                    {
                        bool shift = false;
                        uint32_t sc = 0;
                        if (ch == ' ') sc = 0x39;
                        else if (ch == '_') { sc = 0x0C; shift = true; }
                        else if (ch == '.') sc = 0x34;
                        else if (ch == '/') sc = 0x35;
                        else if (ch == '-') sc = 0x0C;
                        else sc = scanForName(std::string(1, ch));
                        const auto key = [&](uint32_t code, bool down) {
                            postMessageHost(0, down ? 0x100 : 0x101, 0, 1 | code << 16 | (down ? 0 : 3u << 30));
                        };
                        if (shift) key(0x2A, true);
                        key(sc, true);
                        std::this_thread::sleep_for(std::chrono::milliseconds(30));
                        key(sc, false);
                        if (shift) key(0x2A, false);
                        std::this_thread::sleep_for(std::chrono::milliseconds(30));
                    }
                    continue;
                }
                if (ev.key.rfind("look", 0) == 0) // lookX: X pixels of horizontal motion spread over the duration
                {
                    const int total = atoi(ev.key.c_str() + 4), steps = std::max(1, (int)(ev.dur * 100));
                    for (int k = 0; k < steps; k++)
                    {
                        injectMouseDelta(total / steps, 0);
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                    continue;
                }
                if (ev.key == "mouse1")
                {
                    postMessageHost(0, 0x201, 1, 0);
                    std::this_thread::sleep_for(std::chrono::duration<double>(ev.dur));
                    postMessageHost(0, 0x202, 0, 0);
                    continue;
                }
                const uint32_t sc = scanForName(ev.key);
                const uint32_t lp = 1 | (sc & 0xFF) << 16 | ((sc & 0x100) ? 1u << 24 : 0);
                uint32_t vk = 0;
                if (ev.key.size() == 1 && isalnum((unsigned char)ev.key[0]))
                    vk = (uint32_t)toupper((unsigned char)ev.key[0]);
                else if (ev.key == "escape") vk = 0x1B;
                else if (ev.key == "enter") vk = 0x0D;
                else if (ev.key == "space") vk = 0x20;
                else if (ev.key == "tab") vk = 0x09;
                postMessageHost(0, 0x100, vk, lp);
                std::this_thread::sleep_for(std::chrono::duration<double>(ev.dur));
                postMessageHost(0, 0x101, vk, lp | 3u << 30);
            }
        }).detach();
    }

    void memSnapshots(const std::string& spec) // debug: "dir,t1,t2,..." dumps kingpin.exe data
    {
        std::vector<double> times;
        const size_t c0 = spec.find(',');
        const std::string dir = spec.substr(0, c0);
        for (size_t p = c0; p != std::string::npos;)
        {
            const size_t e = spec.find(',', p + 1);
            times.push_back(atof(spec.substr(p + 1, e == std::string::npos ? std::string::npos : e - p - 1).c_str()));
            p = e;
        }
        std::thread([dir, times] {
            const auto t0 = std::chrono::steady_clock::now();
            for (size_t k = 0; k < times.size(); k++)
            {
                std::this_thread::sleep_until(t0 + std::chrono::duration<double>(times[k]));
                if (FILE* f = fopen((dir + "/snap" + std::to_string(k) + ".bin").c_str(), "wb"))
                {
                    fwrite(g_mem + 0x454000, 1, 0x1450000 - 0x454000, f);
                    fclose(f);
                }
                logf("[debug] memory snapshot %zu", k);
            }
        }).detach();
    }

    std::string realPath(const std::string& p)
    {
        char real[PATH_MAX];
        return realpath(p.c_str(), real) ? std::string(real) : p;
    }
}

// ---------------------------------------------------------------- start-up

bool appInit(const AppConfig& cfg)
{
    g_hostExeDir = cfg.hostExeDir;
    g_gameDir = realPath(cfg.gameDir);
    g_discDir = realPath(getenv("KP_DISC") ? getenv("KP_DISC") : cfg.discDir); // a directory or the data track image
    g_cuePath = realPath(cfg.cuePath);
    double seconds = 0;
    std::string cmd = KP_EXE_NAME;
    for (size_t i = 0; i < cfg.args.size(); i++)
    {
        const std::string& a = cfg.args[i];
        if (a == "--game" && i + 1 < cfg.args.size())
            g_gameDir = realPath(cfg.args[++i]);
        else if (a == "--res" && i + 1 < cfg.args.size())
            g_resolutionArg = cfg.args[++i];
        else if (a == "--seconds" && i + 1 < cfg.args.size())
            seconds = atof(cfg.args[++i].c_str());
        else
            cmd += " " + a;
    }
    g_commandLine = cmd;
    chdir(g_gameDir.c_str());
    if (const char* d = getenv("KP_FRAME_DUMP"))
    {
        g_dumpDir = d;
        if (size_t p = g_dumpDir.find(','); p != std::string::npos)
        {
            g_dumpEvery = std::max(1, atoi(g_dumpDir.c_str() + p + 1));
            g_dumpDir.resize(p);
        }
        std::error_code ec;
        std::filesystem::create_directories(g_dumpDir, ec);
    }
    logf("[main] game dir %s, command line \"%s\"", g_gameDir.c_str(), cmd.c_str());

    memInit();
    crashHandlerInstall();
    funcTableInit();
    modulesInit(g_gameDir + "/" KP_EXE_NAME);
    if (std::string(KP_EXE_NAME) == "kingpin.exe")
        kingpinTweaksInit();

    gamepadInit();
    touch::scriptStart();
    if (const char* s = getenv("KP_INPUT"))
        inputScript(s);
    if (const char* s = getenv("KP_MEMSNAP"))
        memSnapshots(s);
    if (seconds > 0)
        std::thread([seconds] {
            std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
            logf("[main] --seconds elapsed, exiting");
            hostExit(0);
        }).detach();
    return true;
}

#if KP_INTRO_IN_PROCESS
namespace intro
{
    const X86Func* funcs(size_t& count); // gen_intro, compiled into this binary (CMakeLists.txt)
}

// Platforms without subprocesses play the intro movie (kpintro.exe, recompiled into this binary
// too) on the guest main thread before the game: its ExitProcess unwinds back here, then the
// game's image and functions replace it. (kingpin.exe's own attempt to start kpintro then fails
// quietly and it carries on.)
static void runIntro(GuestThread* t)
{
    const std::string path = g_gameDir + "/kpintro.exe";
    if (getenv("KP_NO_INTRO") || access(path.c_str(), R_OK) != 0)
        return;
    size_t count;
    const X86Func* funcs = intro::funcs(count);
    const Cpu saved = t->cpu;
    const std::string cmd = g_commandLine;
    modulesReset();
    funcTableInit(funcs, count);
    modulesInit(path);
    g_commandLine = "kpintro.exe";
    logf("[main] intro: starting kpintro.exe at %08X", exeEntry());
    extern bool g_introPlaying;
    g_introPlaying = true;
    g_exitUnwinds = true;
    try
    {
        guestCall(t->cpu, exeEntry(), {});
    }
    catch (const ProgramExit& e)
    {
        logf("[main] intro finished (%d)", e.code);
    }
    g_exitUnwinds = false;
    g_introPlaying = false;
    dsoundReset();
    ddrawReset();
    user32Reset();
    modulesReset();
    t->cpu = saved;
    g_commandLine = cmd;
    funcTableInit();
    modulesInit(g_gameDir + "/" KP_EXE_NAME);
    kingpinTweaksInit();
}
#endif

static void* guestMain(void*)
{
#ifdef __APPLE__
    pthread_setname_np("guest main");
#else
    pthread_setname_np(pthread_self(), "guest main");
#endif
    GuestThread* t = guestThreadCreate(16u << 20);
    guestThreadEnter(t);
#if KP_INTRO_IN_PROCESS
    runIntro(t);
#endif
    const uint32_t entry = exeEntry();
    logf("[main] starting guest at %08X", entry);
    guestCall(t->cpu, entry, {});
    logf("[main] guest entry returned");
    hostExit(0);
    return nullptr;
}

void appStartGuest()
{
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256u << 20);
    pthread_t th;
    pthread_create(&th, &attr, guestMain, nullptr);
}
