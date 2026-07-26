// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QImage>
#include <QColor>
#include <QString>
#include <QWidget>

#include <functional>

#include "ui/internal_choice_dialog.h"

class QVBoxLayout;
class QLayout;
class QPushButton;

namespace TeleMatrix {

/// Toggle button row for settings — full-width, text left, toggle right.
class SettingsToggleButton : public QWidget {
    Q_OBJECT

public:
    SettingsToggleButton(const QString &text, bool checked, QWidget *parent = nullptr);

    [[nodiscard]] bool isChecked() const { return _checked; }
    void setChecked(bool checked);

Q_SIGNALS:
    void toggled(bool checked);

protected:
    void paintEvent(QPaintEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void enterEvent(QEnterEvent *e) override;
    void leaveEvent(QEvent *e) override;

private:
    QString _text;
    bool _checked = false;
    bool _hovered = false;
};

/// Menu item button for the settings sidebar — icon left, text label.
class SettingsMenuButton : public QWidget {
    Q_OBJECT

public:
    enum class IconType {
        MyAccount,
        Notifications,
        Encryption,
        Sessions,
        Appearance,
        Preferences,
        Advanced,
        HelpAbout,
    };

    SettingsMenuButton(IconType icon, const QString &text, QWidget *parent = nullptr);

    void setSelected(bool selected);
    [[nodiscard]] bool isSelected() const { return _selected; }

Q_SIGNALS:
    void clicked();

protected:
    void paintEvent(QPaintEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void enterEvent(QEnterEvent *e) override;
    void leaveEvent(QEvent *e) override;

private:
    IconType _iconType;
    QString _text;
    bool _hovered = false;
    bool _selected = false;
    QImage _icon;
    QImage _iconOver;
};

class SettingsValueButton final : public QWidget {
public:
    SettingsValueButton(
        const QString &text,
        const QString &value,
        QWidget *parent = nullptr,
        const QString &iconPrefix = QString(),
        const QString &iconName = QString());

    void setValue(const QString &value);
    /// Retitle the row. Needed where the label itself is state (the updater's
    /// status row), not a fixed caption.
    void setText(const QString &text);
    void setClickedCallback(std::function<void()> callback);

protected:
    void paintEvent(QPaintEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void enterEvent(QEnterEvent *e) override;
    void leaveEvent(QEvent *e) override;

private:
    QString _text;
    QString _value;
    QImage _icon;
    QImage _iconOver;
    bool _hasIcon = false;
    bool _hovered = false;
    std::function<void()> _clicked;
};

/// A row that opens and closes a block below it: label on the left, a chevron on
/// the right pointing down when collapsed and up when expanded.
class SettingsExpandButton final : public QWidget {
public:
    SettingsExpandButton(
        const QString &text,
        bool expanded,
        QWidget *parent = nullptr);

    void setClickedCallback(std::function<void()> callback);

protected:
    void paintEvent(QPaintEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void enterEvent(QEnterEvent *e) override;
    void leaveEvent(QEvent *e) override;

private:
    QString _text;
    bool _expanded = false;
    bool _hovered = false;
    std::function<void()> _clicked;
};

class SettingsLinkButton final : public QWidget {
public:
    SettingsLinkButton(
        const QString &text,
        const QColor &color,
        QWidget *parent = nullptr);

    void setClickedCallback(std::function<void()> callback);
    void setText(const QString &text);

protected:
    void paintEvent(QPaintEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void enterEvent(QEnterEvent *e) override;
    void leaveEvent(QEvent *e) override;

private:
    QString _text;
    QColor _color;
    bool _hovered = false;
    std::function<void()> _clicked;
};

using SettingsChoiceEntry = Ui::InternalChoiceEntry;

class SettingsChoiceRow final : public QWidget {
public:
    SettingsChoiceRow(
        SettingsChoiceEntry entry,
        bool checked,
        QWidget *parent = nullptr);

    void setChecked(bool checked);
    void setClickedCallback(std::function<void(QString)> callback);

protected:
    void paintEvent(QPaintEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void enterEvent(QEnterEvent *e) override;
    void leaveEvent(QEvent *e) override;

private:
    SettingsChoiceEntry _entry;
    bool _checked = false;
    bool _hovered = false;
    std::function<void(QString)> _clicked;
};

QImage loadColorizedSettingsIcon(const QString &name, const QColor &color);
QImage loadColorizedSettingsIconFromPrefix(
    const QString &prefix,
    const QString &name,
    const QColor &color);

QPushButton *createSettingsCopyIconButton(QWidget *parent);

void addSettingsSectionTitle(
    QWidget *parent,
    QVBoxLayout *layout,
    const QString &title,
    const QString &error = QString(),
    bool loading = false);
void addSettingsDivider(QWidget *parent, QVBoxLayout *layout);
SettingsToggleButton *addSettingsToggle(
    QWidget *parent,
    QVBoxLayout *layout,
    const QString &text,
    bool checked);
void addSettingsInfoRow(
    QWidget *parent,
    QVBoxLayout *layout,
    const QString &label,
    const QString &value,
    bool monospace = false,
    bool copyButton = false,
    std::function<void()> onClick = {});
void clearSettingsLayout(QLayout *layout);

} // namespace TeleMatrix
