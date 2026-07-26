// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "settings/settings_common_widgets.h"

#include "styles/style_constants.h"
#include "ui/painter.h"
#include "ui/style/runtime_scale.h"
#include "ui/style/icon_provider.h"
#include "ui/toast_widget.h"

#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPainterPath>
#include <QLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

namespace TeleMatrix {

namespace {

class InlineLoadingSpinner final : public QWidget {
public:
    explicit InlineLoadingSpinner(QWidget *parent = nullptr)
        : QWidget(parent) {
        const auto titleHeight = QFontMetrics(st::settingsSubsectionTitleFont()).height();
        const auto size = qMax(
            Style::ConvertScale(12),
            qMin(Style::ConvertScale(16), titleHeight - Style::ConvertScale(2)));
        setFixedSize(size, size);
        _timer.setInterval(33);
        QObject::connect(&_timer, &QTimer::timeout, this, [this] {
            _angle = (_angle + 30) % 360;
            update();
        });
        _timer.start();
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        auto pen = QPen(st::windowActiveTextFg, st::uploadRadialLine);
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        const auto inset = qMax(Style::ConvertScale(2), width() / 5);
        p.drawArc(rect().adjusted(inset, inset, -inset, -inset),
            (_angle + 35) * 16,
            285 * 16);
    }

private:
    QTimer _timer;
    int _angle = 0;
};

// Flat icon button painted with live st:: colors: transparent normal state,
// rounded windowBgOver-style hover fill (lightButtonBgOver), centred icon.
// Replaces an inline stylesheet so it tracks theme changes and keeps the
// public QPushButton* return type of createSettingsCopyIconButton().
class IconHoverButton final : public QPushButton {
public:
    explicit IconHoverButton(QWidget *parent)
        : QPushButton(parent) {
    }

protected:
    void enterEvent(QEnterEvent *) override {
        _hovered = true;
        update();
    }
    void leaveEvent(QEvent *) override {
        _hovered = false;
        update();
    }
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        if (_hovered) {
            PainterHighQualityEnabler hq(p);
            p.setPen(Qt::NoPen);
            p.setBrush(st::lightButtonBgOver);
            p.drawRoundedRect(rect(), st::boxRadius, st::boxRadius);
        }
        const auto pm = icon().pixmap(iconSize());
        if (!pm.isNull()) {
            const auto dpr = pm.devicePixelRatio();
            const auto w = pm.width() / dpr;
            const auto h = pm.height() / dpr;
            p.drawPixmap(
                QPointF((width() - w) / 2.0, (height() - h) / 2.0),
                pm);
        }
    }

private:
    bool _hovered = false;
};

} // namespace

QImage loadColorizedSettingsIconFromPrefix(
        const QString &prefix,
        const QString &name,
        const QColor &color) {
    return Style::IconProvider::tintedIcon(
        QStringLiteral(":/") + prefix + QLatin1Char('/'), name, color);
}

QImage loadColorizedSettingsIcon(const QString &name, const QColor &color) {
    return loadColorizedSettingsIconFromPrefix(
        QStringLiteral("settings_icons"),
        name,
        color);
}

// (compactSelectorButtonStyleSheet / compactLineEditStyleSheet /
// settingsSliderStyleSheet were removed in the H4 conversion — those widgets
// now custom-paint from st:: instead of using inline stylesheets.)

QPushButton *createSettingsCopyIconButton(QWidget *parent) {
    auto *button = new IconHoverButton(parent);
    button->setCursor(Qt::PointingHandCursor);
    button->setFixedSize(34, 34);
    button->setFlat(true);
    button->setToolTip(QCoreApplication::translate(
        "SettingsWidget",
        "Copy to clipboard"));
    const auto icon = loadColorizedSettingsIconFromPrefix(
        QStringLiteral("telematrix/icons/menu"),
        QStringLiteral("copy"),
        st::lightButtonFg);
    if (!icon.isNull()) {
        button->setIcon(QIcon(QPixmap::fromImage(icon)));
        button->setIconSize(QSize(18, 18));
    }
    return button;
}

SettingsToggleButton::SettingsToggleButton(
        const QString &text,
        bool checked,
        QWidget *parent)
    : QWidget(parent)
    , _text(text)
    , _checked(checked) {
    setFixedHeight(st::settingsButtonHeight);
    setMouseTracking(true);
    setCursor(Qt::PointingHandCursor);
}

void SettingsToggleButton::setChecked(bool checked) {
    if (_checked != checked) {
        _checked = checked;
        update();
        emit toggled(_checked);
    }
}

void SettingsToggleButton::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    if (_hovered) {
        p.fillRect(rect(), st::windowBgOver);
    }

    p.setFont(st::baseFont(14));
    p.setPen(st::settingsCheckboxTextFg);
    const int textLeft = st::settingsButtonPaddingLeft;
    const int textRight = st::settingsButtonPaddingRight
        + st::settingsToggleWidth
        + st::settingsButtonToggleSkip;
    const QRect textRect(textLeft, 0, width() - textLeft - textRight, height());
    p.drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, _text);

    const int toggleX = width() - st::settingsButtonPaddingRight - st::settingsToggleWidth;
    const int toggleY = (height() - st::settingsToggleHeight) / 2;

    const auto toggled = _checked ? 1.0 : 0.0;
    const auto border = st::settingsToggleBorder;
    const auto diameter = st::settingsToggleDiameter;
    const auto extraW = st::settingsToggleExtraWidth;
    const auto shift = st::settingsToggleShift;
    const auto fullWidth = diameter + extraW;
    const auto innerDiameter = diameter - 2 * shift;
    const auto innerRadius = innerDiameter / 2.0;

    const auto left = toggleX + border;
    const auto top = toggleY + border;
    const auto knobLeft = left + qRound((fullWidth - diameter) * toggled);

    const auto fgColor = _checked ? st::settingsToggleToggledFg : st::settingsToggleUntoggledFg;
    const QRectF bgRect(left + shift, top + shift, fullWidth - 2 * shift, innerDiameter);
    p.setPen(Qt::NoPen);
    p.setBrush(fgColor);
    p.drawRoundedRect(bgRect, innerRadius, innerRadius);

    const auto bgColor = _checked ? st::settingsToggleToggledBg : st::settingsToggleUntoggledBg;
    const QRectF fgRect(knobLeft, top, diameter, diameter);
    auto pen = QPen(fgColor);
    pen.setWidth(border);
    p.setPen(pen);
    p.setBrush(bgColor);
    p.drawEllipse(fgRect);
}

void SettingsToggleButton::mousePressEvent(QMouseEvent *e) {
    if (e->button() == Qt::LeftButton) {
        setChecked(!_checked);
    }
}

void SettingsToggleButton::enterEvent(QEnterEvent *) {
    _hovered = true;
    update();
}

void SettingsToggleButton::leaveEvent(QEvent *) {
    _hovered = false;
    update();
}

SettingsMenuButton::SettingsMenuButton(
        IconType icon,
        const QString &text,
        QWidget *parent)
    : QWidget(parent)
    , _iconType(icon)
    , _text(text) {
    QString iconName;
    switch (_iconType) {
    case IconType::MyAccount:
        iconName = QStringLiteral("menu_profile");
        break;
    case IconType::Notifications:
        iconName = QStringLiteral("menu_notifications");
        break;
    case IconType::Encryption:
        iconName = QStringLiteral("menu_lock");
        break;
    case IconType::Sessions:
        iconName = QStringLiteral("menu_devices");
        break;
    case IconType::Appearance:
        iconName = QStringLiteral("menu_palette");
        break;
    case IconType::Preferences:
        iconName = QStringLiteral("menu_settings");
        break;
    case IconType::Advanced:
        iconName = QStringLiteral("menu_advanced");
        break;
    case IconType::HelpAbout:
        iconName = QStringLiteral("menu_faq");
        break;
    }
    _icon = loadColorizedSettingsIcon(iconName, st::settingsMenuIconFg);
    _iconOver = loadColorizedSettingsIcon(iconName, st::settingsMenuIconFgOver);

    setFixedHeight(st::settingsButtonHeight);
    setMouseTracking(true);
    setCursor(Qt::PointingHandCursor);
}

void SettingsMenuButton::setSelected(bool selected) {
    if (_selected != selected) {
        _selected = selected;
        update();
    }
}

void SettingsMenuButton::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    if (_selected || _hovered) {
        const int margin = 4;
        const QRectF bg(margin, 2, width() - 2 * margin, height() - 4);
        p.setPen(Qt::NoPen);
        p.setBrush(_selected ? st::settingsSidebarSelectedBg : st::windowBgOver);
        p.drawRoundedRect(bg, st::settingsSidebarSelectedRadius, st::settingsSidebarSelectedRadius);
    }

    const auto &img = (_selected || _hovered) ? _iconOver : _icon;
    if (!img.isNull()) {
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        const int iconH = qRound(img.height() / img.devicePixelRatio());
        p.drawImage(
            st::settingsButtonIconLeft,
            (height() - iconH) / 2,
            img);
    }

    p.setFont(st::baseFont(14));
    p.setPen(st::settingsCheckboxTextFg);
    const int textLeft = st::settingsButtonWithIconPaddingLeft;
    const QRect textRect(textLeft, 0, width() - textLeft - st::settingsButtonPaddingRight, height());
    p.drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, _text);
}

void SettingsMenuButton::mousePressEvent(QMouseEvent *e) {
    if (e->button() == Qt::LeftButton) {
        emit clicked();
    }
}

void SettingsMenuButton::enterEvent(QEnterEvent *) {
    _hovered = true;
    update();
}

void SettingsMenuButton::leaveEvent(QEvent *) {
    _hovered = false;
    update();
}

SettingsValueButton::SettingsValueButton(
        const QString &text,
        const QString &value,
        QWidget *parent,
        const QString &iconPrefix,
        const QString &iconName)
    : QWidget(parent)
    , _text(text)
    , _value(value)
    , _hasIcon(!iconPrefix.isEmpty() && !iconName.isEmpty()) {
    if (_hasIcon) {
        _icon = loadColorizedSettingsIconFromPrefix(
            iconPrefix,
            iconName,
            st::settingsMenuIconFg);
        _iconOver = loadColorizedSettingsIconFromPrefix(
            iconPrefix,
            iconName,
            st::settingsMenuIconFgOver);
    }
    setFixedHeight(st::settingsButtonHeight);
    setMouseTracking(true);
    setCursor(Qt::ArrowCursor);
}

void SettingsValueButton::setValue(const QString &value) {
    if (_value == value) {
        return;
    }
    _value = value;
    update();
}

void SettingsValueButton::setText(const QString &text) {
    if (_text == text) {
        return;
    }
    _text = text;
    update();
}

void SettingsValueButton::setClickedCallback(std::function<void()> callback) {
    _clicked = std::move(callback);
    setCursor(_clicked ? Qt::PointingHandCursor : Qt::ArrowCursor);
}

void SettingsValueButton::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    if (_clicked && _hovered) {
        p.fillRect(rect(), st::windowBgOver);
    }

    const auto textFont = st::baseFont(14);
    const auto valueFont = st::baseFont(14);
    const QFontMetrics textMetrics(textFont);
    const QFontMetrics valueMetrics(valueFont);
    const int left = _hasIcon
        ? st::settingsButtonWithIconPaddingLeft
        : st::settingsButtonPaddingLeft;
    if (_hasIcon) {
        const auto &icon = _hovered ? _iconOver : _icon;
        if (!icon.isNull()) {
            p.setRenderHint(QPainter::SmoothPixmapTransform);
            const int iconH = qRound(icon.height() / icon.devicePixelRatio());
            p.drawImage(
                st::settingsButtonIconLeft,
                (height() - iconH) / 2,
                icon);
        }
    }
    const int valueRight = st::settingsButtonRightSkip;
    const int valueMaxWidth = qMax(
        0,
        width()
            - left
            - st::settingsButtonPaddingRight
            - textMetrics.horizontalAdvance(_text)
            - valueRight);
    const auto value = valueMetrics.elidedText(_value, Qt::ElideRight, valueMaxWidth);
    const int valueWidth = value.isEmpty() ? 0 : valueMetrics.horizontalAdvance(value);
    const int valueLeft = width() - valueRight - valueWidth;
    const int titleRight = value.isEmpty()
        ? width() - st::settingsButtonPaddingRight
        : valueLeft - 12;
    const QRect titleRect(left, 0, qMax(0, titleRight - left), height());

    p.setFont(textFont);
    p.setPen(st::settingsCheckboxTextFg);
    p.drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter,
        textMetrics.elidedText(_text, Qt::ElideRight, titleRect.width()));

    if (!value.isEmpty()) {
        p.setFont(valueFont);
        p.setPen(st::windowActiveTextFg);
        p.drawText(QRect(valueLeft, 0, valueWidth, height()),
            Qt::AlignRight | Qt::AlignVCenter, value);
    }
}

void SettingsValueButton::mousePressEvent(QMouseEvent *e) {
    if (e->button() == Qt::LeftButton && _clicked) {
        _clicked();
    }
}

void SettingsValueButton::enterEvent(QEnterEvent *) {
    _hovered = true;
    update();
}

void SettingsValueButton::leaveEvent(QEvent *) {
    _hovered = false;
    update();
}

SettingsExpandButton::SettingsExpandButton(
        const QString &text,
        bool expanded,
        QWidget *parent)
    : QWidget(parent)
    , _text(text)
    , _expanded(expanded) {
    setFixedHeight(st::settingsButtonHeight);
    setMouseTracking(true);
    setCursor(Qt::PointingHandCursor);
}

void SettingsExpandButton::setClickedCallback(std::function<void()> callback) {
    _clicked = std::move(callback);
}

void SettingsExpandButton::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    if (_hovered) {
        p.fillRect(rect(), st::windowBgOver);
    }

    const auto font = st::baseFont(14);
    const QFontMetrics metrics(font);
    const QRect textRect(
        st::settingsButtonPaddingLeft,
        0,
        qMax(0, width() - st::settingsButtonPaddingLeft - st::settingsButtonPaddingRight),
        height());
    p.setFont(font);
    p.setPen(st::windowFg);
    p.drawText(
        textRect,
        Qt::AlignLeft | Qt::AlignVCenter,
        metrics.elidedText(_text, Qt::ElideRight, textRect.width()));

    // Same chevron as the main menu's account switcher, so "this opens more"
    // looks the same wherever it appears.
    const auto size = st::mainMenuToggleSize;
    const auto centre = QPointF(
        width() - st::settingsChevronRight,
        height() / 2.0);
    const auto rise = size / 2.0;
    QPainterPath path;
    if (_expanded) {
        path.moveTo(centre.x() - size / 2.0, centre.y() + rise / 2.0);
        path.lineTo(centre.x(), centre.y() - rise / 2.0);
        path.lineTo(centre.x() + size / 2.0, centre.y() + rise / 2.0);
    } else {
        path.moveTo(centre.x() - size / 2.0, centre.y() - rise / 2.0);
        path.lineTo(centre.x(), centre.y() + rise / 2.0);
        path.lineTo(centre.x() + size / 2.0, centre.y() - rise / 2.0);
    }
    auto pen = QPen(st::windowBoldFg);
    pen.setWidthF(1.5);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);
}

void SettingsExpandButton::mousePressEvent(QMouseEvent *e) {
    if (e->button() == Qt::LeftButton && _clicked) {
        _clicked();
    }
}

void SettingsExpandButton::enterEvent(QEnterEvent *) {
    _hovered = true;
    update();
}

void SettingsExpandButton::leaveEvent(QEvent *) {
    _hovered = false;
    update();
}

SettingsLinkButton::SettingsLinkButton(
        const QString &text,
        const QColor &color,
        QWidget *parent)
    : QWidget(parent)
    , _text(text)
    , _color(color) {
    setFixedHeight(st::settingsButtonHeight);
    setMouseTracking(true);
    setCursor(Qt::PointingHandCursor);
}

void SettingsLinkButton::setClickedCallback(std::function<void()> callback) {
    _clicked = std::move(callback);
}

void SettingsLinkButton::setText(const QString &text) {
    _text = text;
    update();
}

void SettingsLinkButton::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    if (_hovered) {
        p.fillRect(rect(), st::windowBgOver);
    }

    const auto font = st::baseFont(14);
    const QFontMetrics metrics(font);
    const QRect textRect(
        st::settingsButtonPaddingLeft,
        0,
        qMax(0, width() - st::settingsButtonPaddingLeft - st::settingsButtonPaddingRight),
        height());
    p.setFont(font);
    p.setPen(_color);
    p.drawText(
        textRect,
        Qt::AlignLeft | Qt::AlignVCenter,
        metrics.elidedText(_text, Qt::ElideRight, textRect.width()));
}

void SettingsLinkButton::mousePressEvent(QMouseEvent *e) {
    if (e->button() == Qt::LeftButton && _clicked) {
        _clicked();
    }
}

void SettingsLinkButton::enterEvent(QEnterEvent *) {
    _hovered = true;
    update();
}

void SettingsLinkButton::leaveEvent(QEvent *) {
    _hovered = false;
    update();
}

SettingsChoiceRow::SettingsChoiceRow(
        SettingsChoiceEntry entry,
        bool checked,
        QWidget *parent)
    : QWidget(parent)
    , _entry(std::move(entry))
    , _checked(checked) {
    setFixedHeight(_entry.subtitle.isEmpty()
        ? st::settingsButtonHeight
        : st::internalChoiceSubtitleRowHeight);
    setMouseTracking(true);
    setCursor(_entry.enabled ? Qt::PointingHandCursor : Qt::ArrowCursor);
}

void SettingsChoiceRow::setChecked(bool checked) {
    if (_checked == checked) {
        return;
    }
    _checked = checked;
    update();
}

void SettingsChoiceRow::setClickedCallback(std::function<void(QString)> callback) {
    _clicked = std::move(callback);
}

void SettingsChoiceRow::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    if (_entry.enabled && _hovered) {
        p.fillRect(rect(), st::windowBgOver);
    }

    const int left = st::settingsButtonPaddingLeft;
    const int right = st::settingsButtonPaddingRight;
    const int radioSize = st::internalChoiceRadioSize;
    const int radioLeft = width() - right - radioSize;
    const int textRight = radioLeft - st::internalChoiceTextRadioSkip;
    const QRect textRect(left, 0, qMax(0, textRight - left), height());

    auto titleFont = _entry.titleFont;
    if (titleFont.family().isEmpty()) {
        titleFont = st::baseFont(14);
    }
    const QFontMetrics titleMetrics(titleFont);
    p.setFont(titleFont);
    p.setPen(_entry.enabled ? st::settingsCheckboxTextFg : st::windowSubTextFg);
    if (_entry.subtitle.isEmpty()) {
        p.drawText(
            textRect,
            Qt::AlignLeft | Qt::AlignVCenter,
            titleMetrics.elidedText(_entry.title, Qt::ElideRight, textRect.width()));
    } else {
        const int titleY = st::internalChoiceSubtitleTitleTop
            + titleMetrics.ascent();
        p.drawText(
            left,
            titleY,
            titleMetrics.elidedText(_entry.title, Qt::ElideRight, textRect.width()));

        const auto subtitleFont = st::baseFont(13);
        const QFontMetrics subtitleMetrics(subtitleFont);
        p.setFont(subtitleFont);
        p.setPen(st::windowSubTextFg);
        const int subtitleY = titleY
            + st::internalChoiceSubtitleSkip
            + subtitleMetrics.ascent();
        p.drawText(
            left,
            subtitleY,
            subtitleMetrics.elidedText(_entry.subtitle, Qt::ElideRight, textRect.width()));
    }

    const QRect radioRect(
        radioLeft,
        (height() - radioSize) / 2,
        radioSize,
        radioSize);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(_checked ? st::windowActiveTextFg : st::windowSubTextFg, 2));
    p.drawEllipse(radioRect);
    if (_checked) {
        p.setPen(Qt::NoPen);
        p.setBrush(st::windowActiveTextFg);
        p.drawEllipse(radioRect.adjusted(5, 5, -5, -5));
    }
}

void SettingsChoiceRow::mousePressEvent(QMouseEvent *e) {
    if (e->button() == Qt::LeftButton && _entry.enabled && _clicked) {
        _clicked(_entry.id);
    }
}

void SettingsChoiceRow::enterEvent(QEnterEvent *) {
    if (!_entry.enabled) {
        return;
    }
    _hovered = true;
    update();
}

void SettingsChoiceRow::leaveEvent(QEvent *) {
    if (_hovered) {
        _hovered = false;
        update();
    }
}

void addSettingsSectionTitle(
        QWidget *parent,
        QVBoxLayout *layout,
        const QString &title,
        const QString &error,
        bool loading) {
    auto *row = new QWidget(parent);
    auto *rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(
        st::settingsButtonPaddingLeft,
        st::settingsSubsectionTitleTop,
        st::settingsButtonPaddingRight,
        st::settingsSubsectionTitleBottom);
    rowLayout->setSpacing(12);

    auto *label = new QLabel(title, row);
    label->setFont(st::settingsSubsectionTitleFont());
    {
        QPalette pal = label->palette();
        pal.setColor(QPalette::WindowText, st::windowActiveTextFg);
        label->setPalette(pal);
    }
    rowLayout->addWidget(label, 0, Qt::AlignVCenter);
    if (loading) {
        rowLayout->addWidget(new InlineLoadingSpinner(row), 0, Qt::AlignVCenter);
    }
    if (!error.isEmpty()) {
        auto *errorLabel = new QLabel(error, row);
        errorLabel->setFont(st::baseFont(12));
        {
            QPalette pal = errorLabel->palette();
            pal.setColor(QPalette::WindowText, st::attentionButtonFg);
            errorLabel->setPalette(pal);
        }
        rowLayout->addWidget(errorLabel, 1, Qt::AlignVCenter);
    } else {
        rowLayout->addStretch(1);
    }
    layout->addWidget(row);
}

void addSettingsDivider(QWidget *parent, QVBoxLayout *layout) {
    auto *line = new QWidget(parent);
    line->setFixedHeight(1);
    line->setAutoFillBackground(true);
    {
        QPalette pal = line->palette();
        pal.setColor(QPalette::Window, st::shadowFg);
        line->setPalette(pal);
    }
    layout->addWidget(line);
}

SettingsToggleButton *addSettingsToggle(
        QWidget *parent,
        QVBoxLayout *layout,
        const QString &text,
        bool checked) {
    auto *toggle = new SettingsToggleButton(text, checked, parent);
    layout->addWidget(toggle);
    return toggle;
}

namespace {

// Info row that can be clicked (hover highlight + pointer + callback), used for
// e.g. an editable "Display name". A plain QWidget when onClick is unset.
class SettingsInfoRowWidget final : public QWidget {
public:
    using QWidget::QWidget;
    std::function<void()> onClick;

protected:
    void paintEvent(QPaintEvent *) override {
        if (_hovered && onClick) {
            QPainter p(this);
            p.fillRect(rect(), st::windowBgOver);
        }
    }
    void enterEvent(QEnterEvent *) override {
        if (onClick) {
            _hovered = true;
            setCursor(Qt::PointingHandCursor);
            update();
        }
    }
    void leaveEvent(QEvent *) override {
        if (_hovered) {
            _hovered = false;
            setCursor(Qt::ArrowCursor);
            update();
        }
    }
    void mousePressEvent(QMouseEvent *e) override {
        if (onClick && e->button() == Qt::LeftButton) {
            onClick();
        }
    }

private:
    bool _hovered = false;
};

} // namespace

void addSettingsInfoRow(
        QWidget *parent,
        QVBoxLayout *layout,
        const QString &label,
        const QString &value,
        bool monospace,
        bool copyButton,
        std::function<void()> onClick) {
    auto *row = new SettingsInfoRowWidget(parent);
    row->onClick = std::move(onClick);
    row->setFixedHeight(st::settingsButtonHeight);
    auto *rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(
        st::settingsButtonPaddingLeft,
        0,
        st::settingsButtonPaddingRight,
        0);
    rowLayout->setSpacing(8);

    auto *title = new QLabel(label, row);
    title->setFont(st::baseFont(14));
    {
        QPalette pal = title->palette();
        pal.setColor(QPalette::WindowText, st::settingsCheckboxTextFg);
        title->setPalette(pal);
    }
    title->setAttribute(Qt::WA_TransparentForMouseEvents);
    rowLayout->addWidget(title, 0, Qt::AlignVCenter);
    rowLayout->addStretch(1);

    auto *valueLabel = new QLabel(value, row);
    valueLabel->setFont(monospace ? QFontDatabase::systemFont(QFontDatabase::FixedFont)
                                  : st::baseFont(13));
    {
        QPalette pal = valueLabel->palette();
        pal.setColor(QPalette::WindowText, st::windowSubTextFg);
        valueLabel->setPalette(pal);
    }
    // A clickable row must let the click through to the row; a plain row keeps
    // the value selectable.
    if (row->onClick) {
        valueLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    } else {
        valueLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    }
    rowLayout->addWidget(valueLabel, 0, Qt::AlignVCenter);

    // Always reserve the copy-glyph slot on the right so every row's value
    // right-aligns to the same edge; the copy icon (when present) sits outside it.
    constexpr int kCopyButtonSize = 34;
    if (copyButton) {
        auto *button = createSettingsCopyIconButton(row);
        QObject::connect(button, &QPushButton::clicked, row, [value] {
            QGuiApplication::clipboard()->setText(value);
            ::Ui::ShowToast(QCoreApplication::translate(
                "SettingsWidget", "Copied to clipboard"));
        });
        rowLayout->addWidget(button, 0, Qt::AlignVCenter);
    } else {
        rowLayout->addSpacing(kCopyButtonSize);
    }

    layout->addWidget(row);
}

void clearSettingsLayout(QLayout *layout) {
    if (!layout) {
        return;
    }
    while (auto *item = layout->takeAt(0)) {
        if (auto *widget = item->widget()) {
            widget->setParent(nullptr);
            widget->deleteLater();
        }
        if (auto *childLayout = item->layout()) {
            clearSettingsLayout(childLayout);
            childLayout->deleteLater();
        }
        delete item;
    }
}

} // namespace TeleMatrix
