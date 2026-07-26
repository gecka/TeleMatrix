// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "media/video_download_overlay_policy.h"

namespace TeleMatrix {

bool showDeterminateDownload(
        VideoContainer container,
        bool hasFrame,
        bool streamDownloadPending,
        float downloadedFraction) {
    // A decoded frame means playback started: the seek bar's buffered sub-bar
    // takes over from here. Nothing to show for a local file either, and a
    // complete download is about to produce a frame.
    if (hasFrame || !streamDownloadPending || downloadedFraction >= 1.0f) {
        return false;
    }
    switch (container) {
    case VideoContainer::MoovAtEnd:
        return true;
    case VideoContainer::Faststart:
        return false;
    case VideoContainer::Unknown:
        break;
    }
    return downloadedFraction > kNoFrameDownloadHeuristic;
}

} // namespace TeleMatrix
