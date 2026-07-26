# Third-Party Notices

TeleMatrix is distributed under the GNU General Public License v3.0 or later
(with an OpenSSL linking exception); see [`LICENSE`](LICENSE) and [`LEGAL`](LEGAL).
It incorporates and links the third-party components listed below. Each is the
property of its respective authors and is used under the license noted. All of
these licenses are compatible with GPLv3, which governs the combined work.

## C++ / UI

### Desktop App Toolkit — GPL-3.0-or-later (with OpenSSL exception)
Modules: `lib_ui`, `lib_base`, `lib_rpl`, `lib_crl`, `codegen`, `cmake_helpers`.
Source: https://github.com/desktop-app
Copyright (c) 2014-2026 The Desktop App Toolkit Authors.

Vendored as git submodules under `lib/`. **Only the bundled fonts resource
(`lib/lib_ui/fonts/fonts.qrc`) is compiled into TeleMatrix** — the toolkit's GPL
C++ code is not linked (the `src/ui/` widgets are original reimplementations).

### Bundled fonts (via `lib_ui/fonts/fonts.qrc`)
- Open Sans — Apache-2.0. Copyright the Open Sans Project Authors.
- Vazirmatn — SIL Open Font License 1.1. Copyright the Vazirmatn Project Authors.

### Qt 6 — LGPL-3.0 (open source edition)
Components used: Core, Gui, Widgets, Multimedia, Concurrent, Svg.
Source: https://www.qt.io — Copyright (c) The Qt Company Ltd.
On macOS the build bundles Qt's **FFmpeg multimedia backend**
(`libffmpegmediaplugin.dylib`) so `QMediaPlayer` can stream video from the
in-process loopback proxy; see the FFmpeg entry below for the bundled libav.

### FFmpeg (libav) — LGPL-2.1-or-later / GPL-2.0-or-later
Libraries: `libavcodec`, `libavformat`, `libavutil`, `libswscale`, `libswresample`.
Source: https://ffmpeg.org — Copyright (c) the FFmpeg developers.
Used in two places, both compatible with the GPLv3 governing the combined work:
- **Video thumbnails** — Rust (`rust/`) via the `ffmpeg-next` crate linking the
  system libav, decoding one frame for the timeline poster image.
- **Video playback on macOS** — Qt's FFmpeg multimedia backend
  (`libffmpegmediaplugin.dylib`), bundled into `TeleMatrix.app` and used by
  `QMediaPlayer`.
Depending on the FFmpeg build, some components are under GPL-2.0-or-later rather
than LGPL; both are GPLv3-compatible.

### Microsoft GSL — MIT
Source: https://github.com/microsoft/GSL — Copyright (c) 2015 Microsoft Corporation.

### expected (TartanLlama) — CC0-1.0 (public domain dedication)
Source: https://github.com/TartanLlama/expected

## Rust backend (`rust/`)

### matrix-rust-sdk — Apache-2.0
Crates: `matrix-sdk`, `matrix-sdk-ui`, `matrix-sdk-crypto` (0.16).
Source: https://github.com/matrix-org/matrix-rust-sdk
Copyright The Matrix.org Foundation C.I.C. and contributors.

> Apache-2.0 attribution (required by section 4): this product includes software
> developed by The Matrix.org Foundation C.I.C., licensed under the Apache
> License, Version 2.0 (https://www.apache.org/licenses/LICENSE-2.0). Apache-2.0
> code may be relicensed into a GPLv3 work; the combined binary is therefore
> distributed under GPLv3.

### Rust crates — MIT OR Apache-2.0 (unless noted)
`tokio`, `libc`, `async-trait`, `anyhow`, `serde`, `serde_json`, `futures-util`,
`eyeball-im`, `mime`, `dirs`, `tracing`, `tracing-subscriber`, `rand`, `sha2`,
`hkdf`, `chacha20poly1305`.

- `reqwest`/TLS — MIT/Apache-2.0; uses the **rustls** backend (rustls: Apache-2.0 /
  MIT / ISC). `matrix-sdk` and `matrix-sdk-ui` are pinned to `default-features = false`
  + `rustls-tls`, so **native-tls/OpenSSL libssl is not linked** for HTTP.
- OpenSSL **libcrypto** — Apache-2.0 (OpenSSL 3.x). Linked **only** via `rusqlite`
  `bundled-sqlcipher` (SQLCipher's crypto); libssl is not linked. The GPLv3 OpenSSL
  linking exception (see `LEGAL`) covers this libcrypto linkage.
- RustCrypto crates (`sha2`, `hkdf`, `chacha20poly1305`) — MIT/Apache-2.0.

### Bundled storage
- SQLite — Public Domain.
- SQLCipher (via `rusqlite` `bundled-sqlcipher`) — BSD-3-Clause-style license,
  Copyright (c) Zetetic LLC.

---

### Notes
- This file covers the direct, intentionally vendored/linked dependencies.
  Transitive Rust crates are overwhelmingly MIT / Apache-2.0 / BSD / ISC
  permissive licenses, all GPLv3-compatible. To produce a complete crate-level
  manifest for distribution, run `cargo install cargo-about` (or `cargo-license`)
  in `rust/` and regenerate.
- This document is informational and is not a substitute for legal advice.
