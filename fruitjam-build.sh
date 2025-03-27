#!/bin/sh
set -e

# Some configurations that actually work at the time I committed this:
# ./fruitjam-build.sh  -v         # vga resolution, no psram, 128KiB
# ./fruitjam-build.sh  -v -m448   # vga resolution, no psram,  448KiB
# ./fruitjam-build.sh  -m4096     # 512x342 resolution, psram, 4096KiB
# ./fruitjam-build.sh  -d disk.img  # specify disk image

DISP_WIDTH=512
DISP_HEIGHT=342
MEMSIZE=400
DISC_IMAGE=
CMAKE_ARGS=""
OVERCLOCK=0

while getopts "hovd:m:" o; do
    case "$o" in
    (o)
        OVERCLOCK=1
        ;;
    (v)
        DISP_WIDTH=640
        DISP_HEIGHT=480
        CMAKE_ARGS="-DUSE_VGA_RES=1"
        ;;
    (m)
        MEMSIZE=$OPTARG
        ;;
    (d)
        DISC_IMAGE=$OPTARG
        ;;
    (h|?)
        echo "Usage: $0 [-v] [-m KiB] [-d diskimage]"
        echo ""
        echo "   -v: Use framebuffer resolution 640x480 instead of 512x342"
        echo "   -m: Set memory size in KiB (over 400kB requires psram)"
        echo "   -d: Specify disc image to include"
        echo "   -o: Overclock to 264MHz (known to be incompatible with psram)"
        echo ""
        echo "PSRAM is automatically set depending on memory & framebuffer details"
        exit
        ;;
    esac
done

shift $((OPTIND-1))

TAG=fruitjam_${DISP_WIDTH}x${DISP_HEIGHT}_${MEMSIZE}k
PSRAM=$((MEMSIZE > 400))
if [ $PSRAM -ne 0 ] ; then
    if [ $OVERCLOCK -ne 0 ]; then
        echo "*** Overclock + PSRAM is known not to work. You have been warned."
    fi
    TAG=${TAG}_psram
    CMAKE_ARGS="$CMAKE_ARGS -DUSE_PSRAM=1"
fi

MIRROR_FRAMEBUFFER=$((USE_PSRAM || DISP_WIDTH != 640))
if [ "$MIRROR_FRAMEBUFFER" -eq 0 ]; then
    CMAKE_ARGS="$CMAKE_ARGS -DHSTX_CKP=12 -DHSTX_D0P=14 -DHSTX_D1P=16 -DHSTX_D2P=18 "
fi

# Append disk name to build directory if disk image is specified
if [ -n "$DISC_IMAGE" ] && [ -f "$DISC_IMAGE" ]; then
    # Extract filename without extension
    DISC_IMAGE=$(basename "$DISC_IMAGE" | sed 's/\.[^.]*$//')
    CMAKE_ARGS="$CMAKE_ARGS -DDISK_IMAGE=${DISC_IMAGE}"
    TAG=${TAG}_${DISC_IMAGE}
fi

if [ $OVERCLOCK -ne 0 ]; then
    TAG=${TAG}_overclock
fi

set -x
rm -rf build_${TAG}
cmake -S . -B build_${TAG} \
    -DPICO_SDK_PATH=../pico-sdk \
    -DPICOTOOL_FETCH_FROM_GIT_PATH="$(pwd)/picotool" \
    -DBOARD=adafruit_fruit_jam -DPICO_BOARD=adafruit_fruit_jam \
    -DMEMSIZE=${MEMSIZE} \
    -DUSE_HSTX=1 \
    -DSD_TX=35 -DSD_RX=36 -DSD_SCK=34 -DSD_CS=39 -DUSE_SD=1 \
    -DUART_TX=44 -DUART_RX=45 -DUART=0 \
    -DBOARD_FILE=boards/adafruit_fruit_jam.c \
    -DSD_MHZ=16 \
    -DOVERCLOCK=${OVERCLOCK} \
    ${CMAKE_ARGS} "$@"
make -C build_${TAG} -j$(nproc)
