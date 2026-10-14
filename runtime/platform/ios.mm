// iOS / iPadOS platform layer: UIKit app, OpenGL ES 3 on a CAEAGLLayer, display-link frame
// pacing, RemoteIO audio, touch controls (runtime/touch_controls.cpp), game controllers.
//
// Game data: the app's Documents folder (reachable from the Files app) holds the installed game
// (kingpin.exe, ref_gl.dll, main/...), the disc's data track image track1.iso and optionally
// cd/<name>.cue with the audio track .bin files for the music.
#define GLES_SILENCE_DEPRECATION
#import <AVFAudio/AVFAudio.h>
#import <AudioToolbox/AudioToolbox.h>
#import <OpenGLES/EAGL.h>
#import <OpenGLES/ES3/gl.h>
#import <QuartzCore/QuartzCore.h>
#import <UIKit/UIKit.h>

#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

#include "../win32/hle.h"
#include "app.h"
#include "platform.h"

void glBlitRgba(const uint32_t* pixels, int w, int h, int viewW, int viewH); // gl_post.cpp

// ---------------------------------------------------------------- view

@interface KPGLView : UIView
@end

namespace
{
    // Pixels per point of the game view: the screen's native scale times KP_RENDER_SCALE (fewer
    // pixels on slower devices; the layer is scaled up by the compositor).
    double contentScale()
    {
        double k = 1.0;
        if (const char* s = getenv("KP_RENDER_SCALE"))
            k = std::clamp(atof(s), 0.25, 1.0);
        return UIScreen.mainScreen.nativeScale * k;
    }
}

namespace
{
    KPGLView* g_view;
    EAGLContext* g_context;
    GLuint g_fbo, g_colorRb;
    int g_fbW = 0, g_fbH = 0;
    std::atomic<bool> g_layerResized{ false };

    std::mutex g_activeMutex;
    std::condition_variable g_activeCv;
    bool g_active = true; // GL must not be used while the app is in the background

    std::mutex g_vblankMutex;
    std::condition_variable g_vblankCv;
    uint64_t g_vblankCount = 0;
}

@implementation KPGLView
+ (Class)layerClass { return [CAEAGLLayer class]; }
- (instancetype)initWithFrame:(CGRect)frame
{
    if ((self = [super initWithFrame:frame]))
    {
        CAEAGLLayer* layer = (CAEAGLLayer*)self.layer;
        layer.opaque = YES;
        layer.drawableProperties = @{ kEAGLDrawablePropertyRetainedBacking : @NO,
                                      kEAGLDrawablePropertyColorFormat : kEAGLColorFormatRGBA8 };
        self.contentScaleFactor = contentScale();
        self.multipleTouchEnabled = YES;
    }
    return self;
}
- (void)layoutSubviews
{
    [super layoutSubviews];
    g_layerResized = true;
}
- (CGPoint)pixel:(UITouch*)t
{
    const CGPoint p = [t locationInView:self];
    return CGPointMake(p.x * self.contentScaleFactor, p.y * self.contentScaleFactor);
}
- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event
{
    for (UITouch* t in touches)
    {
        const CGPoint p = [self pixel:t];
        touch::begin((intptr_t)(__bridge void*)t, (float)p.x, (float)p.y);
    }
}
- (void)touchesMoved:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event
{
    for (UITouch* t in touches)
    {
        const CGPoint p = [self pixel:t];
        touch::move((intptr_t)(__bridge void*)t, (float)p.x, (float)p.y);
    }
}
- (void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event
{
    for (UITouch* t in touches)
        touch::end((intptr_t)(__bridge void*)t);
}
- (void)touchesCancelled:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event { [self touchesEnded:touches withEvent:event]; }
@end

@interface KPViewController : UIViewController
@end
@implementation KPViewController
- (void)loadView { self.view = g_view; }
- (BOOL)prefersStatusBarHidden { return YES; }
- (BOOL)prefersHomeIndicatorAutoHidden { return YES; }
- (UIRectEdge)preferredScreenEdgesDeferringSystemGestures { return UIRectEdgeAll; }
- (UIInterfaceOrientationMask)supportedInterfaceOrientations { return UIInterfaceOrientationMaskLandscape; }
@end

// Shown instead of the game when the game files are missing.
@interface KPMessageController : UIViewController
@property(nonatomic, copy) NSString* message;
@end
@implementation KPMessageController
- (void)viewDidLoad
{
    [super viewDidLoad];
    self.view.backgroundColor = UIColor.blackColor;
    UILabel* label = [[UILabel alloc] init];
    label.text = self.message;
    label.textColor = UIColor.whiteColor;
    label.numberOfLines = 0;
    label.textAlignment = NSTextAlignmentCenter;
    label.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:label];
    [NSLayoutConstraint activateConstraints:@[
        [label.centerXAnchor constraintEqualToAnchor:self.view.centerXAnchor],
        [label.centerYAnchor constraintEqualToAnchor:self.view.centerYAnchor],
        [label.widthAnchor constraintLessThanOrEqualToAnchor:self.view.widthAnchor multiplier:0.85],
    ]];
}
@end

@interface KPDisplayLinkTarget : NSObject
- (void)tick:(CADisplayLink*)link;
@end
@implementation KPDisplayLinkTarget
- (void)tick:(CADisplayLink*)link
{
    std::lock_guard lock(g_vblankMutex);
    g_vblankCount++;
    g_vblankCv.notify_all();
}
@end

// ---------------------------------------------------------------- app

// The app uses the scene lifecycle (required on iOS 27): the window and the game start when the
// first scene connects.
@interface KPAppDelegate : UIResponder <UIApplicationDelegate>
@end
@implementation KPAppDelegate
- (UISceneConfiguration*)application:(UIApplication*)app
    configurationForConnectingSceneSession:(UISceneSession*)session
                                   options:(UISceneConnectionOptions*)options
{
    UISceneConfiguration* c = [[UISceneConfiguration alloc] initWithName:@"Default" sessionRole:session.role];
    c.delegateClass = NSClassFromString(@"KPSceneDelegate");
    return c;
}
@end

@interface KPSceneDelegate : UIResponder <UIWindowSceneDelegate>
@property(nonatomic, strong) UIWindow* window;
@property(nonatomic, strong) CADisplayLink* displayLink;
@end

@implementation KPSceneDelegate
- (void)scene:(UIScene*)scene willConnectToSession:(UISceneSession*)session options:(UISceneConnectionOptions*)options
{
    static bool started = false; // one game per process (a second window just shows a message)
    UIWindowScene* windowScene = (UIWindowScene*)scene;
    self.window = [[UIWindow alloc] initWithWindowScene:windowScene];
    NSString* docs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
    const std::string gameDir = docs.UTF8String;
    // The guest's 32-bit address space is one 4 GB reservation; without the extended virtual
    // addressing entitlement some devices may not allow it.
    void* probe = mmap(nullptr, 1ull << 32, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    const bool addressSpace = probe != MAP_FAILED;
    if (addressSpace)
        munmap(probe, 1ull << 32);
    logf("[main] 4 GB guest address space: %s", addressSpace ? "available" : "NOT available");
    if (started || !addressSpace || access((gameDir + "/kingpin.exe").c_str(), R_OK) != 0)
    {
        KPMessageController* vc = [[KPMessageController alloc] init];
        vc.message = started        ? @"Kingpin is already running in another window."
                     : !addressSpace ? @"This device does not give the app the 4 GB address space the game needs (the build needs the "
                                       @"Extended Virtual Addressing entitlement)."
                                     : @"Kingpin needs your game files.\n\nCopy the installed game (kingpin.exe, ref_gl.dll and the main "
                                       @"folder, plus kpintro.exe for the intro movie) and the disc's data track image track1.iso "
                                       @"into this app's folder in the Files app, then start it again.";
        self.window.rootViewController = vc;
        [self.window makeKeyAndVisible];
        return;
    }
    started = true;

    g_view = [[KPGLView alloc] initWithFrame:self.window.bounds];
    self.window.rootViewController = [[KPViewController alloc] init];
    [self.window makeKeyAndVisible];
    self.displayLink = [CADisplayLink displayLinkWithTarget:[[KPDisplayLinkTarget alloc] init] selector:@selector(tick:)];
    const float hz = (float)UIScreen.mainScreen.maximumFramesPerSecond;
    self.displayLink.preferredFrameRateRange = CAFrameRateRangeMake(std::min(60.0f, hz), hz, hz);
    [self.displayLink addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];

    AVAudioSession* audio = AVAudioSession.sharedInstance;
    [audio setCategory:AVAudioSessionCategoryPlayback error:nil];
    [audio setPreferredSampleRate:44100 error:nil];
    [audio setActive:YES error:nil];

    AppConfig cfg;
    cfg.hostExeDir = NSBundle.mainBundle.bundlePath.UTF8String;
    cfg.gameDir = gameDir;
    cfg.discDir = access((gameDir + "/track1.iso").c_str(), R_OK) == 0 ? gameDir + "/track1.iso" : gameDir + "/disc";
    for (NSString* f in [NSFileManager.defaultManager contentsOfDirectoryAtPath:[docs stringByAppendingPathComponent:@"cd"] error:nil])
        if ([f.pathExtension.lowercaseString isEqualToString:@"cue"])
            cfg.cuePath = gameDir + "/cd/" + f.UTF8String;
    for (NSString* a in NSProcessInfo.processInfo.arguments)
        if (a != NSProcessInfo.processInfo.arguments.firstObject)
            cfg.args.push_back(a.UTF8String);
    if (appInit(cfg))
        appStartGuest();
}
- (void)setActive:(bool)active
{
    {
        std::lock_guard lock(g_activeMutex);
        g_active = active;
    }
    g_activeCv.notify_all();
    input::activate(active);
}
- (void)sceneWillResignActive:(UIScene*)scene { [self setActive:false]; }
- (void)sceneDidBecomeActive:(UIScene*)scene { [self setActive:true]; }
@end

// ---------------------------------------------------------------- platform interface

namespace platform
{
    void showWindow(int, int, const char*) {}
    void hideWindow() {}
    void setFullscreen(bool) {}
    void setMouseCaptured(bool) {}

    void screenSize(int& w, int& h)
    {
        // Landscape, in pixels of the game view (screen bounds are in points).
        const CGSize b = UIScreen.mainScreen.bounds.size;
        const double k = contentScale();
        w = (int)(std::max(b.width, b.height) * k); // truncated, as the layer's pixel size is
        h = (int)(std::min(b.width, b.height) * k);
    }

    namespace
    {
        // (Re)attaches the window colour buffer to the layer's current size.
        void attachLayer()
        {
            if (!g_fbo)
            {
                glGenFramebuffers(1, &g_fbo);
                glGenRenderbuffers(1, &g_colorRb);
            }
            glBindRenderbuffer(GL_RENDERBUFFER, g_colorRb);
            __block CAEAGLLayer* layer;
            dispatch_sync(dispatch_get_main_queue(), ^{
                layer = (CAEAGLLayer*)g_view.layer;
            });
            [g_context renderbufferStorage:GL_RENDERBUFFER fromDrawable:layer];
            glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_WIDTH, &g_fbW);
            glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_HEIGHT, &g_fbH);
            GLint prev;
            glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev);
            glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, g_colorRb);
            glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev);
            logf("[gfx] window framebuffer %dx%d", g_fbW, g_fbH);
        }

        void waitUntilActive()
        {
            std::unique_lock lock(g_activeMutex);
            if (!g_active)
                glFinish();
            g_activeCv.wait(lock, [] { return g_active; });
        }
    }

    bool glCreate()
    {
        if (g_context)
            return true; // already made (for the intro, or before a video restart)
        g_context = [[EAGLContext alloc] initWithAPI:kEAGLRenderingAPIOpenGLES3];
        return g_context != nil;
    }

    void glMakeCurrent(int, int)
    {
        [EAGLContext setCurrentContext:g_context];
        // Wait for the landscape layout before sizing the window framebuffer.
        for (int i = 0; i < 200; i++)
        {
            __block CGSize s;
            dispatch_sync(dispatch_get_main_queue(), ^{
                s = g_view.bounds.size;
            });
            if (s.width > s.height)
                break;
            usleep(10000);
        }
        g_layerResized = false;
        attachLayer();
    }

    void glRelease() { [EAGLContext setCurrentContext:nil]; } // the context itself is kept (see mac.mm)

    void glPresent()
    {
        glBindRenderbuffer(GL_RENDERBUFFER, g_colorRb);
        [g_context presentRenderbuffer:GL_RENDERBUFFER];
        waitUntilActive();
        if (g_layerResized.exchange(false))
            attachLayer();
    }

    void glWaitVblank()
    {
        std::unique_lock lock(g_vblankMutex);
        const uint64_t seen = g_vblankCount;
        g_vblankCv.wait_for(lock, std::chrono::milliseconds(50), [&] { return g_vblankCount != seen; });
    }

    uint32_t glWindowFramebuffer() { return g_fbo; }

    void glDrawableSize(int& w, int& h)
    {
        w = g_fbW;
        h = g_fbH;
    }

    // The intro movie's frames (DirectDraw), before the game has set up GL.
    void presentRgba(const uint32_t* pixels, int w, int h)
    {
        if (!g_context)
        {
            glCreate();
            glMakeCurrent(w, h);
        }
        glBlitRgba(pixels, w, h, g_fbW, g_fbH);
        glPresent();
    }

    // ---- audio (RemoteIO)
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
        AudioComponentDescription desc{ kAudioUnitType_Output, kAudioUnitSubType_RemoteIO, kAudioUnitManufacturer_Apple, 0, 0 };
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
            logf("[audio] could not start RemoteIO output");
        else
            logf("[audio] RemoteIO output started (44.1 kHz stereo)");
    }
}

#include "apple_images.inc"

int main(int argc, char** argv)
{
#if TARGET_OS_SIMULATOR
    // The simulator's OpenGL ES renders on the CPU: lighter defaults for testing there.
    setenv("KP_RENDER_SCALE", "0.4", 0);
    setenv("KP_POST", "0", 0);
    setenv("KP_MSAA", "0", 0);
#endif
    @autoreleasepool
    {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass([KPAppDelegate class]));
    }
}
