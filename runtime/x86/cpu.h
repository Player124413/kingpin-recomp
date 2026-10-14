// Guest x86 state and helpers used by recompiled code (see tools/x86recomp.py).
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <setjmp.h>

struct Cpu
{
    uint32_t r[8]; // eax ecx edx ebx esp ebp esi edi
    uint32_t df = 0;
    uint32_t fsbase = 0;
    // x87: st[(top + i) & 7] is ST(i). sw holds C0-C3 (TOP is merged in on fnstsw).
    double st[8] = {};
    uint32_t top = 0;
    uint32_t sw = 0;
    uint32_t cw = 0x027F; // round to nearest, 53-bit precision, exceptions masked
    uint32_t threadIndex = 0;
    uint32_t fl[5] = {}; // CF ZF SF OF PF passed across calls (see tools/x86recomp.py)
};

using X86Fn = void (*)(Cpu&);
struct X86Func
{
    uint32_t addr;
    X86Fn fn;
};
struct X86Import
{
    uint32_t iat;
    const char* dll;
    const char* name;
};
extern const X86Func g_x86Funcs[];
extern const size_t g_x86FuncCount;
extern const X86Import g_x86Imports[];
extern const size_t g_x86ImportCount;

extern uint8_t* g_mem; // 4 GB guest address space

inline uint8_t rd8(uint32_t a) { return g_mem[a]; }
inline uint16_t rd16(uint32_t a) { uint16_t v; memcpy(&v, g_mem + a, 2); return v; }
inline uint32_t rd32(uint32_t a) { uint32_t v; memcpy(&v, g_mem + a, 4); return v; }
inline uint64_t rd64(uint32_t a) { uint64_t v; memcpy(&v, g_mem + a, 8); return v; }
inline void wr8(uint32_t a, uint8_t v) { g_mem[a] = v; }
inline void wr16(uint32_t a, uint16_t v) { memcpy(g_mem + a, &v, 2); }
inline void wr32(uint32_t a, uint32_t v) { memcpy(g_mem + a, &v, 4); }
inline void wr64(uint32_t a, uint64_t v) { memcpy(g_mem + a, &v, 8); }
inline double rdf32(uint32_t a) { float v; memcpy(&v, g_mem + a, 4); return v; }
inline double rdf64(uint32_t a) { double v; memcpy(&v, g_mem + a, 8); return v; }
inline void wrf32(uint32_t a, double v) { const float f = (float)v; memcpy(g_mem + a, &f, 4); }
inline void wrf64(uint32_t a, double v) { memcpy(g_mem + a, &v, 8); }
double rdf80(uint32_t a);
void wrf80(uint32_t a, double v);

inline void push32(Cpu& c, uint32_t v)
{
    c.r[4] -= 4;
    wr32(c.r[4], v);
}
inline uint32_t parity8(uint32_t v) { return !__builtin_parity(v & 0xFF); }

// Calls the guest code at `addr` (recompiled function or HLE import thunk); the caller has
// already pushed the return address.
void call_ind(Cpu& c, uint32_t addr);
[[noreturn]] void trap(Cpu& c, uint32_t addr, const char* what);
void x86_fallthrough(Cpu& c, uint32_t addr);
[[noreturn]] void x86_divide_error(Cpu& c, uint32_t addr);

// ---- x87
#define ST(i) c.st[(c.top + (i)) & 7]
inline void fpush(Cpu& c, double v)
{
    c.top = (c.top - 1) & 7;
    c.st[c.top] = v;
}
inline void fpop(Cpu& c) { c.top = (c.top + 1) & 7; }
inline uint16_t fstsw(Cpu& c) { return (uint16_t)((c.sw & ~0x3800u) | (c.top & 7) << 11); }
inline void fcom(Cpu& c, double a, double b)
{
    c.sw &= ~0x4500u;
    if (a != a || b != b)
        c.sw |= 0x4500u;
    else if (a < b)
        c.sw |= 0x0100u;
    else if (a == b)
        c.sw |= 0x4000u;
}
inline double frnd(Cpu& c, double v)
{
    switch ((c.cw >> 10) & 3)
    {
    case 0: return nearbyint(v); // host default rounding is to nearest even
    case 1: return floor(v);
    case 2: return ceil(v);
    default: return trunc(v);
    }
}
inline int64_t fist64(Cpu& c, double v)
{
    const double r = frnd(c, v);
    if (!(r >= -9223372036854775808.0 && r < 9223372036854775808.0))
        return INT64_MIN;
    return (int64_t)r;
}
inline int32_t fist32(Cpu& c, double v)
{
    const double r = frnd(c, v);
    if (!(r >= -2147483648.0 && r <= 2147483647.0))
        return INT32_MIN;
    return (int32_t)r;
}
inline int16_t fist16(Cpu& c, double v)
{
    const double r = frnd(c, v);
    if (!(r >= -32768.0 && r <= 32767.0))
        return INT16_MIN;
    return (int16_t)r;
}
void fxam(Cpu& c);
void fprem(Cpu& c, bool ieee); // fprem1 when ieee
void finit(Cpu& c);
void fldenv(Cpu& c, uint32_t a);
void fstenv(Cpu& c, uint32_t a);
void fsave(Cpu& c, uint32_t a);
void frstor(Cpu& c, uint32_t a);

// ---- string instructions; rep: 0 none, 1 rep/repe, 2 repne. Flag results of the last
// scas/cmps comparison are kept in Cpu-independent thread storage and read by str_flags.
struct StrCmp
{
    uint32_t d, s, size;
};
extern thread_local StrCmp t_strCmp;
// scas/cmps return whether a comparison happened (rep with ecx = 0 leaves the flags alone).
#define STR_DECL(kind, bits, ret) ret str_##kind##bits(Cpu& c, int rep);
STR_DECL(movs, 8, void) STR_DECL(movs, 16, void) STR_DECL(movs, 32, void)
STR_DECL(stos, 8, void) STR_DECL(stos, 16, void) STR_DECL(stos, 32, void)
STR_DECL(lods, 8, void) STR_DECL(lods, 16, void) STR_DECL(lods, 32, void)
STR_DECL(scas, 8, bool) STR_DECL(scas, 16, bool) STR_DECL(scas, 32, bool)
STR_DECL(cmps, 8, bool) STR_DECL(cmps, 16, bool) STR_DECL(cmps, 32, bool)
#undef STR_DECL
template <int Bits>
inline void str_flags(uint32_t& cf, uint32_t& zf, uint32_t& sf, uint32_t& of, uint32_t& pf)
{
    const uint32_t m = Bits == 32 ? 0xFFFFFFFFu : (1u << Bits) - 1;
    const uint32_t d = t_strCmp.d & m, s = t_strCmp.s & m, r = (d - s) & m;
    cf = d < s;
    zf = r == 0;
    sf = r >> (Bits - 1) & 1;
    of = ((d ^ s) & (d ^ r)) >> (Bits - 1) & 1;
    pf = parity8(r);
}
#define str_flags8(c, cf, zf, sf, of, pf) str_flags<8>(cf, zf, sf, of, pf)
#define str_flags16(c, cf, zf, sf, of, pf) str_flags<16>(cf, zf, sf, of, pf)
#define str_flags32(c, cf, zf, sf, of, pf) str_flags<32>(cf, zf, sf, of, pf)

// setjmp/longjmp of the guest C runtime (see OVERRIDE_KINDS in tools/x86recomp.py).
jmp_buf* kp_jmpbuf(uint32_t guestBuf);
void kp_setjmp_save(Cpu& c, uint32_t guestBuf);
void kp_setjmp_restore(Cpu& c, uint32_t guestBuf);
[[noreturn]] void kp_longjmp(Cpu& c);
