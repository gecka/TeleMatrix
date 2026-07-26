// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "history_send_files_dialog.h"

#include <algorithm>
#include <QEnterEvent>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QPalette>
#include <QScrollArea>
#include <QScrollBar>
#include <QTextEdit>

#include "styles/style_constants.h"
#include "../ui/input_submit_settings.h"
#include "ui/painter.h"
#include "ui/widgets/buttons.h"

namespace TeleMatrix {

namespace {

constexpr int kCaptionMinHeight = 44;
constexpr int kCaptionMaxLines = 7;

/// Caption input field with send-on-Enter support and auto-growing height.
/// Matches the main composer field's key behavior.
class CaptionField : public QTextEdit {
public:
    CaptionField(QWidget *parent, std::function<void()> onSubmit,
                 std::function<void(int)> onHeightChanged)
        : QTextEdit(parent)
        , _onSubmit(std::move(onSubmit))
        , _onHeightChanged(std::move(onHeightChanged)) {
        setAcceptRichText(false);
        setTabChangesFocus(true);
        // Flat field: no frame; colors via QPalette, bottom border custom-painted
        // (was a QSS `border-bottom`). Padding replaces the QSS `padding:10 0 4 0`.
        setFrameShape(QFrame::NoFrame);
        setAttribute(Qt::WA_MacShowFocusRect, false);
        setViewportMargins(0, 10, 0, 4);
        {
            QPalette pal = palette();
            pal.setColor(QPalette::Base, st::windowBg);
            pal.setColor(QPalette::Text, st::windowFg);
            pal.setColor(QPalette::Highlight, st::windowActiveTextFg);
            setPalette(pal);
        }
        connect(this, &QTextEdit::textChanged, this, &CaptionField::adjustHeight);
    }

    void setSubmitSettings(InputSubmitSettings s) { _submitSettings = s; }

protected:
    // The 2px bottom border is painted by the dialog: painting it here targets
    // the QTextEdit frame, not the viewport that receives the paint event, so it
    // never showed. Repaint the dialog on focus changes so the border re-colors
    // (activeLineFg focused / inputBorderFg otherwise).
    void focusInEvent(QFocusEvent *e) override {
        QTextEdit::focusInEvent(e);
        if (parentWidget()) {
            parentWidget()->update();
        }
    }
    void focusOutEvent(QFocusEvent *e) override {
        QTextEdit::focusOutEvent(e);
        if (parentWidget()) {
            parentWidget()->update();
        }
    }

    void keyPressEvent(QKeyEvent *e) override {
        const auto key = e->key();
        const auto mod = e->modifiers();

        // Send on Enter (respects submit settings).
        if (ShouldSubmit(key, mod, _submitSettings)) {
            if (_onSubmit) _onSubmit();
            e->accept();
            return;
        }

        // Cmd+Backspace: delete entire line (macOS standard).
#ifdef Q_OS_MAC
        if (key == Qt::Key_Backspace && (mod & Qt::ControlModifier)) {
            auto cursor = textCursor();
            cursor.movePosition(QTextCursor::StartOfLine, QTextCursor::KeepAnchor);
            if (cursor.hasSelection()) {
                cursor.removeSelectedText();
            } else {
                cursor.deletePreviousChar();
            }
            e->accept();
            return;
        }
#endif

        QTextEdit::keyPressEvent(e);
    }

private:
    void adjustHeight() {
        const auto lineH = fontMetrics().lineSpacing();
        const auto docH = int(document()->size().height());
        const auto maxH = kCaptionMinHeight + lineH * (kCaptionMaxLines - 1);
        const auto newH = qBound(kCaptionMinHeight, docH + 14, maxH);
        if (newH != height()) {
            setFixedHeight(newH);
            if (_onHeightChanged) _onHeightChanged(newH);
        }
    }

    std::function<void()> _onSubmit;
    std::function<void(int)> _onHeightChanged;
    InputSubmitSettings _submitSettings = InputSubmitSettings::Enter;
};

// Thin (4px) capsule scrollbar painted with live st:: colors, replacing the
// preview area's QSS scrollbar (`width:4px`, handle `sendMediaScrollBarBg`,
// `border-radius:2px`, no arrow buttons). No hover variant in the original.
class PreviewScrollBar final : public QScrollBar {
public:
    explicit PreviewScrollBar(QWidget *parent)
        : QScrollBar(Qt::Vertical, parent) {}

protected:
    void paintEvent(QPaintEvent *) override {
        if (minimum() >= maximum()) {
            return;
        }
        const int trackHeight = height();
        if (trackHeight <= 0) {
            return;
        }
        const int handleWidth = width();
        const int range = maximum() - minimum();
        const int span = range + pageStep();
        int handleHeight = (span > 0)
            ? int(qint64(trackHeight) * pageStep() / span)
            : trackHeight;
        handleHeight = std::min(handleHeight, trackHeight);

        const int travel = trackHeight - handleHeight;
        const int handleTop = (range > 0)
            ? int(qint64(travel) * (value() - minimum()) / range)
            : 0;

        QPainter p(this);
        PainterHighQualityEnabler hq(p);
        p.setPen(Qt::NoPen);
        p.setBrush(st::sendMediaScrollBarBg);
        p.drawRoundedRect(
            QRectF(0, handleTop, handleWidth, handleHeight), 2.0, 2.0);
    }
};

constexpr int kImagePreviewPadding = 12;
constexpr int kFileCardPadding = 12;
constexpr int kPreviewImageRadius = 6;
// Album small-group thumbnail metrics: group size 30x25, corner radius 4px.
constexpr int kDeleteButtonWidth = 22;
constexpr int kDeleteButtonHeight = 22;
constexpr int kDeleteButtonRadius = 4;
constexpr int kDeleteButtonSkipRight = 14;
constexpr int kDeleteButtonSkipTop = 5;

/// Colorize a mask icon (white-on-black) to a specific color.
/// Uses the first byte of each pixel as opacity mask.
[[nodiscard]] QImage colorizeMask(const QImage &mask, const QColor &color) {
    const auto source = mask.convertToFormat(QImage::Format_ARGB32);
    if (source.isNull()) return {};
    auto result = QImage(source.size(), QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < source.height(); ++y) {
        const auto *src = source.constScanLine(y);
        auto *dst = reinterpret_cast<QRgb*>(result.scanLine(y));
        for (int x = 0; x < source.width(); ++x) {
            const auto maskOpacity = src[x * 4];
            const auto alpha = (maskOpacity * color.alpha()) / 255;
            dst[x] = qPremultiply(qRgba(color.red(), color.green(), color.blue(), alpha));
        }
    }
    result.setDevicePixelRatio(source.devicePixelRatio());
    return result;
}

/// Load a chat icon and colorize it.
[[nodiscard]] QImage tintedIcon(const QString &basePath, qreal dpr, const QColor &color) {
    static QHash<QString, QImage> cache;
    const auto key = basePath + QLatin1Char('|')
        + QString::number(dpr, 'f', 1) + QLatin1Char('|')
        + QString::number(color.rgba(), 16);
    if (auto i = cache.constFind(key); i != cache.cend()) return *i;

    const auto suffix = (dpr > 2.0) ? QStringLiteral("@3x")
        : (dpr > 1.0) ? QStringLiteral("@2x") : QString();
    auto mask = QImage(basePath + suffix + QStringLiteral(".png"));
    if (mask.isNull()) return {};
    mask.setDevicePixelRatio((dpr > 2.0) ? 3.0 : (dpr > 1.0) ? 2.0 : 1.0);

    auto icon = colorizeMask(mask, color);
    cache.insert(key, icon);
    return icon;
}

[[nodiscard]] QString humanFileSize(quint64 bytes) {
    if (bytes < 1024) {
        return QStringLiteral("%1 B").arg(bytes);
    }
    if (bytes < 1024 * 1024) {
        return QStringLiteral("%1 KB").arg(
            QLocale().toString(bytes / 1024.0, 'f', 1));
    }
    if (bytes < 1024ULL * 1024 * 1024) {
        return QStringLiteral("%1 MB").arg(
            QLocale().toString(bytes / (1024.0 * 1024.0), 'f', 1));
    }
    return QStringLiteral("%1 GB").arg(
        QLocale().toString(bytes / (1024.0 * 1024.0 * 1024.0), 'f', 2));
}

// --------------------------------------------------------
// ImagePreviewWidget — paints a rounded-corner image preview.
// --------------------------------------------------------
class ImagePreviewWidget : public QWidget {
public:
    ImagePreviewWidget(const QImage &preview, int maxW, int maxH,
                       bool isVideo,
                       std::function<void()> onDelete, QWidget *parent)
        : QWidget(parent)
        , _isVideo(isVideo)
        , _onDelete(std::move(onDelete)) {
        _cardW = maxW;
        auto imgW = preview.width();
        auto imgH = preview.height();
        if (imgW > maxW) {
            imgH = imgH * maxW / imgW;
            imgW = maxW;
        }
        if (imgH > maxH) {
            imgW = imgW * maxH / imgH;
            imgH = maxH;
        }
        _displayW = qMax(1, imgW);
        _displayH = qMax(1, imgH);
        // Scale to DEVICE pixels so the preview is crisp on retina (the source
        // is kept high-res in prepareFile, see kPreviewMaxDimension).
        const auto dpr = devicePixelRatioF();
        _scaled = preview.scaled(
            int(_displayW * dpr),
            int(_displayH * dpr),
            Qt::KeepAspectRatio,
            Qt::SmoothTransformation);
        _scaled.setDevicePixelRatio(dpr);
        setFixedHeight(_displayH);
        setMouseTracking(true);
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        PainterHighQualityEnabler hq(p);

        // Grey backdrop card spanning the content width (image background), so
        // images sit on grey rather than white — visible as side bars for a
        // portrait image, and behind any transparency.
        const auto cardLeft = (width() - _cardW) / 2;
        const QRect cardRect(cardLeft, 0, _cardW, _displayH);
        {
            QPainterPath cardPath;
            cardPath.addRoundedRect(
                QRectF(cardRect), kPreviewImageRadius, kPreviewImageRadius);
            p.fillPath(cardPath, st::windowBgOver);
        }

        const auto imgX = (width() - _displayW) / 2;
        const QRect imgRect(imgX, 0, _displayW, _displayH);
        QPainterPath clipPath;
        clipPath.addRoundedRect(
            QRectF(imgRect),
            kPreviewImageRadius,
            kPreviewImageRadius);
        p.setClipPath(clipPath);
        p.drawImage(imgRect, _scaled);

        // Video play button (centered, non-interactive, half size).
        if (_isVideo) {
            const auto dpr = devicePixelRatioF();
            p.setRenderHint(QPainter::SmoothPixmapTransform);
            // Background circle.
            const auto bgIcon = tintedIcon(
                QStringLiteral(":/telematrix/icons/chat/media_video_play_bg"),
                dpr, st::msgDateImgBg);
            if (!bgIcon.isNull()) {
                const auto bw = int(bgIcon.width() / bgIcon.devicePixelRatio()) / 2;
                const auto bh = int(bgIcon.height() / bgIcon.devicePixelRatio()) / 2;
                const QRect bgRect(imgRect.x() + (imgRect.width() - bw) / 2,
                                   imgRect.y() + (imgRect.height() - bh) / 2, bw, bh);
                p.drawImage(bgRect, bgIcon);
            }
            // Play triangle.
            const auto playIcon = tintedIcon(
                QStringLiteral(":/telematrix/icons/chat/media_video_play"),
                dpr, st::historyIconFgInverted);
            if (!playIcon.isNull()) {
                const auto pw = int(playIcon.width() / playIcon.devicePixelRatio()) / 2;
                const auto ph = int(playIcon.height() / playIcon.devicePixelRatio()) / 2;
                const QRect playRect(imgRect.x() + (imgRect.width() - pw) / 2,
                                     imgRect.y() + (imgRect.height() - ph) / 2, pw, ph);
                p.drawImage(playRect, playIcon);
            }
        }

        // Delete button: drawn UNCLIPPED so a portrait image's rounded clip
        // can't hide it (it sits at the image's top-right, see deleteButtonRect).
        p.setClipping(false);
        if (_onDelete) {
            const auto btnRect = deleteButtonRect();
            p.setPen(Qt::NoPen);
            p.setBrush(_hoverDelete
                ? st::sendMediaDeleteBgOver
                : st::sendMediaDeleteBg);
            p.drawRoundedRect(btnRect, kDeleteButtonRadius, kDeleteButtonRadius);

            // Cross (X) remove icon — close-button style, replacing the bin.
            {
                PainterHighQualityEnabler hq(p);
                QPen crossPen(st::historyIconFgInverted);
                crossPen.setWidthF(1.5);
                crossPen.setCapStyle(Qt::RoundCap);
                p.setPen(crossPen);
                const auto inset = btnRect.width() * 0.3;
                const QRectF cr = QRectF(btnRect).adjusted(inset, inset, -inset, -inset);
                p.drawLine(cr.topLeft(), cr.bottomRight());
                p.drawLine(cr.topRight(), cr.bottomLeft());
            }
        }
    }

    void mouseMoveEvent(QMouseEvent *e) override {
        const auto hover = _onDelete && deleteButtonRect().contains(e->pos());
        if (hover != _hoverDelete) {
            _hoverDelete = hover;
            setCursor(hover ? Qt::PointingHandCursor : Qt::ArrowCursor);
            update();
        }
    }

    void leaveEvent(QEvent *) override {
        if (_hoverDelete) {
            _hoverDelete = false;
            setCursor(Qt::ArrowCursor);
            update();
        }
    }

    void mouseReleaseEvent(QMouseEvent *e) override {
        if (e->button() == Qt::LeftButton
            && _onDelete
            && deleteButtonRect().contains(e->pos())) {
            _onDelete();
        }
    }

private:
    [[nodiscard]] QRect deleteButtonRect() const {
        // Top-right of the (centered) image, NOT the full widget — otherwise it
        // lands in the side margin outside a portrait image and is invisible.
        const auto imgRight = (width() + _displayW) / 2;
        return QRect(
            imgRight - kDeleteButtonSkipRight - kDeleteButtonWidth,
            kDeleteButtonSkipTop,
            kDeleteButtonWidth,
            kDeleteButtonHeight);
    }

    QImage _scaled;
    int _cardW = 0;     // grey backdrop width (content width)
    int _displayW = 0;  // logical display size (device-independent)
    int _displayH = 0;
    bool _isVideo = false;
    std::function<void()> _onDelete;
    bool _hoverDelete = false;
};

// --------------------------------------------------------
// FileCardWidget — paints a file card.
// --------------------------------------------------------
/// File card matching the file bubble in message history.
/// Uses the same icon, fonts, sizes, and colors as HistoryMessage::paint for ContentType::File.
class FileCardWidget : public QWidget {
public:
    FileCardWidget(
            const QString &filename,
            const QString &mime,
            quint64 size,
            std::function<void()> onDelete,
            QWidget *parent)
        : QWidget(parent)
        , _filename(filename)
        , _mime(mime)
        , _sizeText(humanFileSize(size))
        , _onDelete(std::move(onDelete)) {
        // Same height as file bubble inner: docPaddingTop + docThumbSize + docPaddingBottom.
        setFixedHeight(st::docPaddingTop + st::docThumbSize + st::docPaddingBottom);
        setMouseTracking(true);
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        PainterHighQualityEnabler hq(p);
        const auto dpr = devicePixelRatioF();

        // Icon circle: same as message bubble (docThumbSize = 44, msgFileInBg color).
        const QRect iconRect(
            st::docPaddingLeft,
            st::docPaddingTop,
            st::docThumbSize,
            st::docThumbSize);
        p.setPen(Qt::NoPen);
        p.setBrush(st::msgFileInBg);
        p.drawEllipse(iconRect);

        // Icon: same as timeline bubbles — play for audio, document for files.
        {
            const auto isAudio = _mime.startsWith(QStringLiteral("audio/"));
            const auto iconName = isAudio
                ? QStringLiteral(":/telematrix/icons/chat/history_file_play")
                : QStringLiteral(":/telematrix/icons/chat/history_file_document");
            const auto iconColor = st::historyFileInIconFg;
            const auto icon = tintedIcon(iconName, dpr, iconColor);
            if (!icon.isNull()) {
                const auto iw = int(icon.width() / icon.devicePixelRatio());
                const auto ih = int(icon.height() / icon.devicePixelRatio());
                p.drawImage(QPoint(
                    iconRect.x() + (iconRect.width() - iw) / 2,
                    iconRect.y() + (iconRect.height() - ih) / 2), icon);
            }
        }

        // Filename: semiboldFont at docNameTop.
        const auto textLeft = st::docNameLeft;
        const auto textWidth = width() - textLeft - st::docPaddingRight
            - (_onDelete ? kDeleteButtonWidth + kDeleteButtonSkipRight + 4 : 0);
        p.setFont(st::semiboldFont);
        p.setPen(st::windowFg);
        const auto elidedName = QFontMetrics(st::semiboldFont).elidedText(
            _filename, Qt::ElideMiddle, qMax(1, textWidth));
        p.drawText(textLeft, st::docNameTop + st::semiboldFont->ascent, elidedName);

        // File size: msgFont at docStatusTop.
        p.setFont(st::msgFont);
        p.setPen(st::windowSubTextFg);
        const auto elidedSize = QFontMetrics(st::msgFont).elidedText(
            _sizeText, Qt::ElideRight, qMax(1, textWidth));
        p.drawText(textLeft, st::docStatusTop + st::msgFont->ascent, elidedSize);

        // Delete: cross (X), no background, colorized to match text.
        if (_onDelete) {
            const auto btnRect = deleteButtonRect();
            PainterHighQualityEnabler hq(p);
            QPen crossPen(_hoverDelete ? st::windowFg : st::windowSubTextFg);
            crossPen.setWidthF(1.5);
            crossPen.setCapStyle(Qt::RoundCap);
            p.setPen(crossPen);
            const auto inset = btnRect.width() * 0.3;
            const QRectF cr = QRectF(btnRect).adjusted(inset, inset, -inset, -inset);
            p.drawLine(cr.topLeft(), cr.bottomRight());
            p.drawLine(cr.topRight(), cr.bottomLeft());
        }
    }

    void mouseMoveEvent(QMouseEvent *e) override {
        const auto hover = _onDelete && deleteButtonRect().contains(e->pos());
        if (hover != _hoverDelete) {
            _hoverDelete = hover;
            setCursor(hover ? Qt::PointingHandCursor : Qt::ArrowCursor);
            update();
        }
    }

    void leaveEvent(QEvent *) override {
        if (_hoverDelete) {
            _hoverDelete = false;
            setCursor(Qt::ArrowCursor);
            update();
        }
    }

    void mouseReleaseEvent(QMouseEvent *e) override {
        if (e->button() == Qt::LeftButton
            && _onDelete
            && deleteButtonRect().contains(e->pos())) {
            _onDelete();
        }
    }

private:
    [[nodiscard]] QRect deleteButtonRect() const {
        return QRect(
            width() - kDeleteButtonSkipRight - kDeleteButtonWidth,
            (height() - kDeleteButtonHeight) / 2,
            kDeleteButtonWidth,
            kDeleteButtonHeight);
    }

    QString _filename;
    QString _mime;
    QString _sizeText;
    std::function<void()> _onDelete;
    bool _hoverDelete = false;
};

// --------------------------------------------------------
// CompressToggle — compact checkbox + label for the button bar.
// --------------------------------------------------------
/// Toggles whether selected images are downscaled + re-encoded before sending
/// (Telegram's "compress" behavior). Painted to match the hand-drawn dialog.
class CompressToggle final : public QWidget {
public:
    CompressToggle(
            const QString &text,
            const QString &hint,
            bool checked,
            int maxWidth,
            QWidget *parent)
        : QWidget(parent)
        , _text(text)
        , _checked(checked) {
        setCursor(Qt::PointingHandCursor);
        // The description is shown as a hover tooltip, not an inline line.
        setToolTip(hint);
        const auto labelW = QFontMetrics(st::msgFont).horizontalAdvance(text);
        const auto wanted = kLeftPad + kBox + kGap + labelW + 2;
        setFixedSize(qMin(wanted, maxWidth), 52); // kButtonBarHeight
    }

    [[nodiscard]] bool checked() const { return _checked; }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        PainterHighQualityEnabler hq(p);

        // Single line: checkbox + caption, vertically centered.
        const QRectF box(
            kLeftPad,
            (height() - kBox) / 2.0,
            kBox, kBox);
        if (_checked) {
            p.setPen(Qt::NoPen);
            p.setBrush(st::windowActiveTextFg);
            p.drawRoundedRect(box, 3, 3);

            QPen pen(Qt::white, 1.8);
            pen.setCapStyle(Qt::RoundCap);
            pen.setJoinStyle(Qt::RoundJoin);
            p.setPen(pen);
            p.setBrush(Qt::NoBrush);
            QPainterPath tick;
            tick.moveTo(box.left() + kBox * 0.26, box.top() + kBox * 0.52);
            tick.lineTo(box.left() + kBox * 0.44, box.top() + kBox * 0.70);
            tick.lineTo(box.left() + kBox * 0.74, box.top() + kBox * 0.32);
            p.drawPath(tick);
        } else {
            QPen pen(st::windowSubTextFg, 1.4);
            p.setPen(pen);
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(box, 3, 3);
        }

        const auto textLeft = kLeftPad + kBox + kGap;
        const auto textWidth = width() - textLeft - 2;

        // Caption uses one stable color (no hover/inactive variant).
        p.setFont(st::msgFont);
        p.setPen(st::windowFg);
        p.drawText(
            QRect(textLeft, 0, textWidth, height()),
            Qt::AlignLeft | Qt::AlignVCenter,
            QFontMetrics(st::msgFont).elidedText(_text, Qt::ElideRight, textWidth));
    }

    void mouseReleaseEvent(QMouseEvent *e) override {
        if (e->button() == Qt::LeftButton && rect().contains(e->pos())) {
            _checked = !_checked;
            update();
        }
    }

private:
    static constexpr int kBox = 16;
    static constexpr int kGap = 8;
    static constexpr int kLeftPad = 2; // keep the box stroke off the widget edge
    QString _text;
    bool _checked = true;
};

} // namespace

HistorySendFilesDialog::HistorySendFilesDialog(
        QWidget *parent,
        const QVector<PreparedFile> &files,
        int sendSubmitWay,
        bool compressImagesDefault)
    : QDialog(parent, Qt::Dialog | Qt::FramelessWindowHint)
    , _files(files)
    , _sendSubmitWay(static_cast<InputSubmitSettings>(sendSubmitWay))
    , _compressImagesDefault(compressImagesDefault) {
    setAttribute(Qt::WA_TranslucentBackground);
    setModal(true);
    setupUI();
}

QString HistorySendFilesDialog::caption() const {
    return _captionField ? _captionField->toPlainText() : QString();
}

const QVector<PreparedFile> &HistorySendFilesDialog::files() const {
    return _files;
}

bool HistorySendFilesDialog::compressImages() const {
    // Gate on hasImages() (current file state), NOT the toggle's isVisible():
    // QDialog::accept() hides the dialog before exec() returns, so isVisible()
    // would be false by the time the caller reads this.
    return _compressToggle
        && hasImages()
        && static_cast<CompressToggle *>(_compressToggle)->checked();
}

QString HistorySendFilesDialog::dialogTitle() const {
    return tr("Upload");
}

void HistorySendFilesDialog::setupUI() {
    // Calculate total preview content height.
    const auto availW = kDialogWidth - 2 * kImagePreviewPadding;
    int contentHeight = 0;
    for (const auto &file : _files) {
        const auto hasVisualPreview = !file.preview.isNull()
            && (file.kind == PreparedFileKind::Image || file.kind == PreparedFileKind::Video);
        if (hasVisualPreview) {
            auto imgW = file.preview.width();
            auto imgH = file.preview.height();
            if (imgW > availW) {
                imgH = imgH * availW / imgW;
            }
            contentHeight += qMin(imgH, kPreviewMaxHeight) + kImagePreviewPadding;
        } else {
            contentHeight += st::docPaddingTop + st::docThumbSize + st::docPaddingBottom + kFileCardPadding;
        }
    }
    // "Compress images" sits in its own row above the caption (images only).
    const auto compressBarHeight = hasImages() ? kButtonBarHeight : 0;

    // Grow the preview (and the dialog) up to the available window height so
    // multiple files are visible before the list has to scroll.
    const auto chromeHeight = kTitleBarHeight + compressBarHeight
        + kCaptionHeight + kButtonBarHeight;
    const auto availHeight = parentWidget() ? parentWidget()->height() : 720;
    const auto maxPreviewHeight = qMax(
        kPreviewMaxHeight,
        availHeight - chromeHeight - 2 * kDialogPadding);
    const auto previewHeight = qMin(contentHeight, maxPreviewHeight);

    const auto totalHeight = kTitleBarHeight
        + previewHeight
        + compressBarHeight
        + kCaptionHeight
        + kButtonBarHeight;

    setFixedSize(kDialogWidth, totalHeight);

    // Center on parent.
    if (parentWidget()) {
        const auto pg = parentWidget()->geometry();
        move(
            pg.x() + (pg.width() - width()) / 2,
            pg.y() + (pg.height() - height()) / 2);
    }

    // Preview scroll area.
    _previewArea = new QScrollArea(this);
    _previewArea->setFrameShape(QFrame::NoFrame);
    _previewArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    _previewArea->setVerticalScrollBarPolicy(
        contentHeight > previewHeight
            ? Qt::ScrollBarAsNeeded
            : Qt::ScrollBarAlwaysOff);
    _previewArea->setWidgetResizable(true);
    // Transparent scroll area + custom 4px capsule scrollbar (was QSS).
    {
        auto *bar = new PreviewScrollBar(_previewArea);
        bar->setFixedWidth(4);
        _previewArea->setVerticalScrollBar(bar);
        if (auto *vp = _previewArea->viewport()) {
            vp->setAutoFillBackground(false);
            QPalette pal = vp->palette();
            pal.setColor(QPalette::Window, Qt::transparent);
            pal.setColor(QPalette::Base, Qt::transparent);
            vp->setPalette(pal);
        }
    }
    _previewArea->setGeometry(
        0, kTitleBarHeight,
        kDialogWidth, previewHeight);

    _previewContainer = new QWidget;
    _previewContainer->setMaximumWidth(kDialogWidth);

    int yOffset = 0;
    for (int i = 0; i < _files.size(); ++i) {
        const auto &file = _files[i];
        const auto hasVisualPreview = !file.preview.isNull()
            && (file.kind == PreparedFileKind::Image || file.kind == PreparedFileKind::Video);
        if (hasVisualPreview) {
            addMediaPreview(i, file, _previewContainer, yOffset);
        } else {
            addFilePreview(i, file, _previewContainer, yOffset);
        }
    }
    _previewContainer->setFixedHeight(qMax(yOffset, 1));
    _previewArea->setWidget(_previewContainer);

    // Caption field: flat, bottom border only, no rounded corners.
    // border 1px inputBorderFg, borderActive 2px activeLineFg.
    const auto captionY = kTitleBarHeight + previewHeight + compressBarHeight;
    // boxPadding 24px left/right, inputBorderFg border, activeLineFg focus.
    constexpr int kBoxPadding = 24;
    const auto captionLeft = kBoxPadding;
    const auto captionWidth = kDialogWidth - 2 * kBoxPadding;
    _captionField = new CaptionField(
        this,
        [this] { accept(); },
        [this](int newH) {
            // Grow dialog downward when caption expands. The compress row sits
            // ABOVE the caption, so it stays put — only the button bar moves.
            const auto compressBarHeight = _compressToggle ? kButtonBarHeight : 0;
            setFixedHeight(kTitleBarHeight
                + _previewArea->height()
                + compressBarHeight
                + newH
                + kButtonBarHeight);
            const auto captionY = kTitleBarHeight + _previewArea->height()
                + compressBarHeight;
            _captionField->setGeometry(
                kBoxPadding, captionY,
                kDialogWidth - 2 * kBoxPadding, newH);
            const auto buttonBarY = captionY + newH;
            const auto buttonWidth = 80;
            const auto buttonSpacing = 10;
            _sendButton->move(
                kDialogWidth - kDialogPadding - buttonWidth,
                buttonBarY + (kButtonBarHeight - st::boxButtonHeight) / 2);
            _cancelButton->move(
                kDialogWidth - kDialogPadding - 2 * buttonWidth - buttonSpacing,
                buttonBarY + (kButtonBarHeight - st::boxButtonHeight) / 2);
        });
    _captionField->setPlaceholderText(tr("Add a caption..."));
    static_cast<CaptionField*>(_captionField)->setSubmitSettings(_sendSubmitWay);
    _captionField->setFixedHeight(kCaptionMinHeight);
    _captionField->setGeometry(
        captionLeft,
        captionY,
        captionWidth,
        kCaptionHeight);
    _captionField->setFont(st::msgFont);

    // Button bar.
    const auto buttonBarY = captionY + kCaptionHeight;
    const auto buttonWidth = 80;
    const auto buttonHeight = st::boxButtonHeight;
    const auto buttonSpacing = 10;

    // Flat light buttons: transparent until hovered (lightButtonBgOver),
    // lightButtonFg text, 6px radius. Painted with live st:: colors.
    ::Ui::TextButton::Style lightStyle;
    lightStyle.bgOver = &st::lightButtonBgOver;  // transparent until hovered
    lightStyle.fg = &st::lightButtonFg;
    lightStyle.radius = 6;
    lightStyle.height = buttonHeight;

    _sendButton = new ::Ui::TextButton(tr("Send"), lightStyle, this);
    _sendButton->setFont(*st::boxButtonFont);
    _sendButton->setFixedSize(buttonWidth, buttonHeight);
    _sendButton->move(
        kDialogWidth - kDialogPadding - buttonWidth,
        buttonBarY + (kButtonBarHeight - buttonHeight) / 2);
    connect(_sendButton, &QAbstractButton::clicked, this, &QDialog::accept);

    _cancelButton = new ::Ui::TextButton(tr("Cancel"), lightStyle, this);
    _cancelButton->setFont(*st::boxButtonFont);
    _cancelButton->setFixedSize(buttonWidth, buttonHeight);
    _cancelButton->move(
        kDialogWidth - kDialogPadding - 2 * buttonWidth - buttonSpacing,
        buttonBarY + (kButtonBarHeight - buttonHeight) / 2);
    connect(_cancelButton, &QAbstractButton::clicked, this, &QDialog::reject);

    // "Compress images" toggle (Telegram-style), in its own row directly above
    // the caption, left-aligned with it (same padding). Images only; default off.
    if (hasImages()) {
        _compressToggle = new CompressToggle(
            tr("Compress images"),
            tr("Reduce size and quality; off sends originals."),
            _compressImagesDefault,
            kDialogWidth - (kBoxPadding - 2) - kBoxPadding,
            this);
        // -2 cancels the toggle's internal kLeftPad so the checkbox's visual
        // left aligns with the caption field at kBoxPadding.
        _compressToggle->move(kBoxPadding - 2, kTitleBarHeight + previewHeight);
        _compressToggle->show();
    }

    _captionField->setFocus();
}

bool HistorySendFilesDialog::hasImages() const {
    return std::any_of(_files.cbegin(), _files.cend(),
        [](const PreparedFile &f) { return f.kind == PreparedFileKind::Image; });
}

void HistorySendFilesDialog::addMediaPreview(
        int index,
        const PreparedFile &file,
        QWidget *container,
        int &yOffset) {
    const auto availW = kDialogWidth - 2 * kImagePreviewPadding;
    auto deleteCallback = std::function<void()>([this, index] { removeFile(index); });
    auto *preview = new ImagePreviewWidget(
        file.preview, availW, kPreviewMaxHeight,
        file.kind == PreparedFileKind::Video,
        std::move(deleteCallback), container);

    preview->setFixedWidth(kDialogWidth);
    preview->move(0, yOffset + kImagePreviewPadding / 2);
    preview->show();

    yOffset += preview->height() + kImagePreviewPadding;
}

void HistorySendFilesDialog::addFilePreview(
        int index,
        const PreparedFile &file,
        QWidget *container,
        int &yOffset) {
    auto deleteCallback = std::function<void()>([this, index] { removeFile(index); });
    auto *card = new FileCardWidget(
        file.filename,
        file.mime,
        file.size,
        std::move(deleteCallback),
        container);
    card->setFixedWidth(kDialogWidth - 2 * kFileCardPadding);
    card->move(kFileCardPadding, yOffset + kFileCardPadding / 2);
    card->show();

    const auto cardHeight = st::docPaddingTop + st::docThumbSize + st::docPaddingBottom;
    yOffset += cardHeight + kFileCardPadding;
}

void HistorySendFilesDialog::removeFile(int index) {
    if (index < 0 || index >= _files.size()) return;
    _files.removeAt(index);
    if (_files.isEmpty()) {
        reject();
        return;
    }
    rebuildPreviews();
    update(); // repaint title
}

void HistorySendFilesDialog::rebuildPreviews() {
    // Delete old preview widgets.
    if (_previewContainer) {
        delete _previewContainer;
        _previewContainer = nullptr;
    }

    // Recalculate content height.
    const auto availW = kDialogWidth - 2 * kImagePreviewPadding;
    int contentHeight = 0;
    for (const auto &file : _files) {
        const auto hasVisualPreview = !file.preview.isNull()
            && (file.kind == PreparedFileKind::Image || file.kind == PreparedFileKind::Video);
        if (hasVisualPreview) {
            auto imgW = file.preview.width();
            auto imgH = file.preview.height();
            if (imgW > availW) {
                imgH = imgH * availW / imgW;
            }
            contentHeight += qMin(imgH, kPreviewMaxHeight) + kImagePreviewPadding;
        } else {
            contentHeight += st::docPaddingTop + st::docThumbSize + st::docPaddingBottom + kFileCardPadding;
        }
    }
    // Keep this layout math in sync with setupUI: a compress row above the
    // caption, and the preview grows up to the available window height (so
    // removing a file shrinks the dialog proportionally instead of snapping to
    // the old fixed cap).
    const auto compressBarHeight = hasImages() ? kButtonBarHeight : 0;
    const auto chromeHeight = kTitleBarHeight + compressBarHeight
        + kCaptionHeight + kButtonBarHeight;
    const auto availHeight = parentWidget() ? parentWidget()->height() : 720;
    const auto maxPreviewHeight = qMax(
        kPreviewMaxHeight,
        availHeight - chromeHeight - 2 * kDialogPadding);
    const auto previewHeight = qMin(contentHeight, maxPreviewHeight);

    // Resize dialog.
    const auto totalHeight = kTitleBarHeight
        + previewHeight
        + compressBarHeight
        + kCaptionHeight
        + kButtonBarHeight;
    setFixedSize(kDialogWidth, totalHeight);

    // Reposition preview area.
    _previewArea->setGeometry(0, kTitleBarHeight, kDialogWidth, previewHeight);
    _previewArea->setVerticalScrollBarPolicy(
        contentHeight > previewHeight ? Qt::ScrollBarAsNeeded : Qt::ScrollBarAlwaysOff);

    // Rebuild preview widgets.
    _previewContainer = new QWidget;
    _previewContainer->setMaximumWidth(kDialogWidth);
    int yOffset = 0;
    for (int i = 0; i < _files.size(); ++i) {
        const auto &file = _files[i];
        const auto hasVisualPreview = !file.preview.isNull()
            && (file.kind == PreparedFileKind::Image || file.kind == PreparedFileKind::Video);
        if (hasVisualPreview) {
            addMediaPreview(i, file, _previewContainer, yOffset);
        } else {
            addFilePreview(i, file, _previewContainer, yOffset);
        }
    }
    _previewContainer->setFixedHeight(qMax(yOffset, 1));
    _previewArea->setWidget(_previewContainer);

    // Reposition caption and buttons.
    const auto captionY = kTitleBarHeight + previewHeight + compressBarHeight;
    constexpr int kBoxPad = 24;
    _captionField->setGeometry(
        kBoxPad, captionY,
        kDialogWidth - 2 * kBoxPad, kCaptionHeight);
    const auto buttonBarY = captionY + kCaptionHeight;
    const auto buttonHeight = st::boxButtonHeight;
    const auto buttonWidth = 80;
    const auto buttonSpacing = 10;
    _sendButton->move(
        kDialogWidth - kDialogPadding - buttonWidth,
        buttonBarY + (kButtonBarHeight - buttonHeight) / 2);
    _cancelButton->move(
        kDialogWidth - kDialogPadding - 2 * buttonWidth - buttonSpacing,
        buttonBarY + (kButtonBarHeight - buttonHeight) / 2);
    if (_compressToggle) {
        _compressToggle->setVisible(hasImages()); // hide if all images removed
        // Above the caption, aligned with it (matches setupUI).
        _compressToggle->move(kBoxPad - 2, kTitleBarHeight + previewHeight);
    }
}

void HistorySendFilesDialog::paintEvent(QPaintEvent *) {
    QPainter p(this);
    PainterHighQualityEnabler hq(p);

    // Dialog background with rounded corners.
    QPainterPath bgPath;
    bgPath.addRoundedRect(QRectF(rect()), kDialogRadius, kDialogRadius);
    p.fillPath(bgPath, st::boxBg);
    p.setClipPath(bgPath);

    // Title.
    const auto titleFont = st::baseFont(15, true);
    p.setFont(titleFont);
    p.setPen(st::boxTitleFg);
    const QFontMetrics titleFm(titleFont);
    p.drawText(
        kDialogPadding,
        kTitleBarHeight / 2 + titleFm.ascent() / 2,
        dialogTitle());

    // Caption bottom border (flat-input style): 2px, highlighted on
    // focus. Painted here (not in the field) so it reliably lands on screen.
    if (_captionField) {
        const auto g = _captionField->geometry();
        const auto &color = _captionField->hasFocus()
            ? st::activeLineFg
            : st::inputBorderFg;
        p.fillRect(g.left(), g.bottom() - 1, g.width(), 2, color);
    }
}

void HistorySendFilesDialog::keyPressEvent(QKeyEvent *e) {
    if (e->key() == Qt::Key_Escape) {
        reject();
        return;
    }
    // Ctrl+Enter sends.
    if ((e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter)
            && (e->modifiers() & Qt::ControlModifier)) {
        accept();
        return;
    }
    QDialog::keyPressEvent(e);
}

} // namespace TeleMatrix
