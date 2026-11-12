# xombe-stick

firmware for the raspberry pi pico 2 to act as a drop-in for AMBE-300X USB
sticks such as ThumbDV, USB-3000, and USB-3003. uses MIT-licensed
[xombe](https://github.com/x0rro/xombe) as the vocoder and supports D-STAR and
DMR.

tested against SerialDV 1.1.5, codecserver 0.3.0-dev, AMBEserver (both marrold
and NW Digital Radio), and ambed 1.4.0 (xlxd).

## quick start

build the UF2s first ([building](#building)), then enter bootloader by holding
BOOTSEL while plugging in the board. copy your chosen UF2 onto the `RP2350` drive
that appears. then configure your software to point at the new serial port.

for codecserver (check `/etc/codecserver/codecserver.conf`):

    [device:stick]
    driver=ambe3k
    # replace with port from ls /dev/serial/by-id
    tty=/dev/serial/by-id/usb-x0rro_xombe-stick_YOURSERIAL-if00
    # required, and ignored by the stick
    baudrate=460800

or AMBEserver: `AMBEserver -i /dev/ttyACM0 -s 460800`.

## which uf2

    xombe_stick_dv3k.uf2           AMBE-3000R, one channel, USB serial port
    xombe_stick_dv3003.uf2         AMBE-3003, three channels, USB serial port
    xombe_stick_dv3k_ftdi.uf2      AMBE-3000R as an FTDI USB-3000
    xombe_stick_dv3003_ftdi.uf2    AMBE-3003 as an FTDI USB-3003

use the serial ports for:

 - codecserver (3k or 3003)
 - AMBEserver (3k only)
 - SerialDV (3k only)
 - anything else that takes a tty

for ambed, use the dv3003 ftdi build. this uses the D2XX library to access the
chip via libusb. to use it, `rmmod ftdi_sio` and install libftd2xx.

## version

the firmware's version is its USB interface string:

    $ cat /sys/bus/usb/devices/*/interface | grep xombe
    xombe-stick 0.1.0

for a UF2 run `picotool info FILE.uf2`.

## verifying a release

each release is a tag signed by x0rro (`x0rroRF@proton.me`), key fingerprint
`638C 5D53 640A CEFB 79B6 87AA 3E93 43F0 B819 4BFF`. the tag message has the
hashes of the release's files and of the four firmware builds:

    git tag -v v0.1.0

### reproducing a release

firmware builds are reproducible. rebuild a release on x86_64 in the debian
container that `release/Containerfile` starts from, with the source at `/src`:

    git clone -b v0.1.0 https://github.com/x0rro/xombe-stick && cd xombe-stick && mkdir out
    podman run --rm -v "$PWD:/src" -v "$PWD/out:/out" \
        docker.io/library/debian@sha256:2dc419c6d7fe3dad0a2f9fd469c34cc90b0f9ba642d8fde837fe3d7d34a0e7f1 \
        sh -c 'cd /src && release/deps.sh && git config --global safe.directory /src &&
               release/tarball.sh /out && release/bins.sh v0.1.0 /out /out/x0rro &&
               cd /out && sha256sum *.tar.xz *.buildinfo'

the hashes it prints match the tag's, and the UF2s are left in `out/x0rro`.

## building

deps (debian 13 / ubuntu 26.04):

    apt install cmake gcc-arm-none-eabi libnewlib-arm-none-eabi libstdc++-arm-none-eabi-newlib \
        build-essential git python3 libusb-1.0-0-dev pkg-config

then:

    cmake -B build
    cmake --build build -j --target firmware      # build/xombe_stick_dv*.uf2

this automatically fetches the Pico SDK (2.3.0) and xombe at pinned commits.
override with `-DPICO_SDK_PATH=DIR` and/or `-DXOMBE_GIT_REPOSITORY=URL`.

## patent note

patents covering methods used in this code may still be in force in some
jurisdictions. if that applies to you, get a license from the patent holder
before using it.

## copyright

copyright (c) 2026 x0rro

released under MIT license, see LICENSE

`pico_sdk_import.cmake` is copied from Pico SDK under the BSD-3 license.
