#!/bin/sh
set -eu
describe=$1
out=$2
keep=${3:-}
root=$(cd "$(dirname "$0")/.." && pwd)
ver=$(sed -n 's/^project(xombe_stick VERSION \([0-9.]*\) .*/\1/p' "$root/CMakeLists.txt")

targets="xombe_stick_dv3k 3000-serial dmr
xombe_stick_dv3003 3003-serial dmr
xombe_stick_dv3k_ftdi 3000-ftdi dmr
xombe_stick_dv3003_ftdi 3003-ftdi dmr"

cmake -S "$root" -B /build -G Ninja -DXOMBE_KERNELS=q15 -DXOMBE_LTO=ON \
    ${XOMBE_GIT_REPOSITORY:+-DXOMBE_GIT_REPOSITORY="$XOMBE_GIT_REPOSITORY"} -DXOMBE_STICK_DESCRIBE="$describe" >/build.log 2>&1 ||
    { cat /build.log >&2; exit 1; }
# shellcheck disable=SC2046
cmake --build /build --target $(echo "$targets" | cut -d' ' -f1) >>/build.log 2>&1 ||
    { tail -40 /build.log >&2; exit 1; }

dmr=$(mktemp -d)
echo "$targets" | while read -r t name class; do
    f=xombe-stick-$ver-$name.uf2
    if [ "$class" = ship ]; then
        cp "/build/$t.uf2" "$out/$f"
    else
        cp "/build/$t.uf2" "$dmr/$f"
    fi
done
(cd "$dmr" && for f in *.uf2; do [ -e "$f" ] && sha256sum "$f"; done) | sed 's|  |  x0rro/|'
if [ -n "$keep" ]; then
    mkdir -p "$keep" && cp "$dmr"/*.uf2 "$keep/"
fi
rm -rf "$dmr" /build/*.uf2

{
    arm-none-eabi-gcc --version | head -1
    cmake --version | head -1
    for d in /build/_deps/*-src /build/_deps/pico_sdk-src/lib/tinyusb; do
        [ -d "$d" ] && echo "$(basename "$d") $(git -C "$d" rev-parse HEAD)"
    done
    dpkg-query -W -f='${db:Status-Abbrev} ${Package}:${Architecture} ${Version}\n' |
        awk '$1 == "ii" { print $2, $3 }' | LC_ALL=C sort
} > "$out/xombe-stick-$ver.buildinfo"
