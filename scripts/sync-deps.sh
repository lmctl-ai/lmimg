#!/usr/bin/env bash
# Fetch every dependency in deps.lock into git-ignored third_party/NAME at the pinned commit.
# --verify only checks that each checkout is at its pin.
set -eu
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
while read -r name repo commit; do
  case "$name" in ''|\#*) continue;; esac
  dir="$HERE/third_party/$name"
  if [ "${1:-}" = "--verify" ]; then
    [ -d "$dir/.git" ] || { echo "third_party/$name missing: run scripts/sync-deps.sh" >&2; exit 1; }
    have="$(git -C "$dir" rev-parse HEAD)"
    [ "$have" = "$commit" ] || { echo "third_party/$name at $have, deps.lock pins $commit" >&2; exit 1; }
    git -C "$dir" diff --quiet HEAD -- || { echo "third_party/$name has modified tracked source" >&2; exit 1; }
    continue
  fi
  if [ ! -d "$dir/.git" ]; then mkdir -p "$dir"; git -C "$dir" init -q; git -C "$dir" remote add origin "$repo"; fi
  git -C "$dir" fetch -q --depth 1 origin "$commit"
  git -C "$dir" checkout -q -f FETCH_HEAD
  echo "synced third_party/$name at $commit"
done < "$HERE/deps.lock"
[ "${1:-}" = "--verify" ] && echo "third_party matches deps.lock"
exit 0
