#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$HERE/build}"
HOST="$BUILD_DIR/lmimg"
COMPILER="${LMSCRIPT:-$("$HERE/scripts/select-compiler.sh")}"
[ -x "$HOST" ] || { echo "build lmimg first: $HOST" >&2; exit 1; }
PATH="$(cd "$(dirname "$HOST")" && pwd):$PATH"
export PATH
mkdir -p "$HERE/docs/images"
work="$(mktemp -d "${TMPDIR:-/tmp}/lmimg-gallery-XXXXXX")"
trap 'rm -rf "$work"' EXIT
for src in "$HERE"/examples/*.lms; do
  name="$(basename "$src" .lms)"
  (cd "$work" && "$COMPILER" "$src" "${name}.dslb" >/dev/null && "$HOST" --dsl "${name}.dslb" >/dev/null)
  cp "$work/images/${name}.png" "$HERE/docs/images/${name}.png"
  if [ -f "$work/images/${name}.gif" ]; then cp "$work/images/${name}.gif" "$HERE/docs/images/${name}.gif"; fi
done
python3 "$HERE/scripts/check-gallery.py" --generated
