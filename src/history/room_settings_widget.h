// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QWidget>
#include <QVector>
#include "../protocol/protocol_types.h"

class QLabel;
class QLineEdit;
class QPushButton;
class QScrollArea;
class QStackedWidget;
class QVBoxLayout;

namespace TeleMatrix {

class AppController;
class ProtocolBridge;
class MembersListInner;

/// Room settings panel shown as a layer overlay.
/// Sections: General, Members, Security.
class RoomSettingsWidget : public QWidget {
    Q_OBJECT

public:
    explicit RoomSettingsWidget(
        const QString &roomId,
        AppController *controller,
        ProtocolBridge *bridge,
        QWidget *parent = nullptr);

    [[nodiscard]] QSize sizeHint() const override;
    void showMembersSection();

Q_SIGNALS:
    void closeRequested();
    void exportHistoryRequested(const QString &roomId);
    void leaveRoomRequested(const QString &roomId);
    void openUserProfileRequested(const QString &roomId, const QString &userId);

protected:
    void paintEvent(QPaintEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void leaveEvent(QEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;

private:
    enum class Section {
        General,
        Members,
        Security,
    };

    void setupSidebar();
    void setupGeneralPage();
    void setupMembersPage();
    void setupSecurityPage();
    void showSection(Section section);

    void paintTopBar(QPainter &p);

    void addSectionTitle(QWidget *parent, QVBoxLayout *layout, const QString &title);
    void addDivider(QWidget *parent, QVBoxLayout *layout);
    void addInfoRow(QWidget *parent, QVBoxLayout *layout,
                    const QString &label, const QString &value,
                    bool copyButton = false);
    void addActionRow(QWidget *parent, QVBoxLayout *layout,
                      const QString &text, const QString &iconName,
                      const std::function<void()> &callback,
                      bool attention = false);
    void showAccessOptions();
    void showHistoryVisibilityOptions();
    void updateMemberActions();
    void updateEncryptionSection();
    void updateAccessSection();
    void updateHistoryVisibilitySection();

    void rebuildMembersList();
    [[nodiscard]] int desiredBodyHeight() const;
    void onRoomSettingsReady(bool success, const RoomSettingsSnapshot &snap);
    void onRoomEncryptionEnabled(bool success);
    void onRoomAccessSet(const QString &roomId, bool success);
    void onRoomHistoryVisibilitySet(const QString &roomId, bool success);
    void onRoomNameSet(const QString &roomId, bool success);
    void onRoomTopicSet(const QString &roomId, bool success);
    void chooseAndUploadRoomAvatar();
    void deleteRoomAvatar();
    // Inline room-name commit (on the cover field losing focus / Enter).
    void commitRoomName(const QString &name);
    // Inline topic commit (on the textarea losing focus): optimistic update +
    // setRoomTopic, reverted on failure.
    void commitTopic(const QString &text);
    // Settle the topic textarea's text/placeholder + editability per the current
    // topic and whether the user may change it.
    void updateTopicRow();
    void onRoomAvatarUploaded(
        const QString &roomId,
        bool success,
        const QString &newAvatarUrl);
    void onRoomAvatarDeleted(const QString &roomId, bool success);

    AppController *_controller = nullptr;
    ProtocolBridge *_bridge = nullptr;
    QString _roomId;
    RoomSummary _roomSummary;
    QVector<RoomMemberInfo> _members;
    bool _membersSnapshotLoaded = false;

    Section _currentSection = Section::General;
    QStackedWidget *_stack = nullptr;

    // Sidebar buttons.
    QWidget *_sidebar = nullptr;
    struct SidebarButton {
        QWidget *widget = nullptr;
        Section section;
    };
    QVector<SidebarButton> _sidebarButtons;

    // Content pages.
    QWidget *_membersPage = nullptr;
    QWidget *_addMemberButton = nullptr;
    QLineEdit *_membersSearchField = nullptr;
    MembersListInner *_membersListInner = nullptr;
    QWidget *_membersPreloader = nullptr;
    bool _membersFullRefreshRequested = false;
    QWidget *_securityPage = nullptr;

    QRect _closeButtonRect;
    bool _closeHovered = false;

    // Colorized close (X) icon.
    QImage _closeIcon;
    QImage _closeIconOver;

    // Notifications selector (SettingsValueButton), updated on snapshot.
    QWidget *_notifButton = nullptr;

    // Security page labels (updated asynchronously).
    QLabel *_encryptionValue = nullptr;
    QLabel *_accessValue = nullptr;
    QLabel *_visibilityValue = nullptr;
    QLabel *_newMembersValue = nullptr;

    QWidget *_encryptionRow = nullptr;
    QWidget *_accessRow = nullptr;
    QWidget *_visibilityRow = nullptr;

    bool _roomIsEncrypted = false;
    bool _encryptionRequestInFlight = false;
    RoomAccess _roomAccess = RoomAccess::Unknown;
    RoomAccess _roomAccessBeforeRequest = RoomAccess::Unknown;
    RoomAccess _roomAccessPending = RoomAccess::Unknown;
    bool _accessRequestInFlight = false;
    bool _canInvite = false;
    bool _canChangeAvatar = false;
    bool _canChangeName = false;
    bool _canChangeTopic = false;
    bool _canChangeEncryption = false;
    bool _canChangeAccess = false;

    // General page: the room is an m.space (opened from a space rail tab). Only
    // affects presentation (Type row wording) — a space is otherwise a room.
    bool _isSpace = false;
    // Editable topic, shown as a read-only textarea (settings-keyword-box style);
    // click-to-edit when permitted. Settled in updateTopicRow. _topicRow is a
    // TopicDisplay.
    QWidget *_topicRow = nullptr;
    QString _topicBeforeEdit;
    bool _topicOpInFlight = false;

    HistoryVisibility _historyVisibility = HistoryVisibility::Unknown;
    HistoryVisibility _historyVisibilityBeforeRequest = HistoryVisibility::Unknown;
    HistoryVisibility _historyVisibilityPending = HistoryVisibility::Unknown;
    bool _historyVisibilityRequestInFlight = false;
    bool _canChangeHistoryVisibility = false;

    QWidget *_roomCover = nullptr;
    bool _roomAvatarOperationInFlight = false;
};

} // namespace TeleMatrix
