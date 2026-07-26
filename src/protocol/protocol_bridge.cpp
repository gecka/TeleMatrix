// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "protocol_bridge.h"
#include "media_cache.h"
#include "ffi_conversions.h"
#include "dialogs/saved_messages.h"

extern "C" {
#include "protocol_ffi.h"

void tm_get_folders_async(
    struct Handle *h,
    void (*callback)(bool success, FfiFolderList list, void *userdata),
    void *userdata);

void tm_set_incoming_verification_request_callback(
    struct Handle *h,
    void (*callback)(const char *transaction_id,
                     const char *device_id,
                     const char *device_name,
                     void *userdata),
    void *userdata);

void tm_start_sas_verification_for(
    struct Handle *h,
    const char *transaction_id,
    void (*callback)(bool success,
                     struct FfiSasEmojiList list,
                     void *userdata),
    void *userdata);

void tm_cancel_verification_for(
    struct Handle *h,
    const char *transaction_id,
    void (*callback)(bool success, void *userdata),
    void *userdata);

void tm_start_user_verification(
    struct Handle *h,
    const char *user_id,
    void (*callback)(bool success,
                     struct FfiSasEmojiList list,
                     void *userdata),
    void *userdata);

void tm_withdraw_user_verification(
    struct Handle *h,
    const char *user_id,
    void (*callback)(bool success, void *userdata),
    void *userdata);

void tm_user_trust_state(
    struct Handle *h,
    const char *user_id,
    void (*callback)(bool success, uint32_t state, void *userdata),
    void *userdata);

void tm_set_user_trust_changed_callback(
    struct Handle *h,
    void (*callback)(const char *user_id, uint32_t state, void *userdata),
    void *userdata);

void tm_set_incoming_user_verification_request_callback(
    struct Handle *h,
    void (*callback)(const char *flow_id,
                     const char *user_id,
                     const char *display_name,
                     void *userdata),
    void *userdata);

void tm_set_new_login_callback(
    struct Handle *h,
    void (*callback)(const char *device_id,
                     const char *display_name,
                     const char *last_seen_ip,
                     uint64_t last_seen_ts,
                     void *userdata),
    void *userdata);
}

#include <QCoreApplication>
#include <QMetaObject>
#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>
#include <QMutexLocker>
#include <QElapsedTimer>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using TeleMatrix::ProtocolBridge;

namespace TeleMatrix {
static QVector<RoomSummary> convertFfiRooms(FfiRoomList ffiList);
static TimelineSlice convertFfiTimelineSlice(FfiTimelineSlice ffiSlice);
static QVector<TimelineItem> convertFfiTimeline(const FfiTimeline &ffiTl);
} // namespace TeleMatrix

// Synchronisation guard shared between ProtocolBridge and the persistent
// worker-thread callback trampolines (room-list, sync-state, presence, typing,
// verification, decryption-activity and per-room timeline changes). Rust invokes
// those trampolines from tokio worker threads, passing a raw pointer to this
// guard as their userdata. Each trampoline locks `mutex` and only dereferences
// `bridge` while it is non-null.
//
// ProtocolBridge's destructor takes the same lock and clears `bridge` *before*
// tearing down the Rust handle. That blocks until any trampoline already inside
// the lock has finished touching the bridge, and makes every later trampoline
// observe a null `bridge` and no-op — closing the use-after-free window during
// logout. The guard is owned by ProtocolBridge through a shared_ptr and stays
// alive for the whole destructor body, including tm_destroy(), which shuts the
// tokio runtime down and joins every callback-emitting task. No trampoline can
// fire after tm_destroy() returns, so the guard is never locked after it is
// freed.
//
// Lock ordering (a trampoline may, while holding `mutex`, acquire these — never
// the reverse): `mutex` -> the Qt event-queue lock (via the non-blocking
// QMetaObject::invokeMethod(..., Qt::QueuedConnection)), and `mutex` ->
// ProtocolBridge::_pendingTimelineRoomsMutex. The destructor only ever takes
// `mutex`, so there is no inversion. `mutex` is non-recursive; trampolines never
// re-enter it (their bodies only post to the Qt loop and return).
struct BridgeCallbackGuard {
    std::mutex mutex;
    ProtocolBridge *bridge = nullptr;
};

// Run `fn(bridge)` under the callback guard, but only while the bridge is still
// alive. `userdata` must be a BridgeCallbackGuard* registered by ProtocolBridge.
template <typename F>
static void withGuardedBridge(void *userdata, F &&fn) {
    auto *guard = static_cast<BridgeCallbackGuard *>(userdata);
    // Null-safe: a data-struct callback whose `guard` was never wired up degrades
    // to a dropped callback rather than a crash. (Persistent callbacks always pass
    // a non-null guard.)
    if (!guard) {
        return;
    }
    std::lock_guard<std::mutex> lock(guard->mutex);
    if (guard->bridge) {
        fn(guard->bridge);
    }
}

// Translate known poll subtitle labels from the Rust SDK.
static QString translatePollSubtitle(const QString &s) {
    if (s == QStringLiteral("Poll"))
        return QCoreApplication::translate("ProtocolBridge", "Poll");
    if (s == QStringLiteral("Poll, results after end"))
        return QCoreApplication::translate("ProtocolBridge", "Poll, results after end");
    if (s == QStringLiteral("Quiz"))
        return QCoreApplication::translate("ProtocolBridge", "Quiz");
    if (s == QStringLiteral("Quiz, results after end"))
        return QCoreApplication::translate("ProtocolBridge", "Quiz, results after end");
    if (s == QStringLiteral("Final Results"))
        return QCoreApplication::translate("ProtocolBridge", "Final Results");
    return s;
}

// Translate service message bodies generated by the Rust SDK.
// The SDK produces English strings in "{name} {action}" format.
// We match the action suffix and rebuild with tr() for translation.
static QString translateServiceBody(const QString &body) {
    // Map of English suffixes → translation keys.
    // Order matters: check longer suffixes first to avoid partial matches.
    static const struct { const char *suffix; const char *key; } actions[] = {
        { " joined the room", "%1 joined the room" },
        { " left the room", "%1 left the room" },
        { " was kicked and banned", "%1 was kicked and banned" },
        { " was banned", "%1 was banned" },
        { " was unbanned", "%1 was unbanned" },
        { " was kicked", "%1 was kicked" },
        { " was invited", "%1 was invited" },
        { " accepted the invitation", "%1 accepted the invitation" },
        { " rejected the invitation", "%1 rejected the invitation" },
        { " had the invitation revoked", "%1 had the invitation revoked" },
        { " had the knock accepted", "%1 had the knock accepted" },
        { " retracted the knock", "%1 retracted the knock" },
        { " had the knock denied", "%1 had the knock denied" },
        { " knocked", "%1 knocked" },
        { " membership changed", "%1 membership changed" },
        { " created the room", "%1 created the room" },
        { " changed the room name", "%1 changed the room name" },
        { " changed the room topic", "%1 changed the room topic" },
        { " changed the room avatar", "%1 changed the room avatar" },
        { " changed pinned messages", "%1 changed pinned messages" },
        { " changed join rules", "%1 changed join rules" },
        { " changed permissions", "%1 changed permissions" },
        { " changed history visibility", "%1 changed history visibility" },
        { " changed guest access", "%1 changed guest access" },
        { " enabled encryption", "%1 enabled encryption" },
        { " replaced the room", "%1 replaced the room" },
        { " changed the room address", "%1 changed the room address" },
        { " changed server access rules", "%1 changed server access rules" },
        { " sent a third-party invite", "%1 sent a third-party invite" },
        { " changed display name and avatar", "%1 changed display name and avatar" },
        { " changed display name", "%1 changed display name" },
        { " changed avatar", "%1 changed avatar" },
        { " updated profile", "%1 updated profile" },
        { " started a call", "%1 started a call" },
    };
    for (const auto &a : actions) {
        const auto suffix = QString::fromLatin1(a.suffix);
        if (body.endsWith(suffix)) {
            const auto name = body.left(body.length() - suffix.length());
            return QCoreApplication::translate("ServiceMessage", a.key)
                .arg(name);
        }
    }
    return body;
}

// --- FFI callback trampolines ---
// These must have C linkage since the Rust library calls them as C function pointers.
// They marshal the result to the Qt main thread via QueuedConnection.

extern "C" {

struct SendCallbackData {
    ProtocolBridge *bridge = nullptr;
    std::function<void(bool, const QString &)> handler;
    BridgeCallbackGuard *guard = nullptr;
};

struct SimpleCallbackData {
    ProtocolBridge *bridge = nullptr;
    std::function<void(bool)> handler;
    BridgeCallbackGuard *guard = nullptr;
};

struct SavedMessagesCallbackData {
    ProtocolBridge *bridge = nullptr;
    std::function<void(bool, const QString &)> handler;
    BridgeCallbackGuard *guard = nullptr;
};

struct CreateFolderCallbackData {
    ProtocolBridge *bridge = nullptr;
    std::function<void(bool, int, const QString &, const QString &)> handler;
    BridgeCallbackGuard *guard = nullptr;
};

// Carries success + a server error message (empty on success). Used by folder
// operations that must surface the failure reason to the UI.
struct ResultCallbackData {
    ProtocolBridge *bridge = nullptr;
    std::function<void(bool, const QString &)> handler;
    BridgeCallbackGuard *guard = nullptr;
};

struct FolderListCallbackData {
    ProtocolBridge *bridge = nullptr;
    std::function<void(bool, const QVector<TeleMatrix::FolderInfo> &)> handler;
    BridgeCallbackGuard *guard = nullptr;
};

struct SidebarOrderCallbackData {
    ProtocolBridge *bridge = nullptr;
    std::function<void(bool, const QVector<TeleMatrix::SidebarEntry> &)> handler;
    BridgeCallbackGuard *guard = nullptr;
};

struct SpaceListCallbackData {
    ProtocolBridge *bridge = nullptr;
    std::function<void(bool, const QVector<TeleMatrix::SpaceInfo> &)> handler;
    BridgeCallbackGuard *guard = nullptr;
};

struct UserListCallbackData {
    ProtocolBridge *bridge = nullptr;
    std::function<void(bool, const QVector<TeleMatrix::UserProfile> &)> handler;
    BridgeCallbackGuard *guard = nullptr;
};

struct UserDirectoryCallbackData {
    ProtocolBridge *bridge = nullptr;
    std::function<void(bool, const QVector<TeleMatrix::UserProfile> &, bool)> handler;
    BridgeCallbackGuard *guard = nullptr;
};

struct UserTrustStateCallbackData {
    ProtocolBridge *bridge = nullptr;
    QString userId;
    BridgeCallbackGuard *guard = nullptr;
};

struct RoomMembersSnapshotCallbackData {
    ProtocolBridge *bridge = nullptr;
    QString roomId;
    std::function<void(bool, const TeleMatrix::RoomMembersSnapshot &)> handler;
    BridgeCallbackGuard *guard = nullptr;
};

struct UserProfileDetailsCallbackData {
    ProtocolBridge *bridge = nullptr;
    QString roomId;
    QString userId;
    std::function<void(bool, const TeleMatrix::UserProfileDetails &)> handler;
    BridgeCallbackGuard *guard = nullptr;
};

struct RoomListCallbackData {
    ProtocolBridge *bridge = nullptr;
    quint64 requestId = 0;
    BridgeCallbackGuard *guard = nullptr;
};

struct RoomUnreadSnapshotCallbackData {
    ProtocolBridge *bridge = nullptr;
    QString roomId;
    quint64 requestId = 0;
    BridgeCallbackGuard *guard = nullptr;
};

struct TimelineSliceCallbackData {
    ProtocolBridge *bridge = nullptr;
    QString roomId;
    quint64 requestId = 0;
    BridgeCallbackGuard *guard = nullptr;
};

static void loginCallbackTrampoline(
    bool success,
    const char *user_id,
    const char *display_name,
    const char *avatar_url,
    void *userdata)
{
    // Copy strings before they go out of scope on the Rust side.
    QString userId = success && user_id ? QString::fromUtf8(user_id) : QString();
    QString displayName = success && display_name ? QString::fromUtf8(display_name) : QString();
    QString avatarUrl = success && avatar_url ? QString::fromUtf8(avatar_url) : QString();
    // Guarded: this one-shot callback holds only the bridge guard, so a bridge
    // disposed before it fires (e.g. app quit mid-login) is observed as null.
    withGuardedBridge(userdata, [success, userId, displayName, avatarUrl](ProtocolBridge *bridge) {
        QMetaObject::invokeMethod(bridge, [bridge, success, userId, displayName, avatarUrl]() {
            emit bridge->loginResult(success, userId, displayName, avatarUrl);
        }, Qt::QueuedConnection);
    });
}

static void registerCallbackTrampoline(
    uint32_t status,
    const char *payload_json,
    void *userdata)
{
    QString json = payload_json ? QString::fromUtf8(payload_json) : QString();
    withGuardedBridge(userdata, [status, json](ProtocolBridge *bridge) {
        QMetaObject::invokeMethod(bridge, [bridge, status, json]() {
            if (status == 0) {
                // Success — parse user profile from JSON.
                auto doc = QJsonDocument::fromJson(json.toUtf8());
                auto obj = doc.object();
                emit bridge->registrationSuccess(
                    obj.value(QStringLiteral("user_id")).toString(),
                    obj.value(QStringLiteral("display_name")).toString(),
                    obj.value(QStringLiteral("avatar_url")).toString());
            } else if (status == 1) {
                // UIA challenge.
                emit bridge->registrationChallenge(json);
            } else {
                // Error.
                auto doc = QJsonDocument::fromJson(json.toUtf8());
                auto obj = doc.object();
                emit bridge->registrationFailed(
                    obj.value(QStringLiteral("error")).toString());
            }
        }, Qt::QueuedConnection);
    });
}

static void usernameCheckCallbackTrampoline(
    uint32_t status,
    const char *message,
    void *userdata)
{
    QString msg = message ? QString::fromUtf8(message) : QString();
    withGuardedBridge(userdata, [status, msg](ProtocolBridge *bridge) {
        QMetaObject::invokeMethod(bridge, [bridge, status, msg]() {
            emit bridge->usernameAvailabilityChecked(static_cast<int>(status), msg);
        }, Qt::QueuedConnection);
    });
}

static void sendCallbackWithDataTrampoline(
    bool success,
    const char *event_id,
    void *userdata)
{
    auto *data = static_cast<SendCallbackData *>(userdata);
    auto handler = std::move(data->handler);
    auto *guard = data->guard;
    const QString eventId = event_id ? QString::fromUtf8(event_id) : QString();
    delete data;
    withGuardedBridge(guard, [handler = std::move(handler), success, eventId](ProtocolBridge *bridge) mutable {
        QMetaObject::invokeMethod(bridge, [handler = std::move(handler), success, eventId]() mutable {
            handler(success, eventId);
        }, Qt::QueuedConnection);
    });
}

static void simpleCallbackTrampoline(bool success, void *userdata) {
    auto *data = static_cast<SimpleCallbackData *>(userdata);
    auto handler = std::move(data->handler);
    auto *guard = data->guard;
    delete data;
    withGuardedBridge(guard, [handler = std::move(handler), success](ProtocolBridge *bridge) mutable {
        QMetaObject::invokeMethod(bridge, [handler = std::move(handler), success]() mutable {
            handler(success);
        }, Qt::QueuedConnection);
    });
}

static void notificationSettingsCallbackTrampoline(
    bool success,
    uint32_t dm_level,
    uint32_t room_level,
    bool mention_display_name,
    bool mention_username,
    bool mention_room,
    bool keywords_enabled,
    const char *keywords_csv,
    void *userdata)
{
    // Copy the CSV before taking the guard lock (the pointer is valid only for
    // this stack frame).
    const auto keywordsCsv = keywords_csv
        ? QString::fromUtf8(keywords_csv) : QString();
    const auto toMode = [](uint32_t v) {
        return (v == 1) ? TeleMatrix::RoomNotificationMode::MentionsOnly
                        : TeleMatrix::RoomNotificationMode::AllMessages;
    };
    const auto dmLevel = toMode(dm_level);
    const auto roomLevel = toMode(room_level);
    withGuardedBridge(userdata, [=](ProtocolBridge *bridge) {
        QMetaObject::invokeMethod(bridge, [=]() {
            emit bridge->notificationSettingsReady(
                success, dmLevel, roomLevel,
                mention_display_name, mention_username, mention_room,
                keywords_enabled, keywordsCsv);
        }, Qt::QueuedConnection);
    });
}

static void resultCallbackTrampoline(bool success, char *error, void *userdata) {
    auto *data = static_cast<ResultCallbackData *>(userdata);
    auto handler = std::move(data->handler);
    auto *guard = data->guard;
    const QString errorMsg = error ? QString::fromUtf8(error) : QString();
    if (error) {
        tm_free_string(error);
    }
    delete data;
    withGuardedBridge(guard, [handler = std::move(handler), success, errorMsg](ProtocolBridge *bridge) mutable {
        QMetaObject::invokeMethod(bridge, [handler = std::move(handler), success, errorMsg]() mutable {
            handler(success, errorMsg);
        }, Qt::QueuedConnection);
    });
}

static void createFolderCallbackTrampoline(bool success, int32_t folder_id, char *tag_key, char *error, void *userdata) {
    auto *data = static_cast<CreateFolderCallbackData *>(userdata);
    auto handler = std::move(data->handler);
    auto *guard = data->guard;
    const int folderId = static_cast<int>(folder_id);
    const QString sectionKey = tag_key ? QString::fromUtf8(tag_key) : QString();
    const QString errorMsg = error ? QString::fromUtf8(error) : QString();
    if (tag_key) {
        tm_free_string(tag_key);
    }
    if (error) {
        tm_free_string(error);
    }
    delete data;
    withGuardedBridge(guard, [handler = std::move(handler), success, folderId, sectionKey, errorMsg](ProtocolBridge *bridge) mutable {
        QMetaObject::invokeMethod(bridge, [handler = std::move(handler), success, folderId, sectionKey, errorMsg]() mutable {
            handler(success, folderId, sectionKey, errorMsg);
        }, Qt::QueuedConnection);
    });
}

} // end extern "C" — a C-linkage function may not return a C++ class type
  // (MSVC error C2526; Clang only warns, C4190). The callback trampolines still
  // need C linkage, so only these converter helpers drop out of the block.

static QVector<TeleMatrix::FolderInfo> folderInfosFromFfiFolderList(FfiFolderList list) {
    QVector<TeleMatrix::FolderInfo> result;
    if (!list.folders || list.len == 0) {
        return result;
    }

    result.reserve(static_cast<int>(list.len));
    for (size_t i = 0; i < list.len; ++i) {
        const FfiFolderMeta &ffi = list.folders[i];
        TeleMatrix::FolderInfo folder;
        folder.filterId = static_cast<int>(ffi.id);
        folder.sectionKey = ffi.tag_key ? QString::fromUtf8(ffi.tag_key) : QString();
        folder.displayName = ffi.name ? QString::fromUtf8(ffi.name) : QString();
        result.append(folder);
    }
    return result;
}

static QVector<TeleMatrix::SidebarEntry> sidebarOrderFromFfi(FfiSidebarOrder list) {
    QVector<TeleMatrix::SidebarEntry> result;
    if (!list.refs || list.len == 0) {
        return result;
    }
    result.reserve(static_cast<int>(list.len));
    for (size_t i = 0; i < list.len; ++i) {
        const FfiSidebarRef &ffi = list.refs[i];
        TeleMatrix::SidebarEntry entry;
        entry.isSpace = (ffi.kind == 1);
        entry.key = ffi.key ? QString::fromUtf8(ffi.key) : QString();
        result.append(entry);
    }
    return result;
}

static QVector<TeleMatrix::SpaceInfo> spaceInfosFromFfi(FfiSpaceList list) {
    QVector<TeleMatrix::SpaceInfo> result;
    if (!list.spaces || list.len == 0) {
        return result;
    }
    result.reserve(static_cast<int>(list.len));
    for (size_t i = 0; i < list.len; ++i) {
        const FfiSpaceInfo &ffi = list.spaces[i];
        TeleMatrix::SpaceInfo space;
        space.roomId = ffi.room_id ? QString::fromUtf8(ffi.room_id) : QString();
        space.displayName = ffi.name ? QString::fromUtf8(ffi.name) : QString();
        space.avatarUrl = ffi.avatar_url ? QString::fromUtf8(ffi.avatar_url) : QString();
        space.topic = ffi.topic ? QString::fromUtf8(ffi.topic) : QString();
        space.memberCount = ffi.member_count;
        space.canonicalAlias = ffi.canonical_alias ? QString::fromUtf8(ffi.canonical_alias) : QString();
        result.append(space);
    }
    return result;
}

extern "C" {

static void folderListCallbackTrampoline(bool success, FfiFolderList list, void *userdata) {
    auto *data = static_cast<FolderListCallbackData *>(userdata);
    auto handler = std::move(data->handler);
    auto *guard = data->guard;
    const auto result = folderInfosFromFfiFolderList(list);
    tm_free_folders(list);
    delete data;
    withGuardedBridge(guard, [handler = std::move(handler), success, result](ProtocolBridge *bridge) mutable {
        QMetaObject::invokeMethod(bridge, [handler = std::move(handler), success, result]() mutable {
            handler(success, result);
        }, Qt::QueuedConnection);
    });
}

static void sidebarOrderCallbackTrampoline(bool success, FfiSidebarOrder list, void *userdata) {
    auto *data = static_cast<SidebarOrderCallbackData *>(userdata);
    auto handler = std::move(data->handler);
    auto *guard = data->guard;
    const auto result = sidebarOrderFromFfi(list);
    tm_free_sidebar_order(list);
    delete data;
    withGuardedBridge(guard, [handler = std::move(handler), success, result](ProtocolBridge *bridge) mutable {
        QMetaObject::invokeMethod(bridge, [handler = std::move(handler), success, result]() mutable {
            handler(success, result);
        }, Qt::QueuedConnection);
    });
}

static void spaceListCallbackTrampoline(bool success, FfiSpaceList list, void *userdata) {
    auto *data = static_cast<SpaceListCallbackData *>(userdata);
    auto handler = std::move(data->handler);
    auto *guard = data->guard;
    const auto result = spaceInfosFromFfi(list);
    tm_free_spaces(list);
    delete data;
    withGuardedBridge(guard, [handler = std::move(handler), success, result](ProtocolBridge *bridge) mutable {
        QMetaObject::invokeMethod(bridge, [handler = std::move(handler), success, result]() mutable {
            handler(success, result);
        }, Qt::QueuedConnection);
    });
}

} // end extern "C" — C++ converter helpers below (see note above)

static QVector<TeleMatrix::UserProfile> userProfilesFromFfiMemberList(FfiMemberList list) {
    QVector<TeleMatrix::UserProfile> result;
    if (!list.members || list.len == 0) {
        return result;
    }

    result.reserve(static_cast<int>(list.len));
    for (size_t i = 0; i < list.len; ++i) {
        const FfiUserProfile &ffi = list.members[i];
        TeleMatrix::UserProfile profile;
        profile.userId = ffi.user_id ? QString::fromUtf8(ffi.user_id) : QString();
        profile.displayName = ffi.display_name ? QString::fromUtf8(ffi.display_name) : QString();
        profile.avatarUrl = ffi.avatar_url ? QString::fromUtf8(ffi.avatar_url) : QString();
        result.append(profile);
    }
    return result;
}

static QVector<TeleMatrix::UserProfile> userProfilesFromFfiUserDirectory(
    const FfiUserDirectoryResults &results) {
    QVector<TeleMatrix::UserProfile> out;
    if (!results.members || results.len == 0) {
        return out;
    }
    out.reserve(static_cast<int>(results.len));
    for (size_t i = 0; i < results.len; ++i) {
        const FfiUserProfile &ffi = results.members[i];
        TeleMatrix::UserProfile profile;
        profile.userId = ffi.user_id ? QString::fromUtf8(ffi.user_id) : QString();
        profile.displayName = ffi.display_name ? QString::fromUtf8(ffi.display_name) : QString();
        profile.avatarUrl = ffi.avatar_url ? QString::fromUtf8(ffi.avatar_url) : QString();
        out.append(profile);
    }
    return out;
}

static TeleMatrix::RoomMembersSnapshot roomMembersSnapshotFromFfi(
    FfiRoomMembersSnapshot ffiSnap,
    const QString &fallbackRoomId = QString()) {
    TeleMatrix::RoomMembersSnapshot result;
    result.roomId = ffiSnap.room_id ? QString::fromUtf8(ffiSnap.room_id) : fallbackRoomId;
    result.myUserId = ffiSnap.my_user_id ? QString::fromUtf8(ffiSnap.my_user_id) : QString();
    result.canInvite = ffiSnap.can_invite;
    result.canRemoveAny = ffiSnap.can_remove_any;

    if (ffiSnap.members && ffiSnap.members_len > 0) {
        result.members.reserve(static_cast<int>(ffiSnap.members_len));
        for (size_t i = 0; i < ffiSnap.members_len; ++i) {
            const FfiRoomMemberInfo &ffi = ffiSnap.members[i];
            TeleMatrix::RoomMemberInfo info;
            info.userId = ffi.user_id ? QString::fromUtf8(ffi.user_id) : QString();
            info.displayName = ffi.display_name ? QString::fromUtf8(ffi.display_name) : QString();
            info.avatarUrl = ffi.avatar_url ? QString::fromUtf8(ffi.avatar_url) : QString();
            info.membership = static_cast<TeleMatrix::MembershipState>(ffi.membership);
            info.powerLevel = ffi.power_level;
            info.role = static_cast<TeleMatrix::MemberRole>(ffi.role);
            info.isSelf = ffi.is_self;
            info.canBeRemovedByMe = ffi.can_be_removed_by_me;
            info.canBeBannedByMe = ffi.can_be_banned_by_me;
            info.canBeUnbannedByMe = ffi.can_be_unbanned_by_me;
            result.members.append(info);
        }
    }

    return result;
}

static TeleMatrix::UserProfileDetails userProfileDetailsFromFfi(
    FfiUserProfileDetails ffi,
    const QString &fallbackRoomId,
    const QString &fallbackUserId) {
    TeleMatrix::UserProfileDetails result;
    result.roomId = ffi.room_id ? QString::fromUtf8(ffi.room_id) : fallbackRoomId;
    result.userId = ffi.user_id ? QString::fromUtf8(ffi.user_id) : fallbackUserId;
    result.displayName = ffi.display_name ? QString::fromUtf8(ffi.display_name) : fallbackUserId;
    result.avatarUrl = ffi.avatar_url ? QString::fromUtf8(ffi.avatar_url) : QString();
    result.presence = static_cast<TeleMatrix::PresenceState>(ffi.presence);
    result.lastActiveTs = static_cast<qint64>(ffi.last_active_ts);
    result.membership = static_cast<TeleMatrix::MembershipState>(ffi.membership);
    result.powerLevel = ffi.power_level;
    result.role = static_cast<TeleMatrix::MemberRole>(ffi.role);
    result.isIgnored = ffi.is_ignored;
    result.dmRoomId = ffi.dm_room_id ? QString::fromUtf8(ffi.dm_room_id) : QString();
    result.canInvite = ffi.can_invite;
    result.canKick = ffi.can_kick;
    result.canBan = ffi.can_ban;
    result.canMute = ffi.can_mute;
    result.canChangePowerLevel = ffi.can_change_power_level;
    result.maxAssignablePowerLevel = ffi.max_assignable_power_level;
    return result;
}

extern "C" {

static void userListCallbackTrampoline(bool success, FfiMemberList list, void *userdata) {
    auto *data = static_cast<UserListCallbackData *>(userdata);
    auto handler = std::move(data->handler);
    auto *guard = data->guard;
    const auto result = userProfilesFromFfiMemberList(list);
    tm_free_room_members(list);
    delete data;
    withGuardedBridge(guard, [handler = std::move(handler), success, result](ProtocolBridge *bridge) mutable {
        QMetaObject::invokeMethod(bridge, [handler = std::move(handler), success, result]() mutable {
            handler(success, result);
        }, Qt::QueuedConnection);
    });
}

static void userDirectorySearchCallbackTrampoline(
    bool success,
    FfiUserDirectoryResults results,
    void *userdata) {
    auto *data = static_cast<UserDirectoryCallbackData *>(userdata);
    auto handler = std::move(data->handler);
    auto *guard = data->guard;
    const auto profiles = userProfilesFromFfiUserDirectory(results);
    const bool limited = results.limited;
    tm_free_user_directory_results(results);
    delete data;
    withGuardedBridge(guard, [handler = std::move(handler), success, profiles, limited](ProtocolBridge *bridge) mutable {
        QMetaObject::invokeMethod(bridge, [handler = std::move(handler), success, profiles, limited]() mutable {
            handler(success, profiles, limited);
        }, Qt::QueuedConnection);
    });
}

static void roomMembersSnapshotCallbackTrampoline(
    bool success,
    FfiRoomMembersSnapshot snapshot,
    void *userdata) {
    auto *data = static_cast<RoomMembersSnapshotCallbackData *>(userdata);
    auto handler = std::move(data->handler);
    auto *guard = data->guard;
    const auto roomId = data->roomId;
    const auto result = roomMembersSnapshotFromFfi(snapshot, roomId);
    tm_free_room_members_snapshot(snapshot);
    delete data;
    withGuardedBridge(guard, [handler = std::move(handler), success, result](ProtocolBridge *bridge) mutable {
        QMetaObject::invokeMethod(bridge, [handler = std::move(handler), success, result]() mutable {
            handler(success, result);
        }, Qt::QueuedConnection);
    });
}

static void userProfileDetailsCallbackTrampoline(
    bool success,
    FfiUserProfileDetails details,
    void *userdata) {
    auto *data = static_cast<UserProfileDetailsCallbackData *>(userdata);
    auto handler = std::move(data->handler);
    auto *guard = data->guard;
    const auto result = userProfileDetailsFromFfi(details, data->roomId, data->userId);
    tm_free_user_profile_details(details);
    delete data;
    withGuardedBridge(guard, [handler = std::move(handler), success, result](ProtocolBridge *bridge) mutable {
        QMetaObject::invokeMethod(bridge, [handler = std::move(handler), success, result]() mutable {
            handler(success, result);
        }, Qt::QueuedConnection);
    });
}

static void roomListCallbackTrampoline(
    bool success,
    FfiRoomList list,
    void *userdata) {
    auto *data = static_cast<RoomListCallbackData *>(userdata);
    auto *guard = data->guard;
    const auto requestId = data->requestId;
    auto result = success
        ? TeleMatrix::convertFfiRooms(list)
        : QVector<TeleMatrix::RoomSummary>();
    tm_free_rooms(list);
    delete data;
    withGuardedBridge(guard, [requestId, success, result = std::move(result)](ProtocolBridge *bridge) mutable {
        QMetaObject::invokeMethod(
            bridge,
            [bridge, requestId, success, result = std::move(result)]() {
                bridge->handleRoomsReady(requestId, success, result);
            },
            Qt::QueuedConnection);
    });
}

static void roomUnreadSnapshotCallbackTrampoline(
    bool success,
    FfiTimelineSlice slice,
    void *userdata) {
    auto *data = static_cast<RoomUnreadSnapshotCallbackData *>(userdata);
    auto *guard = data->guard;
    const auto roomId = data->roomId;
    const auto requestId = data->requestId;
    // The notif-info portion of the snapshot needs the live bridge, so build the
    // result inside the guard. Capture the slice-derived field first, since the
    // slice must be freed (below) before the guard is taken.
    const auto unreadCount = static_cast<int>(slice.unread_count);
    tm_free_timeline_slice(slice);
    delete data;
    withGuardedBridge(guard, [roomId, requestId, success, unreadCount](ProtocolBridge *bridge) {
        TeleMatrix::RoomUnreadSnapshot result;
        if (success) {
            result.unreadCount = unreadCount;
            result.highlightCount = 0;
            result.notificationMode = TeleMatrix::RoomNotificationMode::AllMessages;
            result.isMuted = false;
            result.isMarkedUnread = false;
            if (bridge) {
                const auto info = bridge->roomNotifInfo(roomId);
                result.notificationMode = info.notificationMode;
                result.isMuted = info.isMuted;
                result.isMarkedUnread = info.isMarkedUnread;
            }
        }
        QMetaObject::invokeMethod(
            bridge,
            [bridge, roomId, requestId, success, result]() {
                bridge->handleRoomUnreadSnapshotReady(roomId, requestId, success, result);
            },
            Qt::QueuedConnection);
    });
}

static void timelineSliceCallbackTrampoline(
    bool success,
    FfiTimelineSlice slice,
    void *userdata) {
    auto *data = static_cast<TimelineSliceCallbackData *>(userdata);
    auto *guard = data->guard;
    const auto roomId = data->roomId;
    const auto requestId = data->requestId;
    auto result = success
        ? TeleMatrix::convertFfiTimelineSlice(slice)
        : TeleMatrix::TimelineSlice();
    tm_free_timeline_slice(slice);
    delete data;
    withGuardedBridge(guard, [roomId, requestId, success, result = std::move(result)](ProtocolBridge *bridge) mutable {
        QMetaObject::invokeMethod(
            bridge,
            [bridge, roomId, requestId, success, result = std::move(result)]() {
                bridge->handleTimelineSliceReady(roomId, requestId, success, result);
            },
            Qt::QueuedConnection);
    });
}

static void createRoomCallbackTrampoline(
    bool success,
    const char *room_id,
    void *userdata)
{
    auto *data = static_cast<SendCallbackData *>(userdata);
    auto handler = std::move(data->handler);
    auto *guard = data->guard;
    QString roomId = success && room_id ? QString::fromUtf8(room_id) : QString();
    delete data;
    withGuardedBridge(guard, [handler = std::move(handler), success, roomId](ProtocolBridge *bridge) mutable {
        QMetaObject::invokeMethod(bridge, [handler = std::move(handler), success, roomId]() mutable {
            handler(success, roomId);
        }, Qt::QueuedConnection);
    });
}

static void savedMessagesCallbackTrampoline(
    bool success,
    const char *room_id,
    void *userdata)
{
    auto *data = static_cast<SavedMessagesCallbackData *>(userdata);
    auto handler = std::move(data->handler);
    auto *guard = data->guard;
    QString roomId = success && room_id ? QString::fromUtf8(room_id) : QString();
    delete data;
    withGuardedBridge(guard, [handler = std::move(handler), success, roomId](
                          ProtocolBridge *bridge) mutable {
        QMetaObject::invokeMethod(bridge, [handler = std::move(handler), success, roomId]() mutable {
            handler(success, roomId);
        }, Qt::QueuedConnection);
    });
}

static void roomChangeCallbackTrampoline(void *userdata) {
    withGuardedBridge(userdata, [](ProtocolBridge *bridge) {
        bridge->enqueueRoomListChangedFromCallback();
    });
}

static void previewFetchingCallbackTrampoline(
    const char *room_id,
    const char *event_id,
    bool fetching,
    void *userdata)
{
    QString roomId = room_id ? QString::fromUtf8(room_id) : QString();
    QString eventId = event_id ? QString::fromUtf8(event_id) : QString();
    withGuardedBridge(userdata, [roomId, eventId, fetching](ProtocolBridge *bridge) {
        QMetaObject::invokeMethod(bridge, [bridge, roomId, eventId, fetching]() {
            emit bridge->urlPreviewFetchingChanged(roomId, eventId, fetching);
        }, Qt::QueuedConnection);
    });
}

static void syncStateCallbackTrampoline(uint32_t state, void *userdata) {
    int s = static_cast<int>(state);
    withGuardedBridge(userdata, [s](ProtocolBridge *bridge) {
        QMetaObject::invokeMethod(bridge, [bridge, s]() {
            bridge->setSyncState(s);
        }, Qt::QueuedConnection);
    });
}

// Currently unregistered (initial-dialogs-load-state is derived inside
// setSyncState), but kept consistent with the guarded pattern so it is safe if
// ever wired up as a persistent callback. Its userdata must then be the guard.
static void initialDialogsLoadStateCallbackTrampoline(uint32_t state, void *userdata) {
    const auto s = static_cast<TeleMatrix::InitialDialogsLoadState>(state);
    withGuardedBridge(userdata, [s](ProtocolBridge *bridge) {
        QMetaObject::invokeMethod(bridge, [bridge, s]() {
            bridge->setInitialDialogsLoadState(s);
        }, Qt::QueuedConnection);
    });
}

static void presenceCallbackTrampoline(
    const char *user_id,
    uint32_t state,
    uint64_t last_active_ts,
    void *userdata)
{
    QString userId = user_id ? QString::fromUtf8(user_id) : QString();
    int s = static_cast<int>(state);
    qint64 ts = static_cast<qint64>(last_active_ts);
    withGuardedBridge(userdata, [userId, s, ts](ProtocolBridge *bridge) {
        QMetaObject::invokeMethod(bridge, [bridge, userId, s, ts]() {
            emit bridge->presenceChanged(userId, s, ts);
        }, Qt::QueuedConnection);
    });
}

static void typingCallbackTrampoline(
    const char *roomId,
    const char *const *userIds,
    uintptr_t userCount,
    void *userdata)
{
    QString qRoomId = QString::fromUtf8(roomId);
    QStringList qUserIds;
    for (uintptr_t i = 0; i < userCount; ++i) {
        qUserIds.append(QString::fromUtf8(userIds[i]));
    }
    withGuardedBridge(userdata, [qRoomId, qUserIds](ProtocolBridge *bridge) {
        QMetaObject::invokeMethod(bridge, [bridge, qRoomId, qUserIds]() {
            emit bridge->typingChanged(qRoomId, qUserIds);
        }, Qt::QueuedConnection);
    });
}

// Userdata for per-room timeline callbacks. Captures bridge + room_id
// since the C callback signature only passes an opaque void*.
struct TimelineCallbackData {
    ProtocolBridge *bridge;
    // Same guard ProtocolBridge owns; lets this per-room trampoline detect
    // teardown. `data` itself lives in a bridge-owned map, so it is only freed
    // after tm_destroy() (by which point no trampoline can fire).
    BridgeCallbackGuard *guard = nullptr;
    QByteArray roomIdUtf8;
};

static void timelineChangeCallbackTrampoline(void *userdata) {
    auto *data = static_cast<TimelineCallbackData *>(userdata);
    QString roomId = QString::fromUtf8(data->roomIdUtf8);
    withGuardedBridge(data->guard, [&roomId](ProtocolBridge *bridge) {
        bridge->enqueueTimelineChangedFromCallback(roomId);
    });
}

struct FocusCallbackData {
    ProtocolBridge *bridge = nullptr;
    QString roomId;
    quint64 requestId = 0;
    BridgeCallbackGuard *guard = nullptr;
};

extern "C" void focusOnEventCallbackTrampoline(bool success, void *userdata) {
    auto *data = static_cast<FocusCallbackData *>(userdata);
    auto *guard = data->guard;
    auto roomId = data->roomId;
    const auto requestId = data->requestId;
    delete data;
    withGuardedBridge(guard, [roomId, requestId, success](ProtocolBridge *bridge) {
        QMetaObject::invokeMethod(bridge, [bridge, roomId, requestId, success]() {
            emit bridge->focusOnEventResult(roomId, requestId, success);
        }, Qt::QueuedConnection);
    });
}

struct SessionCallbackData {
    ProtocolBridge *bridge = nullptr;
    std::function<void(bool)> handler;
    BridgeCallbackGuard *guard = nullptr;
};

static void sessionCallbackTrampoline(
    bool success,
    const char *user_id,
    const char *display_name,
    const char *avatar_url,
    const char *error,
    void *userdata)
{
    auto *data = static_cast<SessionCallbackData *>(userdata);
    auto handler = std::move(data->handler);
    auto *guard = data->guard;
    delete data;
    QString userId = success && user_id ? QString::fromUtf8(user_id) : QString();
    QString dispName = success && display_name ? QString::fromUtf8(display_name) : QString();
    QString avatarUrl = success && avatar_url ? QString::fromUtf8(avatar_url) : QString();
    QString errorStr = !success && error ? QString::fromUtf8(error) : QString();
    withGuardedBridge(
        guard,
        [handler = std::move(handler),
         success,
         userId,
         dispName,
         avatarUrl,
         errorStr](ProtocolBridge *bridge) mutable {
            QMetaObject::invokeMethod(
                bridge,
                [bridge,
                 handler = std::move(handler),
                 success,
                 userId,
                 dispName,
                 avatarUrl,
                 errorStr]() mutable {
                    handler(success);
                    emit bridge->sessionRestored(success, userId, dispName, avatarUrl, errorStr);
                },
                Qt::QueuedConnection);
        });
}

struct DiscoverCallbackData {
    ProtocolBridge *bridge = nullptr;
    quint64 requestId = 0;
    BridgeCallbackGuard *guard = nullptr;
};

static void discoverCallbackTrampoline(bool success, const char *homeserver_url, void *userdata) {
    auto *data = static_cast<DiscoverCallbackData *>(userdata);
    auto *guard = data->guard;
    const auto requestId = data->requestId;
    delete data;
    QString url = success && homeserver_url ? QString::fromUtf8(homeserver_url) : QString();
    withGuardedBridge(guard, [requestId, success, url](ProtocolBridge *bridge) {
        QMetaObject::invokeMethod(bridge, [bridge, requestId, success, url]() {
            emit bridge->homeserverDiscovered(requestId, success, url);
        }, Qt::QueuedConnection);
    });
}

static void classifyRegistrationCallbackTrampoline(int status, const char *url, void *userdata) {
    auto *data = static_cast<DiscoverCallbackData *>(userdata);
    auto *guard = data->guard;
    const auto requestId = data->requestId;
    delete data;
    QString urlStr = url ? QString::fromUtf8(url) : QString();
    withGuardedBridge(guard, [requestId, status, urlStr](ProtocolBridge *bridge) {
        QMetaObject::invokeMethod(bridge, [bridge, requestId, status, urlStr]() {
            emit bridge->registrationClassified(requestId, status, urlStr);
        }, Qt::QueuedConnection);
    });
}

struct AuthDelegationCallbackData {
    ProtocolBridge *bridge = nullptr;
    quint64 requestId = 0;
    BridgeCallbackGuard *guard = nullptr;
};

static void authDelegationCallbackTrampoline(bool delegated, const char *account_url, void *userdata) {
    auto *data = static_cast<AuthDelegationCallbackData *>(userdata);
    auto *guard = data->guard;
    const auto requestId = data->requestId;
    delete data;
    QString url = delegated && account_url ? QString::fromUtf8(account_url) : QString();
    withGuardedBridge(guard, [requestId, delegated, url](ProtocolBridge *bridge) {
        QMetaObject::invokeMethod(bridge, [bridge, requestId, delegated, url]() {
            emit bridge->authDelegationProbed(requestId, delegated, url);
        }, Qt::QueuedConnection);
    });
}

static void accountManagementCallbackTrampoline(bool available, const char *account_url, void *userdata) {
    auto *data = static_cast<AuthDelegationCallbackData *>(userdata);
    auto *guard = data->guard;
    const auto requestId = data->requestId;
    delete data;
    QString url = available && account_url ? QString::fromUtf8(account_url) : QString();
    withGuardedBridge(guard, [requestId, available, url](ProtocolBridge *bridge) {
        QMetaObject::invokeMethod(bridge, [bridge, requestId, available, url]() {
            emit bridge->accountManagementProbed(requestId, available, url);
        }, Qt::QueuedConnection);
    });
}

static void passwordResetPageCallbackTrampoline(bool available, const char *page_url, void *userdata) {
    auto *data = static_cast<AuthDelegationCallbackData *>(userdata);
    auto *guard = data->guard;
    const auto requestId = data->requestId;
    delete data;
    QString url = available && page_url ? QString::fromUtf8(page_url) : QString();
    withGuardedBridge(guard, [requestId, available, url](ProtocolBridge *bridge) {
        QMetaObject::invokeMethod(bridge, [bridge, requestId, available, url]() {
            emit bridge->passwordResetPageProbed(requestId, available, url);
        }, Qt::QueuedConnection);
    });
}

static void emailThreepidSupportCallbackTrampoline(bool known, bool supported, void *userdata) {
    auto *data = static_cast<AuthDelegationCallbackData *>(userdata);
    auto *guard = data->guard;
    const auto requestId = data->requestId;
    delete data;
    withGuardedBridge(guard, [requestId, known, supported](ProtocolBridge *bridge) {
        QMetaObject::invokeMethod(bridge, [bridge, requestId, known, supported]() {
            emit bridge->emailThreepidSupportProbed(requestId, known, supported);
        }, Qt::QueuedConnection);
    });
}

struct MediaCallbackData {
    ProtocolBridge *bridge = nullptr;
    QString mxcUrl;
    BridgeCallbackGuard *guard = nullptr;
};

struct MediaExportCallbackData {
    ProtocolBridge *bridge = nullptr;
    QString mxcUrl;
    QString targetPath;
    BridgeCallbackGuard *guard = nullptr;
};

static void mediaProgressCallbackTrampoline(
    uint64_t received_bytes,
    uint64_t total_bytes,
    uint32_t phase,
    void *userdata) {
    auto *data = static_cast<MediaCallbackData *>(userdata);
    auto *guard = data->guard;
    const auto mxcUrl = data->mxcUrl;
    const auto received = static_cast<quint64>(received_bytes);
    const auto total = static_cast<quint64>(total_bytes);
    const auto qphase = static_cast<uint>(phase);
    withGuardedBridge(guard, [mxcUrl, received, total, qphase](ProtocolBridge *bridge) {
        QMetaObject::invokeMethod(bridge, [bridge, mxcUrl, received, total, qphase]() {
            emit bridge->mediaDownloadProgress(mxcUrl, received, total, qphase);
        }, Qt::QueuedConnection);
    });
}

static void mediaBytesCallbackTrampoline(
        bool success,
        const uint8_t *bytes,
        uintptr_t bytes_len,
        const char *mime,
        const char *filename,
        bool terminal,
        void *userdata) {
    auto *data = static_cast<MediaCallbackData *>(userdata);
    auto *guard = data->guard;
    QString mxcUrl = data->mxcUrl;
    delete data;
    QByteArray payload;
    if (success && bytes && bytes_len > 0) {
        payload = QByteArray(
            reinterpret_cast<const char *>(bytes),
            static_cast<qsizetype>(bytes_len));
    }
    const auto qmime = success && mime ? QString::fromUtf8(mime) : QString();
    const auto qfilename = success && filename ? QString::fromUtf8(filename) : QString();
    withGuardedBridge(guard, [success, terminal, mxcUrl, payload, qmime, qfilename](ProtocolBridge *bridge) {
        QMetaObject::invokeMethod(bridge, [bridge, success, terminal, mxcUrl, payload, qmime, qfilename]() {
            emit bridge->mediaBytesResolved(success, mxcUrl, payload, qmime, qfilename, terminal);
        }, Qt::QueuedConnection);
    });
}

static void mediaExportCallbackTrampoline(bool success, void *userdata) {
    auto *data = static_cast<MediaExportCallbackData *>(userdata);
    auto *guard = data->guard;
    const auto mxcUrl = data->mxcUrl;
    const auto targetPath = data->targetPath;
    delete data;
    withGuardedBridge(guard, [success, mxcUrl, targetPath](ProtocolBridge *bridge) {
        QMetaObject::invokeMethod(bridge, [bridge, success, mxcUrl, targetPath]() {
            emit bridge->mediaExported(success, mxcUrl, targetPath);
        }, Qt::QueuedConnection);
    });
}

static void mediaCallbackTrampoline(
        bool success,
        const char *local_path,
        bool terminal,
        void *userdata) {
    auto *data = static_cast<MediaCallbackData *>(userdata);
    auto *guard = data->guard;
    QString mxcUrl = data->mxcUrl;
    delete data;
    QString path = success && local_path ? QString::fromUtf8(local_path) : QString();
    withGuardedBridge(guard, [success, terminal, mxcUrl, path](ProtocolBridge *bridge) {
        QMetaObject::invokeMethod(bridge, [bridge, success, terminal, mxcUrl, path]() {
            emit bridge->mediaResolved(success, mxcUrl, path, terminal);
        }, Qt::QueuedConnection);
    });
}

static void sasCallbackTrampoline(
    bool success,
    FfiSasEmojiList list,
    void *userdata)
{
    QStringList emojis;
    QStringList labels;
    if (success && list.emojis && list.len > 0) {
        emojis.reserve(static_cast<int>(list.len));
        labels.reserve(static_cast<int>(list.len));
        for (size_t i = 0; i < list.len; ++i) {
            emojis.append(list.emojis[i].emoji
                ? QString::fromUtf8(list.emojis[i].emoji) : QString());
            labels.append(list.emojis[i].label
                ? QString::fromUtf8(list.emojis[i].label) : QString());
        }
    }
    tm_free_sas_emojis(list);
    withGuardedBridge(userdata, [success, emojis, labels](ProtocolBridge *bridge) {
        QMetaObject::invokeMethod(bridge,
            [bridge, success, emojis, labels]() {
                emit bridge->sasVerificationStarted(success, emojis, labels);
            }, Qt::QueuedConnection);
    });
}

static void qrCodeCallbackTrampoline(
    bool success,
    FfiQrCode qr,
    void *userdata)
{
    const int size = static_cast<int>(qr.size);
    QByteArray modules;
    if (success && qr.modules && size > 0) {
        modules = QByteArray(
            reinterpret_cast<const char *>(qr.modules), size * size);
    }
    tm_free_qr_code(qr);
    withGuardedBridge(userdata, [success, modules, size](ProtocolBridge *bridge) {
        QMetaObject::invokeMethod(bridge,
            [bridge, success, modules, size]() {
                emit bridge->qrCodeReady(success, modules, size);
            }, Qt::QueuedConnection);
    });
}

// --- Sessions + Encryption callback trampolines ---

static void deviceListCallbackTrampoline(
    bool success,
    FfiDeviceSessionList list,
    void *userdata)
{
    TeleMatrix::DeviceSessionList result;
    if (success && list.sessions && list.sessions_len > 0) {
        result.currentDeviceId = list.current_device_id
            ? QString::fromUtf8(list.current_device_id) : QString();
        result.sessions.reserve(static_cast<int>(list.sessions_len));
        for (size_t i = 0; i < list.sessions_len; ++i) {
            const auto &s = list.sessions[i];
            TeleMatrix::DeviceSession ds;
            ds.deviceId = s.device_id ? QString::fromUtf8(s.device_id) : QString();
            ds.displayName = s.display_name ? QString::fromUtf8(s.display_name) : QString();
            ds.isCurrent = s.is_current;
            ds.isDehydrated = s.is_dehydrated;
            ds.lastSeenTs = static_cast<qint64>(s.last_seen_ts);
            ds.hasLastSeenTs = s.has_last_seen_ts;
            ds.lastSeenIp = s.last_seen_ip ? QString::fromUtf8(s.last_seen_ip) : QString();
            ds.lastSeenUserAgent = s.last_seen_user_agent ? QString::fromUtf8(s.last_seen_user_agent) : QString();
            ds.appName = s.app_name ? QString::fromUtf8(s.app_name) : QString();
            ds.appVersion = s.app_version ? QString::fromUtf8(s.app_version) : QString();
            ds.deviceModel = s.device_model ? QString::fromUtf8(s.device_model) : QString();
            ds.os = s.os ? QString::fromUtf8(s.os) : QString();
            ds.browser = s.browser ? QString::fromUtf8(s.browser) : QString();
            ds.verificationState = static_cast<TeleMatrix::DeviceVerificationState>(s.verification_state);
            result.sessions.append(ds);
        }
    }
    tm_free_device_session_list(list);
    withGuardedBridge(userdata, [success, result](ProtocolBridge *bridge) {
        QMetaObject::invokeMethod(bridge,
            [bridge, success, result]() {
                emit bridge->ownDevicesReady(success, result);
            }, Qt::QueuedConnection);
    });
}

static void deleteDevicesCallbackTrampoline(
    bool success,
    FfiDeleteDevicesResult ffiResult,
    void *userdata)
{
    auto *data = static_cast<SimpleCallbackData *>(userdata);
    auto *guard = data->guard;
    delete data;
    TeleMatrix::DeleteDevicesResult result;
    result.completed = ffiResult.completed;
    result.challengeJson = ffiResult.challenge_json
        ? QString::fromUtf8(ffiResult.challenge_json) : QString();
    result.accountManagementUrl = ffiResult.account_management_url
        ? QString::fromUtf8(ffiResult.account_management_url) : QString();
    tm_free_delete_devices_result(ffiResult);
    withGuardedBridge(guard, [success, result](ProtocolBridge *bridge) {
        QMetaObject::invokeMethod(bridge,
            [bridge, success, result]() {
                emit bridge->devicesDeleted(success, result);
            }, Qt::QueuedConnection);
    });
}

static void encryptionOverviewCallbackTrampoline(
    bool success,
    FfiEncryptionOverview ffiOv,
    void *userdata)
{
    TeleMatrix::EncryptionOverview ov;
    if (success) {
        ov.deviceId = ffiOv.device_id ? QString::fromUtf8(ffiOv.device_id) : QString();
        ov.deviceEd25519 = ffiOv.device_ed25519 ? QString::fromUtf8(ffiOv.device_ed25519) : QString();
        ov.isCurrentDeviceVerified = ffiOv.is_current_device_verified;
        ov.crossSigningReady = ffiOv.cross_signing_ready;
        ov.crossSigningKeysCachedLocally = ffiOv.cross_signing_keys_cached_locally;
        ov.crossSigningKeysInSecretStorage = ffiOv.cross_signing_keys_in_secret_storage;
        ov.secretStorageReady = ffiOv.secret_storage_ready;
        ov.secretStorageDefaultKeyId = ffiOv.secret_storage_default_key_id
            ? QString::fromUtf8(ffiOv.secret_storage_default_key_id) : QString();
        ov.keyBackupUploadActive = ffiOv.key_backup_upload_active;
        ov.backupKeyCached = ffiOv.backup_key_cached;
        ov.backupKeyStoredIn4s = ffiOv.backup_key_stored_in_4s;
        ov.backupDisabledAccountFlag = ffiOv.backup_disabled_account_flag;
        ov.recoveryDisabledAccountFlag = ffiOv.recovery_disabled_account_flag;
        ov.historyDecryptable = ffiOv.history_decryptable;
        ov.healthState = static_cast<TeleMatrix::EncryptionHealthState>(ffiOv.health_state);
    }
    tm_free_encryption_overview(ffiOv);
    withGuardedBridge(userdata, [success, ov](ProtocolBridge *bridge) {
        QMetaObject::invokeMethod(bridge,
            [bridge, success, ov]() {
                emit bridge->encryptionOverviewReady(success, ov);
            }, Qt::QueuedConnection);
    });
}

static void recoveryKeyCreatedCallbackTrampoline(
    bool success,
    const char *recovery_key,
    void *userdata)
{
    QString key = success && recovery_key ? QString::fromUtf8(recovery_key) : QString();
    withGuardedBridge(userdata, [success, key](ProtocolBridge *bridge) {
        QMetaObject::invokeMethod(bridge,
            [bridge, success, key]() {
                emit bridge->recoveryKeyCreated(success, key);
            }, Qt::QueuedConnection);
    });
}

static void recoverySetupCallbackTrampoline(
    bool success,
    const char *recovery_key,
    uint32_t error_code,
    const char *error,
    void *userdata)
{
    // Both strings are only valid for this call — Rust frees them as soon as it returns.
    QString key = success && recovery_key ? QString::fromUtf8(recovery_key) : QString();
    QString message = error ? QString::fromUtf8(error) : QString();
    const int code = static_cast<int>(error_code);
    withGuardedBridge(userdata, [success, key, code, message](ProtocolBridge *bridge) {
        QMetaObject::invokeMethod(bridge,
            [bridge, success, key, code, message]() {
                emit bridge->recoverySetupResult(success, key, code, message);
            }, Qt::QueuedConnection);
    });
}

static void resetIdentityCallbackTrampoline(
    bool success,
    FfiResetIdentityResult ffiResult,
    void *userdata)
{
    TeleMatrix::ResetIdentityResult result;
    result.completed = ffiResult.completed;
    result.challengeJson = ffiResult.challenge_json
        ? QString::fromUtf8(ffiResult.challenge_json) : QString();
    tm_free_reset_identity_result(ffiResult);
    withGuardedBridge(userdata, [success, result](ProtocolBridge *bridge) {
        QMetaObject::invokeMethod(bridge,
            [bridge, success, result]() {
                emit bridge->identityResetResult(success, result);
            }, Qt::QueuedConnection);
    });
}

static void importKeysCallbackTrampoline(
    bool success,
    FfiImportKeysResult ffiResult,
    void *userdata)
{
    int imported = static_cast<int>(ffiResult.imported_count);
    int total = static_cast<int>(ffiResult.total_count);
    withGuardedBridge(userdata, [success, imported, total](ProtocolBridge *bridge) {
        QMetaObject::invokeMethod(bridge,
            [bridge, success, imported, total]() {
                emit bridge->e2eKeysImported(success, imported, total);
            }, Qt::QueuedConnection);
    });
}

struct SearchCallbackData {
    ProtocolBridge *bridge = nullptr;
    quint64 requestId = 0;
    BridgeCallbackGuard *guard = nullptr;
};

static void searchCallbackTrampoline(
    bool success,
    FfiSearchPage page,
    const char *error,
    void *userdata)
{
    using TeleMatrix::SearchPage;
    using TeleMatrix::SearchHit;

    auto *data = static_cast<SearchCallbackData *>(userdata);
    auto *guard = data->guard;
    const quint64 requestId = data->requestId;
    delete data;

    if (success) {
        SearchPage result;
        result.requestId = page.request_id;
        result.totalApprox = page.total_approx;
        result.nextToken = page.next_token
            ? QString::fromUtf8(page.next_token) : QString();
        result.done = page.done;
        result.e2eeDisabled = page.e2ee_disabled;
        result.indexing = page.indexing;
        if (page.hits && page.hits_len > 0) {
            result.hits.reserve(static_cast<int>(page.hits_len));
            for (size_t i = 0; i < page.hits_len; ++i) {
                const auto &ffi = page.hits[i];
                SearchHit hit;
                hit.roomId = ffi.room_id
                    ? QString::fromUtf8(ffi.room_id) : QString();
                hit.eventId = ffi.event_id
                    ? QString::fromUtf8(ffi.event_id) : QString();
                hit.senderId = ffi.sender_id
                    ? QString::fromUtf8(ffi.sender_id) : QString();
                hit.senderName = ffi.sender_name
                    ? QString::fromUtf8(ffi.sender_name) : QString();
                hit.timestamp = static_cast<qint64>(ffi.timestamp);
                hit.snippet = ffi.snippet
                    ? QString::fromUtf8(ffi.snippet) : QString();
                hit.rank = ffi.rank;
                hit.localOnly = ffi.local_only;
                result.hits.append(hit);
            }
        }
        tm_free_search_page(page);
        withGuardedBridge(guard, [requestId, result = std::move(result)](ProtocolBridge *bridge) mutable {
            QMetaObject::invokeMethod(bridge,
                [bridge, requestId, result = std::move(result)]() mutable {
                    bridge->handleSearchPageReady(requestId, result);
                }, Qt::QueuedConnection);
        });
    } else {
        QString errorStr = error
            ? QString::fromUtf8(error) : QStringLiteral("Unknown error");
        tm_free_search_page(page);
        withGuardedBridge(guard, [requestId, errorStr](ProtocolBridge *bridge) {
            QMetaObject::invokeMethod(bridge,
                [bridge, requestId, errorStr]() {
                    bridge->handleSearchFailed(requestId, errorStr);
                }, Qt::QueuedConnection);
        });
    }
}

// --- Room discovery callback trampolines ---

struct RoomDirectoryCallbackData {
    BridgeCallbackGuard *guard = nullptr;
    quint64 requestId = 0;
    // Which view asked, so the dialog can route the page without a second signal.
    bool isSpaceChildren = false;
    QString spaceId;
};

struct RoomTargetCallbackData {
    BridgeCallbackGuard *guard = nullptr;
    QString roomIdOrAlias;
};

static void roomDirectoryCallbackTrampoline(
    bool success,
    FfiRoomDirectoryPage page,
    const char *error,
    void *userdata)
{
    using TeleMatrix::RoomDirectoryEntry;
    using TeleMatrix::RoomDirectoryPage;

    auto *data = static_cast<RoomDirectoryCallbackData *>(userdata);
    auto *guard = data->guard;
    const quint64 requestId = data->requestId;
    const bool isSpaceChildren = data->isSpaceChildren;
    const QString spaceId = data->spaceId;
    delete data;

    if (success) {
        RoomDirectoryPage result;
        result.requestId = page.request_id;
        result.totalApprox = page.total_approx;
        result.nextToken = page.next_token
            ? QString::fromUtf8(page.next_token) : QString();
        result.done = page.done;
        result.isSpaceChildren = isSpaceChildren;
        result.spaceId = spaceId;

        if (page.entries && page.entries_len > 0) {
            result.entries.reserve(static_cast<int>(page.entries_len));
            for (size_t i = 0; i < page.entries_len; ++i) {
                const auto &ffi = page.entries[i];
                RoomDirectoryEntry entry;
                entry.roomId = ffi.room_id ? QString::fromUtf8(ffi.room_id) : QString();
                entry.name = ffi.name ? QString::fromUtf8(ffi.name) : QString();
                entry.topic = ffi.topic ? QString::fromUtf8(ffi.topic) : QString();
                entry.canonicalAlias = ffi.canonical_alias
                    ? QString::fromUtf8(ffi.canonical_alias) : QString();
                entry.avatarUrl = ffi.avatar_url
                    ? QString::fromUtf8(ffi.avatar_url) : QString();
                entry.memberCount = static_cast<int>(ffi.member_count);
                entry.childrenCount = static_cast<int>(ffi.children_count);
                entry.isSpace = ffi.is_space;
                entry.worldReadable = ffi.world_readable;
                entry.guestCanJoin = ffi.guest_can_join;
                entry.joinRule = TeleMatrix::convertFfiRoomJoinRule(ffi.join_rule);
                entry.membership = TeleMatrix::convertFfiRoomMembership(ffi.membership);
                if (ffi.via && ffi.via_len > 0) {
                    for (size_t v = 0; v < ffi.via_len; ++v) {
                        if (ffi.via[v]) {
                            entry.via.append(QString::fromUtf8(ffi.via[v]));
                        }
                    }
                }
                result.entries.append(entry);
            }
        }
        tm_free_room_directory_page(page);
        withGuardedBridge(guard, [requestId, result = std::move(result)](ProtocolBridge *bridge) mutable {
            QMetaObject::invokeMethod(bridge,
                [bridge, requestId, result = std::move(result)]() mutable {
                    bridge->handleRoomDirectoryPageReady(requestId, result);
                }, Qt::QueuedConnection);
        });
    } else {
        QString errorStr = error
            ? QString::fromUtf8(error) : QStringLiteral("Unknown error");
        tm_free_room_directory_page(page);
        withGuardedBridge(guard, [requestId, errorStr](ProtocolBridge *bridge) {
            QMetaObject::invokeMethod(bridge,
                [bridge, requestId, errorStr]() {
                    bridge->handleRoomDirectoryFailed(requestId, errorStr);
                }, Qt::QueuedConnection);
        });
    }
}

static void roomPreviewCallbackTrampoline(
    bool success,
    FfiRoomPreview preview,
    const char *error,
    void *userdata)
{
    auto *data = static_cast<RoomTargetCallbackData *>(userdata);
    auto *guard = data->guard;
    const QString target = data->roomIdOrAlias;
    delete data;

    TeleMatrix::RoomPreviewInfo info;
    if (success) {
        info.roomId = preview.room_id ? QString::fromUtf8(preview.room_id) : QString();
        info.name = preview.name ? QString::fromUtf8(preview.name) : QString();
        info.topic = preview.topic ? QString::fromUtf8(preview.topic) : QString();
        info.canonicalAlias = preview.canonical_alias
            ? QString::fromUtf8(preview.canonical_alias) : QString();
        info.avatarUrl = preview.avatar_url
            ? QString::fromUtf8(preview.avatar_url) : QString();
        info.memberCount = static_cast<int>(preview.member_count);
        info.isSpace = preview.is_space;
        info.joinRule = TeleMatrix::convertFfiRoomJoinRule(preview.join_rule);
        info.membership = TeleMatrix::convertFfiRoomMembership(preview.membership);
        info.worldReadable = preview.world_readable;
    }
    const QString errorStr = error ? QString::fromUtf8(error) : QString();
    tm_free_room_preview(preview);

    withGuardedBridge(guard, [target, success, info, errorStr](ProtocolBridge *bridge) {
        QMetaObject::invokeMethod(bridge,
            [bridge, target, success, info, errorStr]() {
                emit bridge->roomPreviewReady(target, success, info, errorStr);
            }, Qt::QueuedConnection);
    });
}

static void joinRoomCallbackTrampoline(
    bool success,
    const char *room_id,
    const char *error,
    void *userdata)
{
    auto *data = static_cast<RoomTargetCallbackData *>(userdata);
    auto *guard = data->guard;
    const QString target = data->roomIdOrAlias;
    delete data;

    // Both strings are borrowed — Rust frees them the moment this returns.
    const QString roomId = room_id ? QString::fromUtf8(room_id) : QString();
    const QString errorStr = error ? QString::fromUtf8(error) : QString();

    withGuardedBridge(guard, [target, success, roomId, errorStr](ProtocolBridge *bridge) {
        QMetaObject::invokeMethod(bridge,
            [bridge, target, success, roomId, errorStr]() {
                emit bridge->roomJoined(target, success, roomId, errorStr);
            }, Qt::QueuedConnection);
    });
}

static void knockRoomCallbackTrampoline(
    bool success,
    const char *room_id,
    const char *error,
    void *userdata)
{
    auto *data = static_cast<RoomTargetCallbackData *>(userdata);
    auto *guard = data->guard;
    const QString target = data->roomIdOrAlias;
    delete data;

    const QString roomId = room_id ? QString::fromUtf8(room_id) : QString();
    const QString errorStr = error ? QString::fromUtf8(error) : QString();

    withGuardedBridge(guard, [target, success, roomId, errorStr](ProtocolBridge *bridge) {
        QMetaObject::invokeMethod(bridge,
            [bridge, target, success, roomId, errorStr]() {
                emit bridge->roomKnocked(target, success, roomId, errorStr);
            }, Qt::QueuedConnection);
    });
}

static void previewMessagesCallbackTrampoline(
    const char *room_id,
    bool success,
    FfiTimeline timeline,
    const char *next_token,
    const char *error,
    void *userdata)
{
    auto *data = static_cast<RoomTargetCallbackData *>(userdata);
    auto *guard = data->guard;
    // The request stored the resolved room id; the borrowed `room_id` echoes it back.
    const QString target = room_id ? QString::fromUtf8(room_id) : data->roomIdOrAlias;
    delete data;

    // Rust handed us ownership of the items — convert, then free.
    auto items = TeleMatrix::convertFfiTimeline(timeline);
    tm_free_timeline(timeline);
    const QString nextToken = next_token ? QString::fromUtf8(next_token) : QString();
    const QString errorStr = error ? QString::fromUtf8(error) : QString();

    withGuardedBridge(guard,
        [target, success, items = std::move(items), nextToken, errorStr](
            ProtocolBridge *bridge) mutable {
        QMetaObject::invokeMethod(bridge,
            [bridge, target, success, items = std::move(items), nextToken, errorStr]() mutable {
                emit bridge->roomPreviewMessagesReady(
                    target, success, items, nextToken, errorStr);
            }, Qt::QueuedConnection);
    });
}

// --- Account settings callback trampolines ---

struct AccountSummaryCallbackData {
    ProtocolBridge *bridge = nullptr;
    BridgeCallbackGuard *guard = nullptr;
};

static void accountSummaryCallbackTrampoline(
    bool success,
    FfiAccountSummary summary,
    const char *error,
    void *userdata)
{
    auto *data = static_cast<AccountSummaryCallbackData *>(userdata);
    auto *guard = data->guard;
    delete data;

    TeleMatrix::AccountSummary result;
    QString errorStr;
    if (success) {
        result.userId = summary.user_id ? QString::fromUtf8(summary.user_id) : QString();
        result.displayName = summary.display_name ? QString::fromUtf8(summary.display_name) : QString();
        result.avatarUrl = summary.avatar_url ? QString::fromUtf8(summary.avatar_url) : QString();
        result.capabilities.canChangePassword = summary.capabilities.can_change_password;
        result.capabilities.canChange3pid = summary.capabilities.can_change_3pid;
        result.capabilities.canSetDisplayName = summary.capabilities.can_set_display_name;
        result.capabilities.canSetAvatarUrl = summary.capabilities.can_set_avatar_url;
    } else {
        errorStr = error ? QString::fromUtf8(error) : QStringLiteral("Unknown error");
    }
    tm_free_account_summary(summary);
    withGuardedBridge(guard, [success, result, errorStr](ProtocolBridge *bridge) {
        QMetaObject::invokeMethod(bridge, [bridge, success, result, errorStr]() {
            emit bridge->accountSummaryReady(success, result, errorStr);
        }, Qt::QueuedConnection);
    });
}

struct ThreePidListCallbackData {
    ProtocolBridge *bridge = nullptr;
    BridgeCallbackGuard *guard = nullptr;
};

static void threePidListCallbackTrampoline(
    bool success,
    FfiThreePidList list,
    const char *error,
    void *userdata)
{
    auto *data = static_cast<ThreePidListCallbackData *>(userdata);
    auto *guard = data->guard;
    delete data;

    QVector<TeleMatrix::ThreePid> items;
    QString errorStr;
    if (success && list.items && list.len > 0) {
        items.reserve(static_cast<int>(list.len));
        for (size_t i = 0; i < list.len; ++i) {
            const auto &ffi = list.items[i];
            TeleMatrix::ThreePid pid;
            pid.medium = static_cast<TeleMatrix::ThreePidMedium>(ffi.medium);
            pid.address = ffi.address ? QString::fromUtf8(ffi.address) : QString();
            pid.validatedAt = ffi.validated_at;
            pid.addedAt = ffi.added_at;
            items.append(pid);
        }
    } else if (!success) {
        errorStr = error ? QString::fromUtf8(error) : QStringLiteral("Unknown error");
    }
    tm_free_3pid_list(list);
    withGuardedBridge(guard, [success, items, errorStr](ProtocolBridge *bridge) {
        QMetaObject::invokeMethod(bridge, [bridge, success, items, errorStr]() {
            emit bridge->threepidsReady(success, items, errorStr);
        }, Qt::QueuedConnection);
    });
}

struct ThreePidTokenCallbackData {
    ProtocolBridge *bridge = nullptr;
    BridgeCallbackGuard *guard = nullptr;
};

static void threePidTokenCallbackTrampoline(
    bool success,
    FfiThreePidTokenResponse response,
    const char *error,
    void *userdata)
{
    auto *data = static_cast<ThreePidTokenCallbackData *>(userdata);
    auto *guard = data->guard;
    delete data;

    TeleMatrix::ThreePidTokenResponse token;
    QString errorStr;
    if (success) {
        token.sid = response.sid ? QString::fromUtf8(response.sid) : QString();
        token.submitUrl = response.submit_url ? QString::fromUtf8(response.submit_url) : QString();
    } else {
        errorStr = error ? QString::fromUtf8(error) : QStringLiteral("Unknown error");
    }
    tm_free_3pid_token_response(response);
    withGuardedBridge(guard, [success, token, errorStr](ProtocolBridge *bridge) {
        QMetaObject::invokeMethod(bridge, [bridge, success, token, errorStr]() {
            emit bridge->threepidTokenReady(success, token, errorStr);
        }, Qt::QueuedConnection);
    });
}

struct AccountActionCallbackData {
    ProtocolBridge *bridge = nullptr;
    enum class Kind { ChangePassword, Deactivate, Add3pid } kind;
    BridgeCallbackGuard *guard = nullptr;
};

static void accountActionCallbackTrampoline(
    FfiAccountActionResult result,
    void *userdata)
{
    auto *data = static_cast<AccountActionCallbackData *>(userdata);
    auto *guard = data->guard;
    auto kind = data->kind;
    delete data;

    TeleMatrix::AccountActionResult actionResult;
    actionResult.completed = result.completed;
    actionResult.errorMessage = result.error_message ? QString::fromUtf8(result.error_message) : QString();
    actionResult.uiaSession = result.uia_session ? QString::fromUtf8(result.uia_session) : QString();
    actionResult.uiaFlowsJson = result.uia_flows_json ? QString::fromUtf8(result.uia_flows_json) : QString();
    tm_free_account_action_result(result);

    withGuardedBridge(guard, [actionResult, kind](ProtocolBridge *bridge) {
        QMetaObject::invokeMethod(bridge, [bridge, actionResult, kind]() {
            switch (kind) {
            case AccountActionCallbackData::Kind::ChangePassword:
                emit bridge->changePasswordResult(actionResult);
                break;
            case AccountActionCallbackData::Kind::Deactivate:
                emit bridge->deactivateAccountResult(actionResult);
                break;
            case AccountActionCallbackData::Kind::Add3pid:
                emit bridge->threepidAdded(actionResult);
                break;
            }
        }, Qt::QueuedConnection);
    });
}

static void verificationStateCallbackTrampoline(
    uint32_t state,
    const char *flowId,
    void *userdata)
{
    // Copy flowId now: the Rust-owned C string is only valid for the duration
    // of this synchronous call, but the emit below runs later (QueuedConnection).
    const auto flow = flowId ? QString::fromUtf8(flowId) : QString();
    withGuardedBridge(userdata, [state, flow](ProtocolBridge *bridge) {
        QMetaObject::invokeMethod(bridge,
            [bridge, state, flow]() {
                // Latch verified the instant SAS completes; the SDK observable confirms ~1s later.
                constexpr uint32_t kVerificationDone = 8;
                if (state == kVerificationDone) {
                    bridge->handleDeviceVerifiedChanged(true);
                }
                emit bridge->verificationStateChanged(static_cast<int>(state), flow);
            }, Qt::QueuedConnection);
    });
}

static void incomingVerificationRequestCallbackTrampoline(
    const char *transactionId,
    const char *deviceId,
    const char *deviceName,
    void *userdata)
{
    const auto tx = transactionId ? QString::fromUtf8(transactionId) : QString();
    const auto id = deviceId ? QString::fromUtf8(deviceId) : QString();
    const auto name = deviceName ? QString::fromUtf8(deviceName) : QString();
    withGuardedBridge(userdata, [tx, id, name](ProtocolBridge *bridge) {
        QMetaObject::invokeMethod(bridge,
            [bridge, tx, id, name]() {
                emit bridge->incomingVerificationRequestReceived(tx, id, name);
            }, Qt::QueuedConnection);
    });
}

// Another user's cross-signing trust state changed (identity-updates stream).
static void userTrustChangedTrampoline(
    const char *userId,
    uint32_t state,
    void *userdata)
{
    const auto uid = userId ? QString::fromUtf8(userId) : QString();
    const int trust = static_cast<int>(state);
    withGuardedBridge(userdata, [uid, trust](ProtocolBridge *bridge) {
        QMetaObject::invokeMethod(bridge,
            [bridge, uid, trust]() {
                emit bridge->userTrustChanged(uid, trust);
            }, Qt::QueuedConnection);
    });
}

// Another user requested to verify with us (in-room cross-user verification).
static void incomingUserVerificationRequestTrampoline(
    const char *flowId,
    const char *userId,
    const char *displayName,
    void *userdata)
{
    const auto flow = flowId ? QString::fromUtf8(flowId) : QString();
    const auto uid = userId ? QString::fromUtf8(userId) : QString();
    const auto name = displayName ? QString::fromUtf8(displayName) : QString();
    withGuardedBridge(userdata, [flow, uid, name](ProtocolBridge *bridge) {
        QMetaObject::invokeMethod(bridge,
            [bridge, flow, uid, name]() {
                emit bridge->incomingUserVerificationRequestReceived(flow, uid, name);
            }, Qt::QueuedConnection);
    });
}

// One-shot result of a `userTrustState` query (carries the queried user id).
static void userTrustStateTrampoline(bool success, uint32_t state, void *userdata) {
    auto *data = static_cast<UserTrustStateCallbackData *>(userdata);
    const auto userId = data->userId;
    auto *guard = data->guard;
    delete data;
    const int trust = success ? static_cast<int>(state) : 0;
    withGuardedBridge(guard, [userId, trust](ProtocolBridge *bridge) {
        QMetaObject::invokeMethod(bridge,
            [bridge, userId, trust]() {
                emit bridge->userTrustStateResult(userId, trust);
            }, Qt::QueuedConnection);
    });
}

static void notificationCallbackTrampoline(
    const char *roomId,
    const char *eventId,
    const char *senderDisplayName,
    const char *senderAvatarUrl,
    const char *roomDisplayName,
    const char *body,
    bool isDirect,
    bool isMention,
    uint64_t timestamp,
    void *userdata)
{
    // Copy all C strings to QString before taking the guard lock: the Rust-side
    // pointers are valid only for this stack frame.
    const auto roomIdStr = roomId ? QString::fromUtf8(roomId) : QString();
    const auto eventIdStr = eventId ? QString::fromUtf8(eventId) : QString();
    const auto senderStr = senderDisplayName ? QString::fromUtf8(senderDisplayName) : QString();
    const auto avatarStr = senderAvatarUrl ? QString::fromUtf8(senderAvatarUrl) : QString();
    const auto roomNameStr = roomDisplayName ? QString::fromUtf8(roomDisplayName) : QString();
    const auto bodyStr = body ? QString::fromUtf8(body) : QString();
    const auto ts = static_cast<qint64>(timestamp);
    withGuardedBridge(userdata, [=](ProtocolBridge *bridge) {
        QMetaObject::invokeMethod(bridge,
            [=]() {
                emit bridge->incomingNotification(
                    roomIdStr, eventIdStr, senderStr, avatarStr, roomNameStr, bodyStr,
                    isDirect, isMention, ts);
            }, Qt::QueuedConnection);
    });
}

static void memberSyncCallbackTrampoline(
    const char *roomId,
    bool inProgress,
    void *userdata)
{
    const auto roomIdStr = roomId ? QString::fromUtf8(roomId) : QString();
    withGuardedBridge(userdata, [=](ProtocolBridge *bridge) {
        QMetaObject::invokeMethod(bridge,
            [=]() {
                emit bridge->memberSyncStateChanged(roomIdStr, inProgress);
            }, Qt::QueuedConnection);
    });
}

static void inviteNotificationCallbackTrampoline(
    const char *roomId,
    const char *inviterDisplayName,
    const char *inviterAvatarUrl,
    const char *roomDisplayName,
    bool isDirect,
    void *userdata)
{
    // Copy the C strings before taking the guard lock: the Rust-side pointers are
    // valid only for this stack frame.
    const auto roomIdStr = roomId ? QString::fromUtf8(roomId) : QString();
    const auto inviterStr = inviterDisplayName
        ? QString::fromUtf8(inviterDisplayName) : QString();
    const auto avatarStr = inviterAvatarUrl
        ? QString::fromUtf8(inviterAvatarUrl) : QString();
    const auto roomNameStr = roomDisplayName
        ? QString::fromUtf8(roomDisplayName) : QString();
    withGuardedBridge(userdata, [=](ProtocolBridge *bridge) {
        QMetaObject::invokeMethod(bridge,
            [=]() {
                emit bridge->incomingInvite(
                    roomIdStr, inviterStr, avatarStr, roomNameStr, isDirect);
            }, Qt::QueuedConnection);
    });
}

static void newLoginCallbackTrampoline(
    const char *deviceId,
    const char *displayName,
    const char *lastSeenIp,
    uint64_t lastSeenTs,
    void *userdata)
{
    // Copy the C strings before taking the guard lock (see the invite trampoline).
    const auto deviceIdStr = deviceId ? QString::fromUtf8(deviceId) : QString();
    const auto nameStr = displayName ? QString::fromUtf8(displayName) : QString();
    const auto ipStr = lastSeenIp ? QString::fromUtf8(lastSeenIp) : QString();
    const auto ts = static_cast<qint64>(lastSeenTs);
    withGuardedBridge(userdata, [=](ProtocolBridge *bridge) {
        QMetaObject::invokeMethod(bridge,
            [=]() {
                emit bridge->newLoginReceived(deviceIdStr, nameStr, ipStr, ts);
            }, Qt::QueuedConnection);
    });
}

static void uploadProgressCallbackTrampoline(
    const char *transactionId,
    uint64_t current,
    uint64_t total,
    void *userdata)
{
    const auto txnStr = transactionId ? QString::fromUtf8(transactionId) : QString();
    withGuardedBridge(userdata, [=](ProtocolBridge *bridge) {
        QMetaObject::invokeMethod(bridge, [=]() {
            emit bridge->uploadProgress(txnStr, current, total);
        }, Qt::QueuedConnection);
    });
}

} // end extern "C" — a C-linkage function may not return a C++ class type (MSVC C2526)

// Parse the FFI's `[[emoji,count],...]` JSON into (emoji, count) pairs.
static QVector<QPair<QString, int>> parseRecentEmojiPairs(const QByteArray &json) {
    QVector<QPair<QString, int>> pairs;
    const auto doc = QJsonDocument::fromJson(json);
    if (!doc.isArray()) {
        return pairs;
    }
    for (const auto &entry : doc.array()) {
        const auto arr = entry.toArray();
        if (arr.size() >= 2) {
            const auto emoji = arr.at(0).toString();
            const auto count = arr.at(1).toInt(1);
            if (!emoji.isEmpty()) {
                pairs.push_back({ emoji, count });
            }
        }
    }
    return pairs;
}

extern "C" {

static void recentEmojiCallbackTrampoline(const char *jsonPairs, void *userdata)
{
    const auto pairs = parseRecentEmojiPairs(
        jsonPairs ? QByteArray(jsonPairs) : QByteArray());
    withGuardedBridge(userdata, [pairs](ProtocolBridge *bridge) {
        QMetaObject::invokeMethod(bridge, [bridge, pairs]() {
            emit bridge->recentEmojiChanged(pairs);
        }, Qt::QueuedConnection);
    });
}

static void deviceVerifiedCallbackTrampoline(bool verified, void *userdata)
{
    withGuardedBridge(userdata, [verified](ProtocolBridge *bridge) {
        QMetaObject::invokeMethod(bridge,
            [bridge, verified]() {
                bridge->handleDeviceVerifiedChanged(verified);
            }, Qt::QueuedConnection);
    });
}

struct PasswordResetTokenCallbackData {
    ProtocolBridge *bridge = nullptr;
    QString clientSecret;
    BridgeCallbackGuard *guard = nullptr;
};

static void passwordResetTokenCallbackTrampoline(
    bool success,
    const char *sid,
    const char * /*submit_url*/,
    const char *error,
    void *userdata)
{
    auto *data = static_cast<PasswordResetTokenCallbackData *>(userdata);
    auto *guard = data->guard;
    QString clientSecret = data->clientSecret;
    delete data;
    QString sidStr = success && sid ? QString::fromUtf8(sid) : QString();
    QString errStr = !success && error ? QString::fromUtf8(error) : QString();
    withGuardedBridge(guard, [success, sidStr, clientSecret, errStr](ProtocolBridge *bridge) {
        QMetaObject::invokeMethod(bridge, [bridge, success, sidStr, clientSecret, errStr]() {
            emit bridge->passwordResetTokenSent(success, sidStr, clientSecret, errStr);
        }, Qt::QueuedConnection);
    });
}

} // extern "C"

namespace TeleMatrix {

namespace {

struct PercentCounterItem {
    int index = 0;
    int percent = 0;
    int remainder = 0;
};

void adjustPercentCount(std::vector<PercentCounterItem> &items, int left) {
    std::sort(items.begin(), items.end(), [](const PercentCounterItem &a, const PercentCounterItem &b) {
        if (a.remainder != b.remainder) {
            return a.remainder > b.remainder;
        }
        return a.percent < b.percent;
    });
    for (int i = 0, count = static_cast<int>(items.size()); i != count && left > 0;) {
        const auto percent = items[i].percent;
        const auto remainder = items[i].remainder;
        auto j = i + 1;
        while (j != count
            && items[j].percent == percent
            && items[j].remainder == remainder) {
            ++j;
        }
        if (remainder == 0) {
            break;
        }
        const auto equal = j - i;
        if (equal <= left) {
            left -= equal;
            for (; i != j; ++i) {
                ++items[i].percent;
            }
        } else {
            i = j;
        }
    }
}

void countNicePercent(const QVector<int> &votes, int total, QVector<int> &result) {
    result.fill(0, votes.size());
    if (total <= 0 || votes.isEmpty()) {
        return;
    }

    std::vector<PercentCounterItem> items;
    items.reserve(static_cast<size_t>(votes.size()));
    auto left = 100;
    for (auto i = 0; i != votes.size(); ++i) {
        const auto percent = (votes[i] * 100) / total;
        const auto remainder = (votes[i] * 100) - (percent * total);
        items.push_back(PercentCounterItem{
            .index = i,
            .percent = percent,
            .remainder = remainder,
        });
        left -= percent;
    }
    if (left > 0 && left <= static_cast<int>(items.size())) {
        adjustPercentCount(items, left);
    }
    for (const auto &item : items) {
        result[item.index] = item.percent;
    }
}

[[nodiscard]] QString ffiString(const char *value) {
    return value ? QString::fromUtf8(value) : QString();
}

// convertFfiContentType / convertFfiPreviewType / convertFfiSendState moved to
// protocol/ffi_conversions.{h,cpp} so the discriminant mapping is unit-testable
// without the Rust runtime. They are referenced here via the included header.

[[nodiscard]] QString translateUtdBody(uint32_t cause) {
    switch (cause) {
    case 1:
        return QCoreApplication::translate(
            "UnableToDecrypt", "Sent before you joined this chat, so its keys were never shared with you.");
    case 2:
        return QCoreApplication::translate(
            "UnableToDecrypt", "The sender's verified identity has changed.");
    case 3:
        return QCoreApplication::translate(
            "UnableToDecrypt", "Sent from a device the sender hasn't verified.");
    case 4:
        return QCoreApplication::translate(
            "UnableToDecrypt", "Sent from a device that's no longer available.");
    case 5:
        return QCoreApplication::translate(
            "UnableToDecrypt", "Sent before this device was set up. Earlier messages can't be opened here.");
    case 6:
        return QCoreApplication::translate(
            "UnableToDecrypt", "The sender didn't share the keys with this device.");
    case 7:
        return QCoreApplication::translate(
            "UnableToDecrypt", "The sender chose not to share the keys for this message.");
    case 8:
        return QCoreApplication::translate(
            "UnableToDecrypt", "Verify this device to read messages sent before you signed in.");
    case 0:
    default:
        return QCoreApplication::translate(
            "UnableToDecrypt", "Re-establishing the secure session may restore access to this message.");
    }
}

[[nodiscard]] QString translatedBodyFor(const FfiTimelineItem &ffi, ContentType type) {
    auto body = ffiString(ffi.body);
    if (type == ContentType::Service) {
        body = translateServiceBody(body);
    } else if (type == ContentType::UnableToDecrypt) {
        body = translateUtdBody(ffi.utd_cause);
    }
    return body;
}

[[nodiscard]] TimelineSenderInfo convertFfiSenderInfo(const FfiTimelineItem &ffi) {
    return TimelineSenderInfo{
        .id = ffiString(ffi.sender_user_id),
        .name = ffiString(ffi.sender_display_name),
        .avatarUrl = ffiString(ffi.sender_avatar_url),
    };
}

[[nodiscard]] TimelineMediaContent convertFfiMediaContent(const FfiTimelineItem &ffi) {
    return TimelineMediaContent{
        .body = translatedBodyFor(ffi, convertFfiContentType(ffi.content_type)),
        .url = ffiString(ffi.media_url),
        .mime = ffiString(ffi.media_mime),
        .filename = ffiString(ffi.media_filename),
        .caption = ffiString(ffi.media_caption),
        .thumbUrl = ffiString(ffi.media_thumb_url),
        .blurhash = ffiString(ffi.media_blurhash),
        .size = static_cast<quint64>(ffi.media_size),
        .width = static_cast<int>(ffi.media_width),
        .height = static_cast<int>(ffi.media_height),
        .durationMs = static_cast<quint64>(ffi.media_duration_ms),
    };
}

[[nodiscard]] QByteArray convertFfiAudioWaveform(const FfiTimelineItem &ffi) {
    QByteArray result;
    if (!ffi.audio_waveform_json) {
        return result;
    }
    const auto doc = QJsonDocument::fromJson(QByteArray(ffi.audio_waveform_json));
    if (!doc.isArray()) {
        return result;
    }
    const auto arr = doc.array();
    result.reserve(arr.size());
    for (const auto &value : arr) {
        result.append(static_cast<char>(qBound(0, value.toInt(), 31)));
    }
    return result;
}

[[nodiscard]] TimelinePollContent convertFfiPollContent(const FfiTimelineItem &ffi) {
    TimelinePollContent result;
    result.question = ffiString(ffi.poll_question);
    result.subtitle = translatePollSubtitle(ffiString(ffi.poll_subtitle));
    result.totalVoters = static_cast<int>(ffi.poll_total_voters);
    result.maxSelections = qMax(1, static_cast<int>(ffi.poll_max_selections));
    result.isClosed = ffi.poll_is_closed;
    result.isMultiChoice = ffi.poll_is_multi_choice;
    result.isQuiz = ffi.poll_is_quiz;
    result.hasVoted = ffi.poll_has_voted;
    result.kind = (ffi.poll_kind == 1)
        ? PollKind::Undisclosed
        : PollKind::Disclosed;

    QVector<int> voteCounts;
    auto maxVotes = 0;
    if (ffi.poll_options_json) {
        const auto doc = QJsonDocument::fromJson(QByteArray(ffi.poll_options_json));
        if (doc.isArray()) {
            const auto arr = doc.array();
            result.options.reserve(arr.size());
            voteCounts.reserve(arr.size());
            for (const auto &value : arr) {
                const auto obj = value.toObject();
                PollOptionInfo option;
                option.id = obj.value(QStringLiteral("id")).toString();
                option.text = obj.value(QStringLiteral("text")).toString();
                option.voteCount = obj.value(QStringLiteral("vote_count")).toInt();
                option.isChosen = obj.value(QStringLiteral("is_chosen")).toBool();
                option.isCorrect = obj.value(QStringLiteral("is_correct")).toBool();
                maxVotes = std::max(maxVotes, option.voteCount);
                voteCounts.append(option.voteCount);
                result.options.append(option);
            }
        }
    }

    QVector<int> nicePercents;
    auto sumVoteCounts = 0;
    for (const auto voteCount : voteCounts) {
        sumVoteCounts += voteCount;
    }
    const auto percentTotal = std::max(result.totalVoters, sumVoteCounts);
    countNicePercent(voteCounts, qMax(1, percentTotal), nicePercents);
    for (auto index = 0; index != result.options.size(); ++index) {
        result.options[index].votePercent = (index < nicePercents.size())
            ? nicePercents[index]
            : 0;
        result.options[index].filling = (maxVotes > 0)
            ? (static_cast<double>(result.options[index].voteCount) / maxVotes)
            : 0.0;
    }

    return result;
}

[[nodiscard]] TimelineContent convertFfiContent(const FfiTimelineItem &ffi) {
    const auto type = convertFfiContentType(ffi.content_type);
    const auto body = translatedBodyFor(ffi, type);
    switch (type) {
    case ContentType::Image:
        return TimelineImageContent{ .media = convertFfiMediaContent(ffi) };
    case ContentType::File:
        return TimelineFileContent{ .media = convertFfiMediaContent(ffi) };
    case ContentType::Video:
        return TimelineVideoContent{ .media = convertFfiMediaContent(ffi) };
    case ContentType::Audio:
        return TimelineAudioContent{
            .media = convertFfiMediaContent(ffi),
            .isVoice = ffi.is_voice_message,
            .waveform = convertFfiAudioWaveform(ffi),
        };
    case ContentType::Service:
        return TimelineServiceContent{ .body = body };
    case ContentType::Poll:
        return convertFfiPollContent(ffi);
    case ContentType::UnableToDecrypt:
        return TimelineUnableToDecryptContent{
            .body = body,
            .cause = static_cast<int>(ffi.utd_cause),
            .utdState = static_cast<int>(ffi.utd_state),
        };
    case ContentType::Text:
    default:
        return TimelineTextContent{
            .body = body,
            .formattedBody = ffiString(ffi.formatted_body),
        };
    }
}

[[nodiscard]] std::optional<TimelineReplyInfo> convertFfiReplyInfo(const FfiTimelineItem &ffi) {
    TimelineReplyInfo result;
    result.eventId = ffiString(ffi.reply_to_event_id);
    result.senderName = ffiString(ffi.reply_preview_sender_name);
    result.text = ffiString(ffi.reply_preview_text);
    result.thumbUrl = ffiString(ffi.reply_preview_thumb_url);
    result.hasThumb = ffi.reply_preview_has_thumb;
    result.isTextColorized = ffi.reply_preview_is_text_colorized;
    result.isDeleted = ffi.reply_preview_is_deleted;
    result.isUnavailable = ffi.reply_preview_is_unavailable;
    if (result.eventId.isEmpty()
        && result.senderName.isEmpty()
        && result.text.isEmpty()
        && result.thumbUrl.isEmpty()
        && !result.hasThumb
        && !result.isDeleted
        && !result.isUnavailable) {
        return std::nullopt;
    }
    return result;
}

[[nodiscard]] std::optional<TimelineForwardInfo> convertFfiForwardInfo(const FfiTimelineItem &ffi) {
    TimelineForwardInfo result{
        .senderName = ffiString(ffi.forwarded_from_sender_name),
        .senderId = ffiString(ffi.forwarded_from_sender_id),
        .avatarUrl = ffiString(ffi.forwarded_from_avatar_url),
    };
    if (result.senderName.isEmpty()) {
        return std::nullopt;
    }
    return result;
}

[[nodiscard]] std::optional<TimelineUrlPreviewInfo> convertFfiUrlPreview(const FfiTimelineItem &ffi) {
    TimelineUrlPreviewInfo result;
    result.url = ffiString(ffi.url_preview_url);
    result.siteName = ffiString(ffi.url_preview_site_name);
    result.title = ffiString(ffi.url_preview_title);
    result.description = ffiString(ffi.url_preview_description);
    result.imageUrl = ffiString(ffi.url_preview_image_url);
    result.imageWidth = static_cast<int>(ffi.url_preview_image_width);
    result.imageHeight = static_cast<int>(ffi.url_preview_image_height);
    result.type = convertFfiPreviewType(ffi.url_preview_type);
    result.duration = static_cast<int>(ffi.url_preview_duration);
    result.author = ffiString(ffi.url_preview_author);
    result.hasLargeMedia = ffi.url_preview_has_large_media;
    result.siteNameCanonical = ffiString(ffi.url_preview_site_name_canonical);
    if (result.url.isEmpty()
        && result.siteName.isEmpty()
        && result.title.isEmpty()
        && result.description.isEmpty()
        && result.imageUrl.isEmpty()
        && result.type == PreviewType::None
        && result.duration == 0
        && result.author.isEmpty()
        && !result.hasLargeMedia) {
        return std::nullopt;
    }
    return result;
}

[[nodiscard]] TimelineEncryptionInfo convertFfiEncryptionInfo(const FfiTimelineItem &ffi) {
    return TimelineEncryptionInfo{
        .encrypted = ffi.is_encrypted,
        .decryptionError = ffiString(ffi.decryption_error),
    };
}

[[nodiscard]] TimelineDeliveryInfo convertFfiDeliveryInfo(const FfiTimelineItem &ffi) {
    return TimelineDeliveryInfo{
        .sendState = convertFfiSendState(ffi.send_state),
        .uploadProgress = ffi.upload_progress,
        .outgoing = ffi.is_outgoing,
        .deleted = ffi.is_deleted,
    };
}

[[nodiscard]] QVector<ReactionInfo> convertFfiReactions(const FfiTimelineItem &ffi) {
    QVector<ReactionInfo> result;
    if (!ffi.reactions) {
        return result;
    }
    const auto doc = QJsonDocument::fromJson(QByteArray(ffi.reactions));
    if (!doc.isArray()) {
        return result;
    }
    const auto arr = doc.array();
    result.reserve(arr.size());
    for (const auto &value : arr) {
        const auto obj = value.toObject();
        ReactionInfo reaction;
        reaction.key = obj.value(QStringLiteral("key")).toString();
        reaction.count = obj.value(QStringLiteral("count")).toInt();
        reaction.isSelf = obj.value(QStringLiteral("is_self")).toBool();
        if (!reaction.key.isEmpty() && reaction.count > 0) {
            result.append(reaction);
        }
    }
    return result;
}

} // namespace

// --- ProtocolBridge implementation ---

ProtocolBridge::ProtocolBridge(const QString &dataDir, QObject *parent)
    : QObject(parent)
{
    _callbackGuard = std::make_shared<BridgeCallbackGuard>();
    _callbackGuard->bridge = this;
    const QByteArray dir = dataDir.toUtf8();
    _handle = tm_create(dataDir.isEmpty() ? nullptr : dir.constData());
    if (_handle) {
        registerCallbacks();
    }
    // Keep a live cross-signing trust cache so any surface can read a user's
    // trust as an O(1) lookup during paint instead of an FFI call per frame.
    const auto cacheTrust = [this](const QString &userId, int state) {
        _userTrustCache.insert(userId, state);
    };
    connect(this, &ProtocolBridge::userTrustChanged, this, cacheTrust);
    connect(this, &ProtocolBridge::userTrustStateResult, this, cacheTrust);

    // Saved Messages is never auto-created — it exists only after an explicit
    // forward / open. At session start just ADOPT an existing room (create=
    // false) so, if one is already there, the list can label it and open it
    // instantly; when there is none the cached id simply stays empty.
    connect(this, &ProtocolBridge::loginResult, this,
        [this](bool success, const QString &, const QString &, const QString &) {
            if (success) {
                ensureSavedMessagesRoom(/*create=*/false);
            }
        });
    connect(this, &ProtocolBridge::sessionRestored, this,
        [this](bool success, const QString &, const QString &, const QString &,
               const QString &) {
            if (success) {
                ensureSavedMessagesRoom(/*create=*/false);
            }
        });
}

void ProtocolBridge::ensureUserTrust(const QString &userId) {
    if (userId.isEmpty() || _userTrustCache.contains(userId)) {
        return;
    }
    _userTrustCache.insert(userId, 0); // provisional, prevents duplicate queries
    userTrustState(userId);
}

ProtocolBridge::~ProtocolBridge() {
    // Barrier: block until any persistent callback trampoline currently running
    // on a tokio worker thread has finished touching this bridge, and make every
    // later one observe a null bridge and no-op. Must happen before tm_destroy()
    // so the worker threads can never dereference the freed bridge. See
    // BridgeCallbackGuard for the full contract.
    if (_callbackGuard) {
        std::lock_guard<std::mutex> lock(_callbackGuard->mutex);
        _callbackGuard->bridge = nullptr;
    }
    if (_handle) {
        // App-quit path only; logout disposes via shutdownAsync(). Short cap so quit can't hang.
        tm_clear_callbacks(_handle);
        tm_destroy(_handle, 1500);
        _handle = nullptr;
    }
}

void ProtocolBridge::shutdownAsync() {
    if (_shutdownStarted) {
        return;
    }
    _shutdownStarted = true;
    if (_callbackGuard) {
        std::lock_guard<std::mutex> lock(_callbackGuard->mutex);
        _callbackGuard->bridge = nullptr;
    }
    if (!_handle) {
        emit shutdownComplete();
        deleteLater();
        return;
    }
    auto *handle = _handle;
    _handle = nullptr;
    tm_clear_callbacks(handle);
    // The bridge must outlive tm_destroy: one-shot FFI callbacks hold a raw
    // pointer to it and keep firing until the runtime is fully shut down. Drain
    // on a background thread, then deleteLater() once it signals done.
    // shared_ptr (not a raw heap atomic): if the bridge and its child QTimer are
    // torn down (e.g. app quit) before the detached drain finishes, the thread's
    // store() still targets live memory and nothing is leaked.
    auto done = std::make_shared<std::atomic_bool>(false);
    std::thread([handle, done] {
        // 2s drain cap: matrix-sdk leaves sync/sliding long-poll tasks parked on
        // network I/O that never exit on client drop, so waiting for the runtime
        // to reach idle just stalls logout (~35s). The drain only needs to outlast
        // the sqlite/deadpool blocking-pool cleanup (sub-second; the app-quit path
        // above proves 1.5s is enough); the lingering async tasks are aborted
        // safely by shutdown_timeout.
        tm_destroy(handle, 2000);
        done->store(true, std::memory_order_release);
    }).detach();
    auto *poll = new QTimer(this);
    connect(poll, &QTimer::timeout, this, [this, poll, done] {
        if (!done->load(std::memory_order_acquire)) {
            return;
        }
        poll->stop();
        emit shutdownComplete();
        deleteLater();
    });
    poll->start(250);
}

std::thread ProtocolBridge::drainForQuit() {
    // Null the callback guard first (same barrier as ~ProtocolBridge): make every
    // later trampoline observe a null bridge and no-op before tm_destroy runs on
    // the worker. Then move the handle out so the destructor's tm_destroy no-ops,
    // and drain on a caller-joined thread. See code-review-2026-07-19 PERF-2.
    if (_callbackGuard) {
        std::lock_guard<std::mutex> lock(_callbackGuard->mutex);
        _callbackGuard->bridge = nullptr;
    }
    auto *handle = _handle;
    _handle = nullptr;
    if (!handle) {
        return std::thread();
    }
    tm_clear_callbacks(handle);
    return std::thread([handle] { tm_destroy(handle, 1500); });
}

void ProtocolBridge::setSyncState(int state) {
    if (_syncState != state) {
        qDebug() << "[sync] state" << _syncState << "->" << state
            << "(0=not-started 1=syncing 2=synced 3=store-error)";
        _syncState = state;
        emit syncStateChanged(state);
    }
    if (state == 2) {
        setInitialDialogsLoadState(InitialDialogsLoadState::Ready);
    }
}

void ProtocolBridge::setInitialDialogsLoadState(InitialDialogsLoadState state) {
    if (_initialDialogsLoadState != state) {
        _initialDialogsLoadState = state;
        emit initialDialogsLoadStateChanged(state);
    }
}

void ProtocolBridge::registerCallbacks() {
    // Persistent callbacks fire from tokio worker threads for the bridge's whole
    // lifetime, so they receive the shared guard (not a raw `this`) as userdata.
    // See BridgeCallbackGuard for why.
    void *const guard = static_cast<void *>(_callbackGuard.get());
    tm_set_room_change_callback(
        _handle,
        roomChangeCallbackTrampoline,
        guard);
    tm_set_sync_state_callback(
        _handle,
        syncStateCallbackTrampoline,
        guard);
    tm_set_presence_callback(
        _handle,
        presenceCallbackTrampoline,
        guard);
    tm_set_typing_callback(
        _handle,
        typingCallbackTrampoline,
        guard);
    tm_set_verification_state_callback(
        _handle,
        verificationStateCallbackTrampoline,
        guard);
    tm_set_incoming_verification_request_callback(
        _handle,
        incomingVerificationRequestCallbackTrampoline,
        guard);
    tm_set_user_trust_changed_callback(
        _handle,
        userTrustChangedTrampoline,
        guard);
    tm_set_incoming_user_verification_request_callback(
        _handle,
        incomingUserVerificationRequestTrampoline,
        guard);
    tm_set_device_verified_callback(
        _handle,
        deviceVerifiedCallbackTrampoline,
        guard);
    tm_set_notification_callback(
        _handle,
        notificationCallbackTrampoline,
        guard);
    tm_set_invite_notification_callback(
        _handle,
        inviteNotificationCallbackTrampoline,
        guard);
    tm_set_member_sync_callback(
        _handle,
        memberSyncCallbackTrampoline,
        guard);
    tm_set_new_login_callback(
        _handle,
        newLoginCallbackTrampoline,
        guard);
    tm_set_preview_fetching_callback(
        _handle,
        previewFetchingCallbackTrampoline,
        guard);
    tm_set_upload_progress_callback(
        _handle,
        uploadProgressCallbackTrampoline,
        guard);
    tm_set_recent_emoji_callback(
        _handle,
        recentEmojiCallbackTrampoline,
        guard);
}

void ProtocolBridge::setRecentEmoji(const QVector<QPair<QString, int>> &pairs) {
    if (!_handle) {
        return;
    }
    QJsonArray arr;
    for (const auto &pair : pairs) {
        arr.append(QJsonArray{ pair.first, pair.second });
    }
    const auto json = QJsonDocument(arr).toJson(QJsonDocument::Compact);
    tm_set_recent_emoji(_handle, json.constData());
}

QVector<QPair<QString, int>> ProtocolBridge::recentEmojiForStartup() {
    if (!_handle) {
        return {};
    }
    char *raw = tm_get_recent_emoji(_handle);
    if (!raw) {
        return {};
    }
    const auto pairs = parseRecentEmojiPairs(QByteArray(raw));
    tm_free_string(raw);
    return pairs;
}

void ProtocolBridge::handleDeviceVerifiedChanged(bool verified) {
    _deviceVerified = verified;
    emit deviceVerifiedChanged(verified);
}

void ProtocolBridge::login(const QString &homeserver, const QString &user, const QString &pass) {
    // Starting a new session: re-enable media fetches in case this bridge was
    // gated by an earlier logout() — startup leftover-data cleanup
    // (startUnauthorisedCleanup) calls logout() on this same bridge before the
    // user logs in, which would otherwise leave all media stuck loading.
    _loggingOut = false;
    if (!_handle) {
        qWarning() << "[login] no handle, emitting failure";
        emit loginResult(false, QString(), QString(), QString());
        return;
    }
    const QByteArray hs = homeserver.toUtf8();
    const QByteArray u = user.toUtf8();
    const QByteArray p = pass.toUtf8();
    tm_login(
        _handle,
        hs.constData(),
        u.constData(),
        p.constData(),
        loginCallbackTrampoline,
        static_cast<void *>(_callbackGuard.get()));
}

void ProtocolBridge::registerAccount(
    const QString &homeserver,
    const QString &username,
    const QString &password,
    const QString &session,
    const QString &authJson) {
    if (!_handle) {
        emit registrationFailed(QStringLiteral("No protocol handle"));
        return;
    }
    const QByteArray hs = homeserver.toUtf8();
    const QByteArray u = username.toUtf8();
    const QByteArray p = password.toUtf8();
    const QByteArray sess = session.toUtf8();
    const QByteArray auth = authJson.toUtf8();
    tm_register(
        _handle,
        hs.constData(),
        u.constData(),
        p.constData(),
        session.isEmpty() ? nullptr : sess.constData(),
        authJson.isEmpty() ? nullptr : auth.constData(),
        registerCallbackTrampoline,
        static_cast<void *>(_callbackGuard.get()));
}

void ProtocolBridge::checkUsernameAvailable(const QString &homeserver, const QString &username) {
    if (!_handle) {
        emit usernameAvailabilityChecked(3, QStringLiteral("No protocol handle"));
        return;
    }
    const QByteArray hs = homeserver.toUtf8();
    const QByteArray u = username.toUtf8();
    tm_check_username_available(
        _handle,
        hs.constData(),
        u.constData(),
        usernameCheckCallbackTrampoline,
        static_cast<void *>(_callbackGuard.get()));
}

quint64 ProtocolBridge::nextRequestId() {
    return _nextUnreadRequestId++;
}

static QVector<TeleMatrix::RoomSummary> convertFfiRooms(FfiRoomList ffiList) {
    QVector<TeleMatrix::RoomSummary> result;
    if (!ffiList.rooms || ffiList.len == 0) {
        return result;
    }
    result.reserve(static_cast<int>(ffiList.len));
    for (size_t i = 0; i < ffiList.len; ++i) {
        const FfiRoomSummary &ffi = ffiList.rooms[i];
        TeleMatrix::RoomSummary room;
        room.roomId = ffi.room_id ? QString::fromUtf8(ffi.room_id) : QString();
        room.displayName = ffi.display_name ? QString::fromUtf8(ffi.display_name) : QString();
        room.canonicalAlias = ffi.canonical_alias ? QString::fromUtf8(ffi.canonical_alias) : QString();
        room.avatarUrl = ffi.avatar_url ? QString::fromUtf8(ffi.avatar_url) : QString();
        room.avatarEntityId = ffi.avatar_entity_id ? QString::fromUtf8(ffi.avatar_entity_id) : room.roomId;
        room.lastMessage = ffi.last_event_text ? QString::fromUtf8(ffi.last_event_text) : QString();
        room.lastSender = ffi.last_event_sender ? QString::fromUtf8(ffi.last_event_sender) : QString();
        // Translate service message bodies and media type labels from the Rust SDK.
        if (ffi.is_last_event_service) {
            room.lastMessage = translateServiceBody(room.lastMessage);
        }
        room.timestamp = static_cast<qint64>(ffi.last_event_timestamp);
        room.unreadCount = static_cast<int>(ffi.unread_count);
        room.isMarkedUnread = ffi.is_marked_unread;
        room.highlightCount = static_cast<int>(ffi.highlight_count);
        switch (ffi.notification_mode) {
        case 1:
            room.notificationMode = RoomNotificationMode::MentionsOnly;
            break;
        case 2:
            room.notificationMode = RoomNotificationMode::Mute;
            break;
        default:
            room.notificationMode = RoomNotificationMode::AllMessages;
            break;
        }
        room.isMuted = ffi.is_muted;
        room.isPinned = ffi.is_pinned;
        room.pinnedOrder = ffi.pinned_order;
        room.isDirect = ffi.is_direct;
        room.isPublic = ffi.is_public;
        if (ffi.filter_ids && ffi.filter_ids_len > 0) {
            room.filterIds.reserve(static_cast<int>(ffi.filter_ids_len));
            for (size_t j = 0; j < ffi.filter_ids_len; ++j) {
                room.filterIds.push_back(static_cast<int>(ffi.filter_ids[j]));
            }
        }
        if (ffi.space_ids && ffi.space_ids_len > 0) {
            room.spaceIds.reserve(static_cast<int>(ffi.space_ids_len));
            for (size_t j = 0; j < ffi.space_ids_len; ++j) {
                if (ffi.space_ids[j]) {
                    room.spaceIds.push_back(QString::fromUtf8(ffi.space_ids[j]));
                }
            }
        }
        room.isLastMessageOutgoing = ffi.is_last_event_outgoing;
        room.isLastMessageService = ffi.is_last_event_service;
        room.memberCount = ffi.member_count;
        room.canPinMessages = ffi.can_pin_messages;
        room.peerPresence = static_cast<int>(ffi.peer_presence);
        room.membership = static_cast<MembershipState>(ffi.membership);
        room.inviterUserId = ffi.inviter_user_id ? QString::fromUtf8(ffi.inviter_user_id) : QString();
        room.inviterDisplayName = ffi.inviter_display_name ? QString::fromUtf8(ffi.inviter_display_name) : QString();
        room.inviterAvatarUrl = ffi.inviter_avatar_url ? QString::fromUtf8(ffi.inviter_avatar_url) : QString();
        room.roomTopic = ffi.room_topic ? QString::fromUtf8(ffi.room_topic) : QString();
        switch (ffi.last_event_send_state) {
        case 0:
            room.lastMessageSendState = SendState::Sending;
            break;
        case 1:
            room.lastMessageSendState = SendState::Sent;
            break;
        case 3:
            room.lastMessageSendState = SendState::Failed;
            break;
        case 2:
        default:
            room.lastMessageSendState = SendState::Read;
            break;
        }
        result.append(room);
    }
    return result;
}

QVector<RoomSummary> ProtocolBridge::getRoomsBlockingForStartupOnly() {
    QVector<RoomSummary> result;
    if (!_handle) {
        setCachedRooms({});
        return result;
    }

    QElapsedTimer timer;
    timer.start();
    FfiRoomList ffiList = tm_get_rooms(_handle);

    // RAII guard: ensure tm_free_rooms is called even if we throw.
    auto guard = std::unique_ptr<FfiRoomList, void (*)(FfiRoomList *)>(
        &ffiList, [](FfiRoomList *list) { tm_free_rooms(*list); });

    result = convertFfiRooms(ffiList);
    if (timer.elapsed() > 100) {
        qWarning() << "[PERF] ProtocolBridge::getRoomsBlockingForStartupOnly blocked"
            << timer.elapsed() << "ms for" << result.size() << "rooms";
    }
    {
        auto withPreview = 0;
        for (const auto &room : result) {
            if (!room.lastMessage.isEmpty()) {
                ++withPreview;
            }
        }
        qDebug() << "[startup] getRoomsBlocking:" << result.size()
            << "rooms," << withPreview << "with last message";
    }
    if (result.isEmpty()) {
        setCachedRooms({});
        return result;
    }
    applySavedMessagesIdentity(result);
    setCachedRooms(result);
    return result;
}

void ProtocolBridge::getRoomsAsync(quint64 requestId) {
    if (!_handle) {
        emit roomsReady(requestId, false, QVector<RoomSummary>());
        return;
    }
    auto *data = new RoomListCallbackData{this, requestId};
    data->guard = _callbackGuard.get();
    tm_get_rooms_async(_handle, roomListCallbackTrampoline, static_cast<void *>(data));
}

void ProtocolBridge::getRoomUnreadSnapshotAsync(
    const QString &roomId,
    quint64 requestId) {
    if (!_handle || roomId.isEmpty()) {
        emit roomUnreadSnapshotReady(roomId, requestId, false, RoomUnreadSnapshot());
        return;
    }

    const QByteArray rid = roomId.toUtf8();
    auto *data = new RoomUnreadSnapshotCallbackData{this, roomId, requestId};
    data->guard = _callbackGuard.get();
    tm_get_timeline_slice_async(
        _handle,
        rid.constData(),
        roomUnreadSnapshotCallbackTrampoline,
        static_cast<void *>(data));
}

QVector<RoomSummary> ProtocolBridge::cachedRooms() const {
    return _cachedRooms;
}

void ProtocolBridge::setCachedRooms(const QVector<RoomSummary> &rooms) {
    _cachedRooms = rooms;
    _roomNotifById.clear();
    _roomNotifById.reserve(rooms.size());
    for (const auto &room : rooms) {
        _roomNotifById.insert(
            room.roomId,
            RoomNotifInfo{room.notificationMode, room.isMuted, room.isMarkedUnread});
    }
}

RoomNotifInfo ProtocolBridge::roomNotifInfo(const QString &roomId) const {
    return _roomNotifById.value(roomId);
}

void ProtocolBridge::getRoomMembersAsync(const QString &roomId) {
    if (!_handle) {
        emit roomMembersReady(roomId, {});
        return;
    }

    const QByteArray rid = roomId.toUtf8();
    auto *data = new UserListCallbackData{
        this,
        [this, roomId](bool /*success*/, const QVector<UserProfile> &results) {
            emit roomMembersReady(roomId, results);
        },
    };
    data->guard = _callbackGuard.get();
    tm_get_room_members_async(
        _handle,
        rid.constData(),
        userListCallbackTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::getRoomMembersSnapshotAsync(const QString &roomId, bool forceRefresh) {
    if (!_handle) {
        emit roomMembersSnapshotReady(roomId, false, {});
        return;
    }

    const QByteArray rid = roomId.toUtf8();
    auto *data = new RoomMembersSnapshotCallbackData{
        this,
        roomId,
        [this, roomId](bool success, const RoomMembersSnapshot &snapshot) {
            emit roomMembersSnapshotReady(roomId, success, snapshot);
        },
    };
    data->guard = _callbackGuard.get();
    tm_get_room_members_snapshot_async(
        _handle,
        rid.constData(),
        forceRefresh,
        roomMembersSnapshotCallbackTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::searchUserDirectory(const QString &query, int limit) {
    if (!_handle) {
        emit userDirectorySearchReady(query, false, {}, false);
        return;
    }

    const auto trimmed = query.trimmed();
    if (trimmed.isEmpty()) {
        emit userDirectorySearchReady(trimmed, true, {}, false);
        return;
    }

    const QByteArray q = trimmed.toUtf8();
    auto *data = new UserDirectoryCallbackData{
        this,
        [this, trimmed](bool success, const QVector<UserProfile> &results, bool limited) {
            emit userDirectorySearchReady(trimmed, success, results, limited);
        },
    };
    data->guard = _callbackGuard.get();
    tm_search_user_directory(
        _handle,
        q.constData(),
        static_cast<uint64_t>(std::max(limit, 1)),
        userDirectorySearchCallbackTrampoline,
        static_cast<void *>(data));
}

/// Convert an FfiTimeline to a QVector<TimelineItem>. Shared by
/// timeline slice and pinned-message async callbacks.
static QVector<TimelineItem> convertFfiTimeline(const FfiTimeline &ffiTl) {
    QVector<TimelineItem> result;
    if (!ffiTl.items || ffiTl.len == 0) return result;
    result.reserve(static_cast<int>(ffiTl.len));
    for (size_t i = 0; i < ffiTl.len; ++i) {
        const FfiTimelineItem &ffi = ffiTl.items[i];
        TimelineItem item;
        item.eventId = ffiString(ffi.event_id);
        item.transactionId = ffiString(ffi.transaction_id);
        item.sender = convertFfiSenderInfo(ffi);
        item.content = convertFfiContent(ffi);
        item.reply = convertFfiReplyInfo(ffi);
        item.forwardedFrom = convertFfiForwardInfo(ffi);
        item.isEdited = ffi.is_edited;
        item.isPinned = ffi.is_pinned;
        item.timestamp = static_cast<qint64>(ffi.timestamp);
        item.delivery = convertFfiDeliveryInfo(ffi);
        item.reactions = convertFfiReactions(ffi);
        item.urlPreview = convertFfiUrlPreview(ffi);
        item.encryption = convertFfiEncryptionInfo(ffi);

        result.append(item);
    }
    return result;
}

static TeleMatrix::TimelineSlice convertFfiTimelineSlice(FfiTimelineSlice ffiSlice) {
    TeleMatrix::TimelineSlice result;
    result.updateKind = static_cast<TeleMatrix::TimelineUpdateKind>(ffiSlice.update_kind);
    result.updateIndex = static_cast<int>(ffiSlice.update_index);
    result.canPaginateBack = ffiSlice.can_paginate_back;
    result.canPaginateForward = ffiSlice.can_paginate_forward;
    result.hitTimelineStart = ffiSlice.hit_timeline_start;
    result.isLive = ffiSlice.is_live;
    if (ffiSlice.focus_event_id) {
        result.focusEventId = QString::fromUtf8(ffiSlice.focus_event_id);
    }
    if (ffiSlice.pinned_event_ids && ffiSlice.pinned_event_ids_count > 0) {
        for (uint32_t i = 0; i < ffiSlice.pinned_event_ids_count; ++i) {
            if (ffiSlice.pinned_event_ids[i]) {
                result.pinnedEventIds.append(
                    QString::fromUtf8(ffiSlice.pinned_event_ids[i]));
            }
        }
    }
    if (ffiSlice.first_unread_event_id) {
        result.firstUnreadEventId = QString::fromUtf8(ffiSlice.first_unread_event_id);
    }
    result.readMarkerLoaded = ffiSlice.read_marker_loaded;
    result.unreadCount = static_cast<int>(ffiSlice.unread_count);
    result.unreadStateKnown = ffiSlice.unread_state_known;

    if (!ffiSlice.items || ffiSlice.items_count == 0) {
        return result;
    }

    // Use shared conversion — wrap FfiTimelineSlice as FfiTimeline temporarily.
    FfiTimeline tmpTl;
    tmpTl.items = ffiSlice.items;
    tmpTl.len = ffiSlice.items_count;
    result.items = convertFfiTimeline(tmpTl);
    // Prevent double-free — items are owned by ffiSlice, not tmpTl.
    tmpTl.items = nullptr;
    tmpTl.len = 0;

    return result;
}

void ProtocolBridge::getTimelineSliceAsync(const QString &roomId, quint64 requestId) {
    if (!_handle || roomId.isEmpty()) {
        emit timelineSliceReady(roomId, requestId, false, TimelineSlice());
        return;
    }
    const QByteArray rid = roomId.toUtf8();
    auto *data = new TimelineSliceCallbackData{this, roomId, requestId};
    data->guard = _callbackGuard.get();
    tm_get_timeline_slice_async(
        _handle,
        rid.constData(),
        timelineSliceCallbackTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::getTimelineUpdateAsync(const QString &roomId, quint64 requestId) {
    if (!_handle || roomId.isEmpty()) {
        emit timelineSliceReady(roomId, requestId, false, TimelineSlice());
        return;
    }
    const QByteArray rid = roomId.toUtf8();
    auto *data = new TimelineSliceCallbackData{this, roomId, requestId};
    data->guard = _callbackGuard.get();
    tm_get_timeline_update_async(
        _handle,
        rid.constData(),
        timelineSliceCallbackTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::paginateBack(const QString &roomId, quint16 count) {
    if (!_handle) return;
    tm_paginate_back(_handle, roomId.toUtf8().constData(), count);
}

void ProtocolBridge::paginateForward(const QString &roomId, quint16 count) {
    if (!_handle) return;
    tm_paginate_forward(_handle, roomId.toUtf8().constData(), count);
}

void ProtocolBridge::focusOnEvent(
    const QString &roomId,
    const QString &eventId,
    quint64 requestId) {
    if (!_handle) return;
    auto *data = new FocusCallbackData{this, roomId, requestId};
    data->guard = _callbackGuard.get();
    tm_focus_on_event(_handle,
                      roomId.toUtf8().constData(),
                      eventId.toUtf8().constData(),
                      focusOnEventCallbackTrampoline,
                      data);
}

void ProtocolBridge::returnToLive(const QString &roomId) {
    if (!_handle) return;
    tm_return_to_live(_handle, roomId.toUtf8().constData());
}

void ProtocolBridge::releaseRoomTimeline(const QString &roomId) {
    if (!_handle) return;
    tm_release_room_timeline(_handle, roomId.toUtf8().constData());
}

void ProtocolBridge::cancelUpload(const QString &roomId, const QString &transactionId) {
    if (!_handle) return;
    tm_cancel_upload(_handle, roomId.toUtf8().constData(), transactionId.toUtf8().constData());
}

struct PinnedCallbackData {
    ProtocolBridge *bridge;
    BridgeCallbackGuard *guard = nullptr;
};

extern "C" void pinnedMessagesCallbackTrampoline(FfiTimeline timeline, void *userdata) {
    auto *data = static_cast<PinnedCallbackData*>(userdata);
    auto *guard = data->guard;
    delete data;
    auto items = convertFfiTimeline(timeline);
    tm_free_timeline(timeline);
    for (auto &item : items) {
        item.isPinned = true;
    }
    withGuardedBridge(guard, [items = std::move(items)](ProtocolBridge *bridge) mutable {
        QMetaObject::invokeMethod(bridge, [bridge, items = std::move(items)]() {
            emit bridge->pinnedMessagesFetched(items);
        }, Qt::QueuedConnection);
    });
}

void ProtocolBridge::getPinnedMessagesAsync(const QString &roomId) {
    if (!_handle) return;
    auto *data = new PinnedCallbackData{this};
    data->guard = _callbackGuard.get();
    tm_get_pinned_messages_async(_handle, roomId.toUtf8().constData(),
                                  pinnedMessagesCallbackTrampoline, data);
}

void ProtocolBridge::unpinAllMessages(const QString &roomId) {
    if (!_handle) {
        emit messagePinned(false);
        return;
    }
    auto *data = new SimpleCallbackData{
        this,
        [this](bool success) { emit messagePinned(success); },
    };
    data->guard = _callbackGuard.get();
    tm_unpin_all_messages(_handle, roomId.toUtf8().constData(),
                          simpleCallbackTrampoline, data);
}

void ProtocolBridge::searchMessagesAsync(const SearchRequest &request) {
    if (!_handle) {
        emit searchFailed(request.requestId, QStringLiteral("No handle"));
        return;
    }
    if (request.query.trimmed().isEmpty()) {
        SearchPage empty;
        empty.requestId = request.requestId;
        empty.done = true;
        emit searchPageReady(empty);
        return;
    }

    _activeSearchRequests.insert(request.requestId);

    const QByteArray roomId = request.roomId.toUtf8();
    const QByteArray query = request.query.toUtf8();
    const QByteArray nextToken = request.nextToken.toUtf8();
    const QByteArray senderFilter = request.senderFilter.toUtf8();

    auto *data = new SearchCallbackData{this, request.requestId};
    data->guard = _callbackGuard.get();
    tm_search_messages(
        _handle,
        request.requestId,
        static_cast<uint32_t>(request.scope),
        request.roomId.isEmpty() ? nullptr : roomId.constData(),
        query.constData(),
        static_cast<uint32_t>(request.limit),
        request.nextToken.isEmpty() ? nullptr : nextToken.constData(),
        request.senderFilter.isEmpty() ? nullptr : senderFilter.constData(),
        0,  // date_from (unused)
        0,  // date_to (unused)
        searchCallbackTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::cancelSearch(quint64 requestId) {
    _activeSearchRequests.remove(requestId);
    if (_handle) {
        tm_cancel_search(_handle, requestId);
    }
}

void ProtocolBridge::handleSearchPageReady(quint64 requestId, const SearchPage &page) {
    if (!_activeSearchRequests.contains(requestId)) {
        return; // stale response
    }
    _activeSearchRequests.remove(requestId);
    emit searchPageReady(page);
}

void ProtocolBridge::handleSearchFailed(quint64 requestId, const QString &error) {
    _activeSearchRequests.remove(requestId);
    emit searchFailed(requestId, error);
}

void ProtocolBridge::searchPublicRoomsAsync(
    quint64 requestId,
    const QString &query,
    int limit,
    const QString &nextToken)
{
    if (!_handle) {
        emit roomDirectoryFailed(requestId, tr("Not connected."));
        return;
    }

    _activeRoomDirectoryRequests.insert(requestId);

    // An empty query is meaningful here (unlike message search): it browses the whole directory.
    const QByteArray queryUtf8 = query.toUtf8();
    const QByteArray tokenUtf8 = nextToken.toUtf8();

    auto *data = new RoomDirectoryCallbackData{_callbackGuard.get(), requestId, false, QString()};
    tm_search_public_rooms(
        _handle,
        requestId,
        queryUtf8.constData(),
        static_cast<uint32_t>(limit),
        nextToken.isEmpty() ? nullptr : tokenUtf8.constData(),
        roomDirectoryCallbackTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::getSpaceChildrenAsync(
    quint64 requestId,
    const QString &spaceId,
    int limit,
    const QString &nextToken)
{
    if (!_handle) {
        emit roomDirectoryFailed(requestId, tr("Not connected."));
        return;
    }

    _activeRoomDirectoryRequests.insert(requestId);

    const QByteArray spaceIdUtf8 = spaceId.toUtf8();
    const QByteArray tokenUtf8 = nextToken.toUtf8();

    auto *data = new RoomDirectoryCallbackData{_callbackGuard.get(), requestId, true, spaceId};
    tm_get_space_children(
        _handle,
        requestId,
        spaceIdUtf8.constData(),
        static_cast<uint32_t>(limit),
        nextToken.isEmpty() ? nullptr : tokenUtf8.constData(),
        roomDirectoryCallbackTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::cancelRoomDirectoryRequest(quint64 requestId) {
    _activeRoomDirectoryRequests.remove(requestId);
    if (_handle) {
        tm_cancel_room_directory_request(_handle, requestId);
    }
}

void ProtocolBridge::handleRoomDirectoryPageReady(
    quint64 requestId,
    const RoomDirectoryPage &page)
{
    if (!_activeRoomDirectoryRequests.contains(requestId)) {
        return; // stale response
    }
    _activeRoomDirectoryRequests.remove(requestId);
    emit roomDirectoryPageReady(page);
}

void ProtocolBridge::handleRoomDirectoryFailed(quint64 requestId, const QString &error) {
    if (!_activeRoomDirectoryRequests.contains(requestId)) {
        return; // cancelled, or already answered
    }
    _activeRoomDirectoryRequests.remove(requestId);
    emit roomDirectoryFailed(requestId, error);
}

namespace {

/// Rust copies the `via` strings before it spawns, so the backing QByteArrays only need to outlive
/// the FFI call itself.
struct ViaServers {
    QVector<QByteArray> storage;
    QVector<const char *> pointers;

    explicit ViaServers(const QStringList &via) {
        storage.reserve(via.size());
        pointers.reserve(via.size());
        for (const auto &server : via) {
            storage.append(server.toUtf8());
            pointers.append(storage.last().constData());
        }
    }

    [[nodiscard]] const char *const *data() const {
        return pointers.isEmpty() ? nullptr : pointers.constData();
    }
    [[nodiscard]] size_t size() const { return static_cast<size_t>(pointers.size()); }
};

} // namespace

void ProtocolBridge::getRoomPreview(const QString &roomIdOrAlias, const QStringList &via) {
    if (!_handle) {
        emit roomPreviewReady(roomIdOrAlias, false, RoomPreviewInfo(), tr("Not connected."));
        return;
    }

    const QByteArray target = roomIdOrAlias.toUtf8();
    const ViaServers servers(via);

    auto *data = new RoomTargetCallbackData{_callbackGuard.get(), roomIdOrAlias};
    tm_get_room_preview(
        _handle,
        target.constData(),
        servers.data(),
        servers.size(),
        roomPreviewCallbackTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::previewMessages(const QString &roomId, const QString &from, int limit) {
    if (!_handle) {
        emit roomPreviewMessagesReady(roomId, false, {}, QString(), tr("Not connected."));
        return;
    }

    const QByteArray target = roomId.toUtf8();
    const QByteArray fromToken = from.toUtf8();
    auto *data = new RoomTargetCallbackData{_callbackGuard.get(), roomId};
    tm_preview_messages(
        _handle,
        target.constData(),
        from.isEmpty() ? nullptr : fromToken.constData(),
        static_cast<uint32_t>(qMax(0, limit)),
        previewMessagesCallbackTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::joinRoom(const QString &roomIdOrAlias, const QStringList &via) {
    if (!_handle) {
        emit roomJoined(roomIdOrAlias, false, QString(), tr("Not connected."));
        return;
    }

    const QByteArray target = roomIdOrAlias.toUtf8();
    const ViaServers servers(via);

    auto *data = new RoomTargetCallbackData{_callbackGuard.get(), roomIdOrAlias};
    tm_join_room(
        _handle,
        target.constData(),
        servers.data(),
        servers.size(),
        joinRoomCallbackTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::knockRoom(const QString &roomIdOrAlias, const QStringList &via) {
    if (!_handle) {
        emit roomKnocked(roomIdOrAlias, false, QString(), tr("Not connected."));
        return;
    }

    const QByteArray target = roomIdOrAlias.toUtf8();
    const ViaServers servers(via);

    auto *data = new RoomTargetCallbackData{_callbackGuard.get(), roomIdOrAlias};
    tm_knock_room(
        _handle,
        target.constData(),
        servers.data(),
        servers.size(),
        knockRoomCallbackTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::watchTimeline(const QString &roomId) {
    if (!_handle) return;

    const std::string key = roomId.toStdString();

    // Reuse existing data if we already watch this room,
    // otherwise allocate new data that outlives the registration.
    auto &data = _timelineCallbackDatas[key];
    if (!data) {
        data = std::make_unique<TimelineCallbackData>();
        data->bridge = this;
        data->guard = _callbackGuard.get();
    }
    data->roomIdUtf8 = roomId.toUtf8();

    tm_set_timeline_change_callback(
        _handle,
        data->roomIdUtf8.constData(),
        timelineChangeCallbackTrampoline,
        static_cast<void *>(data.get()));
}

void ProtocolBridge::sendMessage(
    const QString &roomId,
    const QString &body,
    const QString &formattedBody,
    const QString &replyToEventId,
    quint64 requestId) {
    if (!_handle) {
        emit messageSent(requestId, false, QString());
        return;
    }
    const QByteArray rid = roomId.toUtf8();
    const QByteArray b = body.toUtf8();
    const QByteArray fb = formattedBody.toUtf8();
    const QByteArray reply = replyToEventId.toUtf8();
    auto *data = new SendCallbackData{
        this,
        [this, requestId](bool success, const QString &eventId) {
            emit messageSent(requestId, success, eventId);
        },
    };
    data->guard = _callbackGuard.get();
    tm_send_message(
        _handle,
        rid.constData(),
        b.constData(),
        formattedBody.isEmpty() ? nullptr : fb.constData(),
        replyToEventId.isEmpty() ? nullptr : reply.constData(),
        sendCallbackWithDataTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::editMessage(
    const QString &roomId,
    const QString &eventId,
    const QString &body,
    const QString &formattedBody,
    bool asMediaCaption) {
    if (!_handle) {
        emit messageEdited(false, QString());
        return;
    }

    const QByteArray rid = roomId.toUtf8();
    const QByteArray eid = eventId.toUtf8();
    const QByteArray b = body.toUtf8();
    const QByteArray fb = formattedBody.toUtf8();

    auto *data = new SendCallbackData{
        this,
        [this](bool success, const QString &newEventId) {
            emit messageEdited(success, newEventId);
        },
    };
    data->guard = _callbackGuard.get();
    tm_edit_message(
        _handle,
        rid.constData(),
        eid.constData(),
        b.constData(),
        formattedBody.isEmpty() ? nullptr : fb.constData(),
        asMediaCaption,
        sendCallbackWithDataTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::deleteMessage(const QString &roomId, const QString &eventId) {
    if (!_handle) {
        emit messageDeleted(false);
        return;
    }

    const QByteArray rid = roomId.toUtf8();
    const QByteArray eid = eventId.toUtf8();
    auto *data = new SimpleCallbackData{
        this,
        [this](bool success) { emit messageDeleted(success); },
    };
    data->guard = _callbackGuard.get();
    tm_delete_message(
        _handle,
        rid.constData(),
        eid.constData(),
        simpleCallbackTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::pinMessage(const QString &roomId, const QString &eventId, bool pinned) {
    if (!_handle) {
        emit messagePinned(false);
        return;
    }

    const QByteArray rid = roomId.toUtf8();
    const QByteArray eid = eventId.toUtf8();
    auto *data = new SimpleCallbackData{
        this,
        [this](bool success) { emit messagePinned(success); },
    };
    data->guard = _callbackGuard.get();
    tm_pin_message(
        _handle,
        rid.constData(),
        eid.constData(),
        pinned,
        simpleCallbackTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::setAudioDuration(const QString &mxcUrl, quint64 durationMs) {
    if (!_handle || mxcUrl.isEmpty() || durationMs == 0) {
        return;
    }
    tm_set_audio_duration(_handle, mxcUrl.toUtf8().constData(), durationMs);
}

quint64 ProtocolBridge::maxUploadSize() const {
    return _handle ? tm_max_upload_size(_handle) : 0;
}

void ProtocolBridge::pinRoom(const QString &roomId, bool pinned, double order) {
    if (!_handle) {
        emit roomPinned(false);
        return;
    }

    const QByteArray rid = roomId.toUtf8();
    auto *data = new SimpleCallbackData{
        this,
        [this](bool success) { emit roomPinned(success); },
    };
    data->guard = _callbackGuard.get();
    tm_pin_room(
        _handle,
        rid.constData(),
        pinned,
        order,
        simpleCallbackTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::setPinnedOrder(const QVector<QString> &roomIds) {
    if (!_handle) {
        emit pinnedOrderStored(false);
        return;
    }

    // The C strings must outlive the call, so keep the QByteArrays alive alongside
    // the pointer array handed to Rust.
    QVector<QByteArray> storage;
    storage.reserve(roomIds.size());
    QVector<const char *> pointers;
    pointers.reserve(roomIds.size());
    for (const auto &roomId : roomIds) {
        storage.append(roomId.toUtf8());
        pointers.append(storage.last().constData());
    }

    auto *data = new SimpleCallbackData{
        this,
        [this](bool success) { emit pinnedOrderStored(success); },
    };
    data->guard = _callbackGuard.get();
    tm_set_pinned_order(
        _handle,
        pointers.isEmpty() ? nullptr : pointers.constData(),
        size_t(pointers.size()),
        simpleCallbackTrampoline,
        static_cast<void *>(data));
}



void ProtocolBridge::setRoomNotificationMode(const QString &roomId, RoomNotificationMode mode) {
    if (!_handle) {
        emit roomNotificationModeSet(false);
        emit roomNotificationModeSetForRoom(roomId, mode, false);
        return;
    }

    emit roomNotificationModeChangeRequested(roomId, mode);

    const QByteArray rid = roomId.toUtf8();
    auto *data = new SimpleCallbackData{
        this,
        [this, roomId, mode](bool success) {
            emit roomNotificationModeSet(success);
            emit roomNotificationModeSetForRoom(roomId, mode, success);
        },
    };
    data->guard = _callbackGuard.get();
    tm_set_room_notification_mode(
        _handle,
        rid.constData(),
        static_cast<uint32_t>(static_cast<int>(mode)),
        simpleCallbackTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::getNotificationSettings() {
    if (!_handle) {
        emit notificationSettingsReady(
            false,
            RoomNotificationMode::AllMessages,
            RoomNotificationMode::AllMessages,
            false, false, false, false,
            QString());
        return;
    }
    tm_get_notification_settings(
        _handle,
        notificationSettingsCallbackTrampoline,
        static_cast<void *>(_callbackGuard.get()));
}

void ProtocolBridge::setCategoryNotificationLevel(
        NotificationCategory category,
        RoomNotificationMode level) {
    if (!_handle) {
        emit notificationSettingsSaved(false);
        return;
    }
    auto *data = new SimpleCallbackData{
        this,
        [this](bool success) { emit notificationSettingsSaved(success); },
    };
    data->guard = _callbackGuard.get();
    tm_set_category_notification_level(
        _handle,
        static_cast<uint32_t>(static_cast<int>(category)),
        static_cast<uint32_t>(static_cast<int>(level)),
        simpleCallbackTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::setKeywords(const QString &keywordsCsv) {
    if (!_handle) {
        emit notificationSettingsSaved(false);
        return;
    }
    const QByteArray csv = keywordsCsv.toUtf8();
    auto *data = new SimpleCallbackData{
        this,
        [this](bool success) { emit notificationSettingsSaved(success); },
    };
    data->guard = _callbackGuard.get();
    tm_set_keywords(
        _handle,
        csv.constData(),
        simpleCallbackTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::setNotificationToggle(NotificationToggle toggle, bool enabled) {
    if (!_handle) {
        emit notificationSettingsSaved(false);
        return;
    }
    auto *data = new SimpleCallbackData{
        this,
        [this](bool success) { emit notificationSettingsSaved(success); },
    };
    data->guard = _callbackGuard.get();
    tm_set_notification_toggle(
        _handle,
        static_cast<uint32_t>(static_cast<int>(toggle)),
        enabled,
        simpleCallbackTrampoline,
        static_cast<void *>(data));
}

quint64 ProtocolBridge::markRoomRead(const QString &roomId, bool read) {
    const auto requestId = _nextUnreadRequestId++;
    if (!_handle) {
        emit roomMarkedRead(requestId, roomId, read, false);
        return requestId;
    }

    const QByteArray rid = roomId.toUtf8();
    auto *data = new SimpleCallbackData{
        this,
        [this, requestId, roomId, read](bool success) {
            emit roomMarkedRead(requestId, roomId, read, success);
        },
    };
    data->guard = _callbackGuard.get();
    tm_mark_room_read(
        _handle,
        rid.constData(),
        read,
        simpleCallbackTrampoline,
        static_cast<void *>(data));
    return requestId;
}

quint64 ProtocolBridge::sendReadReceipt(const QString &roomId, const QString &eventId) {
    const auto requestId = _nextUnreadRequestId++;
    if (!_handle) {
        emit readReceiptSent(requestId, roomId, eventId, false);
        return requestId;
    }

    const QByteArray rid = roomId.toUtf8();
    const QByteArray eid = eventId.toUtf8();
    auto *data = new SimpleCallbackData{
        this,
        [this, requestId, roomId, eventId](bool success) {
            emit readReceiptSent(requestId, roomId, eventId, success);
        },
    };
    data->guard = _callbackGuard.get();
    tm_send_read_receipt(
        _handle,
        rid.constData(),
        eid.constData(),
        simpleCallbackTrampoline,
        static_cast<void *>(data));
    return requestId;
}

void ProtocolBridge::sendTypingNotice(const QString &roomId, bool typing) {
    if (!_handle) return;
    const QByteArray rid = roomId.toUtf8();
    tm_send_typing(_handle, rid.constData(), typing);
}

void ProtocolBridge::leaveRoom(const QString &roomId) {
    if (!_handle) {
        emit roomLeft(false);
        return;
    }

    const QByteArray rid = roomId.toUtf8();
    auto *data = new SimpleCallbackData{
        this,
        [this](bool success) { emit roomLeft(success); },
    };
    data->guard = _callbackGuard.get();
    tm_leave_room(
        _handle,
        rid.constData(),
        simpleCallbackTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::acceptInvite(const QString &roomId) {
    if (!_handle) {
        emit inviteAccepted(false, roomId);
        return;
    }

    const QByteArray rid = roomId.toUtf8();
    const auto roomIdCopy = roomId;
    auto *data = new SimpleCallbackData{
        this,
        [this, roomIdCopy](bool success) { emit inviteAccepted(success, roomIdCopy); },
    };
    data->guard = _callbackGuard.get();
    tm_accept_invite(
        _handle,
        rid.constData(),
        simpleCallbackTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::createRoom(const CreateRoomRequest &request) {
    if (!_handle || request.name.trimmed().isEmpty()) {
        emit roomCreated(false, QString());
        return;
    }

    const QByteArray name = request.name.toUtf8();
    const QByteArray topic = request.topic.toUtf8();
    const QByteArray alias = request.alias.toUtf8();
    const QByteArray avatarPath = request.avatarPath.toUtf8();
    auto *data = new SendCallbackData{
        this,
        [this](bool success, const QString &roomId) {
            emit roomCreated(success, roomId);
        },
    };
    data->guard = _callbackGuard.get();
    tm_create_room(
        _handle,
        name.constData(),
        request.topic.isEmpty() ? nullptr : topic.constData(),
        request.isPublic,
        request.encrypted,
        request.alias.isEmpty() ? nullptr : alias.constData(),
        request.avatarPath.isEmpty() ? nullptr : avatarPath.constData(),
        static_cast<int32_t>(request.guestAccess),
        static_cast<int32_t>(request.historyVisibility),
        request.federate,
        createRoomCallbackTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::addRoomToFolder(const QString &roomId, const QString &sectionKey) {
    if (!_handle || roomId.isEmpty() || sectionKey.isEmpty()) {
        emit roomFolderChanged(false, roomId, sectionKey, tr("Not connected"));
        return;
    }

    const QByteArray rid = roomId.toUtf8();
    const QByteArray key = sectionKey.toUtf8();
    auto *data = new ResultCallbackData{
        this,
        [this, roomId, sectionKey](bool success, const QString &error) {
            emit roomFolderChanged(success, roomId, sectionKey, error);
        },
    };
    data->guard = _callbackGuard.get();
    tm_add_room_to_folder(
        _handle,
        rid.constData(),
        key.constData(),
        resultCallbackTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::createFolder(const QString &name) {
    if (!_handle || name.trimmed().isEmpty()) {
        emit folderCreated(false, -1, QString(), tr("Not connected"));
        return;
    }

    const QByteArray n = name.toUtf8();
    auto *data = new CreateFolderCallbackData{
        this,
        [this](bool success, int folderId, const QString &sectionKey, const QString &error) {
            emit folderCreated(success, folderId, sectionKey, error);
        },
    };
    data->guard = _callbackGuard.get();
    tm_create_folder(
        _handle,
        n.constData(),
        createFolderCallbackTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::editFolder(const QString &sectionKey, const QString &name) {
    if (!_handle || sectionKey.isEmpty() || name.trimmed().isEmpty()) {
        emit folderEdited(false, tr("Not connected"));
        return;
    }

    const QByteArray key = sectionKey.toUtf8();
    const QByteArray n = name.toUtf8();
    auto *data = new ResultCallbackData{
        this,
        [this](bool success, const QString &error) { emit folderEdited(success, error); },
    };
    data->guard = _callbackGuard.get();
    tm_edit_folder(
        _handle,
        key.constData(),
        n.constData(),
        resultCallbackTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::deleteFolder(const QString &sectionKey) {
    if (!_handle || sectionKey.isEmpty()) {
        emit folderDeleted(false, tr("Not connected"));
        return;
    }

    const QByteArray key = sectionKey.toUtf8();
    auto *data = new ResultCallbackData{
        this,
        [this](bool success, const QString &error) { emit folderDeleted(success, error); },
    };
    data->guard = _callbackGuard.get();
    tm_delete_folder(
        _handle,
        key.constData(),
        resultCallbackTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::setSidebarOrder(const QVector<SidebarEntry> &order) {
    if (!_handle) {
        emit sidebarOrderSaved(false);
        return;
    }

    // Keep the UTF-8 buffers alive across the (synchronous) FFI call.
    QVector<QByteArray> keyBuffers;
    keyBuffers.reserve(order.size());
    QVector<FfiSidebarRef> refs;
    refs.reserve(order.size());
    for (const auto &entry : order) {
        keyBuffers.append(entry.key.toUtf8());
        FfiSidebarRef ref;
        ref.kind = entry.isSpace ? 1u : 0u;
        ref.key = const_cast<char *>(keyBuffers.last().constData());
        refs.append(ref);
    }

    auto *data = new SimpleCallbackData{
        this,
        [this](bool success) { emit sidebarOrderSaved(success); },
    };
    data->guard = _callbackGuard.get();
    tm_set_sidebar_order(
        _handle,
        refs.constData(),
        static_cast<size_t>(refs.size()),
        simpleCallbackTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::getSidebarOrderAsync() {
    if (!_handle) {
        emit sidebarOrderReady(false, {});
        return;
    }
    auto *data = new SidebarOrderCallbackData{
        this,
        [this](bool success, const QVector<SidebarEntry> &order) {
            emit sidebarOrderReady(success, order);
        },
    };
    data->guard = _callbackGuard.get();
    tm_get_sidebar_order_async(
        _handle,
        sidebarOrderCallbackTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::getJoinedSpacesAsync() {
    if (!_handle) {
        emit joinedSpacesReady(false, {});
        return;
    }
    auto *data = new SpaceListCallbackData{
        this,
        [this](bool success, const QVector<SpaceInfo> &spaces) {
            if (success) {
                _cachedJoinedSpaces = spaces;
            }
            emit joinedSpacesReady(success, spaces);
        },
    };
    data->guard = _callbackGuard.get();
    tm_get_joined_spaces_async(
        _handle,
        spaceListCallbackTrampoline,
        static_cast<void *>(data));
}

QVector<SpaceInfo> ProtocolBridge::cachedJoinedSpaces() const {
    return _cachedJoinedSpaces;
}

QVector<FolderInfo> ProtocolBridge::getCustomFoldersBlockingForStartupOnly() {
    QVector<FolderInfo> result;
    if (!_handle) {
        _cachedCustomFolders.clear();
        return result;
    }

    FfiFolderList ffiList = tm_get_folders(_handle);
    auto guard = std::unique_ptr<FfiFolderList, void (*)(FfiFolderList *)>(
        &ffiList, [](FfiFolderList *list) { tm_free_folders(*list); });

    result = folderInfosFromFfiFolderList(ffiList);
    _cachedCustomFolders = result;
    return result;
}

void ProtocolBridge::getCustomFoldersAsync() {
    if (!_handle) {
        emit customFoldersReady(false, {});
        return;
    }

    auto *data = new FolderListCallbackData{
        this,
        [this](bool success, const QVector<FolderInfo> &folders) {
            if (success) {
                _cachedCustomFolders = folders;
            }
            emit customFoldersReady(success, folders);
        },
    };
    data->guard = _callbackGuard.get();
    tm_get_folders_async(_handle, folderListCallbackTrampoline, static_cast<void *>(data));
}

QVector<FolderInfo> ProtocolBridge::cachedCustomFolders() const {
    return _cachedCustomFolders;
}

void ProtocolBridge::forwardMessage(const QString &srcRoomId, const QString &eventId, const QString &dstRoomId) {
    if (!_handle) {
        emit messageForwarded(false, QString());
        return;
    }

    const QByteArray src = srcRoomId.toUtf8();
    const QByteArray eid = eventId.toUtf8();
    const QByteArray dst = dstRoomId.toUtf8();
    auto *data = new SendCallbackData{
        this,
        [this](bool success, const QString &newEventId) {
            emit messageForwarded(success, newEventId);
        },
    };
    data->guard = _callbackGuard.get();
    tm_forward_message(
        _handle,
        src.constData(),
        eid.constData(),
        dst.constData(),
        sendCallbackWithDataTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::sendMedia(
    const QString &roomId,
    ContentType type,
    const QString &url,
    const QString &mime,
    const QString &filename,
    const QString &caption,
    const QString &thumbUrl,
    quint64 size,
    int width,
    int height,
    quint64 durationMs,
    const QString &transactionId,
    bool isVoice,
    const QByteArray &waveform) {
    if (!_handle) {
        emit mediaSent(false, transactionId);
        return;
    }

    const QByteArray rid = roomId.toUtf8();
    const QByteArray u = url.toUtf8();
    const QByteArray m = mime.toUtf8();
    const QByteArray f = filename.toUtf8();
    const QByteArray c = caption.toUtf8();
    const QByteArray t = thumbUrl.toUtf8();
    const QByteArray tx = transactionId.toUtf8();
    const auto *waveformData = waveform.isEmpty()
        ? nullptr
        : reinterpret_cast<const uint8_t*>(waveform.constData());
    auto *data = new SendCallbackData{
        this,
        [this](bool success, const QString &eventId) {
            emit mediaSent(success, eventId);
        },
    };
    data->guard = _callbackGuard.get();

    tm_send_media(
        _handle,
        rid.constData(),
        static_cast<uint32_t>(type),
        u.constData(),
        m.constData(),
        f.constData(),
        caption.isEmpty() ? nullptr : c.constData(),
        thumbUrl.isEmpty() ? nullptr : t.constData(),
        static_cast<uint64_t>(size),
        static_cast<uint32_t>(qMax(0, width)),
        static_cast<uint32_t>(qMax(0, height)),
        static_cast<uint64_t>(durationMs),
        transactionId.isEmpty() ? nullptr : tx.constData(),
        isVoice,
        waveformData,
        static_cast<uintptr_t>(waveform.size()),
        sendCallbackWithDataTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::setReaction(
    const QString &roomId,
    const QString &eventId,
    const QString &key,
    bool active) {
    if (!_handle) {
        emit reactionSet(false);
        return;
    }

    const QByteArray rid = roomId.toUtf8();
    const QByteArray eid = eventId.toUtf8();
    const QByteArray k = key.toUtf8();
    auto *data = new SimpleCallbackData{
        this,
        [this](bool success) { emit reactionSet(success); },
    };
    data->guard = _callbackGuard.get();
    tm_set_reaction(
        _handle,
        rid.constData(),
        eid.constData(),
        k.constData(),
        active,
        simpleCallbackTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::sendPollVote(
    const QString &roomId,
    const QString &pollEventId,
    const QStringList &optionIds) {
    if (!_handle) {
        emit pollVoteSent(false);
        return;
    }

    const QByteArray rid = roomId.toUtf8();
    const QByteArray eid = pollEventId.toUtf8();
    QJsonArray optionArray;
    for (const auto &optionId : optionIds) {
        optionArray.append(optionId);
    }
    const QByteArray optionIdsJson = QJsonDocument(optionArray).toJson(QJsonDocument::Compact);

    auto *data = new SimpleCallbackData{
        this,
        [this](bool success) { emit pollVoteSent(success); },
    };
    data->guard = _callbackGuard.get();
    tm_send_poll_vote(
        _handle,
        rid.constData(),
        eid.constData(),
        optionIdsJson.constData(),
        simpleCallbackTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::restoreSession(
    const QString &homeserver,
    const QString &userId,
    const QString &deviceId,
    const QString &accessToken) {
    // Restoring a session: ensure media fetches aren't gated by a stale logout().
    _loggingOut = false;
    if (!_handle) {
        _cachedSessionInfo = {};
        emit sessionRestored(false, QString(), QString(), QString(),
            QStringLiteral("No protocol handle"));
        return;
    }

    const QByteArray hs = homeserver.toUtf8();
    const QByteArray uid = userId.toUtf8();
    const QByteArray did = deviceId.toUtf8();
    const QByteArray at = accessToken.toUtf8();
    _cachedSessionInfo.homeserver = homeserver;
    _cachedSessionInfo.userId = userId;
    _cachedSessionInfo.deviceId = deviceId;
    _cachedSessionInfo.accessToken.clear();
    auto *data = new SessionCallbackData{
        this,
        [this](bool success) {
            if (!success) {
                _cachedSessionInfo = {};
            }
        },
    };
    data->guard = _callbackGuard.get();
    tm_restore_session(
        _handle,
        hs.constData(),
        uid.constData(),
        did.constData(),
        at.constData(),
        sessionCallbackTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::enqueueRoomListChangedFromCallback() {
    if (_roomListChangeQueued.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    QMetaObject::invokeMethod(this, [this] {
        _roomListChangeQueued.store(false, std::memory_order_release);
        emit roomListChanged();
    }, Qt::QueuedConnection);
}

void ProtocolBridge::enqueueTimelineChangedFromCallback(const QString &roomId) {
    {
        QMutexLocker locker(&_pendingTimelineRoomsMutex);
        _pendingTimelineRooms.insert(roomId);
        if (_timelineChangeFlushQueued.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
    }
    QMetaObject::invokeMethod(this, [this] {
        QList<QString> roomIds;
        {
            QMutexLocker locker(&_pendingTimelineRoomsMutex);
            roomIds = _pendingTimelineRooms.values();
            _pendingTimelineRooms.clear();
            _timelineChangeFlushQueued.store(false, std::memory_order_release);
        }
        std::sort(roomIds.begin(), roomIds.end());
        for (const auto &roomId : roomIds) {
            emit timelineChanged(roomId);
        }
    }, Qt::QueuedConnection);
}

void ProtocolBridge::handleRoomsReady(
    quint64 requestId,
    bool success,
    const QVector<RoomSummary> &rooms) {
    auto presented = rooms;
    applySavedMessagesIdentity(presented);
    if (success) {
        setCachedRooms(presented);
    }
    emit roomsReady(requestId, success, presented);
}

void ProtocolBridge::handleRoomUnreadSnapshotReady(
    const QString &roomId,
    quint64 requestId,
    bool success,
    const RoomUnreadSnapshot &snapshot) {
    emit roomUnreadSnapshotReady(roomId, requestId, success, snapshot);
}

void ProtocolBridge::handleTimelineSliceReady(
    const QString &roomId,
    quint64 requestId,
    bool success,
    const TimelineSlice &slice) {
    emit timelineSliceReady(roomId, requestId, success, slice);
}

ProtocolBridge::SessionInfo ProtocolBridge::getSessionInfoBlockingForPersistence() {
    SessionInfo info;
    if (!_handle) return info;

    FfiSessionInfo ffi = tm_get_session_info(_handle);
    if (ffi.user_id) {
        info.homeserver = ffi.homeserver ? QString::fromUtf8(ffi.homeserver) : QString();
        info.userId = ffi.user_id ? QString::fromUtf8(ffi.user_id) : QString();
        info.deviceId = ffi.device_id ? QString::fromUtf8(ffi.device_id) : QString();
        info.accessToken = ffi.access_token ? QString::fromUtf8(ffi.access_token) : QString();
    }
    tm_free_session_info(ffi);
    if (!info.userId.isEmpty()) {
        _cachedSessionInfo = info;
        _cachedSessionInfo.accessToken.clear();
    }
    return info;
}

ProtocolBridge::SessionInfo ProtocolBridge::cachedSessionInfo() const {
    return _cachedSessionInfo;
}

void ProtocolBridge::reconnect() {
    if (_handle) {
        tm_sync_reconnect(_handle);
    }
}

void ProtocolBridge::logout() {
    // Stop serving media/OG image fetches: the timeline keeps painting behind the
    // sign-out overlay, but the client is being torn down, so any new request is
    // a doomed round trip. Set before tm_logout so the burst on logout is gated.
    _loggingOut = true;
    if (!_handle) {
        emit loggedOut(false);
        return;
    }
    _cachedSessionInfo = {};

    auto *data = new SimpleCallbackData{
        this,
        [this](bool success) { emit loggedOut(success); },
    };
    data->guard = _callbackGuard.get();
    tm_logout(
        _handle,
        simpleCallbackTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::discoverHomeserver(const QString &domain, quint64 requestId) {
    if (!_handle) {
        emit homeserverDiscovered(requestId, false, QString());
        return;
    }

    const QByteArray d = domain.toUtf8();
    auto *data = new DiscoverCallbackData{this, requestId};
    data->guard = _callbackGuard.get();
    tm_discover_homeserver(
        _handle,
        d.constData(),
        discoverCallbackTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::classifyRegistration(const QString &input, quint64 requestId) {
    if (!_handle) {
        emit registrationClassified(requestId, 0, QString());
        return;
    }

    const QByteArray in = input.toUtf8();
    auto *data = new DiscoverCallbackData{this, requestId};
    data->guard = _callbackGuard.get();
    tm_classify_registration(
        _handle,
        in.constData(),
        classifyRegistrationCallbackTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::probeAuthDelegation(const QString &baseUrl, quint64 requestId) {
    if (!_handle) {
        emit authDelegationProbed(requestId, false, QString());
        return;
    }

    const QByteArray url = baseUrl.toUtf8();
    auto *data = new AuthDelegationCallbackData{this, requestId};
    data->guard = _callbackGuard.get();
    tm_probe_auth_delegation(
        _handle,
        url.constData(),
        authDelegationCallbackTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::probeAccountManagement(const QString &baseUrl, quint64 requestId) {
    if (!_handle) {
        emit accountManagementProbed(requestId, false, QString());
        return;
    }

    const QByteArray url = baseUrl.toUtf8();
    auto *data = new AuthDelegationCallbackData{this, requestId};
    data->guard = _callbackGuard.get();
    tm_probe_account_management(
        _handle,
        url.constData(),
        accountManagementCallbackTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::probePasswordResetPage(const QString &homeserver, quint64 requestId) {
    if (!_handle) {
        emit passwordResetPageProbed(requestId, false, QString());
        return;
    }

    // Raw user input, not a resolved base URL — Rust resolves it, because the
    // forgot-password screen never does.
    const QByteArray input = homeserver.toUtf8();
    auto *data = new AuthDelegationCallbackData{this, requestId};
    data->guard = _callbackGuard.get();
    tm_probe_password_reset_page(
        _handle,
        input.constData(),
        passwordResetPageCallbackTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::probeEmailThreepidSupport(const QString &baseUrl, quint64 requestId) {
    if (!_handle) {
        emit emailThreepidSupportProbed(requestId, false, false);
        return;
    }

    const QByteArray url = baseUrl.toUtf8();
    auto *data = new AuthDelegationCallbackData{this, requestId};
    data->guard = _callbackGuard.get();
    tm_probe_email_threepid_support(
        _handle,
        url.constData(),
        emailThreepidSupportCallbackTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::resolveMedia(const QString &mxcUrl) {
    if (!_handle || _loggingOut) {
        emit mediaResolved(false, mxcUrl, QString());
        return;
    }

    const QByteArray url = mxcUrl.toUtf8();
    auto *data = new MediaCallbackData{this, mxcUrl};
    data->guard = _callbackGuard.get();
    tm_resolve_media_with_progress(
        _handle,
        url.constData(),
        mediaProgressCallbackTrampoline,
        mediaCallbackTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::resolveAvatar(const QString &mxcUrl) {
    // Covers the largest avatar view (room-info 84px) at 3x DPI; every avatar
    // downscales from this one server thumbnail, so a single size serves all.
    constexpr uint32_t kAvatarThumbnailSize = 256;
    if (!_handle || _loggingOut) {
        emit mediaResolved(false, mxcUrl, QString());
        return;
    }

    const QByteArray url = mxcUrl.toUtf8();
    auto *data = new MediaCallbackData{this, mxcUrl};
    data->guard = _callbackGuard.get();
    tm_resolve_avatar(
        _handle,
        url.constData(),
        kAvatarThumbnailSize,
        mediaCallbackTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::resolveMediaBytes(const QString &mxcUrl) {
    if (!_handle || _loggingOut) {
        emit mediaBytesResolved(false, mxcUrl, QByteArray(), QString(), QString());
        return;
    }

    const QByteArray url = mxcUrl.toUtf8();
    auto *data = new MediaCallbackData{this, mxcUrl};
    data->guard = _callbackGuard.get();
    tm_resolve_media_bytes_with_progress(
        _handle,
        url.constData(),
        mediaProgressCallbackTrampoline,
        mediaBytesCallbackTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::cancelMediaDownload(const QString &mxcUrl) {
    if (!_handle || mxcUrl.isEmpty()) {
        return;
    }

    const QByteArray url = mxcUrl.toUtf8();
    tm_cancel_media_download(_handle, url.constData());
}

void ProtocolBridge::resolveMediaThumbnail(const QString &mxcUrl, int width, int height) {
    // Use "srvthumb:" prefix so mediaResolved stores the thumbnail path
    // under a separate key, not overwriting the full video file path.
    const auto thumbKey = QStringLiteral("srvthumb:") + mxcUrl;
    if (!_handle || _loggingOut) {
        emit mediaResolved(false, thumbKey, QString());
        return;
    }

    const QByteArray url = mxcUrl.toUtf8();
    auto *data = new MediaCallbackData{this, thumbKey};
    data->guard = _callbackGuard.get();
    tm_resolve_media_thumbnail(
        _handle,
        url.constData(),
        static_cast<uint32_t>(width),
        static_cast<uint32_t>(height),
        /*allow_partial_video=*/true, // video server thumb: 2MB partial for a frame
        mediaCallbackTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::resolveMediaThumbnailBytes(const QString &mxcUrl, int width, int height) {
    const auto thumbKey = QStringLiteral("srvthumb:") + mxcUrl;
    if (!_handle || _loggingOut) {
        emit mediaBytesResolved(false, thumbKey, QByteArray(), QString(), QString());
        return;
    }

    const QByteArray url = mxcUrl.toUtf8();
    auto *data = new MediaCallbackData{this, thumbKey};
    data->guard = _callbackGuard.get();
    tm_resolve_media_thumbnail_bytes(
        _handle,
        url.constData(),
        static_cast<uint32_t>(width),
        static_cast<uint32_t>(height),
        mediaBytesCallbackTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::getVideoThumbnail(
        const QString &eventId,
        const QString &mxcUrl,
        int width,
        int height) {
    // Persistent encrypted cache keyed by event id; the JPEG frame arrives via
    // mediaBytesResolved under "vidthumb:<eventId>".
    const auto key = QStringLiteral("vidthumb:") + eventId;
    if (!_handle || _loggingOut) {
        emit mediaBytesResolved(false, key, QByteArray(), QString(), QString());
        return;
    }

    const QByteArray event = eventId.toUtf8();
    const QByteArray url = mxcUrl.toUtf8();
    auto *data = new MediaCallbackData{this, key};
    data->guard = _callbackGuard.get();
    tm_get_video_thumbnail(
        _handle,
        event.constData(),
        url.constData(),
        static_cast<uint32_t>(width),
        static_cast<uint32_t>(height),
        mediaBytesCallbackTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::resolveMediaPreviewImage(const QString &mxcUrl, int width, int height) {
    if (!_handle || _loggingOut) {
        emit mediaResolved(false, MediaCache::previewImageKey(mxcUrl), QString());
        return;
    }

    const QByteArray url = mxcUrl.toUtf8();
    auto *data = new MediaCallbackData{this, MediaCache::previewImageKey(mxcUrl)};
    data->guard = _callbackGuard.get();
    tm_resolve_media_thumbnail(
        _handle,
        url.constData(),
        static_cast<uint32_t>(width),
        static_cast<uint32_t>(height),
        /*allow_partial_video=*/false, // OG image: never cache a truncated .mp4
        mediaCallbackTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::resolveMediaPreviewImageBytes(const QString &mxcUrl, int width, int height) {
    const auto previewKey = MediaCache::previewImageKey(mxcUrl);
    if (!_handle || _loggingOut) {
        emit mediaBytesResolved(false, previewKey, QByteArray(), QString(), QString());
        return;
    }

    const QByteArray url = mxcUrl.toUtf8();
    auto *data = new MediaCallbackData{this, previewKey};
    data->guard = _callbackGuard.get();
    tm_resolve_media_thumbnail_bytes(
        _handle,
        url.constData(),
        static_cast<uint32_t>(width),
        static_cast<uint32_t>(height),
        mediaBytesCallbackTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::exportMediaToPath(const QString &mxcUrl, const QString &targetPath) {
    if (!_handle || mxcUrl.isEmpty() || targetPath.isEmpty()) {
        emit mediaExported(false, mxcUrl, targetPath);
        return;
    }

    const QByteArray url = mxcUrl.toUtf8();
    const QByteArray target = targetPath.toUtf8();
    auto *data = new MediaExportCallbackData{this, mxcUrl, targetPath};
    data->guard = _callbackGuard.get();
    tm_export_media_to_path(
        _handle,
        url.constData(),
        target.constData(),
        mediaExportCallbackTrampoline,
        static_cast<void *>(data));
}

// --- Cache management ---

static void cacheStatsCallbackTrampoline(
    uint64_t mediaFiles,
    uint64_t previewCache,
    uint64_t appCache,
    uint64_t searchIndex,
    uint64_t total,
    uint64_t fileCount,
    void *userdata)
{
    withGuardedBridge(userdata, [=](ProtocolBridge *bridge) {
        QMetaObject::invokeMethod(bridge, [=]() {
            CacheStats stats;
            stats.mediaFilesBytes = mediaFiles;
            stats.previewCacheBytes = previewCache;
            stats.appCacheBytes = appCache;
            stats.searchIndexBytes = searchIndex;
            stats.totalBytes = total;
            stats.mediaFileCount = fileCount;
            emit bridge->cacheStatsReady(stats);
        }, Qt::QueuedConnection);
    });
}

static void cacheClearCallbackTrampoline(
    bool success, uint64_t freedBytes, void *userdata)
{
    withGuardedBridge(userdata, [=](ProtocolBridge *bridge) {
        QMetaObject::invokeMethod(bridge, [=]() {
            emit bridge->cacheClearResult(success, freedBytes);
        }, Qt::QueuedConnection);
    });
}

void ProtocolBridge::getCacheStats() {
    if (!_handle) return;
    tm_get_cache_stats(_handle, cacheStatsCallbackTrampoline, _callbackGuard.get());
}

void ProtocolBridge::clearMediaCache(quint32 maxAgeDays, quint64 sizeLimitBytes) {
    if (!_handle) return;
    tm_clear_media_cache(_handle, maxAgeDays, sizeLimitBytes,
                         cacheClearCallbackTrampoline, _callbackGuard.get());
}

void ProtocolBridge::clearAllCaches() {
    if (!_handle) return;
    tm_clear_all_caches(_handle, cacheClearCallbackTrampoline, _callbackGuard.get());
}

void ProtocolBridge::autoCleanupCache(quint64 sizeLimitBytes) {
    if (!_handle) return;
    tm_auto_cleanup_cache(_handle, sizeLimitBytes);
}

void ProtocolBridge::setMediaCacheLimit(quint64 limitBytes) {
    if (!_handle) return;
    tm_set_media_cache_limit(_handle, limitBytes);
}

void ProtocolBridge::setE2eeSearchEnabled(bool enabled) {
    if (!_handle) return;
    tm_set_e2ee_search_enabled(_handle, enabled);
}

// --- Session verification ---

void ProtocolBridge::startSasVerification(const QString &transactionId) {
    if (!_handle) {
        emit sasVerificationStarted(false, QStringList(), QStringList());
        return;
    }
    if (transactionId.isEmpty()) {
        tm_start_sas_verification(
            _handle,
            sasCallbackTrampoline,
            static_cast<void *>(_callbackGuard.get()));
    } else {
        const QByteArray tx = transactionId.toUtf8();
        tm_start_sas_verification_for(
            _handle,
            tx.constData(),
            sasCallbackTrampoline,
            static_cast<void *>(_callbackGuard.get()));
    }
}

void ProtocolBridge::startUserVerification(const QString &userId) {
    if (!_handle) {
        emit sasVerificationStarted(false, QStringList(), QStringList());
        return;
    }
    const QByteArray uid = userId.toUtf8();
    tm_start_user_verification(
        _handle,
        uid.constData(),
        sasCallbackTrampoline,
        static_cast<void *>(_callbackGuard.get()));
}

void ProtocolBridge::withdrawUserVerification(const QString &userId) {
    if (!_handle) {
        emit userVerificationWithdrawn(userId, false);
        return;
    }
    const QByteArray uid = userId.toUtf8();
    auto *data = new SimpleCallbackData{
        this,
        [this, userId](bool success) { emit userVerificationWithdrawn(userId, success); },
    };
    data->guard = _callbackGuard.get();
    tm_withdraw_user_verification(
        _handle,
        uid.constData(),
        simpleCallbackTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::userTrustState(const QString &userId) {
    if (!_handle) {
        emit userTrustStateResult(userId, 0);
        return;
    }
    const QByteArray uid = userId.toUtf8();
    auto *data = new UserTrustStateCallbackData{this, userId, _callbackGuard.get()};
    tm_user_trust_state(
        _handle,
        uid.constData(),
        userTrustStateTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::confirmSasMatch() {
    if (!_handle) {
        emit sasConfirmed(false);
        return;
    }
    auto *data = new SimpleCallbackData{
        this,
        [this](bool success) { emit sasConfirmed(success); },
    };
    data->guard = _callbackGuard.get();
    tm_confirm_sas(
        _handle,
        simpleCallbackTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::verifyWithRecoveryKey(const QString &key) {
    if (!_handle) {
        emit recoveryKeyVerified(false);
        return;
    }
    const QByteArray k = key.toUtf8();
    auto *data = new SimpleCallbackData{
        this,
        [this](bool success) { emit recoveryKeyVerified(success); },
    };
    data->guard = _callbackGuard.get();
    tm_verify_recovery_key(
        _handle,
        k.constData(),
        simpleCallbackTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::skipVerification() {
    if (!_handle) {
        emit verificationSkipped(true);
        return;
    }
    auto *data = new SimpleCallbackData{
        this,
        [this](bool success) { emit verificationSkipped(success); },
    };
    data->guard = _callbackGuard.get();
    tm_skip_verification(
        _handle,
        simpleCallbackTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::startQrVerification(const QString &transactionId) {
    if (!_handle) {
        emit qrCodeReady(false, QByteArray(), 0);
        return;
    }
    if (transactionId.isEmpty()) {
        tm_start_qr_verification(
            _handle,
            qrCodeCallbackTrampoline,
            static_cast<void *>(_callbackGuard.get()));
    } else {
        const QByteArray tx = transactionId.toUtf8();
        tm_start_qr_verification_for(
            _handle,
            tx.constData(),
            qrCodeCallbackTrampoline,
            static_cast<void *>(_callbackGuard.get()));
    }
}

void ProtocolBridge::confirmQrScanned() {
    if (!_handle) {
        emit qrScanConfirmed(false);
        return;
    }
    auto *data = new SimpleCallbackData{
        this,
        [this](bool success) { emit qrScanConfirmed(success); },
    };
    data->guard = _callbackGuard.get();
    tm_confirm_qr_scanned(
        _handle,
        simpleCallbackTrampoline,
        static_cast<void *>(data));
}

// --- Account settings ---

void ProtocolBridge::fetchAccountSummary() {
    if (!_handle) {
        emit accountSummaryReady(false, AccountSummary(), QStringLiteral("No handle"));
        return;
    }
    auto *data = new AccountSummaryCallbackData{this};
    data->guard = _callbackGuard.get();
    tm_get_account_summary(
        _handle,
        accountSummaryCallbackTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::setDisplayName(const QString &name) {
    if (!_handle) {
        emit displayNameSet(false, QStringLiteral("No handle"));
        return;
    }
    const QByteArray n = name.toUtf8();
    auto *data = new SimpleCallbackData{
        this,
        [this](bool success) {
            emit displayNameSet(success, success ? QString() : QStringLiteral("Failed"));
        },
    };
    data->guard = _callbackGuard.get();
    tm_set_display_name(
        _handle,
        n.constData(),
        simpleCallbackTrampoline,
        static_cast<void *>(data));
}

// --- Sessions + Encryption ---

void ProtocolBridge::getOwnDevices() {
    if (!_handle) {
        emit ownDevicesReady(false, DeviceSessionList());
        return;
    }
    tm_get_own_devices(
        _handle,
        deviceListCallbackTrampoline,
        static_cast<void *>(_callbackGuard.get()));
}

void ProtocolBridge::renameDevice(const QString &deviceId, const QString &displayName) {
    if (!_handle) {
        emit deviceRenamed(false);
        return;
    }
    const QByteArray devId = deviceId.toUtf8();
    const QByteArray name = displayName.toUtf8();
    auto *data = new SimpleCallbackData{
        this,
        [this](bool success) { emit deviceRenamed(success); },
    };
    data->guard = _callbackGuard.get();
    tm_rename_device(
        _handle,
        devId.constData(),
        name.constData(),
        simpleCallbackTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::cancelVerification(const QString &transactionId) {
    if (!_handle) {
        emit verificationCancelled(false);
        return;
    }
    auto *data = new SimpleCallbackData{
        this,
        [this](bool success) { emit verificationCancelled(success); },
    };
    data->guard = _callbackGuard.get();
    if (transactionId.isEmpty()) {
        tm_cancel_verification(
            _handle,
            simpleCallbackTrampoline,
            static_cast<void *>(data));
    } else {
        const QByteArray tx = transactionId.toUtf8();
        tm_cancel_verification_for(
            _handle,
            tx.constData(),
            simpleCallbackTrampoline,
            static_cast<void *>(data));
    }
}

void ProtocolBridge::mismatchSas() {
    if (!_handle) {
        emit verificationCancelled(false);
        return;
    }
    auto *data = new SimpleCallbackData{
        this,
        [this](bool success) { emit verificationCancelled(success); },
    };
    data->guard = _callbackGuard.get();
    tm_mismatch_sas(
        _handle,
        simpleCallbackTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::setAvatarUrl(const QString &mxcUrl) {
    if (!_handle) {
        emit avatarSet(false, QStringLiteral("No handle"));
        return;
    }
    const QByteArray u = mxcUrl.toUtf8();
    auto *data = new SimpleCallbackData{
        this,
        [this](bool success) {
            emit avatarSet(success, success ? QString() : QStringLiteral("Failed"));
        },
    };
    data->guard = _callbackGuard.get();
    tm_set_avatar_url(
        _handle,
        mxcUrl.isEmpty() ? nullptr : u.constData(),
        simpleCallbackTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::deleteDevices(const QStringList &deviceIds, const QString &authJson) {
    if (!_handle) {
        emit devicesDeleted(false, DeleteDevicesResult());
        return;
    }
    QVector<QByteArray> idBytes;
    QVector<const char *> idPtrs;
    idBytes.reserve(deviceIds.size());
    idPtrs.reserve(deviceIds.size());
    for (const auto &id : deviceIds) {
        idBytes.append(id.toUtf8());
        idPtrs.append(idBytes.last().constData());
    }
    const QByteArray authUtf8 = authJson.toUtf8();
    auto *data = new SimpleCallbackData{ this, {} };
    data->guard = _callbackGuard.get();
    tm_delete_devices(
        _handle,
        idPtrs.constData(),
        static_cast<size_t>(idPtrs.size()),
        authJson.isEmpty() ? nullptr : authUtf8.constData(),
        deleteDevicesCallbackTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::getEncryptionOverview() {
    if (!_handle) {
        emit encryptionOverviewReady(false, EncryptionOverview());
        return;
    }
    tm_get_encryption_overview(
        _handle,
        encryptionOverviewCallbackTrampoline,
        static_cast<void *>(_callbackGuard.get()));
}

void ProtocolBridge::setKeyStorageEnabled(bool enabled) {
    if (!_handle) {
        emit keyStorageUpdated(false);
        return;
    }
    auto *data = new SimpleCallbackData{
        this,
        [this](bool success) { emit keyStorageUpdated(success); },
    };
    data->guard = _callbackGuard.get();
    tm_set_key_storage_enabled(
        _handle,
        enabled,
        simpleCallbackTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::uploadAvatarAndSet(const QByteArray &imageData, const QString &contentType) {
    if (!_handle) {
        emit avatarUploaded(false, QString(), QStringLiteral("No handle"));
        return;
    }
    const QByteArray ct = contentType.toUtf8();
    auto *data = new SendCallbackData{
        this,
        [this](bool success, const QString &mxcUrl) {
            emit avatarUploaded(success, mxcUrl, success ? QString() : QStringLiteral("Failed"));
        },
    };
    data->guard = _callbackGuard.get();
    tm_upload_avatar_and_set(
        _handle,
        reinterpret_cast<const uint8_t *>(imageData.constData()),
        static_cast<size_t>(imageData.size()),
        ct.constData(),
        sendCallbackWithDataTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::fetchThreepids() {
    if (!_handle) {
        emit threepidsReady(false, {}, QStringLiteral("No handle"));
        return;
    }
    auto *data = new ThreePidListCallbackData{this};
    data->guard = _callbackGuard.get();
    tm_get_3pids(
        _handle,
        threePidListCallbackTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::requestThreepidToken(
    ThreePidMedium medium,
    const QString &address,
    const QString &clientSecret,
    quint32 sendAttempt,
    const QString &country)
{
    if (!_handle) {
        emit threepidTokenReady(false, ThreePidTokenResponse(), QStringLiteral("No handle"));
        return;
    }
    const QByteArray addr = address.toUtf8();
    const QByteArray countryBytes = country.toUtf8();
    const QByteArray cs = clientSecret.toUtf8();
    auto *data = new ThreePidTokenCallbackData{this};
    data->guard = _callbackGuard.get();
    tm_request_3pid_token(
        _handle,
        static_cast<uint32_t>(medium),
        addr.constData(),
        countryBytes.constData(),
        cs.constData(),
        sendAttempt,
        threePidTokenCallbackTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::addThreepid(
        const QString &clientSecret, const QString &sid, const QString &authJson) {
    if (!_handle) {
        AccountActionResult r;
        r.errorMessage = QStringLiteral("No handle");
        emit threepidAdded(r);
        return;
    }
    const QByteArray cs = clientSecret.toUtf8();
    const QByteArray s = sid.toUtf8();
    const QByteArray aj = authJson.toUtf8();
    auto *data = new AccountActionCallbackData{
        this,
        AccountActionCallbackData::Kind::Add3pid,
    };
    data->guard = _callbackGuard.get();
    tm_add_3pid(
        _handle,
        cs.constData(),
        s.constData(),
        authJson.isEmpty() ? nullptr : aj.constData(),
        accountActionCallbackTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::deleteThreepid(ThreePidMedium medium, const QString &address) {
    if (!_handle) {
        emit threepidDeleted(false);
        return;
    }
    const QByteArray addr = address.toUtf8();
    auto *data = new SimpleCallbackData{
        this,
        [this](bool ok) { emit threepidDeleted(ok); }
    };
    data->guard = _callbackGuard.get();
    tm_delete_3pid(
        _handle,
        static_cast<uint32_t>(medium),
        addr.constData(),
        simpleCallbackTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::changePassword(const QString &newPassword, const QString &authJson) {
    if (!_handle) {
        AccountActionResult r;
        r.errorMessage = QStringLiteral("No handle");
        emit changePasswordResult(r);
        return;
    }
    const QByteArray pw = newPassword.toUtf8();
    const QByteArray aj = authJson.toUtf8();
    auto *data = new AccountActionCallbackData{
        this,
        AccountActionCallbackData::Kind::ChangePassword,
    };
    data->guard = _callbackGuard.get();
    tm_change_password(
        _handle,
        pw.constData(),
        authJson.isEmpty() ? nullptr : aj.constData(),
        accountActionCallbackTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::deactivateAccount(bool eraseData, const QString &authJson) {
    if (!_handle) {
        AccountActionResult r;
        r.errorMessage = QStringLiteral("No handle");
        emit deactivateAccountResult(r);
        return;
    }
    const QByteArray aj = authJson.toUtf8();
    auto *data = new AccountActionCallbackData{
        this,
        AccountActionCallbackData::Kind::Deactivate,
    };
    data->guard = _callbackGuard.get();
    tm_deactivate_account(
        _handle,
        eraseData,
        authJson.isEmpty() ? nullptr : aj.constData(),
        accountActionCallbackTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::enterRecoveryKey(const QString &recoveryKey) {
    if (!_handle) {
        emit recoveryKeyAccepted(false);
        return;
    }
    const QByteArray key = recoveryKey.toUtf8();
    auto *data = new SimpleCallbackData{
        this,
        [this](bool success) { emit recoveryKeyAccepted(success); },
    };
    data->guard = _callbackGuard.get();
    tm_enter_recovery_key(
        _handle,
        key.constData(),
        simpleCallbackTrampoline,
        static_cast<void *>(data));
}

static void verificationCapabilitiesTrampoline(
    bool success,
    FfiVerificationCapabilities caps,
    void *userdata)
{
    withGuardedBridge(userdata, [success, caps](ProtocolBridge *bridge) {
        QMetaObject::invokeMethod(bridge,
            [bridge, success, caps]() {
                emit bridge->verificationCapabilitiesReady(
                    success,
                    caps.can_verify_with_device,
                    caps.can_verify_with_recovery,
                    caps.sas_supported,
                    caps.qr_supported);
            }, Qt::QueuedConnection);
    });
}

void ProtocolBridge::getVerificationCapabilities() {
    if (!_handle) {
        emit verificationCapabilitiesReady(false, false, false, false, false);
        return;
    }
    tm_get_verification_capabilities(
        _handle,
        verificationCapabilitiesTrampoline,
        static_cast<void *>(_callbackGuard.get()));
}

// --- Room Settings Snapshot ---

static void roomSettingsTrampoline(
    bool success,
    FfiRoomSettingsSnapshot ffiSnap,
    void *userdata)
{
    // Convert FFI struct to C++ struct on this thread, then free.
    RoomSettingsSnapshot snap;
    snap.roomId = ffiSnap.room_id ? QString::fromUtf8(ffiSnap.room_id) : QString();
    if (success) {
        snap.displayName = ffiSnap.display_name ? QString::fromUtf8(ffiSnap.display_name) : QString();
        snap.canonicalAlias = ffiSnap.canonical_alias ? QString::fromUtf8(ffiSnap.canonical_alias) : QString();
        switch (ffiSnap.notification_mode) {
        case 1:
            snap.notificationMode = RoomNotificationMode::MentionsOnly;
            break;
        case 2:
            snap.notificationMode = RoomNotificationMode::Mute;
            break;
        default:
            snap.notificationMode = RoomNotificationMode::AllMessages;
            break;
        }
        snap.isMuted = ffiSnap.is_muted;
        snap.memberCount = ffiSnap.member_count;
        snap.isEncrypted = ffiSnap.is_encrypted;
        snap.encryptionAlgorithm = ffiSnap.encryption_algorithm ? QString::fromUtf8(ffiSnap.encryption_algorithm) : QString();
        snap.access = static_cast<RoomAccess>(ffiSnap.access);
        snap.historyVisibility = static_cast<HistoryVisibility>(ffiSnap.history_visibility);
        snap.newMembersCanSeeHistory = ffiSnap.new_members_can_see_history;
        snap.canInvite = ffiSnap.can_invite;
        snap.canKick = ffiSnap.can_kick;
        snap.canBan = ffiSnap.can_ban;
        snap.canChangeAvatar = ffiSnap.can_change_avatar;
        snap.canChangeName = ffiSnap.can_change_name;
        snap.canChangeTopic = ffiSnap.can_change_topic;
        snap.canChangeEncryption = ffiSnap.can_change_encryption;
        snap.canChangeAccess = ffiSnap.can_change_access;
        snap.canChangeHistoryVisibility = ffiSnap.can_change_history_visibility;
    }
    tm_free_room_settings(ffiSnap);

    withGuardedBridge(userdata, [success, snap](ProtocolBridge *bridge) {
        QMetaObject::invokeMethod(bridge,
            [bridge, success, snap]() {
                emit bridge->roomSettingsReady(success, snap);
            }, Qt::QueuedConnection);
    });
}

void ProtocolBridge::getRoomSettings(const QString &roomId) {
    if (!_handle) {
        RoomSettingsSnapshot snapshot;
        snapshot.roomId = roomId;
        emit roomSettingsReady(false, snapshot);
        return;
    }
    const auto roomIdUtf8 = roomId.toUtf8();
    tm_get_room_settings(
        _handle,
        roomIdUtf8.constData(),
        roomSettingsTrampoline,
        static_cast<void *>(_callbackGuard.get()));
}

void ProtocolBridge::enableRoomEncryption(const QString &roomId) {
    if (!_handle) {
        emit roomEncryptionEnabled(false);
        return;
    }

    const QByteArray rid = roomId.toUtf8();
    auto *data = new SimpleCallbackData{
        this,
        [this](bool success) { emit roomEncryptionEnabled(success); },
    };
    data->guard = _callbackGuard.get();
    tm_enable_room_encryption(
        _handle,
        rid.constData(),
        simpleCallbackTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::setRoomAccess(const QString &roomId, RoomAccess access) {
    if (!_handle) {
        emit roomAccessSet(roomId, false);
        return;
    }

    const QByteArray rid = roomId.toUtf8();
    auto *data = new SimpleCallbackData{
        this,
        [this, roomId](bool success) {
            emit roomAccessSet(roomId, success);
            if (success) {
                emit roomListChanged();
            }
        },
    };
    data->guard = _callbackGuard.get();
    tm_set_room_access(
        _handle,
        rid.constData(),
        static_cast<uint32_t>(access),
        simpleCallbackTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::setRoomName(const QString &roomId, const QString &name) {
    if (!_handle) {
        emit roomNameSet(roomId, false);
        return;
    }

    const QByteArray rid = roomId.toUtf8();
    const QByteArray nm = name.toUtf8();
    auto *data = new SimpleCallbackData{
        this,
        [this, roomId](bool success) {
            emit roomNameSet(roomId, success);
            if (success) {
                emit roomListChanged();
            }
        },
    };
    data->guard = _callbackGuard.get();
    tm_set_room_name(
        _handle,
        rid.constData(),
        nm.constData(),
        simpleCallbackTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::setRoomTopic(const QString &roomId, const QString &topic) {
    if (!_handle) {
        emit roomTopicSet(roomId, false);
        return;
    }

    const QByteArray rid = roomId.toUtf8();
    const QByteArray tp = topic.toUtf8();
    auto *data = new SimpleCallbackData{
        this,
        [this, roomId](bool success) {
            emit roomTopicSet(roomId, success);
            if (success) {
                emit roomListChanged();
            }
        },
    };
    data->guard = _callbackGuard.get();
    tm_set_room_topic(
        _handle,
        rid.constData(),
        tp.constData(),
        simpleCallbackTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::setRoomHistoryVisibility(
    const QString &roomId,
    HistoryVisibility visibility) {
    if (!_handle) {
        emit roomHistoryVisibilitySet(roomId, false);
        return;
    }

    const QByteArray rid = roomId.toUtf8();
    auto *data = new SimpleCallbackData{
        this,
        [this, roomId](bool success) {
            emit roomHistoryVisibilitySet(roomId, success);
        },
    };
    data->guard = _callbackGuard.get();
    tm_set_room_history_visibility(
        _handle,
        rid.constData(),
        static_cast<uint32_t>(visibility),
        simpleCallbackTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::uploadRoomAvatar(
    const QString &roomId,
    const QByteArray &imageData,
    const QString &contentType) {
    if (!_handle || roomId.isEmpty() || imageData.isEmpty()) {
        emit roomAvatarUploaded(roomId, false, QString());
        return;
    }
    const QByteArray rid = roomId.toUtf8();
    const QByteArray ct = contentType.toUtf8();
    auto *data = new SendCallbackData{
        this,
        [this, roomId](bool success, const QString &mxcUrl) {
            emit roomAvatarUploaded(roomId, success, mxcUrl);
            if (success) {
                emit roomListChanged();
            }
        },
    };
    data->guard = _callbackGuard.get();
    tm_upload_room_avatar(
        _handle,
        rid.constData(),
        reinterpret_cast<const uint8_t *>(imageData.constData()),
        static_cast<size_t>(imageData.size()),
        ct.constData(),
        sendCallbackWithDataTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::deleteRoomAvatar(const QString &roomId) {
    if (!_handle || roomId.isEmpty()) {
        emit roomAvatarDeleted(roomId, false);
        return;
    }
    const QByteArray rid = roomId.toUtf8();
    auto *data = new SimpleCallbackData{
        this,
        [this, roomId](bool success) {
            emit roomAvatarDeleted(roomId, success);
            if (success) {
                emit roomListChanged();
            }
        },
    };
    data->guard = _callbackGuard.get();
    tm_delete_room_avatar(
        _handle,
        rid.constData(),
        simpleCallbackTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::getUserProfileDetailsAsync(
    const QString &roomId,
    const QString &userId) {
    if (!_handle) {
        UserProfileDetails result;
        result.roomId = roomId;
        result.userId = userId;
        result.displayName = userId;
        emit userProfileDetailsReady(roomId, userId, false, result);
        return;
    }

    const QByteArray rid = roomId.toUtf8();
    const QByteArray uid = userId.toUtf8();
    auto *data = new UserProfileDetailsCallbackData{
        this,
        roomId,
        userId,
        [this, roomId, userId](bool success, const UserProfileDetails &details) {
            emit userProfileDetailsReady(roomId, userId, success, details);
        },
    };
    data->guard = _callbackGuard.get();
    tm_get_user_profile_details_async(
        _handle,
        rid.constData(),
        uid.constData(),
        userProfileDetailsCallbackTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::setUserPowerLevel(
    const QString &roomId,
    const QString &userId,
    qint64 powerLevel) {
    if (!_handle) {
        emit userPowerLevelSet(roomId, userId, false, powerLevel);
        return;
    }
    const QByteArray rid = roomId.toUtf8();
    const QByteArray uid = userId.toUtf8();
    auto *data = new SimpleCallbackData{
        this,
        [this, roomId, userId, powerLevel](bool success) {
            emit userPowerLevelSet(roomId, userId, success, powerLevel);
        },
    };
    data->guard = _callbackGuard.get();
    tm_set_user_power_level(
        _handle,
        rid.constData(),
        uid.constData(),
        powerLevel,
        simpleCallbackTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::adoptSavedMessagesRoomId(const QString &roomId) {
    if (_savedMessagesRoomId == roomId) {
        return;
    }
    _savedMessagesRoomId = roomId;
    emit savedMessagesRoomChanged(roomId);
}

void ProtocolBridge::applySavedMessagesIdentity(QVector<RoomSummary> &rooms) const {
    // Saved Messages presents with a fixed name and the drawn bookmark
    // userpic, whatever the room's server state says.
    if (_savedMessagesRoomId.isEmpty()) {
        return;
    }
    for (auto &room : rooms) {
        if (room.roomId == _savedMessagesRoomId) {
            room.displayName = SavedMessages::displayName();
            room.avatarUrl.clear();
            // Self-chat: no "sender:" prefix on the preview line (tdesktop
            // shows the bare text, like any DM row).
            room.lastSender.clear();
            break;
        }
    }
}

void ProtocolBridge::ensureSavedMessagesRoom(bool create) {
    if (!_handle) {
        emit savedMessagesRoomReady(false, QString());
        return;
    }
    auto *data = new SavedMessagesCallbackData{
        this,
        [this](bool success, const QString &roomId) {
            // roomId may be empty (a passive ensure found no saved room and did
            // not create one); adopt it either way so the cached id tracks
            // reality.
            if (success) {
                adoptSavedMessagesRoomId(roomId);
            }
            emit savedMessagesRoomReady(success, roomId);
        },
    };
    data->guard = _callbackGuard.get();
    tm_ensure_saved_messages_room(
        _handle, create, savedMessagesCallbackTrampoline, static_cast<void *>(data));
}

void ProtocolBridge::deleteSavedMessages() {
    if (!_handle || _savedMessagesRoomId.isEmpty()) {
        return;
    }
    // Permanent: the room is left + forgotten and the marker cleared. Clear the
    // cached id at once so the UI stops treating the (leaving) room as saved;
    // the room-list Remove that follows drops it from the list.
    adoptSavedMessagesRoomId(QString());
    auto *data = new SimpleCallbackData{
        this,
        [](bool success) {
            if (!success) {
                qWarning() << "[saved-messages] delete failed";
            }
        },
    };
    data->guard = _callbackGuard.get();
    tm_delete_saved_messages(
        _handle, simpleCallbackTrampoline, static_cast<void *>(data));
}

void ProtocolBridge::createDirectRoom(const QString &userId) {
    if (!_handle || userId.isEmpty()) {
        emit directRoomCreated(userId, false, QString());
        return;
    }
    const QByteArray uid = userId.toUtf8();
    auto *data = new SendCallbackData{
        this,
        [this, userId](bool success, const QString &roomId) {
            emit directRoomCreated(userId, success, roomId);
        },
    };
    data->guard = _callbackGuard.get();
    tm_create_direct_room(
        _handle,
        uid.constData(),
        createRoomCallbackTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::kickUser(
    const QString &roomId,
    const QString &userId,
    const QString &reason) {
    if (!_handle) {
        emit userKicked(false);
        return;
    }
    const QByteArray rid = roomId.toUtf8();
    const QByteArray uid = userId.toUtf8();
    const QByteArray reasonUtf8 = reason.toUtf8();
    auto *data = new SimpleCallbackData{
        this,
        [this](bool success) { emit userKicked(success); },
    };
    data->guard = _callbackGuard.get();
    tm_kick_user(
        _handle,
        rid.constData(),
        uid.constData(),
        reason.isEmpty() ? nullptr : reasonUtf8.constData(),
        simpleCallbackTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::banUser(
    const QString &roomId,
    const QString &userId,
    const QString &reason) {
    if (!_handle) {
        emit userBanned(false);
        return;
    }
    const QByteArray rid = roomId.toUtf8();
    const QByteArray uid = userId.toUtf8();
    const QByteArray reasonUtf8 = reason.toUtf8();
    auto *data = new SimpleCallbackData{
        this,
        [this](bool success) { emit userBanned(success); },
    };
    data->guard = _callbackGuard.get();
    tm_ban_user(
        _handle,
        rid.constData(),
        uid.constData(),
        reason.isEmpty() ? nullptr : reasonUtf8.constData(),
        simpleCallbackTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::createRecoveryKey() {
    if (!_handle) {
        emit recoveryKeyCreated(false, QString());
        return;
    }
    tm_create_recovery_key(
        _handle,
        recoveryKeyCreatedCallbackTrampoline,
        static_cast<void *>(_callbackGuard.get()));
}

void ProtocolBridge::setupRecovery() {
    if (!_handle) {
        emit recoverySetupResult(false, QString(), 2, QString());
        return;
    }
    tm_setup_recovery(
        _handle,
        recoverySetupCallbackTrampoline,
        static_cast<void *>(_callbackGuard.get()));
}

void ProtocolBridge::resetRecovery() {
    if (!_handle) {
        emit recoverySetupResult(false, QString(), 2, QString());
        return;
    }
    tm_reset_recovery(
        _handle,
        recoverySetupCallbackTrampoline,
        static_cast<void *>(_callbackGuard.get()));
}

void ProtocolBridge::commitRecoveryKey(const QString &recoveryKey) {
    if (!_handle) {
        emit recoveryKeyCommitted(false);
        return;
    }
    const QByteArray key = recoveryKey.toUtf8();
    auto *data = new SimpleCallbackData{
        this,
        [this](bool success) { emit recoveryKeyCommitted(success); },
    };
    data->guard = _callbackGuard.get();
    tm_commit_recovery_key(
        _handle,
        key.constData(),
        simpleCallbackTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::unbanUser(
    const QString &roomId,
    const QString &userId) {
    if (!_handle) {
        emit userUnbanned(false);
        return;
    }
    const QByteArray rid = roomId.toUtf8();
    const QByteArray uid = userId.toUtf8();
    auto *data = new SimpleCallbackData{
        this,
        [this](bool success) { emit userUnbanned(success); },
    };
    data->guard = _callbackGuard.get();
    tm_unban_user(
        _handle,
        rid.constData(),
        uid.constData(),
        simpleCallbackTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::resetIdentity(const QString &authJson) {
    if (!_handle) {
        emit identityResetResult(false, ResetIdentityResult());
        return;
    }
    const QByteArray authUtf8 = authJson.toUtf8();
    tm_reset_identity(
        _handle,
        authJson.isEmpty() ? nullptr : authUtf8.constData(),
        resetIdentityCallbackTrampoline,
        static_cast<void *>(_callbackGuard.get()));
}

void ProtocolBridge::exportE2EKeys(const QString &path, const QString &passphrase) {
    if (!_handle) {
        emit e2eKeysExported(false);
        return;
    }
    const QByteArray p = path.toUtf8();
    const QByteArray pp = passphrase.toUtf8();
    auto *data = new SimpleCallbackData{
        this,
        [this](bool success) { emit e2eKeysExported(success); },
    };
    data->guard = _callbackGuard.get();
    tm_export_e2e_keys(
        _handle,
        p.constData(),
        pp.constData(),
        simpleCallbackTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::inviteUser(
    const QString &roomId,
    const QString &userId) {
    if (!_handle) {
        emit userInvited(false);
        return;
    }
    const QByteArray rid = roomId.toUtf8();
    const QByteArray uid = userId.toUtf8();
    auto *data = new SimpleCallbackData{
        this,
        [this](bool success) { emit userInvited(success); },
    };
    data->guard = _callbackGuard.get();
    tm_invite_user(
        _handle,
        rid.constData(),
        uid.constData(),
        simpleCallbackTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::setUserIgnored(const QString &userId, bool ignored) {
    if (!_handle) {
        emit userIgnoredSet(false, ignored);
        return;
    }
    const QByteArray uid = userId.toUtf8();
    auto *data = new SimpleCallbackData{
        this,
        [this, ignored](bool success) { emit userIgnoredSet(success, ignored); },
    };
    data->guard = _callbackGuard.get();
    tm_set_user_ignored(
        _handle,
        uid.constData(),
        ignored,
        simpleCallbackTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::importE2EKeys(const QString &path, const QString &passphrase) {
    if (!_handle) {
        emit e2eKeysImported(false, 0, 0);
        return;
    }
    const QByteArray p = path.toUtf8();
    const QByteArray pp = passphrase.toUtf8();
    tm_import_e2e_keys(
        _handle,
        p.constData(),
        pp.constData(),
        importKeysCallbackTrampoline,
        static_cast<void *>(_callbackGuard.get()));
}

// --- Password reset ---

void ProtocolBridge::requestPasswordReset(const QString &homeserver, const QString &email) {
    if (!_handle) {
        emit passwordResetTokenSent(false, QString(), QString(),
                                     QStringLiteral("No protocol handle"));
        return;
    }
    const QString clientSecret = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QByteArray hs = homeserver.toUtf8();
    const QByteArray em = email.toUtf8();
    const QByteArray cs = clientSecret.toUtf8();
    auto *data = new PasswordResetTokenCallbackData{this, clientSecret};
    data->guard = _callbackGuard.get();
    tm_request_password_reset(
        _handle,
        hs.constData(),
        em.constData(),
        cs.constData(),
        passwordResetTokenCallbackTrampoline,
        static_cast<void *>(data));
}

void ProtocolBridge::resetPassword(
    const QString &homeserver,
    const QString &newPassword,
    const QString &sid,
    const QString &clientSecret) {
    if (!_handle) {
        emit passwordResetComplete(false, QStringLiteral("No protocol handle"));
        return;
    }
    const QByteArray hs = homeserver.toUtf8();
    const QByteArray pw = newPassword.toUtf8();
    const QByteArray sidBytes = sid.toUtf8();
    const QByteArray cs = clientSecret.toUtf8();
    auto *data = new SimpleCallbackData{
        this,
        [this](bool success) {
            emit passwordResetComplete(success, success ? QString() : QStringLiteral("Password reset failed"));
        },
    };
    data->guard = _callbackGuard.get();
    tm_reset_password(
        _handle,
        hs.constData(),
        pw.constData(),
        sidBytes.constData(),
        cs.constData(),
        simpleCallbackTrampoline,
        static_cast<void *>(data));
}

// --- System keychain access ---

QString ProtocolBridge::videoStreamUrl(const QString &mxcUrl) {
    if (!_handle || _loggingOut || mxcUrl.isEmpty()) {
        return {};
    }
    const QByteArray url = mxcUrl.toUtf8();
    char *out = tm_video_stream_url(_handle, url.constData());
    if (!out) {
        return {};
    }
    const QString result = QString::fromUtf8(out);
    tm_free_string(out);
    return result;
}

float ProtocolBridge::videoStreamProgress(const QString &mxcUrl) {
    if (!_handle || _loggingOut || mxcUrl.isEmpty()) {
        return 1.0f;
    }
    const QByteArray url = mxcUrl.toUtf8();
    return tm_video_stream_progress(_handle, url.constData());
}

bool ProtocolBridge::videoStreamProgressBytes(
        const QString &mxcUrl, quint64 &downloaded, quint64 &total) {
    downloaded = 0;
    total = 0;
    if (!_handle || _loggingOut || mxcUrl.isEmpty()) {
        return false;
    }
    const QByteArray url = mxcUrl.toUtf8();
    // uint64_t is unsigned long on Linux/g++ but quint64 is unsigned long long,
    // so the FFI out-pointers need matching-typed temporaries.
    uint64_t outDownloaded = 0;
    uint64_t outTotal = 0;
    const bool ok = tm_video_stream_progress_bytes(
        _handle, url.constData(), &outDownloaded, &outTotal);
    downloaded = outDownloaded;
    total = outTotal;
    return ok;
}

bool ProtocolBridge::videoStreamErrored(const QString &mxcUrl) {
    if (!_handle || _loggingOut || mxcUrl.isEmpty()) {
        return false;
    }
    const QByteArray url = mxcUrl.toUtf8();
    return tm_video_stream_errored(_handle, url.constData());
}

VideoContainer ProtocolBridge::videoStreamContainer(const QString &mxcUrl) {
    if (!_handle || _loggingOut || mxcUrl.isEmpty()) {
        return VideoContainer::Unknown;
    }
    const QByteArray url = mxcUrl.toUtf8();
    switch (tm_video_stream_container(_handle, url.constData())) {
    case 1: return VideoContainer::Faststart;
    case 2: return VideoContainer::MoovAtEnd;
    default: return VideoContainer::Unknown;
    }
}

bool ProtocolBridge::keychainStore(const QString &key, const QString &value) {
    return tm_keychain_store(key.toUtf8().constData(), value.toUtf8().constData());
}

QString ProtocolBridge::keychainLoad(const QString &key, bool *readFailed) {
    bool failed = false;
    auto *result = tm_keychain_load(key.toUtf8().constData(), &failed);
    if (readFailed) {
        *readFailed = failed;
    }
    if (!result) return {};
    QString value = QString::fromUtf8(result);
    tm_free_string(result);
    return value;
}

void ProtocolBridge::keychainForgetCache() {
    tm_keychain_forget_cache();
}

bool ProtocolBridge::keychainDelete(const QString &key) {
    return tm_keychain_delete(key.toUtf8().constData());
}

void ProtocolBridge::keychainClearAll() {
    tm_keychain_clear_all();
}

void ProtocolBridge::secretStoreInit(const QString &dataDir, int backend) {
    tm_secret_store_init(dataDir.toUtf8().constData(), backend);
}

int ProtocolBridge::secretStoreState() {
    return tm_secret_store_state();
}

bool ProtocolBridge::secretServiceAvailable() {
    return tm_secret_service_available();
}

int ProtocolBridge::secretServiceStatus() {
    return tm_secret_service_status();
}

int ProtocolBridge::secretStoreUnlock(const QString &passphrase) {
    return tm_secret_store_unlock(passphrase.toUtf8().constData());
}

bool ProtocolBridge::secretStoreSetPassphrase(const QString &passphrase) {
    return tm_secret_store_set_passphrase(passphrase.toUtf8().constData());
}

bool ProtocolBridge::secretStoreSwitchBackend(int backend, const QString &passphrase) {
    // Empty passphrase => null pointer (the keychain target ignores it).
    const QByteArray pass = passphrase.toUtf8();
    return tm_secret_store_switch_backend(
        backend, passphrase.isEmpty() ? nullptr : pass.constData());
}

} // namespace TeleMatrix
