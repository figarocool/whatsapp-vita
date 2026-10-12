#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
output="${1:-$repo_root/build/vita-sw-decoder-2.0.0.tar.gz}"
[[ -z "$(git -C "$repo_root" status --porcelain)" ]] || {
  echo "Refusing source package from a dirty tree" >&2; exit 1;
}
mkdir -p "$(dirname "$output")"
git -C "$repo_root" archive --format=tar \
  --prefix=vita-sw-decoder-2.0.0/ HEAD | gzip -n > "$output"
tar -tzf "$output" >/dev/null
echo "$output"
