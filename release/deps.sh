#!/bin/sh
set -eu
here=$(cd "$(dirname "$0")" && pwd)
rm -f /etc/apt/sources.list.d/*
printf '%s\n' 'deb http://deb.debian.org/debian trixie main' \
    'deb [check-valid-until=no] http://snapshot.debian.org/archive/debian/20261111T000000Z trixie main' \
    > /etc/apt/sources.list
apt-get -o Acquire::Retries=5 update || true
# shellcheck disable=SC2046
DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends $(cat "$here/packages.lock")
rm -rf /var/lib/apt/lists/*
