// x86 string instructions (movs/stos/lods/scas/cmps) with rep prefixes; see cpu.h.
#include "cpu.h"

thread_local StrCmp t_strCmp;

namespace
{
    template <int N>
    uint32_t rdN(uint32_t a) { return N == 1 ? rd8(a) : N == 2 ? rd16(a) : rd32(a); }
    template <int N>
    void wrN(uint32_t a, uint32_t v)
    {
        if (N == 1) wr8(a, (uint8_t)v);
        else if (N == 2) wr16(a, (uint16_t)v);
        else wr32(a, v);
    }
    template <int N>
    uint32_t acc(const Cpu& c) { return N == 4 ? c.r[0] : c.r[0] & ((1u << (8 * N)) - 1); }
    template <int N>
    void setAcc(Cpu& c, uint32_t v)
    {
        if (N == 4) c.r[0] = v;
        else c.r[0] = (c.r[0] & ~((1u << (8 * N)) - 1)) | v;
    }
    inline uint32_t step(const Cpu& c, int n) { return c.df ? (uint32_t)-n : (uint32_t)n; }

    template <int N>
    void movs(Cpu& c, int rep)
    {
        const uint32_t d = step(c, N);
        if (!rep) { wrN<N>(c.r[7], rdN<N>(c.r[6])); c.r[6] += d; c.r[7] += d; return; }
        if (!c.df && N == 1 && c.r[1] && (c.r[7] >= c.r[6] + c.r[1] || c.r[7] + c.r[1] <= c.r[6]))
        {
            memcpy(g_mem + c.r[7], g_mem + c.r[6], c.r[1]);
            c.r[6] += c.r[1]; c.r[7] += c.r[1]; c.r[1] = 0;
            return;
        }
        if (!c.df && N == 4 && c.r[1] && (c.r[7] >= c.r[6] + 4 * c.r[1] || c.r[7] + 4 * c.r[1] <= c.r[6]))
        {
            memcpy(g_mem + c.r[7], g_mem + c.r[6], 4 * (size_t)c.r[1]);
            c.r[6] += 4 * c.r[1]; c.r[7] += 4 * c.r[1]; c.r[1] = 0;
            return;
        }
        for (; c.r[1]; c.r[1]--) { wrN<N>(c.r[7], rdN<N>(c.r[6])); c.r[6] += d; c.r[7] += d; }
    }
    template <int N>
    void stos(Cpu& c, int rep)
    {
        const uint32_t d = step(c, N);
        if (!rep) { wrN<N>(c.r[7], acc<N>(c)); c.r[7] += d; return; }
        if (!c.df && N == 1 && c.r[1])
        {
            memset(g_mem + c.r[7], (int)(c.r[0] & 0xFF), c.r[1]);
            c.r[7] += c.r[1]; c.r[1] = 0;
            return;
        }
        for (; c.r[1]; c.r[1]--) { wrN<N>(c.r[7], acc<N>(c)); c.r[7] += d; }
    }
    template <int N>
    void lods(Cpu& c, int rep)
    {
        const uint32_t d = step(c, N);
        if (!rep) { setAcc<N>(c, rdN<N>(c.r[6])); c.r[6] += d; return; }
        for (; c.r[1]; c.r[1]--) { setAcc<N>(c, rdN<N>(c.r[6])); c.r[6] += d; }
    }
    template <int N>
    bool scas(Cpu& c, int rep)
    {
        const uint32_t d = step(c, N);
        if (!rep) { t_strCmp = { acc<N>(c), rdN<N>(c.r[7]), N }; c.r[7] += d; return true; }
        bool any = false;
        while (c.r[1])
        {
            t_strCmp = { acc<N>(c), rdN<N>(c.r[7]), N };
            any = true;
            c.r[7] += d;
            c.r[1]--;
            const bool eq = t_strCmp.d == t_strCmp.s;
            if (rep == 1 ? !eq : eq) break;
        }
        return any;
    }
    template <int N>
    bool cmps(Cpu& c, int rep)
    {
        const uint32_t d = step(c, N);
        if (!rep) { t_strCmp = { rdN<N>(c.r[6]), rdN<N>(c.r[7]), N }; c.r[6] += d; c.r[7] += d; return true; }
        bool any = false;
        while (c.r[1])
        {
            t_strCmp = { rdN<N>(c.r[6]), rdN<N>(c.r[7]), N };
            any = true;
            c.r[6] += d;
            c.r[7] += d;
            c.r[1]--;
            const bool eq = t_strCmp.d == t_strCmp.s;
            if (rep == 1 ? !eq : eq) break;
        }
        return any;
    }
}

#define STR_DEF(kind, bits, ret) \
    ret str_##kind##bits(Cpu& c, int rep) { return kind<bits / 8>(c, rep); }
STR_DEF(movs, 8, void) STR_DEF(movs, 16, void) STR_DEF(movs, 32, void)
STR_DEF(stos, 8, void) STR_DEF(stos, 16, void) STR_DEF(stos, 32, void)
STR_DEF(lods, 8, void) STR_DEF(lods, 16, void) STR_DEF(lods, 32, void)
STR_DEF(scas, 8, bool) STR_DEF(scas, 16, bool) STR_DEF(scas, 32, bool)
STR_DEF(cmps, 8, bool) STR_DEF(cmps, 16, bool) STR_DEF(cmps, 32, bool)
