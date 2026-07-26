// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include "media/video_container.h"

namespace TeleMatrix {

// Should the player paint a determinate download overlay (progress ring + a
// "received / total" badge) rather than an indeterminate spinner?
//
// A MoovAtEnd verdict is a proof that no frame can arrive until the download
// finishes, so progress is shown from 0% — the user sees bytes moving instead of
// a spinner that could mean anything. Faststart suppresses the overlay entirely
// (a frame is imminent; a download bar would be a misleading flash). Unknown
// keeps the pre-existing heuristic: past 20% downloaded with still no frame, it
// is almost certainly non-faststart.
//
// Pure logic, shared by the inline timeline player and the fullscreen overlay so
// the two cannot drift, and unit tested (tst_video_download_overlay_policy).
[[nodiscard]] bool showDeterminateDownload(
    VideoContainer container,
    bool hasFrame,
    bool streamDownloadPending,
    float downloadedFraction);

// Fraction above which an unclassified, frameless stream is assumed to be
// non-faststart. Exposed for the tests and for the call sites' comments.
inline constexpr float kNoFrameDownloadHeuristic = 0.2f;

} // namespace TeleMatrix
