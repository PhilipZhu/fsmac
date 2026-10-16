#!/bin/bash
# Packages build/FSMac.app into build/dist/FSMac-<version>.dmg.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
APP=build/FSMac.app
[ -d "$APP" ] || scripts/build-app.sh
VERSION=$(/usr/libexec/PlistBuddy -c "Print CFBundleShortVersionString" "$APP/Contents/Info.plist")
DMG="build/dist/FSMac-$VERSION.dmg"
STAGE=build/dist/stage

rm -rf "$STAGE" "$DMG"
mkdir -p "$STAGE"
ditto "$APP" "$STAGE/FSMac.app"
ln -s /Applications "$STAGE/Applications"
cp README.md "$STAGE/Read Me.md"
cp "docs/FSMac Manual.html" "$STAGE/FSMac Manual.html"
cp LICENSE "$STAGE/License.txt"

hdiutil create -quiet -volname "FSMac $VERSION" -srcfolder "$STAGE" -fs HFS+ \
    -format UDZO -imagekey zlib-level=9 -ov "$DMG"
rm -rf "$STAGE"

IDENTITY=$(codesign -dvv "$APP" 2>&1 | sed -n 's/^Authority=\(.*\)/\1/p' | head -1)
if [ -n "$IDENTITY" ]; then
    codesign --force --sign "$IDENTITY" "$DMG"
fi
echo "Created $DMG ($(du -h "$DMG" | cut -f1))"
