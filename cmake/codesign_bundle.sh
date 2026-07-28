#!/usr/bin/env bash
#
# The single code-signing pass over TeleMatrix.app. Signs every nested Mach-O
# inside-out and the bundle last, retries the transient timestamp-authority
# failures Apple's TSA hands out under load, and fails the build if anything is
# left unsigned.
#
# It replaces an arrangement where macdeployqt (-sign-for-notarization) and a
# CMake POST_BUILD codesign both signed, neither was authoritative, and neither
# checked the other. macdeployqt prints "ERROR: Codesign signing error" and then
# EXITS 0 — after it has already rewritten the binary's load commands with
# install_name_tool. A single rate-limited timestamp call therefore shipped a
# modified-after-signing binary, which notarization rejects with "The signature
# of the binary is invalid." (and blames the main executable, because that is the
# signature sealing _CodeSignature/CodeResources). macdeployqt no longer signs;
# this script owns it.
#
# Inputs arrive as environment variables, set by the CMake POST_BUILD command.

set -euo pipefail

: "${APP_BUNDLE:?path to the built TeleMatrix.app}"
: "${SIGN_IDENTITY:?codesign identity, or '-' for ad-hoc}"
: "${BUNDLE_ID:?bundle signing identifier}"
: "${CODESIGN:=/usr/bin/codesign}"

# Apple's timestamp service rate-limits, and a bundle this size needs one call per
# nested binary — ~40 per build. Three attempts with growing backoff covers it.
MAX_ATTEMPTS="${CODESIGN_MAX_ATTEMPTS:-3}"

sign_args=(--force --sign "$SIGN_IDENTITY")
if [ "$SIGN_IDENTITY" != "-" ]; then
    # Both are mandatory for Developer ID notarization. An ad-hoc signature cannot
    # carry a secure timestamp at all, hence the branch.
    sign_args+=(--options runtime --timestamp)
fi

sign_one() {
    local target="$1" attempt=1 out backoff
    while :; do
        if out="$("$CODESIGN" "${sign_args[@]}" "${@:2}" "$target" 2>&1)"; then
            return 0
        fi
        # Only network/TSA failures are worth retrying. A locked keychain or a
        # missing identity must fail immediately with the real message.
        if [ "$attempt" -ge "$MAX_ATTEMPTS" ] \
           || ! grep -qiE 'timestamp|network|connection|temporarily|try again' <<<"$out"; then
            echo "!! codesign failed on: $target" >&2
            sed 's/^/   /' <<<"$out" >&2
            if [ "$SIGN_IDENTITY" != "-" ]; then
                echo "   Refusing to fall back to an ad-hoc signature: macOS pins a keychain" >&2
                echo "   item's ACL to the designated requirement of the binary that created it." >&2
                echo "   A Developer ID requirement is stable across rebuilds; an ad-hoc one is a" >&2
                echo "   bare cdhash that changes every build, so each rebuild reads the previous" >&2
                echo "   build's secrets as errSecAuthFailed. Unlock your login keychain" >&2
                echo "   and allow codesign to use the signing key, or configure with" >&2
                echo "   -DTELEMATRIX_CODESIGN_IDENTITY=- to build unsigned." >&2
            fi
            return 1
        fi
        backoff=$(( attempt * attempt * 3 ))
        echo "   .. transient codesign failure on $(basename "$target") (attempt ${attempt}/${MAX_ATTEMPTS}), retrying in ${backoff}s"
        sleep "$backoff"
        attempt=$(( attempt + 1 ))
    done
}

main_executable="$APP_BUNDLE/Contents/MacOS/$(/usr/libexec/PlistBuddy \
    -c 'Print :CFBundleExecutable' "$APP_BUNDLE/Contents/Info.plist" 2>/dev/null || echo TeleMatrix)"

echo ">> Signing nested code in $(basename "$APP_BUNDLE") (identity: $SIGN_IDENTITY)"

# 1. Loose Mach-O files: plugins, the dylibs macdeployqt copied into Frameworks/,
#    any helper executable. Anything inside a .framework is covered by step 2,
#    which signs the framework as a bundle; the main executable is covered by
#    step 3, because signing a bundle IS signing its main executable.
count=0
while IFS= read -r f; do
    case "$f" in
        *.framework/*)     continue ;;
        "$main_executable") continue ;;
    esac
    file -b "$f" 2>/dev/null | grep -q 'Mach-O' || continue
    sign_one "$f"
    count=$(( count + 1 ))
done < <(find "$APP_BUNDLE" -type f)

# 2. Frameworks, signed at the versioned directory rather than the bare dylib so
#    codesign seals the framework's own Resources/Info.plist. `Versions/Current`
#    is a symlink and would sign the same thing twice under a second name.
while IFS= read -r fw; do
    for version in "$fw"/Versions/*; do
        [ -d "$version" ] && [ ! -L "$version" ] || continue
        sign_one "$version"
        count=$(( count + 1 ))
    done
done < <(find "$APP_BUNDLE" -type d -name '*.framework')

echo ">> Signed $count nested item(s); sealing the bundle"

# 3. The bundle last: this writes the main executable's signature and the resource
#    seal over everything above, so it must follow every modification.
sign_one "$APP_BUNDLE" --identifier "$BUNDLE_ID"

# 4. Prove it. Without this the failure surfaces ~20 minutes later as a notary
#    rejection that names a path but never a reason.
echo ">> Verifying"
if ! "$CODESIGN" --verify --deep --strict --verbose=2 "$APP_BUNDLE"; then
    echo "!! the freshly signed bundle does not verify — refusing to continue" >&2
    exit 1
fi
echo ">> $(basename "$APP_BUNDLE") signed and verified"
