#!/usr/bin/env bash
set -euo pipefail
output_dir=$(realpath "${1:?Usage: build-ci.sh OUTPUT_DIRECTORY}")
: "${PACKAGE_VERSION:?}" "${PACKAGE_SYSTEM:?}"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
tar -xf "$output_dir/archivist-$PACKAGE_VERSION.tar.gz" -C "$work"
cd "$work/Archivist-$PACKAGE_VERSION"
nix build --no-update-lock-file ".#packages.$PACKAGE_SYSTEM.archivist" --out-link "$work/result" -L
store_path=$(readlink -f "$work/result")
name="archivist-$PACKAGE_VERSION-nix-$PACKAGE_SYSTEM"
printf '%s\n' "$store_path" > "$output_dir/$name.store-path"
# Export the full runtime dependency closure, including Qt.
mapfile -t closure < <(nix-store --query --requisites "$store_path")
nix-store --export "${closure[@]}" | xz -T2 > "$output_dir/$name.nar.xz"
cd "$output_dir"
sha256sum "$name.nar.xz" "$name.store-path" > "SHA256SUMS-nix-$PACKAGE_SYSTEM"
