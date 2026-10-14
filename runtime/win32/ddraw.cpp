// DirectDraw (version 1 interfaces) for the intro movie player.
//
// COM objects live in guest memory: a vtable pointer followed by a host object id. Every vtable
// slot is an HLE thunk named "Interface_Method" (unimplemented ones trap with that name).
// Surfaces are plain guest memory; the primary surface is shown (converted to RGBA) whenever it
// is flipped, blitted to or unlocked.
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "com.h"

void presentRgba(const uint32_t* pixels, int w, int h); // user32.cpp
void setDisplayModeWindow(int w, int h);              // user32.cpp
void requestFullscreen(bool on);                     // user32.cpp

namespace
{
    struct Palette
    {
        uint32_t rgba[256] = {};
    };
    struct Surface
    {
        uint32_t w = 0, h = 0, bpp = 16, pitch = 0, mem = 0;
        bool primary = false;
        uint32_t back = 0;    // attached back buffer (COM pointer)
        uint32_t palette = 0; // COM pointer
        uint32_t colorKey = 0;
        bool hasColorKey = false;
    };

    std::mutex g_ddMutex;
    uint32_t g_modeW = 640, g_modeH = 480, g_modeBpp = 16;
    std::map<uint32_t, Surface> g_surfaces; // by COM pointer
    std::map<uint32_t, Palette> g_palettes;
    uint32_t g_primary = 0;

    uint32_t rgbaOf(const Surface& s, const uint8_t* p, const Palette* pal)
    {
        switch (s.bpp)
        {
        case 8: return pal ? pal->rgba[*p] : (uint32_t)*p * 0x010101u | 0xFF000000u;
        case 16:
        {
            const uint16_t v = (uint16_t)(p[0] | p[1] << 8);
            const uint32_t r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
            return 0xFF000000u | (b * 255 / 31) << 16 | (g * 255 / 63) << 8 | (r * 255 / 31);
        }
        case 15:
        {
            const uint16_t v = (uint16_t)(p[0] | p[1] << 8);
            const uint32_t r = (v >> 10) & 31, g = (v >> 5) & 31, b = v & 31;
            return 0xFF000000u | (b * 255 / 31) << 16 | (g * 255 / 31) << 8 | (r * 255 / 31);
        }
        case 24: return 0xFF000000u | (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
        default: return 0xFF000000u | (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
        }
    }

    void present()
    {
        std::vector<uint32_t> px;
        int w, h;
        {
            std::lock_guard lock(g_ddMutex);
            auto it = g_surfaces.find(g_primary);
            if (it == g_surfaces.end() || !it->second.mem)
                return;
            const Surface& s = it->second;
            const Palette* pal = s.palette && g_palettes.count(s.palette) ? &g_palettes[s.palette] : nullptr;
            w = (int)s.w;
            h = (int)s.h;
            px.resize((size_t)w * h);
            const uint32_t bytes = s.bpp == 15 ? 2 : s.bpp / 8;
            for (int y = 0; y < h; y++)
            {
                const uint8_t* row = g_mem + s.mem + (size_t)y * s.pitch;
                for (int x = 0; x < w; x++)
                    px[(size_t)y * w + x] = rgbaOf(s, row + x * bytes, pal);
            }
        }
        presentRgba(px.data(), w, h);
    }

    uint32_t bytesPerPixel(uint32_t bpp) { return bpp == 15 ? 2 : bpp / 8; }

    uint32_t newSurface(uint32_t w, uint32_t h, uint32_t bpp, bool primary)
    {
        const uint32_t obj = comNew("IDirectDrawSurface");
        Surface s;
        s.w = w;
        s.h = h;
        s.bpp = bpp;
        s.pitch = (w * bytesPerPixel(bpp) + 3) & ~3u;
        s.mem = heapAlloc(s.pitch * h, true);
        s.primary = primary;
        std::lock_guard lock(g_ddMutex);
        g_surfaces[obj] = s;
        return obj;
    }

    void writePixelFormat(uint32_t p, uint32_t bpp)
    {
        memset(g_mem + p, 0, 32);
        wr32(p, 32);
        wr32(p + 4, bpp == 8 ? 0x60 : 0x40); // DDPF_PALETTEINDEXED8 | RGB  /  DDPF_RGB
        wr32(p + 12, bpp == 15 ? 16 : bpp);
        if (bpp == 16)
        {
            wr32(p + 16, 0xF800);
            wr32(p + 20, 0x07E0);
            wr32(p + 24, 0x001F);
        }
        else if (bpp == 15)
        {
            wr32(p + 16, 0x7C00);
            wr32(p + 20, 0x03E0);
            wr32(p + 24, 0x001F);
        }
        else if (bpp >= 24)
        {
            wr32(p + 16, 0xFF0000);
            wr32(p + 20, 0x00FF00);
            wr32(p + 24, 0x0000FF);
        }
    }

    void writeSurfaceDesc(uint32_t d, const Surface& s)
    {
        memset(g_mem + d, 0, 108);
        wr32(d, 108);
        wr32(d + 4, 0x1 | 0x2 | 0x4 | 0x8 | 0x1000 | 0x20); // CAPS HEIGHT WIDTH PITCH PIXELFORMAT LPSURFACE
        wr32(d + 8, s.h);
        wr32(d + 12, s.w);
        wr32(d + 16, s.pitch);
        wr32(d + 36, s.mem);
        writePixelFormat(d + 72, s.bpp);
        wr32(d + 104, s.primary ? 0x200 | 0x4000 : 0x800 | 0x40); // PRIMARY|VIDEOMEMORY : OFFSCREEN|SYSTEMMEMORY
    }

    Surface* surf(uint32_t obj)
    {
        auto it = g_surfaces.find(obj);
        return it == g_surfaces.end() ? nullptr : &it->second;
    }

    struct Rect
    {
        int32_t l, t, r, b;
    };
    Rect rectOr(uint32_t p, const Surface& s)
    {
        if (!p)
            return { 0, 0, (int32_t)s.w, (int32_t)s.h };
        return { (int32_t)rd32(p), (int32_t)rd32(p + 4), (int32_t)rd32(p + 8), (int32_t)rd32(p + 12) };
    }

    void blit(Surface& d, Rect dr, const Surface& s, Rect sr, bool colorKey)
    {
        const uint32_t bpp = bytesPerPixel(d.bpp);
        const int dw = dr.r - dr.l, dh = dr.b - dr.t, sw = sr.r - sr.l, sh = sr.b - sr.t;
        if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0)
            return;
        for (int y = 0; y < dh; y++)
        {
            const int dy = dr.t + y, sy = sr.t + (int)((int64_t)y * sh / dh);
            if (dy < 0 || dy >= (int)d.h || sy < 0 || sy >= (int)s.h)
                continue;
            uint8_t* drow = g_mem + d.mem + (size_t)dy * d.pitch;
            const uint8_t* srow = g_mem + s.mem + (size_t)sy * s.pitch;
            if (dw == sw && !colorKey && dr.l >= 0 && dr.r <= (int)d.w && sr.l >= 0 && sr.r <= (int)s.w)
            {
                memcpy(drow + dr.l * bpp, srow + sr.l * bpp, (size_t)dw * bpp);
                continue;
            }
            for (int x = 0; x < dw; x++)
            {
                const int dx = dr.l + x, sx = sr.l + (int)((int64_t)x * sw / dw);
                if (dx < 0 || dx >= (int)d.w || sx < 0 || sx >= (int)s.w)
                    continue;
                uint32_t v = 0;
                memcpy(&v, srow + sx * bpp, bpp);
                if (colorKey && v == s.colorKey)
                    continue;
                memcpy(drow + dx * bpp, &v, bpp);
            }
        }
    }
}

void ddrawReset()
{
    std::lock_guard lock(g_ddMutex);
    g_surfaces.clear();
    g_palettes.clear();
    g_primary = 0;
}

// ---------------------------------------------------------------- DirectDrawCreate / IDirectDraw

COM_INTERFACE(IDirectDraw, "QueryInterface", "AddRef", "Release", "Compact", "CreateClipper", "CreatePalette",
              "CreateSurface", "DuplicateSurface", "EnumDisplayModes", "EnumSurfaces", "FlipToGDISurface", "GetCaps",
              "GetDisplayMode", "GetFourCCCodes", "GetGDISurface", "GetMonitorFrequency", "GetScanLine",
              "GetVerticalBlankStatus", "Initialize", "RestoreDisplayMode", "SetCooperativeLevel", "SetDisplayMode",
              "WaitForVerticalBlank");
COM_INTERFACE(IDirectDrawSurface, "QueryInterface", "AddRef", "Release", "AddAttachedSurface", "AddOverlayDirtyRect",
              "Blt", "BltBatch", "BltFast", "DeleteAttachedSurface", "EnumAttachedSurfaces", "EnumOverlayZOrders",
              "Flip", "GetAttachedSurface", "GetBltStatus", "GetCaps", "GetClipper", "GetColorKey", "GetDC",
              "GetFlipStatus", "GetOverlayPosition", "GetPalette", "GetPixelFormat", "GetSurfaceDesc", "Initialize",
              "IsLost", "Lock", "ReleaseDC", "Restore", "SetClipper", "SetColorKey", "SetOverlayPosition",
              "SetPalette", "Unlock", "UpdateOverlay", "UpdateOverlayDisplay", "UpdateOverlayZOrder");
COM_INTERFACE(IDirectDrawPalette, "QueryInterface", "AddRef", "Release", "GetCaps", "GetEntries", "Initialize",
              "SetEntries");

WIN32API(ddraw, DirectDrawCreate, 3)
{
    wr32(ARG(1), comNew("IDirectDraw"));
    logf("[ddraw] DirectDrawCreate");
    RET(0);
}

COMMETHOD(IDirectDraw, QueryInterface, 3)
{
    wr32(ARG(2), 0);
    RET(0x80004002u); // E_NOINTERFACE
}
COMMETHOD(IDirectDraw, AddRef, 1) { RET(1); }
COMMETHOD(IDirectDraw, Release, 1) { RET(0); }
COMMETHOD(IDirectDraw, SetCooperativeLevel, 3)
{
    requestFullscreen((ARG(2) & 1) != 0); // DDSCL_FULLSCREEN
    RET(0);
}
COMMETHOD(IDirectDraw, RestoreDisplayMode, 1) { RET(0); }
COMMETHOD(IDirectDraw, FlipToGDISurface, 1) { RET(0); }
COMMETHOD(IDirectDraw, WaitForVerticalBlank, 3) { RET(0); }
COMMETHOD(IDirectDraw, Compact, 1) { RET(0); }
COMMETHOD(IDirectDraw, GetMonitorFrequency, 2)
{
    wr32(ARG(1), 60);
    RET(0);
}
COMMETHOD(IDirectDraw, SetDisplayMode, 4)
{
    g_modeW = ARG(1);
    g_modeH = ARG(2);
    g_modeBpp = ARG(3);
    logf("[ddraw] SetDisplayMode %ux%u %u bpp", g_modeW, g_modeH, g_modeBpp);
    setDisplayModeWindow((int)g_modeW, (int)g_modeH);
    RET(0);
}
COMMETHOD(IDirectDraw, GetDisplayMode, 2)
{
    Surface s;
    s.w = g_modeW;
    s.h = g_modeH;
    s.bpp = g_modeBpp;
    s.pitch = g_modeW * bytesPerPixel(g_modeBpp);
    writeSurfaceDesc(ARG(1), s);
    RET(0);
}
COMMETHOD(IDirectDraw, GetCaps, 3)
{
    for (int k = 1; k <= 2; k++)
        if (ARG(k))
        {
            const uint32_t size = rd32(ARG(k));
            memset(g_mem + ARG(k), 0, size ? size : 316);
            wr32(ARG(k), size ? size : 316);
            wr32(ARG(k) + 4, 0x40 | 0x40000); // DDCAPS_BLT | BLTSTRETCH-ish
        }
    RET(0);
}
COMMETHOD(IDirectDraw, EnumDisplayModes, 5)
{
    const uint32_t cb = ARG(4), ctx = ARG(3);
    const uint32_t d = heapAlloc(108, true);
    static const uint32_t modes[][3] = { { 640, 480, 8 }, { 640, 480, 16 }, { 640, 480, 32 }, { 800, 600, 16 } };
    for (auto& m : modes)
    {
        Surface s;
        s.w = m[0];
        s.h = m[1];
        s.bpp = m[2];
        s.pitch = s.w * bytesPerPixel(s.bpp);
        writeSurfaceDesc(d, s);
        if (!guestCall(c, cb, { d, ctx }))
            break;
    }
    heapFree(d);
    RET(0);
}

COMMETHOD(IDirectDraw, CreatePalette, 5)
{
    const uint32_t obj = comNew("IDirectDrawPalette");
    Palette p;
    for (int i = 0; i < 256 && ARG(2); i++)
    {
        const uint32_t e = ARG(2) + 4 * i;
        p.rgba[i] = 0xFF000000u | (uint32_t)rd8(e + 2) << 16 | (uint32_t)rd8(e + 1) << 8 | rd8(e);
    }
    {
        std::lock_guard lock(g_ddMutex);
        g_palettes[obj] = p;
    }
    wr32(ARG(3), obj);
    RET(0);
}

COMMETHOD(IDirectDraw, CreateSurface, 4)
{
    const uint32_t d = ARG(1);
    const uint32_t flags = rd32(d + 4), caps = rd32(d + 104);
    uint32_t bpp = g_modeBpp;
    if (flags & 0x1000) // DDSD_PIXELFORMAT
    {
        bpp = rd32(d + 84);
        if (bpp == 16 && rd32(d + 88) == 0x7C00)
            bpp = 15;
    }
    uint32_t obj;
    if (caps & 0x200) // DDSCAPS_PRIMARYSURFACE
    {
        obj = newSurface(g_modeW, g_modeH, g_modeBpp, true);
        g_primary = obj;
        if ((flags & 0x20) && rd32(d + 20)) // DDSD_BACKBUFFERCOUNT
        {
            const uint32_t back = newSurface(g_modeW, g_modeH, g_modeBpp, false);
            std::lock_guard lock(g_ddMutex);
            g_surfaces[obj].back = back;
        }
        logf("[ddraw] primary surface %ux%u %u bpp%s", g_modeW, g_modeH, g_modeBpp, (flags & 0x20) ? " with back buffer" : "");
    }
    else
    {
        obj = newSurface(rd32(d + 12), rd32(d + 8), bpp, false);
        logf("[ddraw] surface %ux%u %u bpp", rd32(d + 12), rd32(d + 8), bpp);
    }
    wr32(ARG(2), obj);
    RET(0);
}

// ---------------------------------------------------------------- IDirectDrawSurface

COMMETHOD(IDirectDrawSurface, QueryInterface, 3)
{
    wr32(ARG(2), 0);
    RET(0x80004002u);
}
COMMETHOD(IDirectDrawSurface, AddRef, 1) { RET(1); }
COMMETHOD(IDirectDrawSurface, Release, 1) { RET(0); }
COMMETHOD(IDirectDrawSurface, IsLost, 1) { RET(0); }
COMMETHOD(IDirectDrawSurface, Restore, 1) { RET(0); }
COMMETHOD(IDirectDrawSurface, SetClipper, 2) { RET(0); }
COMMETHOD(IDirectDrawSurface, GetBltStatus, 2) { RET(0); }
COMMETHOD(IDirectDrawSurface, GetFlipStatus, 2) { RET(0); }

COMMETHOD(IDirectDrawSurface, GetSurfaceDesc, 2)
{
    std::lock_guard lock(g_ddMutex);
    if (Surface* s = surf(ARG(0)))
        writeSurfaceDesc(ARG(1), *s);
    RET(0);
}
COMMETHOD(IDirectDrawSurface, GetPixelFormat, 2)
{
    std::lock_guard lock(g_ddMutex);
    if (Surface* s = surf(ARG(0)))
        writePixelFormat(ARG(1), s->bpp);
    RET(0);
}
COMMETHOD(IDirectDrawSurface, GetCaps, 2)
{
    std::lock_guard lock(g_ddMutex);
    Surface* s = surf(ARG(0));
    wr32(ARG(1), s && s->primary ? 0x200 | 0x4000 : 0x800 | 0x40);
    RET(0);
}
COMMETHOD(IDirectDrawSurface, GetAttachedSurface, 3)
{
    std::lock_guard lock(g_ddMutex);
    Surface* s = surf(ARG(0));
    wr32(ARG(2), s ? s->back : 0);
    RET(s && s->back ? 0 : 0x887600FFu); // DDERR_NOTFOUND
}
COMMETHOD(IDirectDrawSurface, Lock, 5)
{
    std::lock_guard lock(g_ddMutex);
    Surface* s = surf(ARG(0));
    if (!s)
        RET(0x80070057u);
    writeSurfaceDesc(ARG(2), *s);
    if (ARG(1)) // locking a sub-rectangle: point at its first pixel
        wr32(ARG(2) + 36, s->mem + rd32(ARG(1) + 4) * s->pitch + rd32(ARG(1)) * bytesPerPixel(s->bpp));
    RET(0);
}
COMMETHOD(IDirectDrawSurface, Unlock, 2)
{
    if (ARG(0) == g_primary)
        present();
    RET(0);
}
COMMETHOD(IDirectDrawSurface, Flip, 3)
{
    {
        std::lock_guard lock(g_ddMutex);
        Surface* s = surf(ARG(0));
        if (s && s->back)
            if (Surface* b = surf(s->back))
                std::swap(s->mem, b->mem);
    }
    present();
    RET(0);
}
COMMETHOD(IDirectDrawSurface, SetPalette, 2)
{
    {
        std::lock_guard lock(g_ddMutex);
        if (Surface* s = surf(ARG(0)))
            s->palette = ARG(1);
    }
    RET(0);
}
COMMETHOD(IDirectDrawSurface, GetPalette, 2)
{
    std::lock_guard lock(g_ddMutex);
    Surface* s = surf(ARG(0));
    wr32(ARG(1), s ? s->palette : 0);
    RET(s && s->palette ? 0 : 0x88760000u + 0x83);
}
COMMETHOD(IDirectDrawSurface, SetColorKey, 3)
{
    std::lock_guard lock(g_ddMutex);
    if (Surface* s = surf(ARG(0)); s && ARG(2))
    {
        s->colorKey = rd32(ARG(2));
        s->hasColorKey = true;
    }
    RET(0);
}

COMMETHOD(IDirectDrawSurface, Blt, 6)
{
    const uint32_t flags = ARG(4);
    bool toPrimary;
    {
        std::lock_guard lock(g_ddMutex);
        Surface* d = surf(ARG(0));
        if (!d)
            RET(0x80070057u);
        toPrimary = ARG(0) == g_primary;
        const Rect dr = rectOr(ARG(1), *d);
        if (flags & 0x400) // DDBLT_COLORFILL
        {
            const uint32_t color = ARG(5) ? rd32(ARG(5) + 80) : 0, bpp = bytesPerPixel(d->bpp);
            for (int y = std::max(0, dr.t); y < std::min<int>(dr.b, (int)d->h); y++)
                for (int x = std::max(0, dr.l); x < std::min<int>(dr.r, (int)d->w); x++)
                    memcpy(g_mem + d->mem + (size_t)y * d->pitch + x * bpp, &color, bpp);
        }
        else if (Surface* s = surf(ARG(2)))
            blit(*d, dr, *s, rectOr(ARG(3), *s), (flags & 0x8000) && s->hasColorKey); // DDBLT_KEYSRC
    }
    if (toPrimary)
        present();
    RET(0);
}

COMMETHOD(IDirectDrawSurface, BltFast, 6)
{
    bool toPrimary;
    {
        std::lock_guard lock(g_ddMutex);
        Surface* d = surf(ARG(0));
        Surface* s = surf(ARG(3));
        if (!d || !s)
            RET(0x80070057u);
        toPrimary = ARG(0) == g_primary;
        const Rect sr = rectOr(ARG(4), *s);
        const int32_t x = (int32_t)ARG(1), y = (int32_t)ARG(2);
        blit(*d, { x, y, x + sr.r - sr.l, y + sr.b - sr.t }, *s, sr, (ARG(5) & 1) && s->hasColorKey);
    }
    if (toPrimary)
        present();
    RET(0);
}

// ---------------------------------------------------------------- IDirectDrawPalette

COMMETHOD(IDirectDrawPalette, AddRef, 1) { RET(1); }
COMMETHOD(IDirectDrawPalette, Release, 1) { RET(0); }
COMMETHOD(IDirectDrawPalette, SetEntries, 5)
{
    const uint32_t start = ARG(2), count = ARG(3), e = ARG(4);
    {
        std::lock_guard lock(g_ddMutex);
        Palette& p = g_palettes[ARG(0)];
        for (uint32_t i = 0; i < count && start + i < 256; i++)
            p.rgba[start + i] = 0xFF000000u | (uint32_t)rd8(e + 4 * i + 2) << 16 | (uint32_t)rd8(e + 4 * i + 1) << 8 | rd8(e + 4 * i);
    }
    RET(0);
}
COMMETHOD(IDirectDrawPalette, GetEntries, 5)
{
    std::lock_guard lock(g_ddMutex);
    Palette& p = g_palettes[ARG(0)];
    for (uint32_t i = 0; i < ARG(3) && ARG(2) + i < 256; i++)
    {
        const uint32_t v = p.rgba[ARG(2) + i];
        wr8(ARG(4) + 4 * i, (uint8_t)v);
        wr8(ARG(4) + 4 * i + 1, (uint8_t)(v >> 8));
        wr8(ARG(4) + 4 * i + 2, (uint8_t)(v >> 16));
        wr8(ARG(4) + 4 * i + 3, 0);
    }
    RET(0);
}
