# FFmpeg 7.1.2 public headers

Public headers only — no sources, no binaries. The libraries themselves come
from **Qt**, not from here: official Qt 6.10.1 for macOS ships universal
(`arm64` + `x86_64`) FFmpeg n7.1.2 dylibs in `<qt>/macos/lib` because its
multimedia backend needs them.

## Why these are vendored

`rust/src/video_thumbnail_service.rs` uses `ffmpeg-next` to decode one frame of a
video and MJPEG-encode a thumbnail. That is the only reason this project links
libav at all.

macOS used to satisfy it from Homebrew, which meant:

- an **arm64-only** libav, blocking any universal build;
- a **second** FFmpeg in the bundle (Homebrew's 8.x alongside Qt's 7.1), plus
  ~14 transitive dylibs, about 37 MB;
- **GPL** binaries (Homebrew builds `--enable-gpl` with x264/x265), where Qt's
  build is LGPL.

Linking Qt's copy instead fixes all three. Qt ships the dylibs but no headers and
no `.pc` files, so both are supplied here — headers in `include/`, pkg-config
templates in `cmake/ffmpeg-pc/` (generated into the build tree at configure time
so no absolute path is ever committed).

## Provenance

| | |
|---|---|
| Upstream | <https://ffmpeg.org/releases/ffmpeg-7.1.2.tar.xz> |
| SHA-256 | `089bc60fb59d6aecc5d994ff530fd0dcb3ee39aa55867849a2bbc4e555f9c304` |
| License | LGPL v2.1 or later (see `COPYING.LGPLv2.1`) |

Not hand-copied. Regenerate with FFmpeg's own header-install target:

```sh
tar xf ffmpeg-7.1.2.tar.xz && cd ffmpeg-7.1.2
./configure --prefix=/tmp/ffmpeg-install --disable-x86asm --disable-doc --disable-programs
make install-headers
# copy libav{codec,format,util}, libsw{scale,resample} from /tmp/ffmpeg-install/include
```

`libavutil/avconfig.h` and `libavutil/ffversion.h` are *generated* by configure
rather than shipped in the source tree, which is why the install target is used
instead of copying from the tarball. `avconfig.h` resolves to
`AV_HAVE_BIGENDIAN 0` / `AV_HAVE_FAST_UNALIGNED 1` on both arm64 and x86_64
macOS, so a single copy serves both slices of a universal build.

`libavdevice` and `libavfilter` are installed by that target too but deliberately
not vendored — nothing here uses them.

## Keeping this in step with Qt

Four things are pinned to one another and must move together:

1. these headers,
2. `ffmpeg-next` / `ffmpeg-sys-next` in `rust/Cargo.toml` (major tracks FFmpeg's:
   `7.x` ⇒ FFmpeg 7),
3. the sonames Qt actually ships (`libavcodec.61`, `libavformat.61`,
   `libavutil.59`, `libswscale.8`, `libswresample.5`),
4. the **vcpkg commit** the Windows CI job checks out
   (`.github/workflows/_reusable-build.yml`). Windows takes its headers and import
   libraries from vcpkg's ffmpeg port, whose version floats; the DLLs it ships are
   Qt's, so the two must agree on the major. The pin is the last vcpkg commit on
   ffmpeg 7.1.2. Bump the binary-cache key in that job whenever the pin moves.

A Qt upgrade that moves FFmpeg to 8.x changes those sonames. `CMakeLists.txt`
asserts them at configure time — Qt's dylibs on macOS, pkg-config's major against
Qt's DLLs on Windows — and fails with instructions, so the break is loud and
immediate rather than a link error or — worse — a silent ABI mismatch.

To re-pin: read the version Qt ships (`strings <qt>/macos/lib/libavcodec.*.dylib
| grep -m1 -- --prefix` reveals FFmpeg's own configure line, including its
version), regenerate these headers at that version, bump the crate, and update
the expected sonames and `.pc` versions.
