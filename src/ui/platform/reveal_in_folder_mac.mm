// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "ui/platform/reveal_in_folder.h"

#include <QFileInfo>
#include <QString>

#include <Cocoa/Cocoa.h>

namespace TeleMatrix::Platform {

void RevealInFolder(const QString &filepath) {
    const auto folder = QFileInfo(filepath).absolutePath();

    @autoreleasepool {
        [[NSWorkspace sharedWorkspace]
                          selectFile:filepath.toNSString()
            inFileViewerRootedAtPath:folder.toNSString()];
    }
}

} // namespace TeleMatrix::Platform
