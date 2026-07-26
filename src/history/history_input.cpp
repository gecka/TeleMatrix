// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "history_input.h"
#include "history_draft_state.h"
#include "history_emoji_picker.h"
#include "history_voice_recorder.h"
#include "mention_autocomplete.h"
#include "mention_trigger.h"
#include "compose_html.h"
#include "../app/app_controller.h"
#include "../core/core_settings.h"
#include "../ui/input_submit_settings.h"

#include <QAbstractTextDocumentLayout>
#include <QTextDocument>
#include <QApplication>
#include <QClipboard>
#include <QCursor>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QHash>
#include <QImage>
#include <QKeyEvent>
#include <QMoveEvent>
#include "history/history_popup_menu_style.h"
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QScreen>
#include <QScrollBar>
#include <QWindow>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextFormat>
#include <QTextCursor>
#include <QTextFragment>

#include "history_popup_menu_style.h"
#include "protocol/media_cache.h"
#include "ui/painter.h"
#include "ui/style/icon_provider.h"
#include "ui/style/runtime_scale.h"
#include "ui/widgets/formatted_text_edit.h"
#include "styles/style_constants.h"

namespace TeleMatrix {

namespace {

// Attach button size (runtime-scaled).
inline int kButtonWidth = 44;
inline int kButtonHeight = 46;

// Send button right offset = 2px.
inline int kSendRight = 2;
// Send button padding = 9px.
inline int kSendPadding = 9;

// Compose field height constraints.
inline int kFieldHeightMin = 28;  // kButtonHeight - 2 * kSendPadding
inline int kFieldHeightMax = 224;

// Edit/reply-style compose bar (runtime-scaled).
#define kEditBarHeight st::historyReplyHeight
constexpr qreal kReplyPreviewRadius = 3.0;

// kMentionUserIdProperty / kRoomMentionProperty live in compose_html.h — shared
// with the formatted-body serializer that reads them back.

// Ripple area for buttons = 40px, offset (2, 3).
inline int kRippleSize = 40;
// Emoji button circle size.
inline int kEmojiCircleSize = 20;
constexpr qreal kEmojiCircleLine = 1.5;
constexpr quint64 kVoiceRecordMinDurationMs = 200;
constexpr quint64 kVoiceRecordMaxDurationMs = 100 * 60 * 1000;

void applyInputScale() {
    using TeleMatrix::Style::ConvertScale;
    kButtonWidth = ConvertScale(44);
    kButtonHeight = ConvertScale(46);
    kSendRight = ConvertScale(2);
    kSendPadding = ConvertScale(9);
    kFieldHeightMin = kButtonHeight - 2 * kSendPadding;
    kFieldHeightMax = ConvertScale(224);
    kRippleSize = ConvertScale(40);
    kEmojiCircleSize = ConvertScale(20);
}

void drawChatIcon(
        QPainter &p,
        const QString &name,
        qreal /*dpr*/,
        const QColor &color,
        QPoint pos) {
    const auto icon = TeleMatrix::Style::IconProvider::tintedIcon(
        QStringLiteral(":/telematrix/icons/chat/"), name, color);
    if (icon.isNull()) {
        return;
    }
    p.drawImage(pos, icon);
}


/// Ensure selection boundaries align with block (paragraph) boundaries.
/// If the selection starts or ends mid-block, insert paragraph separators
/// to split the block so that block-level formatting only affects the
/// selected lines.  Returns updated (selStart, selEnd).
std::pair<int,int> ensureBlockBoundaries(Ui::FormattedTextEdit *field, QTextCursor &c) {
    int selStart = c.selectionStart();
    int selEnd = c.selectionEnd();
    auto *doc = field->document();

    c.beginEditBlock();

    // Split at the end of selection if mid-block.
    // If the character at selEnd is a line separator (Shift+Enter),
    // replace it with a block separator to avoid an empty line.
    {
        auto endBlock = doc->findBlock(selEnd);
        if (endBlock.isValid()
            && selEnd > endBlock.position()
            && selEnd < endBlock.position() + endBlock.length() - 1) {
            QTextCursor split(doc);
            split.setPosition(selEnd);
            if (doc->characterAt(selEnd) == QChar::LineSeparator) {
                split.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor, 1);
                split.removeSelectedText();
                split.setPosition(selEnd);
            }
            split.insertBlock();
        }
    }

    // Split at the start of selection if mid-block.
    // If the character before selStart is a line separator, replace it
    // with a block separator to avoid an empty line.
    {
        auto startBlock = doc->findBlock(selStart);
        if (startBlock.isValid() && selStart > startBlock.position()) {
            QTextCursor split(doc);
            if (selStart > 0
                && doc->characterAt(selStart - 1) == QChar::LineSeparator) {
                split.setPosition(selStart - 1);
                split.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor, 1);
                split.removeSelectedText();
                split.insertBlock();
                selStart = split.position();
                // Removed 1 char + inserted 1 block sep = net 0 for selEnd.
            } else {
                split.setPosition(selStart);
                split.insertBlock();
                selStart = split.position();
                selEnd += 1;
            }
        }
    }

    c.endEditBlock();

    c.setPosition(selStart);
    c.setPosition(selEnd, QTextCursor::KeepAnchor);
    field->setTextCursor(c);

    return {selStart, selEnd};
}

[[nodiscard]] QRect centeredCropRect(const QSize &source, const QSize &target) {
    if (source.width() <= 0
        || source.height() <= 0
        || target.width() <= 0
        || target.height() <= 0) {
        return QRect();
    }

    const auto targetW = target.width();
    const auto targetH = target.height();
    const auto sourceW = source.width();
    const auto sourceH = source.height();

    if (sourceW * targetH > sourceH * targetW) {
        const auto cropW = qMax(1, (sourceH * targetW) / targetH);
        const auto left = (sourceW - cropW) / 2;
        return QRect(left, 0, cropW, sourceH);
    }
    if (sourceW * targetH < sourceH * targetW) {
        const auto cropH = qMax(1, (sourceW * targetH) / targetW);
        const auto top = (sourceH - cropH) / 2;
        return QRect(0, top, sourceW, cropH);
    }
    return QRect(0, 0, sourceW, sourceH);
}

} // namespace

void HistoryInput::initInputPxValues() {
    applyInputScale();
}

// ─── ComposeIconButton ────────────────────────────────────

ComposeIconButton::ComposeIconButton(QWidget *parent, Icon icon)
    : QWidget(parent)
    , _icon(icon)
{
    setFixedSize(kButtonWidth, kButtonHeight);
    setCursor(Qt::PointingHandCursor);
    setAttribute(Qt::WA_OpaquePaintEvent, false);
}

void ComposeIconButton::paintEvent(QPaintEvent *) {
    QPainter p(this);
    PainterHighQualityEnabler hq(p);

    if (_recordingActive && _icon == Record) {
        p.setPen(Qt::NoPen);
        p.setBrush(_recordingSendOnRelease
            ? st::historyRecordVoiceFgActive
            : st::historyRecordVoiceFgInactive);
        const auto radius = st::historyRecordMainBlobMinRadius;
        p.drawEllipse(QPointF(width() / 2.0, height() / 2.0), radius, radius);
        paintIcon(p, QStringLiteral("input_record"), st::historyRecordVoiceFgActiveIcon);
        return;
    }

    const auto &composeFg = _over
        ? st::historyComposeIconFgOver
        : st::historyComposeIconFg;

    switch (_icon) {
    case Attach:
        paintIcon(p, QStringLiteral("input_attach"), composeFg);
        break;
    case Emoji:
        paintEmoji(p, composeFg);
        break;
    case BotCommand:
        paintIcon(p, QStringLiteral("input_bot_command"), composeFg);
        break;
    case Record:
        paintIcon(
            p,
            QStringLiteral("input_record"),
            _over ? st::historyRecordVoiceFgOver : st::historyRecordVoiceFg);
        break;
    case Save:
        paintIcon(
            p,
            QStringLiteral("input_save"),
            _over ? st::historySendIconFgOver : st::historySendIconFg);
        break;
    case Send:
        paintIcon(
            p,
            QStringLiteral("input_send"),
            _over ? st::historySendIconFgOver : st::historySendIconFg);
        break;
    }
}

void ComposeIconButton::paintEmoji(QPainter &p, const QColor &fg) {
    paintIcon(p, QStringLiteral("input_smile_face"), fg);

    // Emoji button circle.
    const auto cx = width() / 2.0;
    const auto cy = height() / 2.0;
    const auto r = kEmojiCircleSize / 2.0;
    p.setPen(QPen(fg, kEmojiCircleLine));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(QPointF(cx, cy), r, r);
}

void ComposeIconButton::setIcon(Icon icon) {
    if (_icon != icon) {
        _icon = icon;
        update();
    }
}

ComposeIconButton::Icon ComposeIconButton::icon() const {
    return _icon;
}

void ComposeIconButton::setRecordingState(bool active, bool sendOnRelease) {
    if (_recordingActive == active
        && _recordingSendOnRelease == sendOnRelease) {
        return;
    }
    _recordingActive = active;
    _recordingSendOnRelease = sendOnRelease;
    const auto recordingDiameter = 2 * st::historyRecordMainBlobMinRadius;
    const QSize targetSize(
        active ? qMax(kButtonWidth, recordingDiameter) : kButtonWidth,
        active ? qMax(kButtonHeight, recordingDiameter) : kButtonHeight);
    if (size() != targetSize) {
        setFixedSize(targetSize);
    }
    if (active) {
        raise();
    }
    update();
}

void ComposeIconButton::paintIcon(QPainter &p, const QString &name, const QColor &fg) {
    const auto icon = TeleMatrix::Style::IconProvider::tintedIcon(
        QStringLiteral(":/telematrix/icons/chat/"), name, fg);
    if (icon.isNull()) {
        return;
    }
    const auto iconW = int(icon.width() / icon.devicePixelRatio());
    const auto iconH = int(icon.height() / icon.devicePixelRatio());
    const auto x = (width() - iconW) / 2;
    const auto y = (height() - iconH) / 2;
    p.drawImage(QPoint(x, y), icon);
}

void ComposeIconButton::enterEvent(QEnterEvent *) {
    _over = true;
    update();
}

void ComposeIconButton::leaveEvent(QEvent *) {
    _over = false;
    update();
}

void ComposeIconButton::mousePressEvent(QMouseEvent *e) {
    if (e->button() == Qt::LeftButton) {
        _pressed = true;
        emit pressed();
        e->accept();
        return;
    }
    QWidget::mousePressEvent(e);
}

void ComposeIconButton::mouseMoveEvent(QMouseEvent *e) {
    if (_pressed) {
        emit pointerMoved(rect().contains(e->pos()));
    }
    QWidget::mouseMoveEvent(e);
}

void ComposeIconButton::mouseReleaseEvent(QMouseEvent *e) {
    if (e->button() == Qt::LeftButton && _pressed) {
        const auto inside = rect().contains(e->pos());
        _pressed = false;
        emit released(inside);
        if (inside) {
            emit clicked();
        }
        e->accept();
        return;
    }
    QWidget::mouseReleaseEvent(e);
}

// ─── HistoryInput ─────────────────────────────────────────

HistoryInput::HistoryInput(
    AppController *controller,
    QWidget *parent)
    : Ui::RpWidget(parent)
    , _controller(controller)
{
    setMouseTracking(true);

    // Ensure white compose area background.
    setAutoFillBackground(true);
    QPalette pal = palette();
    pal.setColor(QPalette::Window, st::historyComposeAreaBg);
    setPalette(pal);

    // Left button.
    _attachButton = new ComposeIconButton(this, ComposeIconButton::Attach);

    // Right buttons.
    _emojiPickerButton = new ComposeIconButton(this, ComposeIconButton::Emoji);
    _recordButton = new ComposeIconButton(this, ComposeIconButton::Record);
    _voiceRecorder = new HistoryVoiceRecorder(this);

    // Input field — custom QTextEdit with blockquote decoration painting.
    _field = new Ui::FormattedTextEdit(this);
    _field->setPlaceholderText(tr("Message..."));
    _field->setFrameShape(QFrame::NoFrame);
    _field->setAcceptRichText(true);
    _field->setTabChangesFocus(true);
    _field->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    _field->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // Set colors explicitly via QPalette to avoid macOS dark mode override.
    {
        QPalette fieldPal = _field->palette();
        fieldPal.setColor(QPalette::Base, st::historyComposeAreaBg);
        fieldPal.setColor(QPalette::Text, st::historyComposeAreaFg);
        fieldPal.setColor(QPalette::PlaceholderText, st::placeholderFg);
        fieldPal.setColor(QPalette::Highlight, st::msgInBgSelected);
        fieldPal.setColor(QPalette::HighlightedText, st::historyComposeAreaFg);
        _field->setPalette(fieldPal);

        const auto f = st::baseFont(st::fsize);
        _field->setFont(f);

        // Set default text color for new text via char format.
        // Save as _defaultCharFormat for rich-format detection in send().
        _defaultCharFormat = QTextCharFormat();
        _defaultCharFormat.setFont(f);
        _defaultCharFormat.setForeground(st::historyComposeAreaFg);
        _field->setCurrentCharFormat(_defaultCharFormat);

        // Document margin = 4px.
        const auto topPad = 4;
        _baseDocMargin = topPad;
        _field->document()->setDocumentMargin(topPad);
    }

    // Refresh colors when theme changes (day/night toggle).
    if (_controller) {
        if (auto *tm = _controller->themeManager()) {
            QObject::connect(tm, &Theme::ThemeManager::themeChanged,
                    this, [this](bool /*isNight*/, Theme::ThemeMode /*mode*/) {
                // Refresh own background palette.
                QPalette p = palette();
                p.setColor(QPalette::Window, st::historyComposeAreaBg);
                setPalette(p);

                // Refresh field palette with fresh theme colors.
                QPalette fieldPal = _field->palette();
                fieldPal.setColor(QPalette::Base, st::historyComposeAreaBg);
                fieldPal.setColor(QPalette::Text, st::historyComposeAreaFg);
                fieldPal.setColor(QPalette::PlaceholderText, st::placeholderFg);
                fieldPal.setColor(QPalette::Highlight, st::msgInBgSelected);
                fieldPal.setColor(QPalette::HighlightedText, st::historyComposeAreaFg);
                _field->setPalette(fieldPal);

                // Refresh default char format foreground.
                _defaultCharFormat.setForeground(st::historyComposeAreaFg);
                _field->setCurrentCharFormat(_defaultCharFormat);

                update();
            });
        }
    }

    // Handle Enter to send and formatting shortcuts via event filter.
    _field->installEventFilter(this);

    // Custom right-click context menu.
    _field->setContextMenuPolicy(Qt::CustomContextMenu);
    QObject::connect(_field, &QWidget::customContextMenuRequested,
        this, [this](const QPoint &pos) {
            showContextMenu(_field->mapToGlobal(pos));
        });

    // Toggle record ↔ send icon and auto-resize when text changes.
    QObject::connect(_field, &QTextEdit::textChanged,
        this, [this] {
            static const bool typeStats
                = qEnvironmentVariableIsSet("TM_TYPE_STATS");
            QElapsedTimer timer;
            if (typeStats) {
                timer.start();
            }
            const auto text = _field->toPlainText();   // the ONE extraction
            updateSendButton(text);
            checkContentHeight();
            checkMentionTrigger();
            emit contentChanged(text);
            if (typeStats && timer.nsecsElapsed() > 500'000) {
                qWarning("composer hub: %.2f ms (doc %d chars, %d blocks)",
                    timer.nsecsElapsed() / 1e6,
                    _field->document()->characterCount(),
                    _field->document()->blockCount());
            }
        });

    // Also check height after document layout changes (blockquote/code toggles).
    QObject::connect(_field->document()->documentLayout(),
        &QAbstractTextDocumentLayout::documentSizeChanged,
        this, [this] { checkContentHeight(); });

    QObject::connect(_voiceRecorder, &HistoryVoiceRecorder::failed,
        this, [this](const QString &) {
            if (_voiceRecording) {
                finishVoiceRecording(false);
            }
        });

    _voiceRecordingTimer.setInterval(100);
    QObject::connect(&_voiceRecordingTimer, &QTimer::timeout,
        this, &HistoryInput::updateVoiceRecordingDuration);

    QObject::connect(_recordButton, &ComposeIconButton::pressed,
        this, [this] {
            if (canStartVoiceRecording()) {
                startVoiceRecording();
            }
        });
    QObject::connect(_recordButton, &ComposeIconButton::pointerMoved,
        this, [this](bool inside) {
            if (_voiceRecording) {
                _voiceRecordingSendOnRelease = inside;
                _recordButton->setRecordingState(true, inside);
                update();
            }
        });
    QObject::connect(_recordButton, &ComposeIconButton::released,
        this, [this](bool inside) {
            if (_voiceRecording) {
                finishVoiceRecording(inside);
                return;
            }
            if (inside) {
                send();
            }
        });

    // Attach button emits signal for the popup chooser.
    QObject::connect(_attachButton, &ComposeIconButton::clicked,
        this, [this] {
            emit attachPopupRequested();
        });

	QObject::connect(_emojiPickerButton, &ComposeIconButton::clicked,
		this, [this] {
			_emojiPickerSuppressHoverOpen = false;
			_emojiPickerHideTimer.stop();
			showEmojiPicker();
		});

	_emojiPickerButton->installEventFilter(this);
    _emojiPickerHideTimer.setSingleShot(true);
	QObject::connect(&_emojiPickerHideTimer, &QTimer::timeout, this, [this] {
		if (_emojiPicker && _emojiPicker->isVisible() && !isPointerOverEmojiPicker()) {
			_emojiPicker->hide();
		}
	});
	QObject::connect(qApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
        if (_voiceRecording && state != Qt::ApplicationActive) {
            finishVoiceRecording(false);
            return;
        }
		if (state == Qt::ApplicationActive) {
			if (!_emojiPickerButton
				|| !_emojiPickerButton->rect().contains(
					_emojiPickerButton->mapFromGlobal(QCursor::pos()))) {
				_emojiPickerSuppressHoverOpen = false;
			}
			return;
		}
		_emojiPickerSuppressHoverOpen = true;
		_emojiPickerHideTimer.stop();
	});

	// Initial height: the drawn field area around the input:
	// field height + 2 * kSendPadding.
	setMinimumHeight(kFieldHeightMin + 2 * kSendPadding);
    setMaximumHeight(kFieldHeightMax + 2 * kSendPadding + kEditBarHeight);
    setFixedHeight(kFieldHeightMin + 2 * kSendPadding);
}

HistoryInput::~HistoryInput() {
    cancelVoiceRecording();
    if (_appEventFilterInstalled) {
        qApp->removeEventFilter(this);
        _appEventFilterInstalled = false;
    }
}

void HistoryInput::setSendCallback(SendCallback callback) {
    _sendCallback = std::move(callback);
}

void HistoryInput::clearInput() {
    if (_field) {
        _field->clear();
        // Reset char format to the saved default so next typed text is plain.
        _field->setCurrentCharFormat(_defaultCharFormat);
    }
}

void HistoryInput::focusInput() {
    if (_field) {
        _field->setFocus();
    }
}

QString HistoryInput::fieldText() const {
    return _field ? _field->toPlainText() : QString();
}

QString HistoryInput::fieldHtml() const {
    return _field ? _field->toHtml() : QString();
}

void HistoryInput::setFieldText(const QString &text) {
    if (!_field) {
        return;
    }
    _field->setPlainText(text);
    auto cursor = _field->textCursor();
    cursor.movePosition(QTextCursor::End);
    _field->setTextCursor(cursor);
    updateSendButton();
    checkContentHeight();
}

void HistoryInput::setFieldHtml(const QString &html) {
    if (!_field) {
        return;
    }
    _field->setHtml(html);

    // Fix pre block margins: Qt's HTML parser sets nonBreakableLines but
    // doesn't add the top/bottom margins needed for the decoration header.
    // The keyboard shortcut path (togglePreBlock) sets these explicitly.
    {
        const auto &preStyle = st::historyPreStyle;
        auto *doc = _field->document();
        auto block = doc->begin();
        while (block.isValid()) {
            if (block.blockFormat().nonBreakableLines()) {
                // Find the first block in this pre group.
                const bool isFirstInGroup = !block.previous().isValid()
                    || !block.previous().blockFormat().nonBreakableLines();
                auto bfmt = block.blockFormat();
                bfmt.setTopMargin(isFirstInGroup
                    ? (preStyle.header + 2 * preStyle.verticalSkip) : 0);
                bfmt.setBottomMargin(2 * preStyle.verticalSkip);
                QTextCursor blockCursor(block);
                blockCursor.setBlockFormat(bfmt);
            }
            block = block.next();
        }
    }

    auto cursor = _field->textCursor();
    cursor.movePosition(QTextCursor::End);
    _field->setTextCursor(cursor);
    updateSendButton();
    checkContentHeight();
}

void HistoryInput::insertMentionAtCursor(const QString &userId, const QString &displayName) {
    if (!_field) {
        return;
    }

    auto cursor = _field->textCursor();
    if (!_field->hasFocus()) {
        cursor.movePosition(QTextCursor::End);
    }
    if (cursor.hasSelection()) {
        cursor.removeSelectedText();
    }

    QTextCharFormat mentionFmt = _defaultCharFormat;
    mentionFmt.setForeground(st::historyLinkInFg);
    mentionFmt.setProperty(kMentionUserIdProperty, userId);
    cursor.insertText(QStringLiteral("@") + displayName, mentionFmt);
    cursor.insertText(QStringLiteral(" "), _defaultCharFormat);

    _field->setTextCursor(cursor);
    if (_mentionAutocomplete && _mentionAutocomplete->isVisible()) {
        _mentionAutocomplete->hide();
    }
    _mentionCursorStart = -1;
    _field->setFocus();
}

void HistoryInput::enterEditMode(
    const QString &eventId,
    const QString &senderName,
    const QString &body,
    const QString &formattedBody) {
    _replyMode = false;
    _replyEventId.clear();
    _replySenderName.clear();
    _replyPreviewText.clear();
    _replyPreviewPath.clear();
    _replyPreviewImage = QImage();

    _editMode = true;
    _editEventId = eventId;
    _editSenderName = senderName;
    _editPreviewText = body;

    if (!formattedBody.isEmpty()) {
        setFieldHtml(formattedBody);
    } else {
        _field->setPlainText(body);
    }
    // Store original text for dirty check on cancel.
    _editOriginalBody = _field->toPlainText();
    auto cursor = _field->textCursor();
    cursor.movePosition(QTextCursor::End);
    _field->setTextCursor(cursor);
    _field->setFocus();
    updateSendButton();
    checkContentHeight();
    update();
}

void HistoryInput::cancelEditMode() {
    if (!_editMode) {
        return;
    }
    _editMode = false;
    _editEventId.clear();
    _editSenderName.clear();
    _editPreviewText.clear();
    _editCancelRect = QRect();
    clearInput();
    checkContentHeight();
    update();
    emit editCancelled();
}

bool HistoryInput::isInEditMode() const {
    return _editMode;
}

QString HistoryInput::editEventId() const {
    return _editEventId;
}

QString HistoryInput::editSenderName() const {
    return _editSenderName;
}

QString HistoryInput::editPreviewText() const {
    return _editPreviewText;
}

void HistoryInput::enterReplyMode(
    const QString &eventId,
    const QString &senderName,
    const QString &body,
    const QString &quotedText,
    const QString &previewPath) {
    _editMode = false;
    _editEventId.clear();
    _editSenderName.clear();
    _editPreviewText.clear();

    _replyMode = true;
    _replyEventId = eventId;
    _replySenderName = senderName;
    _replyPreviewText = body;
    _replyPreviewPath = previewPath;
    _replyPreviewImage = _replyPreviewPath.isEmpty()
        ? QImage()
        : MediaCache::loadImage(_replyPreviewPath);

    if (!quotedText.isEmpty()) {
        _field->setPlainText(QStringLiteral("> %1\n\n").arg(quotedText));
    } else {
        _field->clear();
    }
    auto cursor = _field->textCursor();
    cursor.movePosition(QTextCursor::End);
    _field->setTextCursor(cursor);
    _field->setFocus();
    updateSendButton();
    checkContentHeight();
    update();
}

void HistoryInput::cancelReplyMode() {
    if (!_replyMode) {
        return;
    }
    _replyMode = false;
    _replyEventId.clear();
    _replySenderName.clear();
    _replyPreviewText.clear();
    _replyPreviewPath.clear();
    _replyPreviewImage = QImage();
    _editCancelRect = QRect();
    clearInput();
    checkContentHeight();
    update();
}

bool HistoryInput::isInReplyMode() const {
    return _replyMode;
}

QString HistoryInput::replyEventId() const {
    return _replyEventId;
}

QString HistoryInput::replySenderName() const {
    return _replySenderName;
}

QString HistoryInput::replyPreviewText() const {
    return _replyPreviewText;
}

QString HistoryInput::replyPreviewPath() const {
    return _replyPreviewPath;
}

bool HistoryInput::eventFilter(QObject *obj, QEvent *event) {
    if (_voiceRecording) {
        if (event->type() == QEvent::KeyPress) {
            const auto *ke = static_cast<QKeyEvent*>(event);
            if (ke->key() == Qt::Key_Escape) {
                finishVoiceRecording(false);
                return true;
            }
        } else if (event->type() == QEvent::MouseMove
            || event->type() == QEvent::MouseButtonPress
            || event->type() == QEvent::MouseButtonRelease) {
            const auto *me = static_cast<QMouseEvent*>(event);
            updateVoiceRecordingHotspot(me->globalPosition().toPoint());
        } else if (event->type() == QEvent::ApplicationDeactivate
            || event->type() == QEvent::WindowDeactivate) {
            finishVoiceRecording(false);
            return false;
        }
    }

	if (_emojiPickerGlobalTracking
		&& _emojiPicker
		&& _emojiPicker->isVisible()) {
		if (event->type() == QEvent::MouseMove
			|| event->type() == QEvent::MouseButtonPress) {
			const auto *me = static_cast<QMouseEvent*>(event);
			const auto global = me->globalPosition().toPoint();
			if (!isGlobalPointOverEmojiPicker(global)) {
				_emojiPickerHideTimer.stop();
				_emojiPicker->hide();
			}
		} else if (event->type() == QEvent::ApplicationDeactivate) {
			_emojiPickerHideTimer.stop();
			_emojiPicker->hide();
		}
	}
	if (obj == _emojiPickerButton) {
		if (event->type() == QEvent::Enter) {
			_emojiPickerHideTimer.stop();
			if (!_emojiPickerSuppressHoverOpen) {
				showEmojiPicker();
			}
		} else if (event->type() == QEvent::Leave) {
			_emojiPickerSuppressHoverOpen = false;
			scheduleEmojiPickerHide(150);
		}
		return false;
	} else if (obj == _emojiPicker) {
		if (event->type() == QEvent::KeyPress) {
			// The picker is a Qt::Popup and grabs the keyboard app-wide, so while
			// it's open keys reach it, not the composer (unless its search field
			// has focus, in which case they never arrive here). Route everything
			// but Escape back to the message field: close the panel and replay the
			// keystroke, so Enter sends + closes and any character keeps typing
			// where you left off.
			auto *ke = static_cast<QKeyEvent *>(event);
			if (ke->key() != Qt::Key_Escape) {
				_emojiPickerHideTimer.stop();
				_emojiPicker->hide();
				if (_field) {
					_field->setFocus();
					QCoreApplication::postEvent(_field, new QKeyEvent(
						QEvent::KeyPress,
						ke->key(),
						ke->modifiers(),
						ke->text(),
						ke->isAutoRepeat(),
						ke->count()));
				}
				return true;
			}
		} else if (event->type() == QEvent::Enter) {
			_emojiPickerHideTimer.stop();
		} else if (event->type() == QEvent::Leave) {
			scheduleEmojiPickerHide(300);
		} else if (event->type() == QEvent::WindowDeactivate) {
			_emojiPickerSuppressHoverOpen = true;
			_emojiPickerHideTimer.stop();
		} else if (event->type() == QEvent::Hide) {
			if (_emojiPickerGlobalTracking) {
				_emojiPickerGlobalTracking = false;
                updateAppEventFilter();
			}
			_emojiPickerHideTimer.stop();
			// Reset emoji button hover: the popup stole mouse tracking,
			// so leaveEvent may not have fired on the button.
            if (_emojiPickerButton) {
                const auto global = QCursor::pos();
                const auto local = _emojiPickerButton->mapFromGlobal(global);
                if (!_emojiPickerButton->rect().contains(local)) {
                    // Synthesize a leave so the button repaints in default color.
                    QEvent leave(QEvent::Leave);
                    QCoreApplication::sendEvent(_emojiPickerButton, &leave);
                }
            }
		}
		return false;
	}
    if (obj == _field && event->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(event);
        const auto key = ke->key();
        const auto mod = ke->modifiers();

        // Mention autocomplete key handling — must come before Enter/send.
        if (_mentionAutocomplete && _mentionAutocomplete->isVisible()) {
            if (key == Qt::Key_Up) {
                _mentionAutocomplete->moveSelection(-1);
                return true;
            }
            if (key == Qt::Key_Down) {
                _mentionAutocomplete->moveSelection(1);
                return true;
            }
            if (key == Qt::Key_Return || key == Qt::Key_Enter
                || key == Qt::Key_Tab) {
                if (_mentionAutocomplete->chooseSelected()) {
                    return true;
                }
            }
            if (key == Qt::Key_Escape) {
                _mentionAutocomplete->hide();
                _mentionCursorStart = -1;
                return true;
            }
        }

        // Submit handling: respects the user's send-by-enter preference.
        {
            const auto submitSetting = _controller
                ? static_cast<InputSubmitSettings>(_controller->settings().sendSubmitWay())
                : InputSubmitSettings::Enter;
            if (ShouldSubmit(key, mod, submitSetting)) {
                send();
                return true;
            }
        }

        if (key == Qt::Key_Escape && (_editMode || _replyMode)) {
            if (_editMode) {
                // If text was modified, ask for confirmation first.
                if (_field->toPlainText() != _editOriginalBody) {
                    emit editCancelConfirmRequested();
                } else {
                    cancelEditMode();
                }
            } else {
                cancelReplyMode();
            }
            return true;
        }

        if (key == Qt::Key_Escape) {
            emit escapePressed();
            return true;
        }

        if (key == Qt::Key_Up
            && _field->toPlainText().trimmed().isEmpty()
            && !_editMode
            && !_replyMode
            && _field->textCursor().blockNumber() == 0
            && !(mod & (Qt::ShiftModifier
                | Qt::ControlModifier
                | Qt::AltModifier
                | Qt::MetaModifier))) {
            emit editLastMessageRequested();
            return true;
        }

        // Cmd+Shift+. toggles blockquote.
        // On macOS, Cmd+Shift+. produces Key_Greater with ControlModifier
        // (Shift is consumed by the key mapping: Shift+. = ">").
        // Match both the macOS-actual combo and the logical combo.
        if ((key == Qt::Key_Greater && (mod & Qt::ControlModifier))
            || (key == Qt::Key_Period
                && (mod & Qt::ControlModifier)
                && (mod & Qt::ShiftModifier))) {
            toggleQuote();
            return true;
        }

        // Down arrow at the end of a block-level element (pre/blockquote)
        // that is the last block in the document: create a new plain line
        // below so the cursor can escape the block.
        if (key == Qt::Key_Down && !(mod & Qt::ShiftModifier)) {
            auto cursor = _field->textCursor();
            const auto bfmt = cursor.blockFormat();
            const bool inBlock = bfmt.nonBreakableLines()
                || bfmt.property(QTextFormat::BlockQuoteLevel).toInt() > 0;
            if (inBlock) {
                // Check if cursor is on the last line and can't move down.
                auto probe = cursor;
                const auto posBefore = probe.position();
                probe.movePosition(QTextCursor::Down);
                const auto posAfter = probe.position();
                if (posBefore == posAfter) {
                    // Stuck — append a new plain block after the current one.
                    cursor.movePosition(QTextCursor::End);
                    // Insert block, then force-set a clean format to avoid
                    // Qt inheriting properties from the previous block.
                    // Use negative left margin to counteract the document
                    // margin so the cursor starts near the viewport edge,
                    // aligned with the block decoration above.
                    cursor.insertBlock();
                    QTextBlockFormat plain;
                    plain.setNonBreakableLines(false);
                    plain.setProperty(QTextFormat::BlockQuoteLevel, 0);
                    plain.setLeftMargin(-(_baseDocMargin - 1));
                    cursor.setBlockFormat(plain);
                    cursor.setBlockCharFormat(_defaultCharFormat);
                    _field->setTextCursor(cursor);
                    checkContentHeight();
                    return true;
                }
            }
        }

        // Up arrow at the top of a block-level element (pre/blockquote)
        // that is the first block in the document: create a new plain line
        // above so the cursor can escape the block.
        if (key == Qt::Key_Up && !(mod & Qt::ShiftModifier)) {
            auto cursor = _field->textCursor();
            const auto bfmt = cursor.blockFormat();
            const bool inBlock = bfmt.nonBreakableLines()
                || bfmt.property(QTextFormat::BlockQuoteLevel).toInt() > 0;
            if (inBlock) {
                auto probe = cursor;
                const auto posBefore = probe.position();
                probe.movePosition(QTextCursor::Up);
                const auto posAfter = probe.position();
                if (posBefore == posAfter) {
                    // Stuck — prepend a new plain block before the first one.
                    // Insert a ParagraphSeparator character
                    // (not insertBlock!) to push content down, then move to
                    // Start and set plain format.  insertText goes through
                    // QTextEdit's text-change path which properly resets
                    // the cursor blink timer.
                    QTextBlockFormat plain;
                    plain.setNonBreakableLines(false);
                    plain.setProperty(QTextFormat::BlockQuoteLevel, 0);
                    plain.setLeftMargin(-(_baseDocMargin - 1));
                    cursor.beginEditBlock();
                    cursor.movePosition(QTextCursor::Start);
                    cursor.insertText(
                        QString(QChar::ParagraphSeparator),
                        _defaultCharFormat);
                    cursor.movePosition(QTextCursor::Start);
                    cursor.setBlockFormat(plain);
                    cursor.setCharFormat(_defaultCharFormat);
                    cursor.endEditBlock();
                    _field->setTextCursor(cursor);
                    updateDocumentMargin();
                    checkContentHeight();
                    return true;
                }
            }
        }

        // Backspace in an empty block-level element (pre/blockquote):
        // remove the block formatting instead of deleting content.
        if (key == Qt::Key_Backspace && !(mod & Qt::ControlModifier)) {
            auto cursor = _field->textCursor();
            if (!cursor.hasSelection()) {
                const auto bfmt = cursor.blockFormat();
                const bool inBlock = bfmt.nonBreakableLines()
                    || bfmt.property(QTextFormat::BlockQuoteLevel).toInt() > 0;
                if (inBlock && cursor.block().text().isEmpty()) {
                    QTextBlockFormat plain;
                    plain.setNonBreakableLines(false);
                    plain.setProperty(QTextFormat::BlockQuoteLevel, 0);
                    plain.setLeftMargin(0);
                    plain.setRightMargin(0);
                    plain.setTopMargin(0);
                    plain.setBottomMargin(0);
                    cursor.setBlockFormat(plain);
                    // Reset char format (remove mono font from code blocks).
                    cursor.setBlockCharFormat(_defaultCharFormat);
                    _field->setTextCursor(cursor);
                    updateDocumentMargin();
                    // Defer height recalculation — the document layout
                    // hasn't propagated the block format change yet.
                    // Reset textWidth to force a full relayout first.
                    QMetaObject::invokeMethod(this, [this] {
                        auto *doc = _field->document();
                        const auto tw = doc->textWidth();
                        doc->setTextWidth(-1);
                        doc->setTextWidth(tw);
                        checkContentHeight();
                    }, Qt::QueuedConnection);
                    _field->viewport()->update();
                    return true;
                }
                // Backspace on an empty plain line directly above a block:
                // remove the empty line and move cursor into the block.
                if (!inBlock && cursor.block().text().isEmpty()
                    && cursor.atBlockStart()) {
                    const auto nextBlock = cursor.block().next();
                    if (nextBlock.isValid()) {
                        const auto nbfmt = nextBlock.blockFormat();
                        const bool nextIsBlock = nbfmt.nonBreakableLines()
                            || nbfmt.property(QTextFormat::BlockQuoteLevel).toInt() > 0;
                        if (nextIsBlock) {
                            cursor.select(QTextCursor::BlockUnderCursor);
                            cursor.removeSelectedText();
                            // If removal left cursor before the block,
                            // delete the trailing newline to merge.
                            if (!cursor.blockFormat().nonBreakableLines()
                                && cursor.blockFormat().property(QTextFormat::BlockQuoteLevel).toInt() <= 0
                                && cursor.block().next().isValid()) {
                                cursor.deleteChar();
                            }
                            _field->setTextCursor(cursor);
                            updateDocumentMargin();
                            checkContentHeight();
                            _field->viewport()->update();
                            return true;
                        }
                    }
                }
            }
        }

        // macOS: Cmd+Backspace = delete to start of line.
        if (key == Qt::Key_Backspace && (mod & Qt::ControlModifier)) {
            auto cursor = _field->textCursor();
            cursor.movePosition(QTextCursor::StartOfBlock, QTextCursor::KeepAnchor);
            cursor.removeSelectedText();
            return true;
        }
    }
    return Ui::RpWidget::eventFilter(obj, event);
}

void HistoryInput::paintEvent(QPaintEvent *) {
    QPainter p(this);

    // Top separator line (1px).
    p.setPen(QPen(st::toolbarSeparatorFg, 1));
    p.drawLine(0, 0, width(), 0);

    if (_voiceRecording) {
        const auto centerY = height() / 2;
        const auto radius = st::historyRecordSignalRadius;
        const QRect redCircleRect(
            centerY - radius,
            centerY - radius,
            2 * radius,
            2 * radius);

        PainterHighQualityEnabler hq(p);
        p.setPen(Qt::NoPen);
        p.setBrush(st::historyRecordVoiceFgInactive);
        p.drawEllipse(redCircleRect.center() + QPoint(1, 1), radius, radius);

        const auto font = st::historyRecordFont();
        p.setFont(font);
        p.setPen(st::historyRecordDurationFg);
        const auto metrics = QFontMetrics(font);
        const auto durationLeft = redCircleRect.right() + 1 + st::historyRecordDurationSkip;
        const QRect durationRect(
            durationLeft,
            centerY - metrics.height() / 2,
            metrics.horizontalAdvance(QStringLiteral("00:00")),
            metrics.height());
        p.drawText(durationRect, Qt::AlignLeft | Qt::AlignVCenter, voiceRecordingDurationText());

        const auto cancel = tr("Release outside this field to cancel");
        const auto messageLeft = durationRect.right() + st::historyRecordTextLeft;
        const auto messageRight = _recordButton
            ? (_recordButton->x() - st::historyRecordTextLeft)
            : (width() - st::historyRecordTextLeft);
        const auto messageWidth = qMax(0, messageRight - messageLeft);
        const QRect cancelRect(
            messageLeft,
            centerY - metrics.height() / 2,
            messageWidth,
            metrics.height());
        p.setPen(_voiceRecordingSendOnRelease
            ? st::historyRecordCancel
            : st::historyRecordCancelActive);
        p.drawText(
            cancelRect,
            Qt::AlignCenter,
            metrics.elidedText(cancel, Qt::ElideRight, messageWidth));
        _editCancelRect = QRect();
        return;
    }

    if (_editMode || _replyMode) {
        const QRect barRect(0, 1, width(), st::historyReplyHeight);
        p.fillRect(barRect, st::historyReplyBg);
        if (_editMode) {
            drawChatIcon(
                p,
                QStringLiteral("input_edit"),
                devicePixelRatioF(),
                st::historyReplyIconFg,
                QPoint(
                    st::historyReplyIconPosition.x(),
                    barRect.top() + st::historyReplyIconPosition.y()));
        }

        _editCancelRect = QRect(
            width() - st::historyReplyHeight,
            barRect.top(),
            st::historyReplyHeight,
            st::historyReplyHeight);

        const auto cursorPos = mapFromGlobal(QCursor::pos());
        const auto overCancel = _editCancelRect.contains(cursorPos);

        PainterHighQualityEnabler hq(p);
        p.setPen(QPen(
            overCancel ? st::historyReplyCancelFgOver : st::historyReplyCancelFg,
            1.4,
            Qt::SolidLine,
            Qt::RoundCap,
            Qt::RoundJoin));
        const auto cx = _editCancelRect.center().x();
        const auto cy = _editCancelRect.center().y();
        p.drawLine(QPointF(cx - 4.5, cy - 4.5), QPointF(cx + 4.5, cy + 4.5));
        p.drawLine(QPointF(cx + 4.5, cy - 4.5), QPointF(cx - 4.5, cy + 4.5));

        auto textLeft = st::historyReplySkip;
        if (_replyMode && !_replyPreviewImage.isNull()) {
            const QRect previewRect(
                st::historyReplySkip,
                barRect.top() + (st::historyReplyHeight - st::historyReplyPreview) / 2,
                st::historyReplyPreview,
                st::historyReplyPreview);
            {
                PainterHighQualityEnabler hq(p);
                p.setPen(Qt::NoPen);
                p.setBrush(st::windowBgOver);
                p.drawRoundedRect(previewRect, kReplyPreviewRadius, kReplyPreviewRadius);
            }
            p.save();
            QPainterPath clip;
            clip.addRoundedRect(
                QRectF(previewRect),
                kReplyPreviewRadius,
                kReplyPreviewRadius);
            p.setClipPath(clip);
            p.setRenderHint(QPainter::SmoothPixmapTransform, true);
            const auto sourceRect = centeredCropRect(_replyPreviewImage.size(), previewRect.size());
            p.drawImage(previewRect, _replyPreviewImage, sourceRect);
            p.restore();
            textLeft += st::historyReplyPreview + st::msgReplyBarSkip;
        }
        const int textRight = _editCancelRect.left() - st::msgReplyPadding.right();
        const int textWidth = qMax(0, textRight - textLeft);

        const auto title = _editMode
            ? tr("Edit Message")
            : (_replySenderName.isEmpty() ? tr("Reply") : _replySenderName);
        const auto preview = (_editMode ? _editPreviewText : _replyPreviewText).simplified();

        p.setPen(st::historyReplyNameFg);
        p.setFont(st::msgServiceNameFont);
        const auto titleText = QFontMetrics(st::msgServiceNameFont).elidedText(
            title,
            Qt::ElideRight,
            textWidth);
        p.drawText(
            textLeft,
            barRect.top() + st::msgReplyPadding.top() + st::msgServiceNameFont->ascent,
            titleText);

        p.setPen(st::historyComposeAreaFg);
        p.setFont(st::msgFont);
        const auto previewText = QFontMetrics(st::msgFont).elidedText(
            preview,
            Qt::ElideRight,
            textWidth);
        p.drawText(
            textLeft,
            barRect.top() + st::msgReplyPadding.top() + st::msgServiceNameFont->height + st::msgFont->ascent,
            previewText);
    } else {
        _editCancelRect = QRect();
    }
}

void HistoryInput::resizeEvent(QResizeEvent *e) {
    Ui::RpWidget::resizeEvent(e);
    updateControlsGeometry();
}

void HistoryInput::moveEvent(QMoveEvent *e) {
    Ui::RpWidget::moveEvent(e);
    // The popup is anchored to the composer's window position; keep it flush
    // when the parent re-lays-out the composer (reply/edit bar, window resize).
    repositionMentionPopup();
}

void HistoryInput::mousePressEvent(QMouseEvent *e) {
    if ((_editMode || _replyMode)
        && e->button() == Qt::LeftButton
        && _editCancelRect.contains(e->pos())) {
        if (_editMode) {
            if (_field->toPlainText() != _editOriginalBody) {
                emit editCancelConfirmRequested();
            } else {
                cancelEditMode();
            }
        } else {
            cancelReplyMode();
        }
        e->accept();
        return;
    }
    Ui::RpWidget::mousePressEvent(e);
}

void HistoryInput::mouseMoveEvent(QMouseEvent *e) {
    const auto overCancel = (_editMode || _replyMode)
        && _editCancelRect.contains(e->pos());
    if (overCancel) {
        setCursor(Qt::PointingHandCursor);
    } else {
        unsetCursor();
    }
    Ui::RpWidget::mouseMoveEvent(e);
}

void HistoryInput::leaveEvent(QEvent *e) {
    unsetCursor();
    Ui::RpWidget::leaveEvent(e);
}

void HistoryInput::updateControlsGeometry() {
    const auto w = width();
    const auto h = height();
    const auto editTop = (_editMode || _replyMode) ? kEditBarHeight : 0;

    // Buttons are bottom-aligned (pinned to the bottom of the compose area).
    const auto btnY = h - kButtonHeight;
    const auto recordBtnY = h - _recordButton->height();

    // Left: [attach]
    auto leftX = kSendRight;
    _attachButton->move(leftX, btnY);
    leftX += kButtonWidth;

    // Right: [emoji_picker][record]
    auto rightX = w - kSendRight;
    rightX -= _recordButton->width();
    _recordButton->move(rightX, recordBtnY);
    rightX -= _emojiPickerButton->width();
    _emojiPickerButton->move(rightX, btnY);

    if (_voiceRecording) {
        _field->setGeometry(0, 0, 0, 0);
        return;
    }

    // Input field fills the space between left and right button groups,
    // and stretches from top padding to bottom padding.
    const auto fieldLeft = leftX;
    const auto fieldRight = rightX;
    const auto fieldWidth = fieldRight - fieldLeft;
    const auto fieldHeight = qMax(1, h - 2 * kSendPadding - editTop);
    _field->setGeometry(fieldLeft, kSendPadding + editTop, fieldWidth, fieldHeight);

    // Keep the mention popup anchored as the composer grows / the bars toggle.
    repositionMentionPopup();
}

void HistoryInput::checkContentHeight() {
    if (_voiceRecording) {
        const auto newHeight = kFieldHeightMin + 2 * kSendPadding;
        if (height() != newHeight) {
            setFixedHeight(newHeight);
            emit heightChanged();
        }
        return;
    }

    // Keep document margin in sync with code block presence.
    updateDocumentMargin();

    // Compute desired height from QTextDocument size, clamp to min/max.
    const auto docHeight = static_cast<int>(
        std::ceil(_field->document()->size().height()));

    const auto fieldHeight = std::clamp(
        docHeight,
        static_cast<int>(kFieldHeightMin),
        static_cast<int>(kFieldHeightMax));
    const auto editTop = (_editMode || _replyMode) ? kEditBarHeight : 0;
    const auto newHeight = fieldHeight + 2 * kSendPadding + editTop;

    if (height() != newHeight) {
        setFixedHeight(newHeight);
        emit heightChanged();
    }
}

void HistoryInput::updateDocumentMargin() {
    // Qt doesn't apply QTextBlockFormat::topMargin for the first block
    // in a document.  To create header space for a code block that starts
    // the document, set an explicit top margin on the root frame format.
    // When there is no code block, clear the explicit property so that
    // topMargin() falls back to the uniform margin() (== documentMargin).
    auto *doc = _field->document();
    bool firstIsPre = false;
    const auto first = doc->begin();
    if (first.isValid() && first.blockFormat().nonBreakableLines()) {
        firstIsPre = true;
    }
    auto *rootFrame = doc->rootFrame();
    auto ffmt = rootFrame->frameFormat();
    if (firstIsPre) {
        // baseDocMargin for padding above header (matching bottom padding),
        // plus header height + skip for the decoration area.
        const auto &preStyle = st::historyPreStyle;
        const qreal needed = _baseDocMargin + preStyle.header + preStyle.verticalSkip;
        if (ffmt.topMargin() != needed) {
            ffmt.setTopMargin(needed);
            rootFrame->setFrameFormat(ffmt);
        }
    } else if (ffmt.topMargin() != _baseDocMargin) {
        // Set to the base margin value instead of clearing the property.
        // clearProperty + adjustSize + setTextWidth triggers a triple
        // relayout that kills Qt's cursor blink timer (QTBUG-16627).
        // Always setting a concrete value avoids this.
        ffmt.setTopMargin(_baseDocMargin);
        rootFrame->setFrameFormat(ffmt);
    }

    // Hide placeholder when block-level formatting is active.
    const auto hasBlockFormat = _field->blockFormats().any();
    if (hasBlockFormat != _placeholderHidden) {
        _placeholderHidden = hasBlockFormat;
        _field->setPlaceholderText(hasBlockFormat
            ? QString()
            : tr("Write a message..."));
    }
}

void HistoryInput::updateSendButton() {
    updateSendButton(_field ? _field->toPlainText() : QString());
}

void HistoryInput::updateSendButton(const QString &text) {
    if (_voiceRecording) {
        _recordButton->setIcon(ComposeIconButton::Record);
        return;
    }
    const auto hasText = HistoryDraftStore::hasVisibleContent(text);
    if (_editMode) {
        _recordButton->setIcon(ComposeIconButton::Save);
        return;
    }
    _recordButton->setIcon(
        hasText
            ? ComposeIconButton::Send
            : ComposeIconButton::Record);
}

bool HistoryInput::canStartVoiceRecording() const {
    return _field
        && _recordButton
        && !_voiceRecording
        && !_editMode
        && !_replyMode
        && _field->toPlainText().trimmed().isEmpty()
        && _recordButton->icon() == ComposeIconButton::Record;
}

void HistoryInput::startVoiceRecording() {
    if (!canStartVoiceRecording() || !_voiceRecorder) {
        return;
    }

    QString error;
    if (!_voiceRecorder->start(&error)) {
        return;
    }

    if (_emojiPicker && _emojiPicker->isVisible()) {
        _emojiPicker->hide();
    }

    _voiceRecording = true;
    _voiceRecordingSendOnRelease = true;
    _recordButton->setIcon(ComposeIconButton::Record);
    _recordButton->setRecordingState(true, true);
    _attachButton->hide();
    _emojiPickerButton->hide();
    _field->hide();

    _voiceRecordingTimer.start();
    updateVoiceRecordingDuration();
    updateAppEventFilter();
    checkContentHeight();
    updateControlsGeometry();
    update();
}

void HistoryInput::finishVoiceRecording(bool send) {
    if (!_voiceRecording || !_voiceRecorder) {
        return;
    }

    _voiceRecordingTimer.stop();
    const auto result = send
        ? _voiceRecorder->stop()
        : HistoryVoiceRecorder::Result();
    if (!send) {
        _voiceRecorder->cancel();
    }

    _voiceRecording = false;
    _voiceRecordingSendOnRelease = true;
    _recordButton->setRecordingState(false, true);
    _attachButton->show();
    _emojiPickerButton->show();
    _field->show();

    updateAppEventFilter();
    updateSendButton();
    checkContentHeight();
    updateControlsGeometry();
    update();

    if (!send) {
        return;
    }

    if (result.durationMs < kVoiceRecordMinDurationMs || result.path.isEmpty()) {
        if (!result.path.isEmpty()) {
            QFile::remove(result.path);
        }
        return;
    }

    emit voiceRecorded(result.path, result.durationMs, result.waveform);
}

void HistoryInput::cancelVoiceRecording() {
    finishVoiceRecording(false);
}

void HistoryInput::updateVoiceRecordingHotspot(const QPoint &globalPos) {
    if (!_voiceRecording || !_recordButton) {
        return;
    }
    const auto inside = _recordButton->rect().contains(_recordButton->mapFromGlobal(globalPos));
    if (_voiceRecordingSendOnRelease == inside) {
        return;
    }
    _voiceRecordingSendOnRelease = inside;
    _recordButton->setRecordingState(true, inside);
    update();
}

void HistoryInput::updateVoiceRecordingDuration() {
    if (!_voiceRecording || !_voiceRecorder) {
        return;
    }
    if (_voiceRecorder->durationMs() >= kVoiceRecordMaxDurationMs) {
        finishVoiceRecording(true);
        return;
    }
    update();
}

void HistoryInput::updateAppEventFilter() {
    const auto needed = _voiceRecording || _emojiPickerGlobalTracking;
    if (needed == _appEventFilterInstalled) {
        return;
    }
    if (needed) {
        qApp->installEventFilter(this);
    } else {
        qApp->removeEventFilter(this);
    }
    _appEventFilterInstalled = needed;
}

QString HistoryInput::voiceRecordingDurationText() const {
    const auto ms = _voiceRecorder ? _voiceRecorder->durationMs() : 0;
    const auto seconds = int(ms / 1000);
    return QStringLiteral("%1:%2")
        .arg(seconds / 60)
        .arg(seconds % 60, 2, 10, QLatin1Char('0'));
}

/// Escape HTML special characters.
static QString escapeHtml(const QString &s) {
    QString out;
    out.reserve(s.size());
    for (const auto ch : s) {
        switch (ch.unicode()) {
        case u'&': out += QStringLiteral("&amp;"); break;
        case u'<': out += QStringLiteral("&lt;"); break;
        case u'>': out += QStringLiteral("&gt;"); break;
        case u'"': out += QStringLiteral("&quot;"); break;
        default: out += ch; break;
        }
    }
    return out;
}

/// Convert inline markdown to HTML within a single line of text.
/// Handles: **bold**, __bold__, *italic*, _italic_, ~~strike~~, `code`.
static QString convertInlineMarkdown(const QString &line) {
    QString html = escapeHtml(line);

    // Inline code: `code` (must be done before other inline patterns
    // to avoid converting markdown inside backticks).
    static const QRegularExpression reCode(QStringLiteral("`([^`]+)`"));
    html.replace(reCode, QStringLiteral("<code>\\1</code>"));

    // Bold: **text** or __text__
    static const QRegularExpression reBold1(QStringLiteral("\\*\\*(.+?)\\*\\*"));
    static const QRegularExpression reBold2(QStringLiteral("__(.+?)__"));
    html.replace(reBold1, QStringLiteral("<b>\\1</b>"));
    html.replace(reBold2, QStringLiteral("<b>\\1</b>"));

    // Italic: *text* or _text_ (but not inside words for underscore)
    static const QRegularExpression reItalic1(QStringLiteral("\\*(.+?)\\*"));
    static const QRegularExpression reItalic2(QStringLiteral("(?<![\\w])_(.+?)_(?![\\w])"));
    html.replace(reItalic1, QStringLiteral("<i>\\1</i>"));
    html.replace(reItalic2, QStringLiteral("<i>\\1</i>"));

    // Strikethrough: ~~text~~
    static const QRegularExpression reStrike(QStringLiteral("~~(.+?)~~"));
    html.replace(reStrike, QStringLiteral("<del>\\1</del>"));

    return html;
}

/// Convert plain text with markdown syntax to HTML.
/// Handles code blocks (```), blockquotes (>), and inline formatting.
/// Returns empty string if no markdown was found.
static QString markdownToHtml(const QString &text) {
    const auto lines = text.split(u'\n');
    bool hasMarkdown = false;
    QString html;
    const auto fence = QStringLiteral("```");
    const auto quotePrefix = QStringLiteral("> ");
    const auto br = QStringLiteral("<br>");

    int i = 0;
    while (i < lines.size()) {
        const auto &line = lines[i];

        // Fenced code block: ```...```
        if (line.trimmed().startsWith(fence)) {
            hasMarkdown = true;
            ++i;
            QString codeContent;
            while (i < lines.size() && !lines[i].trimmed().startsWith(fence)) {
                if (!codeContent.isEmpty()) codeContent += u'\n';
                codeContent += lines[i];
                ++i;
            }
            if (i < lines.size()) ++i; // skip closing ```
            // Ensure <br> separator so QTextDocument creates a new block.
            if (!html.isEmpty() && !html.endsWith(br)) html += br;
            html += QStringLiteral("<pre><code>")
                + escapeHtml(codeContent)
                + QStringLiteral("</code></pre>");
            continue;
        }

        // Blockquote: > text (consecutive > lines merge into one block)
        if (line.startsWith(quotePrefix) || line == QStringLiteral(">")) {
            hasMarkdown = true;
            QString quoteContent;
            while (i < lines.size()
                   && (lines[i].startsWith(quotePrefix)
                       || lines[i] == QStringLiteral(">"))) {
                if (!quoteContent.isEmpty()) quoteContent += br;
                const auto content = lines[i].startsWith(quotePrefix)
                    ? lines[i].mid(2)
                    : QString();
                quoteContent += convertInlineMarkdown(content);
                ++i;
            }
            // Ensure <br> separator so QTextDocument creates a new block.
            if (!html.isEmpty() && !html.endsWith(br)) html += br;
            html += QStringLiteral("<blockquote>")
                + quoteContent
                + QStringLiteral("</blockquote>");
            continue;
        }

        // Regular line: convert inline markdown.
        const auto converted = convertInlineMarkdown(line);
        if (converted != escapeHtml(line)) {
            hasMarkdown = true;
        }
        if (!html.isEmpty()) html += br;
        html += converted;
        ++i;
    }

    return hasMarkdown ? html : QString();
}

void HistoryInput::send() {
    if (!_field) return;
    auto text = _field->toPlainText().trimmed();
    if (text.isEmpty() && !_editMode) return;
    // 1. Try markdown conversion first (handles ```, >, **bold**, etc.).
    //    This is the primary path for user-typed messages.
    QString html = markdownToHtml(text);

    // 2. If no markdown found, check if the QTextEdit document has
    //    user-applied rich formatting (bold, italic, etc.).
    //    Compare each fragment against the SAVED default char format
    //    (not currentCharFormat() which changes with cursor position).
    if (html.isEmpty()) {
        const auto &defaultFmt = _defaultCharFormat;
        const auto *doc = _field->document();
        bool hasRichFormat = false;
        for (auto block = doc->begin(); block != doc->end(); block = block.next()) {
            const auto bfmt = block.blockFormat();
            if (bfmt.nonBreakableLines()
                || bfmt.property(QTextFormat::BlockQuoteLevel).toInt() > 0) {
                hasRichFormat = true;
                break;
            }
            for (auto it = block.begin(); !it.atEnd(); ++it) {
                const auto frag = it.fragment();
                if (!frag.isValid()) continue;
                const auto fmt = frag.charFormat();
                // Only detect formatting that differs from the field default.
                if (fmt.fontWeight() != defaultFmt.fontWeight()
                        && fmt.fontWeight() >= QFont::Bold
                    || fmt.fontItalic() != defaultFmt.fontItalic()
                        && fmt.fontItalic()
                    || fmt.fontUnderline() != defaultFmt.fontUnderline()
                        && fmt.fontUnderline()
                    || fmt.fontStrikeOut() != defaultFmt.fontStrikeOut()
                        && fmt.fontStrikeOut()
                    || fmt.fontFixedPitch() != defaultFmt.fontFixedPitch()
                        && fmt.fontFixedPitch()
                    || fmt.isAnchor()
                    || !fmt.property(kMentionUserIdProperty).toString().isEmpty()
                    || fmt.property(kRoomMentionProperty).toBool()) {
                    hasRichFormat = true;
                    break;
                }
            }
            if (hasRichFormat) break;
        }
        if (hasRichFormat) {
            html = buildCleanHtml(doc);
        }
    }

    // Generate clean plain text from HTML so the Matrix plain-text fallback
    // body doesn't contain raw markdown markers (**, >, backticks, etc.).
    if (!html.isEmpty()) {
        QTextDocument doc;
        doc.setHtml(html);
        text = doc.toPlainText();
    }

    if (_editMode) {
        emit editSubmitted(_editEventId, text, html);
        return;
    }

    if (_sendCallback) {
        _sendCallback(text, html, _replyMode ? _replyEventId : QString());
    }
    if (_replyMode) {
        cancelReplyMode();
    } else {
        clearInput();
    }
}

void HistoryInput::setRoomMembers(const QVector<UserProfile> &members) {
    if (!_mentionAutocomplete) {
        _mentionAutocomplete = new MentionAutocomplete(window());
        QObject::connect(_mentionAutocomplete, &MentionAutocomplete::mentionChosen,
            this, [this](
                    const QString &userId,
                    const QString &displayName,
                    bool roomMention) {
                if (roomMention) {
                    insertRoomMention();
                } else {
                    insertMention(userId, displayName);
                }
                _field->setFocus();
            });
    }
    _mentionAutocomplete->setMembers(members);
}

void HistoryInput::refreshMentionPopup() {
    if (_mentionAutocomplete && _mentionAutocomplete->isVisible()) {
        _mentionAutocomplete->refresh();
    }
}

void HistoryInput::repositionMentionPopup() {
    if (!_mentionAutocomplete || !_mentionAutocomplete->isVisible()) {
        return;
    }
    // Anchor the popup flush above the composer: full composer width, with its
    // bottom edge at the composer's top so it never overlaps the input bar.
    // The popup is a child of window(), so map composer coords to the window.
    const auto topLeft = mapTo(window(), QPoint(0, 0));
    _mentionAutocomplete->setFixedWidth(width());
    _mentionAutocomplete->move(
        topLeft.x(),
        topLeft.y() - _mentionAutocomplete->height());
    _mentionAutocomplete->raise();
}

void HistoryInput::checkMentionTrigger() {
    if (!_mentionAutocomplete || !_field) return;

    auto cursor = _field->textCursor();
    if (cursor.hasSelection()) {
        if (_mentionAutocomplete->isVisible()) {
            _mentionAutocomplete->hide();
        }
        _mentionCursorStart = -1;
        return;
    }

    // Scan only the cursor's paragraph: the trigger scan never crosses a
    // space, and QTextDocument block boundaries are '\n' (which isSpace()
    // stops on), so block-local text is equivalent to the whole document
    // but O(paragraph) instead of O(document) per keystroke.
    const auto block = cursor.block();
    const auto result = TeleMatrix::MentionTrigger::Scan(
        block.text(), cursor.positionInBlock());

    if (result.atPos < 0) {
        if (_mentionAutocomplete->isVisible()) {
            _mentionAutocomplete->hide();
        }
        _mentionCursorStart = -1;
        return;
    }

    // _mentionCursorStart stays a document-global position: insertMention /
    // insertRoomMention feed it to cursor.setPosition().
    _mentionCursorStart = block.position() + result.atPos;
    _mentionAutocomplete->updateFilter(result.query);
    repositionMentionPopup();
}

void HistoryInput::insertMention(const QString &userId, const QString &displayName) {
    if (!_field || _mentionCursorStart < 0) return;

    auto cursor = _field->textCursor();
    const auto pos = cursor.position();

    // Select from '@' to current cursor position and remove.
    cursor.setPosition(_mentionCursorStart);
    cursor.setPosition(pos, QTextCursor::KeepAnchor);
    cursor.removeSelectedText();

    // Insert colored mention text with custom property.
    QTextCharFormat mentionFmt = _defaultCharFormat;
    mentionFmt.setForeground(st::historyLinkInFg);
    mentionFmt.setProperty(kMentionUserIdProperty, userId);
    cursor.insertText(QStringLiteral("@") + displayName, mentionFmt);

    // Insert trailing space with default format.
    cursor.insertText(QStringLiteral(" "), _defaultCharFormat);

    _field->setTextCursor(cursor);
    _mentionCursorStart = -1;
}

void HistoryInput::insertRoomMention() {
    if (!_field || _mentionCursorStart < 0) return;

    auto cursor = _field->textCursor();
    const auto pos = cursor.position();

    cursor.setPosition(_mentionCursorStart);
    cursor.setPosition(pos, QTextCursor::KeepAnchor);
    cursor.removeSelectedText();

    QTextCharFormat mentionFmt = _defaultCharFormat;
    mentionFmt.setForeground(st::historyLinkInFg);
    mentionFmt.setProperty(kRoomMentionProperty, true);
    cursor.insertText(QStringLiteral("@room"), mentionFmt);
    cursor.insertText(QStringLiteral(" "), _defaultCharFormat);

    _field->setTextCursor(cursor);
    _mentionCursorStart = -1;
}

void HistoryInput::showEmojiPicker() {
	if (!canShowEmojiPicker()) {
		return;
	}
	HistoryEmojiPicker::initEmojiPanelPxValues();
	if (!_emojiPicker) {
		_emojiPicker = new HistoryEmojiPicker(_controller, window());
		_emojiPicker->installEventFilter(this);
        QObject::connect(_emojiPicker, &HistoryEmojiPicker::emojiSelected, this, [this](const QString &emoji) {
            if (_controller) {
                _controller->accountSettings().incrementRecentEmoji(emoji);
                // Persist to account data (server) + local app_cache.db mirror;
                // recents are no longer stored in settings.json.
                _controller->pushRecentEmoji();
            }
            _field->insertPlainText(emoji);
            _field->setFocus();
        });
    }

    auto *anchorWidget = window();
    if (!anchorWidget) {
        anchorWidget = this;
    }
    auto *historyArea = parentWidget() ? parentWidget() : anchorWidget;
    const auto global = _emojiPickerButton->mapToGlobal(QPoint(0, 0));
    const auto local = historyArea
        ? historyArea->mapFromGlobal(global)
        : global;
    const auto anchorArea = historyArea
        ? QRect(historyArea->mapToGlobal(QPoint(0, 0)), historyArea->size())
        : QRect(global.x() - 500, global.y() - 500, 1000, 1000);
    const auto shadow = HistoryEmojiPicker::shadowExtend();
    const auto panelWidth = HistoryEmojiPicker::panelWidth() + 2 * shadow;
    const auto dropDown = historyArea
        && (local.y() < (historyArea->height() / 2));
    const auto availableHeight = dropDown
        ? (anchorArea.bottom() - (global.y() + _emojiPickerButton->height()) + 1)
        : (global.y() - anchorArea.top());
    const auto bodyHeight = qBound(
        HistoryEmojiPicker::minBodyHeight(),
        qRound(availableHeight * 0.75) - 2 * shadow,
        HistoryEmojiPicker::maxBodyHeight());
    const QSize hint(
        panelWidth,
        bodyHeight + 2 * shadow);
    const auto right = global.x() + _emojiPickerButton->width() * 3;
    QPoint topLeftGlobal(
        right - hint.width() + shadow,
        dropDown
            ? (global.y() + _emojiPickerButton->height())
            : (global.y() - hint.height()));

    const auto *screenObj = _emojiPickerButton->screen();
    const QRect screen = screenObj
        ? screenObj->availableGeometry()
        : QRect(global.x() - 500, global.y() - 500, 1000, 1000);
    QRect available = anchorArea.intersected(screen);
    if (!available.isValid()) {
        available = screen;
    }
    if (topLeftGlobal.x() + hint.width() > available.right() + 1) {
        topLeftGlobal.setX(available.right() + 1 - hint.width());
    }
    if (topLeftGlobal.x() < available.left()) {
        topLeftGlobal.setX(available.left());
    }
    if (topLeftGlobal.y() + hint.height() > available.bottom() + 1) {
        topLeftGlobal.setY(available.bottom() + 1 - hint.height());
    }
    if (topLeftGlobal.y() < available.top()) {
        topLeftGlobal.setY(
            global.y() + _emojiPickerButton->height() + 4);
    }

    _emojiPicker->resize(hint);
    _emojiPicker->move(topLeftGlobal);
	_emojiPicker->show();
	_emojiPicker->raise();
	if (!_emojiPickerGlobalTracking) {
		_emojiPickerGlobalTracking = true;
        updateAppEventFilter();
	}
}

bool HistoryInput::canShowEmojiPicker() const {
	if (!_emojiPickerButton
		|| !_emojiPickerButton->isVisible()
		|| !_emojiPickerButton->isEnabled()) {
		return false;
	}
	auto *anchorWindow = window();
	if (!anchorWindow) {
		anchorWindow = const_cast<HistoryInput*>(this);
	}
	if (!anchorWindow || !anchorWindow->isVisible()) {
		return false;
	}
	if (QGuiApplication::applicationState() != Qt::ApplicationActive) {
		return false;
	}
	if (!anchorWindow->isActiveWindow()) {
		return false;
	}
	if (const auto *handle = anchorWindow->windowHandle(); handle && !handle->isExposed()) {
		return false;
	}
	return true;
}

void HistoryInput::positionAndShowPicker(HistoryEmojiPicker *picker) {
	if (!picker || !_emojiPickerButton) return;
	HistoryEmojiPicker::initEmojiPanelPxValues();

    // Exact same positioning logic as showEmojiPicker().
    auto *anchorWidget = window();
    if (!anchorWidget) anchorWidget = this;
    auto *historyArea = parentWidget() ? parentWidget() : anchorWidget;

    const auto global = _emojiPickerButton->mapToGlobal(QPoint(0, 0));
    const auto local = historyArea
        ? historyArea->mapFromGlobal(global) : global;
    const auto anchorArea = historyArea
        ? QRect(historyArea->mapToGlobal(QPoint(0, 0)), historyArea->size())
        : QRect(global.x() - 500, global.y() - 500, 1000, 1000);

    const auto shadow = HistoryEmojiPicker::shadowExtend();
    const auto panelWidth = HistoryEmojiPicker::panelWidth() + 2 * shadow;
    const auto dropDown = historyArea && (local.y() < (historyArea->height() / 2));
    const auto availableHeight = dropDown
        ? (anchorArea.bottom() - (global.y() + _emojiPickerButton->height()) + 1)
        : (global.y() - anchorArea.top());
    const auto bodyHeight = qBound(
        HistoryEmojiPicker::minBodyHeight(),
        qRound(availableHeight * 0.75) - 2 * shadow,
        HistoryEmojiPicker::maxBodyHeight());
    const QSize hint(panelWidth, bodyHeight + 2 * shadow);

    const auto right = global.x() + _emojiPickerButton->width() * 3;
    QPoint topLeft(
        right - hint.width() + shadow,
        dropDown
            ? (global.y() + _emojiPickerButton->height())
            : (global.y() - hint.height()));

    const auto *screenObj = _emojiPickerButton->screen();
    const QRect screen = screenObj
        ? screenObj->availableGeometry()
        : QRect(global.x() - 500, global.y() - 500, 1000, 1000);
    QRect available = anchorArea.intersected(screen);
    if (!available.isValid()) available = screen;

    if (topLeft.x() + hint.width() > available.right() + 1)
        topLeft.setX(available.right() + 1 - hint.width());
    if (topLeft.x() < available.left())
        topLeft.setX(available.left());
    if (topLeft.y() + hint.height() > available.bottom() + 1)
        topLeft.setY(available.bottom() + 1 - hint.height());
    if (topLeft.y() < available.top())
        topLeft.setY(global.y() + _emojiPickerButton->height() + 4);

    picker->resize(hint);
    picker->move(topLeft);
    picker->show();
    picker->raise();
}

void HistoryInput::scheduleEmojiPickerHide(int delayMs) {
	if (!_emojiPicker || !_emojiPicker->isVisible()) {
		return;
	}
	_emojiPickerHideTimer.start(qMax(delayMs, 0));
}

bool HistoryInput::isGlobalPointOverEmojiPicker(const QPoint &global) const {
	const auto overButton = _emojiPickerButton
		&& _emojiPickerButton->rect().contains(
			_emojiPickerButton->mapFromGlobal(global));
	const auto overPicker = _emojiPicker
		&& _emojiPicker->isVisible()
		&& _emojiPicker->frameGeometry().contains(global);
	return overButton || overPicker;
}

bool HistoryInput::isPointerOverEmojiPicker() const {
    return isGlobalPointOverEmojiPicker(QCursor::pos());
}

namespace {

/// Create action with a shortcut label visible in the menu text.
/// Qt doesn't reliably show shortcuts on custom-styled menus on macOS,
/// so we embed the shortcut text directly.
QAction *addActionWithShortcut(
    HistoryPopupMenuStyle::PopupMenu *menu,
    const QString &text,
    const QString &shortcutLabel,
    const std::function<void()> &slot)
{
    // Use tab character to right-align the shortcut hint (Qt convention).
    auto *act = menu->addAction(text + u'\t' + shortcutLabel);
    if (slot) {
        QObject::connect(act, &QAction::triggered, slot);
    }
    return act;
}

} // namespace

HistoryPopupMenuStyle::PopupMenu *HistoryInput::createStyledMenu(QWidget *parent) {
    return HistoryPopupMenuStyle::createStyledMenu(parent);
}

HistoryPopupMenuStyle::PopupMenu *HistoryInput::createFormattingSubmenu(
        HistoryPopupMenuStyle::PopupMenu *parent) {
    auto *sub = createStyledMenu(parent);
    sub->setTitle(tr("Formatting"));

    const bool hasSel = _field->textCursor().hasSelection();
    const bool hasText = !_field->toPlainText().isEmpty();

    auto *boldAct = addActionWithShortcut(sub,
        tr("Bold"),
        QKeySequence(QKeySequence::Bold).toString(QKeySequence::NativeText),
        [this] { toggleBold(); });
    boldAct->setEnabled(hasSel);

    auto *italicAct = addActionWithShortcut(sub,
        tr("Italic"),
        QKeySequence(QKeySequence::Italic).toString(QKeySequence::NativeText),
        [this] { toggleItalic(); });
    italicAct->setEnabled(hasSel);

    auto *underlineAct = addActionWithShortcut(sub,
        tr("Underline"),
        QKeySequence(QKeySequence::Underline).toString(QKeySequence::NativeText),
        [this] { toggleUnderline(); });
    underlineAct->setEnabled(hasSel);

    auto *strikeAct = addActionWithShortcut(sub,
        tr("Strikethrough"),
        QKeySequence(Qt::SHIFT | Qt::CTRL | Qt::Key_X).toString(QKeySequence::NativeText),
        [this] { toggleStrikethrough(); });
    strikeAct->setEnabled(hasSel);

    auto *quoteAct = addActionWithShortcut(sub,
        tr("Quote"),
        QKeySequence(Qt::SHIFT | Qt::CTRL | Qt::Key_Period).toString(QKeySequence::NativeText),
        [this] { toggleQuote(); });
    // Quote works without selection, but should be unavailable in an empty field.
    quoteAct->setEnabled(hasText);

    auto *monoAct = addActionWithShortcut(sub,
        tr("Monospace"),
        QKeySequence(Qt::SHIFT | Qt::CTRL | Qt::Key_M).toString(QKeySequence::NativeText),
        [this] { toggleMonospace(); });
    monoAct->setEnabled(hasSel);

    sub->addSeparator();

    auto *clearAct = addActionWithShortcut(sub,
        tr("Clear formatting"),
        QKeySequence(Qt::SHIFT | Qt::CTRL | Qt::Key_N).toString(QKeySequence::NativeText),
        [this] { clearFormatting(); });
    clearAct->setEnabled(hasSel);

    return sub;
}

void HistoryInput::showContextMenu(const QPoint &globalPos) {
    auto *menu = createStyledMenu(this);

    const bool hasSelection = _field->textCursor().hasSelection();
    const bool hasText = !_field->toPlainText().isEmpty();
    const bool canPaste = !QApplication::clipboard()->text().isEmpty();

    // Undo / Redo.
    auto *undoAct = menu->addAction(tr("Undo"));
    QObject::connect(undoAct, &QAction::triggered, _field, &QTextEdit::undo);
    undoAct->setEnabled(_field->document()->isUndoAvailable());

    auto *redoAct = menu->addAction(tr("Redo"));
    QObject::connect(redoAct, &QAction::triggered, _field, &QTextEdit::redo);
    redoAct->setEnabled(_field->document()->isRedoAvailable());

    menu->addSeparator();

    // Cut / Copy / Paste / Delete.
    auto *cutAct = menu->addAction(tr("Cut"));
    QObject::connect(cutAct, &QAction::triggered, _field, &QTextEdit::cut);
    cutAct->setEnabled(hasSelection);

    auto *copyAct = menu->addAction(tr("Copy"));
    QObject::connect(copyAct, &QAction::triggered, _field, &QTextEdit::copy);
    copyAct->setEnabled(hasSelection);

    auto *pasteAct = menu->addAction(tr("Paste"));
    QObject::connect(pasteAct, &QAction::triggered, _field, &QTextEdit::paste);
    pasteAct->setEnabled(canPaste);

    auto *deleteAct = menu->addAction(tr("Delete"));
    QObject::connect(deleteAct, &QAction::triggered, _field, [this] {
        _field->textCursor().removeSelectedText();
    });
    deleteAct->setEnabled(hasSelection);

    menu->addSeparator();

    // Formatting submenu.
    menu->addSubmenu(createFormattingSubmenu(menu));

    menu->addSeparator();

    // Select All.
    auto *selectAllAct = menu->addAction(tr("Select All"));
    QObject::connect(selectAllAct, &QAction::triggered, _field, &QTextEdit::selectAll);
    selectAllAct->setEnabled(hasText);

    menu->popup(globalPos);
    QObject::connect(menu, &HistoryPopupMenuStyle::PopupMenu::aboutToHide, menu, &QObject::deleteLater);
}

// ─── Formatting actions ──────────────────────────────────

void HistoryInput::toggleBold() {
    auto cursor = _field->textCursor();
    if (!cursor.hasSelection()) return;
    QTextCharFormat fmt;
    const bool wasBold = cursor.charFormat().fontWeight() == QFont::Bold;
    fmt.setFontWeight(wasBold ? QFont::Normal : QFont::Bold);
    cursor.mergeCharFormat(fmt);
    _field->setTextCursor(cursor);
}

void HistoryInput::toggleItalic() {
    auto cursor = _field->textCursor();
    if (!cursor.hasSelection()) return;
    QTextCharFormat fmt;
    fmt.setFontItalic(!cursor.charFormat().fontItalic());
    cursor.mergeCharFormat(fmt);
    _field->setTextCursor(cursor);
}

void HistoryInput::toggleUnderline() {
    auto cursor = _field->textCursor();
    if (!cursor.hasSelection()) return;
    QTextCharFormat fmt;
    fmt.setFontUnderline(!cursor.charFormat().fontUnderline());
    cursor.mergeCharFormat(fmt);
    _field->setTextCursor(cursor);
}

void HistoryInput::toggleStrikethrough() {
    auto cursor = _field->textCursor();
    if (!cursor.hasSelection()) return;
    QTextCharFormat fmt;
    fmt.setFontStrikeOut(!cursor.charFormat().fontStrikeOut());
    cursor.mergeCharFormat(fmt);
    _field->setTextCursor(cursor);
}

void HistoryInput::toggleMonospace() {
    auto cursor = _field->textCursor();

    // No selection: only allow REMOVING format from the current block.
    if (!cursor.hasSelection()) {
        if (!cursor.blockFormat().nonBreakableLines()) return;
        // Remove code block from the current block.
        QTextBlockFormat bfmt;
        bfmt.setNonBreakableLines(false);
        cursor.setBlockFormat(bfmt);
        cursor.movePosition(QTextCursor::StartOfBlock);
        cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
        QTextCharFormat cfmt;
        cfmt.setFontFixedPitch(false);
        cfmt.setFont(st::baseFont(st::fsize));
        cfmt.setForeground(st::historyComposeAreaFg);
        cursor.mergeCharFormat(cfmt);
        _field->setTextCursor(cursor);
        updateDocumentMargin();
        checkContentHeight();
        _field->viewport()->update();
        return;
    }

    // Helper: apply or remove monospace char format on a block range.
    auto applyMonoFont = [this](QTextCursor &c, bool enable) {
        QTextCharFormat fmt;
        if (enable) {
            auto mono = st::monospaceFont(st::fsize);
            fmt.setFont(mono);
            fmt.setFontFixedPitch(true);
            fmt.setForeground(st::msgInMonoFg);
        } else {
            fmt.setFontFixedPitch(false);
            fmt.setFont(st::baseFont(st::fsize));
            fmt.setForeground(st::historyComposeAreaFg);
        }
        c.mergeCharFormat(fmt);
    };

    // Split blocks at selection boundaries so formatting applies only
    // to the selected lines, not the entire containing block.
    auto [selStart, selEnd] = ensureBlockBoundaries(_field, cursor);

    bool allPre = true;
    auto block = _field->document()->findBlock(selStart);
    while (block.isValid() && block.position() < selEnd) {
        if (!block.blockFormat().nonBreakableLines()) {
            allPre = false;
            break;
        }
        block = block.next();
    }

    const bool newPre = !allPre;
    const auto &preStyle = st::historyPreStyle;
    bool isFirst = true;
    cursor.beginEditBlock();
    block = _field->document()->findBlock(selStart);
    while (block.isValid() && block.position() < selEnd) {
        QTextCursor blockCursor(block);
        QTextBlockFormat bfmt = block.blockFormat();
        bfmt.setNonBreakableLines(newPre);
        // First block: topMargin accounts for decoration header + gap.
        // All blocks: bottomMargin accounts for decoration extent + gap.
        bfmt.setTopMargin((newPre && isFirst)
            ? (preStyle.header + 2 * preStyle.verticalSkip) : 0);
        bfmt.setBottomMargin(newPre ? (2 * preStyle.verticalSkip) : 0);
        blockCursor.setBlockFormat(bfmt);
        isFirst = false;
        // Apply/remove mono font on the block text.
        blockCursor.movePosition(QTextCursor::StartOfBlock);
        blockCursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
        applyMonoFont(blockCursor, newPre);
        block = block.next();
    }
    cursor.endEditBlock();
    updateDocumentMargin();
    checkContentHeight();
    _field->viewport()->update();
}

void HistoryInput::toggleQuote() {
    auto cursor = _field->textCursor();

    // No selection: only allow REMOVING quote from the current block.
    if (!cursor.hasSelection()) {
        if (cursor.blockFormat().property(QTextFormat::BlockQuoteLevel).toInt() <= 0)
            return;
        QTextBlockFormat bfmt;
        bfmt.setProperty(QTextFormat::BlockQuoteLevel, 0);
        cursor.setBlockFormat(bfmt);
        _field->setTextCursor(cursor);
        _field->viewport()->update();
        return;
    }

    // Split blocks at selection boundaries.
    auto [selStart, selEnd] = ensureBlockBoundaries(_field, cursor);

    // Check if ALL selected blocks are already quoted.
    bool allQuoted = true;
    auto block = _field->document()->findBlock(selStart);
    while (block.isValid() && block.position() < selEnd) {
        if (block.blockFormat().property(QTextFormat::BlockQuoteLevel).toInt() <= 0) {
            allQuoted = false;
            break;
        }
        block = block.next();
    }

    // Toggle: if all quoted, remove; otherwise, add.
    const int newLevel = allQuoted ? 0 : 1;
    const auto &bqStyle = st::historyBlockquoteStyle;

    bool isFirst = true;
    cursor.beginEditBlock();
    block = _field->document()->findBlock(selStart);
    while (block.isValid() && block.position() < selEnd) {
        QTextCursor blockCursor(block);
        QTextBlockFormat bfmt = block.blockFormat();
        bfmt.setProperty(QTextFormat::BlockQuoteLevel, newLevel);
        bfmt.setTopMargin((newLevel > 0 && isFirst)
            ? (2 * bqStyle.verticalSkip) : 0);
        bfmt.setBottomMargin(newLevel > 0
            ? (2 * bqStyle.verticalSkip) : 0);
        blockCursor.setBlockFormat(bfmt);
        isFirst = false;
        block = block.next();
    }
    cursor.endEditBlock();

    _field->viewport()->update();
}


void HistoryInput::clearFormatting() {
    auto cursor = _field->textCursor();
    if (!cursor.hasSelection()) return;

    // Clear inline formatting.
    QTextCharFormat fmt;
    fmt.setFont(st::baseFont(st::fsize));
    fmt.setFontWeight(QFont::Normal);
    fmt.setFontItalic(false);
    fmt.setFontUnderline(false);
    fmt.setFontStrikeOut(false);
    fmt.setFontFixedPitch(false);
    fmt.setAnchor(false);
    fmt.setAnchorHref(QString());
    fmt.setForeground(st::historyComposeAreaFg);
    cursor.setCharFormat(fmt);

    // Clear block-level formatting (blockquote, pre).
    const int selStart = cursor.selectionStart();
    const int selEnd = cursor.selectionEnd();
    cursor.beginEditBlock();
    auto block = _field->document()->findBlock(selStart);
    while (block.isValid() && block.position() < selEnd) {
        QTextCursor blockCursor(block);
        QTextBlockFormat bfmt;
        bfmt.setNonBreakableLines(false);
        bfmt.setProperty(QTextFormat::BlockQuoteLevel, 0);
        bfmt.setLeftMargin(0);
        bfmt.setRightMargin(0);
        bfmt.setTopMargin(0);
        bfmt.setBottomMargin(0);
        blockCursor.setBlockFormat(bfmt);
        block = block.next();
    }
    cursor.endEditBlock();

    _field->setTextCursor(cursor);
    updateDocumentMargin();
    checkContentHeight();
    _field->viewport()->update();
}

} // namespace TeleMatrix
