#!/bin/sh
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
engine=${CONTAINER:-podman}
base=$(sed -n 's/^FROM //p' "$root/release/Containerfile")
snap=$(grep -o 'http://snapshot\.debian\.org/archive/debian/[0-9TZ]*' "$root/release/deps.sh")
pkgs="ca-certificates cmake ninja-build git python3 build-essential gcc-arm-none-eabi libnewlib-arm-none-eabi
libstdc++-arm-none-eabi-newlib libusb-1.0-0-dev pkg-config xz-utils"
"$engine" run --rm "$base" sh -euc '
    export DEBIAN_FRONTEND=noninteractive
    q() { dpkg-query -W -f="\${Package}=\${Version}\n" | LC_ALL=C sort; }
    q > /tmp/base
    rm -f /etc/apt/sources.list.d/*
    echo "deb [check-valid-until=no] $1 trixie main" > /etc/apt/sources.list
    apt-get -qq -o Acquire::Retries=5 update
    apt-get -qq install -y --no-install-recommends $2 > /dev/null
    q | LC_ALL=C comm -13 /tmp/base -
' _ "$snap" "$pkgs"
