// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "app_main_window.h"
#include "app_controller.h"
#include "core/core_settings.h"
#include "history/history_confirm_dialog.h"
#include "theme/theme_manager.h"

#include "styles/style_constants.h"

#ifdef Q_OS_MAC
#include "ui/platform/confirm_quit_mac.h"
#include "ui/platform/ui_utility_mac.h"
#endif

#include <QMenuBar>
#include <QMenu>
#include <QAction>
#include <QCloseEvent>
#include <QEvent>
#include <QApplication>
#include <QPalette>
#include <QLineEdit>
#include <QScreen>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextEdit>
#include <QTextFormat>

#include <utility>

using namespace Qt::Literals::StringLiterals;

namespace TeleMatrix {

namespace {
// Use st:: constants directly — they are runtime-scaled via st::initPxValues().
#define kDefaultWidth  st::windowDefaultWidth
#define kDefaultHeight st::windowDefaultHeight
#define kMinWidth      st::windowMinWidth
#define kMinHeight     st::windowMinHeight

int screenChecksum(const QScreen *screen) {
    if (!screen) {
        return 0;
    }
    const auto sg = screen->geometry();
    const uint32_t crc =
        uint32_t(sg.x()) ^ (uint32_t(sg.y()) << 8)
        ^ (uint32_t(sg.width()) << 16) ^ (uint32_t(sg.height()) << 24);
    return static_cast<int32_t>(crc);
}

QScreen *screenForPosition(const Core::WindowPosition &position) {
    if (position.moncrc != 0) {
        for (auto *screen : QGuiApplication::screens()) {
            if (screenChecksum(screen) == position.moncrc) {
                return screen;
            }
        }
    }
    const auto saved = position.rect();
    if (saved.isValid()) {
        if (auto *screen = QGuiApplication::screenAt(saved.center())) {
            return screen;
        }
    }
    return QGuiApplication::primaryScreen();
}

QRect clampToAvailableGeometry(QRect rect, const QRect &available) {
    if (!rect.isValid() || !available.isValid()) {
        return rect;
    }

    rect.setWidth(qMin(qMax(rect.width(), kMinWidth), available.width()));
    rect.setHeight(qMin(qMax(rect.height(), kMinHeight), available.height()));

    const auto maxX = available.x() + available.width() - rect.width();
    const auto maxY = available.y() + available.height() - rect.height();
    rect.moveLeft(qBound(available.x(), rect.x(), maxX));
    rect.moveTop(qBound(available.y(), rect.y(), maxY));
    return rect;
}

// Direct formatting helpers — apply formatting on the focused QTextEdit.
// Menu bar owns the shortcuts (so macOS doesn't pass them to the system),
// and these callbacks apply the formatting directly without synthetic events.

QTextEdit *focusedEdit() {
    return qobject_cast<QTextEdit *>(QApplication::focusWidget());
}

void fmtToggleBold() {
    auto *e = focusedEdit(); if (!e) return;
    auto c = e->textCursor(); if (!c.hasSelection()) return;
    QTextCharFormat fmt;
    fmt.setFontWeight(c.charFormat().fontWeight() == QFont::Bold
        ? QFont::Normal : QFont::Bold);
    c.mergeCharFormat(fmt);
    e->setTextCursor(c);
}

void fmtToggleItalic() {
    auto *e = focusedEdit(); if (!e) return;
    auto c = e->textCursor(); if (!c.hasSelection()) return;
    QTextCharFormat fmt;
    fmt.setFontItalic(!c.charFormat().fontItalic());
    c.mergeCharFormat(fmt);
    e->setTextCursor(c);
}

void fmtToggleUnderline() {
    auto *e = focusedEdit(); if (!e) return;
    auto c = e->textCursor(); if (!c.hasSelection()) return;
    QTextCharFormat fmt;
    fmt.setFontUnderline(!c.charFormat().fontUnderline());
    c.mergeCharFormat(fmt);
    e->setTextCursor(c);
}

void fmtToggleStrikethrough() {
    auto *e = focusedEdit(); if (!e) return;
    auto c = e->textCursor(); if (!c.hasSelection()) return;
    QTextCharFormat fmt;
    fmt.setFontStrikeOut(!c.charFormat().fontStrikeOut());
    c.mergeCharFormat(fmt);
    e->setTextCursor(c);
}

/// Ensure the selection boundaries align with block (paragraph) boundaries.
/// If the selection starts or ends in the middle of a block, insert
/// paragraph separators to split the block so that block-level formatting
/// (code block, quote) only affects the selected lines.
/// Returns the updated (selStart, selEnd) positions.
std::pair<int,int> ensureBlockBoundaries(QTextEdit *e, QTextCursor &c) {
    int selStart = c.selectionStart();
    int selEnd = c.selectionEnd();
    auto *doc = e->document();

    c.beginEditBlock();

    // Split at the end of selection if it doesn't end at a block boundary.
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

    // Split at the start of selection if it doesn't start at a block boundary.
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

    // Update the cursor to select the (possibly adjusted) range.
    c.setPosition(selStart);
    c.setPosition(selEnd, QTextCursor::KeepAnchor);
    e->setTextCursor(c);

    return {selStart, selEnd};
}

void fmtToggleMonospace() {
    auto *e = focusedEdit(); if (!e) return;
    auto c = e->textCursor();

    auto applyMonoFont = [e](QTextCursor &cur, bool enable) {
        QTextCharFormat fmt;
        if (enable) {
            auto mono = st::monospaceFont(e->font().pixelSize());
            fmt.setFont(mono);
            fmt.setFontFixedPitch(true);
        } else {
            fmt.setFontFixedPitch(false);
            fmt.setFont(st::baseFont(e->font().pixelSize()));
        }
        cur.mergeCharFormat(fmt);
    };

    // No selection: only allow removing code block from current block.
    if (!c.hasSelection()) {
        if (!c.blockFormat().nonBreakableLines()) return;
        QTextBlockFormat bfmt;
        bfmt.setNonBreakableLines(false);
        c.setBlockFormat(bfmt);
        c.movePosition(QTextCursor::StartOfBlock);
        c.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
        applyMonoFont(c, false);
        e->setTextCursor(c);
        e->viewport()->update();
        return;
    }

    // Split blocks at selection boundaries so block-level formatting
    // only applies to the selected lines.
    auto [selStart, selEnd] = ensureBlockBoundaries(e, c);

    bool allPre = true;
    auto block = e->document()->findBlock(selStart);
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
    c.beginEditBlock();
    block = e->document()->findBlock(selStart);
    while (block.isValid() && block.position() < selEnd) {
        QTextCursor blockCursor(block);
        QTextBlockFormat bfmt = block.blockFormat();
        bfmt.setNonBreakableLines(newPre);
        bfmt.setTopMargin((newPre && isFirst)
            ? (preStyle.header + 2 * preStyle.verticalSkip) : 0);
        bfmt.setBottomMargin(newPre ? (2 * preStyle.verticalSkip) : 0);
        blockCursor.setBlockFormat(bfmt);
        isFirst = false;
        blockCursor.movePosition(QTextCursor::StartOfBlock);
        blockCursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
        applyMonoFont(blockCursor, newPre);
        block = block.next();
    }
    c.endEditBlock();
    e->viewport()->update();
}

void fmtApplyQuote() {
    auto *e = focusedEdit(); if (!e) return;
    auto c = e->textCursor();

    // No selection: only allow removing quote from current block.
    if (!c.hasSelection()) {
        if (c.blockFormat().property(QTextFormat::BlockQuoteLevel).toInt() <= 0)
            return;
        QTextBlockFormat bfmt;
        bfmt.setProperty(QTextFormat::BlockQuoteLevel, 0);
        c.setBlockFormat(bfmt);
        e->setTextCursor(c);
        e->viewport()->update();
        return;
    }

    // Split blocks at selection boundaries.
    auto [selStart, selEnd] = ensureBlockBoundaries(e, c);

    bool allQuoted = true;
    auto block = e->document()->findBlock(selStart);
    while (block.isValid() && block.position() < selEnd) {
        if (block.blockFormat().property(QTextFormat::BlockQuoteLevel).toInt() <= 0) {
            allQuoted = false;
            break;
        }
        block = block.next();
    }

    const int newLevel = allQuoted ? 0 : 1;
    const auto &bqStyle = st::historyBlockquoteStyle;
    bool isFirst = true;
    c.beginEditBlock();
    block = e->document()->findBlock(selStart);
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
    c.endEditBlock();
    e->viewport()->update();
}

void fmtClearFormatting() {
    auto *e = focusedEdit(); if (!e) return;
    auto c = e->textCursor(); if (!c.hasSelection()) return;

    // Clear inline formatting.
    QTextCharFormat fmt;
    QFont f = e->font();
    fmt.setFont(f);
    fmt.setFontWeight(QFont::Normal);
    fmt.setFontItalic(false);
    fmt.setFontUnderline(false);
    fmt.setFontStrikeOut(false);
    fmt.setFontFixedPitch(false);
    fmt.setAnchor(false);
    fmt.setAnchorHref(QString());
    fmt.setForeground(e->palette().color(QPalette::Text));
    c.setCharFormat(fmt);

    // Clear block-level formatting (code block / blockquote).
    const int selStart = c.selectionStart();
    const int selEnd = c.selectionEnd();
    c.beginEditBlock();
    auto block = e->document()->findBlock(selStart);
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
    c.endEditBlock();

    e->setTextCursor(c);
    e->viewport()->update();
}

} // namespace

AppMainWindow::AppMainWindow(AppController *controller, QWidget *parent)
    : QMainWindow(parent)
    , _controller(controller)
{
    setWindowTitle(u"TeleMatrix"_s);
    setMinimumSize(kMinWidth, kMinHeight);

    // Apply current theme palette (set by ThemeManager before window creation).
    QPalette pal;
    pal.setColor(QPalette::Window, st::windowBg);
    pal.setColor(QPalette::WindowText, st::windowFg);
    pal.setColor(QPalette::Base, st::windowBg);
    pal.setColor(QPalette::AlternateBase, st::windowBgOver);
    pal.setColor(QPalette::Text, st::windowFg);
    pal.setColor(QPalette::Button, st::windowBgOver);
    pal.setColor(QPalette::ButtonText, st::windowFg);
    pal.setColor(QPalette::Highlight, st::windowBgActive);
    pal.setColor(QPalette::HighlightedText, st::windowFgActive);
    setPalette(pal);

    // Refresh own palette when theme switches.
    if (auto *tm = controller->themeManager()) {
        connect(tm, &Theme::ThemeManager::themeChanged,
                this, [this](bool /*isNight*/, Theme::ThemeMode /*mode*/) {
            QPalette pal;
            pal.setColor(QPalette::Window, st::windowBg);
            pal.setColor(QPalette::WindowText, st::windowFg);
            pal.setColor(QPalette::Base, st::windowBg);
            pal.setColor(QPalette::AlternateBase, st::windowBgOver);
            pal.setColor(QPalette::Text, st::windowFg);
            pal.setColor(QPalette::Button, st::windowBgOver);
            pal.setColor(QPalette::ButtonText, st::windowFg);
            pal.setColor(QPalette::Highlight, st::windowBgActive);
            pal.setColor(QPalette::HighlightedText, st::windowFgActive);
            setPalette(pal);
            update();
        });
    }

    setupMenuBar();
    setupUtilityShortcuts();
    // Keep default size for a predictable first paint. Controller restores
    // persisted geometry before the first show().
    resize(kDefaultWidth, kDefaultHeight);

#ifdef Q_OS_MAC
    // Force sRGB color space on the window's backing store so Qt's raster
    // engine output matches Preview.app / Chromium color management.
    Platform::ForceWindowSRGB(this);
#endif
}

void AppMainWindow::setupMenuBar() {
#ifdef Q_OS_MAC
    // macOS-only: QMenuBar goes to the global menu bar; in-window elsewhere.
    auto *menuBar = this->menuBar();

    // File menu.
    auto *fileMenu = menuBar->addMenu(tr("&File"));

    // About — macOS relocates AboutRole actions into the application menu.
    auto *aboutAction = fileMenu->addAction(tr("About TeleMatrix"));
    aboutAction->setMenuRole(QAction::AboutRole);
    connect(aboutAction, &QAction::triggered, this, &AppMainWindow::showAboutBox);

    auto *quitAction = fileMenu->addAction(tr("&Quit"));
    quitAction->setShortcut(QKeySequence::Quit);
    connect(quitAction, &QAction::triggered, this, [this] {
        requestQuit(QuitReason::KeyboardShortcut);
    });

    // Edit menu (standard).
    auto *editMenu = menuBar->addMenu(tr("&Edit"));
    editMenu->addAction(tr("&Undo"), QKeySequence::Undo);
    editMenu->addAction(tr("&Redo"), QKeySequence::Redo);
    editMenu->addSeparator();
    editMenu->addAction(tr("Cu&t"), QKeySequence::Cut);
    editMenu->addAction(tr("&Copy"), QKeySequence::Copy);
    editMenu->addAction(tr("&Paste"), QKeySequence::Paste);
    editMenu->addAction(tr("Select &All"), QKeySequence::SelectAll);

    // Formatting shortcuts — registered in the menu bar so macOS respects
    // them instead of passing through to system shortcuts.
    // Callbacks apply formatting directly on the focused QTextEdit.
    editMenu->addSeparator();
    auto *fmtMenu = editMenu->addMenu(tr("Formatting"));

    auto addFmt = [fmtMenu](const QString &label, const QKeySequence &seq,
                             void(*fn)()) {
        auto *act = fmtMenu->addAction(label, fn);
        act->setShortcut(seq);
    };

    addFmt(tr("Bold"), QKeySequence::Bold, fmtToggleBold);
    addFmt(tr("Italic"), QKeySequence::Italic, fmtToggleItalic);
    addFmt(tr("Underline"), QKeySequence::Underline, fmtToggleUnderline);
    addFmt(tr("Strikethrough"),
           QKeySequence(Qt::SHIFT | Qt::CTRL | Qt::Key_X), fmtToggleStrikethrough);
    // Quote: On macOS, Cmd+Shift+. produces Key_Greater with ControlModifier
    // (Shift is consumed by key mapping). Register the actual key combo macOS
    // delivers so the QAction fires correctly.
#ifdef Q_OS_MAC
    addFmt(tr("Quote"),
           QKeySequence(Qt::CTRL | Qt::Key_Greater), fmtApplyQuote);
#else
    addFmt(tr("Quote"),
           QKeySequence(Qt::SHIFT | Qt::CTRL | Qt::Key_Period), fmtApplyQuote);
#endif
    addFmt(tr("Monospace"),
           QKeySequence(Qt::SHIFT | Qt::CTRL | Qt::Key_M), fmtToggleMonospace);
    fmtMenu->addSeparator();
    addFmt(tr("Clear formatting"),
           QKeySequence(Qt::SHIFT | Qt::CTRL | Qt::Key_N), fmtClearFormatting);

    // Settings (Cmd+,) — macOS standard preferences shortcut.
    auto *settingsAction = fileMenu->addAction(tr("&Settings..."));
    auto settingsShortcuts = QKeySequence::keyBindings(QKeySequence::Preferences);
    const auto explicitPreferencesShortcut = QKeySequence(Qt::CTRL | Qt::Key_Comma);
    if (!settingsShortcuts.contains(explicitPreferencesShortcut)) {
        settingsShortcuts.push_back(explicitPreferencesShortcut);
    }
    settingsAction->setShortcuts(settingsShortcuts);
    settingsAction->setShortcutContext(Qt::ApplicationShortcut);
    settingsAction->setMenuRole(QAction::PreferencesRole);
    addAction(settingsAction);
    connect(settingsAction, &QAction::triggered,
            this, &AppMainWindow::settingsRequested);

    setupAccountShortcuts();

    // View menu.
    menuBar->addMenu(tr("&View"));
#else // Q_OS_MAC
    // No in-window menu bar; register the custom shortcuts on the window instead.
    const auto addShortcut = [this](const QKeySequence &seq, void (*fn)()) {
        auto *act = new QAction(this);
        act->setShortcut(seq);
        act->setShortcutContext(Qt::WindowShortcut);
        connect(act, &QAction::triggered, this, fn);
        addAction(act);
    };
    addShortcut(QKeySequence::Bold, fmtToggleBold);
    addShortcut(QKeySequence::Italic, fmtToggleItalic);
    addShortcut(QKeySequence::Underline, fmtToggleUnderline);
    addShortcut(QKeySequence(Qt::SHIFT | Qt::CTRL | Qt::Key_X), fmtToggleStrikethrough);
    addShortcut(QKeySequence(Qt::SHIFT | Qt::CTRL | Qt::Key_Period), fmtApplyQuote);
    addShortcut(QKeySequence(Qt::SHIFT | Qt::CTRL | Qt::Key_M), fmtToggleMonospace);
    addShortcut(QKeySequence(Qt::SHIFT | Qt::CTRL | Qt::Key_N), fmtClearFormatting);

    auto *settingsAction = new QAction(this);
    auto settingsShortcuts = QKeySequence::keyBindings(QKeySequence::Preferences);
    const auto explicitPreferencesShortcut = QKeySequence(Qt::CTRL | Qt::Key_Comma);
    if (!settingsShortcuts.contains(explicitPreferencesShortcut)) {
        settingsShortcuts.push_back(explicitPreferencesShortcut);
    }
    settingsAction->setShortcuts(settingsShortcuts);
    settingsAction->setShortcutContext(Qt::WindowShortcut);
    connect(settingsAction, &QAction::triggered,
            this, &AppMainWindow::settingsRequested);
    addAction(settingsAction);

    setupAccountShortcuts();
#endif // Q_OS_MAC
}

void AppMainWindow::setupUtilityShortcuts() {
    // Deliberately not in the menu bar.
    const auto add = [this](const QKeySequence &seq, auto signal) {
        auto *action = new QAction(this);
        action->setShortcut(seq);
        action->setShortcutContext(Qt::ApplicationShortcut);
        connect(action, &QAction::triggered, this, signal);
        addAction(action);
    };
    add(QKeySequence(Qt::CTRL | Qt::Key_K),
        &AppMainWindow::exploreRoomsRequested);
    add(QKeySequence(Qt::CTRL | Qt::Key_F),
        &AppMainWindow::searchRequested);
}

void AppMainWindow::setupAccountShortcuts() {
    // Ctrl/Cmd+Shift+1..N jump straight to an account, as in tdesktop. The
    // actions exist for every slot the app allows and simply do nothing when
    // fewer accounts are signed in, so they don't need rebuilding as accounts
    // come and go.
    for (int ordinal = 1; ordinal <= kMaxAccounts; ++ordinal) {
        auto *action = new QAction(this);
        action->setShortcut(QKeySequence(
            Qt::CTRL | Qt::SHIFT | static_cast<Qt::Key>(Qt::Key_0 + ordinal)));
        action->setShortcutContext(Qt::ApplicationShortcut);
        connect(action, &QAction::triggered, this, [this, ordinal] {
            if (_controller) {
                _controller->activateAccountByOrdinal(ordinal);
            }
        });
        addAction(action);
    }

    // Ctrl/Cmd+Shift+] and Ctrl/Cmd+Shift+[ cycle through accounts (the
    // macOS next/previous-tab idiom). Text editors never claim these, so
    // unlike Alt+arrows they also work while typing. Depending on layout and
    // platform the key may arrive as the bracket or the shifted brace, so
    // register both.
    auto *nextAccount = new QAction(this);
    nextAccount->setShortcuts({
        QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_BracketRight),
        QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_BraceRight),
    });
    nextAccount->setShortcutContext(Qt::ApplicationShortcut);
    connect(nextAccount, &QAction::triggered, this, [this] {
        if (_controller) {
            _controller->activateAdjacentAccount(1);
        }
    });
    addAction(nextAccount);

    auto *previousAccount = new QAction(this);
    previousAccount->setShortcuts({
        QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_BracketLeft),
        QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_BraceLeft),
    });
    previousAccount->setShortcutContext(Qt::ApplicationShortcut);
    connect(previousAccount, &QAction::triggered, this, [this] {
        if (_controller) {
            _controller->activateAdjacentAccount(-1);
        }
    });
    addAction(previousAccount);
}

void AppMainWindow::showAboutBox() {
    const auto version = tr("version %1")
        .arg(QStringLiteral(TELEMATRIX_VERSION_STR));
    const auto description = tr(
        "A desktop Matrix client with the look and feel of Telegram Desktop.");
    const auto license = tr(
        "This software is licensed under GNU GPL version 3. "
        "Source code is available on %1.")
        .arg(u"<a href=\"https://github.com/gecka/telematrix\">GitHub</a>"_s);
    const auto body =
        version + u"<br><br>"_s + description + u"<br><br>"_s + license;

    HistoryConfirmDialog dialog(
        this,
        tr("TeleMatrix"),
        body,
        tr("Close"),
        QString(),
        HistoryConfirmDialog::Normal,
        0,    // customWidth: default st::boxWidth
        -1,   // customButtonBottomPadding: default
        /*showCancel=*/false,
        /*richText=*/true);
    dialog.exec();
}

void AppMainWindow::useDefaultCentered() {
    const auto restoreSave = _positionPersistenceEnabled;
    _positionPersistenceEnabled = false;

    resize(kDefaultWidth, kDefaultHeight);
    if (const auto *screen = QGuiApplication::primaryScreen()) {
        const auto sg = screen->availableGeometry();
        move(sg.x() + (sg.width() - kDefaultWidth) / 2,
             sg.y() + (sg.height() - kDefaultHeight) / 2);
    }

    _positionPersistenceEnabled = restoreSave;
}

void AppMainWindow::bringToFront() {
    setWindowState((windowState() & ~Qt::WindowMinimized) | Qt::WindowActive);
    show();
    raise();
    activateWindow();
#ifdef Q_OS_MAC
    // A background app raising its own window is blocked by macOS focus-stealing
    // prevention; force-activate the process so it actually comes to the front.
    Platform::ActivateApp();
#endif
}

void AppMainWindow::restoreWindowState() {
    _positionPersistenceEnabled = false;

    const auto &pos = _controller->settings().windowPosition();
    auto applySavedGeometry = [this, &pos] {
        auto *screen = screenForPosition(pos);
        if (!screen) {
            useDefaultCentered();
            return;
        }

        auto saved = pos.rect();
        if (!saved.isValid()) {
            useDefaultCentered();
            return;
        }

        saved.setWidth(qMax(saved.width(), kMinWidth));
        saved.setHeight(qMax(saved.height(), kMinHeight));
        const auto clamped = clampToAvailableGeometry(saved, screen->availableGeometry());
        setGeometry(clamped);
    };

    if (pos.w > 0 && pos.h > 0) {
        applySavedGeometry();
    } else {
        useDefaultCentered();
    }

    if (pos.maximized) {
        showMaximized();
    }

    _positionPersistenceEnabled = true;
}

void AppMainWindow::savePositionToSettings() {
    if (!_controller || !_positionPersistenceEnabled) {
        return;
    }

    if (isMinimized()) {
        return;
    }

    auto pos = _controller->settings().windowPosition();
    pos.scale = 0;

    QRect normalRect;
    if (isMaximized()) {
        pos.maximized = 1;
        normalRect = normalGeometry();
    } else {
        pos.maximized = 0;
        normalRect = geometry();
    }

    if (!normalRect.isValid()) {
        return;
    }

    const auto *targetScreen = this->screen();
    if (!targetScreen) {
        targetScreen = QGuiApplication::screenAt(normalRect.center());
    }
    if (!targetScreen) {
        targetScreen = QGuiApplication::primaryScreen();
    }

    if (targetScreen) {
        normalRect = clampToAvailableGeometry(normalRect, targetScreen->availableGeometry());
        pos.moncrc = screenChecksum(targetScreen);
    } else {
        pos.moncrc = 0;
    }

    pos.x = normalRect.x();
    pos.y = normalRect.y();
    pos.w = normalRect.width();
    pos.h = normalRect.height();

    const auto &current = _controller->settings().windowPosition();
    if (current.x != pos.x || current.y != pos.y
        || current.w != pos.w || current.h != pos.h
        || current.maximized != pos.maximized
        || current.moncrc != pos.moncrc) {
        _controller->settings().setWindowPosition(pos);
        _controller->saveSettingsDelayed();
    }
}

void AppMainWindow::moveEvent(QMoveEvent *e) {
    QMainWindow::moveEvent(e);
    savePositionToSettings(); // Coalesced via delayed timer.
}

void AppMainWindow::resizeEvent(QResizeEvent *e) {
    QMainWindow::resizeEvent(e);
    savePositionToSettings(); // Coalesced via delayed timer.
}

void AppMainWindow::changeEvent(QEvent *e) {
    QMainWindow::changeEvent(e);
    if (e->type() == QEvent::ActivationChange) {
        _windowActive = isActiveWindow();
        emit windowActiveChanged(_windowActive);
    }
}

void AppMainWindow::closeEvent(QCloseEvent *e) {
    Q_EMIT quitRequested(); // break any nested startup loop (e.g. vault unlock)
    savePositionToSettings();
    _controller->saveSettings(); // Immediate save on close.
    QMainWindow::closeEvent(e);
}

void AppMainWindow::requestQuit([[maybe_unused]] QuitReason reason) {
#ifdef Q_OS_MAC
    if (reason == QuitReason::KeyboardShortcut
        && _controller
        && _controller->settings().macWarnBeforeQuit()) {
        const auto keys = Platform::QuitKeysString();
        const auto text = tr("Hold %1 to Quit").arg(keys);
        if (!Platform::ConfirmQuitRunModal(text)) {
            return; // User cancelled.
        }
    }
#endif
    Q_EMIT quitRequested(); // break any nested startup loop (e.g. vault unlock)
    qApp->quit();
}

} // namespace TeleMatrix
