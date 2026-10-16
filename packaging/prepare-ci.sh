#!/usr/bin/env bash
set -euo pipefail

# Create the same source archive and pinned recipe for both native architectures.
cd "$(dirname "$0")/.."
output_dir=$(realpath -m "${1:?Usage: prepare-ci.sh OUTPUT_DIRECTORY}")
mkdir -p "$output_dir"
source packaging/PKGBUILD
version=$(sed -n 's/^project(Archivist VERSION \([^ ]*\) LANGUAGES CXX)/\1/p' CMakeLists.txt)
if [[ "$version" != "$pkgver" ]]; then
  echo "CMake and PKGBUILD versions differ" >&2
  exit 1
fi
if [[ "${GITHUB_REF_TYPE:-}" == tag ]]; then
  [[ "${GITHUB_REF_NAME:?}" == "v$pkgver" ]] || {
    echo "Release tag must match CMake and PKGBUILD version v$pkgver" >&2
    exit 1
  }
else
  pkgver="$pkgver.r$(git rev-list --count HEAD).g$(git rev-parse --short=8 HEAD)"
fi
archive="$pkgname-$pkgver.tar.gz"
git archive --format=tar --prefix="Archivist-$pkgver/" HEAD | gzip -n > "$output_dir/$archive"
checksum=$(sha256sum "$output_dir/$archive" | cut -d ' ' -f 1)
sed -e "s/^pkgver=.*/pkgver=$pkgver/" \
    -e 's|^source=.*|source=("$pkgname-$pkgver.tar.gz")|' \
    -e "s/^sha256sums=.*/sha256sums=('$checksum')/" \
    packaging/PKGBUILD > "$output_dir/PKGBUILD"
