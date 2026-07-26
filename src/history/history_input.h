// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <functional>

#include <QImage>
#include <QTimer>
#include <QTextCharFormat>

#include "ui/rp_widget.h"

class QMouseEvent;

namespace TeleMatrix::HistoryPopupMenuStyle {
class PopupMenu;
} // namespace TeleMatrix::HistoryPopupMenuStyle
class QString;

namespace Ui {
class FormattedTextEdit;
} // namespace Ui

namespace TeleMatrix {

class AppController;
class HistoryEmojiPicker;
class HistoryVoiceRecorder;
class MentionAutocomplete;
struct UserProfile;

/// Compose icon button — custom-painted 44x46 icon button
/// with hover background and ripple area.
class ComposeIconButton : public QWidget {
    Q_OBJECT

public:
    enum Icon {
        Attach,     // Paperclip
        Emoji,      // Smiley face with circle outline
        BotCommand, // "/" in rounded square
        Record,     // Microphone
        Save,       // Checkmark/save icon (edit mode)
        Send,       // Paper plane / send arrow
    };

    ComposeIconButton(QWidget *parent, Icon icon);

    void setIcon(Icon icon);
    [[nodiscard]] Icon icon() const;
    void setRecordingState(bool active, bool sendOnRelease);

signals:
    void clicked();
    void pressed();
    void released(bool inside);
    void pointerMoved(bool inside);

protected:
    void paintEvent(QPaintEvent *e) override;
    void enterEvent(QEnterEvent *e) override;
    void leaveEvent(QEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;

private:
    void paintEmoji(QPainter &p, const QColor &fg);
    void paintIcon(QPainter &p, const QString &name, const QColor &fg);

    Icon _icon;
    bool _over = false;
    bool _pressed = false;
    bool _recordingActive = false;
    bool _recordingSendOnRelease = true;
};

/// Message input area with compose controls.
/// Layout: [attach] [input field] [emoji] [record/send]
class HistoryInput : public Ui::RpWidget {
    Q_OBJECT

public:
    explicit HistoryInput(
        AppController *controller,
        QWidget *parent = nullptr);
    ~HistoryInput() override;

    /// Apply ConvertScale to input field pixel constants (call once at startup).
    static void initInputPxValues();

    using SendCallback = std::function<void(
        const QString &text,
        const QString &formattedBody,
        const QString &replyToEventId)>;
    void setSendCallback(SendCallback callback);

    void clearInput();
    void focusInput();
    QString fieldText() const;
    QString fieldHtml() const;
    void setFieldText(const QString &text);
    void setFieldHtml(const QString &html);
    void insertMentionAtCursor(const QString &userId, const QString &displayName);
    void enterEditMode(
        const QString &eventId,
        const QString &senderName,
        const QString &body,
        const QString &formattedBody = QString());
    void cancelEditMode();
    bool isInEditMode() const;
    QString editEventId() const;
    QString editSenderName() const;
    QString editPreviewText() const;
    void enterReplyMode(
        const QString &eventId,
        const QString &senderName,
        const QString &body,
        const QString &quotedText = QString(),
        const QString &previewPath = QString());
    void cancelReplyMode();
    bool isInReplyMode() const;
    QString replyEventId() const;
    QString replySenderName() const;
    QString replyPreviewText() const;
    QString replyPreviewPath() const;

    void setRoomMembers(const QVector<UserProfile> &members);
    void refreshMentionPopup();
    void cancelVoiceRecording();

    /// Position and show an external emoji picker at the same location
    /// as the compose emoji panel (anchored to the emoji button).
    void positionAndShowPicker(HistoryEmojiPicker *picker);

signals:
    /// Emitted when the compose area height changes due to text content.
    void heightChanged();
    /// Emitted when a voice message was recorded and should be uploaded.
    void voiceRecorded(const QString &path, quint64 durationMs, const QByteArray &waveform);
    /// Emitted when the attach button is clicked (to show popup chooser).
    void attachPopupRequested();
    /// Emitted when compose content changes (for live draft previews).
    /// Carries only plain text: HTML is serialized lazily on room switch-away
    /// (HistoryDraftStore::capture), never per keystroke — toHtml() is slow.
    void contentChanged(const QString &text);
    /// Emitted when user submits an edit.
    void editSubmitted(
        const QString &eventId,
        const QString &text,
        const QString &formattedBody);
    /// Emitted when user presses Up in empty input to edit last own message.
    void editLastMessageRequested();
    /// Emitted when edit mode was cancelled.
    void editCancelled();
    /// Emitted when user tries to cancel edit but text was modified.
    void editCancelConfirmRequested();
    /// Emitted when Escape is pressed with nothing to cancel.
    void escapePressed();

protected:
    void paintEvent(QPaintEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    void moveEvent(QMoveEvent *e) override;
    bool eventFilter(QObject *obj, QEvent *event) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void leaveEvent(QEvent *e) override;

private:
    void updateControlsGeometry();
    void updateSendButton();
    void updateSendButton(const QString &text);
    void checkContentHeight();
    void send();
    bool canStartVoiceRecording() const;
    void startVoiceRecording();
    void finishVoiceRecording(bool send);
    void updateVoiceRecordingHotspot(const QPoint &globalPos);
    void updateVoiceRecordingDuration();
    void updateAppEventFilter();
    QString voiceRecordingDurationText() const;
    void showEmojiPicker();
    void showContextMenu(const QPoint &pos);
    HistoryPopupMenuStyle::PopupMenu *createStyledMenu(QWidget *parent);
    HistoryPopupMenuStyle::PopupMenu *createFormattingSubmenu(
        HistoryPopupMenuStyle::PopupMenu *parent);
    void toggleBold();
    void toggleItalic();
    void toggleUnderline();
    void toggleStrikethrough();
    void toggleMonospace();
    void toggleQuote();
    void clearFormatting();
	void updateDocumentMargin();
	void checkMentionTrigger();
	void repositionMentionPopup();
	void insertMention(const QString &userId, const QString &displayName);
	void insertRoomMention();
	bool canShowEmojiPicker() const;
	bool isGlobalPointOverEmojiPicker(const QPoint &global) const;
	void scheduleEmojiPickerHide(int delayMs);
	bool isPointerOverEmojiPicker() const;

    // Left button.
    ComposeIconButton *_attachButton = nullptr;

    // Input field (custom QTextEdit with blockquote decoration painting).
    Ui::FormattedTextEdit *_field = nullptr;

    // Right buttons.
    ComposeIconButton *_emojiPickerButton = nullptr;
    ComposeIconButton *_recordButton = nullptr;
    HistoryEmojiPicker *_emojiPicker = nullptr;
    HistoryVoiceRecorder *_voiceRecorder = nullptr;

    SendCallback _sendCallback;

    // The default char format set at construction — used to detect
    // user-applied formatting in the QTextEdit (bold, italic, etc.).
    QTextCharFormat _defaultCharFormat;

    // Base document margin (without code block header).
    int _baseDocMargin = 0;

    // Whether the placeholder is currently suppressed by block formatting;
    // avoids per-keystroke tr() + setPlaceholderText churn.
    bool _placeholderHidden = false;

    bool _editMode = false;
    QString _editEventId;
    QString _editSenderName;
    QString _editPreviewText;
    QString _editOriginalBody;  // plain text at edit start, for dirty check
    bool _replyMode = false;
    QString _replyEventId;
    QString _replySenderName;
    QString _replyPreviewText;
    QString _replyPreviewPath;
    QImage _replyPreviewImage;
    QRect _editCancelRect;

	MentionAutocomplete *_mentionAutocomplete = nullptr;
	int _mentionCursorStart = -1; // position of the '@' triggering autocomplete
	AppController *_controller = nullptr;
	QTimer _emojiPickerHideTimer;
	bool _emojiPickerSuppressHoverOpen = false;
	bool _emojiPickerGlobalTracking = false;
    bool _appEventFilterInstalled = false;
    bool _voiceRecording = false;
    bool _voiceRecordingSendOnRelease = true;
    QTimer _voiceRecordingTimer;
};

} // namespace TeleMatrix
