#!/bin/sh
# Release stage 0 (G0): pin TAG and COMMIT in a fresh clone.
#   sh tools/release/setup.sh TAG COMMIT40 [OWNER/REPO]
# Creates WORK=$(mktemp -d ${REL_TMP:-/tmp}/mazda-aa-dr-release.XXXXXX) with
# source/ (detached at COMMIT, clean), evidence/, dist/, jobs/ and release.env
# (TAG, COMMIT, REPO, NAME), and points $REL_POINTER at it. Fails closed when
# the tag format is wrong, COMMIT is not 40 hex or not on origin/master, the
# tree is not clean, or the tag already exists locally or on GitHub.
. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/common.sh"
[ $# -ge 2 ] || die 'usage: setup.sh TAG COMMIT40 [OWNER/REPO]'
TAG=$1; COMMIT=$2; REPO=${3:-$REL_REPO_DEFAULT}
echo "$TAG" | grep -Eq '^v[0-9][A-Za-z0-9._-]*$' || die "bad tag $TAG"
echo "$COMMIT" | grep -Eq '^[0-9a-f]{40}$' || die 'COMMIT must be the full 40-hex SHA'
gh auth status >/dev/null 2>&1 || die 'gh is not authenticated'
WORK=$(mktemp -d "${REL_TMP:-/tmp}/mazda-aa-dr-release.XXXXXX") || die mktemp
mkdir -p "$WORK/jobs"
(
    set -e
    git clone -q "git@github.com:$REPO.git" "$WORK/source"
    cd "$WORK/source"
    git fetch -q origin master --tags
    git cat-file -e "$COMMIT^{commit}"
    git merge-base --is-ancestor "$COMMIT" origin/master
    git checkout -q --detach "$COMMIT"
    test "$(git rev-parse HEAD)" = "$COMMIT"
    test -z "$(git status --porcelain)"
) > "$WORK/jobs/setup.log" 2>&1 || die "clone/pin failed (see $WORK/jobs/setup.log)"
if git -C "$WORK/source" show-ref --verify --quiet "refs/tags/$TAG" ||
   [ -n "$(git -C "$WORK/source" ls-remote --tags origin "refs/tags/$TAG")" ]; then
    die "tag $TAG already exists: choose a new version, never move a tag"
fi
if gh release view "$TAG" --repo "$REPO" >/dev/null 2>&1; then
    die "a release named $TAG already exists (draft or public)"
fi
mkdir -p "$WORK/evidence" "$WORK/dist"
printf "TAG='%s'\nCOMMIT='%s'\nREPO='%s'\nNAME='mazda-aa-dr-%s'\n" "$TAG" "$COMMIT" "$REPO" "$TAG" > "$WORK/release.env"
mkdir -p "$(dirname "$REL_POINTER")" && echo "$WORK" > "$REL_POINTER"
echo 0 > "$WORK/jobs/setup.exit"
echo "work=$WORK tag=$TAG commit=$COMMIT (pointer $REL_POINTER)"
