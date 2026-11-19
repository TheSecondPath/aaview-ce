#!/bin/sh
# make_release_stick.sh: the release build's stick, the flash drive the Pi shows the car.
#
#   sudo sh make_release_stick.sh BIN            write /srv/stick.img (2 GB FAT32, label RMWORK) and plug it in
#   sudo sh make_release_stick.sh --replace BIN  the same when there is an image already: that one is kept as
#                                                /srv/stick.img.old
#   sh make_release_stick.sh --dir OUT BIN       only lay the files out in the folder OUT (no root needed), to look
#                                                at them or to copy onto a real USB stick
#
# BIN is where the head-unit programs are: a folder holding aaview.exe, isvagent.exe and MatMessage.dll, or a
# checkout of this repo that has been built (apps\aaview\build.bat, apps\isvagent\build.bat and
# apps\matmsg\build.bat, on Windows).
#
# What goes on the stick:
#   isv/launch.txt            tells the car's ISV launcher to start \MD\aaview\aaview.exe, with bar=brief: the
#                             launcher's bar goes away 30 s after the start (on the dev stick it stays until tapped)
#   aaview/aaview.exe         writes its log, aaview_log.txt, next to itself
#   ISV_install/ISV/          ISVAgentKronos.exe, ISVAgent.exe and MatMessage.dll, to copy to \Storage Card by hand
#   README_VELOAA.txt
# Unlike the dev stick it has no vagent, autostart.txt or jobs. install.sh likewise leaves out the Pi's side of
# those: the web page, vjob and the input service.
set -eu

IMG=${STICK_IMG:-/srv/stick.img}
SIZE=${STICK_SIZE:-2G}
GADGET=${STICK_GADGET:-/usr/local/sbin/stick-gadget}
GDIR=${STICK_GADGET_DIR:-/sys/kernel/config/usb_gadget/stick}
SETTLE=${SETTLE:-5}    # CE notices an unplug within 1-3 s. If the image changes before CE lets go, CE writes its old FAT back

usage() {
    sed -n '2,10p' "$0" | sed 's/^# \{0,1\}//'
    exit "${1:-1}"
}
die() { echo "make_release_stick: $*" >&2; exit 1; }

OUT=; REPLACE=0; BIN=
while [ $# -gt 0 ]; do
    case "$1" in
        --dir) [ $# -ge 2 ] || usage; OUT=$2; shift 2 ;;
        --replace) REPLACE=1; shift ;;
        -h|--help) usage 0 ;;
        -*) usage ;;
        *) [ -z "$BIN" ] || usage; BIN=$1; shift ;;
    esac
done
[ -n "$BIN" ] || usage
[ -d "$BIN" ] || die "$BIN is not a folder"

# find_bin <file name> <its path in a built checkout>
find_bin() {
    for F in "$BIN/$1" "$BIN/$2"; do
        [ -f "$F" ] && { echo "$F"; return 0; }
    done
    return 1
}
MISSING=
AAVIEW=$(find_bin aaview.exe apps/aaview/out/aaview.exe) || MISSING="$MISSING aaview.exe"
ISVAGENT=$(find_bin isvagent.exe apps/isvagent/out/isvagent.exe) || MISSING="$MISSING isvagent.exe"
MATMSG=$(find_bin MatMessage.dll apps/matmsg/out/MatMessage.dll) || MISSING="$MISSING MatMessage.dll"
[ -z "$MISSING" ] || die "not found in $BIN:$MISSING (they are built on Windows: see the top of this script)"

# lay_out <folder>: write the stick's files into the folder
lay_out() {
    mkdir -p "$1/isv" "$1/aaview" "$1/ISV_install/ISV"
    printf '%s\r\n' '\MD\aaview\aaview.exe' 'bar=brief' > "$1/isv/launch.txt"
    cp "$AAVIEW" "$1/aaview/aaview.exe"
    cp "$ISVAGENT" "$1/ISV_install/ISV/ISVAgentKronos.exe"     # the name kronos looks for
    cp "$ISVAGENT" "$1/ISV_install/ISV/ISVAgent.exe"           # the name vulcan looks for. Only one copy runs
    cp "$MATMSG" "$1/ISV_install/ISV/MatMessage.dll"
    sed 's/$/\r/' > "$1/README_VELOAA.txt" <<'EOF'
Veloster Android Auto receiver: the release stick
=================================================

The Pi shows this to the car's head unit as a USB flash drive. The head unit calls it \MD.

  isv\launch.txt         the program the head unit starts when it boots (\MD\aaview\aaview.exe), and
                         bar=brief: the launcher's bar at the top left goes away 30 s after the start
  aaview\aaview.exe      Android Auto's screen on the head unit. Its log, aaview_log.txt, appears next to it.
  ISV_install\ISV\       the launcher, to be copied onto the head unit once (below)

Once, on the head unit
----------------------
1. With the Pi plugged into the car's USB port, open Explorer Mode on the head unit and copy the folder
   \MD\ISV_install\ISV into \Storage Card, so that \Storage Card\ISV\ISVAgentKronos.exe exists.
2. Test Mode (hold SETUP, tap the corners upper left, lower left, upper right, lower right; code 95889588):
   switch "ISV" on.
3. Restart the head unit. A cyan strip "ISV agent running" shows at the bottom left for 30 seconds, and the
   Android Auto icon appears at the top right: grey with no Pi, blue while it waits for the phone, green with the
   phone connected.

To undo: Test Mode, "ISV" off, and delete \Storage Card\ISV. Nothing else on the head unit is changed.
EOF
}

if [ -n "$OUT" ]; then
    [ ! -e "$OUT" ] || [ -z "$(ls -A "$OUT" 2>/dev/null)" ] || die "$OUT is not empty"
    lay_out "$OUT"
    echo "release stick files in $OUT:"
    (cd "$OUT" && find . -type f | sort | sed 's/^\.\//  /')
    exit 0
fi

[ "$(id -u)" = 0 ] || die "writing $IMG needs root: sudo sh $0 ..."
command -v mkfs.vfat >/dev/null 2>&1 || die "mkfs.vfat not found: sudo apt-get install dosfstools"
if [ -e "$IMG" ]; then
    [ $REPLACE = 1 ] || die "$IMG is there already (on a dev Pi it holds vagent's jobs and the logs). --replace keeps it as $IMG.old and writes a new one"
    [ ! -e "$IMG.old" ] || die "$IMG.old is there too, from an earlier --replace: move it away or delete it first"
fi

# Take the same lock as vjob and load_stick.sh, so this never runs while one of them has the image mounted.
if [ -e "$IMG" ]; then
    exec 9<"$IMG"
    flock -w "${LOCK_WAIT:-600}" 9 || die "$IMG has been busy for ${LOCK_WAIT:-600} s (a vjob or load_stick.sh is using it)"
fi

STAGE=$(mktemp -d)
MNT=$(mktemp -d)
EJECTED=0; MOVED=0; DONE=0
# However this ends, the car is not left with half a stick. If the new image was not finished, the old one is put
# back (or the unfinished one removed), and a stick that was unplugged here is plugged in again.
cleanup() {
    trap - EXIT
    mountpoint -q "$MNT" 2>/dev/null && umount "$MNT"
    rmdir "$MNT" 2>/dev/null || true
    rm -rf "$STAGE"
    [ $DONE = 0 ] || return 0
    if [ $MOVED = 1 ]; then
        rm -f "$IMG"
        mv "$IMG.old" "$IMG" && echo "make_release_stick: it failed: the old image is back at $IMG" >&2
    elif [ $MOVED = 2 ]; then
        rm -f "$IMG"
        echo "make_release_stick: it failed: the unfinished $IMG was removed" >&2
    fi
    if [ $EJECTED = 1 ] && [ -e "$IMG" ]; then
        "$GADGET" insert && echo "make_release_stick: the stick is plugged in again" >&2
    fi
}
trap cleanup EXIT
trap 'exit 1' INT TERM
lay_out "$STAGE"

# Unplug first, and give CE time to let go: the head unit may be using the old image.
if [ -d "$GDIR" ]; then
    echo "unplugging the stick from the car ($SETTLE s)"
    "$GADGET" eject
    EJECTED=1
    sleep "$SETTLE"
fi
if [ -e "$IMG" ]; then
    mv "$IMG" "$IMG.old"
    MOVED=1
    echo "the old image is now $IMG.old"
else
    MOVED=2
fi
truncate -s "$SIZE" "$IMG"
mkfs.vfat -F 32 -n RMWORK "$IMG" >/dev/null
mount -o loop "$IMG" "$MNT"
cp -r "$STAGE"/. "$MNT"/
sync
echo "on the new stick:"
(cd "$MNT" && find . -type f | sort | sed 's/^\.\//  /')
umount "$MNT"
DONE=1
if [ $EJECTED = 1 ]; then
    "$GADGET" insert
    echo "plugged in again"
else
    echo "the USB gadget is not up: it shows the stick to the car after a reboot (stick-gadget.service)"
fi
