// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QDialog>
#include <QTextEdit>

#include "history_prepared_upload.h"
#include "../ui/input_submit_settings.h"
class QScrollArea;

namespace Ui {
class TextButton;
} // namespace Ui

namespace TeleMatrix {

/// Modal preview dialog shown before sending files.
/// Styled as a send-files box.
class HistorySendFilesDialog : public QDialog {
    Q_OBJECT
public:
    HistorySendFilesDialog(
        QWidget *parent,
        const QVector<PreparedFile> &files,
        int sendSubmitWay = 0,
        bool compressImagesDefault = false);

    /// The caption entered by the user.
    [[nodiscard]] QString caption() const;

    /// The prepared files.
    [[nodiscard]] const QVector<PreparedFile> &files() const;

    /// Whether images should be downscaled + re-encoded before sending.
    /// Always false when the selection contains no images.
    [[nodiscard]] bool compressImages() const;

protected:
    void paintEvent(QPaintEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;

private:
    void setupUI();
    void addMediaPreview(int index, const PreparedFile &file, QWidget *container, int &yOffset);
    void addFilePreview(int index, const PreparedFile &file, QWidget *container, int &yOffset);
    void removeFile(int index);
    void rebuildPreviews();
    [[nodiscard]] QString dialogTitle() const;
    [[nodiscard]] bool hasImages() const;

    QVector<PreparedFile> _files;
    InputSubmitSettings _sendSubmitWay = InputSubmitSettings::Enter;
    bool _compressImagesDefault = false;
    QScrollArea *_previewArea = nullptr;
    QWidget *_previewContainer = nullptr;
    QTextEdit *_captionField = nullptr;
    ::Ui::TextButton *_sendButton = nullptr;
    ::Ui::TextButton *_cancelButton = nullptr;
    QWidget *_compressToggle = nullptr; // null when the selection has no images

    static constexpr int kDialogWidth = 364; // px, wide-box width
    static constexpr int kPreviewMaxHeight = 340;
    static constexpr int kCaptionHeight = 44;
    static constexpr int kButtonBarHeight = 52;
    static constexpr int kDialogPadding = 16;
    static constexpr int kDialogRadius = 12;
    static constexpr int kTitleBarHeight = 48;
    static constexpr int kFileCardHeight = 56;
    static constexpr int kFileIconSize = 40;
};

} // namespace TeleMatrix
