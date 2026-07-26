// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "settings/account/account_settings_page.h"

#include "app/app_controller.h"
#include "app/account.h"
#include "core/core_settings.h"
#include "history/history_confirm_dialog.h"
#include "protocol/media_cache.h"
#include "ui/empty_userpic.h"
#include "ui/style/icon_provider.h"
#include "protocol/protocol_bridge.h"
#include "settings/account/account_change_password_dialog.h"
#include "settings/account/account_edit_name_dialog.h"
#include "settings/account/account_profile_cover.h"
#include "settings/dialogs/interactive_auth_dialog.h"
#include "settings/settings_common_widgets.h"
#include "styles/style_constants.h"
#include "ui/focus_restore.h"
#include "ui/internal_choice_dialog.h"
#include "ui/painter.h"
#include "ui/safe_url.h"
#include "ui/style/runtime_scale.h"
#include "ui/widgets/scroll_area.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QFont>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QPushButton>
#include <QScrollBar>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <utility>

namespace TeleMatrix {
namespace {

// A row in the settings account switcher, drawn like the main menu's: avatar (or
// the round accent disc for "Add Account") in the left column, identity beside
// it. `accountIndex < 0` makes it the add-account row.
class SettingsAccountRow final : public QWidget {
public:
    SettingsAccountRow(
        AppController *controller,
        int accountIndex,
        QString label,
        QWidget *parent)
    : QWidget(parent)
    , _controller(controller)
    , _accountIndex(accountIndex)
    , _label(std::move(label)) {
        setFixedHeight(st::settingsButtonHeight);
        setMouseTracking(true);
        setCursor(Qt::PointingHandCursor);
        if (_accountIndex < 0) {
            _icon = Style::IconProvider::tintedIcon(
                QStringLiteral(":/telematrix/icons/menu/"),
                QStringLiteral("add_account"),
                st::activeButtonFg);
        }
    }

    void setClickedCallback(std::function<void()> callback) {
        _clicked = std::move(callback);
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        if (_hovered) {
            p.fillRect(rect(), st::windowBgOver);
        }

        const auto size = st::settingsAccountAvatarSize;
        const auto left = st::settingsButtonPaddingLeft;
        const auto top = (height() - size) / 2;
        if (_accountIndex < 0) {
            if (!_icon.isNull()) {
                // Same rendering as the main menu's Add Account row (tdesktop
                // IconType::Round): the accent disc is the glyph's own integer
                // rect, centred where the avatars above sit.
                const auto glyph = _icon.size() / _icon.devicePixelRatio();
                const auto discRect = QRect(
                    qRound(left + size / 2.0 - glyph.width() / 2.0),
                    (height() - glyph.height()) / 2,
                    glyph.width(),
                    glyph.height());
                p.setPen(Qt::NoPen);
                p.setBrush(st::activeButtonBg);
                p.drawEllipse(discRect);
                p.drawImage(discRect.topLeft(), _icon);
            }
        } else if (const auto account = _controller
                ? _controller->domain().account(_accountIndex)
                : nullptr) {
            const auto dpr = p.device() ? p.device()->devicePixelRatioF() : 1.0;
            const auto avatar = MediaCache::loadAvatarPixmap(
                account->avatarUrl(), size, dpr);
            if (!avatar.isNull()) {
                p.drawPixmap(left, top, avatar);
            } else {
                ::Ui::EmptyUserpic::paint(
                    p, _label, account->displayName(), left, top, size);
            }
        }

        const auto font = st::baseFont(14);
        const QFontMetrics metrics(font);
        const QRect textRect(
            st::settingsAccountTextLeft,
            0,
            qMax(0, width() - st::settingsAccountTextLeft
                - st::settingsButtonPaddingRight),
            height());
        p.setFont(font);
        p.setPen(_accountIndex < 0 ? st::windowActiveTextFg : st::windowFg);
        p.drawText(
            textRect,
            Qt::AlignLeft | Qt::AlignVCenter,
            metrics.elidedText(_label, Qt::ElideRight, textRect.width()));
    }

    void mousePressEvent(QMouseEvent *e) override {
        if (e->button() == Qt::LeftButton && _clicked) {
            _clicked();
        }
    }
    void enterEvent(QEnterEvent *) override { _hovered = true; update(); }
    void leaveEvent(QEvent *) override { _hovered = false; update(); }

private:
    AppController *_controller = nullptr;
    int _accountIndex = -1;
    QString _label;
    QImage _icon;
    bool _hovered = false;
    std::function<void()> _clicked;
};


void showSettingsInformBox(QWidget *parent, const QString &title, const QString &text) {
    HistoryConfirmDialog dialog(
        parent,
        title,
        text,
        QCoreApplication::translate("SettingsWidget", "OK"),
        QString(),
        HistoryConfirmDialog::Normal,
        0,
        -1,
        false);
    dialog.exec();
}

bool confirmSettingsAction(
        QWidget *parent,
        const QString &title,
        const QString &text,
        const QString &confirmText,
        const QString &cancelText,
        HistoryConfirmDialog::ConfirmStyle style = HistoryConfirmDialog::Normal) {
    HistoryConfirmDialog dialog(
        parent,
        title,
        text,
        confirmText,
        cancelText,
        style);
    return dialog.exec() == HistoryConfirmDialog::Accepted;
}

// The homeserver can't verify this medium at all, so no address of it can ever be
// added. Servers implementing MSC4178 (Synapse >= 1.130) say so with an errcode;
// older ones raise a bare 400 whose message is the only thing to match on.
[[nodiscard]] bool isMediumUnsupportedError(const QString &error) {
    return error.contains(QStringLiteral("M_THREEPID_MEDIUM_NOT_SUPPORTED"))
        || error.contains(QStringLiteral("Adding an email to your account is disabled"))
        || error.contains(QStringLiteral("Adding phone numbers to user account is not supported"))
        || error.contains(QStringLiteral("3PID changes are disabled on this server"));
}

// A wrapped, subdued paragraph under a section title — used where a section has
// nothing actionable to offer and has to say why.
void addSettingsSectionNote(
        QWidget *parent,
        QVBoxLayout *layout,
        const QString &text) {
    auto *note = new QLabel(text, parent);
    note->setWordWrap(true);
    note->setFont(st::baseFont(13));
    note->setContentsMargins(st::settingsButtonPaddingLeft, 4,
                             st::settingsButtonPaddingRight, 4);
    QPalette palette = note->palette();
    palette.setColor(QPalette::WindowText, st::windowSubTextFg);
    note->setPalette(palette);
    layout->addWidget(note);
}

struct PhoneCountryCode {
    const char *iso;
    const char *dialCode;
    const char *name;
};

const PhoneCountryCode kPhoneCountryCodes[] = {
    { "US", "+1", "United States" },
    { "CA", "+1", "Canada" },
    { "GB", "+44", "United Kingdom" },
    { "DE", "+49", "Germany" },
    { "FR", "+33", "France" },
    { "IT", "+39", "Italy" },
    { "ES", "+34", "Spain" },
    { "NL", "+31", "Netherlands" },
    { "BE", "+32", "Belgium" },
    { "CH", "+41", "Switzerland" },
    { "AT", "+43", "Austria" },
    { "SE", "+46", "Sweden" },
    { "NO", "+47", "Norway" },
    { "DK", "+45", "Denmark" },
    { "FI", "+358", "Finland" },
    { "PL", "+48", "Poland" },
    { "CZ", "+420", "Czechia" },
    { "SK", "+421", "Slovakia" },
    { "HU", "+36", "Hungary" },
    { "RO", "+40", "Romania" },
    { "BG", "+359", "Bulgaria" },
    { "GR", "+30", "Greece" },
    { "TR", "+90", "Turkey" },
    { "UA", "+380", "Ukraine" },
    { "RS", "+381", "Serbia" },
    { "HR", "+385", "Croatia" },
    { "SI", "+386", "Slovenia" },
    { "BA", "+387", "Bosnia and Herzegovina" },
    { "ME", "+382", "Montenegro" },
    { "MK", "+389", "North Macedonia" },
    { "AL", "+355", "Albania" },
    { "RU", "+7", "Russia" },
    { "IL", "+972", "Israel" },
    { "IN", "+91", "India" },
    { "CN", "+86", "China" },
    { "JP", "+81", "Japan" },
    { "KR", "+82", "South Korea" },
    { "AU", "+61", "Australia" },
    { "NZ", "+64", "New Zealand" },
    { "BR", "+55", "Brazil" },
    { "MX", "+52", "Mexico" },
    { "AR", "+54", "Argentina" },
    { "ZA", "+27", "South Africa" },
};

QString systemPhoneCountryIso() {
    const auto localeName = QLocale::system().name();
    const auto separator = localeName.indexOf(QLatin1Char('_'));
    if (separator >= 0 && separator + 1 < localeName.size()) {
        return localeName.mid(separator + 1).left(2).toUpper();
    }
    return QStringLiteral("US");
}

QString knownPhoneCountryIso(const QString &iso) {
    for (const auto &country : kPhoneCountryCodes) {
        if (QString::fromLatin1(country.iso) == iso) {
            return iso;
        }
    }
    return QStringLiteral("US");
}

QVector<PhoneCountryCode> sortedPhoneCountryCodes() {
    QVector<PhoneCountryCode> result;
    result.reserve(int(sizeof(kPhoneCountryCodes) / sizeof(kPhoneCountryCodes[0])));
    for (const auto &country : kPhoneCountryCodes) {
        result.push_back(country);
    }
    std::sort(result.begin(), result.end(), [](const auto &a, const auto &b) {
        return QString::localeAwareCompare(
            QString::fromLatin1(a.name),
            QString::fromLatin1(b.name)) < 0;
    });
    return result;
}

PhoneCountryCode phoneCountryByIso(const QString &iso) {
    const auto knownIso = knownPhoneCountryIso(iso);
    for (const auto &country : kPhoneCountryCodes) {
        if (QString::fromLatin1(country.iso) == knownIso) {
            return country;
        }
    }
    return kPhoneCountryCodes[0];
}

QString phoneCountryButtonLabel(const QString &iso) {
    const auto country = phoneCountryByIso(iso);
    return QStringLiteral("%1 %2")
        .arg(QString::fromLatin1(country.dialCode), QString::fromLatin1(country.iso));
}

QString phoneCountrySearchText(const PhoneCountryCode &country) {
    return QStringLiteral("%1 %2 %3")
        .arg(QString::fromLatin1(country.name),
             QString::fromLatin1(country.dialCode),
             QString::fromLatin1(country.iso));
}

void paintChoiceBoxShadow(QPainter &p, const QRect &boxRect) {
    PainterHighQualityEnabler hq(p);
    p.setPen(Qt::NoPen);
    const auto extend = qMax(1, st::layerShadowExtend);
    for (int i = extend; i >= 1; --i) {
        const auto progress = qreal(extend - i) / extend;
        const auto alpha = int(18.0 * progress * progress);
        p.setBrush(st::withAlpha(st::windowShadowFg, alpha));
        const auto r = st::boxRadius + i;
        p.drawRoundedRect(boxRect.adjusted(-i, -i, i, i), r, r);
    }
}

// A QFont at a literal device-pixel size with the base family, reproducing the
// raw `font-size: Npx` previously baked into the inline stylesheets.
[[nodiscard]] QFont compactFont(int pixelSize) {
    QFont font;
    font.setFamily(st::baseFontFamily());
    font.setPixelSize(pixelSize);
    return font;
}

// Panel surface painted with live st:: colors (so it tracks theme changes)
// instead of a frozen stylesheet `background; border-radius`.
class RoundedPanel final : public QWidget {
public:
    explicit RoundedPanel(QWidget *parent) : QWidget(parent) {}

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        PainterHighQualityEnabler hq(p);
        p.setPen(Qt::NoPen);
        p.setBrush(st::boxBg);
        p.drawRoundedRect(rect(), st::boxRadius, st::boxRadius);
    }
};

// QLineEdit with a custom-painted 1px border (st::inputBorderFg, switching to
// st::activeLineFg on focus) and st::boxBg fill, replacing the bordered inline
// stylesheet. Text/selection colours go through QPalette so they read live
// st:: values. `radius` and horizontal text padding mirror the old QSS.
class BorderedLineEdit final : public QLineEdit {
public:
    BorderedLineEdit(int pixelSize, int paddingH, QWidget *parent)
        : QLineEdit(parent)
        , _radius(4)
        , _paddingH(paddingH) {
        setFont(compactFont(pixelSize));
        setFrame(false);
        setAttribute(Qt::WA_MacShowFocusRect, false);
        setTextMargins(_paddingH, 0, _paddingH, 0);
        QPalette pal = palette();
        pal.setColor(QPalette::Base, st::boxBg);
        pal.setColor(QPalette::Text, st::windowFg);
        pal.setColor(QPalette::Highlight, st::windowBgActive);
        setPalette(pal);
    }

protected:
    void paintEvent(QPaintEvent *event) override {
        QLineEdit::paintEvent(event);
        QPainter p(this);
        PainterHighQualityEnabler hq(p);
        const auto border = hasFocus() ? st::activeLineFg : st::inputBorderFg;
        p.setPen(QPen(border, 1));
        p.setBrush(Qt::NoBrush);
        const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        p.drawRoundedRect(r, _radius, _radius);
    }

private:
    int _radius = 4;
    int _paddingH = 8;
};

// Flat bordered text button (transparent normal fill, hover fill, coloured
// text and matching border), replacing the inline `QPushButton{...:hover{...}}`
// stylesheets. All colours are read live from st:: at paint time.
class BorderedTextButton final : public QPushButton {
public:
    struct Palette {
        const QColor *fg = nullptr;       // text + border colour
        const QColor *bgOver = nullptr;   // hover fill
        int radius = 4;
        int pixelSize = 13;
        int paddingH = 14;                // horizontal text padding for sizeHint
    };

    BorderedTextButton(const QString &text, const Palette &p, QWidget *parent)
        : QPushButton(text, parent)
        , _palette(p) {
        setCursor(Qt::PointingHandCursor);
        setFont(compactFont(_palette.pixelSize));
        setFlat(true);
    }

    [[nodiscard]] QSize sizeHint() const override {
        const QFontMetrics fm(font());
        const auto w = fm.horizontalAdvance(text()) + 2 * _palette.paddingH;
        return QSize(w, QPushButton::sizeHint().height());
    }

protected:
    void enterEvent(QEnterEvent *) override { _hovered = true; update(); }
    void leaveEvent(QEvent *) override { _hovered = false; update(); }

    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        PainterHighQualityEnabler hq(p);
        const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        if (_hovered && _palette.bgOver) {
            p.setPen(Qt::NoPen);
            p.setBrush(*_palette.bgOver);
            p.drawRoundedRect(r, _palette.radius, _palette.radius);
        }
        const auto fg = _palette.fg ? *_palette.fg : st::windowFg;
        p.setPen(QPen(fg, 1));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(r, _palette.radius, _palette.radius);
        p.setPen(fg);
        p.drawText(rect(), Qt::AlignCenter, text());
    }

private:
    Palette _palette;
    bool _hovered = false;
};

// Bordered left-aligned selector button (the phone country picker). Border is
// st::inputBorderFg, switching to st::activeLineFg on hover/focus; fill st::boxBg,
// text st::windowFg. Mirrors compactSelectorButtonStyleSheet().
class SelectorButton final : public QPushButton {
public:
    SelectorButton(const QString &text, QWidget *parent)
        : QPushButton(text, parent) {
        setCursor(Qt::PointingHandCursor);
        setFont(compactFont(13));
        setFlat(true);
    }

protected:
    void enterEvent(QEnterEvent *) override { _hovered = true; update(); }
    void leaveEvent(QEvent *) override { _hovered = false; update(); }

    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        PainterHighQualityEnabler hq(p);
        const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        p.setPen(Qt::NoPen);
        p.setBrush(st::boxBg);
        p.drawRoundedRect(r, 4, 4);
        const auto active = _hovered || hasFocus();
        p.setPen(QPen(active ? st::activeLineFg : st::inputBorderFg, 1));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(r, 4, 4);
        p.setPen(st::windowFg);
        const QRect textRect = rect().adjusted(10, 0, -10, 0);
        p.drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, text());
    }

private:
    bool _hovered = false;
};

class CountryCodeChoiceDialog final : public QWidget {
public:
    enum DialogCode { Rejected = 0, Accepted = 1 };

    CountryCodeChoiceDialog(QWidget *parent, const QString &currentIso)
        : QWidget(parent ? parent->window() : nullptr)
        , _chosenIso(knownPhoneCountryIso(currentIso))
        , _countries(sortedPhoneCountryCodes()) {
        if (parentWidget()) {
            setGeometry(parentWidget()->rect());
            parentWidget()->installEventFilter(this);
        }
        setFocusPolicy(Qt::StrongFocus);
        setAttribute(Qt::WA_DeleteOnClose, false);

        auto *root = new QVBoxLayout(this);
        root->setContentsMargins(0, 0, 0, 0);
        root->addStretch(1);

        _panel = new RoundedPanel(this);
        _panel->setFixedWidth(st::boxWideWidth);
        root->addWidget(_panel, 0, Qt::AlignHCenter);
        root->addStretch(1);

        auto *layout = new QVBoxLayout(_panel);
        layout->setContentsMargins(0, 0, 0, st::boxRadius);
        layout->setSpacing(0);

        auto *titleLabel = new QLabel(QCoreApplication::translate(
            "SettingsWidget",
            "Country code"), _panel);
        titleLabel->setFixedHeight(st::settingsTopBarHeight);
        titleLabel->setContentsMargins(st::settingsButtonPaddingLeft, 0, 0, 0);
        titleLabel->setFont(st::boxTitleFont);
        titleLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        {
            QPalette pal = titleLabel->palette();
            pal.setColor(QPalette::WindowText, st::boxTitleFg);
            titleLabel->setPalette(pal);
        }
        layout->addWidget(titleLabel);

        auto *searchRow = new QWidget(_panel);
        auto *searchLayout = new QVBoxLayout(searchRow);
        searchLayout->setContentsMargins(
            st::settingsButtonPaddingLeft,
            0,
            st::settingsButtonPaddingRight,
            8);
        searchLayout->setSpacing(0);

        _search = new BorderedLineEdit(13, 8, searchRow);
        _search->setPlaceholderText(QCoreApplication::translate(
            "SettingsWidget",
            "Search"));
        _search->setFixedHeight(32);
        searchLayout->addWidget(_search);
        layout->addWidget(searchRow);

        _scroll = new ::Ui::ScrollArea(_panel);
        _scroll->setWidgetResizable(true);
        _scroll->setFrameShape(QFrame::NoFrame);
        _scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

        _content = new QWidget(_scroll);
        _content->setAutoFillBackground(true);
        {
            QPalette pal = _content->palette();
            pal.setColor(QPalette::Window, st::boxBg);
            _content->setPalette(pal);
        }
        _contentLayout = new QVBoxLayout(_content);
        _contentLayout->setContentsMargins(0, 0, 0, st::internalChoiceBottomSkip);
        _contentLayout->setSpacing(0);
        _scroll->setWidget(_content);
        layout->addWidget(_scroll, 1);

        connect(_search, &QLineEdit::textChanged, this, [this] {
            rebuildRows();
        });
        rebuildRows();

        QTimer::singleShot(0, _search, [this] {
            _search->setFocus();
        });
    }

    ~CountryCodeChoiceDialog() override {
        if (parentWidget()) {
            parentWidget()->removeEventFilter(this);
        }
    }

    [[nodiscard]] int exec() {
        const auto restoreFocus = ::TeleMatrix::Focus::saveFocusForPopup();
        raise();
        show();
        setFocus();

        QEventLoop loop;
        _loop = &loop;
        loop.exec();
        _loop = nullptr;

        hide();
        ::TeleMatrix::Focus::restoreFocusAfterPopup(restoreFocus);
        return _result;
    }

    [[nodiscard]] QString chosenIso() const {
        return _chosenIso;
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.fillRect(rect(), st::layerBg);
        if (_panel) {
            const auto panelRect = _panel->geometry();
            paintChoiceBoxShadow(p, panelRect);
            PainterHighQualityEnabler hq(p);
            p.setPen(Qt::NoPen);
            p.setBrush(st::boxBg);
            p.drawRoundedRect(panelRect, st::boxRadius, st::boxRadius);
        }
    }

    void mousePressEvent(QMouseEvent *event) override {
        if (_panel && !_panel->geometry().contains(event->pos())) {
            reject();
            return;
        }
        QWidget::mousePressEvent(event);
    }

    void keyPressEvent(QKeyEvent *event) override {
        if (event->key() == Qt::Key_Escape) {
            reject();
            return;
        }
        QWidget::keyPressEvent(event);
    }

    bool eventFilter(QObject *obj, QEvent *event) override {
        if (obj == parentWidget() && event->type() == QEvent::Resize) {
            setGeometry(parentWidget()->rect());
        }
        return QWidget::eventFilter(obj, event);
    }

private:
    void accept() {
        _result = Accepted;
        if (_loop) {
            _loop->quit();
        }
    }

    void reject() {
        _result = Rejected;
        if (_loop) {
            _loop->quit();
        }
    }

    void rebuildRows() {
        clearSettingsLayout(_contentLayout);

        const auto query = _search
            ? _search->text().trimmed()
            : QString();
        int rowsHeight = 0;
        int rows = 0;
        for (const auto &country : std::as_const(_countries)) {
            if (!query.isEmpty()
                && !phoneCountrySearchText(country).contains(query, Qt::CaseInsensitive)) {
                continue;
            }
            const auto iso = QString::fromLatin1(country.iso);
            SettingsChoiceEntry entry{
                iso,
                QString::fromLatin1(country.name),
                QString::fromLatin1(country.dialCode),
                st::baseFont(14),
            };
            auto *row = new SettingsChoiceRow(entry, iso == _chosenIso, _content);
            row->setClickedCallback([this](const QString &id) {
                _chosenIso = id;
                accept();
            });
            _contentLayout->addWidget(row);
            rowsHeight += st::internalChoiceSubtitleRowHeight;
            ++rows;
        }

        if (rows == 0) {
            auto *empty = new QLabel(QCoreApplication::translate(
                "SettingsWidget",
                "No results"), _content);
            empty->setFixedHeight(st::settingsButtonHeight);
            empty->setContentsMargins(st::settingsButtonPaddingLeft, 0, 0, 0);
            empty->setFont(st::baseFont(14));
            {
                QPalette pal = empty->palette();
                pal.setColor(QPalette::WindowText, st::windowSubTextFg);
                empty->setPalette(pal);
            }
            empty->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
            _contentLayout->addWidget(empty);
            rowsHeight = st::settingsButtonHeight;
        }

        _content->setFixedHeight(rowsHeight + st::internalChoiceBottomSkip);
        const int desiredHeight = st::settingsTopBarHeight
            + 40
            + rowsHeight
            + st::internalChoiceBottomSkip
            + st::boxRadius;
        _panel->setFixedHeight(qMin(st::internalChoicePopupMaxHeight, desiredHeight));
    }

    QWidget *_panel = nullptr;
    QLineEdit *_search = nullptr;
    ::Ui::ScrollArea *_scroll = nullptr;
    QWidget *_content = nullptr;
    QVBoxLayout *_contentLayout = nullptr;
    QEventLoop *_loop = nullptr;
    QString _chosenIso;
    QVector<PhoneCountryCode> _countries;
    int _result = Rejected;
};

} // namespace

AccountSettingsPage::AccountSettingsPage(
        AppController *controller,
        Core::AccountSettings *settings,
        QWidget *parent)
    : QWidget(parent)
    , _controller(controller)
    , _settings(settings) {
    auto *pageLayout = new QVBoxLayout(this);
    pageLayout->setContentsMargins(0, 0, 0, 0);
    pageLayout->setSpacing(0);

    _cover = new AccountProfileCover(_controller, this);
    pageLayout->addWidget(_cover);
    connect(_cover, &AccountProfileCover::uploadAvatarRequested,
        this, &AccountSettingsPage::chooseAndUploadAvatar);
    connect(_cover, &AccountProfileCover::deleteAvatarRequested,
        this, &AccountSettingsPage::deleteAvatar);

    _scrollArea = new ::Ui::ScrollArea(this);
    _scrollArea->setWidgetResizable(true);
    _scrollArea->setFrameShape(QFrame::NoFrame);
    _scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    _sections = new QWidget(_scrollArea);
    _sectionsLayout = new QVBoxLayout(_sections);
    _sectionsLayout->setContentsMargins(0, 0, 0, 20);
    _sectionsLayout->setSpacing(0);
    _scrollArea->setWidget(_sections);
    pageLayout->addWidget(_scrollArea);

    if (auto *bridge = _controller ? _controller->bridge() : nullptr) {
        connect(bridge, &ProtocolBridge::mediaResolved,
            this, [this](bool success, const QString &mxcUrl, const QString &localPath) {
                if (!success || localPath.isEmpty()) {
                    if (mxcUrl.startsWith(QStringLiteral("mxc://"))) {
                        MediaCache::clearRequested(mxcUrl);
                    }
                    return;
                }
                MediaCache::insertPath(mxcUrl, localPath);
                if (_cover) {
                    _cover->update();
                }
            });
        connect(bridge, &ProtocolBridge::accountSummaryReady,
            this, [this](bool success, const AccountSummary &summary, const QString &error) {
                onAccountSummaryReady(success, summary, error);
            });
        connect(bridge, &ProtocolBridge::displayNameSet,
            this, [this](bool success, const QString &error) {
                if (success) {
                    refreshData(true);
                } else {
                    showSettingsInformBox(this, tr("Error"),
                        tr("Failed to update display name: %1").arg(error));
                }
            });
        connect(bridge, &ProtocolBridge::avatarUploaded,
            this, [this](bool success, const QString &newAvatarUrl, const QString &error) {
                setAvatarOperationInFlight(false);
                if (success) {
                    if (auto *b = _controller ? _controller->bridge() : nullptr) {
                        if (!newAvatarUrl.isEmpty()
                                && newAvatarUrl.startsWith(QStringLiteral("mxc://"))
                                && MediaCache::needsResolution(newAvatarUrl)) {
                            MediaCache::markRequested(newAvatarUrl);
                            b->resolveAvatar(newAvatarUrl);
                        }
                        refreshData(true);
                    }
                } else {
                    showSettingsInformBox(this, tr("Error"),
                        tr("Failed to upload avatar: %1").arg(error));
                }
            });
        connect(bridge, &ProtocolBridge::avatarSet,
            this, [this](bool success, const QString &error) {
                setAvatarOperationInFlight(false);
                if (success) {
                    if (!_accountSummary.avatarUrl.isEmpty()) {
                        MediaCache::clearRequested(_accountSummary.avatarUrl);
                    }
                    _accountSummary.avatarUrl.clear();
                    refreshData(true);
                } else {
                    showSettingsInformBox(this, tr("Error"),
                        tr("Failed to delete avatar: %1").arg(error));
                }
            });
        connect(bridge, &ProtocolBridge::threepidsReady,
            this, [this](bool success, const QVector<ThreePid> &items, const QString &error) {
                _threepidsInFlight = false;
                if (success) {
                    _threepids = items;
                    _threepidsLoaded = true;
                    rebuildSections();
                }
            });
        connect(bridge, &ProtocolBridge::accountManagementProbed,
            this, [this](quint64, bool available, const QString &url) {
                _accountManagementInFlight = false;
                if (available && _accountManagementUrl != url) {
                    _accountManagementUrl = url;
                    rebuildSections();
                }
            });
        connect(bridge, &ProtocolBridge::emailThreepidSupportProbed,
            this, [this](quint64, bool known, bool supported) {
                if (known && !supported && !_emailVerificationUnsupported) {
                    _emailVerificationUnsupported = true;
                    rebuildSections();
                }
            });
        connect(bridge, &ProtocolBridge::threepidTokenReady,
            this, [this](bool success, const ThreePidTokenResponse &token, const QString &error) {
                if (success) {
                    if (_pending3pidMedium == ThreePidMedium::Email) {
                        _email3pidError.clear();
                    } else {
                        _phone3pidError.clear();
                    }
                    _pending3pidSid = token.sid;
                    showSettingsInformBox(this, tr("Verification Sent"),
                        _pending3pidMedium == ThreePidMedium::Email
                            ? tr("A verification email has been sent. "
                                 "Please check your email and click the link, "
                                 "then click OK to complete.")
                            : tr("A verification SMS has been sent. "
                                 "Please complete verification, "
                                 "then click OK to complete."));
                    if (auto *b = _controller ? _controller->bridge() : nullptr) {
                        b->addThreepid(_pending3pidClientSecret, _pending3pidSid);
                    }
                } else {
                    // Stop offering a form that will always fail and say so instead.
                    // Only ever latches on: a later unrelated error (a rejected
                    // address, say) must not un-learn that the medium is unusable.
                    const bool unsupported = isMediumUnsupportedError(error);
                    if (_pending3pidMedium == ThreePidMedium::Email) {
                        _emailVerificationUnsupported =
                            _emailVerificationUnsupported || unsupported;
                        _email3pidError = unsupported
                            ? QString()
                            : relevantThreepidErrorText(ThreePidMedium::Email, error);
                    } else {
                        _phoneVerificationUnsupported =
                            _phoneVerificationUnsupported || unsupported;
                        _phone3pidError = unsupported
                            ? QString()
                            : relevantThreepidErrorText(ThreePidMedium::Msisdn, error);
                    }
                    rebuildSections();
                }
            });
        connect(bridge, &ProtocolBridge::threepidAdded,
            this, [this](const AccountActionResult &result) {
                if (result.completed) {
                    _pending3pidClientSecret.clear();
                    _pending3pidSid.clear();
                    if (_pending3pidMedium == ThreePidMedium::Email) {
                        _email3pidError.clear();
                    } else {
                        _phone3pidError.clear();
                    }
                    refreshData(true);
                    showSettingsInformBox(this,
                        _pending3pidMedium == ThreePidMedium::Email
                            ? tr("Email Added")
                            : tr("Phone Added"),
                        _pending3pidMedium == ThreePidMedium::Email
                            ? tr("Email address added successfully.")
                            : tr("Phone number added successfully."));
                } else if (!result.uiaSession.isEmpty()) {
                    auto *authDlg = new InteractiveAuthDialog(
                        _controller ? _controller->userId() : QString(),
                        result.uiaFlowsJson,
                        this);
                    if (authDlg->exec() == InteractiveAuthDialog::Accepted) {
                        if (auto *b = _controller ? _controller->bridge() : nullptr) {
                            b->addThreepid(_pending3pidClientSecret,
                                _pending3pidSid, authDlg->authJson());
                        }
                    }
                    authDlg->deleteLater();
                } else if (!result.errorMessage.isEmpty()) {
                    if (_pending3pidMedium == ThreePidMedium::Email) {
                        _email3pidError = relevantThreepidErrorText(
                            ThreePidMedium::Email,
                            result.errorMessage);
                    } else {
                        _phone3pidError = relevantThreepidErrorText(
                            ThreePidMedium::Msisdn,
                            result.errorMessage);
                    }
                    rebuildSections();
                }
            });
        connect(bridge, &ProtocolBridge::threepidDeleted,
            this, [this](bool success) {
                if (success) {
                    refreshData(true);
                } else {
                    showSettingsInformBox(this, tr("Error"),
                        tr("Failed to remove the identifier."));
                }
            });
        connect(bridge, &ProtocolBridge::changePasswordResult,
            this, [this](const AccountActionResult &result) {
                if (result.completed) {
                    _pendingNewPassword.clear();
                    showSettingsInformBox(this, tr("Password Changed"),
                        tr("Your password has been changed successfully."));
                } else if (!result.uiaSession.isEmpty()) {
                    auto *authDlg = new InteractiveAuthDialog(
                        _controller ? _controller->userId() : QString(),
                        result.uiaFlowsJson,
                        this);
                    if (authDlg->exec() == InteractiveAuthDialog::Accepted) {
                        if (auto *b = _controller ? _controller->bridge() : nullptr) {
                            b->changePassword(_pendingNewPassword, authDlg->authJson());
                        }
                    }
                    authDlg->deleteLater();
                } else if (!result.errorMessage.isEmpty()) {
                    _pendingNewPassword.clear();
                    showSettingsInformBox(this, tr("Error"),
                        tr("Failed to change password: %1").arg(result.errorMessage));
                }
            });
        connect(bridge, &ProtocolBridge::deactivateAccountResult,
            this, [this](const AccountActionResult &result) {
                if (result.completed) {
                    showSettingsInformBox(this, tr("Account Deactivated"),
                        tr("Your account has been deactivated."));
                } else if (!result.uiaSession.isEmpty()) {
                    auto *authDlg = new InteractiveAuthDialog(
                        _controller ? _controller->userId() : QString(),
                        result.uiaFlowsJson,
                        this);
                    if (authDlg->exec() == InteractiveAuthDialog::Accepted) {
                        if (auto *b = _controller ? _controller->bridge() : nullptr) {
                            b->deactivateAccount(true, authDlg->authJson());
                        }
                    }
                    authDlg->deleteLater();
                } else if (!result.errorMessage.isEmpty()) {
                    showSettingsInformBox(this, tr("Error"),
                        tr("Failed to deactivate account: %1").arg(result.errorMessage));
                }
            });

        const auto avatarUrl = _controller ? _controller->avatarUrl() : QString();
        if (avatarUrl.startsWith(QStringLiteral("mxc://"))
                && MediaCache::needsResolution(avatarUrl)) {
            MediaCache::markRequested(avatarUrl);
            bridge->resolveAvatar(avatarUrl);
        }
    }

    // Both were settled in the background at session-ready, well before this page
    // was built (it is only built when Settings is first opened) — so seed from
    // them and render the truth on the very first paint, rather than offering a
    // form that a late answer then has to take away again.
    if (_controller) {
        _emailVerificationUnsupported =
            (_controller->emailVerificationSupported() == false);
    }
    if (_controller && _controller->cachedAccountSummaryLoaded()) {
        onAccountSummaryReady(true, _controller->cachedAccountSummary(), QString());
    } else {
        rebuildSections();
    }
}

void AccountSettingsPage::prepareForShow() {
    if (_scrollArea) {
        _scrollArea->verticalScrollBar()->setValue(0);
    }
    refreshData();
}

void AccountSettingsPage::refreshData(bool force) {
    auto *bridge = _controller ? _controller->bridge() : nullptr;
    if (!bridge) {
        return;
    }

    if (force || !_accountSummaryInFlight) {
        _accountSummaryInFlight = true;
        bridge->fetchAccountSummary();
    }
    if (force || !_threepidsInFlight) {
        _threepidsInFlight = true;
        bridge->fetchThreepids();
    }
    // Where email/phone actually live on a delegated-auth homeserver (the 3PID API
    // is off there, so the sections below would otherwise be a dead end).
    const auto homeserver = _settings ? _settings->sessionHomeserver() : QString();
    if (!homeserver.isEmpty()
            && _accountManagementUrl.isEmpty()
            && (force || !_accountManagementInFlight)) {
        _accountManagementInFlight = true;
        bridge->probeAccountManagement(homeserver);
    }

    const auto avatarUrl = _accountSummaryLoaded
        ? _accountSummary.avatarUrl
        : (_controller ? _controller->avatarUrl() : QString());
    if (avatarUrl.startsWith(QStringLiteral("mxc://"))
            && MediaCache::needsResolution(avatarUrl)) {
        MediaCache::markRequested(avatarUrl);
        bridge->resolveAvatar(avatarUrl);
    }
}

void AccountSettingsPage::onAccountSummaryReady(
        bool success,
        const AccountSummary &summary,
        const QString &error) {
    _accountSummaryInFlight = false;
    if (!success) {
        return;
    }
    _accountSummary = summary;
    _accountSummaryLoaded = true;
    if (_cover) {
        _cover->setAccountSummary(_accountSummary, _accountSummaryLoaded);
    }

    // m.3pid_changes is the only pre-flight signal the spec offers, and it is a
    // single aggregate flag — there is no per-medium capability, and the only
    // per-medium signal (the requestToken error) costs a real email/SMS to obtain.
    // So a server that takes no 3PID changes can verify neither medium: say so in
    // both sections rather than offering two forms that are certain to fail.
    if (!_accountSummary.capabilities.canChange3pid) {
        _emailVerificationUnsupported = true;
        _phoneVerificationUnsupported = true;
    }

    if (!summary.avatarUrl.isEmpty()
            && summary.avatarUrl.startsWith(QStringLiteral("mxc://"))
            && MediaCache::needsResolution(summary.avatarUrl)) {
        if (auto *b = _controller ? _controller->bridge() : nullptr) {
            MediaCache::markRequested(summary.avatarUrl);
            b->resolveAvatar(summary.avatarUrl);
        }
    }

    rebuildSections();
}

QString AccountSettingsPage::relevantThreepidErrorText(
        ThreePidMedium medium,
        const QString &error) const {
    const auto isEmail = (medium == ThreePidMedium::Email);
    const auto text = error.trimmed();

    auto cleaned = text;
    const auto serverPrefix = QStringLiteral("the server returned an error:");
    const auto serverPrefixAt = cleaned.indexOf(serverPrefix, 0, Qt::CaseInsensitive);
    if (serverPrefixAt >= 0) {
        cleaned = cleaned.mid(serverPrefixAt + serverPrefix.size()).trimmed();
    }
    if (cleaned.startsWith(QLatin1Char('['))) {
        const auto closing = cleaned.indexOf(QLatin1Char(']'));
        if (closing >= 0) {
            cleaned = cleaned.mid(closing + 1).trimmed();
        }
    }
    if (cleaned.size() >= 2
        && cleaned.front() == QLatin1Char('"')
        && cleaned.back() == QLatin1Char('"')) {
        cleaned = cleaned.mid(1, cleaned.size() - 2).trimmed();
    }
    const auto hasRelevantServerText = !cleaned.isEmpty()
        && !cleaned.contains(QStringLiteral("M_"))
        && cleaned.size() <= 100;

    if (text.contains(QStringLiteral("M_THREEPID_IN_USE"))) {
        return isEmail
            ? tr("This email address is already linked to an account.")
            : tr("This phone number is already linked to an account.");
    } else if (text.contains(QStringLiteral("M_THREEPID_NOT_FOUND"))) {
        return isEmail
            ? tr("No account is linked to that email address.")
            : tr("No account is linked to that phone number.");
    } else if (text.contains(QStringLiteral("M_THREEPID_DENIED"))) {
        if (hasRelevantServerText) {
            return cleaned;
        }
        return isEmail
            ? tr("This email address cannot be used.")
            : tr("This phone number cannot be used.");
    } else if (text.contains(QStringLiteral("M_THREEPID_AUTH_FAILED"))) {
        return tr("Verification failed. Please try again.");
    } else if (text.contains(QStringLiteral("M_SERVER_NOT_TRUSTED"))) {
        return tr("The identity server is not trusted by the homeserver.");
    } else if (text.contains(QStringLiteral("M_LIMIT_EXCEEDED"))) {
        return tr("Too many attempts. Please try again later.");
    } else if (text.contains(QStringLiteral("M_INVALID_PARAM"))
        || text.contains(QStringLiteral("M_BAD_JSON"))
        || text.contains(QStringLiteral("M_MISSING_PARAM"))) {
        return isEmail
            ? tr("Please enter a valid email address.")
            : tr("Please enter a valid phone number.");
    } else if (isMediumUnsupportedError(text)) {
        return isEmail
            ? tr("Email verification is not supported by this homeserver.")
            : tr("Phone verification is not supported by this homeserver.");
    }

    if (hasRelevantServerText) {
        return cleaned;
    }
    return tr("Unable to send verification. Please try again.");
}

bool AccountSettingsPage::canChange3pid() const {
    // Optimistic until the capabilities land, so the form isn't withheld from a
    // server that does allow changes while its summary is still in flight.
    return !_accountSummaryLoaded || _accountSummary.capabilities.canChange3pid;
}

bool AccountSettingsPage::managesThreepidsExternally() const {
    // A delegated-auth (MAS/OIDC) homeserver owns email + phone itself, turns the 3PID API off,
    // and points the user at its own website — so the per-medium "can't verify" notes are noise.
    return _accountSummaryLoaded
        && !canChange3pid()
        && !_accountManagementUrl.isEmpty();
}

void AccountSettingsPage::addEmailSection() {
    addSettingsSectionTitle(
        _sections,
        _sectionsLayout,
        tr("Email Addresses"),
        _email3pidError,
        !_threepidsLoaded);

    if (_threepidsLoaded) {
        for (const auto &pid : std::as_const(_threepids)) {
            if (pid.medium != ThreePidMedium::Email) {
                continue;
            }
            auto *row = new QWidget(_sections);
            auto *rowLayout = new QHBoxLayout(row);
            rowLayout->setContentsMargins(st::settingsButtonPaddingLeft, 4,
                                           st::settingsButtonPaddingRight, 4);
            auto *emailLabel = new QLabel(pid.address, row);
            emailLabel->setFont(st::baseFont(14));
            emailLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
            rowLayout->addWidget(emailLabel, 1);

            if (canChange3pid()) {
                BorderedTextButton::Palette removeStyle;
                removeStyle.fg = &st::attentionButtonFg;
                removeStyle.bgOver = &st::attentionButtonBgOver;
                removeStyle.radius = 3;
                removeStyle.pixelSize = 12;
                removeStyle.paddingH = 10;
                auto *removeBtn = new BorderedTextButton(
                    tr("Remove"), removeStyle, row);
                removeBtn->setFixedHeight(24);
                rowLayout->addWidget(removeBtn);

                const auto addr = pid.address;
                connect(removeBtn, &QPushButton::clicked, this, [this, addr] {
                    if (confirmSettingsAction(this,
                        tr("Remove Email"),
                        tr("Remove %1 from your account?").arg(addr),
                        tr("Remove"),
                        tr("Cancel"),
                        HistoryConfirmDialog::Attention)) {
                        if (auto *b = _controller ? _controller->bridge() : nullptr) {
                            b->deleteThreepid(ThreePidMedium::Email, addr);
                        }
                    }
                });
            }

            _sectionsLayout->addWidget(row);
        }
    }

    if (_emailVerificationUnsupported) {
        addSettingsSectionNote(
            _sections,
            _sectionsLayout,
            tr("This homeserver can't verify email addresses, so none can be added."));
    } else if (canChange3pid()) {
        auto *addRow = new QWidget(_sections);
        auto *addLayout = new QHBoxLayout(addRow);
        addLayout->setContentsMargins(st::settingsButtonPaddingLeft, 4,
                                       st::settingsButtonPaddingRight, 4);

        auto *emailInput = new BorderedLineEdit(13, 8, addRow);
        emailInput->setPlaceholderText(tr("Email address"));
        emailInput->setFixedHeight(28);
        addLayout->addWidget(emailInput, 1);

        BorderedTextButton::Palette addStyle;
        addStyle.fg = &st::activeButtonBg;
        addStyle.bgOver = &st::windowBgOver;
        addStyle.radius = 4;
        addStyle.pixelSize = 13;
        addStyle.paddingH = 14;
        auto *addBtn = new BorderedTextButton(tr("Add"), addStyle, addRow);
        addBtn->setFixedHeight(28);
        addLayout->addWidget(addBtn);

        connect(addBtn, &QPushButton::clicked, this, [this, emailInput] {
            const auto email = emailInput->text().trimmed();
            if (email.isEmpty() || !email.contains(QLatin1Char('@'))) {
                _email3pidError = tr("Please enter a valid email address.");
                rebuildSections();
                return;
            }
            _pending3pidClientSecret = QUuid::createUuid()
                .toString(QUuid::WithoutBraces);
            _pending3pidMedium = ThreePidMedium::Email;
            const auto hadError = !_email3pidError.isEmpty();
            _email3pidError.clear();
            if (auto *b = _controller ? _controller->bridge() : nullptr) {
                b->requestThreepidToken(
                    ThreePidMedium::Email, email,
                    _pending3pidClientSecret, 1);
            }
            if (hadError) {
                rebuildSections();
            }
        });

        _sectionsLayout->addWidget(addRow);
    }
}

void AccountSettingsPage::addPhoneSection() {
    addSettingsSectionTitle(
        _sections,
        _sectionsLayout,
        tr("Phone Numbers"),
        _phone3pidError,
        !_threepidsLoaded);

    if (_threepidsLoaded) {
        for (const auto &pid : std::as_const(_threepids)) {
            if (pid.medium != ThreePidMedium::Msisdn) {
                continue;
            }
            auto *row = new QWidget(_sections);
            auto *rowLayout = new QHBoxLayout(row);
            rowLayout->setContentsMargins(st::settingsButtonPaddingLeft, 4,
                                           st::settingsButtonPaddingRight, 4);
            auto *phoneLabel = new QLabel(pid.address, row);
            phoneLabel->setFont(st::baseFont(14));
            phoneLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
            rowLayout->addWidget(phoneLabel, 1);

            if (canChange3pid()) {
                BorderedTextButton::Palette removeStyle;
                removeStyle.fg = &st::attentionButtonFg;
                removeStyle.bgOver = &st::attentionButtonBgOver;
                removeStyle.radius = 3;
                removeStyle.pixelSize = 12;
                removeStyle.paddingH = 10;
                auto *removeBtn = new BorderedTextButton(
                    tr("Remove"), removeStyle, row);
                removeBtn->setFixedHeight(24);
                rowLayout->addWidget(removeBtn);

                const auto addr = pid.address;
                connect(removeBtn, &QPushButton::clicked, this, [this, addr] {
                    if (confirmSettingsAction(this,
                        tr("Remove Phone"),
                        tr("Remove %1 from your account?").arg(addr),
                        tr("Remove"),
                        tr("Cancel"),
                        HistoryConfirmDialog::Attention)) {
                        if (auto *b = _controller ? _controller->bridge() : nullptr) {
                            b->deleteThreepid(ThreePidMedium::Msisdn, addr);
                        }
                    }
                });
            }

            _sectionsLayout->addWidget(row);
        }
    }

    if (_phoneVerificationUnsupported) {
        addSettingsSectionNote(
            _sections,
            _sectionsLayout,
            tr("This homeserver can't verify phone numbers, so none can be added."));
    } else if (canChange3pid()) {
        auto *addRow = new QWidget(_sections);
        auto *addLayout = new QHBoxLayout(addRow);
        addLayout->setContentsMargins(st::settingsButtonPaddingLeft, 4,
                                       st::settingsButtonPaddingRight, 4);

        const auto defaultCountry = knownPhoneCountryIso(systemPhoneCountryIso());
        auto *countryButton = new SelectorButton(
            phoneCountryButtonLabel(defaultCountry),
            addRow);
        countryButton->setFixedHeight(28);
        countryButton->setMinimumWidth(110);
        countryButton->setProperty("countryIso", defaultCountry);
        connect(countryButton, &QPushButton::clicked, this, [this, countryButton] {
            CountryCodeChoiceDialog dialog(
                this,
                countryButton->property("countryIso").toString());
            if (dialog.exec() != CountryCodeChoiceDialog::Accepted) {
                return;
            }
            const auto iso = dialog.chosenIso();
            countryButton->setProperty("countryIso", iso);
            countryButton->setText(phoneCountryButtonLabel(iso));
        });
        addLayout->addWidget(countryButton);

        auto *phoneInput = new BorderedLineEdit(13, 8, addRow);
        phoneInput->setPlaceholderText(tr("Phone number"));
        phoneInput->setFixedHeight(28);
        addLayout->addWidget(phoneInput, 1);

        BorderedTextButton::Palette addStyle;
        addStyle.fg = &st::activeButtonBg;
        addStyle.bgOver = &st::windowBgOver;
        addStyle.radius = 4;
        addStyle.pixelSize = 13;
        addStyle.paddingH = 14;
        auto *addBtn = new BorderedTextButton(tr("Add"), addStyle, addRow);
        addBtn->setFixedHeight(28);
        addLayout->addWidget(addBtn);

        connect(addBtn, &QPushButton::clicked, this, [this, countryButton, phoneInput] {
            const auto rawPhone = phoneInput->text().trimmed();
            QString phoneDigits;
            for (const auto ch : rawPhone) {
                if (ch.isDigit()) {
                    phoneDigits.append(ch);
                }
            }
            const auto country = countryButton->property("countryIso").toString();
            const auto dialCode = QString::fromLatin1(phoneCountryByIso(country).dialCode);
            auto dialDigits = dialCode;
            dialDigits.remove(QLatin1Char('+'));
            if (rawPhone.startsWith(QLatin1Char('+'))
                && !dialDigits.isEmpty()
                && phoneDigits.startsWith(dialDigits)) {
                phoneDigits.remove(0, dialDigits.size());
            }
            if (phoneDigits.isEmpty()) {
                _phone3pidError = tr("Please enter a valid phone number.");
                rebuildSections();
                return;
            }
            _pending3pidClientSecret = QUuid::createUuid()
                .toString(QUuid::WithoutBraces);
            _pending3pidMedium = ThreePidMedium::Msisdn;
            const auto hadError = !_phone3pidError.isEmpty();
            _phone3pidError.clear();
            if (auto *bridge = _controller ? _controller->bridge() : nullptr) {
                bridge->requestThreepidToken(
                    ThreePidMedium::Msisdn,
                    phoneDigits,
                    _pending3pidClientSecret,
                    1,
                    country);
            }
            if (hadError) {
                rebuildSections();
            }
        });

        _sectionsLayout->addWidget(addRow);
    }
}

void AccountSettingsPage::addAccountManagementNote() {
    auto *note = new QWidget(_sections);
    auto *noteLayout = new QVBoxLayout(note);
    noteLayout->setContentsMargins(st::settingsButtonPaddingLeft, 4,
                                    st::settingsButtonPaddingRight, 4);
    noteLayout->setSpacing(6);

    auto *text = new QLabel(
        tr("This homeserver manages your email address and phone number "
           "on its website."),
        note);
    text->setWordWrap(true);
    text->setFont(st::baseFont(13));
    QPalette notePalette = text->palette();
    notePalette.setColor(QPalette::WindowText, st::windowSubTextFg);
    text->setPalette(notePalette);
    noteLayout->addWidget(text);

    BorderedTextButton::Palette openStyle;
    openStyle.fg = &st::activeButtonBg;
    openStyle.bgOver = &st::windowBgOver;
    openStyle.radius = 4;
    openStyle.pixelSize = 13;
    openStyle.paddingH = 14;
    auto *openBtn = new BorderedTextButton(
        tr("Manage on the website"), openStyle, note);
    openBtn->setFixedHeight(28);

    auto *buttonRow = new QHBoxLayout();
    buttonRow->setContentsMargins(0, 0, 0, 0);
    buttonRow->addWidget(openBtn);
    buttonRow->addStretch(1);
    noteLayout->addLayout(buttonRow);

    const auto url = _accountManagementUrl;
    connect(openBtn, &QPushButton::clicked, this, [url] {
        OpenSafeExternalUrl(url);
    });

    _sectionsLayout->addWidget(note);
}

void AccountSettingsPage::chooseAndUploadAvatar() {
    if (_avatarOperationInFlight) {
        return;
    }
    const auto path = QFileDialog::getOpenFileName(
        this,
        tr("Choose Avatar"),
        QString(),
        tr("Images (*.png *.jpg *.jpeg *.gif *.webp)"));
    if (path.isEmpty()) {
        return;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return;
    }
    const auto data = file.readAll();
    if (data.isEmpty()) {
        return;
    }

    QString contentType = QStringLiteral("image/png");
    if (path.endsWith(QStringLiteral(".jpg"), Qt::CaseInsensitive)
        || path.endsWith(QStringLiteral(".jpeg"), Qt::CaseInsensitive)) {
        contentType = QStringLiteral("image/jpeg");
    } else if (path.endsWith(QStringLiteral(".gif"), Qt::CaseInsensitive)) {
        contentType = QStringLiteral("image/gif");
    } else if (path.endsWith(QStringLiteral(".webp"), Qt::CaseInsensitive)) {
        contentType = QStringLiteral("image/webp");
    }
    if (auto *bridge = _controller ? _controller->bridge() : nullptr) {
        setAvatarOperationInFlight(true);
        bridge->uploadAvatarAndSet(data, contentType);
    }
}

void AccountSettingsPage::deleteAvatar() {
    if (_avatarOperationInFlight) {
        return;
    }
    const auto avatarUrl = _accountSummaryLoaded
        ? _accountSummary.avatarUrl
        : (_controller ? _controller->avatarUrl() : QString());
    if (avatarUrl.isEmpty()) {
        return;
    }
    HistoryConfirmDialog dialog(
        this,
        tr("Delete Avatar"),
        tr("Remove your profile avatar?"),
        tr("Delete"),
        QString(),
        HistoryConfirmDialog::Attention);
    if (dialog.exec() != HistoryConfirmDialog::Accepted) {
        return;
    }
    if (auto *bridge = _controller ? _controller->bridge() : nullptr) {
        setAvatarOperationInFlight(true);
        bridge->setAvatarUrl(QString());
    }
}

void AccountSettingsPage::setAvatarOperationInFlight(bool inFlight) {
    if (_avatarOperationInFlight == inFlight) {
        return;
    }
    _avatarOperationInFlight = inFlight;
    if (_cover) {
        _cover->setAvatarOperationInFlight(inFlight);
    }
}

void AccountSettingsPage::editDisplayName() {
    const auto currentName = _accountSummaryLoaded
        ? _accountSummary.displayName
        : (_controller ? _controller->displayName() : QString());
    auto *dlg = new AccountEditNameDialog(currentName, this);
    if (dlg->exec() == AccountEditNameDialog::Accepted) {
        const auto newName = dlg->displayName();
        if (!newName.isEmpty() && newName != currentName) {
            if (auto *bridge = _controller ? _controller->bridge() : nullptr) {
                bridge->setDisplayName(newName);
            }
        }
    }
    dlg->deleteLater();
}

void AccountSettingsPage::rebuildSections() {
    if (!_sectionsLayout) {
        return;
    }
    clearSettingsLayout(_sectionsLayout);

    _sectionsLayout->addSpacing(14);

    // Identity block: display name (click to edit), username and homeserver as
    // labeled rows, above the collapsible contact details.
    {
        const auto name = _accountSummaryLoaded
            ? _accountSummary.displayName
            : (_controller ? _controller->displayName() : QString());
        const auto username = _accountSummaryLoaded
            ? _accountSummary.userId
            : (_controller ? _controller->userId() : QString());
        const auto server = _settings ? _settings->sessionHomeserver() : QString();
        addSettingsInfoRow(
            _sections, _sectionsLayout, tr("Display name"), name,
            /*monospace=*/false, /*copyButton=*/false,
            [this] { editDisplayName(); });
        addSettingsInfoRow(
            _sections, _sectionsLayout, tr("Username"), username,
            /*monospace=*/false, /*copyButton=*/true);
        if (!server.isEmpty()) {
            addSettingsInfoRow(
                _sections, _sectionsLayout, tr("Homeserver"), server,
                /*monospace=*/false, /*copyButton=*/true);
        }
    }
    _sectionsLayout->addSpacing(st::settingsCheckboxesSkip);

    // Collapsed by default: contact details are rarely what the page is opened
    // for, and both blocks together push everything else off screen.
    auto *contacts = new SettingsExpandButton(
        tr("Emails and phones"),
        _threepidsExpanded,
        _sections);
    contacts->setClickedCallback([this] {
        _threepidsExpanded = !_threepidsExpanded;
        rebuildSections();
    });
    _sectionsLayout->addWidget(contacts);
    if (_threepidsExpanded) {
        // A delegated-auth homeserver owns email + phone itself and turns the
        // 3PID API off — there is nothing to show or edit, so the block holds
        // the note pointing at the website instead of the two sections. It stays
        // inside the same collapsible either way: the question the user came to
        // answer is the same one, only the answer differs.
        if (managesThreepidsExternally()) {
            addAccountManagementNote();
        } else {
            addEmailSection();
            _sectionsLayout->addSpacing(8);
            addPhoneSection();
        }
    }

    _sectionsLayout->addSpacing(st::settingsCheckboxesSkip);

    const bool canChange = !_accountSummaryLoaded
        || _accountSummary.capabilities.canChangePassword;
    if (canChange) {
        auto *changePassword = new SettingsLinkButton(
            tr("Change password"),
            st::windowActiveTextFg,
            _sections);
        changePassword->setClickedCallback([this] {
            auto *dlg = new AccountChangePasswordDialog(this);
            if (dlg->exec() == AccountChangePasswordDialog::Accepted) {
                _pendingNewPassword = dlg->newPassword();
                if (!_pendingNewPassword.isEmpty()) {
                    if (auto *bridge = _controller ? _controller->bridge() : nullptr) {
                        const auto curPw = dlg->currentPassword();
                        QString authJson;
                        if (!curPw.isEmpty()) {
                            QJsonObject identifier;
                            identifier[QStringLiteral("type")] = QStringLiteral("m.id.user");
                            identifier[QStringLiteral("user")] =
                                _controller ? _controller->userId() : QString();
                            QJsonObject auth;
                            auth[QStringLiteral("type")] = QStringLiteral("m.login.password");
                            auth[QStringLiteral("identifier")] = identifier;
                            auth[QStringLiteral("password")] = curPw;
                            authJson = QString::fromUtf8(
                                QJsonDocument(auth).toJson(QJsonDocument::Compact));
                        }
                        bridge->changePassword(_pendingNewPassword, authJson);
                    }
                }
            }
            dlg->deleteLater();
        });
        _sectionsLayout->addWidget(changePassword);
    }

    auto *deactivate = new SettingsLinkButton(
        tr("Deactivate account"),
        st::attentionButtonFg,
        _sections);
    deactivate->setClickedCallback([this] {
        HistoryConfirmDialog confirm(
            this,
            QString(),
            tr("Are you sure you want to deactivate your account?\n\n"
               "This action is permanent and cannot be undone. "
               "All your data will be lost."),
            tr("Deactivate"),
            QString(),
            HistoryConfirmDialog::Attention,
            st::signOutConfirmWidth,
            st::boxButtonPadding.bottom() + Style::ConvertScale(10));
        if (confirm.exec() == HistoryConfirmDialog::Accepted) {
            if (auto *bridge = _controller ? _controller->bridge() : nullptr) {
                bridge->deactivateAccount(true);
            }
        }
    });
    _sectionsLayout->addWidget(deactivate);

    _sectionsLayout->addSpacing(st::settingsCheckboxesSkip);

    auto *logout = new SettingsLinkButton(
        tr("Sign out"),
        st::attentionButtonFg,
        _sections);
    logout->setClickedCallback([this] {
        Q_EMIT logoutRequested();
    });
    _sectionsLayout->addWidget(logout);

    // Below a divider under Sign out: the other signed-in accounts (click to
    // switch), then "Add Account".
    if (_controller) {
        const auto &domain = _controller->domain();
        QVector<int> others;
        for (int i = 0; i < domain.count(); ++i) {
            if (i != domain.activeIndex()
                && domain.account(i)->settings().hasSession()) {
                others.push_back(i);
            }
        }
        const bool canAdd = domain.canAddAccount();
        if (!others.isEmpty() || canAdd) {
            _sectionsLayout->addSpacing(st::settingsCheckboxesSkip);
            addSettingsDivider(_sections, _sectionsLayout);
            _sectionsLayout->addSpacing(st::settingsCheckboxesSkip);
            for (const int i : others) {
                auto *row = new SettingsAccountRow(
                    _controller,
                    i,
                    domain.account(i)->settings().sessionUserId(),
                    _sections);
                row->setClickedCallback([this, i] {
                    _controller->activateAccount(i);
                });
                _sectionsLayout->addWidget(row);
            }
            if (canAdd) {
                auto *addAccount = new SettingsAccountRow(
                    _controller,
                    /*accountIndex=*/-1,
                    tr("Add Account"),
                    _sections);
                addAccount->setClickedCallback([this] {
                    _controller->showAddAccountIntro();
                });
                _sectionsLayout->addWidget(addAccount);
            }
        }
    }

    _sectionsLayout->addStretch(1);
}

} // namespace TeleMatrix
