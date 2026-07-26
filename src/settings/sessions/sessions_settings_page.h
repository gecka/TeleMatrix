// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include "protocol/protocol_types.h"
#include "ui/rp_widget.h"

class QVBoxLayout;

namespace TeleMatrix {

class AppController;

class SessionsSettingsPage final : public ::Ui::RpWidget {
    Q_OBJECT

public:
    explicit SessionsSettingsPage(
        AppController *controller,
        QWidget *parent = nullptr);

    void refreshList();

    /// Confirm + sign out a single session (the "New login" banner's Sign out
    /// action). Reuses the standard interactive-auth delete flow.
    void requestSignOut(const QString &deviceId);

private:
    enum class SessionsFilter { All, Verified, Unverified, Inactive };

    void clearBody();
    void showLoading();
    void showError();
    void rebuildUi(const DeviceSessionList &list);
    void showActionPreloader(const QString &title, const QString &text);
    void hideActionPreloader();
    void beginDeleteSessions(
        const QStringList &deviceIds,
        const QString &title,
        const QString &description,
        const QString &confirmText);
    void onRenameSession(const QString &deviceId, const QString &currentName);
    void onTerminateAllOtherSessions();

    AppController *_controller = nullptr;
    QVBoxLayout *_rootLayout = nullptr;
    QStringList _pendingDeleteDeviceIds;
    QString _pendingDeletePassword;
    int _pendingDeleteAuthRetries = 0;
    /// Set when the homeserver manages the account on its own website, in which
    /// case sessions are removed there rather than through a password prompt.
    QString _accountManagementUrl;
    QWidget *_actionPreloader = nullptr;
    DeviceSessionList _lastDeviceList;
    bool _listLoaded = false;
    SessionsFilter _filter = SessionsFilter::All;
};

} // namespace TeleMatrix
