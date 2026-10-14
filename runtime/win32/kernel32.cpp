// kernel32: handles, threads, synchronization, TLS, time, locale, environment and process.
#include <pthread.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>
#include <spawn.h>
#include <sys/wait.h>

#include "kobject.h"

std::string g_commandLine = "kingpin.exe";

// ---------------------------------------------------------------- handles

namespace
{
    std::mutex g_handleMutex;
    std::unordered_map<uint32_t, std::shared_ptr<KObject>> g_handles;
    uint32_t g_nextHandle = 0x100;
}

uint32_t handleNew(std::shared_ptr<KObject> o)
{
    std::lock_guard lock(g_handleMutex);
    const uint32_t h = g_nextHandle += 4;
    g_handles[h] = std::move(o);
    return h;
}

std::shared_ptr<KObject> handleGet(uint32_t h)
{
    std::lock_guard lock(g_handleMutex);
    auto it = g_handles.find(h);
    return it == g_handles.end() ? nullptr : it->second;
}

bool handleClose(uint32_t h)
{
    std::lock_guard lock(g_handleMutex);
    return g_handles.erase(h) != 0;
}

// ---------------------------------------------------------------- threads

namespace
{
    thread_local GuestThread* t_thread;
    std::atomic<uint32_t> g_nextThreadId{ 0x400 };
}

struct ThreadObj : KObject
{
    GuestThread* thread = nullptr;
    WaitableState state;
    WaitableState* waitable() override { return &state; }
};

GuestThread* currentGuestThread() { return t_thread; }
uint32_t guestThreadId() { return t_thread ? t_thread->id : 0; }
void setLastError(uint32_t e)
{
    if (t_thread)
        t_thread->lastError = e;
}
uint32_t getLastError() { return t_thread ? t_thread->lastError : 0; }

GuestThread* guestThreadCreate(uint32_t stackSize)
{
    auto* t = new GuestThread();
    t->id = g_nextThreadId++;
    stackSize = std::max<uint32_t>(stackSize, 1u << 20);
    t->stackLo = vaReserve(stackSize);
    t->stackHi = t->stackLo + stackSize;
    t->teb = heapAlloc(0x1000, true);
    wr32(t->teb + 0x00, 0xFFFFFFFFu); // SEH chain end
    wr32(t->teb + 0x04, t->stackHi);
    wr32(t->teb + 0x08, t->stackLo);
    wr32(t->teb + 0x18, t->teb); // self
    wr32(t->teb + 0x24, t->id);
    t->cpu.r[4] = t->stackHi - 16;
    t->cpu.fsbase = t->teb;
    return t;
}

void guestThreadEnter(GuestThread* t) { t_thread = t; }

WIN32API(kernel32, CreateThread, 6)
{
    const uint32_t stack = ARG(1), start = ARG(2), param = ARG(3), idOut = ARG(5);
    GuestThread* t = guestThreadCreate(stack ? stack : (1u << 20));
    auto obj = std::make_shared<ThreadObj>();
    obj->thread = t;
    const uint32_t h = handleNew(obj);
    if (idOut)
        wr32(idOut, t->id);
    logf("[thread] CreateThread start=%08X param=%08X id=%u", start, param, t->id);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 64u << 20);
    struct Start
    {
        GuestThread* t;
        uint32_t start, param;
        std::shared_ptr<ThreadObj> obj;
    };
    auto* s = new Start{ t, start, param, obj };
    pthread_t pt;
    pthread_create(&pt, &attr, [](void* p) -> void* {
        std::unique_ptr<Start> s(static_cast<Start*>(p));
        guestThreadEnter(s->t);
#ifdef __APPLE__
        pthread_setname_np(("guest thread " + std::to_string(s->t->id)).c_str());
#else
        pthread_setname_np(pthread_self(), ("guest " + std::to_string(s->t->id)).c_str());
#endif
        const uint32_t code = guestCall(s->t->cpu, s->start, { s->param });
        s->t->exitCode = code;
        s->obj->state.signal(true);
        return nullptr;
    }, s);
    pthread_detach(pt);
    RET(h);
}

WIN32API(kernel32, ExitThread, 1)
{
    logf("[thread] ExitThread(%u) on thread %u", ARG(0), guestThreadId());
    // Unwind the host stack back to the thread start by finishing the thread here.
    pthread_exit(nullptr);
}

WIN32API(kernel32, GetCurrentThreadId, 0) { RET(guestThreadId()); }
WIN32API(kernel32, GetCurrentThread, 0) { RET(0xFFFFFFFEu); }
WIN32API(kernel32, GetCurrentProcess, 0) { RET(0xFFFFFFFFu); }
WIN32API(kernel32, GetCurrentProcessId, 0) { RET(0x200); }
WIN32API(kernel32, SetThreadPriority, 2) { RET(1); }
WIN32API(kernel32, GetThreadPriority, 1) { RET(0); }
WIN32API(kernel32, SetPriorityClass, 2) { RET(1); }

// ---------------------------------------------------------------- TLS

namespace
{
    std::mutex g_tlsMutex;
    uint64_t g_tlsUsed = 0;
}

WIN32API(kernel32, TlsAlloc, 0)
{
    std::lock_guard lock(g_tlsMutex);
    for (uint32_t k = 0; k < 64; k++)
        if (!(g_tlsUsed >> k & 1))
        {
            g_tlsUsed |= 1ull << k;
            RET(k);
        }
    RET(0xFFFFFFFFu);
}
WIN32API(kernel32, TlsFree, 1)
{
    std::lock_guard lock(g_tlsMutex);
    g_tlsUsed &= ~(1ull << (ARG(0) & 63));
    RET(1);
}
WIN32API(kernel32, TlsGetValue, 1)
{
    setLastError(0);
    RET(ARG(0) < 64 ? t_thread->tls[ARG(0)] : 0);
}
WIN32API(kernel32, TlsSetValue, 2)
{
    if (ARG(0) >= 64)
        RET(0);
    t_thread->tls[ARG(0)] = ARG(1);
    RET(1);
}

// ---------------------------------------------------------------- synchronization

namespace
{
    std::mutex g_csMutex;
    std::unordered_map<uint32_t, std::unique_ptr<std::recursive_mutex>> g_critical;
    std::recursive_mutex& critical(uint32_t a)
    {
        std::lock_guard lock(g_csMutex);
        auto& p = g_critical[a];
        if (!p)
            p = std::make_unique<std::recursive_mutex>();
        return *p;
    }
}

WIN32API(kernel32, InitializeCriticalSection, 1)
{
    memset(g_mem + ARG(0), 0, 24);
    critical(ARG(0));
    RET(0);
}
WIN32API(kernel32, DeleteCriticalSection, 1)
{
    std::lock_guard lock(g_csMutex);
    g_critical.erase(ARG(0));
    RET(0);
}
WIN32API(kernel32, EnterCriticalSection, 1)
{
    critical(ARG(0)).lock();
    RET(0);
}
WIN32API(kernel32, LeaveCriticalSection, 1)
{
    critical(ARG(0)).unlock();
    RET(0);
}

WIN32API(kernel32, InterlockedIncrement, 1) { RET(__atomic_add_fetch(gptr<uint32_t>(ARG(0)), 1, __ATOMIC_SEQ_CST)); }
WIN32API(kernel32, InterlockedDecrement, 1) { RET(__atomic_sub_fetch(gptr<uint32_t>(ARG(0)), 1, __ATOMIC_SEQ_CST)); }
WIN32API(kernel32, InterlockedExchange, 2) { RET(__atomic_exchange_n(gptr<uint32_t>(ARG(0)), ARG(1), __ATOMIC_SEQ_CST)); }

struct EventObj : KObject
{
    WaitableState state;
    WaitableState* waitable() override { return &state; }
};

WIN32API(kernel32, CreateEventA, 4)
{
    auto e = std::make_shared<EventObj>();
    e->state.manualReset = ARG(1) != 0;
    e->state.signaled = ARG(2) != 0;
    setLastError(0);
    RET(handleNew(e));
}
WIN32API(kernel32, SetEvent, 1)
{
    auto o = handleGet(ARG(0));
    if (!o || !o->waitable())
        RET(0);
    o->waitable()->signal(true);
    RET(1);
}
WIN32API(kernel32, ResetEvent, 1)
{
    auto o = handleGet(ARG(0));
    if (!o || !o->waitable())
        RET(0);
    o->waitable()->signal(false);
    RET(1);
}

struct MutexObj : KObject
{
    WaitableState state; // signaled = not owned (ownership is not tracked per thread)
    WaitableState* waitable() override { return &state; }
};

WIN32API(kernel32, CreateMutexA, 3)
{
    auto m = std::make_shared<MutexObj>();
    m->state.signaled = !ARG(1);
    setLastError(0);
    RET(handleNew(m));
}
WIN32API(kernel32, ReleaseMutex, 1)
{
    auto o = handleGet(ARG(0));
    if (o && o->waitable())
        o->waitable()->signal(true);
    RET(1);
}

WIN32API(kernel32, WaitForSingleObject, 2)
{
    auto o = handleGet(ARG(0));
    if (!o || !o->waitable())
        RET(0xFFFFFFFFu);
    RET(o->waitable()->wait(ARG(1)) ? 0 : 0x102); // WAIT_OBJECT_0 / WAIT_TIMEOUT
}

WIN32API(kernel32, WaitForMultipleObjects, 4)
{
    const uint32_t n = ARG(0), arr = ARG(1), all = ARG(2), ms = ARG(3);
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms == 0xFFFFFFFFu ? 1u << 30 : ms);
    while (true)
    {
        uint32_t ready = 0, first = ~0u;
        for (uint32_t k = 0; k < n; k++)
        {
            auto o = handleGet(rd32(arr + 4 * k));
            if (o && o->waitable() && o->waitable()->isSignaled())
            {
                ready++;
                if (first == ~0u)
                    first = k;
            }
        }
        if (all ? ready == n : ready > 0)
        {
            for (uint32_t k = 0; k < n; k++)
                if (all || k == first)
                    if (auto o = handleGet(rd32(arr + 4 * k)); o && o->waitable())
                        o->waitable()->wait(0);
            RET(all ? 0 : first);
        }
        if (std::chrono::steady_clock::now() >= end)
            RET(0x102);
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
}

WIN32API(kernel32, CloseHandle, 1) { RET(handleClose(ARG(0)) ? 1 : 1); }

WIN32API(kernel32, Sleep, 1)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ARG(0)));
    RET(0);
}

// ---------------------------------------------------------------- time

namespace
{
    const auto g_t0 = std::chrono::steady_clock::now();

    void systemTime(uint32_t p, bool local)
    {
        struct timeval tv;
        gettimeofday(&tv, nullptr);
        struct tm t;
        if (local)
            localtime_r(&tv.tv_sec, &t);
        else
            gmtime_r(&tv.tv_sec, &t);
        wr16(p + 0, (uint16_t)(t.tm_year + 1900));
        wr16(p + 2, (uint16_t)(t.tm_mon + 1));
        wr16(p + 4, (uint16_t)t.tm_wday);
        wr16(p + 6, (uint16_t)t.tm_mday);
        wr16(p + 8, (uint16_t)t.tm_hour);
        wr16(p + 10, (uint16_t)t.tm_min);
        wr16(p + 12, (uint16_t)t.tm_sec);
        wr16(p + 14, (uint16_t)(tv.tv_usec / 1000));
    }
}

uint32_t msSinceStart()
{
    return (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - g_t0).count() + 100000;
}

WIN32API(kernel32, GetTickCount, 0) { RET(msSinceStart()); }
WIN32API(kernel32, GetLocalTime, 1)
{
    systemTime(ARG(0), true);
    RET(0);
}
WIN32API(kernel32, GetSystemTime, 1)
{
    systemTime(ARG(0), false);
    RET(0);
}
WIN32API(kernel32, GetTimeZoneInformation, 1)
{
    memset(g_mem + ARG(0), 0, 172);
    RET(0); // TIME_ZONE_ID_UNKNOWN, bias 0 (times are treated as local)
}
WIN32API(kernel32, FileTimeToLocalFileTime, 2)
{
    wr64(ARG(1), rd64(ARG(0)));
    RET(1);
}
WIN32API(kernel32, FileTimeToSystemTime, 2)
{
    const uint64_t ft = rd64(ARG(0));
    const time_t secs = (time_t)(ft / 10000000ull - 11644473600ull);
    struct tm t;
    gmtime_r(&secs, &t);
    const uint32_t p = ARG(1);
    wr16(p + 0, (uint16_t)(t.tm_year + 1900));
    wr16(p + 2, (uint16_t)(t.tm_mon + 1));
    wr16(p + 4, (uint16_t)t.tm_wday);
    wr16(p + 6, (uint16_t)t.tm_mday);
    wr16(p + 8, (uint16_t)t.tm_hour);
    wr16(p + 10, (uint16_t)t.tm_min);
    wr16(p + 12, (uint16_t)t.tm_sec);
    wr16(p + 14, (uint16_t)(ft / 10000 % 1000));
    RET(1);
}

// ---------------------------------------------------------------- version, process, environment

WIN32API(kernel32, GetVersion, 0) { RET(0xC0000A04u); } // Windows 98
WIN32API(kernel32, GetVersionExA, 1)
{
    const uint32_t p = ARG(0);
    wr32(p + 4, 4);
    wr32(p + 8, 10);
    wr32(p + 12, 0x040A07CEu);
    wr32(p + 16, 1); // VER_PLATFORM_WIN32_WINDOWS
    strcpy(gstr(p + 20), " A");
    RET(1);
}

WIN32API(kernel32, GetCommandLineA, 0)
{
    static std::string cached;
    static uint32_t s = 0;
    if (!s || cached != g_commandLine) // a new program in this process
    {
        cached = g_commandLine;
        s = guestStrdup(cached);
    }
    RET(s);
}
WIN32API(kernel32, GetStartupInfoA, 1)
{
    memset(g_mem + ARG(0), 0, 68);
    wr32(ARG(0), 68);
    RET(0);
}
WIN32API(kernel32, GetStdHandle, 1)
{
    const int32_t n = (int32_t)ARG(0);
    RET(n == -10 ? 0x10 : n == -11 ? 0x11 : n == -12 ? 0x12 : 0xFFFFFFFFu);
}
WIN32API(kernel32, SetStdHandle, 2) { RET(1); }
WIN32API(kernel32, SetHandleCount, 1) { RET(ARG(0)); }

WIN32API(kernel32, GetEnvironmentStrings, 0)
{
    static uint32_t s = [] {
        const uint32_t a = heapAlloc(4, true);
        return a;
    }();
    RET(s);
}
WIN32API(kernel32, GetEnvironmentStringsW, 0)
{
    static uint32_t s = heapAlloc(8, true);
    RET(s);
}
WIN32API(kernel32, FreeEnvironmentStringsA, 1) { RET(1); }
WIN32API(kernel32, FreeEnvironmentStringsW, 1) { RET(1); }
WIN32API(kernel32, GetEnvironmentVariableA, 3) { RET(0); }
WIN32API(kernel32, SetEnvironmentVariableA, 2) { RET(1); }
WIN32API(kernel32, SetEnvironmentVariableW, 2) { RET(1); }

void hostExit(int code);

bool g_exitUnwinds = false;

WIN32API(kernel32, ExitProcess, 1)
{
    logf("[proc] ExitProcess(%u)", ARG(0));
    if (g_exitUnwinds)
        throw ProgramExit{ (int)ARG(0) };
    hostExit((int)ARG(0));
}
WIN32API(kernel32, TerminateProcess, 2)
{
    logf("[proc] TerminateProcess(%u)", ARG(1));
    hostExit((int)ARG(1));
}

WIN32API(kernel32, GetLastError, 0) { RET(getLastError()); }
WIN32API(kernel32, SetLastError, 1)
{
    setLastError(ARG(0));
    RET(0);
}
WIN32API(kernel32, SetErrorMode, 1) { RET(0); }
WIN32API(kernel32, SetUnhandledExceptionFilter, 1) { RET(0); }
WIN32API(kernel32, OutputDebugStringA, 1)
{
    logf("[debug] %s", gstr(ARG(0)));
    RET(0);
}
WIN32API(kernel32, UnhandledExceptionFilter, 1) { trap(c, 0, "UnhandledExceptionFilter"); }
WIN32API(kernel32, RaiseException, 4)
{
    logf("[proc] RaiseException(%08X)", ARG(0));
    trap(c, 0, "RaiseException");
}
WIN32API(kernel32, RtlUnwind, 4)
{
    logf("[proc] RtlUnwind called (ignored)");
    RET(0);
}

WIN32API(kernel32, WinExec, 2)
{
    logf("[proc] WinExec(%s) - not supported", gstr(ARG(0)));
    RET(2); // ERROR_FILE_NOT_FOUND
}
std::string g_hostExeDir; // directory of the host executables (set by main)

struct ProcessObj : KObject
{
    WaitableState state;
    int exitCode = 0;
    WaitableState* waitable() override { return &state; }
};

// Guest programs are other recompiled executables built next to this one (e.g. kpintro.exe ->
// ./kpintro); they run as separate host processes, as on Windows.
WIN32API(kernel32, CreateProcessA, 10)
{
    std::string cmd = ARG(1) ? gstr(ARG(1)) : "", app = ARG(0) ? gstr(ARG(0)) : "";
    if (app.empty())
    {
        const size_t e = cmd.find(' ');
        app = cmd.substr(0, e);
    }
    std::string base = app.substr(app.find_last_of("\\/:") + 1);
    for (auto& ch : base)
        ch = (char)tolower(ch);
    if (base.size() > 4 && base.compare(base.size() - 4, 4, ".exe") == 0)
        base.resize(base.size() - 4);
    const std::string host = g_hostExeDir + "/" + base;
#if KP_INTRO_IN_PROCESS
    const bool inProcess = true; // the intro already ran in this process (platform/app.cpp)
#else
    const bool inProcess = false;
#endif
    if (inProcess || access(host.c_str(), X_OK) != 0 || getenv("KP_NO_INTRO"))
    {
        logf("[proc] CreateProcess(%s): no host program %s", app.c_str(), host.c_str());
        setLastError(2);
        RET(0);
    }
    std::vector<std::string> args = { host, "--game", g_gameDir };
    {
        // pass on arguments after the program name
        std::string rest = cmd.find(' ') == std::string::npos ? "" : cmd.substr(cmd.find(' ') + 1);
        size_t p = 0;
        while (p < rest.size())
        {
            size_t e = rest.find(' ', p);
            if (e == std::string::npos) e = rest.size();
            if (e > p) args.push_back(rest.substr(p, e - p));
            p = e + 1;
        }
    }
    std::vector<char*> argv;
    for (auto& a : args)
        argv.push_back(a.data());
    argv.push_back(nullptr);
    pid_t pid;
    extern char** environ;
    if (posix_spawn(&pid, host.c_str(), nullptr, nullptr, argv.data(), environ) != 0)
    {
        setLastError(2);
        RET(0);
    }
    logf("[proc] CreateProcess(%s) -> %s (pid %d)", app.c_str(), host.c_str(), pid);
    auto proc = std::make_shared<ProcessObj>();
    std::thread([proc, pid] {
        int status = 0;
        waitpid(pid, &status, 0);
        proc->exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
        logf("[proc] child %d exited (%d)", pid, proc->exitCode);
        proc->state.signal(true);
    }).detach();
    const uint32_t h = handleNew(proc), th = handleNew(std::make_shared<EventObj>());
    if (const uint32_t pi = ARG(9))
    {
        wr32(pi, h);
        wr32(pi + 4, th);
        wr32(pi + 8, (uint32_t)pid);
        wr32(pi + 12, (uint32_t)pid);
    }
    RET(1);
}

WIN32API(kernel32, GetExitCodeProcess, 2)
{
    auto p = std::dynamic_pointer_cast<ProcessObj>(handleGet(ARG(0)));
    if (!p)
        RET(0);
    wr32(ARG(1), p->state.isSignaled() ? (uint32_t)p->exitCode : 259); // STILL_ACTIVE
    RET(1);
}

// Console (used by the dedicated server only)
WIN32API(kernel32, AllocConsole, 0) { RET(0); }
WIN32API(kernel32, FreeConsole, 0) { RET(1); }
WIN32API(kernel32, GetNumberOfConsoleInputEvents, 2)
{
    wr32(ARG(1), 0);
    RET(1);
}
WIN32API(kernel32, ReadConsoleInputA, 4)
{
    wr32(ARG(3), 0);
    RET(1);
}
WIN32API(kernel32, WriteConsoleInputA, 4) { RET(1); }
WIN32API(kernel32, ReadConsoleOutputCharacterA, 5) { RET(0); }
WIN32API(kernel32, GetConsoleScreenBufferInfo, 2) { RET(0); }
WIN32API(kernel32, SetConsoleScreenBufferSize, 2) { RET(1); }
WIN32API(kernel32, SetConsoleWindowInfo, 3) { RET(1); }
WIN32API(kernel32, GetLargestConsoleWindowSize, 1) { RET(0x00190050); }

// ---------------------------------------------------------------- locale and strings (Latin-1)

WIN32API(kernel32, GetACP, 0) { RET(1252); }
WIN32API(kernel32, GetOEMCP, 0) { RET(437); }
WIN32API(kernel32, GetCPInfo, 2)
{
    const uint32_t p = ARG(1);
    memset(g_mem + p, 0, 20);
    wr32(p, 1);
    wr8(p + 4, '?');
    RET(1);
}
WIN32API(kernel32, GetSystemDefaultLangID, 0) { RET(0x0409); }
WIN32API(kernel32, GetUserDefaultLCID, 0) { RET(0x0409); }

WIN32API(kernel32, MultiByteToWideChar, 6)
{
    const uint32_t src = ARG(2), dst = ARG(4), dstLen = ARG(5);
    int32_t n = (int32_t)ARG(3);
    if (n < 0)
        n = (int32_t)strlen(gstr(src)) + 1;
    if (!dstLen)
        RET(n);
    const uint32_t m = std::min<uint32_t>(n, dstLen);
    for (uint32_t k = 0; k < m; k++)
        wr16(dst + 2 * k, rd8(src + k));
    RET(m);
}
WIN32API(kernel32, WideCharToMultiByte, 8)
{
    const uint32_t src = ARG(2), dst = ARG(4), dstLen = ARG(5);
    int32_t n = (int32_t)ARG(3);
    if (n < 0)
    {
        n = 0;
        while (rd16(src + 2 * n))
            n++;
        n++;
    }
    if (!dstLen)
        RET(n);
    const uint32_t m = std::min<uint32_t>(n, dstLen);
    for (uint32_t k = 0; k < m; k++)
    {
        const uint16_t ch = rd16(src + 2 * k);
        wr8(dst + k, ch < 256 ? (uint8_t)ch : '?');
    }
    RET(m);
}

static uint16_t ctype1(uint32_t ch)
{
    uint16_t t = 0;
    if (ch < 128)
    {
        if (isupper((int)ch)) t |= 0x001;
        if (islower((int)ch)) t |= 0x002;
        if (isdigit((int)ch)) t |= 0x004;
        if (isspace((int)ch)) t |= 0x008;
        if (ispunct((int)ch)) t |= 0x010;
        if (iscntrl((int)ch)) t |= 0x020;
        if (ch == ' ' || ch == '\t') t |= 0x040;
        if (isxdigit((int)ch)) t |= 0x080;
        if (isalpha((int)ch)) t |= 0x100;
    }
    return t;
}

WIN32API(kernel32, GetStringTypeA, 5)
{
    const uint32_t src = ARG(2), out = ARG(4);
    int32_t n = (int32_t)ARG(3);
    if (n < 0)
        n = (int32_t)strlen(gstr(src)) + 1;
    for (int32_t k = 0; k < n; k++)
        wr16(out + 2 * k, ctype1(rd8(src + k)));
    RET(1);
}
WIN32API(kernel32, GetStringTypeW, 4)
{
    const uint32_t src = ARG(1), out = ARG(3);
    int32_t n = (int32_t)ARG(2);
    if (n < 0)
    {
        n = 0;
        while (rd16(src + 2 * n))
            n++;
        n++;
    }
    for (int32_t k = 0; k < n; k++)
        wr16(out + 2 * k, ctype1(rd16(src + 2 * k)));
    RET(1);
}

template <int W>
static void lcmap(Cpu& c)
{
    const uint32_t flags = ARG(1), src = ARG(2), dst = ARG(4), dstLen = ARG(5);
    int32_t n = (int32_t)ARG(3);
    auto get = [&](int32_t k) -> uint32_t { return W == 1 ? rd8(src + k) : rd16(src + 2 * k); };
    if (n < 0)
    {
        n = 0;
        while (get(n))
            n++;
        n++;
    }
    if (!dstLen)
        RET(n);
    for (int32_t k = 0; k < n && k < (int32_t)dstLen; k++)
    {
        uint32_t ch = get(k);
        if (ch < 128)
        {
            if (flags & 0x200) ch = (uint32_t)toupper((int)ch);
            if (flags & 0x100) ch = (uint32_t)tolower((int)ch);
        }
        if (W == 1) wr8(dst + k, (uint8_t)ch);
        else wr16(dst + 2 * k, (uint16_t)ch);
    }
    RET(std::min<uint32_t>(n, dstLen));
}
WIN32API(kernel32, LCMapStringA, 6) { lcmap<1>(c); }
WIN32API(kernel32, LCMapStringW, 6) { lcmap<2>(c); }

template <int W>
static void compareString(Cpu& c)
{
    const uint32_t flags = ARG(1), a = ARG(2), b = ARG(4);
    int32_t na = (int32_t)ARG(3), nb = (int32_t)ARG(5);
    auto get = [&](uint32_t s, int32_t k) -> uint32_t { return W == 1 ? rd8(s + k) : rd16(s + 2 * k); };
    auto len = [&](uint32_t s) { int32_t n = 0; while (get(s, n)) n++; return n; };
    if (na < 0) na = len(a);
    if (nb < 0) nb = len(b);
    for (int32_t k = 0; k < std::min(na, nb); k++)
    {
        uint32_t x = get(a, k), y = get(b, k);
        if ((flags & 1) && x < 128 && y < 128)
        {
            x = (uint32_t)tolower((int)x);
            y = (uint32_t)tolower((int)y);
        }
        if (x != y)
            RET(x < y ? 1 : 3);
    }
    RET(na == nb ? 2 : na < nb ? 1 : 3);
}
WIN32API(kernel32, CompareStringA, 6) { compareString<1>(c); }
WIN32API(kernel32, CompareStringW, 6) { compareString<2>(c); }

WIN32API(kernel32, lstrlenA, 1) { RET(ARG(0) ? strlen(gstr(ARG(0))) : 0); }
WIN32API(kernel32, FormatMessageA, 7)
{
    const char* msg = "error";
    const uint32_t flags = ARG(0), buf = ARG(4);
    if (flags & 0x100) // FORMAT_MESSAGE_ALLOCATE_BUFFER
        wr32(buf, guestStrdup(msg));
    else
        strcpy(gstr(buf), msg);
    RET(strlen(msg));
}
WIN32API(kernel32, LocalFree, 1)
{
    heapFree(ARG(0));
    RET(0);
}
WIN32API(kernel32, IsBadReadPtr, 2) { RET(ARG(0) < 0x10000); }
WIN32API(kernel32, IsBadWritePtr, 2) { RET(ARG(0) < 0x10000); }

WIN32API(kernel32, IsProcessorFeaturePresent, 1) { RET(ARG(0) == 6 || ARG(0) == 3 ? 1 : 0); } // MMX, CMPXCHG8B... conservative
WIN32API(kernel32, IsDebuggerPresent, 0) { RET(0); }
WIN32API(kernel32, GetSystemInfo, 1)
{
    const uint32_t p = ARG(0);
    memset(g_mem + p, 0, 36);
    wr32(p + 4, 0x1000);
    wr32(p + 8, 0x10000);
    wr32(p + 12, 0x7FFEFFFF);
    wr32(p + 20, 1);
    wr32(p + 24, 586);
    wr32(p + 28, 0x10000);
    RET(0);
}
WIN32API(kernel32, QueryPerformanceFrequency, 1)
{
    wr64(ARG(0), 1000000);
    RET(1);
}
WIN32API(kernel32, QueryPerformanceCounter, 1)
{
    wr64(ARG(0), (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - g_t0).count());
    RET(1);
}
