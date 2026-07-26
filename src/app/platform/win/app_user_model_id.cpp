// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.
//
// Windows-only: compiled solely on WIN32 (see the if(WIN32) block in
// CMakeLists.txt). Not built or verified on the macOS dev host.

#include "app/platform/win/app_user_model_id.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <objbase.h>
#include <propkey.h>
#include <propvarutil.h>
#include <shlobj.h>

#include <string>

#include <winrt/base.h>

namespace TeleMatrix::Platform::Win {

void setAppUserModelId() {
    ::SetCurrentProcessExplicitAppUserModelID(kAppUserModelId);
}

bool ensureStartMenuShortcut() {
    // %APPDATA%\Microsoft\Windows\Start Menu\Programs\TeleMatrix.lnk
    PWSTR programs = nullptr;
    if (FAILED(::SHGetKnownFolderPath(
            FOLDERID_Programs, KF_FLAG_CREATE, nullptr, &programs))
        || !programs) {
        return false;
    }
    std::wstring linkPath = std::wstring(programs) + L"\\TeleMatrix.lnk";
    ::CoTaskMemFree(programs);

    // Already installed: assume valid. (A stricter check would re-validate that
    // the stored AUMID matches; a missing/renamed shortcut just gets recreated
    // on the next launch.)
    if (::GetFileAttributesW(linkPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return true;
    }

    wchar_t exePath[MAX_PATH] = {};
    if (::GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0) {
        return false;
    }

    try {
        auto link = winrt::create_instance<IShellLinkW>(
            CLSID_ShellLink, CLSCTX_INPROC_SERVER);
        link->SetPath(exePath);

        std::wstring dir(exePath);
        const auto slash = dir.find_last_of(L"\\/");
        if (slash != std::wstring::npos) {
            link->SetWorkingDirectory(dir.substr(0, slash).c_str());
        }

        // Stamp the AUMID into the shortcut — the bit Windows keys toast
        // delivery off of.
        auto store = link.as<IPropertyStore>();
        PROPVARIANT pv;
        if (SUCCEEDED(::InitPropVariantFromString(kAppUserModelId, &pv))) {
            store->SetValue(PKEY_AppUserModel_ID, pv);
            store->Commit();
            ::PropVariantClear(&pv);
        }

        auto file = link.as<IPersistFile>();
        return SUCCEEDED(file->Save(linkPath.c_str(), TRUE));
    } catch (...) {
        return false;
    }
}

} // namespace TeleMatrix::Platform::Win
