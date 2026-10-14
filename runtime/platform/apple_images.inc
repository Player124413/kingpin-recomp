// ImageIO image decode / PNG encode, shared by the macOS and iOS platform layers.
// (Included into platform/mac.mm and platform/ios.mm.)
#import <ImageIO/ImageIO.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

namespace platform
{
    bool decodeImage(const std::string& path, std::vector<uint8_t>& rgba, int& w, int& h)
    {
        @autoreleasepool
        {
            NSData* d = [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:path.c_str()]];
            if (!d)
                return false;
            CGImageSourceRef src = CGImageSourceCreateWithData((__bridge CFDataRef)d, nullptr);
            CGImageRef img = src ? CGImageSourceCreateImageAtIndex(src, 0, nullptr) : nullptr;
            if (src)
                CFRelease(src);
            if (!img)
                return false;
            w = (int)CGImageGetWidth(img);
            h = (int)CGImageGetHeight(img);
            rgba.assign((size_t)w * h * 4, 0);
            CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
            // Unpremultiplied RGBA is not a CGBitmapContext format: draw premultiplied, then undo it.
            CGContextRef ctx = CGBitmapContextCreate(rgba.data(), w, h, 8, (size_t)w * 4, cs, kCGImageAlphaPremultipliedLast);
            CGContextSetBlendMode(ctx, kCGBlendModeCopy);
            CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), img);
            CGContextRelease(ctx);
            CGColorSpaceRelease(cs);
            CGImageRelease(img);
            for (size_t i = 0; i < rgba.size(); i += 4)
            {
                const int a = rgba[i + 3];
                if (a && a < 255)
                    for (int ch = 0; ch < 3; ch++)
                        rgba[i + ch] = (uint8_t)std::min(255, rgba[i + ch] * 255 / a);
            }
            return true;
        }
    }

    bool writePng(const std::string& path, const uint8_t* rgba, int w, int h)
    {
        @autoreleasepool
        {
            CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
            CGContextRef ctx = CGBitmapContextCreate((void*)rgba, w, h, 8, (size_t)w * 4, cs, kCGImageAlphaNoneSkipLast);
            CGImageRef img = CGBitmapContextCreateImage(ctx);
            NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
            CGImageDestinationRef dst =
                CGImageDestinationCreateWithURL((__bridge CFURLRef)url, (__bridge CFStringRef)UTTypePNG.identifier, 1, nullptr);
            bool ok = false;
            if (dst)
            {
                CGImageDestinationAddImage(dst, img, nullptr);
                ok = CGImageDestinationFinalize(dst);
                CFRelease(dst);
            }
            CGImageRelease(img);
            CGContextRelease(ctx);
            CGColorSpaceRelease(cs);
            return ok;
        }
    }
}
