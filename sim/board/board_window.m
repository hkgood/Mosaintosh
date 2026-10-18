/*
 * Mosaico board simulator: a window onto the simulated panel.
 *
 * Shows the CO5300's pixels as the firmware left them -- including anything
 * it forgot to clear -- and turns the mouse into a finger on the CST9220.
 * The firmware itself runs on the application thread started by
 * board_start(); this file only looks and touches.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#import <Cocoa/Cocoa.h>

#include "board.h"

/* Initial window size relative to the panel. Resizing keeps the aspect. */
#define BOARD_WINDOW_SCALE 1.5

/* Window refresh rate. The panel itself updates whenever the firmware draws. */
#define BOARD_WINDOW_HZ 60.0

@interface BoardView : NSView
@property(nonatomic, strong) NSBitmapImageRep *rep;
@end

@implementation BoardView {
    uint16_t _panel[BOARD_PANEL_W * BOARD_PANEL_H];
}

/* Top-left origin, like the panel, so mouse and pixel rows agree. */
- (BOOL)isFlipped { return YES; }

- (BOOL)acceptsFirstResponder { return YES; }

- (void)touch:(NSEvent *)e pressed:(BOOL)pressed
{
    const NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    const NSSize s = self.bounds.size;
    board_touch_set(pressed, (int)(p.x * BOARD_PANEL_W / s.width),
                    (int)(p.y * BOARD_PANEL_H / s.height));
}

- (void)mouseDown:(NSEvent *)e { [self touch:e pressed:YES]; }
- (void)mouseDragged:(NSEvent *)e { [self touch:e pressed:YES]; }
- (void)mouseUp:(NSEvent *)e { [self touch:e pressed:NO]; }

- (void)keyDown:(NSEvent *)e
{
    if (e.keyCode == 0x35 /* Escape */ || [e.charactersIgnoringModifiers isEqualToString:@"q"]) {
        [NSApp terminate:nil];
    }
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    if (self.rep == nil) {
        self.rep = [[NSBitmapImageRep alloc]
            initWithBitmapDataPlanes:NULL
                          pixelsWide:BOARD_PANEL_W
                          pixelsHigh:BOARD_PANEL_H
                       bitsPerSample:8
                     samplesPerPixel:3
                            hasAlpha:NO
                            isPlanar:NO
                      colorSpaceName:NSDeviceRGBColorSpace
                         bytesPerRow:BOARD_PANEL_W * 3
                        bitsPerPixel:24];
    }

    /* Full RGB565 -> RGB888: firmware may draw a colour frame around the Mac display. */
    board_panel_snapshot(_panel);
    uint8_t *dst = self.rep.bitmapData;
    for (size_t i = 0; i < BOARD_PANEL_W * BOARD_PANEL_H; i++) {
        const uint16_t v = board_wire_to_rgb565(_panel[i]);
        const uint8_t r = (uint8_t)((v >> 11) & 0x1f);
        const uint8_t g = (uint8_t)((v >> 5) & 0x3f);
        const uint8_t b = (uint8_t)(v & 0x1f);
        dst[i * 3 + 0] = (uint8_t)((r << 3) | (r >> 2));
        dst[i * 3 + 1] = (uint8_t)((g << 2) | (g >> 4));
        dst[i * 3 + 2] = (uint8_t)((b << 3) | (b >> 2));
    }

    [NSGraphicsContext currentContext].imageInterpolation = NSImageInterpolationNone;
    [self.rep drawInRect:self.bounds
                fromRect:NSZeroRect
               operation:NSCompositingOperationCopy
                fraction:1.0
          respectFlipped:YES
                   hints:nil];
}

@end

void board_window_run(const char *title)
{
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

        const NSRect frame = NSMakeRect(0, 0, BOARD_PANEL_W * BOARD_WINDOW_SCALE,
                                        BOARD_PANEL_H * BOARD_WINDOW_SCALE);
        NSWindow *win = [[NSWindow alloc]
            initWithContentRect:frame
                      styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable
                        backing:NSBackingStoreBuffered
                          defer:NO];
        win.contentAspectRatio = NSMakeSize(1, 1);
        [win center];

        BoardView *view = [[BoardView alloc] initWithFrame:frame];
        [win setContentView:view];
        [win makeKeyAndOrderFront:nil];
        [win makeFirstResponder:view];
        [NSApp activateIgnoringOtherApps:YES];

        /* Close button quits, as unplugging the board would. */
        [[NSNotificationCenter defaultCenter]
            addObserverForName:NSWindowWillCloseNotification
                        object:win
                         queue:nil
                    usingBlock:^(NSNotification *n) {
                        (void)n;
                        [NSApp terminate:nil];
                    }];

        NSString *base = [NSString stringWithUTF8String:title];
        __block int ticks = 0;
        [NSTimer scheduledTimerWithTimeInterval:1.0 / BOARD_WINDOW_HZ
                                        repeats:YES
                                          block:^(NSTimer *t) {
                                              (void)t;
                                              [view setNeedsDisplay:YES];
                                              /* Problems in the title, so they are
                                               * not only in a terminal nobody reads. */
                                              if (++ticks % (int)BOARD_WINDOW_HZ == 0) {
                                                  const board_stats_t st = board_stats();
                                                  win.title = [NSString stringWithFormat:
                                                      @"%@ - %u transfers, %u errors, %u warnings, %u bad windows",
                                                      base, st.transfers, st.errors, st.warnings,
                                                      st.window_violations];
                                              }
                                          }];
        win.title = base;
        [NSApp run];
    }
}
