// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

// Compatibility header: provides Ui::FlatLabel stub.
//
// When lib_ui is fully integrated, replace with real lib_ui FlatLabel.
#pragma once

#include <QLabel>
#include <QString>

#include "ui/widgets/input_fields.h" // for rpl::producer, rpl::single

namespace Ui {

// Simplified FlatLabel: wraps a QLabel.
// The real lib_ui FlatLabel supports rich text, links, selection,
// and reactive text updates.
class FlatLabel : public QLabel {
    Q_OBJECT

public:
    // Constructor matching lib_ui's FlatLabel(parent, rpl::producer, style).
    template <typename StyleType>
    FlatLabel(QWidget *parent, rpl::producer<QString> text, const StyleType &)
        : QLabel(parent)
    {
        setText(text.value);
    }

    // Simple constructor.
    explicit FlatLabel(QWidget *parent)
        : QLabel(parent)
    {}
    ~FlatLabel() override;

    // Override to match lib_ui naming (QLabel already has setText).
    using QLabel::setText;
};

} // namespace Ui
