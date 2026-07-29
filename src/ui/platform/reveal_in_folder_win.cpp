// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.
//
// Windows-only: compiled solely on WIN32 (see the if(WIN32) block in
// CMakeLists.txt). Not built or verified on the macOS dev host.

#include "ui/platform/reveal_in_folder.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>

#include <QDir>
#include <QString>

#include <string>

namespace TeleMatrix::Platform {

void RevealInFolder(const QString &filepath) {
    auto nativePath = QDir::toNativeSeparators(filepath);
    const auto path = nativePath.toStdWString();

    // The shell API selects the item inside an already-open Explorer window
    // instead of spawning a second one, so it is the preferred route. It needs
    // COM, which the Qt Windows platform plugin has already initialised on the
    // GUI thread.
    if (const auto pidl = ::ILCreateFromPathW(path.c_str())) {
        ::SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
        ::ILFree(pidl);
        return;
    }

    // Fallback: explorer.exe parses /select, out of one flat command line, so a
    // quote inside the file name has to be doubled or it would end the argument.
    const auto command = (QStringLiteral("/select,")
        + nativePath.replace('"', QStringLiteral("\"\""))).toStdWString();
    ::ShellExecuteW(
        nullptr,
        nullptr,
        L"explorer",
        command.c_str(),
        nullptr,
        SW_SHOWNORMAL);
}

} // namespace TeleMatrix::Platform
