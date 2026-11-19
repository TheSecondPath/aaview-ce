#!/bin/sh
# install.sh: set a Raspberry Pi up as the car's Android Auto receiver.
# For a Pi Zero 2 W with Raspberry Pi OS Lite (64-bit, trixie; NetworkManager is needed). Run on the Pi, from this folder:
#
#   sudo sh install.sh              the release build: the phone's access point (aa-ap), the receiver (aarecvd:
#                                   aarecvd.service has the phone's session, rmwork-aa its Bluetooth start-up)
#                                   and the three USB drives the car sees (stick-gadget). The Pi's Wi-Fi is then for
#                                   the phone only: after the reboot it is on NO network, so no SSH over Wi-Fi.
#   sudo sh install.sh --keep-wifi  the same, and the Pi keeps joining the Wi-Fi network it is on now whenever that
#                                   is in range (writes home_wifi=on and the network's name into veloaa.txt)
#   sudo sh install.sh --dev        the dev build: all of that with --keep-wifi, plus the web page (rmwork-web, vjob)
#                                   and the game-controller input service (rmwork-input), and log level debug
#
#   --force        replace installed unit files that differ from the ones here (the old ones are kept in
#                  /var/lib/veloaa/replaced)
#   --country XX   set the Wi-Fi country (two letters, e.g. US): Wi-Fi stays switched off until one is set
#
# The receiver is a program built on a PC (pi/aarecvd/build.sh arm64, with Docker): it is taken from
# aarecvd/out/aarecvd-arm64 under this folder, or aarecvd-arm64 next to this script. 64-bit Raspberry Pi OS only.
#
# It installs and enables; it starts and stops nothing. Reboot when it is done. Running it again is safe: programs
# are brought up to date, an existing veloaa.txt, passwords and unit files are left as they are (but for unit files an
# older version of this script installed, which are brought up to date).
#
# Settings afterwards: /boot/firmware/veloaa.txt (the "bootfs" drive when the SD card is in a PC). See README.md.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=${VELOAA_ROOT:-}           # for test_install.sh: install into this folder instead of /
PATH=$PATH:/usr/sbin:/sbin
DEV=0; KEEP=0; FORCE=0; COUNTRY=

usage() { sed -n '2,24p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-1}"; }
say() { echo "$*"; }
warn() { echo "WARNING: $*"; WARNINGS=$((WARNINGS + 1)); }
die() { echo "install.sh: $*" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }
WARNINGS=0

while [ $# -gt 0 ]; do
    case "$1" in
        --dev) DEV=1; KEEP=1; shift ;;
        --keep-wifi) KEEP=1; shift ;;
        --force) FORCE=1; shift ;;
        --country) [ $# -ge 2 ] || usage; COUNTRY=$2; shift 2 ;;
        -h|--help) usage 0 ;;
        *) usage ;;
    esac
done
[ -n "$ROOT" ] || [ "$(id -u)" = 0 ] || die "needs root: sudo sh $0 ..."

RELEASE_FILES="aa-ap stick-gadget veloaa.txt.example 60-veloaa-journald.conf
    veloaa-ramfiles.service stick-gadget.service aa-ap.service rmwork-aa.service aarecvd.service"
DEV_FILES="vjob rmwork_web.py rmwork_input.py rmwork-web.service rmwork-input.service"
for F in $RELEASE_FILES $([ $DEV = 1 ] && echo "$DEV_FILES"); do
    [ -f "$HERE/$F" ] || die "$F is not in $HERE: run this from a complete copy of pi"
done
# The receiver program. test_install.sh sets VELOAA_AARECVD to a build for the machine it runs on.
AARECVD=${VELOAA_AARECVD:-}
if [ -z "$AARECVD" ]; then
    for B in "$HERE/aarecvd/out/aarecvd-arm64" "$HERE/aarecvd-arm64"; do
        if [ -f "$B" ]; then AARECVD=$B; break; fi
    done
fi
[ -n "$AARECVD" ] || die "the receiver program is not here: build it on a PC with Docker (pi/aarecvd/build.sh arm64) and copy pi with aarecvd/out/aarecvd-arm64 in it, or put aarecvd-arm64 next to this script. Nothing was changed"
chmod +x "$AARECVD" 2>/dev/null || true
"$AARECVD" --show-settings --shell --settings /nonexistent > /dev/null 2>&1 ||
    die "$AARECVD does not run here ($(uname -m)): it is built for 64-bit Raspberry Pi OS (aarch64). Nothing was changed"
# settings <key> <file>: what veloaa.txt says, read the same way the services read it
settings() {
    "$AARECVD" --show-settings --shell --settings "$2" --loglevel-file /nonexistent 2>/dev/null |
        sed -n "s/^$1=//p" | head -n 1
}

BOOT=/boot/firmware
CONFIG=$BOOT/config.txt
SETTINGS=$BOOT/veloaa.txt
# The network wlan0 is on now, as NetworkManager has it saved (only its name is read).
current_ssid() {
    C=$(LC_ALL=C.UTF-8 nmcli -t -f UUID,DEVICE con show --active 2>/dev/null | sed -n 's/:wlan0$//p' | head -n 1)
    [ -n "$C" ] || return 0
    LC_ALL=C.UTF-8 nmcli --escape no -g 802-11-wireless.ssid con show "$C" 2>/dev/null || true
}
# With --keep-wifi and no veloaa.txt yet, find the network before anything is changed. Going on without one would
# leave home Wi-Fi off and the Pi unreachable after the reboot.
have nmcli || die "NetworkManager (nmcli) is not installed: this needs Raspberry Pi OS bookworm or later. Nothing was changed"
SSID=
if [ $KEEP = 1 ] && [ -d "$ROOT$BOOT" ] && [ ! -f "$ROOT$SETTINGS" ]; then
    SSID=$(current_ssid)
    [ -n "$SSID" ] || die "the Pi is on no Wi-Fi network right now, so there is none to keep (--keep-wifi, --dev). Nothing was changed. Connect it first (sudo nmcli device wifi connect <name> --ask), or write $SETTINGS yourself (veloaa.txt.example), and run this again"
fi

# put <mode> <file here, or a full path> <destination>: for programs and fixed configuration, which are always
# brought up to date
put() {
    case "$2" in /*) S=$2 ;; *) S=$HERE/$2 ;; esac
    if [ -f "$ROOT$3" ] && cmp -s "$S" "$ROOT$3"; then
        say "  as it was   $3"
    else
        install -D -m "$1" "$S" "$ROOT$3"
        say "  installed   $3"
    fi
}
# md5sums of unit files that earlier versions of this script installed. A unit on the Pi that matches one of these
# is ours, not somebody's own change, so it gets updated. When a unit file here changes, add the old one's checksum.
OLD_UNITS="
4ab23718e7270123e7f10b0a001e6613 rmwork-aa.service
83e8c6af212d2553332bfb824cd50202 aa-ap.service
f93c3451120951c17d9d4dddbc1c1a6c rmwork-aa.service
62545a8e0b49fe705b8a54a952531ddc rmwork-input.service
b32a0f72f0a380d7381d5685961012fc rmwork-web.service
a6b0a0bb967acb688f06eef9e23b999a veloaa-ramfiles.service
40cb1d533c5ddc9765ce44347ce7ee77 rmwork-aa.service
088dde61c0510827d5ce683eb809c848 aarecvd.service
c2eb51f1965881767c47cfc5b605c02c stick-gadget.service
"
# old_unit <name> <file>: true if the installed file is one of OLD_UNITS
old_unit() {
    SUM=$(md5sum < "$2" | cut -c1-32)
    echo "$OLD_UNITS" | grep -qx "$SUM $1"
}
# unit <name>: install a unit file. An installed one that differs is taken to be somebody's change and kept,
# unless --force is given or it is an older one of ours (OLD_UNITS).
unit() {
    U=/etc/systemd/system/$1
    if [ ! -f "$ROOT$U" ]; then
        install -D -m 644 "$HERE/$1" "$ROOT$U"
        say "  installed   $U"
    elif cmp -s "$HERE/$1" "$ROOT$U"; then
        say "  as it was   $U"
    elif [ $FORCE = 1 ] || old_unit "$1" "$ROOT$U"; then
        install -D -m 644 "$ROOT$U" "$ROOT/var/lib/veloaa/replaced/$1"
        install -m 644 "$HERE/$1" "$ROOT$U"
        if [ $FORCE = 1 ]; then
            say "  replaced    $U (the old one: /var/lib/veloaa/replaced/$1)"
        else
            say "  updated     $U (an older version of this script's; the old one: /var/lib/veloaa/replaced/$1)"
        fi
    else
        say "  KEPT        $U: it differs from the one here (--force replaces it)"
    fi
}
# where <unit> <script>: the path an installed unit already runs the script from, else /usr/local/bin/<script>
where() {
    P=$(sed -n "s|^ExecStart=.*[[:space:]]\(/[^[:space:]]*/$2\).*|\1|p" "$ROOT/etc/systemd/system/$1" 2>/dev/null | head -n 1)
    echo "${P:-/usr/local/bin/$2}"
}

say "== packages"
NEED=
have mkfs.vfat || NEED="$NEED dosfstools"
have iw || NEED="$NEED iw"
have nft || NEED="$NEED nftables"
have rfkill || NEED="$NEED rfkill"
have bluetoothctl || NEED="$NEED bluez"
[ $DEV = 0 ] || have python3 || NEED="$NEED python3"      # the dev build's web page and input service
if [ -n "$NEED" ]; then
    say "  installing:$NEED"
    apt-get update -qq || die "apt-get update failed: the Pi needs the internet for the missing packages ($NEED )"
    # shellcheck disable=SC2086
    apt-get install -y -qq $NEED || die "could not install:$NEED"
else
    say "  all there"
fi
[ $DEV = 0 ] || have rpicam-vid || warn "rpicam-vid is not installed: the web page will have no camera picture (sudo apt-get install rpicam-apps-lite)"

say "== USB device mode"
if [ ! -f "$ROOT$CONFIG" ]; then
    warn "no $CONFIG: add 'dtoverlay=dwc2,dr_mode=peripheral' under [all] in the Pi's config.txt yourself"
elif grep -q '^dtoverlay=dwc2,dr_mode=peripheral' "$ROOT$CONFIG"; then
    say "  as it was   $CONFIG (dwc2 overlay)"
else
    printf '\n# veloster-aa: the USB port is a device (the car is the host)\n[all]\ndtoverlay=dwc2,dr_mode=peripheral\n' >> "$ROOT$CONFIG"
    say "  added       dtoverlay=dwc2,dr_mode=peripheral to $CONFIG"
fi
T=$(mktemp)
printf 'dwc2\nlibcomposite\n' > "$T"
put 644 "$T" /etc/modules-load.d/usb-gadget.conf
# Without roamoff the chip's firmware roams the home link between access points, the phone's AP has to follow it,
# and Bluetooth on the same chip starves (see README.md). Harmless with home Wi-Fi off.
printf 'options brcmfmac roamoff=1\n' > "$T"
if [ -f "$ROOT/etc/modprobe.d/brcmfmac-roamoff.conf" ]; then
    say "  as it was   /etc/modprobe.d/brcmfmac-roamoff.conf"
else
    install -D -m 644 "$T" "$ROOT/etc/modprobe.d/brcmfmac-roamoff.conf"
    say "  installed   /etc/modprobe.d/brcmfmac-roamoff.conf"
fi
rm -f "$T"

say "== programs"
put 755 stick-gadget /usr/local/sbin/stick-gadget
put 755 aa-ap /usr/local/sbin/aa-ap
put 755 "$AARECVD" /usr/local/lib/veloaa/aarecvd
put 644 60-veloaa-journald.conf /etc/systemd/journald.conf.d/60-veloaa.conf
if [ $DEV = 1 ]; then
    put 755 vjob /usr/local/bin/vjob
    put 755 rmwork_web.py "$(where rmwork-web.service rmwork_web.py)"
    put 755 rmwork_input.py "$(where rmwork-input.service rmwork_input.py)"
    mkdir -p "$ROOT/mnt/stick"
fi

say "== services"
UNITS="veloaa-ramfiles.service stick-gadget.service aa-ap.service rmwork-aa.service aarecvd.service"
[ $DEV = 0 ] || UNITS="$UNITS rmwork-web.service rmwork-input.service"
for UNIT in $UNITS; do
    unit "$UNIT"
done
if [ $DEV = 0 ] && { [ -f "$ROOT/etc/systemd/system/rmwork-web.service" ] ||
                     [ -f "$ROOT/etc/systemd/system/rmwork-input.service" ]; }; then
    say "  (this Pi has the dev build's rmwork-web / rmwork-input: left as they are. --dev keeps them up to date)"
fi
systemctl daemon-reload
# shellcheck disable=SC2086
systemctl enable $UNITS >/dev/null 2>&1 || warn "systemctl enable failed for one of: $UNITS"
systemctl enable bluetooth.service >/dev/null 2>&1 || warn "could not enable bluetooth.service"

say "== settings"
[ -d "$ROOT/etc/aarecv" ] || install -d -m 700 "$ROOT/etc/aarecv"
CR=$(printf '\r')
if [ ! -d "$ROOT$BOOT" ]; then
    warn "no $BOOT on this system: no veloaa.txt written, so the defaults apply (home Wi-Fi off, log level info)"
elif [ -f "$ROOT$SETTINGS" ]; then
    say "  as it was   $SETTINGS (your settings)"
else
    # Copy the example line by line, with CRLF line ends so that any version of Notepad shows it properly.
    while IFS= read -r L || [ -n "$L" ]; do
        L=${L%"$CR"}
        case "$L" in
            home_wifi=*) if [ -n "$SSID" ]; then L="home_wifi=on"; fi ;;
            home_wifi_ssid=*) if [ -n "$SSID" ]; then L="home_wifi_ssid=\"$SSID\""; fi ;;
            log_level=*) if [ $DEV = 1 ]; then L="#$L   (dev build: left out, so /etc/aarecv/loglevel decides)"; fi ;;
        esac
        printf '%s\r\n' "$L"
    done < "$HERE/veloaa.txt.example" > "$ROOT$SETTINGS"
    say "  installed   $SETTINGS"
    if [ -n "$SSID" ]; then
        # Read the name back the way the services will. A name the file cannot hold must not cost the Pi its network.
        BACK=$(settings home_wifi_ssid "$ROOT$SETTINGS")
        [ "$BACK" = "$SSID" ] ||
            warn "the network's name did not survive being written to $SETTINGS: check home_wifi_ssid there before you reboot"
    fi
fi
if [ $DEV = 1 ]; then
    if [ ! -f "$ROOT/etc/aarecv/loglevel" ]; then
        echo debug > "$ROOT/etc/aarecv/loglevel"
        say "  installed   /etc/aarecv/loglevel (debug)"
    fi
    if [ ! -s "$ROOT/etc/rmwork-web.pass" ]; then
        # a random 16-character password (base64 of urandom, without / + =)
        (umask 077; head -c 48 /dev/urandom | base64 -w 0 | tr -d '/+=' | head -c 16 > "$ROOT/etc/rmwork-web.pass")
        say "  installed   /etc/rmwork-web.pass (a new random password for the web page: sudo cat /etc/rmwork-web.pass)"
    fi
fi

say "== Wi-Fi"
if [ -n "$COUNTRY" ]; then
    if have raspi-config; then
        raspi-config nonint do_wifi_country "$COUNTRY" && say "  Wi-Fi country set to $COUNTRY" ||
            warn "raspi-config could not set the Wi-Fi country $COUNTRY"
    else
        warn "no raspi-config here: set the Wi-Fi country $COUNTRY yourself"
    fi
fi
if rfkill list wifi 2>/dev/null | grep -q 'Soft blocked: yes'; then
    warn "Wi-Fi is switched off (rfkill), which is how Raspberry Pi OS leaves it until a Wi-Fi country is set. The phone's access point cannot start like that: run this again with --country XX"
else
    say "  not blocked"
fi

say "== still to do"
N=0
todo() { N=$((N + 1)); say "  $N. $*"; }
if [ ! -f "$ROOT/srv/stick.img" ]; then
    if [ $DEV = 1 ]; then
        todo "The stick image /srv/stick.img is not there: see \"The stick\" in README.md (setup_stick.sh, load_stick.sh)."
    else
        todo "The stick image /srv/stick.img is not there: sudo sh make_release_stick.sh <folder with the built programs>"
    fi
fi
todo "Reboot: sudo reboot"
todo "Pair the phone once: in the car, hold the Android Auto icon on the head unit still for 3 s (it turns purple),"
say "     then pair with the Pi from the phone's Bluetooth settings within 3 minutes. When the code on the icon"
say "     matches the phone's, confirm on the phone and tap the icon. Over SSH instead: sudo systemctl stop rmwork-aa; sudo /usr/local/lib/veloaa/aarecvd --bt-only --pair"
say "     (then Ctrl-C and sudo systemctl start rmwork-aa)"

# Say plainly what the services will read, since it decides whether the Pi is reachable after the reboot.
HOMEWIFI=off
if [ -f "$ROOT$SETTINGS" ]; then
    HOMEWIFI=$(settings home_wifi "$ROOT$SETTINGS")
fi
say ""
if [ "$HOMEWIFI" = on ]; then
    say "Home Wi-Fi: ON. After the reboot the Pi joins only the network named in $SETTINGS, when in range."
else
    say "Home Wi-Fi: OFF. After the reboot the Pi's Wi-Fi is the phone's access point only: it joins NO network,"
    say "so SSH over Wi-Fi stops working. To get it back, put the SD card in a PC and set home_wifi=on and"
    say "home_wifi_ssid=<your network's name> in veloaa.txt on the bootfs drive (or run this with --keep-wifi now)."
fi
if [ $KEEP = 1 ] && [ "$HOMEWIFI" != on ]; then
    if [ -f "$ROOT$SETTINGS" ]; then
        warn "you asked to keep the Wi-Fi network, but $SETTINGS was there already and says otherwise: it was not changed. Edit it before you reboot"
    else
        warn "you asked to keep the Wi-Fi network, but there is no $SETTINGS to say so in: home Wi-Fi will be OFF after the reboot"
    fi
fi
[ $WARNINGS = 0 ] || say "$WARNINGS warning(s) above."
