#!/usr/bin/env bash
#
# Code-signing diagnostics for a macOS app bundle, aimed at the notary service's
# "The signature of the binary is invalid." rejection.
#
# The notary log only names the offending path; it never says *why*. This walks
# every Mach-O in the bundle and reports the four things that actually cause that
# rejection:
#
#   1. the signature blob no longer reaches the end of the file — i.e. the binary
#      was modified after signing (strip / install_name_tool / macdeployqt).
#      Apple's own first answer for this error;
#   2. an ad-hoc or unsigned nested binary (no Authority chain);
#   3. a missing hardened runtime flag (0x10000) — required for Developer ID;
#   4. a missing secure timestamp.
#
# Plus the bundle-level traps: resource forks / AppleDouble `._*` files that break
# the seal, and a signing identifier that disagrees with CFBundleIdentifier.
#
# Usage:  cmake/sigdiag.sh path/to/TeleMatrix.app [label]
# Exit:   0 = everything clean, 1 = at least one FAIL. Read-only; never mutates.
#
# Quiet by default: a passing run prints one line. The full report — the per-file
# table and codesign's own verbose output — is buffered and emitted only when
# something fails, which is the only time anyone wants ~150 lines of
# --prepared/--validated. SIGDIAG_VERBOSE=1 always prints it.

set -uo pipefail

TARGET="${1:?usage: sigdiag.sh <path to .app or binary> [label]}"
LABEL="${2:-$(basename "$TARGET")}"

[ -e "$TARGET" ] || { echo "!! not found: $TARGET" >&2; exit 1; }

REPORT="$(mktemp -t sigdiag)"
trap 'rm -f "$REPORT"' EXIT
exec 3>&1               # the real stdout, kept for the verdict
exec >"$REPORT" 2>&1    # everything below is buffered, codesign's stderr included

FAILURES=0
FINDINGS=()
# Findings are echoed where they happen AND collected, so the summary can repeat
# them at the end — a CI log that gets trimmed still shows the reasons.
fail() { echo "   FAIL: $*"; FINDINGS+=("$*"); FAILURES=$((FAILURES + 1)); }
warn() { echo "   warn: $*"; }

echo "============================================================"
echo "== sigdiag: $LABEL"
echo "==   path: $TARGET"
echo "============================================================"

# --- bundle-level ------------------------------------------------------------
if [ -d "$TARGET" ]; then
    echo
    echo "-- bundle signature --------------------------------------------------"
    codesign -dvvv "$TARGET" 2>&1 || fail "codesign -d could not read the bundle signature"

    echo
    echo "-- codesign --verify --deep --strict ---------------------------------"
    # The single most informative command: it names the first path that fails and
    # the reason, which the notary log never does.
    if codesign --verify --deep --strict --verbose=4 "$TARGET" 2>&1; then
        echo "   OK: bundle verifies deep+strict"
    else
        fail "codesign --verify --deep --strict rejected the bundle (see above)"
    fi

    echo
    echo "-- Gatekeeper assessment (spctl) -------------------------------------"
    # Pre-notarization this legitimately says "rejected ... not notarized"; what
    # matters is that it does NOT say "invalid signature" / "obsolete resource envelope".
    spctl --assess --type exec -vvv "$TARGET" 2>&1 || true

    echo
    echo "-- stapled ticket ----------------------------------------------------"
    xcrun stapler validate "$TARGET" 2>&1 || echo "   (no ticket stapled yet)"

    echo
    echo "-- identifier vs CFBundleIdentifier ----------------------------------"
    plist="$TARGET/Contents/Info.plist"
    if [ -f "$plist" ]; then
        bundle_id="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$plist" 2>/dev/null || echo '?')"
        signed_id="$(codesign -dv "$TARGET" 2>&1 | sed -n 's/^Identifier=//p')"
        echo "   CFBundleIdentifier = $bundle_id"
        echo "   signing Identifier = $signed_id"
        [ "$bundle_id" = "$signed_id" ] || warn "identifier mismatch (allowed, but a smell)"
    fi

    echo
    echo "-- resource-fork / AppleDouble detritus ------------------------------"
    # `._*` files (AppleDouble) and com.apple.FinderInfo xattrs inside a bundle
    # make codesign refuse ("resource fork, Finder information, or similar
    # detritus not allowed") and can invalidate an already-sealed bundle.
    doubles="$(find "$TARGET" -name '._*' -print 2>/dev/null)"
    if [ -n "$doubles" ]; then
        fail "AppleDouble files inside the bundle:"; echo "$doubles" | sed 's/^/        /'
    else
        echo "   OK: no ._* files"
    fi
    xattrs="$(xattr -lr "$TARGET" 2>/dev/null | grep -E 'com\.apple\.(FinderInfo|ResourceFork)' || true)"
    if [ -n "$xattrs" ]; then
        fail "resource-fork/FinderInfo xattrs present:"; echo "$xattrs" | sed 's/^/        /'
    else
        echo "   OK: no resource-fork xattrs"
    fi
fi

# --- per-Mach-O --------------------------------------------------------------
echo
echo "-- every Mach-O in the payload ---------------------------------------"
printf '   %-9s %-8s %-9s %-8s %-9s %s\n' RUNTIME TSTAMP AUTHORITY VERIFY SIG-END PATH

# Does the code-signature blob reach the end of its slice? codesign always writes
# it last, so a gap means the file grew (or was rewritten) after it was signed —
# the classic strip / install_name_tool / macdeployqt-after-signing bug. Must be
# computed PER SLICE: in a universal binary `dataoff` is relative to the slice,
# not the file, so comparing against the file size reports every fat binary short.
# Echoes "eof" when clean, otherwise "SHORT-<bytes>" / "NO-LC".
sig_end_check() {
    local f="$1" archs off size dataoff datasize i=0 arch worst=0

    archs="$(lipo -archs "$f" 2>/dev/null)" || archs=""
    if [ -z "$archs" ] || [ "$(wc -w <<<"$archs")" -le 1 ]; then
        # Thin: the signature has to end at EOF.
        read -r dataoff datasize < <(otool -l "$f" 2>/dev/null | awk '
            /LC_CODE_SIGNATURE/ { c = 1; next }
            c && $1 == "dataoff"  { off = $2; next }
            c && $1 == "datasize" { print off, $2; exit }')
        [ -n "${dataoff:-}" ] || { echo "NO-LC"; return; }
        size="$(stat -f%z "$f")"
        (( dataoff + datasize == size )) && echo "eof" || echo "SHORT-$(( size - dataoff - datasize ))"
        return
    fi

    for arch in $archs; do
        read -r off size < <(otool -f "$f" 2>/dev/null | awk -v want="$i" '
            $1 == "architecture" { cur = $2; next }
            cur == want && $1 == "offset" { o = $2; next }
            cur == want && $1 == "size"   { print o, $2; exit }')
        read -r dataoff datasize < <(otool -arch "$arch" -l "$f" 2>/dev/null | awk '
            /LC_CODE_SIGNATURE/ { c = 1; next }
            c && $1 == "dataoff"  { off = $2; next }
            c && $1 == "datasize" { print off, $2; exit }')
        i=$(( i + 1 ))
        [ -n "${dataoff:-}" ] || { echo "NO-LC"; return; }
        (( size - dataoff - datasize > worst )) && worst=$(( size - dataoff - datasize ))
    done
    (( worst == 0 )) && echo "eof" || echo "SHORT-$worst"
}

# -type f skips the framework Versions/Current symlinks, which would otherwise be
# reported twice under two names.
while IFS= read -r f; do
    file -b "$f" 2>/dev/null | grep -q 'Mach-O' || continue

    info="$(codesign -dvvv "$f" 2>&1)"

    if grep -q 'code object is not signed' <<<"$info"; then
        runtime="UNSIGNED"; tstamp="-"; authority="NONE"
    else
        grep -q 'flags=.*runtime' <<<"$info" && runtime="ok" || runtime="MISSING"
        grep -q '^Timestamp=' <<<"$info" && tstamp="ok" || tstamp="MISSING"
        if grep -q '^Authority=Developer ID Application' <<<"$info"; then
            authority="devid"
        elif grep -q '^Signature=adhoc' <<<"$info"; then
            authority="ADHOC"
        elif grep -q '^Authority=' <<<"$info"; then
            authority="other"
        else
            authority="NONE"
        fi
    fi

    sigstat="$(sig_end_check "$f")"

    # The authoritative per-file check: codesign recomputes the CDHashes, so this
    # is what actually catches a tampered/truncated binary.
    if codesign --verify --strict "$f" >/dev/null 2>&1; then verify="ok"; else verify="BAD"; fi

    rel="${f#"$TARGET"/}"
    printf '   %-9s %-8s %-9s %-8s %-9s %s\n' "$runtime" "$tstamp" "$authority" "$verify" "$sigstat" "$rel"

    case "$runtime"   in MISSING|UNSIGNED) fail "$rel: hardened runtime not enabled";; esac
    case "$tstamp"    in MISSING)          fail "$rel: no secure timestamp";; esac
    case "$authority" in ADHOC|NONE)       fail "$rel: not signed with a Developer ID identity";; esac
    case "$verify"    in BAD)              fail "$rel: codesign --verify --strict rejects this binary";; esac
    case "$sigstat"   in
        SHORT-*) fail "$rel: signature ends ${sigstat#SHORT-} bytes before the end of its slice — MODIFIED AFTER SIGNING";;
        NO-LC)   fail "$rel: no LC_CODE_SIGNATURE load command at all";;
    esac
done < <(find "$TARGET" -type f 2>/dev/null)

exec 1>&3               # back to the real stdout

if [ "$FAILURES" -eq 0 ]; then
    [ -n "${SIGDIAG_VERBOSE:-}" ] && cat "$REPORT"
    echo "== sigdiag: $LABEL — PASS (0 findings)"
    exit 0
fi

# Something failed: the whole report is worth reading, with the findings repeated
# at the end so they survive a truncated log.
cat "$REPORT"
echo
echo "============================================================"
echo "== sigdiag: $LABEL — $FAILURES FINDING(S)"
echo "============================================================"
printf '   FAIL: %s\n' "${FINDINGS[@]}"
exit 1
