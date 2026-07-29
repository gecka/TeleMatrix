# Building & packaging TeleMatrix

TeleMatrix is a Qt 6 / C++20 application with a Rust (`matrix-sdk`) backend linked in
as a static library. The build is driven by CMake:

- The Rust crate (`rust/`) is compiled by a custom CMake command. A `Debug` build uses
  the debuggable `dev-ffi` profile; a `Release` (or unspecified) build uses `--release`.
  Both abort on panic.
- The C FFI header (`build/generated/protocol/protocol_ffi.h`) is generated from the Rust
  crate by the standalone `rust/cbindgen_runner` binary, run via `cargo run` from CMake.
  It regenerates automatically (and re-runs if the header is deleted); there is no
  checked-in shadow copy.
- FFmpeg / libav (`libavcodec`, `libavformat`, `libavutil`, `libswscale`) is a **required**
  dependency, resolved via `pkg-config`. It backs video thumbnails (Rust `ffmpeg-next`) and
  the inline video-streaming backend. The configure step fails if it is not found.

The single source of truth for the app version is `TELEMATRIX_VERSION` near the top of
`CMakeLists.txt` (consumed by the macOS bundle, CPack, and the `TELEMATRIX_VERSION_STR`
compile definition the C++ reads).

Toolchain versions are pinned and must match across local dev and CI: **Qt 6.10.1**
(uses Qt 6.9+ APIs), **rustc 1.96.0** (`rust-toolchain.toml`; matrix-sdk-ui 0.18 needs
≥ 1.93), and **gcc ≥ 12** on Linux.

---

## macOS — `.app` + `.dmg`

Prerequisites:
```sh
brew install cmake create-dmg pkg-config
# Rust toolchain (https://rustup.rs)
# For a universal build also: rustup target add x86_64-apple-darwin
# Qt 6.10.1 — the official build, NOT `brew install qt@6` (see the note below):
#   the online installer (https://www.qt.io/download-qt-installer), or
pip install aqtinstall && aqt install-qt mac desktop 6.10.1 clang_64 -m qtmultimedia qtimageformats -O ~/Qt
```

Build and package:
```sh
# The Qt prefix is only needed the first time — it is stored in the CMake cache.
cmake -B build -DCMAKE_PREFIX_PATH="$HOME/Qt/6.10.1/macos"
cmake --build build -j8 --target TeleMatrix
cmake --build build --target package_dmg      # -> build/TeleMatrix-<version>-<arch>.dmg
```

Notes:
- The deployment target is macOS 15.0 (`CMAKE_OSX_DEPLOYMENT_TARGET`).
- The default build follows the host architecture (arm64 on Apple Silicon). Pass
  `-DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"` for a **universal** binary — which is what
  releases ship, as `TeleMatrix-<version>-universal.dmg`. Each arch is built as a separate
  Rust slice and `lipo`'d into a universal staticlib.
- **No `brew install ffmpeg`.** libav comes from the universal FFmpeg 7.1 that official Qt
  ships for its multimedia backend; headers are vendored in `third_party/ffmpeg-7.1` and
  CMake generates the matching `.pc` files into the build tree. Homebrew's libav was
  arm64-only (which is what used to block universal builds), a second copy of FFmpeg in
  the `.app`, and GPL where Qt's build is LGPL. Configure asserts Qt's FFmpeg sonames, so
  a Qt upgrade that moves to FFmpeg 8 fails immediately with instructions rather than as a
  linker error — see `third_party/ffmpeg-7.1/README.md`.
- **Bare `cargo` commands on macOS need `PKG_CONFIG_PATH`**, because that is how the build
  is told where Qt's libav lives. CMake sets it for its own cargo invocations; for a
  direct `cargo test`/`cargo check`, configure once and then:
  ```sh
  export PKG_CONFIG_PATH="$PWD/build/ffmpeg-pkgconfig"
  ```
  Without it, `ffmpeg-sys-next` resolves whatever FFmpeg is on the default pkg-config path
  (typically Homebrew's 8.x) against the pinned 7.1 crate.
- **Homebrew's Qt will not work.** Video streaming needs Qt's FFmpeg multimedia backend
  (`libffmpegmediaplugin.dylib`): the AVFoundation-only `darwin` backend cannot play the
  loopback HTTP stream the app serves, so video silently falls back to downloading the
  whole file before playing. Homebrew's `qtmultimedia` ships **only**
  `libdarwinmediaplugin.dylib`. Official Qt (online installer / aqtinstall) ships the
  FFmpeg backend, and `macdeployqt` bundles it into the `.app`. Configure hard-fails if
  the resolved Qt lacks it, so this cannot be missed silently.
- **`qtimageformats` is required**, not optional. It supplies the WebP image plugin that
  decodes the emoji sprite atlases (`resources/emoji/`). Without it `QImage(path, "WEBP")`
  returns null and every emoji renders blank — the exact failure this port exists to fix.
  `tests/tst_emoji_atlas.cpp` fails loudly when the plugin is missing, so a Qt install
  without it is caught by `ctest` rather than by a user staring at empty squares.

Signing / notarization (optional):
```sh
cmake -B build \
  -DTELEMATRIX_CODESIGN_IDENTITY="Developer ID Application: …" \
  -DTELEMATRIX_NOTARIZE_PROFILE="my-notary-profile"
```
`TELEMATRIX_CODESIGN_IDENTITY` avoids Keychain prompts. When `TELEMATRIX_NOTARIZE_PROFILE`
is also set (a `xcrun notarytool` keychain profile — create one with
`xcrun notarytool store-credentials`) and a real Developer ID identity is used,
`package_dmg` notarizes and staples the `.dmg`.

---

## Windows — `.exe` + NSIS installer

Prerequisites:
- Visual Studio 2022 (MSVC, "Desktop development with C++").
- Rust with the **MSVC** toolchain: `rustup default stable-x86_64-pc-windows-msvc`.
- Qt 6 for MSVC, **including `qtmultimedia` and `qtimageformats`** (set
  `CMAKE_PREFIX_PATH` / `Qt6_DIR` to the Qt install). `qtimageformats` decodes the
  emoji atlases; without it every emoji renders blank.
- FFmpeg / libav **7.x** with pkg-config `.pc` files (e.g. `vcpkg install ffmpeg:x64-windows`
  + `pkgconfiglite`); point `PKG_CONFIG_PATH` at its `lib/pkgconfig`. Not 8.x:
  `ffmpeg-next` is pinned to 7.1 because macOS links the FFmpeg Qt ships, and
  `ffmpeg-sys-next` 7.x needs `libavcodec/avfft.h`, which FFmpeg 8 removed. vcpkg's
  ffmpeg port floats, so CI pins the vcpkg commit
  (`5087d22fd76fe1844ca492866055dea3c34e7f52`, the last one on 7.1.2) — do the same
  locally if `vcpkg install` gives you 8.x:
  ```bat
  git -C %VCPKG_ROOT% checkout --detach 5087d22fd76fe1844ca492866055dea3c34e7f52
  %VCPKG_ROOT%\bootstrap-vcpkg.bat
  ```
- [NSIS](https://nsis.sourceforge.io/) on `PATH` (for the installer).
- **NASM** (rustls' `aws-lc-rs`/`ring` asm and the vendored OpenSSL/SQLCipher build) and a
  Perl (Strawberry Perl for `openssl-src`).

Build and package:
```bat
cmake -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH=C:\Qt\6.10.1\msvc2022_64
cmake --build build --config Release --target TeleMatrix
cd build && cpack -G NSIS -C Release        REM -> TeleMatrix-<ver>-win64.exe
```

Notes:
- Qt DLLs + plugins are bundled **flat next to `TeleMatrix.exe`** via `windeployqt` (run as a
  post-build step into `build\windeploy\`, then installed/packaged as-is). `--compiler-runtime`
  also bundles the MSVC runtime, so the installed app runs on a clean machine without the
  VC++ redistributable. The staged `build\windeploy\TeleMatrix.exe` is runnable directly; the
  raw `build\<config>\TeleMatrix.exe` is **not** (no Qt DLLs beside it).
- The **libav DLLs that ship are Qt's**, deployed by `windeployqt` for its multimedia
  backend — vcpkg supplies only the headers and import libraries. Same reasoning as macOS:
  one FFmpeg in the package rather than two. Configure asserts the two agree on the major,
  so a vcpkg that drifts fails immediately with instructions.
- A Rust `staticlib` linked into an MSVC exe does **not** auto-propagate the native system
  libs its dependencies need. `CMakeLists.txt` lists the expected set under `if (WIN32)`; if
  the link step reports unresolved externals, regenerate the authoritative list with:
  ```bat
  cd rust && cargo rustc --release -- --print native-static-libs
  ```
  and add any missing libraries.

---

## Linux — `.deb` + `.rpm`

Prerequisites:
- A C++20 compiler (gcc ≥ 12 or clang), CMake ≥ 3.20, `make`/`ninja`.
- Rust toolchain.
- Qt 6 development packages, incl. `qtmultimedia` and **`qtimageformats`** (the latter
  supplies the WebP plugin the emoji atlases need; Debian/Ubuntu:
  `qt6-imageformats-plugins`, Fedora: `qt6-qtimageformats`).
- FFmpeg / libav dev packages: `libavcodec-dev libavformat-dev libavutil-dev libswscale-dev`.
- `libssl-dev` (rusqlite bundled SQLCipher), `libdbus-1-dev` (keyring / Secret Service),
  `nasm` (rustls crypto provider).
- `dpkg-deb` (Debian/Ubuntu) and `rpmbuild` / `rpm` (Fedora/RHEL) for the respective package.

Build and package:
```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8 --target TeleMatrix
cd build && cpack -G "DEB;RPM"
```

Layout & integration:
- Qt is **bundled** (self-contained). The package installs under `/opt/TeleMatrix`
  (binary, Qt libs in `lib/`, plugins in `plugins/`, plus a `qt.conf`); the binary uses
  an `$ORIGIN/../lib` RPATH.
- The maintainer scripts (`resources/linux/deb/*`, `resources/linux/rpm/*`) symlink the
  launcher (`/usr/bin/telematrix`), `.desktop`, icons, and metainfo into the standard
  `/usr` locations on install, and remove them on uninstall.
- The `.desktop` and AppStream metainfo live in `resources/linux/`.

Runtime requirement: secret storage prefers the **Secret Service** API (GNOME Keyring /
KWallet running in the user session). When no Secret Service / D-Bus provider is available,
TeleMatrix falls back to an encrypted master-password file vault, so headless/minimal
distros still work.

Tuning: `lib`/`Depends`/`Requires` lists in `cmake/packaging.cmake` are conservative
starting points — verify on the target distro and adjust (e.g. via `dpkg-shlibdeps`) if
a launch reports a missing library.

### AppImage

A single-file, install-free build that runs on any modern-glibc distro (and on Wayland
via XWayland). Built with `linuxdeploy` + `linuxdeploy-plugin-qt` + `appimagetool`,
independently of the deb/rpm install rules (so one configure yields all three):

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8 --target TeleMatrix
cmake --build build --target package_appimage   # -> build/TeleMatrix-<ver>-x86_64.AppImage
```

`cmake/package_appimage.sh` assembles the AppDir, lets `linuxdeploy-plugin-qt` bundle Qt
(X11/xcb only), force-bundles libav for Qt's FFmpeg video backend, and **verifies** the
`libffmpegmediaplugin.so` + its libav deps landed in the bundle. The three tool AppImages
are auto-downloaded/cached on first use unless the `LINUXDEPLOY`, `LINUXDEPLOY_PLUGIN_QT`,
and `APPIMAGETOOL` env vars point at existing copies. For best portability build on the
oldest glibc you want to support (CI uses Ubuntu 22.04); the container below does this.
There is no auto-update metadata (no `.zsync`).

### Containerized build

The root `Dockerfile` is a builder image that reproduces the Linux CI toolchain
(Ubuntu 22.04, gcc-12, Qt 6.10.1, rustc 1.96.0) plus the AppImage tooling, so you can
build all three package formats off any host distro. Source is mounted at run time — check
out submodules on the host first, and the Rust/CMake caches persist under the mounted tree
across runs.

```sh
git submodule update --init --recursive
docker build -t telematrix-linux-builder .
docker run --rm -v "$PWD":/src telematrix-linux-builder   # -> ./dist/*.deb, *.rpm, *.AppImage
```

Pass `bash` as the run command to build by hand inside the container. Keep the image's
pins in step with `.github/workflows/build.yml`.

---

## Tests

The test suite (Rust `cargo test` + C++ Qt Test) is built by default
(`-DTELEMATRIX_BUILD_TESTS=ON`; pass `OFF` to skip it in a package build).

```sh
# Rust — plain `cargo test` (the unwinding dev profile; never --release/dev-ffi,
# which set panic=abort and break the libtest harness).
cd rust && cargo test

# C++ — build only the test runners (telematrix_core OBJECT lib, no Rust staticlib
# or full app), then run headless.
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DTELEMATRIX_BUILD_TESTS=ON
cmake --build build --target telematrix_cpp_tests
cd build && QT_QPA_PLATFORM=offscreen ctest --output-on-failure
```

---

## Continuous integration

`main` is the only long-lived branch; releases are cut from tags. GitHub Actions under
`.github/workflows/`:

- **`ci.yml`** — on pull requests: the test suite only (Rust `cargo test` + **gating**
  `fmt --check` / `clippy -D warnings`, and C++ `ctest` offscreen), all on Linux. On a push
  to `main` (or `workflow_dispatch`) it *also* runs the full three-platform package build,
  unsigned, uploading artifacts. PRs deliberately skip the macOS/Windows builds — this is a
  private repo, where those runners bill at 10× and 2× respectively.
- **`release.yml`** — on a `v*` tag. Verifies the tag matches `TELEMATRIX_VERSION`, runs the
  tests, builds and signs all three platforms, then publishes **one** GitHub Release with
  every artifact plus a `SHA256SUMS` file.
- **`_reusable-tests.yml` / `_reusable-build.yml`** — the shared job definitions both of the
  above call, so a tag build takes exactly the path already proven green on `main`.

### Cutting a release

One command — it bumps `TELEMATRIX_VERSION` + `TELEMATRIX_RELEASE_DATE` in `CMakeLists.txt`,
commits, tags, and pushes:

```sh
scripts/release.sh 1.2.0           # -> GitHub Release ("Latest")
scripts/release.sh 1.2.0-beta.1    # -> GitHub Pre-release
scripts/release.sh 1.2.0 --dry-run # show what would change, touch nothing
```

The tag alone drives the channel: any `-` suffix (`-beta.N`, `-rc.N`) publishes a
**pre-release**; a bare `X.Y.Z` publishes a normal release. The script refuses to run on a
dirty tree or off `main`, and confirms before pushing (pushing the tag is what triggers the
signed, notarized build and publishes the release).

Don't hand-edit the version: `TELEMATRIX_VERSION` feeds every package filename, the macOS
bundle, the About page and the AppStream metainfo, so it must match the tag —
`release.yml`'s `version-gate` fails the run if it doesn't.

Prerelease versions are normalized per packaging system: DEB/RPM get `1.2.0~beta.1` (`-` is
illegal in an RPM version and would sort *above* the final release in dpkg), while the macOS
`CFBundleShortVersionString` is stripped to `1.2.0` (Apple requires numeric-only). The
`.dmg`/`.AppImage` filenames keep the full string.

CI pins the same toolchains as local dev (Qt 6.10.1, rustc 1.96.0). Bump the versions in
the workflows in step with the ones you develop against.
