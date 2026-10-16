#!/usr/bin/env bash
set -euo pipefail
tag=${1:?Usage: publish-ci.sh TAG ASSET_DIRECTORY}
cd "${2:?Usage: publish-ci.sh TAG ASSET_DIRECTORY}"
sha256sum -c SHA256SUMS-x86_64
sha256sum -c SHA256SUMS-aarch64
# Published releases are immutable to this workflow; reruns only verify them.
if gh release view "$tag" --json isDraft --jq .isDraft > release-state; then
  draft=$(cat release-state)
else
  gh release create "$tag" --verify-tag --draft --generate-notes --title "Archivist ${tag#v}"
  draft=true
fi
if [[ "$draft" == true ]]; then
  gh release upload "$tag" ./*.pkg.tar.zst ./*.tar.gz PKGBUILD SHA256SUMS-* --clobber
fi
verify_dir=$(mktemp -d)
trap 'rm -rf "$verify_dir"' EXIT
gh release download "$tag" --dir "$verify_dir"
cmp SHA256SUMS-x86_64 "$verify_dir/SHA256SUMS-x86_64"
cmp SHA256SUMS-aarch64 "$verify_dir/SHA256SUMS-aarch64"
cd "$verify_dir"
sha256sum -c SHA256SUMS-x86_64
sha256sum -c SHA256SUMS-aarch64
if [[ "$draft" == true ]]; then
  gh release edit "$tag" --draft=false --latest
fi
