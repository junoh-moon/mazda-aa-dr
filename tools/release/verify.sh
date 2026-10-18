#!/bin/sh
# Release stage 11 (G18, read-only): download the PUBLISHED assets again and
# prove they are the built bytes: sha256 sidecar, cmp with WORK/dist, unzip -t,
# SHA256SUMS inside, build-info commit, entry files, the tag's commit, release
# public with the expected pre-release/latest state, and the ZIP checker.
#   sh tools/release/verify.sh [WORK]
. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/common.sh"
rel_work "${1:-}"
stage_begin verify
set -e
d=$WORK/downloaded; rm -rf "$d" "$WORK/dl_unpacked"; mkdir "$d"
gh release download "$TAG" --repo "$REPO" --pattern "$NAME.zip" --pattern "$NAME.zip.sha256" --dir "$d"
(cd "$d" && sha256sum -c "$NAME.zip.sha256")
cmp "$WORK/dist/$NAME.zip" "$d/$NAME.zip"
echo 'cmp: the published ZIP is byte-identical to the built ZIP'
unzip -tq "$d/$NAME.zip"
mkdir "$WORK/dl_unpacked"; unzip -q "$d/$NAME.zip" -d "$WORK/dl_unpacked"
(cd "$WORK/dl_unpacked" && sha256sum -c --quiet SHA256SUMS)
test "$(jq -r .source_commit "$WORK/dl_unpacked/build-info.json")" = "$COMMIT"
test "$(jq -r .source_modified "$WORK/dl_unpacked/build-info.json")" = false
test "$(git -C "$SRC" ls-remote origin "refs/tags/$TAG^{}" | cut -f1)" = "$COMMIT"
v=$(gh release view "$TAG" --repo "$REPO" --json isDraft,isPrerelease,url -q '"\(.isDraft) \(.isPrerelease) \(.url)"')
echo "release: $v"
case $v in false\ *) ;; *) echo 'release still a draft'; exit 1;; esac
pre=${REL_PRERELEASE:-$(case $TAG in *-*) echo 1;; *) echo 0;; esac)}
[ "$(echo "$v" | cut -d' ' -f2)" = "$([ "$pre" = 1 ] && echo true || echo false)" ]
if [ "$pre" = 0 ]; then
    test "$(gh release view --repo "$REPO" --json tagName -q .tagName)" = "$TAG"   # latest
fi
python3 "$GATES/g5g6_zipcheck.py" --zip "$d/$NAME.zip" --commit "$COMMIT" --mode "${REL_MODE:-BETA}" \
    --readelf "$SRC/tools/m3-toolchain/bin/arm-cortexa9_neon-linux-gnueabi-readelf" > "$WORK/evidence/verify-zipcheck.txt"
tail -1 "$WORK/evidence/verify-zipcheck.txt"
evidence verify-zipcheck.txt
