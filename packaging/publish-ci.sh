#!/usr/bin/env bash
set -euo pipefail
tag=${1:?Usage: publish-ci.sh TAG ASSET_DIRECTORY}
cd "${2:?Usage: publish-ci.sh TAG ASSET_DIRECTORY}"
for sums in SHA256SUMS-*; do sha256sum -c "$sums"; done
# Published releases are immutable to this workflow; reruns only verify them.
if gh release view "$tag" --json isDraft --jq .isDraft > release-state; then
  draft=$(cat release-state)
else
  gh release create "$tag" --verify-tag --draft --generate-notes --title "Archivist ${tag#v}"
  draft=true
fi
if [[ "$draft" == true ]]; then
  gh release upload "$tag" ./*.pkg.tar.zst ./*.tar.gz ./*.nar.xz ./*.store-path PKGBUILD SHA256SUMS-* --clobber
fi
verify_dir=$(mktemp -d)
trap 'rm -rf "$verify_dir"' EXIT
gh release download "$tag" --dir "$verify_dir"
for sums in SHA256SUMS-*; do cmp "$sums" "$verify_dir/$sums"; done
cd "$verify_dir"
for sums in SHA256SUMS-*; do sha256sum -c "$sums"; done
if [[ "$draft" == true ]]; then
  gh release edit "$tag" --draft=false --latest
fi
