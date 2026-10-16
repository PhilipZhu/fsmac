#!/bin/bash
# Builds and signs build/FSMac.app.
#
#   scripts/build-app.sh
#
# Environment:
#   SIGN_IDENTITY   codesigning identity (default: the first "Developer ID
#                   Application" or "Apple Development" identity, else ad hoc)
#   ARCH            architecture to build (default: this Mac's)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
ARCH=${ARCH:-$(uname -m)}
VERSION=$(sed -n 's/.*static let version = "\(.*\)".*/\1/p' shared/Protocol.swift)
BUILD_NUMBER=${BUILD_NUMBER:-$(date +%y%m%d%H%M)}
APP="$ROOT/build/FSMac.app"
SWIFT_FLAGS=(-O -swift-version 5 -target "$ARCH-apple-macos14.0")

step() { printf '\033[1m==> %s\033[0m\n' "$*"; }

step "Building the engine and tools ($ARCH)"
make -s ARCH="$ARCH" engine

step "Building the Swift components"
mkdir -p build/bin build/app
swiftc "${SWIFT_FLAGS[@]}" -module-name fsmacd shared/Protocol.swift daemon/*.swift \
    -framework DiskArbitration -framework SystemConfiguration -o build/bin/fsmacd
swiftc "${SWIFT_FLAGS[@]}" -module-name fsmac shared/Protocol.swift shared/Client.swift cli/main.swift \
    -o build/bin/fsmac
swiftc "${SWIFT_FLAGS[@]}" -parse-as-library -module-name FSMac shared/Protocol.swift shared/Client.swift \
    app/*.swift -framework SwiftUI -framework ServiceManagement -framework UserNotifications \
    -o build/app/FSMac

if [ ! -f build/AppIcon.icns ] || [ scripts/make-icon.swift -nt build/AppIcon.icns ]; then
    step "Rendering the app icon"
    rm -rf build/AppIcon.iconset
    swift scripts/make-icon.swift build/AppIcon.iconset
    iconutil -c icns build/AppIcon.iconset -o build/AppIcon.icns
fi

step "Assembling FSMac.app $VERSION ($BUILD_NUMBER)"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Helpers" "$APP/Contents/Resources" \
         "$APP/Contents/Library/LaunchDaemons"
cp build/app/FSMac "$APP/Contents/MacOS/FSMac"
for b in fsmacd fsmac fsmac-worker fsmac-probe mkntfs ntfsfix ntfslabel mke2fs e2fsck tune2fs; do
    cp "build/bin/$b" "$APP/Contents/Helpers/$b"
    strip -S "$APP/Contents/Helpers/$b" 2>/dev/null || true
done
cp build/AppIcon.icns "$APP/Contents/Resources/AppIcon.icns"
cp LICENSE "$APP/Contents/Resources/LICENSE.txt"
cp docs/THIRD-PARTY.md "$APP/Contents/Resources/THIRD-PARTY.md"

cat > "$APP/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleDevelopmentRegion</key><string>en</string>
    <key>CFBundleDisplayName</key><string>FSMac</string>
    <key>CFBundleExecutable</key><string>FSMac</string>
    <key>CFBundleIconFile</key><string>AppIcon</string>
    <key>CFBundleIdentifier</key><string>com.fsmac.FSMac</string>
    <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
    <key>CFBundleName</key><string>FSMac</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>$VERSION</string>
    <key>CFBundleVersion</key><string>$BUILD_NUMBER</string>
    <key>LSApplicationCategoryType</key><string>public.app-category.utilities</string>
    <key>LSMinimumSystemVersion</key><string>14.0</string>
    <key>LSUIElement</key><true/>
    <key>NSHumanReadableCopyright</key><string>FSMac is free software (GNU GPL v2). Includes ntfs-3g and e2fsprogs.</string>
    <key>NSSupportsAutomaticTermination</key><false/>
</dict>
</plist>
EOF

# The helper daemon, registered with SMAppService from the app.
cat > "$APP/Contents/Library/LaunchDaemons/com.fsmac.daemon.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key><string>com.fsmac.daemon</string>
    <key>BundleProgram</key><string>Contents/Helpers/fsmacd</string>
    <key>RunAtLoad</key><true/>
    <key>KeepAlive</key><true/>
    <key>ProcessType</key><string>Interactive</string>
    <key>ExitTimeOut</key><integer>120</integer>
    <key>AssociatedBundleIdentifiers</key>
    <array><string>com.fsmac.FSMac</string></array>
</dict>
</plist>
EOF
# Filesystem bundle the helper installs in /Library/Filesystems so that
# macOS recognises ext volumes (and leaves them to FSMac) instead of
# reporting them as unreadable.
FSB="$APP/Contents/Library/Filesystems/fsmac_ext.fs"
mkdir -p "$FSB/Contents/Resources"
cp build/bin/fsmac_ext.util "$FSB/Contents/Resources/fsmac_ext.util"
cp build/bin/fsmac_ext.util "$FSB/Contents/Resources/mount_fsmac_ext"
probe_entry() { # key content-hint-or-empty order
    local props
    if [ -n "$2" ]; then
        props="<key>Content Hint</key><string>$2</string><key>Leaf</key><true/>"
    else
        props="<key>Leaf</key><true/><key>Whole</key><true/>"
    fi
    printf '        <key>%s</key>\n        <dict>\n' "$1"
    printf '            <key>FSMediaProperties</key><dict>%s</dict>\n' "$props"
    printf '            <key>FSProbeArguments</key><string>-p</string>\n'
    printf '            <key>FSProbeExecutable</key><string>fsmac_ext.util</string>\n'
    printf '            <key>FSProbeOrder</key><integer>%s</integer>\n        </dict>\n' "$3"
}
{
    cat <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleIdentifier</key><string>com.fsmac.filesystems.ext</string>
    <key>CFBundleName</key><string>fsmac_ext</string>
    <key>CFBundlePackageType</key><string>fs  </string>
    <key>CFBundleShortVersionString</key><string>$VERSION</string>
    <key>CFBundleVersion</key><string>$BUILD_NUMBER</string>
    <key>FSMediaTypes</key>
    <dict>
PLIST
    for g in 0FC63DAF-8483-4772-8E79-3D69D8477DE4 933AC7E1-2EB4-4F13-B844-0E14E2AEF915 \
             3B8F8425-20E0-4F3B-907F-1A25A76F98E8 4F68BCE3-E8CD-4DB1-96E7-FBCAF984B709 \
             B921B045-1DF0-41C3-AF44-4C6F280D3FAE Linux Linux_Ext2FS; do
        probe_entry "$g" "$g" 1500
    done
    probe_entry EBD0A0A2-B9E5-4433-87C0-68B6B72699C7 EBD0A0A2-B9E5-4433-87C0-68B6B72699C7 3000
    probe_entry Partitionless "" 5000
    cat <<PLIST
    </dict>
    <key>FSPersonalities</key>
    <dict>
        <key>fsmac_ext</key>
        <dict>
            <key>FSMountExecutable</key><string>mount_fsmac_ext</string>
            <key>FSName</key><string>Linux ext2/ext3/ext4 (FSMac)</string>
        </dict>
    </dict>
</dict>
</plist>
PLIST
} > "$FSB/Contents/Info.plist"

plutil -lint -s "$APP/Contents/Info.plist" "$APP/Contents/Library/LaunchDaemons/com.fsmac.daemon.plist" \
    "$FSB/Contents/Info.plist"

step "Signing"
IDENTITY=${SIGN_IDENTITY:-$(security find-identity -v -p codesigning 2>/dev/null |
    sed -n 's/.*"\(Developer ID Application:.*\)"/\1/p' | head -1)}
IDENTITY=${IDENTITY:-$(security find-identity -v -p codesigning 2>/dev/null |
    sed -n 's/.*"\(Apple Development:.*\)"/\1/p' | head -1)}
IDENTITY=${IDENTITY:--}
SIGN=(codesign --force --options runtime --sign "$IDENTITY")
case "$IDENTITY" in
    "Developer ID"*) SIGN+=(--timestamp) ;;
    *) SIGN+=(--timestamp=none) ;;
esac
for b in "$APP"/Contents/Helpers/* "$FSB"/Contents/Resources/*; do
    "${SIGN[@]}" --identifier "com.fsmac.$(basename "$b")" "$b"
done
"${SIGN[@]}" "$FSB"
"${SIGN[@]}" "$APP"
codesign --verify --strict --deep "$APP"
echo "Signed with: $IDENTITY"

step "Done: $APP"
