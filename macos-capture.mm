#ifdef __APPLE__

#import <Foundation/Foundation.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <ImageIO/ImageIO.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

namespace {
void set_error(char* buffer, size_t capacity, NSString* message) {
    if (!buffer || capacity == 0) return;
    const char* text = message ? message.UTF8String : "unknown ScreenCaptureKit error";
    std::snprintf(buffer, capacity, "%s", text ? text : "unknown ScreenCaptureKit error");
}

std::string copy_string(NSString* message, const char* fallback) {
    if (!message) return fallback;
    const char* text = message.UTF8String;
    return text ? std::string(text) : std::string(fallback);
}
}

extern "C" int bs_macos_capture_png(const char* output_path,
                                     unsigned max_width,
                                     char* error_buffer,
                                     size_t error_capacity) {
    @autoreleasepool {
        if (!output_path || !*output_path) {
            set_error(error_buffer, error_capacity, @"empty capture output path");
            return 0;
        }

        __block BOOL success = NO;
        __block std::string failure;
        dispatch_semaphore_t done = dispatch_semaphore_create(0);

        [SCShareableContent
            getShareableContentExcludingDesktopWindows:NO
            onScreenWindowsOnly:NO
            completionHandler:^(SCShareableContent* content, NSError* error) {
                if (error || content.displays.count == 0) {
                    failure = copy_string(error.localizedDescription, "no shareable displays");
                    dispatch_semaphore_signal(done);
                    return;
                }

                SCDisplay* display = content.displays.firstObject;
                const CGDirectDisplayID main_id = CGMainDisplayID();
                for (SCDisplay* candidate in content.displays) {
                    if (candidate.displayID == main_id) {
                        display = candidate;
                        break;
                    }
                }

                SCContentFilter* filter =
                    [[SCContentFilter alloc] initWithDisplay:display excludingWindows:@[]];
                SCStreamConfiguration* config = [[SCStreamConfiguration alloc] init];
                const size_t source_width = std::max<NSInteger>(1, display.width);
                const size_t source_height = std::max<NSInteger>(1, display.height);
                const size_t requested_width = max_width == 0 ? source_width : max_width;
                config.width = std::min(source_width, requested_width);
                config.height = std::max<size_t>(2,
                    (source_height * config.width / source_width) & ~size_t{1});
                config.showsCursor = YES;
                config.scalesToFit = YES;
                config.preservesAspectRatio = YES;
                config.pixelFormat = 'BGRA';

                [SCScreenshotManager
                    captureImageWithFilter:filter
                    configuration:config
                    completionHandler:^(CGImageRef image, NSError* capture_error) {
                        if (capture_error || image == nullptr) {
                            failure = copy_string(capture_error.localizedDescription,
                                                  "ScreenCaptureKit returned no image");
                            dispatch_semaphore_signal(done);
                            return;
                        }

                        NSURL* url = [NSURL fileURLWithPath:
                            [NSString stringWithUTF8String:output_path]];
                        CGImageDestinationRef destination = CGImageDestinationCreateWithURL(
                            (__bridge CFURLRef)url,
                            (__bridge CFStringRef)UTTypePNG.identifier,
                            1,
                            nullptr);
                        if (!destination) {
                            failure = "failed to create PNG destination";
                        } else {
                            CGImageDestinationAddImage(destination, image, nullptr);
                            success = CGImageDestinationFinalize(destination);
                            if (!success) failure = "failed to finalize PNG capture";
                            CFRelease(destination);
                        }
                        dispatch_semaphore_signal(done);
                    }];
            }];

        const long wait_result = dispatch_semaphore_wait(
            done, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC));
        if (wait_result != 0) failure = "ScreenCaptureKit timed out";
        if (!success && error_buffer && error_capacity) {
            std::snprintf(error_buffer, error_capacity, "%s",
                          failure.empty() ? "unknown ScreenCaptureKit error" : failure.c_str());
        }
        return success ? 1 : 0;
    }
}

#endif
