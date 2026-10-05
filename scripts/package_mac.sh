#!/usr/bin/env bash
# Builds a universal Release of Multiway from the Projucer-generated Xcode project,
# signs it with Developer ID, packages VST3 + AU + Standalone into one installer,
# then notarizes and staples it.
#
# Requirements:
#   - Projucer at $PROJUCER (default ~/JUCE/Projucer.app)
#   - "Developer ID Application" and "Developer ID Installer" identities in the keychain
#   - notarytool credentials stored once with:
#       xcrun notarytool store-credentials <profile> --apple-id ... --team-id ...
#     (NOTARY_PROFILE defaults to the profile shared with Clipotype)
#
# The build folder must NOT be inside iCloud Drive (Desktop/Documents sync):
# iCloud adds extended attributes that make codesign fail.

set -euo pipefail

NOTARY_PROFILE="${NOTARY_PROFILE:-clipotype-notary}"
BUNDLE_PREFIX="com.bopsaudio.multiway"
PROJUCER="${PROJUCER:-$HOME/JUCE/Projucer.app/Contents/MacOS/Projucer}"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PLUGIN="$ROOT/plugin"
XCODE_PROJECT="$PLUGIN/Builds/MacOSX/Multiway.xcodeproj"
ARTEFACTS="$PLUGIN/Builds/MacOSX/build/Release"
STAGE="$PLUGIN/Builds/MacOSX/build/pkg"

step() { printf '\n==> %s\n' "$*"; }
die()  { printf '\nERROR: %s\n' "$*" >&2; exit 1; }

# Prints the SHA-1 of the first valid identity matching $1. Signing by hash avoids
# "ambiguous identity" errors when several certificates share the same name.
find_identity() {
    security find-identity -v $2 | grep "\"$1: " | head -1 | awk '{print $2}' || true
}

step "Checking signing identities and notary profile"
APP_ID="$(find_identity "Developer ID Application" "-p codesigning")"
INST_ID="$(find_identity "Developer ID Installer" "")"
[[ -n "$APP_ID" ]]  || die "No valid 'Developer ID Application' identity in the keychain."
[[ -n "$INST_ID" ]] || die "No valid 'Developer ID Installer' identity in the keychain."
xcrun notarytool history --keychain-profile "$NOTARY_PROFILE" >/dev/null \
    || die "Keychain profile '$NOTARY_PROFILE' not found. Create it with: xcrun notarytool store-credentials $NOTARY_PROFILE"
echo "Application: $APP_ID"
echo "Installer:   $INST_ID"

step "Regenerating the Xcode project and building a universal Release"
"$PROJUCER" --resave "$PLUGIN/Multiway.jucer"
xcodebuild -project "$XCODE_PROJECT" -scheme "Multiway - All" -configuration Release \
    ARCHS="arm64 x86_64" ONLY_ACTIVE_ARCH=NO -quiet build

VST3="$ARTEFACTS/Multiway.vst3"
AU="$ARTEFACTS/Multiway.component"
APP="$ARTEFACTS/Multiway.app"
VERSION="$(/usr/libexec/PlistBuddy -c 'Print CFBundleShortVersionString' "$AU/Contents/Info.plist")"
PKG_NAME="Multiway-v${VERSION}-macOS.pkg"

step "Signing bundles"
rm -rf "$STAGE"
mkdir -p "$STAGE"

# The standalone app needs microphone access under the hardened runtime;
# plugins run inside the host's process and use the host's entitlements.
cat > "$STAGE/Standalone.entitlements" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>com.apple.security.device.audio-input</key>
    <true/>
</dict>
</plist>
EOF

sign() {  # <bundle> [entitlements]
    xattr -cr "$1"
    codesign --force --deep --options runtime --timestamp \
        ${2:+--entitlements "$2"} \
        --sign "$APP_ID" "$1"
    codesign --verify --deep --strict --verbose=2 "$1"
}
sign "$VST3"
sign "$AU"
sign "$APP" "$STAGE/Standalone.entitlements"

step "Building component packages"
component_pkg() {  # <bundle> <name> <install location>
    local root="$STAGE/root-$2"
    mkdir -p "$root"
    ditto "$1" "$root/$(basename "$1")"
    # Stop the installer from "relocating" bundles to other copies it finds on disk.
    pkgbuild --analyze --root "$root" "$STAGE/$2.plist"
    plutil -replace 0.BundleIsRelocatable -bool NO "$STAGE/$2.plist"
    pkgbuild --root "$root" \
        --component-plist "$STAGE/$2.plist" \
        --identifier "$BUNDLE_PREFIX.$2" \
        --version "$VERSION" \
        --install-location "$3" \
        "$STAGE/Multiway-$2.pkg"
}
component_pkg "$VST3" vst3 /Library/Audio/Plug-Ins/VST3
component_pkg "$AU"   au   /Library/Audio/Plug-Ins/Components
component_pkg "$APP"  app  /Applications

step "Building signed product archive"
cat > "$STAGE/distribution.xml" <<EOF
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
    <title>Multiway $VERSION</title>
    <options customize="allow" require-scripts="false" hostArchitectures="arm64,x86_64"/>
    <domains enable_localSystem="true"/>
    <volume-check>
        <allowed-os-versions><os-version min="10.13"/></allowed-os-versions>
    </volume-check>
    <choices-outline>
        <line choice="vst3"/>
        <line choice="au"/>
        <line choice="app"/>
    </choices-outline>
    <choice id="vst3" title="VST3 Plug-in" description="Installs to /Library/Audio/Plug-Ins/VST3">
        <pkg-ref id="$BUNDLE_PREFIX.vst3"/>
    </choice>
    <choice id="au" title="Audio Unit" description="Installs to /Library/Audio/Plug-Ins/Components">
        <pkg-ref id="$BUNDLE_PREFIX.au"/>
    </choice>
    <choice id="app" title="Standalone App" description="Installs to /Applications">
        <pkg-ref id="$BUNDLE_PREFIX.app"/>
    </choice>
    <pkg-ref id="$BUNDLE_PREFIX.vst3" version="$VERSION">Multiway-vst3.pkg</pkg-ref>
    <pkg-ref id="$BUNDLE_PREFIX.au" version="$VERSION">Multiway-au.pkg</pkg-ref>
    <pkg-ref id="$BUNDLE_PREFIX.app" version="$VERSION">Multiway-app.pkg</pkg-ref>
</installer-gui-script>
EOF

PKG="$ARTEFACTS/$PKG_NAME"
productbuild --distribution "$STAGE/distribution.xml" \
    --package-path "$STAGE" \
    --sign "$INST_ID" \
    --timestamp \
    "$PKG"

step "Notarizing (this can take a few minutes)"
set +e
SUBMIT_OUT="$(xcrun notarytool submit "$PKG" --keychain-profile "$NOTARY_PROFILE" --wait 2>&1)"
set -e
echo "$SUBMIT_OUT"
SUBMISSION_ID="$(echo "$SUBMIT_OUT" | awk '/^  id: / {print $2; exit}')"
STATUS="$(echo "$SUBMIT_OUT" | awk '/^  status: / {print $2}' | tail -1)"

if [[ "$STATUS" != "Accepted" ]]; then
    if [[ -n "$SUBMISSION_ID" ]]; then
        step "Notarization log"
        xcrun notarytool log "$SUBMISSION_ID" --keychain-profile "$NOTARY_PROFILE"
    fi
    die "Notarization failed (status: ${STATUS:-unknown})."
fi

step "Stapling"
xcrun stapler staple "$PKG"
xcrun stapler validate "$PKG"

step "Gatekeeper assessment"
spctl --assess --type install -v "$PKG"

step "Done: $PKG"
