// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include "protocol/protocol_types.h"

#include <QWidget>

class QVBoxLayout;

namespace Ui {
class ScrollArea;
}

namespace TeleMatrix {

class AccountProfileCover;
class AppController;
namespace Core { class AccountSettings; }

class AccountSettingsPage final : public QWidget {
    Q_OBJECT

public:
    AccountSettingsPage(
        AppController *controller,
        Core::AccountSettings *settings,
        QWidget *parent = nullptr);

    void prepareForShow();
    void refreshData(bool force = false);

Q_SIGNALS:
    void logoutRequested();

private:
    void onAccountSummaryReady(
        bool success,
        const AccountSummary &summary,
        const QString &error);
    void rebuildSections();
    void editDisplayName();
    void chooseAndUploadAvatar();
    void deleteAvatar();
    void setAvatarOperationInFlight(bool inFlight);
    [[nodiscard]] QString relevantThreepidErrorText(
        ThreePidMedium medium,
        const QString &error) const;

    AppController *_controller = nullptr;
    Core::AccountSettings *_settings = nullptr;
    /// Contact details start collapsed — the page is rarely opened for them.
    bool _threepidsExpanded = false;
    AccountProfileCover *_cover = nullptr;
    ::Ui::ScrollArea *_scrollArea = nullptr;
    QWidget *_sections = nullptr;
    QVBoxLayout *_sectionsLayout = nullptr;

    AccountSummary _accountSummary;
    bool _accountSummaryLoaded = false;
    bool _accountSummaryInFlight = false;
    bool _avatarOperationInFlight = false;
    QVector<ThreePid> _threepids;
    bool _threepidsLoaded = false;
    bool _threepidsInFlight = false;
    QString _pendingNewPassword;
    QString _pending3pidClientSecret;
    QString _pending3pidSid;
    ThreePidMedium _pending3pidMedium = ThreePidMedium::Email;
    QString _email3pidError;
    QString _phone3pidError;
    /// Whether the homeserver accepts 3PID changes from a client at all. False on
    /// delegated-auth (OIDC/MAS) servers, which manage email/phone themselves.
    [[nodiscard]] bool canChange3pid() const;
    /// A delegated-auth homeserver that owns email/phone and exposes a management website. When
    /// true the per-medium "can't verify … so none can be added" notes are redundant — only the
    /// "manages on its website" note is shown.
    [[nodiscard]] bool managesThreepidsExternally() const;
    void addEmailSection();
    void addPhoneSection();
    /// Rows for the other signed-in accounts plus "Add Account", shown while the
    /// cover's chevron is expanded.
    /// Replaces both sections when the server manages 3PIDs itself: one
    /// explanation, plus a link to the page that can actually change them.
    void addAccountManagementNote();

    // Delegated-auth (OIDC/MAS) homeservers disable the 3PID API outright; email
    // and phone are then only reachable on the server's account website.
    QString _accountManagementUrl;
    bool _accountManagementInFlight = false;
    // Latched when the homeserver says it can't verify this medium: the add form is
    // then useless and is replaced by the explanation. Email is settled up front by
    // a probe (see AppController::emailVerificationSupported) and, failing that, by
    // the error a token request comes back with.
    bool _emailVerificationUnsupported = false;
    // Phone starts out unsupported because it almost always is, and unlike email it
    // cannot be asked about: a homeserver checks its SMS delegate only *after* the
    // point where it would have sent the message, so there is no way to probe
    // without texting someone. Sending at all requires an account_threepid_delegate
    // (Sydent), which is deprecated and effectively never deployed — no homeserver
    // can send an SMS on its own. Numbers already bound to the account still show.
    bool _phoneVerificationUnsupported = true;
};

} // namespace TeleMatrix
