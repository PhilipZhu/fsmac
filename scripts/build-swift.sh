#!/bin/bash
# Builds the Swift components (daemon, CLI) into build/bin.
set -euo pipefail
cd "$(dirname "$0")/.."
ARCH=${ARCH:-$(uname -m)}
FLAGS=(-O -target "$ARCH-apple-macos14.0")
mkdir -p build/bin
swiftc "${FLAGS[@]}" -module-name fsmacd shared/Protocol.swift daemon/*.swift -framework IOKit \
    -framework DiskArbitration -framework SystemConfiguration -o build/bin/fsmacd
swiftc "${FLAGS[@]}" -module-name fsmac shared/Protocol.swift shared/Client.swift cli/main.swift \
    -o build/bin/fsmac
