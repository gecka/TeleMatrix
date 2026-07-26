// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QFont>
#include <QString>
#include <QVector>
#include <QWidget>

class QEventLoop;
class QLabel;
class QEvent;
class QKeyEvent;
class QMouseEvent;
class QPaintEvent;

namespace Ui {
class ScrollArea;
} // namespace Ui

namespace TeleMatrix::Ui {

struct InternalChoiceEntry {
    QString id;
    QString title;
    QString subtitle;
    QFont titleFont;
    bool enabled = true;
};

class InternalChoiceDialog final : public QWidget {
public:
    enum DialogCode { Rejected = 0, Accepted = 1 };

    explicit InternalChoiceDialog(
        QWidget *parent,
        const QString &title,
        QVector<InternalChoiceEntry> entries,
        const QString &current);
    ~InternalChoiceDialog() override;

    [[nodiscard]] int exec();
    [[nodiscard]] QString chosenId() const;

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    void accept();
    void reject();
    void syncScrollContentWidth();

    QWidget *_panel = nullptr;
    ::Ui::ScrollArea *_scroll = nullptr;
    QEventLoop *_loop = nullptr;
    QString _chosen;
    int _result = Rejected;
};

} // namespace TeleMatrix::Ui
