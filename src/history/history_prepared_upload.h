// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QImage>
#include <QString>
#include <QVector>

namespace TeleMatrix {

enum class PreparedFileKind { Image, Video, File };

struct PreparedFile {
    QString path;
    QString mime;
    QString filename;
    quint64 size = 0;
    int width = 0;
    int height = 0;
    int durationMs = 0;
    QImage preview;          // thumbnail for display
    PreparedFileKind kind = PreparedFileKind::File;
};

/// Prepare files for upload preview.
/// Classifies each file, probes metadata, generates preview thumbnails.
QVector<PreparedFile> prepareFiles(const QStringList &paths);

/// Prepare a single file.
PreparedFile prepareFile(const QString &path);

} // namespace TeleMatrix
