// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "dialogs_main_menu_overlay.h"

#include "dialogs_main_menu_panel.h"
#include "app/app_controller.h"
#include "theme/theme_manager.h"
#include "styles/style_constants.h"
#include "ui/focus_restore.h"

#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QVariantAnimation>

#include <algorithm>

namespace TeleMatrix {

DialogsMainMenuOverlay::DialogsMainMenuOverlay(
    AppController *controller,
    QWidget *parent)
: QWidget(parent) {
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_NoSystemBackground, false);
    setAttribute(Qt::WA_StyledBackground, false);
    hide();

    _panel = new DialogsMainMenuPanel(controller, this);
    _panel->setGeometry(-st::mainMenuWidth, 0, st::mainMenuWidth, 0);

    QObject::connect(_panel, &DialogsMainMenuPanel::newRoomClicked, this, [this] {
        hideAnimated();
        emit newRoomRequested();
    });
    QObject::connect(_panel, &DialogsMainMenuPanel::newChatClicked, this, [this] {
        hideAnimated();
        emit newChatRequested();
    });
    QObject::connect(_panel, &DialogsMainMenuPanel::exploreRoomsClicked, this, [this] {
        hideAnimated();
        emit exploreRoomsRequested();
    });
    QObject::connect(_panel, &DialogsMainMenuPanel::savedMessagesClicked, this, [this] {
        hideAnimated();
        emit savedMessagesRequested();
    });
    QObject::connect(_panel, &DialogsMainMenuPanel::verifySessionClicked, this, [this] {
        hideAnimated();
        emit verifySessionRequested();
    });
    QObject::connect(_panel, &DialogsMainMenuPanel::settingsClicked, this, [this] {
        hideAnimated();
        emit settingsRequested();
    });
    // Straight to the picker, not via Settings — so closing it returns to the
    // app rather than opening a settings layer the user never asked for
    // (showThemeSelector only restores Settings if it was already showing).
    QObject::connect(_panel, &DialogsMainMenuPanel::colorThemeClicked, this,
        [this, controller] {
            hideAnimated();
            controller->requestThemeSelector();
        });
    QObject::connect(_panel, &DialogsMainMenuPanel::nightModeToggled, this,
        [controller](bool enabled) {
            if (auto *tm = controller->themeManager()) {
                tm->setMode(enabled
                    ? Theme::ThemeMode::Night
                    : Theme::ThemeMode::Day);
            }
        });
    QObject::connect(_panel, &DialogsMainMenuPanel::signOutClicked, this, [this] {
        hideAnimated();
        emit signOutRequested();
    });
    // Account switching and adding are the controller's business (they rebuild
    // the whole logged-in UI), so they go straight there rather than through the
    // widget tree this menu lives in — same as the night-mode toggle above.
    QObject::connect(_panel, &DialogsMainMenuPanel::accountSwitchRequested, this,
        [this, controller](int accountIndex) {
            hideAnimated();
            controller->activateAccount(accountIndex);
        });
    QObject::connect(_panel, &DialogsMainMenuPanel::addAccountClicked, this,
        [this, controller] {
            hideAnimated();
            controller->showAddAccountIntro();
        });

    if (auto *tm = controller->themeManager()) {
        QObject::connect(tm, &Theme::ThemeManager::themeChanged,
            _panel, [this](bool isNight, Theme::ThemeMode) {
                _panel->applyTheme();
                _panel->setNightModeChecked(isNight);
            });
    }

    _animation = new QVariantAnimation(this);
    _animation->setDuration(st::mainMenuAnimationDuration);
    _animation->setEasingCurve(QEasingCurve::OutCubic);

    QObject::connect(_animation, &QVariantAnimation::valueChanged, this,
        [this](const QVariant &value) {
            applyProgress(value.toReal());
        });

    QObject::connect(_animation, &QVariantAnimation::finished, this,
        [this] {
            if (_progress <= 0.0001) {
                _state = State::Hidden;
                restoreFocusBeforeHide();
                hide();
                emit hidden();
            } else {
                _state = State::Visible;
                setFocus(Qt::OtherFocusReason);
            }
            update();
        });
}

void DialogsMainMenuOverlay::showAnimated() {
    if (_state == State::Visible || _state == State::Opening) {
        return;
    }
    // On reopen during the close animation focus is still ours; keep the
    // original restore target instead of capturing ourselves.
    const auto *focused = QApplication::focusWidget();
    if (!focused || (focused != this && !isAncestorOf(focused))) {
        _restoreFocus = Focus::saveFocusForPopup();
    }
    show();
    raise();
    setFocus(Qt::OtherFocusReason);
    startAnimation(1.0);
}

void DialogsMainMenuOverlay::hideAnimated() {
    if (_state == State::Hidden || _state == State::Closing) {
        return;
    }
    startAnimation(0.0);
}

void DialogsMainMenuOverlay::toggle() {
    if (_state == State::Visible || _state == State::Opening) {
        hideAnimated();
    } else {
        showAnimated();
    }
}

bool DialogsMainMenuOverlay::isShown() const {
    return _state == State::Visible || _state == State::Opening;
}

void DialogsMainMenuOverlay::paintEvent([[maybe_unused]] QPaintEvent *e) {
    QPainter p(this);
    auto bg = st::layerBg;
    bg.setAlpha(qRound(bg.alpha() * _progress));
    p.fillRect(rect(), bg);
}

void DialogsMainMenuOverlay::resizeEvent(QResizeEvent *e) {
    QWidget::resizeEvent(e);
    syncPanelGeometry();
}

void DialogsMainMenuOverlay::mousePressEvent(QMouseEvent *e) {
    if (_state != State::Visible) {
        e->accept();
        return;
    }

    if (!_panel->geometry().contains(e->pos())) {
        hideAnimated();
        e->accept();
        return;
    }

    QWidget::mousePressEvent(e);
}

void DialogsMainMenuOverlay::keyPressEvent(QKeyEvent *e) {
    if (e->key() == Qt::Key_Escape
        && (_state == State::Visible || _state == State::Opening)) {
        hideAnimated();
        e->accept();
        return;
    }
    QWidget::keyPressEvent(e);
}

void DialogsMainMenuOverlay::startAnimation(qreal to) {
    if (_animation) {
        _animation->stop();
        _animation->setStartValue(_progress);
        _animation->setEndValue(to);
        _animation->setEasingCurve(
            to > _progress
                ? QEasingCurve::OutCubic
                : QEasingCurve::InCubic);
        _state = (to > _progress) ? State::Opening : State::Closing;
        _animation->start();
    } else {
        applyProgress(to);
        _state = to > 0.0 ? State::Visible : State::Hidden;
        if (_state == State::Hidden) {
            restoreFocusBeforeHide();
            hide();
            emit hidden();
        } else {
            show();
            raise();
        }
    }
}

// Hiding while still owning focus would make Qt pass it down the tab chain,
// which lands on the chat-list search field. Hand it back to whoever had it
// before the menu opened instead.
void DialogsMainMenuOverlay::restoreFocusBeforeHide() {
    const auto target = _restoreFocus;
    _restoreFocus = nullptr;
    if (!hasFocus()) {
        return; // a dialog or layer claimed focus while the menu was open
    }
    if (target && target->isVisible() && target->isEnabled()) {
        target->setFocus(Qt::OtherFocusReason);
    } else {
        clearFocus();
    }
}

void DialogsMainMenuOverlay::applyProgress(qreal progress) {
    _progress = std::clamp(progress, 0.0, 1.0);
    syncPanelGeometry();
    update();
}

void DialogsMainMenuOverlay::syncPanelGeometry() {
    if (!_panel) {
        return;
    }

    const auto panelWidth = st::mainMenuWidth;
    const auto panelX = qRound(-panelWidth + (_progress * panelWidth));
    _panel->setGeometry(panelX, 0, panelWidth, height());
}

} // namespace TeleMatrix
