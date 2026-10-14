// Guest setjmp/longjmp mapped onto host _setjmp/_longjmp (the recompiled callers' host frames
// are where execution must resume).
#include <map>

#include "../win32/hle.h"
#include "cpu.h"

namespace
{
    thread_local std::map<uint32_t, jmp_buf> t_bufs;
    thread_local uint32_t t_value;
}

jmp_buf* kp_jmpbuf(uint32_t guestBuf) { return &t_bufs[guestBuf]; }

void kp_setjmp_save(Cpu& c, uint32_t jb)
{
    // MSVC layout: ebp, ebx, edi, esi, esp (at entry), return address
    wr32(jb + 0, c.r[5]);
    wr32(jb + 4, c.r[3]);
    wr32(jb + 8, c.r[7]);
    wr32(jb + 12, c.r[6]);
    wr32(jb + 16, c.r[4]);
    wr32(jb + 20, rd32(c.r[4]));
}

void kp_setjmp_restore(Cpu& c, uint32_t jb)
{
    c.r[5] = rd32(jb + 0);
    c.r[3] = rd32(jb + 4);
    c.r[7] = rd32(jb + 8);
    c.r[6] = rd32(jb + 12);
    c.r[4] = rd32(jb + 16) + 4;
    c.r[0] = t_value;
    c.top = 0; // the x87 stack is empty at a setjmp call site
}

void kp_longjmp(Cpu& c)
{
    const uint32_t jb = rd32(c.r[4] + 4), v = rd32(c.r[4] + 8);
    t_value = v ? v : 1;
    auto it = t_bufs.find(jb);
    if (it == t_bufs.end())
        trap(c, 0, "longjmp to a jmp_buf without setjmp");
    _longjmp(it->second, 1);
}
