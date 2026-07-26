// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QImage>
#include <QString>

namespace TeleMatrix {

// A small, soft, multi-color "blurry" placeholder, deterministic in `seed`
// (e.g. an event id). Callers upscale it with Qt::SmoothTransformation like the
// blurhash path. Used for videos with no server thumbnail while the first frame
// is being extracted, instead of a flat near-black fill.
[[nodiscard]] QImage syntheticBlurPlaceholder(const QString &seed, int w, int h);

} // namespace TeleMatrix
