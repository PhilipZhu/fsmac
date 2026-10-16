#!/bin/bash
# Builds the upstream filesystem libraries and tools FSMac depends on:
#   - ntfs-3g   (libntfs-3g + mkntfs/ntfsfix/ntfslabel/...)
#   - e2fsprogs (libext2fs + e2fsck/mke2fs/tune2fs/debugfs objects)
#
# Both are built out-of-tree into build/vendor/<name> with their own
# autotools build systems, as static libraries.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${ROOT}/build/vendor"
JOBS="$(sysctl -n hw.ncpu)"
ARCH="${ARCH:-$(uname -m)}"
MINOS="${MACOSX_DEPLOYMENT_TARGET:-14.0}"
CFLAGS_COMMON="-O2 -g -arch ${ARCH} -mmacosx-version-min=${MINOS} -Wno-unused-but-set-variable -Wno-deprecated-non-prototype"

export ACLOCAL_PATH="/opt/homebrew/share/aclocal:${ACLOCAL_PATH:-}"

log() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }

build_ntfs3g() {
	local src="${ROOT}/vendor/ntfs-3g" dst="${OUT}/ntfs-3g"
	# libgcrypt's m4 macro is referenced by configure.ac even when crypto
	# support is disabled; provide a stub so autoreconf succeeds.
	if [ ! -f "${src}/m4/fsmac-stubs.m4" ]; then
		cat > "${src}/m4/fsmac-stubs.m4" <<'EOF'
dnl FSMac: stub for libgcrypt's m4 macro so autoreconf works without libgcrypt.
dnl Crypto support (ntfsdecrypt) is not built.
AC_DEFUN([AM_PATH_LIBGCRYPT], [$3])
EOF
	fi
	if [ ! -x "${src}/configure" ]; then
		log "ntfs-3g: autogen"
		(cd "${src}" && ./autogen.sh >/dev/null)
	fi
	mkdir -p "${dst}"
	if [ ! -f "${dst}/config.status" ]; then
		log "ntfs-3g: configure"
		(cd "${dst}" && CFLAGS="${CFLAGS_COMMON}" "${src}/configure" \
			--disable-ntfs-3g --disable-plugins --disable-nfconv \
			--disable-shared --enable-static --enable-extras \
			--disable-mtab --disable-ldconfig --prefix=/usr/local >/dev/null)
	fi
	log "ntfs-3g: make"
	make -C "${dst}" -j"${JOBS}" >/dev/null
}

build_e2fsprogs() {
	local src="${ROOT}/vendor/e2fsprogs" dst="${OUT}/e2fsprogs"
	mkdir -p "${dst}"
	if [ ! -f "${dst}/config.status" ]; then
		log "e2fsprogs: configure"
		(cd "${dst}" && CFLAGS="${CFLAGS_COMMON}" "${src}/configure" \
			--disable-nls --disable-fuse2fs --disable-uuidd \
			--disable-e2initrd-helper --disable-defrag --without-libarchive \
			--disable-elf-shlibs --disable-bsd-shlibs --prefix=/usr/local >/dev/null)
	fi
	log "e2fsprogs: make libs"
	make -C "${dst}" -j"${JOBS}" libs >/dev/null
	log "e2fsprogs: make tools"
	make -C "${dst}/e2fsck" -j"${JOBS}" e2fsck >/dev/null
	make -C "${dst}/misc" -j"${JOBS}" mke2fs tune2fs dumpe2fs >/dev/null
	make -C "${dst}/debugfs" -j"${JOBS}" debugfs >/dev/null
}

case "${1:-all}" in
	ntfs-3g) build_ntfs3g ;;
	e2fsprogs) build_e2fsprogs ;;
	all) build_ntfs3g; build_e2fsprogs ;;
	*) echo "usage: $0 [all|ntfs-3g|e2fsprogs]" >&2; exit 2 ;;
esac
log "vendor build complete"
