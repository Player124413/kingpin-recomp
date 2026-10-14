// macOS platform layer: Cocoa window and input, GL 3.2 core contexts, display-link frame pacing,
// CoreAudio output, ImageIO.
#define GL_SILENCE_DEPRECATION
#import <AudioToolbox/AudioToolbox.h>
#import <Cocoa/Cocoa.h>
#import <ImageIO/ImageIO.h>
#import <OpenGL/OpenGL.h>
#import <QuartzCore/QuartzCore.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <dlfcn.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>

#include "../win32/hle.h"
#include "app.h"
#include "platform.h"

void glBlitRgba(const uint32_t* pixels, int w, int h, int viewW, int viewH); // gl_post.cpp

namespace
{
    NSWindow* g_window;
    NSView* g_view;
    NSSize g_aspect = { 640, 480 };
    NSOpenGLContext* g_gameContext;
    std::atomic<bool> g_keyWindow{ true }, g_captureWanted{ false };
    NSEventModifierFlags g_lastFlags = 0;
    int g_renderW = 640, g_renderH = 480;
    std::atomic<int> g_viewPixelsW{ 640 }, g_viewPixelsH{ 480 }; // game view in pixels (set on the main thread)

    // macOS virtual key code -> PC set-1 scan code (| 0x100 for extended keys).
    uint32_t scanCode(unsigned short k)
    {
        static const uint16_t map[128] = {
            /*00*/ 0x1E, 0x1F, 0x20, 0x21, 0x23, 0x22, 0x2C, 0x2D, 0x2E, 0x2F, 0x56, 0x30, 0x10, 0x11, 0x12, 0x13,
            /*10*/ 0x15, 0x14, 0x02, 0x03, 0x04, 0x05, 0x07, 0x06, 0x0D, 0x0A, 0x08, 0x0C, 0x09, 0x0B, 0x1B, 0x18,
            /*20*/ 0x16, 0x1A, 0x17, 0x19, 0x1C, 0x26, 0x24, 0x28, 0x25, 0x27, 0x2B, 0x33, 0x35, 0x31, 0x32, 0x34,
            /*30*/ 0x0F, 0x39, 0x29, 0x0E, 0x00, 0x01, 0x00, 0x00, 0x2A, 0x3A, 0x38, 0x1D, 0x36, 0x138, 0x11D, 0x00,
            /*40*/ 0x00, 0x53, 0x00, 0x37, 0x00, 0x4E, 0x00, 0x45, 0x00, 0x00, 0x00, 0x135, 0x11C, 0x00, 0x4A, 0x00,
            /*50*/ 0x00, 0x00, 0x52, 0x4F, 0x50, 0x51, 0x4B, 0x4C, 0x4D, 0x47, 0x00, 0x48, 0x49, 0x00, 0x00, 0x00,
            /*60*/ 0x3F, 0x40, 0x41, 0x3D, 0x42, 0x43, 0x00, 0x57, 0x00, 0x00, 0x00, 0x00, 0x00, 0x44, 0x00, 0x58,
            /*70*/ 0x00, 0x00, 0x152, 0x147, 0x149, 0x153, 0x3E, 0x14F, 0x3C, 0x151, 0x3B, 0x14B, 0x14D, 0x150, 0x148, 0x00,
        };
        return k < 128 ? map[k] : 0;
    }

    uint32_t vkFor(uint32_t sc, NSString* chars)
    {
        if (chars.length == 1)
        {
            const unichar ch = [chars characterAtIndex:0];
            if (ch >= 'a' && ch <= 'z')
                return ch - 32;
            if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9'))
                return ch;
        }
        switch (sc & 0xFF)
        {
        case 0x01: return 0x1B;
        case 0x1C: return 0x0D;
        case 0x0E: return 0x08;
        case 0x0F: return 0x09;
        case 0x39: return 0x20;
        case 0x2A: case 0x36: return 0x10;
        case 0x1D: return 0x11;
        case 0x38: return 0x12;
        default: return 0;
        }
    }

    void updateCursorLock()
    {
        const bool lock = g_captureWanted && g_keyWindow;
        static bool locked = false;
        if (lock == locked)
            return;
        locked = lock;
        dispatch_async(dispatch_get_main_queue(), ^{
            CGAssociateMouseAndMouseCursorPosition(lock ? false : true);
            if (lock)
                [NSCursor hide];
            else
                [NSCursor unhide];
        });
    }

    // KP_TOUCH=1: the mouse acts as a finger on the touch controls (for testing them on the Mac).
    bool touchEvent(NSEvent* e)
    {
        if (!touch::enabled())
            return false;
        const NSPoint p = [g_view convertPoint:e.locationInWindow fromView:nil];
        const NSRect b = g_view.bounds;
        const float x = (float)(p.x / b.size.width * g_renderW), y = (float)((b.size.height - p.y) / b.size.height * g_renderH);
        switch (e.type)
        {
        case NSEventTypeLeftMouseDown: touch::begin(1, x, y); return true;
        case NSEventTypeLeftMouseDragged: touch::move(1, x, y); return true;
        case NSEventTypeLeftMouseUp: touch::end(1); return true;
        case NSEventTypeMouseMoved: return true;
        default: return false;
        }
    }

    void handleEvent(NSEvent* e)
    {
        if (touchEvent(e))
            return;
        switch (e.type)
        {
        case NSEventTypeKeyDown:
            if (e.modifierFlags & NSEventModifierFlagCommand)
            {
                switch (e.keyCode)
                {
                case 0x0C: input::close(); break;                                // Cmd+Q
                case 0x24: case 0x03: [g_window toggleFullScreen:nil]; break;    // Cmd+Enter / Cmd+F
                case 0x18: gammaAdjust(0.05f); break;                            // Cmd+=
                case 0x1B: gammaAdjust(-0.05f); break;                           // Cmd+-
                case 0x23: glPostToggleEffects(); break;                         // Cmd+P
                default: break;
                }
                return; // Cmd shortcuts are not game keys
            }
            input::key(scanCode(e.keyCode), true, vkFor(scanCode(e.keyCode), e.charactersIgnoringModifiers));
            if (e.characters.length == 1)
            {
                const unichar ch = [e.characters characterAtIndex:0];
                if (ch >= 32 && ch < 127)
                    input::character(ch);
            }
            break;
        case NSEventTypeKeyUp:
            input::key(scanCode(e.keyCode), false, vkFor(scanCode(e.keyCode), e.charactersIgnoringModifiers));
            break;
        case NSEventTypeFlagsChanged:
        {
            const NSEventModifierFlags f = e.modifierFlags, changed = f ^ g_lastFlags;
            g_lastFlags = f;
            for (NSEventModifierFlags flag : { NSEventModifierFlagShift, NSEventModifierFlagControl, NSEventModifierFlagOption,
                                               NSEventModifierFlagCapsLock })
                if (changed & flag)
                {
                    const uint32_t sc = scanCode(e.keyCode);
                    input::key(sc, (f & flag) != 0, vkFor(sc, @""));
                }
            break;
        }
        case NSEventTypeMouseMoved:
        case NSEventTypeLeftMouseDragged:
        case NSEventTypeRightMouseDragged:
        case NSEventTypeOtherMouseDragged:
            if (g_captureWanted)
                input::mouseDelta((int)lround(e.deltaX), (int)lround(e.deltaY));
            else
            {
                // Follow the real pointer while the game shows its own cursor (menus).
                const NSPoint p = [g_view convertPoint:e.locationInWindow fromView:nil];
                const NSRect b = g_view.bounds;
                input::mouseAbsolute(p.x / b.size.width, (b.size.height - p.y) / b.size.height);
            }
            break;
        case NSEventTypeLeftMouseDown: input::mouseButton(0, true); break;
        case NSEventTypeLeftMouseUp: input::mouseButton(0, false); break;
        case NSEventTypeRightMouseDown: input::mouseButton(1, true); break;
        case NSEventTypeRightMouseUp: input::mouseButton(1, false); break;
        case NSEventTypeOtherMouseDown: input::mouseButton(2, true); break;
        case NSEventTypeOtherMouseUp: input::mouseButton(2, false); break;
        case NSEventTypeScrollWheel:
            if (e.scrollingDeltaY != 0)
                input::wheel(e.scrollingDeltaY > 0 ? 1 : -1);
            break;
        default: break;
        }
    }
}

@interface KPWindowDelegate : NSObject <NSWindowDelegate>
@end
@implementation KPWindowDelegate
- (BOOL)windowShouldClose:(NSWindow*)sender
{
    input::close();
    return NO;
}
- (void)windowDidBecomeKey:(NSNotification*)n
{
    g_keyWindow = true;
    input::activate(true);
    updateCursorLock();
}
- (void)windowDidResignKey:(NSNotification*)n
{
    g_keyWindow = false;
    input::activate(false);
    updateCursorLock();
}
@end

@interface KPView : NSView
@end
@implementation KPView
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)isOpaque { return YES; }
@end

// Black window content that keeps the game view centred at the game's aspect ratio.
@interface KPContainerView : NSView
@end
@implementation KPContainerView
- (BOOL)isOpaque { return YES; }
- (void)drawRect:(NSRect)r
{
    [NSColor.blackColor setFill];
    NSRectFill(r);
}
- (void)resizeSubviewsWithOldSize:(NSSize)old
{
    const NSRect b = self.bounds;
    const double scale = std::min(b.size.width / g_aspect.width, b.size.height / g_aspect.height);
    const NSSize s = NSMakeSize(std::floor(g_aspect.width * scale), std::floor(g_aspect.height * scale));
    g_view.frame = NSMakeRect(std::floor((b.size.width - s.width) / 2), std::floor((b.size.height - s.height) / 2), s.width, s.height);
    const NSSize px = [g_view convertSizeToBacking:s];
    g_viewPixelsW = (int)px.width;
    g_viewPixelsH = (int)px.height;
    if (NSOpenGLContext* ctx = g_gameContext)
    {
        CGLLockContext(ctx.CGLContextObj);
        [ctx update];
        CGLUnlockContext(ctx.CGLContextObj);
    }
}
@end

// ---------------------------------------------------------------- frame pacing
//
// One game frame per display refresh: an NSView display link (follows the window's screen,
// ProMotion aware) signals each refresh; after presenting, the game thread waits for the next
// signal. GL vsync is off (macOS's OpenGL swap does not block consistently). KP_PACING=0 uses
// GL vsync instead.

@interface KPDisplayLinkTarget : NSObject
- (void)tick:(id)link;
@end

namespace
{
    std::mutex g_vblankMutex;
    std::condition_variable g_vblankCv;
    uint64_t g_vblankCount = 0;
    bool g_pacing = false;
    id g_displayLink;
    KPDisplayLinkTarget* g_linkTarget;
}

@implementation KPDisplayLinkTarget
- (void)tick:(id)link
{
    std::lock_guard lock(g_vblankMutex);
    g_vblankCount++;
    g_vblankCv.notify_all();
}
@end

namespace
{
    void pacingInit()
    {
        static bool done = false;
        if (done)
            return;
        done = true;
        g_pacing = !getenv("KP_PACING") || atoi(getenv("KP_PACING")) != 0;
        if (!g_pacing)
            return;
        dispatch_sync(dispatch_get_main_queue(), ^{
            if (@available(macOS 14.0, *))
            {
                g_linkTarget = [[KPDisplayLinkTarget alloc] init];
                CADisplayLink* link = [g_view displayLinkWithTarget:g_linkTarget selector:@selector(tick:)];
                const float maxHz = (float)NSScreen.mainScreen.maximumFramesPerSecond;
                link.preferredFrameRateRange = CAFrameRateRangeMake(maxHz, maxHz, maxHz);
                [link addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
                g_displayLink = link;
            }
            else
                g_pacing = false;
        });
        logf("[gfx] frame pacing: %s", g_pacing ? "display link (one frame per refresh)" : "GL vsync");
    }

    NSOpenGLContext* makeContext()
    {
        NSOpenGLPixelFormatAttribute attrs[] = { NSOpenGLPFADoubleBuffer, NSOpenGLPFAAccelerated, NSOpenGLPFAColorSize, 24,
                                                 NSOpenGLPFAAlphaSize, 8, NSOpenGLPFAOpenGLProfile,
                                                 NSOpenGLProfileVersion3_2Core, 0 };
        return [[NSOpenGLContext alloc] initWithFormat:[[NSOpenGLPixelFormat alloc] initWithAttributes:attrs] shareContext:nil];
    }
}

// ---------------------------------------------------------------- platform interface

namespace platform
{
    void showWindow(int w, int h, const char* title)
    {
        dispatch_sync(dispatch_get_main_queue(), ^{
            const NSRect vis = NSScreen.mainScreen.visibleFrame;
            // Fit the window into the visible screen at the game's aspect ratio: small modes are
            // scaled up by whole steps, large ones (the native mode) scaled down (rendering is scaled).
            double scale = std::min(vis.size.width * 0.92 / w, (vis.size.height - 32) * 0.95 / h);
            if (scale >= 2)
                scale = std::floor(scale);
            const NSRect r = NSMakeRect(0, 0, w * scale, h * scale);
            if (!g_window)
            {
                g_window = [[NSWindow alloc] initWithContentRect:r
                                                       styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                                 NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                                         backing:NSBackingStoreBuffered
                                                           defer:NO];
                g_view = [[KPView alloc] initWithFrame:r];
                KPContainerView* container = [[KPContainerView alloc] initWithFrame:r];
                [container addSubview:g_view];
                g_window.contentView = container;
                g_window.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
                g_window.backgroundColor = NSColor.blackColor;
                static KPWindowDelegate* delegate = [[KPWindowDelegate alloc] init]; // the property is weak
                g_window.delegate = delegate;
                g_window.acceptsMouseMovedEvents = YES;
                g_window.releasedWhenClosed = NO;
            }
            else if (!(g_window.styleMask & NSWindowStyleMaskFullScreen))
                [g_window setContentSize:r.size];
            g_aspect = NSMakeSize(w, h);
            [g_window.contentView resizeSubviewsWithOldSize:r.size];
            g_window.title = [NSString stringWithUTF8String:title && *title ? title : "Kingpin"];
            [g_window center];
            [g_window makeKeyAndOrderFront:nil];
            [g_window makeFirstResponder:g_view];
            [NSApp activateIgnoringOtherApps:YES];
        });
    }

    void hideWindow()
    {
        dispatch_async(dispatch_get_main_queue(), ^{ [g_window orderOut:nil]; });
    }

    void setFullscreen(bool on)
    {
        dispatch_async(dispatch_get_main_queue(), ^{
            if (g_window && g_window.isVisible && on != ((g_window.styleMask & NSWindowStyleMaskFullScreen) != 0))
                [g_window toggleFullScreen:nil];
        });
    }

    void screenSize(int& w, int& h)
    {
        // The area a fullscreen window gets (below the camera housing on notched displays).
        static int sw = 0, sh = 0;
        if (!sw)
        {
            NSScreen* scr = NSScreen.mainScreen;
            sw = (int)scr.frame.size.width;
            sh = (int)(scr.frame.size.height - scr.safeAreaInsets.top);
        }
        w = sw;
        h = sh;
    }

    void setMouseCaptured(bool captured)
    {
        g_captureWanted = captured && !touch::enabled();
        updateCursorLock();
    }

    bool glCreate()
    {
        if (g_gameContext)
            return true; // already made (for the intro, or before a video restart)
        g_gameContext = makeContext();
        return g_gameContext != nil;
    }

    void glMakeCurrent(int renderW, int renderH)
    {
        g_renderW = renderW;
        g_renderH = renderH;
        NSOpenGLContext* ctx = g_gameContext;
        dispatch_sync(dispatch_get_main_queue(), ^{
            ctx.view = g_view;
        });
        // Render at the game's resolution; the window scales the surface.
        const GLint dim[2] = { renderW, renderH };
        CGLSetParameter(ctx.CGLContextObj, kCGLCPSurfaceBackingSize, dim);
        CGLEnable(ctx.CGLContextObj, kCGLCESurfaceBackingSize);
        [ctx makeCurrentContext];
        pacingInit();
        const GLint interval = g_pacing ? 0 : 1;
        [ctx setValues:&interval forParameter:NSOpenGLContextParameterSwapInterval];
    }

    // The context lives as long as the process: the game deletes and recreates it on a video
    // restart, and the GL emulation's objects (shaders, buffers, textures) must stay valid.
    void glRelease() { [NSOpenGLContext clearCurrentContext]; }

    void glPresent()
    {
        @autoreleasepool
        {
            CGLLockContext(g_gameContext.CGLContextObj);
            [g_gameContext flushBuffer];
            CGLUnlockContext(g_gameContext.CGLContextObj);
        }
    }

    void glWaitVblank()
    {
        if (!g_pacing)
            return;
        std::unique_lock lock(g_vblankMutex);
        const uint64_t seen = g_vblankCount;
        g_vblankCv.wait_for(lock, std::chrono::milliseconds(50), [&] { return g_vblankCount != seen; });
    }

    uint32_t glWindowFramebuffer() { return 0; }

    void glDrawableSize(int& w, int& h)
    {
        w = g_renderW; // the surface backing size
        h = g_renderH;
    }

    // The intro movie's frames (DirectDraw). They go through the game's context, as on the other
    // platforms (the post-processing objects live in one context; vertex arrays are not shared).
    void presentRgba(const uint32_t* pixels, int w, int h)
    {
        @autoreleasepool
        {
            if (!g_gameContext)
            {
                glCreate();
                glMakeCurrent(g_viewPixelsW, g_viewPixelsH); // the window's full resolution
            }
            glBlitRgba(pixels, w, h, g_renderW, g_renderH);
            frameHook();
            glPresent();
        }
    }

    // ---- audio
    namespace
    {
        void (*g_render)(float*, uint32_t);
        AudioComponentInstance g_unit;
        OSStatus renderCallback(void*, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32, UInt32 frames,
                                AudioBufferList* io)
        {
            g_render(static_cast<float*>(io->mBuffers[0].mData), frames);
            return noErr;
        }
    }

    void audioStart(void (*render)(float* out, uint32_t frames))
    {
        g_render = render;
        AudioComponentDescription desc{ kAudioUnitType_Output, kAudioUnitSubType_DefaultOutput, kAudioUnitManufacturer_Apple, 0, 0 };
        AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
        if (!comp || AudioComponentInstanceNew(comp, &g_unit) != noErr)
            return;
        AudioStreamBasicDescription fmt{};
        fmt.mSampleRate = 44100;
        fmt.mFormatID = kAudioFormatLinearPCM;
        fmt.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
        fmt.mChannelsPerFrame = 2;
        fmt.mFramesPerPacket = 1;
        fmt.mBitsPerChannel = 32;
        fmt.mBytesPerFrame = 8;
        fmt.mBytesPerPacket = 8;
        AudioUnitSetProperty(g_unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &fmt, sizeof fmt);
        AURenderCallbackStruct cb{ renderCallback, nullptr };
        AudioUnitSetProperty(g_unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb);
        if (AudioUnitInitialize(g_unit) != noErr || AudioOutputUnitStart(g_unit) != noErr)
            logf("[audio] could not start CoreAudio output");
        else
            logf("[audio] CoreAudio output started (44.1 kHz stereo)");
    }
}

#include "apple_images.inc"

// ---------------------------------------------------------------- debug window capture

// KP_WINDOW_CAPTURE=path,seconds saves what the window actually shows (composited).
static void windowCaptureSchedule()
{
    const char* e = getenv("KP_WINDOW_CAPTURE");
    if (!e)
        return;
    std::string spec = e;
    const size_t comma = spec.find(',');
    const double secs = comma == std::string::npos ? 10.0 : atof(spec.c_str() + comma + 1);
    const std::string path = spec.substr(0, comma);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(secs * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        if (!g_window)
            return;
        using CaptureFn = CGImageRef (*)(CGRect, uint32_t, uint32_t, uint32_t);
        static CaptureFn capture = (CaptureFn)dlsym(RTLD_DEFAULT, "CGWindowListCreateImage");
        CGImageRef img = capture ? capture(CGRectNull, 1 << 3 /* IncludingWindow */, (uint32_t)g_window.windowNumber,
                                           1 << 0 /* BoundsIgnoreFraming */)
                                 : nullptr;
        if (!img)
        {
            logf("[debug] window capture failed");
            return;
        }
        NSBitmapImageRep* rep = [[NSBitmapImageRep alloc] initWithCGImage:img];
        [[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:[NSString stringWithUTF8String:path.c_str()]
                                                                               atomically:YES];
        CGImageRelease(img);
        logf("[debug] window captured to %s", path.c_str());
    });
}

// ---------------------------------------------------------------- main

int main(int argc, char** argv)
{
    @autoreleasepool
    {
        std::string exeDir = argv[0];
        exeDir = exeDir.substr(0, exeDir.find_last_of('/') + 1);
        AppConfig cfg;
        char real[PATH_MAX];
        cfg.hostExeDir = realpath(exeDir.empty() ? "." : exeDir.c_str(), real) ? real : exeDir;
        cfg.gameDir = cfg.hostExeDir + "/../game";
        // D:\ is the disc's data track image (tools/install_game.sh), or a mounted copy in disc/
        cfg.discDir = access((cfg.hostExeDir + "/../track1.iso").c_str(), R_OK) == 0 ? cfg.hostExeDir + "/../track1.iso"
                                                                                      : cfg.hostExeDir + "/../disc";
        // CD music: the disc's cue sheet and audio track .bin files in cd/ (KP_CUE overrides)
        for (NSString* f in [NSFileManager.defaultManager contentsOfDirectoryAtPath:
                                                      [NSString stringWithUTF8String:(cfg.hostExeDir + "/../cd").c_str()]
                                                                              error:nil])
            if ([f.pathExtension.lowercaseString isEqualToString:@"cue"])
                cfg.cuePath = cfg.hostExeDir + "/../cd/" + f.UTF8String;
        for (int i = 1; i < argc; i++)
            cfg.args.push_back(argv[i]);
        if (!appInit(cfg))
            return 1;

        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        windowCaptureSchedule();
        [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskAny
                                              handler:^NSEvent*(NSEvent* e) {
                                                  if (e.window == g_window || e.type == NSEventTypeFlagsChanged)
                                                  {
                                                      handleEvent(e);
                                                      if (e.type == NSEventTypeKeyDown || e.type == NSEventTypeKeyUp)
                                                          return nil; // no system beep
                                                  }
                                                  return e;
                                              }];
        appStartGuest();
        [NSApp finishLaunching];
        [NSApp run];
    }
    return 0;
}
