// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QtGlobal>

namespace TeleMatrix {

// What a video's container header says about progressive playback. Mirrors the
// Rust `media_stream::container::Container` discriminants, delivered over FFI by
// tm_video_stream_container.
enum class VideoContainer : quint8 {
    // Not classified yet, or bytes we couldn't recognise (which is also what a
    // wrong decryption key looks like). Callers must not treat this as proof of
    // anything: it is the "don't know" bucket.
    Unknown = 0,
    // Decodes from the front (moov before mdat, or mkv/webm/avi/ogg/flv).
    Faststart = 1,
    // moov trails mdat: the whole file downloads before the first frame.
    MoovAtEnd = 2,
};

} // namespace TeleMatrix
