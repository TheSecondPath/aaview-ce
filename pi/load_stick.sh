#!/bin/sh
# Run on the Pi: unplug the stick, copy /tmp/load onto it, plug it back in.
# It takes the same lock as vjob, so only one stick job runs at a time. Whatever happens (a failed copy, a full
# image, Ctrl-C), the image is unmounted and the stick plugged back into the car on the way out.
set -e
IMG=${STICK_IMG:-/srv/stick.img}
MNT=/mnt/stick
LOCK_WAIT=${LOCK_WAIT:-600}

[ -f "$IMG" ] || { echo "load_stick.sh: no stick image at $IMG" >&2; exit 1; }
[ -d /tmp/load ] || { echo "load_stick.sh: nothing to copy: put the files in /tmp/load first" >&2; exit 1; }

ejected=0
mounted=0
cleanup() {
    if [ $mounted = 1 ]; then sudo umount "$MNT" 2>/dev/null || true; fi
    if [ $ejected = 1 ]; then
        sudo stick-gadget insert || echo "load_stick.sh: could not plug the stick back in" >&2
        sleep 3
        sudo stick-gadget status || true
    fi
}
trap cleanup EXIT
trap 'exit 130' INT TERM HUP

exec 9<"$IMG"
if ! flock -w "$LOCK_WAIT" 9; then
    echo "load_stick.sh: the stick has been busy for $LOCK_WAIT s (a vjob or another load_stick.sh)" >&2
    exit 3
fi

ejected=1
sudo stick-gadget eject
# CE notices the unplug within 1-3 s, so 5 s leaves a margin. If the image changes before CE lets go, CE later
# writes its stale FAT back.
sleep ${SETTLE:-5}
sudo mkdir -p "$MNT"
sudo mount -o loop,uid="$(id -u)",gid="$(id -g)" "$IMG" "$MNT"
mounted=1
echo "--- before:"; find "$MNT" -maxdepth 2 | sort
cp -r /tmp/load/. "$MNT"/
sync
echo "--- after:"; find "$MNT" -type f | sort
df -h "$MNT" | tail -1
