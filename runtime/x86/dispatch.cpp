// Guest memory, the recompiled function table, indirect calls and the HLE thunk registry.
#if !defined(__ANDROID__)
#include <execinfo.h>
#else
#include <android/log.h>
#endif
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../win32/hle.h"
#include "cpu.h"

uint8_t* g_mem;

namespace
{
    // Flat lookup per module text range.
    struct Range
    {
        uint32_t lo, hi;
        std::vector<X86Fn> fns;
    };
    std::vector<Range> g_ranges;
    std::vector<HleFunc>& hleTable()
    {
        static std::vector<HleFunc> t;
        return t;
    }
    std::mutex g_hleMutex;
    std::unordered_map<std::string, uint32_t>& hleIndex()
    {
        static std::unordered_map<std::string, uint32_t> m;
        return m;
    }
    const auto g_start = std::chrono::steady_clock::now();

    std::string key(const std::string& dll, const std::string& name)
    {
        std::string k = dll;
        for (auto& ch : k)
            ch = (char)tolower(ch);
        if (k.size() > 4 && k.compare(k.size() - 4, 4, ".dll") == 0)
            k.resize(k.size() - 4);
        return k + "!" + name;
    }

    std::string& unimplementedName()
    {
        thread_local std::string s;
        return s;
    }
}

void logf(const char* fmt, ...)
{
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fprintf(stderr, "[%8.3f] %s\n", t, buf);
#if defined(__ANDROID__)
    __android_log_print(ANDROID_LOG_INFO, "kingpin", "[%8.3f] %s", t, buf);
#endif
}

void memInit()
{
    g_mem = (uint8_t*)mmap(nullptr, 1ull << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (g_mem == MAP_FAILED)
    {
        perror("mmap guest memory");
        exit(1);
    }
    // Catch null pointer accesses.
    mprotect(g_mem, 0x10000, PROT_NONE);
}

void funcTableInit(const X86Func* funcs, size_t count);
void funcTableInit() { funcTableInit(g_x86Funcs, g_x86FuncCount); }

// Indexes a program's recompiled functions (replacing the previous program's, and any hooks).
void funcTableInit(const X86Func* funcs, size_t count)
{
    g_ranges.clear();
    // Group functions by module (images are 0x10000000 apart at most; split on gaps > 16 MB).
    std::vector<X86Func> fs(funcs, funcs + count);
    size_t k = 0;
    while (k < fs.size())
    {
        size_t e = k;
        while (e + 1 < fs.size() && fs[e + 1].addr - fs[e].addr < 0x1000000)
            e++;
        Range r{ fs[k].addr, fs[e].addr + 1, {} };
        r.fns.assign(r.hi - r.lo, nullptr);
        for (size_t j = k; j <= e; j++)
            r.fns[fs[j].addr - r.lo] = fs[j].fn;
        g_ranges.push_back(std::move(r));
        k = e + 1;
    }
    logf("[x86] %zu recompiled functions in %zu ranges", count, g_ranges.size());
}

// Replaces the recompiled function at addr (for indirect calls) and returns the original.
X86Fn x86HookInstall(uint32_t addr, X86Fn hook)
{
    for (auto& r : g_ranges)
        if (addr >= r.lo && addr < r.hi && r.fns[addr - r.lo])
        {
            X86Fn orig = r.fns[addr - r.lo];
            r.fns[addr - r.lo] = hook;
            return orig;
        }
    return nullptr;
}

X86Fn x86Lookup(uint32_t addr)
{
    for (auto& r : g_ranges)
        if (addr >= r.lo && addr < r.hi)
            return r.fns[addr - r.lo];
    return nullptr;
}

bool hleRegister(const char* dll, const char* name, HleFn fn, uint32_t argBytes)
{
    std::lock_guard lock(g_hleMutex);
    const std::string k = key(dll, name);
    auto it = hleIndex().find(k);
    if (it != hleIndex().end())
    {
        hleTable()[it->second].fn = fn;
        hleTable()[it->second].argBytes = argBytes;
        return true;
    }
    hleIndex()[k] = (uint32_t)hleTable().size();
    hleTable().push_back({ dll, name, fn, argBytes });
    return true;
}

static void hleUnimplemented(Cpu& c)
{
    trap(c, 0, ("unimplemented import " + unimplementedName()).c_str());
}

uint32_t hleThunk(const std::string& dll, const std::string& name)
{
    std::lock_guard lock(g_hleMutex);
    const std::string k = key(dll, name);
    auto it = hleIndex().find(k);
    uint32_t idx;
    if (it != hleIndex().end())
        idx = it->second;
    else
    {
        idx = (uint32_t)hleTable().size();
        hleIndex()[k] = idx;
        hleTable().push_back({ dll, name, nullptr, 0 });
    }
    return kThunkBase + 16 * idx;
}

const HleFunc* hleAt(uint32_t addr)
{
    if (addr < kThunkBase || (addr - kThunkBase) % 16)
        return nullptr;
    const uint32_t idx = (addr - kThunkBase) / 16;
    std::lock_guard lock(g_hleMutex);
    return idx < hleTable().size() ? &hleTable()[idx] : nullptr;
}

static bool traceHle() // read at first use: platforms may set options after static initialisation
{
    static const bool on = getenv("KP_TRACE_HLE") != nullptr;
    return on;
}

void call_ind(Cpu& c, uint32_t addr)
{
    if (X86Fn f = x86Lookup(addr))
    {
        f(c);
        return;
    }
    if (const HleFunc* h = hleAt(addr))
    {
        if (!h->fn)
        {
            unimplementedName() = h->dll + "!" + h->name;
            hleUnimplemented(c);
        }
        if (traceHle())
            logf("[hle] %s!%s(%08X, %08X, %08X, %08X) from %08X", h->dll.c_str(), h->name.c_str(), rd32(c.r[4] + 4),
                 rd32(c.r[4] + 8), rd32(c.r[4] + 12), rd32(c.r[4] + 16), rd32(c.r[4]));
        static const char* usageFile = getenv("KP_GL_USAGE");
        if (usageFile)
        {
            // Record each HLE function the first time it is called (to size platform layers).
            static std::mutex m;
            static std::unordered_set<const HleFunc*> seen;
            std::lock_guard lock(m);
            if (seen.insert(h).second)
                if (FILE* f = fopen(usageFile, "a"))
                {
                    fprintf(f, "%s!%s\n", h->dll.c_str(), h->name.c_str());
                    fclose(f);
                }
        }
        const uint32_t pop = h->argBytes;
        h->fn(c);
        c.r[4] += 4 + pop;
        return;
    }
    trap(c, addr, "call to code that was not recompiled (add it to tools/seeds.txt)");
}

void x86_fallthrough(Cpu& c, uint32_t addr)
{
    call_ind(c, addr);
}

uint32_t guestCall(Cpu& c, uint32_t fn, std::initializer_list<uint32_t> args)
{
    const uint32_t esp = c.r[4];
    const uint32_t saved[3] = { c.r[3], c.r[6], c.r[7] };
    const uint32_t ebp = c.r[5];
    for (auto it = std::rbegin(args); it != std::rend(args); ++it)
        push32(c, *it);
    push32(c, kReturnMagic);
    call_ind(c, fn);
    const uint32_t ret = c.r[0];
    c.r[4] = esp;
    c.r[3] = saved[0];
    c.r[6] = saved[1];
    c.r[7] = saved[2];
    c.r[5] = ebp;
    return ret;
}

static void dumpRegs(Cpu& c)
{
    logf("  eax=%08X ecx=%08X edx=%08X ebx=%08X esp=%08X ebp=%08X esi=%08X edi=%08X", c.r[0], c.r[1], c.r[2], c.r[3],
         c.r[4], c.r[5], c.r[6], c.r[7]);
    std::string st = "  stack:";
    for (int k = 0; k < 16; k++)
    {
        char b[16];
        snprintf(b, sizeof b, " %08X", rd32(c.r[4] + 4 * k));
        st += b;
    }
    logf("%s", st.c_str());
    // Return addresses on the stack (values that point into recompiled code)
    std::string bt = "  code addresses on stack:";
    for (uint32_t k = 0, n = 0; k < 4096 && n < 24; k++)
    {
        const uint32_t v = rd32(c.r[4] + 4 * k);
        bool code = false;
        for (auto& r : g_ranges)
            code |= v >= r.lo && v < r.hi + 0x1000;
        if (code)
        {
            char b[16];
            snprintf(b, sizeof b, " %08X", v);
            bt += b;
            n++;
        }
    }
    logf("%s", bt.c_str());
}

void trap(Cpu& c, uint32_t addr, const char* what)
{
    logf("[x86] TRAP at %08X: %s", addr, what);
    dumpRegs(c);
    if (FILE* f = fopen("kp_missing.txt", "a"))
    {
        if (strstr(what, "not recompiled"))
            fprintf(f, "%08X\n", addr);
        fclose(f);
    }
    fflush(stderr);
    _exit(3);
}

void x86_divide_error(Cpu& c, uint32_t addr)
{
    trap(c, addr, "integer divide by zero");
}

static void crashHandler(int sig, siginfo_t* info, void*)
{
    const uintptr_t a = (uintptr_t)info->si_addr;
    char buf[256];
    if (a >= (uintptr_t)g_mem && a < (uintptr_t)g_mem + (1ull << 32))
        snprintf(buf, sizeof buf, "[x86] FAULT (signal %d) at guest address %08X\n", sig, (unsigned)(a - (uintptr_t)g_mem));
    else
        snprintf(buf, sizeof buf, "[x86] FAULT (signal %d) at host address %p\n", sig, info->si_addr);
    write(2, buf, strlen(buf));
    void* frames[64];
#if !defined(__ANDROID__)
    const int n = backtrace(frames, 64);
    backtrace_symbols_fd(frames, n, 2);
#else
    (void)frames;
#endif
    _exit(4);
}

void crashHandlerInstall()
{
    struct sigaction sa{};
    sa.sa_sigaction = crashHandler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
    sigaction(SIGILL, &sa, nullptr);
    sigaction(SIGFPE, &sa, nullptr);
}
