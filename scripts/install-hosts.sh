#!/usr/bin/env bash
set -euo pipefail
shopt -s globstar nullglob

if [[ ! -d examples ]]; then
  echo "install-hosts: no examples directory; nothing to install"
  exit 0
fi

host_bin="${LMCTL_HOST_BIN_DIR:-${RUNNER_TEMP:-/tmp}/lmctl-example-hosts}"
mkdir -p "$host_bin"
skip_file="${RUNNER_TEMP:-/tmp}/lmctl-unavailable-examples.tsv"
: > "$skip_file"

declare -A tag_by_repo=()
declare -A owner_by_repo=()
declare -A cli_names=()
declare -A cli_repo=()
declare -A unavailable_reason=()
install_re="install[[:space:]]*=[[:space:]]*[\"'](github:[^\"']+)[\"']"
cli_re="cli[[:space:]]*=[[:space:]]*[\"']([^\"']+)[\"']"
examples=(examples/**/*.lms)

for example in "${examples[@]}"; do
  while IFS= read -r line || [[ -n "$line" ]]; do
    if [[ "$line" =~ $install_re ]]; then
      spec="${BASH_REMATCH[1]#github:}"
      if [[ "$spec" != *@* ]]; then
        echo "install-hosts: unsupported GitHub install (missing @TAG) in $example: $spec" >&2
        exit 1
      fi
      full_repo="${spec%@*}"
      tag="${spec##*@}"
      if [[ ! "$full_repo" =~ ^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$ || ! "$tag" =~ ^[A-Za-z0-9_.-]+$ ]]; then
        echo "install-hosts: invalid GitHub install in $example: $spec" >&2
        exit 1
      fi
      repo="${full_repo##*/}"
      if [[ -n "${tag_by_repo[$full_repo]+set}" && "${tag_by_repo[$full_repo]}" != "$tag" ]]; then
        echo "install-hosts: conflicting tags for $full_repo (${tag_by_repo[$full_repo]} and $tag)" >&2
        exit 1
      fi
      tag_by_repo["$full_repo"]="$tag"
      owner_by_repo["$full_repo"]="$repo"
    fi
    if [[ "$line" =~ $cli_re ]]; then
      cli="${BASH_REMATCH[1]}"
      cli="${cli%%[[:space:]]*}"
      [[ -z "$cli" ]] || cli_names["$cli"]=1
    fi
  done < "$example"
done

for full_repo in "${!tag_by_repo[@]}"; do
  repo="${owner_by_repo[$full_repo]}"
  tag="${tag_by_repo[$full_repo]}"
  if [[ -n "${cli_names[$repo]+set}" ]]; then cli_repo["$repo"]="$full_repo"; fi

  if ! gh repo view "$full_repo" --json name >/dev/null 2>&1; then
    echo "install-hosts: cannot verify public repository $full_repo" >&2
    exit 1
  fi

  if [[ "$tag" == latest ]]; then
    if ! resolved_tag="$(gh release list -R "$full_repo" -L 1 --json tagName --jq '.[0].tagName // empty')"; then
      echo "install-hosts: release lookup failed for $full_repo@latest" >&2
      exit 1
    fi
    if [[ -z "$resolved_tag" ]]; then
      reason="$full_repo has no published release yet; dependent examples are not checkable"
      echo "install-hosts: SKIP $full_repo@latest: $reason"
      unavailable_reason["$repo"]="$reason"
      continue
    fi
    tag="$resolved_tag"
  else
    response=""
    api_status=0
    response="$(gh api -i "repos/$full_repo/releases/tags/$tag" 2>&1)" || api_status=$?
    first_line="${response%%$'\n'*}"
    if [[ "$first_line" =~ HTTP/[0-9.]+[[:space:]]([0-9]{3}) ]]; then
      release_status="${BASH_REMATCH[1]}"
    else
      release_status="000"
    fi
    if [[ "$release_status" == 404 ]]; then
      reason="$full_repo@$tag has no published release; dependent examples are not checkable"
      echo "install-hosts: SKIP $full_repo@$tag: $reason"
      unavailable_reason["$repo"]="$reason"
      continue
    fi
    if [[ "$api_status" != 0 || "$release_status" != 200 ]]; then
      echo "install-hosts: release lookup failed for $full_repo@$tag ($first_line)" >&2
      exit 1
    fi
  fi

  tmp="$(mktemp -d "${RUNNER_TEMP:-/tmp}/lmctl-host-download.XXXXXX")"
  if ! gh release download "$tag" -R "$full_repo" --pattern "$repo-linux-x64" --dir "$tmp"; then
    rm -rf "$tmp"
    echo "install-hosts: release exists but $repo-linux-x64 could not be downloaded from $full_repo@$tag" >&2
    exit 1
  fi
  asset="$tmp/$repo-linux-x64"
  if [[ ! -s "$asset" ]]; then
    rm -rf "$tmp"
    echo "install-hosts: release exists but has no non-empty $repo-linux-x64 asset: $full_repo@$tag" >&2
    exit 1
  fi
  cp "$asset" "$host_bin/$repo"
  chmod +x "$host_bin/$repo"
  rm -rf "$tmp"
  echo "install-hosts: installed $repo from $full_repo@$tag"
done

for cli in "${!cli_names[@]}"; do
  if [[ -z "${cli_repo[$cli]+set}" ]]; then
    echo "install-hosts: cli=\"$cli\" has no matching GitHub install declaration" >&2
    exit 1
  fi
done

for example in "${examples[@]}"; do
  while IFS= read -r line || [[ -n "$line" ]]; do
    if [[ "$line" =~ $install_re ]]; then
      spec="${BASH_REMATCH[1]#github:}"
      full_repo="${spec%@*}"
      repo="${full_repo##*/}"
      if [[ -n "${unavailable_reason[$repo]+set}" ]]; then
        printf '%s\t%s\n' "$example" "${unavailable_reason[$repo]}" >> "$skip_file"
        break
      fi
    fi
  done < "$example"
done

if [[ -n "${GITHUB_ENV:-}" ]]; then
  printf 'LMCTL_UNAVAILABLE_EXAMPLES_FILE=%s\n' "$skip_file" >> "$GITHUB_ENV"
fi
export PATH="$host_bin:$PATH"
if [[ -n "${GITHUB_PATH:-}" ]]; then printf '%s\n' "$host_bin" >> "$GITHUB_PATH"; fi
echo "install-hosts: host directory prepended to PATH: $host_bin"
