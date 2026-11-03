#include "cafe/platform/DesktopWindow.hpp"

#import <Cocoa/Cocoa.h>
#import <GameController/GameController.h>

#include <algorithm>
#include <cmath>

namespace cafe {
struct DesktopWindow::Impl {
    NSWindow* window{nil};
    NSView* view{nil};
    bool running{true};
    bool keys[256]{};
    Display pointerDisplay{Display::GamePad};
    double pointerX{0};
    double pointerY{0};
    bool pointerInside{false};
    bool mouseDown{false};
    bool mouseRight{false};
    int paneWidth{640};
    int paneHeight{480};
    const std::vector<Color>* tvPixels{nullptr};
    const std::vector<Color>* padPixels{nullptr};
};
}

@interface CafeView : NSView
@property(nonatomic, assign) cafe::DesktopWindow::Impl* impl;
@end

@interface CafeWindowDelegate : NSObject <NSWindowDelegate>
@property(nonatomic, assign) cafe::DesktopWindow::Impl* impl;
@end

@implementation CafeWindowDelegate
- (BOOL)windowShouldClose:(NSWindow*)sender {
    (void)sender;
    if (self.impl) self.impl->running = false;
    return YES;
}
@end

@implementation CafeView
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)isFlipped { return NO; }

- (void)updateTracking {
    for (NSTrackingArea* area in self.trackingAreas) [self removeTrackingArea:area];
    NSTrackingArea* area = [[NSTrackingArea alloc]
        initWithRect:self.bounds
        options:NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited | NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect
        owner:self
        userInfo:nil];
    [self addTrackingArea:area];
}

- (void)viewDidMoveToWindow {
    [super viewDidMoveToWindow];
    [self updateTracking];
}

- (void)setFrameSize:(NSSize)newSize {
    [super setFrameSize:newSize];
    [self updateTracking];
    [self setNeedsDisplay:YES];
    if (self.window && self.impl) {
        const NSPoint windowPoint = [self.window mouseLocationOutsideOfEventStream];
        [self mapPoint:[self convertPoint:windowPoint fromView:nil]];
    }
}

- (void)mapPoint:(NSPoint)local {
    if (!self.impl) return;
    const double width = self.bounds.size.width;
    const double height = self.bounds.size.height;
    const cafe::DualPaneLayout panes = cafe::splitDisplayPanes(width, height);
    double x = local.x;
    double y = self.bounds.size.height - local.y;
    self.impl->pointerInside = false;
    if (x < 0.0 || y < 0.0 || x > width || y > height ||
        self.impl->paneWidth <= 0 || self.impl->paneHeight <= 0) return;

    const bool overTvPane = x >= panes.left.x && x < panes.left.x + panes.left.width;
    const bool overGamePadPane = x >= panes.right.x && x <= panes.right.x + panes.right.width;
    if (!overTvPane && !overGamePadPane) return;
    self.impl->pointerDisplay = overTvPane ? cafe::Display::Tv : cafe::Display::GamePad;
    const cafe::Rect& pane = overTvPane ? panes.left : panes.right;
    const double paneX = x - pane.x;
    const cafe::Rect viewport = cafe::aspectFitViewport(
        self.impl->paneWidth, self.impl->paneHeight, pane.width, pane.height);
    const auto point = cafe::outputToVirtual(
        viewport, self.impl->paneWidth, self.impl->paneHeight, paneX, y);
    self.impl->pointerInside = point.has_value();
    if (point) {
        self.impl->pointerX = point->x;
        self.impl->pointerY = point->y;
    }
}

- (void)mouseExited:(NSEvent*)event {
    (void)event;
    if (self.impl) self.impl->pointerInside = false;
}

- (void)mouseDown:(NSEvent*)event {
    [self mapPoint:[self convertPoint:event.locationInWindow fromView:nil]];
    if (self.impl) self.impl->mouseDown = true;
}

- (void)mouseUp:(NSEvent*)event {
    [self mapPoint:[self convertPoint:event.locationInWindow fromView:nil]];
    if (self.impl) self.impl->mouseDown = false;
}

- (void)rightMouseDown:(NSEvent*)event {
    [self mapPoint:[self convertPoint:event.locationInWindow fromView:nil]];
    if (self.impl) self.impl->mouseRight = true;
}

- (void)rightMouseUp:(NSEvent*)event {
    [self mapPoint:[self convertPoint:event.locationInWindow fromView:nil]];
    if (self.impl) self.impl->mouseRight = false;
}

- (void)mouseDragged:(NSEvent*)event {
    [self mapPoint:[self convertPoint:event.locationInWindow fromView:nil]];
}

- (void)mouseMoved:(NSEvent*)event {
    [self mapPoint:[self convertPoint:event.locationInWindow fromView:nil]];
}

- (void)keyDown:(NSEvent*)event {
    if (self.impl && event.keyCode < 256) self.impl->keys[event.keyCode] = true;
}

- (void)keyUp:(NSEvent*)event {
    if (self.impl && event.keyCode < 256) self.impl->keys[event.keyCode] = false;
}

- (void)flagsChanged:(NSEvent*)event {
    (void)event;
}

- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    if (!self.impl) return;
    const int width = self.impl->paneWidth;
    const int height = self.impl->paneHeight;
    if (width <= 0 || height <= 0) return;

    auto drawPane = [&](const std::vector<cafe::Color>* pixels, NSRect dest) {
        [[NSColor blackColor] set];
        NSRectFill(dest);
        if (!pixels || pixels->size() < static_cast<std::size_t>(width * height)) return;
        CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
        CGDataProviderRef provider = CGDataProviderCreateWithData(
            nullptr, pixels->data(), pixels->size() * sizeof(cafe::Color), nullptr);
        CGImageRef image = CGImageCreate(
            width, height, 8, 32, static_cast<size_t>(width) * 4, space,
            kCGImageAlphaNoneSkipLast, provider, nullptr, false,
            kCGRenderingIntentDefault);
        CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];
        const cafe::Rect viewport = cafe::aspectFitViewport(
            width, height, dest.size.width, dest.size.height);
        const NSRect fitted = NSMakeRect(dest.origin.x + viewport.x,
                                         dest.origin.y + viewport.y,
                                         viewport.width, viewport.height);
        CGContextDrawImage(ctx, fitted, image);
        CGImageRelease(image);
        CGDataProviderRelease(provider);
        CGColorSpaceRelease(space);
    };

    const NSRect bounds = self.bounds;
    const cafe::DualPaneLayout panes = cafe::splitDisplayPanes(
        bounds.size.width, bounds.size.height);
    const NSRect tvPane = NSMakeRect(panes.left.x, panes.left.y,
                                     panes.left.width, panes.left.height);
    const NSRect gamePadPane = NSMakeRect(panes.right.x, panes.right.y,
                                          panes.right.width, panes.right.height);
    drawPane(self.impl->tvPixels, tvPane);
    drawPane(self.impl->padPixels, gamePadPane);

    [[NSColor colorWithWhite:0.85 alpha:1] set];
    NSRectFill(NSMakeRect(panes.divider.x, panes.divider.y,
                          panes.divider.width, panes.divider.height));

    NSDictionary* attrs = @{
        NSFontAttributeName: [NSFont boldSystemFontOfSize:11],
        NSForegroundColorAttributeName: [NSColor colorWithWhite:1 alpha:0.85]
    };
    [@"TV" drawAtPoint:NSMakePoint(8, bounds.size.height - 22) withAttributes:attrs];
    [@"GamePad" drawAtPoint:NSMakePoint(panes.right.x + 8, bounds.size.height - 22)
               withAttributes:attrs];
}
@end

namespace cafe {
namespace {

void orButton(bool& held, bool down) {
    if (down) held = true;
}

} // namespace

DesktopWindow::DesktopWindow() : impl_(std::make_unique<Impl>()) {}
DesktopWindow::~DesktopWindow() {
    if (impl_ && impl_->window) {
        impl_->window.delegate = nil;
        [impl_->window close];
        impl_->window = nil;
    }
}

bool DesktopWindow::open(int paneWidth, int paneHeight) {
    impl_->paneWidth = paneWidth;
    impl_->paneHeight = paneHeight;

    if (NSApp == nil) {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    }

    const NSRect frame = NSMakeRect(0, 0, paneWidth * 2, paneHeight);
    NSWindow* window = [[NSWindow alloc]
        initWithContentRect:frame
        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                  NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
        backing:NSBackingStoreBuffered
        defer:NO];
    window.title = @"Cafe Engine — TV | GamePad";
    window.releasedWhenClosed = NO;
    window.contentMinSize = NSMakeSize(480, 180);

    CafeView* view = [[CafeView alloc] initWithFrame:frame];
    view.impl = impl_.get();
    view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    window.contentView = view;
    CafeWindowDelegate* delegate = [[CafeWindowDelegate alloc] init];
    delegate.impl = impl_.get();
    window.delegate = delegate;
    [window center];
    [window makeKeyAndOrderFront:nil];
    [window makeFirstResponder:view];
    [NSApp activateIgnoringOtherApps:YES];
    [GCController startWirelessControllerDiscoveryWithCompletionHandler:nil];

    impl_->window = window;
    impl_->view = view;
    impl_->running = true;
    return true;
}

bool DesktopWindow::poll(InputSystem& input) {
    if (!impl_->running || !impl_->window) return false;

    NSEvent* event = nil;
    while ((event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                       untilDate:[NSDate distantPast]
                                          inMode:NSDefaultRunLoopMode
                                         dequeue:YES])) {
        [NSApp sendEvent:event];
    }

    auto key = [&](KeyCode code, bool down) {
        holdButton(input.keyboard.keys[static_cast<std::size_t>(code)], down);
    };
    // Normalize Cocoa's hardware key codes into CafeScript's portable key
    // names. Games never need to know the host keyboard layout indices.
    static constexpr std::pair<int, char> letters[] = {
        {0,'a'},{11,'b'},{8,'c'},{2,'d'},{14,'e'},{3,'f'},{5,'g'},{4,'h'},
        {34,'i'},{38,'j'},{40,'k'},{37,'l'},{46,'m'},{45,'n'},{31,'o'},
        {35,'p'},{12,'q'},{15,'r'},{1,'s'},{17,'t'},{32,'u'},{9,'v'},
        {13,'w'},{7,'x'},{16,'y'},{6,'z'}
    };
    for (const auto [hardware, ascii] : letters)
        holdButton(input.keyboard.keys[static_cast<unsigned char>(ascii)], impl_->keys[hardware]);
    key(KeyCode::Backspace, impl_->keys[51]);
    key(KeyCode::Enter, impl_->keys[36] || impl_->keys[76]);
    key(KeyCode::Escape, impl_->keys[53]);
    key(KeyCode::Space, impl_->keys[49]);
    key(KeyCode::Left, impl_->keys[123]);
    key(KeyCode::Right, impl_->keys[124]);
    key(KeyCode::Down, impl_->keys[125]);
    key(KeyCode::Up, impl_->keys[126]);

    const bool overTv = impl_->pointerInside && impl_->pointerDisplay == Display::Tv;
    const bool overPad = impl_->pointerInside && impl_->pointerDisplay == Display::GamePad;

    bool remoteA = impl_->keys[6] || impl_->keys[49] || impl_->keys[36] || (overTv && impl_->mouseDown);
    bool remoteB = impl_->keys[7] || impl_->keys[53] || (overTv && impl_->mouseRight);
    bool remoteOne = impl_->keys[18];
    bool remoteTwo = impl_->keys[19];
    bool remoteUp = impl_->keys[13] || impl_->keys[126];
    bool remoteDown = impl_->keys[1] || impl_->keys[125];
    bool remoteLeft = impl_->keys[0] || impl_->keys[123];
    bool remoteRight = impl_->keys[2] || impl_->keys[124];
    bool remotePlus = impl_->keys[24];
    bool remoteMinus = impl_->keys[27];
    bool remoteHome = impl_->keys[4];
    bool nunchukC = impl_->keys[8];
    bool nunchukZ = impl_->keys[9];

    input.wiiRemote.connected = true;
    input.wiiRemote.pointerValid = overTv;
    if (overTv) {
        input.wiiRemote.x = impl_->pointerX;
        input.wiiRemote.y = impl_->pointerY;
    }
    holdButton(input.wiiRemote.A, remoteA);
    holdButton(input.wiiRemote.B, remoteB);
    holdButton(input.wiiRemote.one, remoteOne);
    holdButton(input.wiiRemote.two, remoteTwo);
    holdButton(input.wiiRemote.plus, remotePlus);
    holdButton(input.wiiRemote.minus, remoteMinus);
    holdButton(input.wiiRemote.home, remoteHome);
    holdButton(input.wiiRemote.up, remoteUp);
    holdButton(input.wiiRemote.down, remoteDown);
    holdButton(input.wiiRemote.left, remoteLeft);
    holdButton(input.wiiRemote.right, remoteRight);
    holdButton(input.wiiRemote.nunchuk.C, nunchukC);
    holdButton(input.wiiRemote.nunchuk.Z, nunchukZ);

    bool a = false, b = false, x = false, y = false;
    bool up = false, down = false, left = false, right = false;
    bool l = false, r = false, zl = false, zr = false, plus = false, minus = false;
    double leftX = 0, leftY = 0, rightX = 0, rightY = 0;

    for (GCController* controller in GCController.controllers) {
        GCExtendedGamepad* pad = controller.extendedGamepad;
        if (!pad) continue;
        orButton(a, pad.buttonA.pressed);
        orButton(b, pad.buttonB.pressed);
        orButton(x, pad.buttonX.pressed);
        orButton(y, pad.buttonY.pressed);
        orButton(l, pad.leftShoulder.pressed);
        orButton(r, pad.rightShoulder.pressed);
        orButton(up, pad.dpad.up.pressed);
        orButton(down, pad.dpad.down.pressed);
        orButton(left, pad.dpad.left.pressed);
        orButton(right, pad.dpad.right.pressed);
        orButton(zl, pad.leftTrigger.value > 0.5f);
        orButton(zr, pad.rightTrigger.value > 0.5f);
        leftX = pad.leftThumbstick.xAxis.value;
        leftY = pad.leftThumbstick.yAxis.value;
        rightX = pad.rightThumbstick.xAxis.value;
        rightY = pad.rightThumbstick.yAxis.value;
        if (@available(macOS 10.15, *)) {
            if (pad.buttonMenu) orButton(plus, pad.buttonMenu.pressed);
        }
        constexpr double zone = 0.4;
        if (leftX > zone) right = true;
        if (leftX < -zone) left = true;
        if (leftY > zone) up = true;
        if (leftY < -zone) down = true;
    }

    holdButton(input.gamePad.A, a);
    holdButton(input.gamePad.B, b);
    holdButton(input.gamePad.X, x);
    holdButton(input.gamePad.Y, y);
    holdButton(input.gamePad.L, l);
    holdButton(input.gamePad.R, r);
    holdButton(input.gamePad.ZL, zl);
    holdButton(input.gamePad.ZR, zr);
    holdButton(input.gamePad.plus, plus);
    holdButton(input.gamePad.minus, minus);
    holdButton(input.gamePad.up, up);
    holdButton(input.gamePad.down, down);
    holdButton(input.gamePad.left, left);
    holdButton(input.gamePad.right, right);
    input.gamePad.leftStick.x = leftX;
    input.gamePad.leftStick.y = leftY;
    input.gamePad.rightStick.x = rightX;
    input.gamePad.rightStick.y = rightY;

    input.pointerDisplay = impl_->pointerDisplay;
    input.mouse.x = impl_->pointerX;
    input.mouse.y = impl_->pointerY;
    holdButton(input.mouse.left, overPad && impl_->mouseDown);
    holdButton(input.mouse.right, overPad && impl_->mouseRight);
    return impl_->running;
}

void DesktopWindow::present(const std::vector<Color>& tv, const std::vector<Color>& gamePad, int width, int height) {
    if (!impl_->view) return;
    impl_->paneWidth = width;
    impl_->paneHeight = height;
    impl_->tvPixels = &tv;
    impl_->padPixels = &gamePad;
    [impl_->view setNeedsDisplay:YES];
    [impl_->window displayIfNeeded];
}

} // namespace cafe
