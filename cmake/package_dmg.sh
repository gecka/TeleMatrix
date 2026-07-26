#!/usr/bin/env bash
#
# Builds the stylized TeleMatrix .dmg installer.
#
# Extracted from CMakeLists.txt so the fiddly shell / AppleScript packaging lives
# in one readable place instead of being buried in CMake string escaping. All
# inputs arrive as environment variables set by the `package_dmg` CMake target.
#
# It does three things the inline version didn't:
#   * tags the background's DPI so Finder sizes it to the window (Finder draws it
#     at pixels / DPI * 72 points: the original art was 300 DPI, which Finder duly
#     drew tiny in a corner);
#   * gives the .dmg *file* a custom Finder icon (create-dmg's --volicon only
#     themes the mounted volume, not the file you download);
#   * tolerates create-dmg's occasional non-zero exit on an otherwise-good build.

set -euo pipefail

# --- required inputs --------------------------------------------------------
: "${APP_BUNDLE:?path to built TeleMatrix.app}"
: "${OUTPUT_DMG:?path to write the .dmg}"
: "${STAGING_DIR:?scratch dir (wiped each run)}"
: "${ICNS:?app/volume/file icon (.icns)}"
: "${BACKGROUND:?source background png}"
: "${MACDEPLOYQT:?macdeployqt path}"
: "${CREATEDMG:?create-dmg path}"

# --- optional inputs (with sensible defaults) -------------------------------
: "${VOLNAME:=TeleMatrix Installer}"
: "${WINDOW_W:=600}"
: "${WINDOW_H:=400}"
: "${ICON_SIZE:=100}"
: "${TEXT_SIZE:=12}"
: "${APP_ICON_X:=150}"
: "${APP_ICON_Y:=190}"
: "${APPS_X:=450}"
: "${APPS_Y:=185}"
: "${SIGN_IDENTITY:=-}"    # "-" means ad-hoc / unsigned
: "${NOTARIZE_PROFILE:=}"  # xcrun notarytool keychain profile; empty = don't notarize

# The custom-Finder-icon step needs DeRez/Rez/SetFile, which ship inside the
# Xcode developer dir (not on a bare Command Line Tools PATH). Fail early with a
# readable message instead of a cryptic mid-build error.
for tool in DeRez Rez SetFile; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "!! '$tool' not found. It lives under \$(xcode-select -p)/usr/bin." >&2
        echo "   Install Xcode (not just the Command Line Tools), or run:" >&2
        echo "     sudo xcode-select -s /Applications/Xcode.app/Contents/Developer" >&2
        exit 1
    }
done

SRC_DIR="$STAGING_DIR/source"

echo ">> Cleaning staging: $STAGING_DIR"
rm -rf "$STAGING_DIR" "$OUTPUT_DMG"
mkdir -p "$SRC_DIR"

echo ">> Staging app bundle"
# cp -a preserves the framework symlinks inside the bundle.
cp -a "$APP_BUNDLE" "$SRC_DIR/TeleMatrix.app"

echo ">> Deploying Qt runtime into staged bundle"
deploy_args=(-always-overwrite)
if [ "$SIGN_IDENTITY" != "-" ] && [ -n "$SIGN_IDENTITY" ]; then
    deploy_args+=("-sign-for-notarization=$SIGN_IDENTITY")
fi
"$MACDEPLOYQT" "$SRC_DIR/TeleMatrix.app" "${deploy_args[@]}" \
    || "$MACDEPLOYQT" "$SRC_DIR/TeleMatrix.app" -always-overwrite

echo ">> Tagging background DPI so Finder draws it at window size"
mkdir -p "$STAGING_DIR/.background"
BG_FIXED="$STAGING_DIR/.background/dmg_background.png"
cp -f "$BACKGROUND" "$BG_FIXED"
# Finder draws a .background unscaled, at its *point* size: pixels / DPI * 72. So
# the DPI is what maps art pixels onto window points -- deriving it from the art's
# own width is what lets the @2x source (1200px) land on 600pt of window and stay
# crisp on Retina, instead of overflowing it at 72 DPI.
BG_PIXEL_W=$(sips -g pixelWidth "$BG_FIXED" | awk '/pixelWidth:/ { print $2 }')
[[ "$BG_PIXEL_W" =~ ^[0-9]+$ ]] || { echo "!! cannot read background pixel width" >&2; exit 1; }
BG_DPI=$(( 72 * BG_PIXEL_W / WINDOW_W ))
sips -s dpiWidth "$BG_DPI" -s dpiHeight "$BG_DPI" "$BG_FIXED" >/dev/null
echo "   ${BG_PIXEL_W}px wide -> ${BG_DPI} dpi -> ${WINDOW_W}pt"

echo ">> Building DMG with create-dmg"
# create-dmg can exit non-zero even when it produced a usable image (e.g. it
# could not auto-open or internet-enable), so judge success by the output file.
"$CREATEDMG" \
    --volname "$VOLNAME" \
    --volicon "$ICNS" \
    --background "$BG_FIXED" \
    --window-pos 200 120 \
    --window-size "$WINDOW_W" "$WINDOW_H" \
    --no-internet-enable \
    --text-size "$TEXT_SIZE" \
    --icon-size "$ICON_SIZE" \
    --icon "TeleMatrix.app" "$APP_ICON_X" "$APP_ICON_Y" \
    --hide-extension "TeleMatrix.app" \
    --app-drop-link "$APPS_X" "$APPS_Y" \
    "$OUTPUT_DMG" \
    "$SRC_DIR" \
    || true

if [ ! -f "$OUTPUT_DMG" ]; then
    echo "!! create-dmg did not produce $OUTPUT_DMG" >&2
    exit 1
fi

# Sign / notarize / staple BEFORE the custom icon is applied: codesign refuses a
# file that carries a resource fork ("...detritus not allowed"), and the icon is
# stored as a resource-fork xattr. Signing + stapling touch only the data fork, so
# adding the icon xattr afterwards leaves the signature and ticket intact. Only a
# real Developer ID is worth applying to a .dmg (ad-hoc "-" is pointless here).
if [ "$SIGN_IDENTITY" != "-" ] && [ -n "$SIGN_IDENTITY" ]; then
    echo ">> Signing the .dmg file with: $SIGN_IDENTITY"
    codesign --force --timestamp --sign "$SIGN_IDENTITY" "$OUTPUT_DMG"
    codesign --verify --verbose=2 "$OUTPUT_DMG"

    if [ -n "$NOTARIZE_PROFILE" ]; then
        echo ">> Notarizing (notarytool profile: $NOTARIZE_PROFILE) — this can take a few minutes"
        xcrun notarytool submit "$OUTPUT_DMG" --keychain-profile "$NOTARIZE_PROFILE" --wait
        echo ">> Stapling notarization ticket"
        xcrun stapler staple "$OUTPUT_DMG"
        xcrun stapler validate "$OUTPUT_DMG"
    else
        echo ">> Skipping notarization (NOTARIZE_PROFILE not set)"
    fi
else
    echo ">> Skipping .dmg signing (ad-hoc / unsigned build)"
fi

echo ">> Applying custom Finder icon to the .dmg file"
# --volicon themes only the mounted volume; the downloaded .dmg keeps the generic
# disk-image icon. Embed an 'icns' resource into the file's resource fork and set
# the Finder "has custom icon" attribute so the file shows our logo.
TMP_ICON="$STAGING_DIR/dmg_file_icon.icns"
TMP_RSRC="$STAGING_DIR/dmg_file_icon.rsrc"
cp -f "$ICNS" "$TMP_ICON"
sips -i "$TMP_ICON" >/dev/null                 # generate icon family in the file's resource fork
DeRez -only icns "$TMP_ICON" > "$TMP_RSRC"
Rez -append "$TMP_RSRC" -o "$OUTPUT_DMG"
SetFile -a C "$OUTPUT_DMG"                      # kHasCustomIcon

echo ">> Done: $OUTPUT_DMG"
