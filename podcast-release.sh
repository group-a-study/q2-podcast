#!/usr/bin/env bash
# Build a Q2 Podcast release from a commit of group-a-study/q2-podcast and publish it as that repo's
# latest release. Network Update on Q2 Podcast fetches releases/latest/download/update.tar, so the Q2
# takes it with one press.
#
#   ./podcast-release.sh [REF]     REF: a commit already pushed to the podcast remote (default: podcast/main)
#
# The firmware is built here, because GitHub can't build it: the stock V1.32 ZIP it patches isn't
# public. This is a release build (no --dev): About shows VERSION, and Network Update takes only a
# newer tag. The release is tagged VERSION, its notes are the changelog's **VERSION** entry, and
# update.tar, the notes and the workflow go in a single commit on the branch firmware, replaced each
# time so the tars don't pile up in history. The workflow there publishes them.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
POD=$HERE/q2-pod    # the clone; its remote "podcast" is the fork
TC=$HERE/toolchain  # clang/lld 23 and Python 3.12 from conda-forge, and the stock ZIP
ZIP="$TC/Q2 Firmware V1.32.zip"
REPO=group-a-study/q2-podcast
REMOTE=podcast
export PATH="$TC/llvm/bin:$TC/py312/bin:$PATH"
git -C "$POD" remote get-url "$REMOTE" >/dev/null 2>&1 ||
  git -C "$POD" remote add "$REMOTE" "git@github-groupa:$REPO.git"
git -C "$POD" fetch -q "$REMOTE"
ref=${1:-$REMOTE/main}
src=$(git -C "$POD" rev-parse --verify "$ref^{commit}")
git -C "$POD" branch -r --contains "$src" | grep -q "^ *$REMOTE/" ||
  { echo "$ref ($src) isn't on $REMOTE: push it first, so the release can point at it" >&2; exit 1; }
work=$(mktemp -d "${TMPDIR:-/tmp}/q2-podcast-release.XXXXXX")
cleanup() { git -C "$POD" worktree remove --force "$work/src" 2>/dev/null || true; rm -rf "$work"; }
trap cleanup EXIT
git -C "$POD" worktree add -q --detach "$work/src" "$src"
cd "$work/src"
version=$(sed -n "s/^VERSION = '\([^']*\)'.*/\1/p" tools/build.py)
grep -q "^- \*\*$version\*\*" docs/changelog.md ||
  { echo "docs/changelog.md has no **$version** entry" >&2; exit 1; }
if git -C "$POD" ls-remote --exit-code --tags "$REMOTE" "refs/tags/$version" >/dev/null; then
  echo "$REPO already has a release $version: raise VERSION in tools/build.py first" >&2; exit 1
fi
for step in build test-build test-patch; do
  echo "== $step"
  case $step in
    build) cmd=(python3 tools/build.py "$ZIP" --out "$work/ipod" --update-repo "$REPO") ;;
    test-build) cmd=(python3 test/build.py --build "$work/ipod") ;;
    test-patch) cmd=(python3 test/patch.py "$work/ipod") ;;
  esac
  "${cmd[@]}" > "$work/$step.log" 2>&1 || { tail -30 "$work/$step.log"; exit 1; }
done
fw_tag=$(python3 -c 'import json, sys; print(json.load(open(sys.argv[1]))["version"])' "$work/ipod/manifest.json")
short=$(git rev-parse --short "$src")
tf=$work/tf
mkdir -p "$tf/.github/workflows"
cp "$work/ipod/update.tar" "$tf/update.tar"
(cd "$tf" && sha256sum update.tar > SHA256SUMS)
echo "$src" > "$tf/SOURCE"
echo "$version" > "$tf/TAG"
cp "$HERE/podcast-release.sh" "$tf/podcast-release.sh"
cat > "$tf/.github/workflows/firmware.yml" <<'YML'
name: Firmware release
# A push to this branch publishes its update.tar as the latest release, which Network Update fetches.
# podcast-release.sh builds it locally: the stock ZIP it patches isn't public.
on:
  push:
    branches: [firmware]
permissions:
  contents: write
jobs:
  release:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - run: sha256sum -c SHA256SUMS
      - env:
          GH_TOKEN: ${{ github.token }}
        run: >
          gh release create "$(cat TAG)" update.tar SHA256SUMS
          -R "$GITHUB_REPOSITORY" --target "$(cat SOURCE)" --latest
          --title "Q2 Podcast $(cat TAG)" --notes-file NOTES.md
YML
{
  # the changelog's entry: its nested bullets, one level out
  awk -v v="- **$version**:" 'index($0, v) == 1 { on = 1; next } on && /^  - / { sub(/^  /, ""); print; next } on { exit }' docs/changelog.md
  echo
  echo "Built from \`$short\` on $(date +%F); $fw_tag."
  echo
  echo "**Install:** on a Q2 running Q2 Podcast, **System settings → System Update → Network Update**. Otherwise copy \`update.tar\` to the root of the card, then **System Update → TF card update**; it installs over Q2 Pod or stock firmware."
  echo
  echo '**SHA-256:**'
  echo '```'
  cat "$tf/SHA256SUMS"
  echo '```'
} > "$tf/NOTES.md"
# A one-commit repo of its own, so the tars never enter the clone's object store.
git -C "$tf" init -q
git -C "$tf" symbolic-ref HEAD refs/heads/firmware
git -C "$tf" add -A -f
git -C "$tf" -c user.name="$(git -C "$POD" config user.name)" -c user.email="$(git -C "$POD" config user.email)" \
  commit -q -m "Q2 Podcast $version: $fw_tag, from $src"
git -C "$tf" push -q -f "$(git -C "$POD" remote get-url "$REMOTE")" firmware
echo "Pushed release $version ($fw_tag, from $ref $short)."
echo "In a minute or two it's https://github.com/$REPO/releases/tag/$version"
