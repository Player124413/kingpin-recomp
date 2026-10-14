// x87 helpers that are too large to inline (see cpu.h).
#include "cpu.h"

double rdf80(uint32_t a)
{
    uint64_t mant;
    uint16_t se;
    memcpy(&mant, g_mem + a, 8);
    memcpy(&se, g_mem + a + 8, 2);
    const int exp = se & 0x7FFF;
    const bool neg = se & 0x8000;
    double v;
    if (exp == 0 && mant == 0)
        v = 0.0;
    else if (exp == 0x7FFF)
        v = (mant << 1) ? NAN : INFINITY;
    else
        v = ldexp((double)mant, exp - 16383 - 63);
    return neg ? -v : v;
}

void wrf80(uint32_t a, double v)
{
    uint64_t mant = 0;
    uint16_t se = std::signbit(v) ? 0x8000 : 0;
    v = fabs(v);
    if (v != v)
    {
        mant = 0xC000000000000000ull;
        se |= 0x7FFF;
    }
    else if (std::isinf(v))
    {
        mant = 0x8000000000000000ull;
        se |= 0x7FFF;
    }
    else if (v != 0.0)
    {
        int e;
        const double m = frexp(v, &e); // v = m * 2^e, m in [0.5, 1)
        mant = (uint64_t)ldexp(m, 64);
        se |= (uint16_t)(e - 1 + 16383);
    }
    memcpy(g_mem + a, &mant, 8);
    memcpy(g_mem + a + 8, &se, 2);
}

void fxam(Cpu& c)
{
    const double v = ST(0);
    uint32_t cc;
    if (v != v)
        cc = 0x0100; // NaN
    else if (std::isinf(v))
        cc = 0x0500;
    else if (v == 0.0)
        cc = 0x4000;
    else if (std::fpclassify(v) == FP_SUBNORMAL)
        cc = 0x4400;
    else
        cc = 0x0400;
    c.sw = (c.sw & ~0x4700u) | cc | (std::signbit(v) ? 0x0200u : 0u);
}

void finit(Cpu& c)
{
    c.cw = 0x037F;
    c.sw = 0;
    c.top = 0;
}

void fstenv(Cpu& c, uint32_t a)
{
    memset(g_mem + a, 0, 28);
    wr32(a, c.cw | 0xFFFF0000u);
    wr32(a + 4, fstsw(c) | 0xFFFF0000u);
    wr32(a + 8, 0xFFFFFFFFu); // tag word: report all empty
}

void fldenv(Cpu& c, uint32_t a)
{
    c.cw = rd16(a);
    const uint32_t sw = rd16(a + 4);
    c.sw = sw & 0x4700u;
    c.top = (sw >> 11) & 7;
}

void fsave(Cpu& c, uint32_t a)
{
    fstenv(c, a);
    for (int i = 0; i < 8; i++)
        wrf80(a + 28 + 10 * i, ST(i));
    finit(c);
}

void frstor(Cpu& c, uint32_t a)
{
    fldenv(c, a);
    for (int i = 0; i < 8; i++)
        ST(i) = rdf80(a + 28 + 10 * i);
}

void fprem(Cpu& c, bool ieee)
{
    const double a = ST(0), b = ST(1);
    const double q = ieee ? nearbyint(a / b) : trunc(a / b);
    ST(0) = ieee ? remainder(a, b) : fmod(a, b);
    const uint64_t qi = (uint64_t)fabs(q);
    // C2 = 0 (complete); C0 = Q2, C3 = Q1, C1 = Q0
    c.sw = (c.sw & ~0x4700u) | (qi & 4 ? 0x0100u : 0) | (qi & 2 ? 0x4000u : 0) | (qi & 1 ? 0x0200u : 0);
}
