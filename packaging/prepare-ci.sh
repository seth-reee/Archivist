#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."
output_dir=$(realpath -m "${1:?Usage: prepare-ci.sh OUTPUT_DIRECTORY}")
mkdir -p "$output_dir"
source packaging/PKGBUILD
version=$(sed -n 's/^project(Archivist VERSION \([^ ]*\) LANGUAGES CXX)/\1/p' CMakeLists.txt)
[[ "$version" == "$pkgver" ]] || { echo "CMake and PKGBUILD versions differ" >&2; exit 1; }
release=false
if [[ "${AUTO_RELEASE:-false}" == true ]]; then
  # A manually increased, untagged version is respected; otherwise bump patch.
  if git rev-parse --verify "refs/tags/v$pkgver" >/dev/null 2>&1; then
    IFS=. read -r major minor patch <<< "$pkgver"
    pkgver="$major.$minor.$((patch + 1))"
  fi
  sed -i "s/project(Archivist VERSION $version /project(Archivist VERSION $pkgver /" CMakeLists.txt
  sed -i -e "s/^pkgver=.*/pkgver=$pkgver/" -e 's/^pkgrel=.*/pkgrel=1/' packaging/PKGBUILD
  release=true
elif [[ "${GITHUB_REF_TYPE:-}" == tag ]]; then
  [[ "${GITHUB_REF_NAME:?}" == "v$pkgver" ]] || {
    echo "Release tag must match CMake and PKGBUILD version v$pkgver" >&2; exit 1;
  }
  release=true
else
  pkgver="$pkgver.r$(git rev-list --count HEAD).g$(git rev-parse --short=8 HEAD)"
fi
archive="$pkgname-$pkgver.tar.gz"
# Only application files: the pinned recipe cannot hash an archive containing itself.
# Stable metadata makes both architectures and retries use identical source bytes.
tar --sort=name --mtime=@0 --owner=0 --group=0 --numeric-owner \
  --transform="s,^,Archivist-$pkgver/," -cf - \
  CMakeLists.txt LICENSE README.md main.cpp tests.cpp archivist.desktop archivist.png \
  | gzip -n > "$output_dir/$archive"
checksum=$(sha256sum "$output_dir/$archive" | cut -d ' ' -f 1)
sed -e "s/^pkgver=.*/pkgver=$pkgver/" \
    -e 's|^source=.*|source=("$pkgname-$pkgver.tar.gz::$url/releases/download/v$pkgver/$pkgname-$pkgver.tar.gz")|' \
    -e "s/^sha256sums=.*/sha256sums=('$checksum')/" \
    packaging/PKGBUILD > "$output_dir/PKGBUILD"
if [[ "${AUTO_RELEASE:-false}" == true ]]; then
  cp "$output_dir/PKGBUILD" packaging/PKGBUILD
  git add CMakeLists.txt packaging/PKGBUILD
  # Fixed timestamp and identity make reruns produce the same release commit.
  timestamp=$(git show -s --format=%cI HEAD)
  GIT_AUTHOR_DATE="$timestamp" GIT_COMMITTER_DATE="$timestamp" \
    git -c user.name='github-actions[bot]' \
        -c user.email='41898282+github-actions[bot]@users.noreply.github.com' \
        commit -m "Release $pkgver"
  git bundle create "$output_dir/release.bundle" HEAD
fi
if [[ -n "${GITHUB_OUTPUT:-}" ]]; then
  printf 'version=%s\nrelease=%s\ncommit=%s\n' "$pkgver" "$release" "$(git rev-parse HEAD)" >> "$GITHUB_OUTPUT"
fi
