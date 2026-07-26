#include "window/notifications_manager_mac.h"

#import <Cocoa/Cocoa.h>
#import <UserNotifications/UserNotifications.h>

// Obj-C delegate must be declared outside C++ namespace.
@interface TeleMatrixNotificationDelegate
    : NSObject<UNUserNotificationCenterDelegate>
@property (nonatomic, assign) TeleMatrix::Notifications::MacManager *manager;
@end

@implementation TeleMatrixNotificationDelegate

// Called when a notification arrives while the app is in the foreground.
// tdesktop behavior: always present (active-room suppression is handled in C++
// before showNotification is ever called).
- (void)userNotificationCenter:(UNUserNotificationCenter *)center
       willPresentNotification:(UNNotification *)notification
         withCompletionHandler:
             (void (^)(UNNotificationPresentationOptions))completionHandler {
    Q_UNUSED(center);
    Q_UNUSED(notification);
    completionHandler(
        UNNotificationPresentationOptionBanner
        | UNNotificationPresentationOptionList
        | UNNotificationPresentationOptionSound);
}

// Called when the user clicks a delivered notification.
// tdesktop: didActivateNotification -> notificationActivated.
- (void)userNotificationCenter:(UNUserNotificationCenter *)center
    didReceiveNotificationResponse:(UNNotificationResponse *)response
             withCompletionHandler:(void (^)(void))completionHandler {
    Q_UNUSED(center);
    NSDictionary *info = response.notification.request.content.userInfo;
    NSString *roomId = info[@"roomId"];
    TeleMatrix::Notifications::MacManager *manager = self.manager;
    if (roomId && manager) {
        const QString room = QString::fromNSString(roomId);
        NSString *actionId = response.actionIdentifier;
        // UN delegate callbacks arrive off the main thread; emit on the main
        // queue so the Qt signals reach the main-thread System object safely.
        if ([actionId isEqualToString:@"tm.reply"]
            && [response isKindOfClass:[UNTextInputNotificationResponse class]]) {
            NSString *typed =
                [(UNTextInputNotificationResponse *)response userText];
            const QString text = typed ? QString::fromNSString(typed) : QString();
            dispatch_async(dispatch_get_main_queue(), ^{
                emit manager->notificationReplied(room, text);
            });
        } else if ([actionId isEqualToString:@"tm.markread"]) {
            dispatch_async(dispatch_get_main_queue(), ^{
                emit manager->notificationMarkRead(room);
            });
        } else if ([actionId isEqualToString:UNNotificationDefaultActionIdentifier]) {
            dispatch_async(dispatch_get_main_queue(), ^{
                emit manager->notificationActivated(room);
            });
        }
    }
    completionHandler();
}

@end

namespace TeleMatrix::Notifications {

struct MacManager::Impl {
    TeleMatrixNotificationDelegate *delegate = nil;

    Impl(MacManager *manager) {
        delegate = [[TeleMatrixNotificationDelegate alloc] init];
        delegate.manager = manager;
        auto *center = [UNUserNotificationCenter currentNotificationCenter];
        center.delegate = delegate;

        // Reply (text input) + Mark-as-read actions, attached to every message
        // toast via content.categoryIdentifier.
        UNTextInputNotificationAction *reply = [UNTextInputNotificationAction
            actionWithIdentifier:@"tm.reply"
                           title:@"Reply"
                         options:UNNotificationActionOptionNone
            textInputButtonTitle:@"Send"
            textInputPlaceholder:@"Reply"];
        UNNotificationAction *markRead = [UNNotificationAction
            actionWithIdentifier:@"tm.markread"
                           title:@"Mark as read"
                         options:UNNotificationActionOptionNone];
        UNNotificationCategory *category = [UNNotificationCategory
            categoryWithIdentifier:@"tm.message"
                           actions:@[ reply, markRead ]
                 intentIdentifiers:@[]
                           options:UNNotificationCategoryOptionNone];
        [center setNotificationCategories:[NSSet setWithObject:category]];
    }

    ~Impl() {
        [UNUserNotificationCenter currentNotificationCenter].delegate = nil;
        delegate = nil;
    }
};

MacManager::MacManager()
    : _impl(std::make_unique<Impl>(this))
{
    @autoreleasepool {
        auto *center = [UNUserNotificationCenter currentNotificationCenter];
        [center getNotificationSettingsWithCompletionHandler:^(
            UNNotificationSettings *settings) {
            if (settings.authorizationStatus != UNAuthorizationStatusNotDetermined) {
                return;
            }

            const auto options =
                (UNAuthorizationOptionAlert
                 | UNAuthorizationOptionSound
                 | UNAuthorizationOptionBadge);
            [center requestAuthorizationWithOptions:options
                                  completionHandler:^(
                                      BOOL /*granted*/,
                                      NSError * /*error*/) {
            }];
        }];
    }
}

MacManager::~MacManager() = default;

void MacManager::showNotification(
    const QString &roomId,
    const QString &eventId,
    const QString &senderName,
    const QString &chatName,
    const QString &messageText,
    bool isDirect,
    bool isMention,
    const QString &avatarPath,
    bool isInvite)
{
    @autoreleasepool {
        auto *content = [[UNMutableNotificationContent alloc] init];

        // tdesktop: title = sender name, subtitle = chat name (groups),
        // body = message body.
        content.title = senderName.toNSString();
        if (!isDirect && !chatName.isEmpty()) {
            content.subtitle = chatName.toNSString();
        }

        // Truncate message text to ~200 chars.
        const auto truncated = messageText.left(200);
        content.body = truncated.toNSString();

        // Store roomId + eventId in userInfo for click handling / clearing.
        content.userInfo = @{
            @"roomId": roomId.toNSString(),
            @"eventId": eventId.toNSString(),
        };

        // Group a room's notifications under one native macOS stack instead of N
        // independent banners (tdesktop paces/groups bursts; here we let the OS
        // coalesce by room).
        content.threadIdentifier = roomId.toNSString();

        // Attach the Reply / Mark-as-read actions registered in Impl() — but not
        // for an invite, where neither applies (click still opens the room).
        if (!isInvite) {
            content.categoryIdentifier = @"tm.message";
        }

        // A mention/keyword is time-sensitive so it can break through Focus / Do
        // Not Disturb; ordinary messages stay at the default active level. (Time-
        // sensitive delivery also needs the matching entitlement; without it this
        // degrades cleanly to active.)
        if (@available(macOS 12.0, *)) {
            content.interruptionLevel = isMention
                ? UNNotificationInterruptionLevelTimeSensitive
                : UNNotificationInterruptionLevelActive;
        }

        // Sender avatar thumbnail. UNNotificationAttachment takes ownership of
        // (moves) its source file, so attach a per-notification COPY rather than
        // the shared MediaCache file.
        if (!avatarPath.isEmpty()) {
            NSString *src = avatarPath.toNSString();
            NSFileManager *fm = [NSFileManager defaultManager];
            if ([fm fileExistsAtPath:src]) {
                NSString *name = [[NSUUID UUID] UUIDString];
                NSString *ext = [src pathExtension];
                if (ext.length > 0) {
                    name = [name stringByAppendingPathExtension:ext];
                }
                NSString *copy =
                    [NSTemporaryDirectory() stringByAppendingPathComponent:name];
                if ([fm copyItemAtPath:src toPath:copy error:nil]) {
                    UNNotificationAttachment *attachment =
                        [UNNotificationAttachment
                            attachmentWithIdentifier:@"avatar"
                                                 URL:[NSURL fileURLWithPath:copy]
                                             options:nil
                                               error:nil];
                    if (attachment) {
                        content.attachments = @[ attachment ];
                    }
                }
            }
        }

        // Stable per-event identifier: re-delivering the same event replaces
        // (idempotent) rather than duplicating, while distinct events still
        // stack. Fall back to a random id if the event id is missing.
        NSString *identifier = eventId.isEmpty()
            ? [[NSUUID UUID] UUIDString]
            : (roomId + QStringLiteral(":") + eventId).toNSString();
        UNNotificationRequest *request =
            [UNNotificationRequest requestWithIdentifier:identifier
                                                 content:content
                                                 trigger:nil];

        [[UNUserNotificationCenter currentNotificationCenter]
            addNotificationRequest:request
             withCompletionHandler:nil];
    }
}

void MacManager::clearFromRoom(const QString &roomId) {
    @autoreleasepool {
        auto *center = [UNUserNotificationCenter currentNotificationCenter];
        NSString *targetRoom = roomId.toNSString();
        [center getDeliveredNotificationsWithCompletionHandler:^(
            NSArray<UNNotification *> *delivered) {
            NSMutableArray<NSString *> *ids = [NSMutableArray array];
            for (UNNotification *n in delivered) {
                NSString *nRoom =
                    n.request.content.userInfo[@"roomId"];
                if ([nRoom isEqualToString:targetRoom]) {
                    [ids addObject:n.request.identifier];
                }
            }
            if (ids.count > 0) {
                [center removeDeliveredNotificationsWithIdentifiers:ids];
            }
        }];
    }
}

void MacManager::clearAll() {
    @autoreleasepool {
        [[UNUserNotificationCenter currentNotificationCenter]
            removeAllDeliveredNotifications];
    }
}

void MacManager::updateDockBadge(int totalUnread) {
    @autoreleasepool {
        NSString *badge = (totalUnread > 0)
            ? [NSString stringWithFormat:@"%d", totalUnread]
            : @"";
        [[[NSApplication sharedApplication] dockTile]
            setBadgeLabel:badge];
    }
}

void MacManager::bounceDockIcon() {
    @autoreleasepool {
        [NSApp requestUserAttention:NSInformationalRequest];
    }
}

} // namespace TeleMatrix::Notifications
