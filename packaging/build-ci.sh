#!/usr/bin/env bash
set -euo pipefail

# Runs as an unprivileged user inside the architecture's Arch Linux container.
expected_arch=${1:?Usage: build-ci.sh ARCH}
[[ "$(uname -m)" == "$expected_arch" ]]
build_dir=$(mktemp -d)
trap 'rm -rf "$build_dir"' EXIT
cp /out/PKGBUILD /out/archivist-*.tar.gz "$build_dir/"
cd "$build_dir"
printf "source /etc/makepkg.conf\nPKGEXT='.pkg.tar.zst'\n" > makepkg.conf
PKGDEST=/out makepkg --config "$PWD/makepkg.conf" --cleanbuild --force --noconfirm
source PKGBUILD
package="/out/$pkgname-$pkgver-$pkgrel-$expected_arch.pkg.tar.zst"
bsdtar -xOf "$package" .PKGINFO | grep -Fx "arch = $expected_arch"
mkdir unpacked
bsdtar -xf "$package" -C unpacked
readelf -h unpacked/usr/bin/archivist
case "$expected_arch" in
  x86_64) readelf -h unpacked/usr/bin/archivist | grep -F 'Advanced Micro Devices X86-64' ;;
  aarch64) readelf -h unpacked/usr/bin/archivist | grep -F 'AArch64' ;;
  *) exit 1 ;;
esac
set +e
QT_QPA_PLATFORM=offscreen timeout 3s unpacked/usr/bin/archivist
smoke_status=$?
set -e
[[ "$smoke_status" == 124 ]] || { echo "Startup smoke test failed: $smoke_status" >&2; exit 1; }
cd /out
sha256sum "$pkgname-$pkgver.tar.gz" PKGBUILD "$pkgname-$pkgver-$pkgrel-$expected_arch.pkg.tar.zst" > "SHA256SUMS-$expected_arch"
