// Kingpin-specific presentation tweaks (host side; the game code is unchanged):
//  * A native-resolution video mode: the last entry of the game's mode table (gl_mode 6,
//    normally 1600x1200) becomes the display's size, or the size given with --res WxH.
//  * 2D scaling: at high resolutions the client lays out its HUD and menus on a virtual screen
//    1/N the size (N = round(height / 600), or KP_HUD_SCALE), the 2D projection maps that onto the
//    window and 3D view rectangles are scaled back to real pixels.
//  * Hor+ field of view: for views wider than 4:3, the renderer gets the vertical FOV the game
//    would use at 4:3 and a correspondingly wider horizontal FOV (instead of cropping the top and
//    bottom). Disable with KP_NO_HORPLUS=1.
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "platform/platform.h"
#include "win32/hle.h"

X86Fn x86HookInstall(uint32_t addr, X86Fn hook);
extern int g_ortho2DFromW, g_ortho2DFromH, g_ortho2DToW, g_ortho2DToH; // gl_textures.cpp
extern double g_ortho2DPad;

namespace
{
    constexpr uint32_t kModeTable = 0x0045EFE0; // kingpin.exe: {desc, width, height, mode} x 7
    constexpr uint32_t kRenderFrame = 0x10010940; // ref_gl.dll: R_RenderFrame(refdef_t*) (refexport +0x2C)
    constexpr uint32_t kVidNewWindow = 0x00443370; // kingpin.exe: VID_NewWindow(width, height)
    X86Fn g_renderFrame, g_vidNewWindow;
    int g_realW = 640, g_realH = 480, g_virtW = 640, g_virtH = 480;

    // Real pixels per virtual (client) unit and the horizontal offset of the client's 4:3 area.
    double g_scale = 1.0, g_offX = 0.0;

    void vidNewWindowScaled(Cpu& c)
    {
        g_realW = (int)rd32(c.r[4] + 4);
        g_realH = (int)rd32(c.r[4] + 8);
        int scale = (int)lround(g_realH / 600.0);
        if (const char* e = getenv("KP_HUD_SCALE"))
            scale = atoi(e);
        scale = std::max(1, scale);
        g_virtH = g_realH / scale;
        g_virtW = g_realW / scale;
        // Widescreen: the client lays out HUD, menus and pictures on a 4:3 screen, centred with
        // black bars (unless KP_STRETCH_2D=1); its 3D view is widened to the full screen.
        static const bool stretch = getenv("KP_STRETCH_2D") && atoi(getenv("KP_STRETCH_2D")) != 0;
        if (!stretch && g_realW * 3 > g_realH * 4 + 8)
            g_virtW = (g_virtH * 4 / 3) & ~7;
        g_scale = (double)g_realH / g_virtH;
        g_offX = (g_realW - g_virtW * g_scale) / 2;
        g_ortho2DFromW = g_realW;
        g_ortho2DFromH = g_realH;
        g_ortho2DToW = g_virtW;
        g_ortho2DToH = g_virtH;
        g_ortho2DPad = g_offX / g_scale; // virtual units of black bar on each side
        logf("[tweaks] %dx%d: 2D laid out at %dx%d (x%d)%s", g_realW, g_realH, g_virtW, g_virtH, scale,
             g_offX > 0.5 ? ", centred 4:3" : "");
        wr32(c.r[4] + 4, (uint32_t)g_virtW);
        wr32(c.r[4] + 8, (uint32_t)g_virtH);
        g_vidNewWindow(c);
    }

    float rdf(uint32_t a)
    {
        float f;
        memcpy(&f, g_mem + a, 4);
        return f;
    }
    void wrf(uint32_t a, float f) { memcpy(g_mem + a, &f, 4); }

    void renderFrameHorPlus(Cpu& c)
    {
        const uint32_t fd = rd32(c.r[4] + 4);
        static int logged = 0;
        if (fd && getenv("KP_TRACE_VIEW") && logged++ < 5)
            logf("[tweaks] RenderFrame view %d,%d %dx%d fov %.1f/%.1f", (int)rd32(fd), (int)rd32(fd + 4), (int)rd32(fd + 8),
                 (int)rd32(fd + 12), rdf(fd + 16), rdf(fd + 20));
        if (fd && (g_virtW != g_realW || g_virtH != g_realH))
        {
            // View rectangle: virtual (client layout) -> real pixels. A view covering the whole
            // virtual screen (the client rounds widths to multiples of 8) covers the whole window.
            const int x = (int)rd32(fd), y = (int)rd32(fd + 4), vw = (int)rd32(fd + 8), vh = (int)rd32(fd + 12);
            if (vw >= g_virtW - 16)
            {
                wr32(fd, 0);
                wr32(fd + 8, (uint32_t)g_realW);
            }
            else
            {
                wr32(fd, (uint32_t)lround(g_offX + x * g_scale));
                wr32(fd + 8, (uint32_t)lround(vw * g_scale));
            }
            if (vh >= g_virtH - 16)
            {
                wr32(fd + 4, 0);
                wr32(fd + 12, (uint32_t)g_realH);
            }
            else
            {
                wr32(fd + 4, (uint32_t)lround(y * g_scale));
                wr32(fd + 12, (uint32_t)lround(vh * g_scale));
            }
        }
        const int w = (int)rd32(fd + 8), h = (int)rd32(fd + 12);
        static const bool horPlus = getenv("KP_NO_HORPLUS") == nullptr;
        if (horPlus && fd && h > 0 && w * 3 > h * 4 + 4)
        {
            const double d2r = M_PI / 180.0;
            const double fovX = rdf(fd + 16);
            const double fovY43 = 2.0 * atan(tan(fovX * d2r / 2) * 3.0 / 4.0);
            const double fovXWide = 2.0 * atan(tan(fovY43 / 2) * w / h);
            wrf(fd + 16, (float)(fovXWide / d2r));
            wrf(fd + 20, (float)(fovY43 / d2r));
        }
        g_renderFrame(c);
    }
}

// ---------------------------------------------------------------- HD textures
//
// ref_gl GL_LoadPic(name, pic, width, height, type, bits) is wrapped so the GL layer knows which
// image an upload belongs to; upscaled images live in main/hd/<name>.png (tools/upscale_textures.py).

void glTexturesBeginImage(const char* name, uint32_t pic, int w, int h, int type);
void glTexturesEndImage();

namespace
{
    constexpr uint32_t kLoadPic = 0x10004320;
    X86Fn g_loadPic;

    void loadPicHooked(Cpu& c)
    {
        const uint32_t name = rd32(c.r[4] + 4), pic = rd32(c.r[4] + 8);
        const int w = (int)rd32(c.r[4] + 12), h = (int)rd32(c.r[4] + 16), type = (int)rd32(c.r[4] + 20);
        const int bits = (int)rd32(c.r[4] + 24);
        // it_skin 0, it_sprite 1, it_wall 2, it_pic 3 (UI, kept original), it_sky 4
        if (name && bits == 32 && type != 3)
            glTexturesBeginImage(gstr(name), pic, w, h, type);
        g_loadPic(c);
        glTexturesEndImage();
    }
}

bool loadHdTexture(const char* name, std::vector<uint8_t>& rgba, int& w, int& h)
{
    std::string n = name;
    const size_t dot = n.find_last_of('.');
    if (dot != std::string::npos)
        n.resize(dot);
    return platform::decodeImage(g_gameDir + "/main/hd/" + n + ".png", rgba, w, h);
}

std::string g_resolutionArg; // --res WxH
extern bool (*g_postSuppressed)(); // gl_post.cpp

void kingpinTweaksInit()
{
    int w = 0, h = 0;
    if (sscanf(g_resolutionArg.c_str(), "%dx%d", &w, &h) != 2)
        platform::screenSize(w, h); // the native video mode
    const uint32_t e = kModeTable + 16 * 6;
    if (rd32(e + 4) == 1600 && rd32(e + 8) == 1200)
    {
        wr32(e, guestStrdup("Mode 9: " + std::to_string(w) + "x" + std::to_string(h)));
        wr32(e + 4, (uint32_t)w);
        wr32(e + 8, (uint32_t)h);
        logf("[tweaks] video mode 6 is now %dx%d (native)", w, h);
    }
    g_renderFrame = x86HookInstall(kRenderFrame, renderFrameHorPlus);
    g_vidNewWindow = x86HookInstall(kVidNewWindow, vidNewWindowScaled);
    g_loadPic = x86HookInstall(kLoadPic, loadPicHooked);
    // No AO/bloom while the console is down (cls.key_dest == key_console).
    g_postSuppressed = [] { return rd32(0x0125DD04) == 1; };
}
