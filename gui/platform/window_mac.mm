#include "platform/window.hh"

#include <QGuiApplication>
#include <QWidget>

#import <AppKit/AppKit.h>

namespace astral::gui::platform {

void applyWindowShape(QWidget *window, int radius)
{
    // Only the cocoa platform hands back an NSView; offscreen and the test
    // platforms return a handle that means something else entirely, and
    // treating it as an object is a crash rather than a wrong corner radius.
    if (QGuiApplication::platformName() != QLatin1String("cocoa"))
        return;
    if (window == nullptr || !window->isWindow())
        return;
    // The content layer clips to a rounded rect, which AppKit antialiases and
    // the window shadow follows. The window itself must be clear so the
    // corners show what is behind them.
    NSView *view = (__bridge NSView *)reinterpret_cast<void *>(window->winId());
    if (!view)
        return;
    NSWindow *native = [view window];
    if (!native)
        return;
    native.opaque = NO;
    native.backgroundColor = [NSColor clearColor];
    native.hasShadow = YES;
    view.wantsLayer = YES;
    view.layer.cornerRadius = radius;
    view.layer.masksToBounds = radius > 0;
    [native invalidateShadow];
}

} // namespace astral::gui::platform
