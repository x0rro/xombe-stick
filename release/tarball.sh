#!/bin/sh
set -eu
out=$1
root=$(cd "$(dirname "$0")/.." && pwd)
ver=$(sed -n 's/^project(xombe_stick VERSION \([0-9.]*\) .*/\1/p' "$root/CMakeLists.txt")
name=xombe-stick-$ver
git -C "$root" archive --format=tar --prefix="$name/" HEAD -- . ':!.gitignore' | xz -9 -T1 > "$out/$name.tar.xz"
