#include "ui/platform/ui_utility_mac.h"

#include <Cocoa/Cocoa.h>

namespace TeleMatrix::Platform {

void AcceptAllMouseInput(QWidget *widget) {
    if (!widget) {
        return;
    }
    NSWindow *window = [reinterpret_cast<NSView*>(widget->winId()) window];
    [window setIgnoresMouseEvents:NO];
}

void ActivateApp() {
    [NSApp activateIgnoringOtherApps:YES];
}

void ForcePointingHandCursor() {
    [[NSCursor pointingHandCursor] set];
}

void ForceArrowCursor() {
    [[NSCursor arrowCursor] set];
}

void ForceIBeamCursor() {
    [[NSCursor IBeamCursor] set];
}

void ForceWindowSRGB(QWidget *widget) {
    if (!widget) {
        return;
    }
    NSWindow *window = [reinterpret_cast<NSView*>(widget->winId()) window];
    [window setColorSpace:[NSColorSpace sRGBColorSpace]];
}

} // namespace TeleMatrix::Platform
