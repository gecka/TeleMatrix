// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include <QtTest>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>

#include "app/dialogs_width.h"

using namespace TeleMatrix;

namespace {

constexpr int kMin = 260;     // st::columnMinimalWidthLeft
constexpr int kMax = 540;     // st::columnMaximalWidthLeft
constexpr int kDefault = 300; // kSidebarDefaultWidth
constexpr int kContentMin = 300;

// The app's splitter: a rooms-list column that must not stretch, next to a
// timeline that absorbs everything else.
class Host : public QWidget {
public:
    Host() {
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        splitter = new QSplitter(Qt::Horizontal, this);
        auto *dialogs = new QWidget(splitter);
        dialogs->setMinimumWidth(kMin);
        dialogs->setMaximumWidth(kMax);
        auto *history = new QWidget(splitter);
        history->setMinimumWidth(kContentMin);
        splitter->addWidget(dialogs);
        splitter->addWidget(history);
        splitter->setStretchFactor(0, 0);
        splitter->setStretchFactor(1, 1);
        layout->addWidget(splitter);
    }

    // Mirrors AppMainWidget::restoreDialogsWidth(): applied once, from the
    // first resize the widget sees.
    void restore(int savedWidth) {
        if (restored || width() <= 0) return;
        const int sidebar = Dialogs::RestoredWidth(
            savedWidth, kDefault, kMin, kMax);
        splitter->setSizes({ sidebar, qMax(0, width() - sidebar) });
        restored = true;
    }

    [[nodiscard]] int sidebarWidth() const { return splitter->sizes().value(0); }

    QSplitter *splitter = nullptr;
    bool restored = false;
};

} // namespace

class TestDialogsWidth : public QObject {
    Q_OBJECT

private slots:
    // The regression. The main widget is built inside a window still at its
    // pre-maximize size, so the restore runs at ~880px and only afterwards does
    // the queued showMaximized() widen it. The column must come back at the
    // user's width, not at whatever that transient width implied. Storing a
    // ratio failed exactly here: 0.2506 * 880 = 220, clamped up to the 260
    // minimum, and stretch factor 0 then kept it there forever.
    void restoresSavedWidthWhenWindowMaximizesAfterRestore() {
        Host host;
        host.resize(880, 600); // pre-maximize window
        host.show();
        QCoreApplication::processEvents();

        host.restore(451); // user's saved column width
        QCOMPARE(host.sidebarWidth(), 451);

        host.resize(1800, 600); // settleRestoredGeometry() -> showMaximized()
        QCoreApplication::processEvents();
        QCOMPARE(host.sidebarWidth(), 451);
        QVERIFY(host.sidebarWidth() > kMin);
    }

    // A window too narrow to honour the saved width squeezes the column, but
    // the splitter keeps the request and restores it once there is room again.
    void squeezedWidthReExpandsWhenWindowGrows() {
        Host host;
        host.resize(700, 600);
        host.show();
        QCoreApplication::processEvents();

        host.restore(kMax);
        QVERIFY(host.sidebarWidth() < kMax); // no room at 700px

        host.resize(1800, 600);
        QCoreApplication::processEvents();
        QCOMPARE(host.sidebarWidth(), kMax);
    }

    // At the window's minimum size (380px) a wide saved column is wider than
    // the whole window, so the restore asks for a negative timeline width.
    // That must not poison the stored request either.
    void restoreAtMinimumWindowWidthStillReExpands() {
        Host host;
        host.resize(380, 480); // st::windowMinWidth / windowMinHeight
        host.show();
        QCoreApplication::processEvents();

        host.restore(kMax);
        QVERIFY(host.sidebarWidth() >= 0);

        host.resize(1800, 600);
        QCoreApplication::processEvents();
        QCOMPARE(host.sidebarWidth(), kMax);
    }

    // Never-set (0) means "use the default", not "collapse to the minimum".
    void unsetWidthFallsBackToDefault() {
        QCOMPARE(Dialogs::RestoredWidth(0, kDefault, kMin, kMax), kDefault);
    }

    void savedWidthIsClampedToTheColumnLimits() {
        QCOMPARE(Dialogs::RestoredWidth(60, kDefault, kMin, kMax), kMin);
        QCOMPARE(Dialogs::RestoredWidth(9000, kDefault, kMin, kMax), kMax);
        QCOMPARE(Dialogs::RestoredWidth(451, kDefault, kMin, kMax), 451);
    }
};

QTEST_MAIN(TestDialogsWidth)
#include "tst_dialogs_width.moc"
