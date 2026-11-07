// The Cocoa test window (spec §12): an NSWindow whose content view is layer-backed by a
// CAMetalLayer. Compiled with ARC (-fobjc-arc, set in CMakeLists.txt): State owns the window and
// the view as strong references, so destroying the State releases both. Main thread only.

#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#import <QuartzCore/CAMetalLayer.h>

#include "dvv/platform/testing/test_window.h"

// The content view: its backing layer is a CAMetalLayer, which a Vulkan surface is created on.
@interface DvvMetalView : NSView
@end

@implementation DvvMetalView
- (BOOL)wantsUpdateLayer {
    return YES;
}
- (CALayer*)makeBackingLayer {
    CAMetalLayer* const layer = [CAMetalLayer layer];
    layer.opaque = YES;
    return layer;
}
@end

namespace dvv::platform::testing {

// Owns the window and its view. The window is not released on close (releasedWhenClosed is NO), so
// ARC alone ends its life.
struct TestWindow::State {
    State() = default;
    State(const State&) = delete;
    State& operator=(const State&) = delete;
    ~State() {
        @autoreleasepool {
            [window orderOut:nil];
            [window close];
            window = nil;
            view = nil;
        }
    }

    NSWindow* window = nil;
    DvvMetalView* view = nil;  // a subview of the window's plain content view
};

namespace {

// Handles the events queued so far without waiting.
void drainEvents() {
    @autoreleasepool {
        for (;;) {
            NSEvent* const event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                                      untilDate:[NSDate distantPast]
                                                         inMode:NSDefaultRunLoopMode
                                                        dequeue:YES];
            if (event == nil) {
                break;
            }
            [NSApp sendEvent:event];
        }
        [NSApp updateWindows];
    }
}

// Polls `done` while draining events, for up to 2 s; true once it holds.
template <class Done>
bool waitUntil(Done done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    for (;;) {
        drainEvents();
        if (done()) {
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        @autoreleasepool {
            [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
                                     beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
        }
    }
}

// Window frames snap to whole points, so on a 2x display an odd pixel count cannot be a window
// size. The window is made large enough (rounded up) and the Metal view, which carries the layer,
// gets the exact size in points (physical / scale, possibly fractional): clientSize() reads it
// back. `State` is private to TestWindow, hence the template.
template <class S>
void applySize(S& state, PixelSize client_size) {
    const CGFloat scale = state.window.backingScaleFactor;
    const CGFloat width = client_size.width / scale;
    const CGFloat height = client_size.height / scale;
    [state.window setContentSize:NSMakeSize(std::ceil(width), std::ceil(height))];
    state.view.frame = NSMakeRect(0, 0, width, height);
    // drawableSize is not written: the Vulkan swapchain sets it.
    state.view.layer.frame = state.view.bounds;
    state.view.layer.contentsScale = scale;
}

bool hasWindowServerSession() {
    CFDictionaryRef const session = CGSessionCopyCurrentDictionary();
    if (session == nullptr) {
        return false;
    }
    CFRelease(session);
    return true;
}

std::uint32_t toPixels(CGFloat points, CGFloat scale) {
    const double pixels = std::round(static_cast<double>(points * scale));
    return pixels > 0.0 ? static_cast<std::uint32_t>(pixels) : 0U;
}

}  // namespace

Result<TestWindow> TestWindow::create(PixelSize client_size, std::string_view title) {
    if (client_size.empty()) {
        return makeError(Errc::Internal, "TestWindow::create: the client size is empty");
    }
    if (![NSThread isMainThread]) {
        return makeError(Errc::Internal, "TestWindow::create: not on the main thread");
    }
    if (!hasWindowServerSession()) {
        return makeError(Errc::Unsupported, "no display: no window server session");
    }
    @autoreleasepool {
        static std::once_flag application;
        std::call_once(application, [] {
            [NSApplication sharedApplication];
            [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
            [NSApp finishLaunching];
        });
        auto state = std::make_unique<State>();
        const NSWindowStyleMask style =
            NSWindowStyleMaskBorderless | NSWindowStyleMaskMiniaturizable;
        // The size is corrected to physical pixels below, once the window knows its scale.
        NSWindow* const window = [[NSWindow alloc]
            initWithContentRect:NSMakeRect(100, 100, client_size.width, client_size.height)
                      styleMask:style
                        backing:NSBackingStoreBuffered
                          defer:NO];
        if (window == nil) {
            return makeError(Errc::Internal, "the test window could not be created");
        }
        window.releasedWhenClosed = NO;
        window.contentMinSize = NSMakeSize(1, 1);
        window.animationBehavior = NSWindowAnimationBehaviorNone;
        window.title = [NSString stringWithUTF8String:std::string(title).c_str()];
        DvvMetalView* const view = [[DvvMetalView alloc] initWithFrame:NSMakeRect(0, 0, 1, 1)];
        view.wantsLayer = YES;
        view.autoresizingMask = NSViewNotSizable;
        window.contentView = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 1, 1)];
        [window.contentView addSubview:view];
        state->window = window;
        state->view = view;

        const CGFloat scale = window.backingScaleFactor;
        view.layer.contentsScale = scale;
        TestWindow result(std::move(state));
        applySize(*result.state_, client_size);
        [window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
        if (!waitUntil([&] { return window.visible; })) {
            return makeError(Errc::Internal, "the test window was not shown within 2 s");
        }
        return result;
    }
}

TestWindow::TestWindow(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}
TestWindow::TestWindow(TestWindow&& other) noexcept = default;
TestWindow& TestWindow::operator=(TestWindow&& other) noexcept = default;
TestWindow::~TestWindow() = default;

NativeWindow TestWindow::native() const noexcept {
    if (!state_) {
        return std::monostate{};
    }
    return MetalWindow{.layer = (__bridge void*)state_->view.layer};
}

PixelSize TestWindow::clientSize() const {
    @autoreleasepool {
        const NSSize points = state_->view.bounds.size;
        const CGFloat scale = state_->window.backingScaleFactor;
        return {.width = toPixels(points.width, scale), .height = toPixels(points.height, scale)};
    }
}

void TestWindow::resize(PixelSize client_size) {
    @autoreleasepool {
        applySize(*state_, client_size);
        static_cast<void>(waitUntil([&] { return clientSize() == client_size; }));
    }
}

void TestWindow::minimise() {
    @autoreleasepool {
        [state_->window miniaturize:nil];
        static_cast<void>(waitUntil([&] { return state_->window.miniaturized; }));
    }
}

void TestWindow::restore() {
    @autoreleasepool {
        [state_->window deminiaturize:nil];
        static_cast<void>(waitUntil([&] { return !state_->window.miniaturized; }));
    }
}

void TestWindow::pump() {
    drainEvents();
}

}  // namespace dvv::platform::testing
