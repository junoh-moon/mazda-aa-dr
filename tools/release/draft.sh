#!/bin/sh
# Release stage 9: upload the built ZIP and its .sha256 to a DRAFT release
# targeting the pinned commit, WITHOUT creating the tag (publish.sh creates
# it). Needs WORK/release-notes.md. A tag with a '-' suffix (beta, rc) is a
# pre-release; a plain vX.Y.Z is marked latest (REL_PRERELEASE=1|0 overrides).
# Refuses when any release with this tag already exists (no duplicate drafts).
#   REL_TITLE='...' sh tools/release/draft.sh [WORK]
. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/common.sh"
rel_work "${1:-}"
stage_begin draft
set -e
test -s "$WORK/release-notes.md"
test -s "$WORK/dist/$NAME.zip"
(cd "$WORK/dist" && sha256sum -c "$NAME.zip.sha256")
for s in setup build zip; do test "$(cat "$WORK/jobs/$s.exit" 2>/dev/null)" = 0; done
n=$(gh api "repos/$REPO/releases" --paginate --jq "[.[]|select(.tag_name==\"$TAG\")]|length")
[ "$n" = 0 ] || { echo "$n release(s) named $TAG exist already"; exit 1; }
pre=${REL_PRERELEASE:-$(case $TAG in *-*) echo 1;; *) echo 0;; esac)}
if [ "$pre" = 1 ]; then flags='--prerelease --latest=false'; else flags='--latest'; fi
# shellcheck disable=SC2086
gh release create "$TAG" --repo "$REPO" --target "$COMMIT" --draft $flags \
    --title "${REL_TITLE:-$TAG}" --notes-file "$WORK/release-notes.md" \
    "$WORK/dist/$NAME.zip" "$WORK/dist/$NAME.zip.sha256"
gh release view "$TAG" --repo "$REPO" --json isDraft,isPrerelease,tagName,targetCommitish,assets \
    -q '{draft:.isDraft,prerelease:.isPrerelease,tag:.tagName,target:.targetCommitish,assets:[.assets[]|{name,size}]}'
