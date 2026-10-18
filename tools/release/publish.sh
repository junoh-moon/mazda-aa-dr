#!/bin/sh
# Release stage 10 (publication, a separate requested action): create the
# annotated tag at the pinned commit, push it, and turn the existing draft
# public. Never creates a second release; never moves or force-pushes a tag.
# Refuses unless REL_CONFIRM_PUBLISH equals the tag (an explicit,
# per-release decision) and every gate stage recorded exit 0 (stages listed in
# REL_REQUIRED, default: setup build zip arm host pack stock soak replay draft).
#   REL_CONFIRM_PUBLISH=vX.Y.Z sh tools/release/publish.sh [WORK]
. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/common.sh"
rel_work "${1:-}"
stage_begin publish
set -e
[ "${REL_CONFIRM_PUBLISH:-}" = "$TAG" ] || { echo "REL_CONFIRM_PUBLISH must equal $TAG"; exit 1; }
for s in ${REL_REQUIRED:-setup build zip arm host pack stock soak replay draft}; do
    [ "$(cat "$WORK/jobs/$s.exit" 2>/dev/null)" = 0 ] || { echo "stage $s did not pass"; exit 1; }
done
cd "$SRC"
test "$(git rev-parse HEAD)" = "$COMMIT"
test -s "$WORK/release-notes.md"
n=$(gh api "repos/$REPO/releases" --paginate --jq "[.[]|select(.tag_name==\"$TAG\")]|length")
[ "$n" = 1 ] || { echo "expected exactly one (draft) release named $TAG, found $n"; exit 1; }
[ "$(gh release view "$TAG" --repo "$REPO" --json isDraft -q .isDraft)" = true ] || { echo 'release is not a draft'; exit 1; }
git tag -a "$TAG" "$COMMIT" -m "Release $TAG"
git push origin "refs/tags/$TAG"
pre=${REL_PRERELEASE:-$(case $TAG in *-*) echo 1;; *) echo 0;; esac)}
if [ "$pre" = 1 ]; then gh release edit "$TAG" --repo "$REPO" --draft=false --prerelease --latest=false
else gh release edit "$TAG" --repo "$REPO" --draft=false --prerelease=false --latest; fi
echo "tag->commit: $(git rev-parse "$TAG^{commit}")"
