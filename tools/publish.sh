#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Kerem Kilic (Ellic Studio)
# Publish this repository's tree as a commit has it (REV, default HEAD) into a checkout of its public repository (DEST,
# default melodee-modules-move-public beside the main checkout): one commit there, REV's subject unless -m gives another.
# The melodee-modules submodule points at the public melodee-modules commit with the same tree as REV's (MM_PUBLIC,
# default melodee-modules-public beside it: publish melodee-modules first). Pushing stays yours.
#   tools/publish.sh [-m SUBJECT] [DEST] [REV]
set -eu
msg=
if [ "${1:-}" = "-m" ]; then msg=$2; shift 2; fi
here=$(cd "$(dirname "$0")/.." && pwd)
main=$(dirname "$(git -C "$here" rev-parse --path-format=absolute --git-common-dir)")
dest=${1:-"$(dirname "$main")/melodee-modules-move-public"}
pub=${MM_PUBLIC:-"$(dirname "$main")/melodee-modules-public"}
rev=${2:-HEAD}
sha=$(git -C "$here" rev-parse --short=8 "$rev^{commit}")
mm=$(git -C "$here" ls-tree "$sha" melodee-modules | awk '{print $3}')
tree=$(git -C "$here/melodee-modules" rev-parse "$mm^{tree}" 2>/dev/null) ||
    { echo "publish: melodee-modules $mm is not in $here/melodee-modules (git submodule update)" >&2; exit 1; }
link=$(git -C "$pub" log --format='%H %T' | awk -v t="$tree" '$2 == t { print $1; exit }')
[ -n "$link" ] || { echo "publish: no commit in $pub has melodee-modules $mm's tree (publish it first)" >&2; exit 1; }
[ "$(git -C "$dest" rev-parse --show-toplevel 2>/dev/null)" = "$(cd "$dest" 2>/dev/null && pwd -P)" ] ||
    { echo "publish: $dest is not a git checkout's top (git init or clone it first)" >&2; exit 1; }
[ -z "$(git -C "$dest" status --porcelain)" ] || { echo "publish: $dest has changes of its own" >&2; exit 1; }
git -C "$dest" rm -rq --ignore-unmatch -- .
git -C "$here" archive "$sha" | tar -x -C "$dest"
git -C "$dest" add -A
git -C "$dest" update-index --add --cacheinfo 160000,"$link",melodee-modules
if git -C "$dest" diff --cached --quiet; then
    echo "publish: $dest already has $sha"
else
    git -C "$dest" commit -q -m "${msg:-$(git -C "$here" log -1 --format=%s "$sha")}"
    echo "publish: $dest $(git -C "$dest" log -1 --format='%h %s' | cut -c1-100) (melodee-modules $(echo "$link" | cut -c1-8))"
fi
