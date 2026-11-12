#!/bin/sh
set -eu
check=0 keep=
while getopts ck o; do
    case $o in
    c) check=1 ;;
    k) keep=/out/x0rro ;;
    *) exit 2 ;;
    esac
done
shift $((OPTIND - 1))
ref=${1:-HEAD}
root=$(git rev-parse --show-toplevel)
commit=$(git rev-parse --verify "$ref^{commit}")
gitdir=$(cd "$(git rev-parse --git-common-dir)" && pwd)
describe=${XOMBE_STICK_DESCRIBE:-$(git describe --tags --match 'v*' --always "$commit")}
engine=${CONTAINER:-podman}
xombe=${XOMBE_REPO:-https://github.com/x0rro/xombe}
image=xombe-stick-build:$(git show "$commit:release/Containerfile" "$commit:release/deps.sh" "$commit:release/packages.lock" |
    sha256sum | cut -c1-12)
if ! "$engine" image exists "$image" 2>/dev/null; then
    ctx=$(mktemp -d)
    git show "$commit:release/Containerfile" > "$ctx/Containerfile"
    git show "$commit:release/deps.sh" > "$ctx/deps.sh" && chmod +x "$ctx/deps.sh"
    git show "$commit:release/packages.lock" > "$ctx/packages.lock"
    "$engine" build -t "$image" "$ctx" >&2
    rm -rf "$ctx"
fi
case $xombe in
*://*) repo=$xombe mount= ;;
*) repo=/xombe mount="-v $(cd "$xombe" && pwd):/xombe:ro" ;;
esac

tmp=$(mktemp -d "${TMPDIR:-/tmp}/xombe-stick-release.XXXXXX")
trap 'rm -rf "$tmp"' EXIT

build() {
    mkdir "$1"
    # shellcheck disable=SC2086
    "$engine" run --rm -v "$gitdir:/repo:ro" $mount -v "$1:/out" -e XOMBE_GIT_REPOSITORY="$repo" "$image" sh -euc '
        git config --global --add safe.directory "*"
        git clone -q /repo /src
        git -C /src checkout -q "$1"
        /src/release/tarball.sh /out
        /src/release/bins.sh "$2" /out $3 > /out/dmr.sums
    ' _ "$commit" "$describe" "$keep" >&2
    (cd "$1" && sha256sum -- xombe-stick-* | LC_ALL=C sort -k2 > SHA256SUMS)
}

build "$tmp/a"
if [ $check = 1 ]; then
    build "$tmp/b"
    if ! cmp -s "$tmp/a/SHA256SUMS" "$tmp/b/SHA256SUMS" || ! cmp -s "$tmp/a/dmr.sums" "$tmp/b/dmr.sums"; then
        echo "release/sums.sh: two clean builds differ" >&2
        diff "$tmp/a/SHA256SUMS" "$tmp/b/SHA256SUMS" >&2 || true
        diff "$tmp/a/dmr.sums" "$tmp/b/dmr.sums" >&2 || true
        exit 1
    fi
    echo "two clean builds match" >&2
fi
rm -rf "$root/dist" && mkdir "$root/dist"
(cd "$tmp/a" && cp -r -- xombe-stick-* SHA256SUMS "$root/dist/" && { [ ! -d x0rro ] || cp -r x0rro "$root/dist/"; })
cat "$tmp/a/SHA256SUMS"
echo
echo "DMR+D-STAR builds, not shipped:"
cat "$tmp/a/dmr.sums"
