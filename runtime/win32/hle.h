// Win32 high-level emulation: registry of host implementations of imported functions.
//
// An import resolves to a thunk address (kThunkBase + 16 * index). call_ind() recognizes thunk
// addresses and runs the host function, which reads its stdcall arguments from the guest
// stack (ARG(n)), sets eax, and is popped by the dispatcher (return address + argBytes).
#pragma once

#include <cstdarg>
#include <cstdint>
#include <string>

#include "../x86/cpu.h"

constexpr uint32_t kThunkBase = 0xF0000000u;
constexpr uint32_t kReturnMagic = 0xEFFFFFF0u; // return address of host->guest calls

using HleFn = void (*)(Cpu&);
struct HleFunc
{
    std::string dll, name;
    HleFn fn;
    uint32_t argBytes; // popped after the return address (stdcall); 0 for cdecl
};

bool hleRegister(const char* dll, const char* name, HleFn fn, uint32_t argBytes);
// Thunk address for dll!name (creating a trapping stub if it is not implemented).
uint32_t hleThunk(const std::string& dll, const std::string& name);
const HleFunc* hleAt(uint32_t addr);

#define WIN32API(dll, name, nargs)                                                              \
    static void hle_##dll##_##name(Cpu& c);                                                     \
    [[maybe_unused]] static const bool reg_##dll##_##name = hleRegister(#dll, #name, hle_##dll##_##name, (nargs) * 4); \
    static void hle_##dll##_##name(Cpu& c)
#define ARG(n) rd32(c.r[4] + 4 + 4 * (n))
#define ARGF(n) ([&] { uint32_t v_ = ARG(n); float f_; memcpy(&f_, &v_, 4); return f_; }())
#define RET(v)                       \
    do                               \
    {                                \
        c.r[0] = (uint32_t)(v);      \
        return;                      \
    } while (0)

inline char* gstr(uint32_t a) { return a ? reinterpret_cast<char*>(g_mem + a) : nullptr; }
template <typename T>
inline T* gptr(uint32_t a) { return a ? reinterpret_cast<T*>(g_mem + a) : nullptr; }

// Calls guest code at `fn` with 32-bit stack arguments; returns eax. Works for both stdcall
// and cdecl callees (esp is restored).
uint32_t guestCall(Cpu& c, uint32_t fn, std::initializer_list<uint32_t> args);

// Logging (stderr, with a timestamp).
void logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// Guest memory allocation (thread safe). Heap blocks are 16-byte aligned.
uint32_t heapAlloc(uint32_t size, bool zero = false);
void heapFree(uint32_t addr);
uint32_t heapSize(uint32_t addr);
uint32_t heapRealloc(uint32_t addr, uint32_t size);
uint32_t vaReserve(uint32_t size, uint32_t want = 0);
bool vaRelease(uint32_t addr);
uint32_t guestStrdup(const std::string& s);

// Win32 last error (per guest thread).
void setLastError(uint32_t e);
uint32_t getLastError();

// Modules: guest PE images and pseudo modules (host-implemented DLLs).
uint32_t moduleLoad(Cpu& c, const std::string& name); // 0 when unavailable
uint32_t moduleProc(uint32_t module, const std::string& name);
uint32_t moduleHandle(const std::string& name);
std::string moduleFileName(uint32_t module);

// Paths: guest (Windows) path -> host path.
std::string hostPath(const std::string& guestPath);
extern std::string g_gameDir; // host directory holding kingpin.exe
extern std::string g_discDir; // the CD (D:\): a host directory or the data track .iso image
extern std::string g_commandLine;

// ExitProcess while another program follows in the same process (the intro movie player on
// platforms without subprocesses): unwinds the guest to platform/app.cpp instead of exiting.
struct ProgramExit
{
    int code;
};
extern bool g_exitUnwinds;

// Threads
struct GuestThread;
GuestThread* currentGuestThread();
uint32_t guestThreadId();
