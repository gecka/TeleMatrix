#!/usr/bin/env bash
#
# Build a Linux AppImage for TeleMatrix with linuxdeploy + linuxdeploy-plugin-qt +
# appimagetool. Driven by the CMake `package_appimage` target (inputs via env), mirroring
# the macOS `package_dmg.sh` pattern. See docs/appimage-build-target-plan.md.
#
# The AppDir is assembled independently of the CPack /opt install rules: linuxdeploy-plugin-qt
# is the sole Qt bundler (no double-deploy), so one configure yields deb + rpm + AppImage.
# X11/xcb only — the AppImage runs on Wayland desktops via XWayland (no wayland plugins).
#
# Required env: BINARY VERSION DESKTOP_FILE ICON_FILE APPIMAGE_OUTPUT APPDIR
# Optional env: METAINFO_FILE QMAKE
#               LINUXDEPLOY LINUXDEPLOY_PLUGIN_QT APPIMAGETOOL  (tool paths; downloaded if unset)
#               APPIMAGE_TOOLS_DIR  (download cache; default ~/.cache/telematrix-appimage-tools)
set -euo pipefail

for v in BINARY VERSION DESKTOP_FILE ICON_FILE APPIMAGE_OUTPUT APPDIR; do
    [ -n "${!v:-}" ] || { echo "package_appimage: \$$v not set" >&2; exit 2; }
done
[ -f "$BINARY" ]      || { echo "package_appimage: binary not found: $BINARY" >&2; exit 2; }
[ -f "$DESKTOP_FILE" ] || { echo "package_appimage: desktop file not found: $DESKTOP_FILE" >&2; exit 2; }
[ -f "$ICON_FILE" ]   || { echo "package_appimage: icon not found: $ICON_FILE" >&2; exit 2; }

ARCH="$(uname -m)"                    # x86_64
export ARCH
export APPIMAGE_EXTRACT_AND_RUN=1     # containers / CI runners lack FUSE — self-extract instead

# --- Acquire the three AppImage tools (use pre-provided paths, else download + cache) -------
TOOLS_DIR="${APPIMAGE_TOOLS_DIR:-$HOME/.cache/telematrix-appimage-tools}"
mkdir -p "$TOOLS_DIR"

# Tool versions are PINNED to tagged releases, not the rolling `continuous` tag: these
# binaries are downloaded and executed in the release path, so a moving target is a
# supply-chain hole. Downloads are checksum-verified (x86_64 hashes below — the only arch
# CI builds); bump tag + hash together.
LINUXDEPLOY_TAG="1-alpha-20251107-1"
LINUXDEPLOY_PLUGIN_QT_TAG="1-alpha-20250213-1"
APPIMAGETOOL_TAG="1.9.1"

sha256_of() {  # <file> -> hex digest (sha256sum on Linux, shasum on macOS)
    if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1
    else shasum -a 256 "$1" | cut -d' ' -f1; fi
}

expected_sha256() {  # <filename> -> hex digest, or "" when unknown for this arch
    case "$1" in
        linuxdeploy-x86_64.AppImage)           echo "c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d" ;;
        linuxdeploy-plugin-qt-x86_64.AppImage) echo "15106be885c1c48a021198e7e1e9a48ce9d02a86dd0a1848f00bdbf3c1c92724" ;;
        appimagetool-x86_64.AppImage)          echo "ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0" ;;
        *) echo "" ;;
    esac
}

fetch_tool() {  # <env-var-name> <filename> <url> -> echoes an executable path
    local preset="${!1:-}"
    if [ -n "$preset" ] && [ -x "$preset" ]; then echo "$preset"; return; fi
    local dest="$TOOLS_DIR/$2"
    local want; want="$(expected_sha256 "$2")"

    # A cached file with the wrong digest is stale (tag bumped) — refetch it.
    if [ -x "$dest" ] && [ -n "$want" ] && [ "$(sha256_of "$dest")" != "$want" ]; then
        rm -f "$dest"
    fi
    if [ ! -x "$dest" ]; then
        echo "package_appimage: downloading $2" >&2
        curl -fL --retry 3 -o "$dest" "$3"
        chmod +x "$dest"
    fi

    if [ -n "$want" ]; then
        local got; got="$(sha256_of "$dest")"
        if [ "$got" != "$want" ]; then
            echo "package_appimage: checksum mismatch for $2" >&2
            echo "  expected $want" >&2
            echo "  got      $got" >&2
            exit 1
        fi
    else
        echo "package_appimage: WARNING: no pinned checksum for $2 (arch $ARCH) — not verified" >&2
    fi
    echo "$dest"
}

LINUXDEPLOY="$(fetch_tool LINUXDEPLOY "linuxdeploy-$ARCH.AppImage" \
    "https://github.com/linuxdeploy/linuxdeploy/releases/download/$LINUXDEPLOY_TAG/linuxdeploy-$ARCH.AppImage")"
# Keep the qt plugin co-located with linuxdeploy so `--plugin qt` discovery finds it.
LINUXDEPLOY_PLUGIN_QT="$(fetch_tool LINUXDEPLOY_PLUGIN_QT "linuxdeploy-plugin-qt-$ARCH.AppImage" \
    "https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/$LINUXDEPLOY_PLUGIN_QT_TAG/linuxdeploy-plugin-qt-$ARCH.AppImage")"
APPIMAGETOOL="$(fetch_tool APPIMAGETOOL "appimagetool-$ARCH.AppImage" \
    "https://github.com/AppImage/appimagetool/releases/download/$APPIMAGETOOL_TAG/appimagetool-$ARCH.AppImage")"
export APPIMAGETOOL

# linuxdeploy discovers plugins + appimagetool on PATH or beside itself.
_ld_dir="$(dirname "$LINUXDEPLOY")"
_qt_dir="$(dirname "$LINUXDEPLOY_PLUGIN_QT")"
export PATH="$_ld_dir:$_qt_dir:$PATH"

# qmake for linuxdeploy-plugin-qt (fall back to PATH discovery if the passed path is bad).
if [ -n "${QMAKE:-}" ] && [ -x "$QMAKE" ]; then export QMAKE; else unset QMAKE; fi

# --- Assemble the AppDir ---------------------------------------------------------------------
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/share/metainfo"

# The repo .desktop targets the /opt install (Exec=/opt/TeleMatrix/bin/TeleMatrix). In an
# AppImage the binary is on PATH inside the bundle, so rewrite Exec to the bare name —
# linuxdeploy derives/validates the executable from it.
DESKTOP_TMP="$(dirname "$APPIMAGE_OUTPUT")/dev.telematrix.TeleMatrix.appimage.desktop"
sed -E 's#^Exec=.*#Exec=TeleMatrix %u#' "$DESKTOP_FILE" > "$DESKTOP_TMP"

# AppStream metainfo — linuxdeploy has no flag for it, so place it by hand.
if [ -n "${METAINFO_FILE:-}" ] && [ -f "$METAINFO_FILE" ]; then
    cp "$METAINFO_FILE" "$APPDIR/usr/share/metainfo/dev.telematrix.TeleMatrix.metainfo.xml"
fi

# Force-bundle libav* so Qt's FFmpeg multimedia backend (inline video streaming) keeps its
# runtime deps — Qt's deploy tooling does NOT handle libav on Linux/X11, and the plugin's
# libs may not be auto-traced. Verified below.
LIB_ARGS=()
for lib in libavcodec libavformat libavutil libswscale libswresample; do
    sopath="$(ldconfig -p 2>/dev/null | awk -v l="$lib" '$1 ~ "^"l"\\.so" {print $NF; exit}')"
    [ -n "$sopath" ] && LIB_ARGS+=(--library "$sopath")
done

# --- Deploy Qt (xcb only) + pack -------------------------------------------------------------
export LINUXDEPLOY_OUTPUT_VERSION="$VERSION"
OUTDIR="$(dirname "$APPIMAGE_OUTPUT")/.appimage-build"
rm -rf "$OUTDIR"; mkdir -p "$OUTDIR"
(
    cd "$OUTDIR"
    # linuxdeploy-plugin-appimage treats $OUTPUT (deprecated alias of $LDAI_OUTPUT) as the
    # AppImage path to write. Anything inherited under those names would divert the build
    # out of $OUTDIR and defeat the glob below, so clear both and rely on cwd.
    unset OUTPUT LDAI_OUTPUT
    "$LINUXDEPLOY" \
        --appdir "$APPDIR" \
        --executable "$BINARY" \
        --desktop-file "$DESKTOP_TMP" \
        --icon-file "$ICON_FILE" \
        "${LIB_ARGS[@]}" \
        --plugin qt \
        --output appimage
)

shopt -s nullglob
produced=("$OUTDIR"/*.AppImage)
shopt -u nullglob
[ "${#produced[@]}" -ge 1 ] || { echo "package_appimage: no .AppImage produced" >&2; exit 1; }
mv -f "${produced[0]}" "$APPIMAGE_OUTPUT"

# --- Verify the FFmpeg multimedia backend is bundled (the acid test for video streaming) -----
plugin="$APPDIR/usr/plugins/multimedia/libffmpegmediaplugin.so"
if [ ! -f "$plugin" ]; then
    echo "ERROR: libffmpegmediaplugin.so not bundled — video streaming would be unavailable." >&2
    echo "Bundled multimedia plugins:" >&2
    ls -1 "$APPDIR/usr/plugins/multimedia/" 2>/dev/null || echo "  (none)" >&2
    exit 1
fi
missing=0
while read -r need; do
    case "$need" in
        libav*|libsw*)
            [ -e "$APPDIR/usr/lib/$need" ] || { echo "ERROR: FFmpeg backend dep not bundled: $need" >&2; missing=1; } ;;
    esac
done < <(patchelf --print-needed "$plugin" 2>/dev/null || true)
[ "$missing" -eq 0 ] || exit 1

# --- Verify the WebP image plugin is bundled (the acid test for emoji) ---------------------
# The emoji sprite atlases are WebP. Without this plugin QImage(path, "WEBP") returns null
# and every emoji falls back to text — which on a Linux host with no colour emoji font
# renders nothing at all. That is the exact bug the sprite port exists to fix, so a build
# that would ship it is a failed build, not a warning.
webp_plugin="$APPDIR/usr/plugins/imageformats/libqwebp.so"
if [ ! -f "$webp_plugin" ]; then
    echo "ERROR: libqwebp.so not bundled — emoji would render blank." >&2
    echo "Bundled image format plugins:" >&2
    ls -1 "$APPDIR/usr/plugins/imageformats/" 2>/dev/null || echo "  (none)" >&2
    echo "Install the qtimageformats module for the Qt used to build this." >&2
    exit 1
fi

# X11/xcb sanity: the platform plugin must be present (confirms the QPA backend is bundled).
[ -f "$APPDIR/usr/plugins/platforms/libqxcb.so" ] \
    || echo "WARNING: libqxcb.so not bundled — the app may fail to find a platform plugin." >&2

echo "OK: AppImage built -> $APPIMAGE_OUTPUT"
ls -lh "$APPIMAGE_OUTPUT"
