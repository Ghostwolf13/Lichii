#!/usr/bin/env bash
# Deletes this ref's Actions caches whose key matches a regex, except the one just saved. Run after
# a cache save on nx2 so each cache family keeps one entry there: superseded entries filled the
# repository's 10 GB cache budget and evicted the release builds' caches. Nothing is deleted unless
# the saved key exists on this ref, so a failed or skipped save keeps the previous entry.
#
# usage: prune-caches.sh <key regex> <saved key>
set -euo pipefail

export PRUNE_PATTERN=$1 PRUNE_KEEP=$2
caches=$(gh cache list -R "$GITHUB_REPOSITORY" --ref "$GITHUB_REF" --limit 1000 --json id,key \
    --jq '.[] | select(.key | test(env.PRUNE_PATTERN)) | "\(.id) \(.key)"')
if ! grep -qxF -- "$PRUNE_KEEP" <<< "$(cut -d' ' -f2- <<< "$caches")"; then
    echo "No cache saved as '$PRUNE_KEEP'; keeping the existing entries"
    exit 0
fi
while read -r id key; do
    if [ "$key" != "$PRUNE_KEEP" ]; then
        gh cache delete -R "$GITHUB_REPOSITORY" "$id"
    fi
done <<< "$caches"
