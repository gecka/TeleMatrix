// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include "app/account_index.h"

namespace TeleMatrix::Core { class Settings; }

namespace TeleMatrix::Local {

// Initialize the storage subsystem. Must be called once at startup.
// Creates the tdata/ directory if needed.
void start();

// Read the device settings and the account list from the versioned JSON file.
// Returns false if no settings file exists or it is not a version we read, in
// which case both outputs keep their defaults (no accounts).
bool readSettings(Core::Settings &settings, AccountIndex &accounts);

// Write the device settings and account list. One atomic write covers both, so
// the active-account pointer can never disagree with the list it points into.
// Returns false if the atomic write/commit fails.
bool writeSettings(const Core::Settings &settings, const AccountIndex &accounts);

} // namespace TeleMatrix::Local
