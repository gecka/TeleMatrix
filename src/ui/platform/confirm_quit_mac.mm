#include "ui/platform/confirm_quit_mac.h"

#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>

namespace {

constexpr int64_t kShowDurationMs = 1500;
constexpr int64_t kWindowFadeOutDurationMs = 200;
constexpr int64_t kTimeDeltaFuzzMs = 1000;

[[nodiscard]] int64_t nowMs() {
	return int64_t([[NSProcessInfo processInfo] systemUptime] * 1000.0);
}

[[nodiscard]] NSString *toNSString(const QString &str) {
	return str.toNSString();
}

[[nodiscard]] QString fromNSString(NSString *str) {
	return QString::fromNSString(str);
}

} // namespace

@class TMConfirmQuitFrameView;

@interface TMConfirmQuitPanelController : NSWindowController<NSWindowDelegate> {
@private
	TMConfirmQuitFrameView *_contentView;
	NSString *_message;
}

+ (TMConfirmQuitPanelController *)sharedControllerWithMessage:(NSString *)message;
- (BOOL)runModalLoopForApplication:(NSApplication *)app;
- (void)showWindow:(id)sender;
- (void)dismissPanel;

@end

@interface TMConfirmQuitFrameView : NSView {
@private
	NSTextField *_messageField;
}
- (void)setMessageText:(NSString *)text;
@end

@implementation TMConfirmQuitFrameView

- (instancetype)initWithFrame:(NSRect)frameRect {
	if ((self = [super initWithFrame:frameRect])) {
		_messageField = [[NSTextField alloc] initWithFrame:NSZeroRect];
		[_messageField setEditable:NO];
		[_messageField setSelectable:NO];
		[_messageField setBezeled:NO];
		[_messageField setDrawsBackground:NO];
		[_messageField setFont:[NSFont boldSystemFontOfSize:24]];
		[_messageField setTextColor:[NSColor whiteColor]];
		[self addSubview:_messageField];
		[_messageField release];
	}
	return self;
}

- (void)drawRect:(NSRect)dirtyRect {
	Q_UNUSED(dirtyRect);
	const CGFloat kCornerRadius = 9.0;
	NSBezierPath *path = [NSBezierPath
		bezierPathWithRoundedRect:[self bounds]
		xRadius:kCornerRadius
		yRadius:kCornerRadius];
	NSColor *fillColor = [NSColor colorWithCalibratedWhite:0.2 alpha:0.75];
	[fillColor set];
	[path fill];
}

- (void)setMessageText:(NSString *)text {
	const CGFloat kHorizontalPadding = 30;

	NSMutableAttributedString *attrString =
		[[NSMutableAttributedString alloc] initWithString:text];
	NSShadow *textShadow = [[NSShadow alloc] init];
	[textShadow setShadowColor:[NSColor colorWithCalibratedWhite:0 alpha:0.6]];
	[textShadow setShadowOffset:NSMakeSize(0, -1)];
	[textShadow setShadowBlurRadius:1.0];
	[attrString addAttribute:NSShadowAttributeName
		value:textShadow
		range:NSMakeRange(0, [text length])];
	[_messageField setAttributedStringValue:attrString];
	[textShadow release];
	[attrString release];

	[_messageField sizeToFit];
	NSRect messageFrame = [_messageField frame];
	NSRect frameInViewSpace =
		[_messageField convertRect:[[self window] frame] fromView:nil];

	if (NSWidth(messageFrame) > NSWidth(frameInViewSpace)) {
		frameInViewSpace.size.width = NSWidth(messageFrame) + kHorizontalPadding;
	}

	messageFrame.origin.x = NSWidth(frameInViewSpace) / 2 - NSMidX(messageFrame);
	messageFrame.origin.y = NSHeight(frameInViewSpace) / 2 - NSMidY(messageFrame);

	[[self window]
		setFrame:[_messageField convertRect:frameInViewSpace toView:nil]
		display:YES];
	[_messageField setFrame:messageFrame];
}

@end

@interface TMFadeAllWindowsAnimation : NSAnimation<NSAnimationDelegate> {
@private
	NSApplication *_application;
}
- (instancetype)initWithApplication:(NSApplication *)app
	animationDuration:(NSTimeInterval)duration;
@end

@implementation TMFadeAllWindowsAnimation

- (instancetype)initWithApplication:(NSApplication *)app
	animationDuration:(NSTimeInterval)duration {
	if ((self = [super initWithDuration:duration
		animationCurve:NSAnimationLinear])) {
		_application = app;
		[self setDelegate:self];
	}
	return self;
}

- (void)setCurrentProgress:(NSAnimationProgress)progress {
	for (NSWindow *window in [_application windows]) {
		[window setAlphaValue:1.0 - progress];
	}
}

- (void)animationDidStop:(NSAnimation *)animation {
	Q_UNUSED(animation);
	[self autorelease];
}

@end

@interface TMConfirmQuitPanelController (Private) <CAAnimationDelegate>
- (void)animateFadeOut;
- (NSEvent *)pumpEventQueueForKeyUp:(NSApplication *)app untilDate:(NSDate *)date;
- (void)hideAllWindowsForApplication:(NSApplication *)app
	withDuration:(NSTimeInterval)duration;
- (void)sendAccessibilityAnnouncement;
@end

namespace {

TMConfirmQuitPanelController *gConfirmQuitPanelController = nil;

} // namespace

@implementation TMConfirmQuitPanelController

+ (TMConfirmQuitPanelController *)sharedControllerWithMessage:(NSString *)message {
	if (!gConfirmQuitPanelController) {
		gConfirmQuitPanelController =
			[[TMConfirmQuitPanelController alloc] initWithMessage:message];
	}
	return [[gConfirmQuitPanelController retain] autorelease];
}

- (instancetype)initWithMessage:(NSString *)message {
	const NSRect kWindowFrame = NSMakeRect(0, 0, 350, 70);
	NSWindow *window = [[NSWindow alloc]
		initWithContentRect:kWindowFrame
		styleMask:NSWindowStyleMaskBorderless
		backing:NSBackingStoreBuffered
		defer:NO];
	if (!(self = [super initWithWindow:window])) {
		[window release];
		return nil;
	}
	[window release];

	[[self window] setDelegate:self];
	[[self window] setBackgroundColor:[NSColor clearColor]];
	[[self window] setOpaque:NO];
	[[self window] setHasShadow:NO];

	NSRect frame = [[[self window] contentView] frame];
	_contentView = [[TMConfirmQuitFrameView alloc] initWithFrame:frame];
	[[self window] setContentView:_contentView];

	_message = [message retain];
	[_contentView setMessageText:_message];
	[_contentView release];

	return self;
}

- (BOOL)runModalLoopForApplication:(NSApplication *)app {
	TMConfirmQuitPanelController *keepAlive = [self retain];

	static int64_t lastQuitAttempt = 0;
	const auto timeNow = nowMs();
	if (lastQuitAttempt && (timeNow - lastQuitAttempt) < kTimeDeltaFuzzMs) {
		[self hideAllWindowsForApplication:app withDuration:0.0];
		NSEvent *nextEvent = [self
			pumpEventQueueForKeyUp:app
			untilDate:[NSDate distantFuture]];
		[app discardEventsMatchingMask:NSEventMaskAny beforeEvent:nextEvent];
		[keepAlive release];
		return YES;
	}
	lastQuitAttempt = timeNow;

	[self showWindow:self];
	[self sendAccessibilityAnnouncement];

	const auto targetDate = nowMs() + kShowDurationMs;
	BOOL willQuit = NO;
	NSEvent *nextEvent = nil;
	do {
		NSDate *waitDate = [NSDate dateWithTimeIntervalSinceNow:
			double(kShowDurationMs - kTimeDeltaFuzzMs) / 1000.0];
		nextEvent = [self pumpEventQueueForKeyUp:app untilDate:waitDate];

		if (!willQuit) {
			const auto difference = targetDate - nowMs();
			if (difference < kTimeDeltaFuzzMs) {
				willQuit = YES;
				[self hideAllWindowsForApplication:app
					withDuration:double(kWindowFadeOutDurationMs) / 1000.0];
			}
		}
	} while (!nextEvent);

	[app discardEventsMatchingMask:NSEventMaskAny beforeEvent:nextEvent];

	const auto result = willQuit;
	if (!willQuit) {
		[self dismissPanel];
	}

	[keepAlive release];
	return result;
}

- (void)windowWillClose:(NSNotification *)notification {
	Q_UNUSED(notification);
	[[self window] setAnimations:@{}];
	gConfirmQuitPanelController = nil;
	[_message release];
	_message = nil;
	[self autorelease];
}

- (void)showWindow:(id)sender {
	TMConfirmQuitPanelController *keepAlive = [self retain];
	[[self window] setAnimations:@{}];
	[[self window] center];
	[[self window] setAlphaValue:1.0];
	[super showWindow:sender];
	[keepAlive release];
}

- (void)dismissPanel {
	[self performSelector:@selector(animateFadeOut)
		withObject:nil
		afterDelay:1.0];
}

- (void)animateFadeOut {
	NSWindow *window = [self window];
	CAAnimation *animation = [[window animationForKey:@"alphaValue"] copy];
	if (animation) {
		[animation setDelegate:self];
		[animation setDuration:0.2];
		NSMutableDictionary *dictionary =
			[NSMutableDictionary dictionaryWithDictionary:[window animations]];
		dictionary[@"alphaValue"] = animation;
		[window setAnimations:dictionary];
		[animation release];
	}
	[[window animator] setAlphaValue:0.0];
}

- (void)animationDidStart:(CAAnimation *)animation {
	Q_UNUSED(animation);
}

- (void)animationDidStop:(CAAnimation *)animation finished:(BOOL)finished {
	Q_UNUSED(animation);
	Q_UNUSED(finished);
	[self close];
}

- (NSEvent *)pumpEventQueueForKeyUp:(NSApplication *)app untilDate:(NSDate *)date {
	return [app nextEventMatchingMask:NSEventMaskKeyUp
		untilDate:date
		inMode:NSEventTrackingRunLoopMode
		dequeue:YES];
}

- (void)hideAllWindowsForApplication:(NSApplication *)app
	withDuration:(NSTimeInterval)duration {
	TMFadeAllWindowsAnimation *animation =
		[[TMFadeAllWindowsAnimation alloc] initWithApplication:app
			animationDuration:duration];
	[animation startAnimation];
}

- (void)sendAccessibilityAnnouncement {
	NSAccessibilityPostNotificationWithUserInfo(
		[NSApp mainWindow],
		NSAccessibilityAnnouncementRequestedNotification,
		@{
			NSAccessibilityAnnouncementKey : _message,
			NSAccessibilityPriorityKey : @(NSAccessibilityPriorityHigh),
		});
}

@end

namespace TeleMatrix::Platform {
namespace {

[[nodiscard]] NSMenuItem *quitMenuItem() {
	NSMenu *mainMenu = [NSApp mainMenu];
	NSMenu *appMenu = [[mainMenu itemAtIndex:0] submenu];
	for (NSMenuItem *item in [appMenu itemArray]) {
		if ([item action] == @selector(terminate:)) {
			return item;
		}
	}

	NSMenuItem *item = [[[NSMenuItem alloc]
		initWithTitle:@""
		action:@selector(terminate:)
		keyEquivalent:@"q"] autorelease];
	item.keyEquivalentModifierMask = NSEventModifierFlagCommand;
	return item;
}

[[nodiscard]] QString keyCombinationForMenuItem(NSMenuItem *item) {
	auto result = QString();
	const NSUInteger modifiers = item.keyEquivalentModifierMask;
	if (modifiers & NSEventModifierFlagCommand) {
		result.append(QChar(0x2318));
	}
	if (modifiers & NSEventModifierFlagControl) {
		result.append(QChar(0x2303));
	}
	if (modifiers & NSEventModifierFlagOption) {
		result.append(QChar(0x2325));
	}
	if (modifiers & NSEventModifierFlagShift) {
		result.append(QChar(0x21E7));
	}
	result.append(fromNSString([item.keyEquivalent uppercaseString]));
	return result;
}

} // namespace

bool ConfirmQuitRunModal(const QString &text) {
	@autoreleasepool {
		return [[TMConfirmQuitPanelController
			sharedControllerWithMessage:toNSString(text)]
			runModalLoopForApplication:NSApp];
	}
}

QString QuitKeysString() {
	@autoreleasepool {
		return keyCombinationForMenuItem(quitMenuItem());
	}
}

} // namespace TeleMatrix::Platform
