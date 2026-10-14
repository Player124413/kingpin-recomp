// Modules: guest PE images (mapped at their preferred base; code is recompiled ahead of time)
// and pseudo modules for host-implemented system DLLs.
#include <cstdio>
#include <fstream>
#include <map>
#include <mutex>
#include <vector>

#include "hle.h"

namespace
{
    struct Module
    {
        std::string name;     // lower case file name, e.g. "ref_gl.dll"
        std::string hostFile; // empty for pseudo modules
        std::string guestPath;
        uint32_t base = 0, size = 0, entry = 0;
        bool image = false, loaded = false;
        int refs = 0;
        std::map<std::string, uint32_t> exports;
    };
    std::recursive_mutex g_modMutex;
    std::vector<Module> g_modules;

    // DLLs implemented on the host; LoadLibrary returns a pseudo handle for them.
    const char* kPseudo[] = { "kernel32.dll", "user32.dll", "gdi32.dll", "winmm.dll", "wsock32.dll",
                              "advapi32.dll", "opengl32.dll", "ntdll.dll", "ddraw.dll", "dsound.dll" };

    std::string lower(std::string s)
    {
        for (auto& ch : s)
            ch = (char)tolower(ch);
        return s;
    }

    std::string baseName(const std::string& path)
    {
        const size_t p = path.find_last_of("/\\:");
        std::string n = lower(p == std::string::npos ? path : path.substr(p + 1));
        if (n.find('.') == std::string::npos)
            n += ".dll";
        return n;
    }

    std::string ordinalName(const std::string& dll, uint32_t ord, const std::string& fallback)
    {
        static const std::map<uint32_t, const char*> wsock = {
            { 1, "accept" }, { 2, "bind" }, { 3, "closesocket" }, { 4, "connect" }, { 5, "getpeername" },
            { 6, "getsockname" }, { 7, "getsockopt" }, { 8, "htonl" }, { 9, "htons" }, { 10, "inet_addr" },
            { 11, "inet_ntoa" }, { 12, "ioctlsocket" }, { 13, "listen" }, { 14, "ntohl" }, { 15, "ntohs" },
            { 16, "recv" }, { 17, "recvfrom" }, { 18, "select" }, { 19, "send" }, { 20, "sendto" },
            { 21, "setsockopt" }, { 22, "shutdown" }, { 23, "socket" }, { 51, "gethostbyaddr" },
            { 52, "gethostbyname" }, { 57, "gethostname" }, { 111, "WSAGetLastError" }, { 115, "WSAStartup" },
            { 116, "WSACleanup" } };
        if (dll == "wsock32.dll")
            if (auto it = wsock.find(ord); it != wsock.end())
                return it->second;
        return fallback;
    }

    bool mapImage(Module& m)
    {
        std::ifstream f(m.hostFile, std::ios::binary);
        if (!f)
            return false;
        std::vector<uint8_t> file((std::istreambuf_iterator<char>(f)), {});
        if (file.size() < 0x200 || file[0] != 'M' || file[1] != 'Z')
        {
            logf("[mod] %s (%s) is not a PE image", m.name.c_str(), m.hostFile.c_str());
            return false;
        }
        auto u16 = [&](size_t o) { uint16_t v; memcpy(&v, &file[o], 2); return v; };
        auto u32 = [&](size_t o) { uint32_t v; memcpy(&v, &file[o], 4); return v; };
        const uint32_t pe = u32(0x3C);
        const uint32_t nsec = u16(pe + 6), optSize = u16(pe + 20);
        const uint32_t opt = pe + 24;
        m.base = u32(opt + 28);
        m.size = u32(opt + 56);
        m.entry = u32(opt + 16) ? m.base + u32(opt + 16) : 0;
        const uint32_t headers = u32(opt + 60);
        memset(g_mem + m.base, 0, m.size);
        memcpy(g_mem + m.base, file.data(), std::min<size_t>(headers, file.size()));
        for (uint32_t s = 0; s < nsec; s++)
        {
            const uint32_t sh = opt + optSize + 40 * s;
            const uint32_t vsize = u32(sh + 8), va = u32(sh + 12), rawSize = u32(sh + 16), raw = u32(sh + 20);
            const uint32_t n = std::min(rawSize, vsize ? vsize : rawSize);
            if (raw && n)
                memcpy(g_mem + m.base + va, &file[raw], std::min<size_t>(n, file.size() - raw));
        }
        // Exports
        const uint32_t expRva = u32(opt + 96);
        if (expRva)
        {
            const uint32_t e = m.base + expRva;
            const uint32_t nNames = rd32(e + 24), funcs = m.base + rd32(e + 28), names = m.base + rd32(e + 32),
                           ords = m.base + rd32(e + 36);
            for (uint32_t k = 0; k < nNames; k++)
            {
                const std::string nm = gstr(m.base + rd32(names + 4 * k));
                m.exports[nm] = m.base + rd32(funcs + 4 * rd16(ords + 2 * k));
            }
        }
        // Imports: every IAT slot gets a host thunk (or a guest export).
        const uint32_t impRva = u32(opt + 104);
        for (uint32_t d = impRva ? m.base + impRva : 0; d && rd32(d + 12); d += 20)
        {
            const std::string dll = lower(gstr(m.base + rd32(d + 12)));
            const uint32_t ilt = rd32(d) ? m.base + rd32(d) : m.base + rd32(d + 16);
            const uint32_t iat = m.base + rd32(d + 16);
            for (uint32_t k = 0; rd32(ilt + 4 * k); k++)
            {
                const uint32_t v = rd32(ilt + 4 * k);
                std::string fn = (v & 0x80000000u) ? "#" + std::to_string(v & 0xFFFF) : std::string(gstr(m.base + v + 2));
                if (v & 0x80000000u)
                    fn = ordinalName(dll, v & 0xFFFF, fn);
                wr32(iat + 4 * k, hleThunk(dll, fn));
            }
        }
        logf("[mod] mapped %s at %08X (%u bytes, entry %08X, %zu exports)", m.name.c_str(), m.base, m.size, m.entry,
             m.exports.size());
        return true;
    }

    Module* find(const std::string& name)
    {
        for (auto& m : g_modules)
            if (m.name == name)
                return &m;
        return nullptr;
    }
}

// Unloads every module and clears their images (before another program is loaded).
void modulesReset()
{
    std::lock_guard lock(g_modMutex);
    for (const Module& m : g_modules)
        if (m.image && m.size)
            memset(g_mem + m.base, 0, m.size);
    g_modules.clear();
}

void modulesInit(const std::string& exePath)
{
    std::lock_guard lock(g_modMutex);
    for (const char* p : kPseudo)
    {
        Module m;
        m.name = p;
        m.base = 0x7E000000u + 0x10000u * (uint32_t)g_modules.size();
        m.loaded = true;
        g_modules.push_back(m);
    }
    Module exe;
    exe.name = baseName(exePath);
    exe.hostFile = exePath;
    exe.guestPath = "C:\\KINGPIN\\" + exe.name;
    exe.image = true;
    if (!mapImage(exe))
    {
        logf("cannot load %s", exePath.c_str());
        exit(1);
    }
    exe.loaded = true;
    exe.refs = 1;
    g_modules.push_back(exe);
}

uint32_t exeEntry()
{
    std::lock_guard lock(g_modMutex);
    for (auto& m : g_modules)
        if (m.image && m.base == 0x00400000)
            return m.entry;
    return 0;
}

uint32_t moduleLoad(Cpu& c, const std::string& path)
{
    std::unique_lock lock(g_modMutex);
    const std::string name = baseName(path);
    if (Module* m = find(name))
    {
        if (!m->image || m->loaded)
        {
            m->refs++;
            return m->base;
        }
    }
    const std::string host = hostPath(path);
    logf("[mod] LoadLibrary(%s) -> %s", path.c_str(), host.c_str());
    FILE* f = fopen(host.c_str(), "rb");
    if (!f)
    {
        logf("[mod] LoadLibrary(%s): not found", path.c_str());
        return 0;
    }
    fclose(f);
    Module* m = find(name);
    if (!m)
    {
        g_modules.push_back({});
        m = &g_modules.back();
        m->name = name;
        m->image = true;
    }
    m->hostFile = host;
    m->guestPath = path;
    if (!mapImage(*m)) // remapping restores the image's initial data on every load
        return 0;
    m->loaded = true;
    m->refs = 1;
    const uint32_t base = m->base, entry = m->entry;
    lock.unlock();
    if (entry && !guestCall(c, entry, { base, 1, 0 })) // DLL_PROCESS_ATTACH
    {
        logf("[mod] %s: DllMain failed", name.c_str());
        return 0;
    }
    return base;
}

void moduleFree(Cpu& c, uint32_t base)
{
    std::unique_lock lock(g_modMutex);
    for (auto& m : g_modules)
    {
        if (m.base != base || !m.image || !m.loaded)
            continue;
        if (--m.refs > 0)
            return;
        m.loaded = false;
        const uint32_t entry = m.entry;
        lock.unlock();
        if (entry)
            guestCall(c, entry, { base, 0, 0 }); // DLL_PROCESS_DETACH
        logf("[mod] unloaded %s", m.name.c_str());
        return;
    }
}

uint32_t moduleProc(uint32_t module, const std::string& name)
{
    std::lock_guard lock(g_modMutex);
    for (auto& m : g_modules)
    {
        if (m.base != module)
            continue;
        if (!m.image)
            return hleThunk(m.name, name);
        auto it = m.exports.find(name);
        return it == m.exports.end() ? 0 : it->second;
    }
    return 0;
}

uint32_t moduleHandle(const std::string& name)
{
    std::lock_guard lock(g_modMutex);
    Module* m = find(baseName(name));
    return m && m->loaded ? m->base : 0;
}

std::string moduleFileName(uint32_t module)
{
    std::lock_guard lock(g_modMutex);
    for (auto& m : g_modules)
        if (m.base == module)
            return m.image ? (m.guestPath.find(':') != std::string::npos ? m.guestPath : "C:\\KINGPIN\\" + m.guestPath)
                           : "C:\\WINDOWS\\SYSTEM\\" + m.name;
    return "C:\\KINGPIN\\" KP_EXE_NAME;
}

// ---------------------------------------------------------------- kernel32 module API

// DLLs the game may try to load that we deliberately do not provide (it falls back).
static bool unavailable(const std::string& n)
{
    return n == "dsound.dll" || n == "dinput.dll" || n == "3dfxgl.dll" || n == "a3d.dll" || n == "ddraw.dll";
}

WIN32API(kernel32, LoadLibraryA, 1)
{
    const std::string path = gstr(ARG(0));
    const std::string n = baseName(path);
    if (unavailable(n))
    {
        logf("[mod] LoadLibrary(%s): not provided", path.c_str());
        setLastError(126);
        RET(0);
    }
    const uint32_t h = moduleLoad(c, path);
    logf("[mod] LoadLibrary(%s) = %08X", path.c_str(), h);
    if (!h)
        setLastError(126);
    RET(h);
}

WIN32API(kernel32, FreeLibrary, 1)
{
    moduleFree(c, ARG(0));
    RET(1);
}

WIN32API(kernel32, GetProcAddress, 2)
{
    const uint32_t mod = ARG(0), np = ARG(1);
    const std::string name = np < 0x10000 ? "#" + std::to_string(np) : std::string(gstr(np));
    const uint32_t a = moduleProc(mod, name);
    static const bool trace = getenv("KP_TRACE_GPA") != nullptr;
    if (trace || !a)
        logf("[mod] GetProcAddress(%08X, %s) = %08X", mod, name.c_str(), a);
    RET(a);
}

WIN32API(kernel32, GetModuleHandleA, 1)
{
    if (!ARG(0))
        RET(0x00400000);
    RET(moduleHandle(gstr(ARG(0))));
}

WIN32API(kernel32, GetModuleFileNameA, 3)
{
    const std::string s = moduleFileName(ARG(0) ? ARG(0) : 0x00400000);
    const uint32_t n = std::min<uint32_t>((uint32_t)s.size(), ARG(2) - 1);
    memcpy(gstr(ARG(1)), s.c_str(), n);
    gstr(ARG(1))[n] = 0;
    RET(n);
}
