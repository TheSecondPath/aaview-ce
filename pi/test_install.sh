#!/bin/sh
# test_install.sh: runs install.sh into a pretend root folder (VELOAA_ROOT) and make_release_stick.sh against
# stand-ins for nmcli, apt-get, systemctl, rfkill, raspi-config, id, mkfs.vfat, mount, umount and stick-gadget, and
# checks what they wrote, said and asked for. It touches nothing real and needs no root. Run it on Linux from
# pi, in aarecvd's amd64 build image (aarecvd/build.sh shtest): sh test_install.sh
# The receiver program is VELOAA_AARECVD, else aarecvd/out/aarecvd-amd64.
# Each case gets its own folder: root/ (the pretend Pi), calls (what the stand-ins were asked), out (what was said),
# and the pretend state as files: active (uuid:device), ssid (the active network's name), blocked (rfkill), uid.
HERE=$(cd "$(dirname "$0")" && pwd)
AARECVD=${VELOAA_AARECVD:-$HERE/aarecvd/out/aarecvd-amd64}
"$AARECVD" --show-settings --shell --settings /nonexistent > /dev/null 2>&1 ||
    { echo "test_install.sh: cannot run $AARECVD (build it: aarecvd/build.sh amd64; run this in its image)"; exit 2; }
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
FAILED=0
CR=$(printf '\r')

mkdir "$T/bin"
cat > "$T/bin/fake" <<'EOF'
#!/bin/sh
ME=$(basename "$0")
echo "$ME $*" >> "$FAKE/calls"
case "$ME" in
nmcli)
    case "$*" in
    "-t -f UUID,DEVICE con show --active") cat "$FAKE/active" 2>/dev/null ;;
    "--escape no -g 802-11-wireless.ssid con show u-home") cat "$FAKE/ssid" 2>/dev/null; echo ;;
    esac ;;
rfkill)
    printf '0: phy0: Wireless LAN\n\tSoft blocked: %s\n\tHard blocked: no\n' "$([ -f "$FAKE/blocked" ] && echo yes || echo no)" ;;
id) [ "$1" = -u ] && cat "$FAKE/uid" ;;
mountpoint) exit 1 ;;
mkfs.vfat) [ -f "$FAKE/mkfs_fails" ] && exit 1 ;;
esac
exit 0
EOF
chmod +x "$T/bin/fake"
for c in nmcli apt-get systemctl rfkill raspi-config iw nft bluetoothctl mkfs.vfat id mount umount mountpoint \
         stick-gadget; do
    cp "$T/bin/fake" "$T/bin/$c"
done

# new_case <name>: a fresh pretend Pi, on the Wi-Fi network "My Net", with a boot partition and nothing installed
new_case() {
    CASE=$1
    FAKE="$T/$CASE"
    R="$FAKE/root"
    mkdir -p "$R/boot/firmware" "$FAKE/tmp"
    printf '# config.txt\ndtparam=audio=on\n\n[cm5]\ndtoverlay=dwc2,dr_mode=host\n\n[all]\n' > "$R/boot/firmware/config.txt"
    printf 'u-eth:eth0\nu-home:wlan0\n' > "$FAKE/active"
    printf 'My Net' > "$FAKE/ssid"
    echo 1000 > "$FAKE/uid"
    : > "$FAKE/calls"
}
# inst [options]: run install.sh into the pretend root
inst() {
    env PATH="$T/bin:$PATH" FAKE="$FAKE" VELOAA_ROOT="$R" VELOAA_AARECVD="${INST_AARECVD-$AARECVD}" \
        sh "${INSTALL:-$HERE/install.sh}" "$@" > "$FAKE/out" 2>&1
    echo "exit $?" >> "$FAKE/out"
}
# stick [options]: run make_release_stick.sh with a pretend image, gadget and mount
stick() {
    env PATH="$T/bin:$PATH" FAKE="$FAKE" TMPDIR="$FAKE/tmp" STICK_IMG="$FAKE/stick.img" STICK_SIZE=1M SETTLE=0 \
        STICK_GADGET="$T/bin/stick-gadget" STICK_GADGET_DIR="$FAKE/gadget" \
        sh "$HERE/make_release_stick.sh" "$@" > "$FAKE/out" 2>&1
    echo "exit $?" >> "$FAKE/out"
}

ok() { echo "  ok    $CASE: $1"; }
bad() { echo "  FAIL  $CASE: $1"; FAILED=1; sed 's/^/        | /' "$FAKE/out"; sed 's/^/        > /' "$FAKE/calls"; }
check() { D=$1; shift; if "$@"; then ok "$D"; else bad "$D"; fi; }
called() { if grep -Eq -- "$1" "$FAKE/calls"; then ok "did: $1"; else bad "should have done: $1"; fi; }
not_called() { if grep -Eq -- "$1" "$FAKE/calls"; then bad "should not have done: $1"; else ok "did not: $1"; fi; }
said() { if grep -Eq -- "$1" "$FAKE/out"; then ok "said: $1"; else bad "should have said: $1"; fi; }
not_said() { if grep -Eq -- "$1" "$FAKE/out"; then bad "should not have said: $1"; else ok "did not say: $1"; fi; }
there() { if [ -e "$R$1" ]; then ok "there: $1"; else bad "missing: $1"; fi; }
not_there() { if [ -e "$R$1" ]; then bad "should not be there: $1"; else ok "not there: $1"; fi; }
has() { if grep -Eq -- "$2" "$R$1" 2>/dev/null; then ok "$1 has: $2"; else bad "$1 should have: $2"; fi; }
has_not() { if grep -Eq -- "$2" "$R$1" 2>/dev/null; then bad "$1 should not have: $2"; else ok "$1 has no: $2"; fi; }
# before <a> <b>: check that the first call matching a came before the first one matching b
before() {
    A=$(grep -En -- "$1" "$FAKE/calls" | head -n 1 | cut -d: -f1)
    B=$(grep -En -- "$2" "$FAKE/calls" | head -n 1 | cut -d: -f1)
    if [ -n "$A" ] && [ -n "$B" ] && [ "$A" -lt "$B" ]; then ok "$1 before $2"; else bad "$1 should come before $2"; fi
}
# setting <key>: what the services would read from the pretend root's veloaa.txt
setting() {
    "$AARECVD" --show-settings --shell --settings "$R/boot/firmware/veloaa.txt" --loglevel-file "$R/etc/aarecv/loglevel" |
        sed -n "s/^$1=//p" | head -n 1
}
all_crlf() { [ -s "$1" ] && [ "$(grep -c "$CR\$" "$1")" = "$(grep -c '' "$1")" ]; }

echo "== release build on a fresh Pi"
new_case release
inst
said "^exit 0$"
for F in /usr/local/sbin/aa-ap /usr/local/sbin/stick-gadget /usr/local/lib/veloaa/aarecvd \
         /etc/systemd/system/aarecvd.service \
         /etc/systemd/journald.conf.d/60-veloaa.conf /etc/modules-load.d/usb-gadget.conf \
         /etc/modprobe.d/brcmfmac-roamoff.conf /etc/systemd/system/veloaa-ramfiles.service \
         /etc/systemd/system/stick-gadget.service /etc/systemd/system/aa-ap.service \
         /etc/systemd/system/veloaa-aa.service /etc/aarecv /boot/firmware/veloaa.txt; do
    there "$F"
done
for F in /usr/local/bin/vjob /usr/local/bin/veloaa_web.py /usr/local/bin/veloaa_input.py /etc/veloaa-web.pass \
         /etc/systemd/system/veloaa-web.service /etc/systemd/system/veloaa-input.service /etc/aarecv/loglevel; do
    not_there "$F"
done
check "aa-ap can be run" [ -x "$R/usr/local/sbin/aa-ap" ]
check "aa-ap is the one here" cmp -s "$HERE/aa-ap" "$R/usr/local/sbin/aa-ap"
check "aarecvd is the program given" cmp -s "$AARECVD" "$R/usr/local/lib/veloaa/aarecvd"
check "aarecvd can be run" [ -x "$R/usr/local/lib/veloaa/aarecvd" ]
check "veloaa-aa runs the Bluetooth start-up from where it was put" grep -q "^ExecStart=/usr/local/lib/veloaa/aarecvd --bt-only$" "$R/etc/systemd/system/veloaa-aa.service"
check "aarecvd.service runs the session from where it was put" grep -q "^ExecStart=/usr/local/lib/veloaa/aarecvd \$AARECVD_ARGS$" "$R/etc/systemd/system/aarecvd.service"
has /boot/firmware/config.txt "^dtoverlay=dwc2,dr_mode=peripheral$"
check "the overlay is the file's last line, under [all]" [ "$(tail -n 2 "$R/boot/firmware/config.txt" | head -n 1)" = "[all]" ]
has /boot/firmware/config.txt "^dtoverlay=dwc2,dr_mode=host$"         # the other model's line is left alone
check "veloaa.txt has CRLF line ends" all_crlf "$R/boot/firmware/veloaa.txt"
check "home Wi-Fi reads off" [ "$(setting home_wifi)" = off ]
check "log level reads info" [ "$(setting log_level)" = info ]
called "^systemctl daemon-reload$"
called "^systemctl enable veloaa-ramfiles.service stick-gadget.service aa-ap.service veloaa-aa.service aarecvd.service$"
called "^systemctl enable bluetooth.service$"
not_called "^systemctl (start|stop|restart|reboot)"
not_called "veloaa-web|veloaa-input"
not_called "^raspi-config"
said "^Home Wi-Fi: OFF"
said "joins NO network"
said "aarecvd --bt-only --pair"
said "make_release_stick.sh"
said "Reboot: sudo reboot"
not_said "WARNING"

echo "== run again: nothing changes"
cp "$R/boot/firmware/veloaa.txt" "$FAKE/before.txt"; cp "$R/boot/firmware/config.txt" "$FAKE/before.cfg"
inst
said "^exit 0$"
not_said "  (installed|replaced|added|KEPT) "
check "veloaa.txt untouched" cmp -s "$FAKE/before.txt" "$R/boot/firmware/veloaa.txt"
check "config.txt untouched" cmp -s "$FAKE/before.cfg" "$R/boot/firmware/config.txt"

echo "== a veloaa.txt the user edited survives an update, and a changed program is brought up to date"
printf 'home_wifi=on\r\nhome_wifi_ssid=Their Net\r\nlog_level=warning\r\n' > "$R/boot/firmware/veloaa.txt"
echo "# old" > "$R/usr/local/sbin/aa-ap"
inst
check "their network name is still there" [ "$(setting home_wifi_ssid)" = "Their Net" ]
check "aa-ap is the new one again" cmp -s "$HERE/aa-ap" "$R/usr/local/sbin/aa-ap"
said "^Home Wi-Fi: ON"

echo "== --keep-wifi: the network the Pi is on goes into veloaa.txt"
new_case keep
inst --keep-wifi
said "^exit 0$"
has /boot/firmware/veloaa.txt "^home_wifi=on$CR\$"
has /boot/firmware/veloaa.txt "^home_wifi_ssid=\"My Net\"$CR\$"
check "home Wi-Fi reads on" [ "$(setting home_wifi)" = on ]
check "the name reads back" [ "$(setting home_wifi_ssid)" = "My Net" ]
check "veloaa.txt has CRLF line ends" all_crlf "$R/boot/firmware/veloaa.txt"
said "^Home Wi-Fi: ON"
not_said "WARNING"
not_there /etc/systemd/system/veloaa-web.service

echo "== --keep-wifi: names with quotes, a #, an = and spaces at the ends read back exactly"
for NAME in 'Bob'"'"'s "Net" #1' ' led = trail ' 'caf\303\251 5G'; do
    new_case "keep_odd"
    printf "$NAME" > "$FAKE/ssid"
    inst --keep-wifi
    check "reads back: $(cat "$FAKE/ssid")" [ "$(setting home_wifi_ssid)" = "$(cat "$FAKE/ssid")" ]
    not_said "WARNING"
    rm -rf "$FAKE"
done

echo "== --keep-wifi with the Pi on no network: stops before anything is changed"
new_case keep_none
printf 'u-eth:eth0\n' > "$FAKE/active"
inst --keep-wifi
said "^exit 1$"
said "on no Wi-Fi network"
said "Nothing was changed"
not_there /usr
not_there /etc
not_there /boot/firmware/veloaa.txt
has_not /boot/firmware/config.txt "peripheral"
not_called "^(systemctl|apt-get)"

echo "== --keep-wifi when veloaa.txt is there and says off: left alone, and said"
new_case keep_exists
printf 'home_wifi=off\n' > "$R/boot/firmware/veloaa.txt"
inst --keep-wifi
said "^exit 0$"
said "WARNING: you asked to keep the Wi-Fi network, but .* was there already"
said "^Home Wi-Fi: OFF"
check "veloaa.txt untouched" [ "$(cat "$R/boot/firmware/veloaa.txt")" = "home_wifi=off" ]

echo "== no boot partition: said, twice with --keep-wifi"
new_case noboot
rm -rf "$R/boot"
inst --keep-wifi
said "^exit 0$"
said "WARNING: no /boot/firmware/config.txt"
said "WARNING: no /boot/firmware on this system"
said "WARNING: you asked to keep the Wi-Fi network, but there is no"
said "^Home Wi-Fi: OFF"

echo "== dev build"
new_case dev
inst --dev
said "^exit 0$"
for F in /usr/local/bin/vjob /usr/local/bin/veloaa_web.py /usr/local/bin/veloaa_input.py /etc/veloaa-web.pass \
         /etc/systemd/system/veloaa-web.service /etc/systemd/system/veloaa-input.service /etc/aarecv/loglevel \
         /usr/local/sbin/aa-ap /usr/local/lib/veloaa/aarecvd /mnt/stick; do
    there "$F"
done
called "^systemctl enable veloaa-ramfiles.service stick-gadget.service aa-ap.service veloaa-aa.service aarecvd.service veloaa-web.service veloaa-input.service$"
check "home Wi-Fi reads on" [ "$(setting home_wifi)" = on ]
check "the name reads back" [ "$(setting home_wifi_ssid)" = "My Net" ]
has /boot/firmware/veloaa.txt "^#log_level=info"
check "log level reads debug, from /etc/aarecv/loglevel" [ "$(setting log_level)" = debug ]
check "the web password is 16 characters" [ "$(wc -c < "$R/etc/veloaa-web.pass")" = 16 ]
check "the web password is for root only" [ "$(stat -c %a "$R/etc/veloaa-web.pass")" = 600 ]
check "the web password is not shown" sh -c '! grep -qF "$(cat "$1")" "$2"' x "$R/etc/veloaa-web.pass" "$FAKE/out"
said "setup_stick.sh"
cp "$R/etc/veloaa-web.pass" "$FAKE/pass"
echo error > "$R/etc/aarecv/loglevel"
inst --dev
check "the web password is kept" cmp -s "$FAKE/pass" "$R/etc/veloaa-web.pass"
check "the log level file is kept" [ "$(cat "$R/etc/aarecv/loglevel")" = error ]

echo "== dev build on a Pi whose units run the scripts from somewhere else: units kept, scripts updated there"
new_case dev_paths
mkdir -p "$R/etc/systemd/system"
printf '[Service]\nUser=root\nExecStart=/usr/bin/python3 -u /home/pi/veloaa_web.py\n' > "$R/etc/systemd/system/veloaa-web.service"
inst --dev
said "KEPT        /etc/systemd/system/veloaa-web.service"
there /home/pi/veloaa_web.py
not_there /usr/local/bin/veloaa_web.py
there /usr/local/bin/veloaa_input.py
has /etc/systemd/system/veloaa-web.service "/home/pi/veloaa_web.py"

echo "== a unit that differs is kept; --force replaces it and keeps the old one"
new_case units
mkdir -p "$R/etc/systemd/system"
echo "# mine" > "$R/etc/systemd/system/aa-ap.service"
inst
said "KEPT        /etc/systemd/system/aa-ap.service"
has /etc/systemd/system/aa-ap.service "^# mine$"
inst --force
said "replaced    /etc/systemd/system/aa-ap.service"
check "the unit is the one here" cmp -s "$HERE/aa-ap.service" "$R/etc/systemd/system/aa-ap.service"
has /var/lib/veloaa/replaced/aa-ap.service "^# mine$"

echo "== a unit an earlier install.sh put there (OLD_UNITS) is updated without --force"
new_case old_unit
mkdir -p "$R/etc/systemd/system"
# The earlier veloaa-aa.service whose checksum is in install.sh's OLD_UNITS. It still waited for network-online.target.
printf '%s\n' '[Unit]' \
    'Description=rmworkpi: Android Auto Bluetooth start-up (aarecvd --bt-only); the session is aarecvd.service' \
    'After=bluetooth.service stick-gadget.service network-online.target' 'Wants=bluetooth.service' '' \
    '[Service]' 'ExecStart=/usr/local/lib/veloaa/aarecvd --bt-only' 'Restart=always' 'RestartSec=5' '' \
    '[Install]' 'WantedBy=multi-user.target' > "$R/etc/systemd/system/rmwork-aa.service"
cp "$R/etc/systemd/system/rmwork-aa.service" "$FAKE/old-unit"
inst
said "renamed     /etc/systemd/system/rmwork-aa.service to veloaa-aa.service [(]the old one"
called "^systemctl disable rmwork-aa.service$"
not_there /etc/systemd/system/rmwork-aa.service
check "veloaa-aa.service is the new one" cmp -s "$HERE/veloaa-aa.service" "$R/etc/systemd/system/veloaa-aa.service"
check "the old one is kept aside" cmp -s "$FAKE/old-unit" "$R/var/lib/veloaa/replaced/rmwork-aa.service"
check "it no longer waits for network-online.target" \
    sh -c "! grep -q '^After=.*network-online' '$R/etc/systemd/system/veloaa-aa.service'"

echo "== dev Pi with the old rmwork-* names: renamed, the web password kept"
new_case old_names
mkdir -p "$R/etc/systemd/system" "$R/usr/local/bin"
printf '[Service]\nExecStart=/usr/bin/python3 /usr/local/bin/rmwork_web.py\n' \
    > "$R/etc/systemd/system/rmwork-web.service"
echo old > "$R/usr/local/bin/rmwork_web.py"
printf 'abcdefgh12345678' > "$R/etc/rmwork-web.pass"
inst --dev
said "renamed     /etc/systemd/system/rmwork-web.service to veloaa-web.service [(]it had changes of its own[)]"
called "^systemctl disable rmwork-web.service$"
not_there /etc/systemd/system/rmwork-web.service
not_there /usr/local/bin/rmwork_web.py
there /var/lib/veloaa/replaced/rmwork-web.service
there /etc/systemd/system/veloaa-web.service
not_there /etc/rmwork-web.pass
check "the web password is the old one" [ "$(cat "$R/etc/veloaa-web.pass")" = abcdefgh12345678 ]

echo "== release build over a dev Pi: the dev services are left as they are"
new_case over_dev
inst --dev
: > "$FAKE/calls"
inst
said "left as they are"
there /etc/systemd/system/veloaa-web.service
not_called "disable"
check "home Wi-Fi still reads on" [ "$(setting home_wifi)" = on ]

echo "== Wi-Fi switched off (no country set): said; --country sets it"
new_case blocked; touch "$FAKE/blocked"
inst
said "WARNING: Wi-Fi is switched off"
not_called "^raspi-config"
rm "$FAKE/blocked"
inst --country US
called "^raspi-config nonint do_wifi_country US$"
said "Wi-Fi country set to US"
said "not blocked"
called "^rfkill unblock bluetooth$"
said "Bluetooth unblocked"

echo "== the image is not asked for twice"
new_case todo
mkdir -p "$R/srv"
echo x > "$R/srv/stick.img"
inst
not_said "stick image"
said "  1. Reboot"

echo "== the receiver program missing, or built for another machine: stops before anything is changed"
new_case nobinary
INST_AARECVD="$FAKE/aarecvd-arm64" inst
said "^exit 1$"
said "does not run here"
said "Nothing was changed"
not_there /usr
printf '#!/bin/sh\nexit 126\n' > "$FAKE/aarecvd-arm64"
INST_AARECVD="$FAKE/aarecvd-arm64" inst
said "^exit 1$"
said "does not run here"
not_there /usr
mkdir "$FAKE/copy"
find "$HERE" -maxdepth 1 -type f -exec cp {} "$FAKE/copy/" \;     # this folder without aarecvd/out
rm -f "$FAKE/copy/aarecvd-arm64"
INSTALL="$FAKE/copy/install.sh" INST_AARECVD= inst
said "^exit 1$"
said "the receiver program is not here"
said "aarecvd/build.sh arm64"
not_there /usr
cp "$AARECVD" "$FAKE/copy/aarecvd-arm64"
INSTALL="$FAKE/copy/install.sh" INST_AARECVD= inst
said "^exit 0$"
check "aarecvd-arm64 next to install.sh is taken" cmp -s "$AARECVD" "$R/usr/local/lib/veloaa/aarecvd"

echo "== an incomplete copy of this folder, and options it does not know"
new_case incomplete
mkdir "$FAKE/copy"; cp "$HERE/install.sh" "$FAKE/copy/"
INSTALL="$FAKE/copy/install.sh" inst
said "^exit 1$"
said "aa-ap is not in"
not_there /usr
inst --nonsense
said "^exit 1$"
said "sudo sh install.sh --keep-wifi"
not_there /usr
inst --help
said "^exit 0$"
not_there /usr

# ---- make_release_stick.sh

# bins <folder>: put three pretend head-unit programs straight into the folder
bins() {
    mkdir -p "$1"
    echo aaview > "$1/aaview.exe"; echo isvagent > "$1/isvagent.exe"; echo matmsg > "$1/MatMessage.dll"
}
stick_files() {
    check "launch.txt: aaview and bar=brief, CRLF" sh -c 'printf "%s\r\n" "\\MD\\aaview\\aaview.exe" "bar=brief" | cmp -s - "$1"' x "$1/isv/launch.txt"
    check "aaview.exe" cmp -s "$2/aaview.exe" "$1/aaview/aaview.exe"
    check "ISVAgentKronos.exe is isvagent" cmp -s "$2/isvagent.exe" "$1/ISV_install/ISV/ISVAgentKronos.exe"
    check "ISVAgent.exe is isvagent" cmp -s "$2/isvagent.exe" "$1/ISV_install/ISV/ISVAgent.exe"
    check "MatMessage.dll" cmp -s "$2/MatMessage.dll" "$1/ISV_install/ISV/MatMessage.dll"
    check "the readme has CRLF line ends" all_crlf "$1/README_VELOAA.txt"
    check "six files, no more" [ "$(find "$1" -type f | wc -l)" = 6 ]
    check "no dev-build files" [ -z "$(find "$1" -iname 'vagent*' -o -iname 'autostart*')" ]
}

echo "== stick: --dir lays the files out, from a flat folder"
new_case stick_dir
bins "$FAKE/bin"
stick --dir "$FAKE/stick" "$FAKE/bin"
said "^exit 0$"
stick_files "$FAKE/stick" "$FAKE/bin"
not_called "."                                              # no stand-in was needed: nothing but files
stick --dir "$FAKE/stick" "$FAKE/bin"
said "^exit 1$"
said "is not empty"

echo "== stick: --dir from a built checkout"
new_case stick_checkout
mkdir -p "$FAKE/co/apps/aaview/out" "$FAKE/co/apps/isvagent/out" "$FAKE/co/apps/matmsg/out" "$FAKE/flat"
echo aaview > "$FAKE/co/apps/aaview/out/aaview.exe"; echo aaview > "$FAKE/flat/aaview.exe"
echo isvagent > "$FAKE/co/apps/isvagent/out/isvagent.exe"; echo isvagent > "$FAKE/flat/isvagent.exe"
echo matmsg > "$FAKE/co/apps/matmsg/out/MatMessage.dll"; echo matmsg > "$FAKE/flat/MatMessage.dll"
stick --dir "$FAKE/stick" "$FAKE/co"
said "^exit 0$"
stick_files "$FAKE/stick" "$FAKE/flat"

echo "== stick: a program missing, no folder, no arguments"
new_case stick_missing
bins "$FAKE/bin"; rm "$FAKE/bin/MatMessage.dll" "$FAKE/bin/isvagent.exe"
stick --dir "$FAKE/stick" "$FAKE/bin"
said "^exit 1$"
said "not found in .*: isvagent.exe MatMessage.dll"
check "nothing laid out" [ ! -e "$FAKE/stick" ]
stick --dir "$FAKE/stick" "$FAKE/nowhere"
said "is not a folder"
stick
said "^exit 1$"
said "sudo sh make_release_stick.sh BIN"

echo "== stick: the image needs root"
new_case stick_user
bins "$FAKE/bin"
stick "$FAKE/bin"
said "^exit 1$"
said "needs root"
check "no image" [ ! -e "$FAKE/stick.img" ]

echo "== stick: a new image on a Pi whose gadget is not up"
new_case stick_new
bins "$FAKE/bin"; echo 0 > "$FAKE/uid"
stick "$FAKE/bin"
said "^exit 0$"
check "the image is there, 1 MB" [ "$(wc -c < "$FAKE/stick.img")" = 1048576 ]
called "^mkfs.vfat -F 32 -n VELOSTERAA $FAKE/stick.img$"
called "^mount -o loop $FAKE/stick.img "
before "^mkfs.vfat" "^mount"
before "^mount" "^umount"
not_called "^stick-gadget"
said "  isv/launch.txt"
said "the USB gadget is not up"
check "no temporary folders left" [ -z "$(find "$FAKE/tmp" -mindepth 1 -type d -empty)" ]

echo "== stick: an image is there already: refused without --replace"
new_case stick_exists
bins "$FAKE/bin"; echo 0 > "$FAKE/uid"; echo old > "$FAKE/stick.img"; mkdir "$FAKE/gadget"
stick "$FAKE/bin"
said "^exit 1$"
said "is there already"
check "the image is untouched" [ "$(cat "$FAKE/stick.img")" = old ]
not_called "^(stick-gadget|mkfs.vfat|mount)"

echo "== stick: --replace keeps the old image, unplugs before and plugs in after"
stick --replace "$FAKE/bin"
said "^exit 0$"
check "the old image is stick.img.old" [ "$(cat "$FAKE/stick.img.old")" = old ]
check "the new image is there" [ "$(wc -c < "$FAKE/stick.img")" = 1048576 ]
before "^stick-gadget eject$" "^mkfs.vfat"
before "^umount" "^stick-gadget insert$"
said "plugged in again"

echo "== stick: a second --replace does not overwrite the kept image"
: > "$FAKE/calls"
stick --replace "$FAKE/bin"
said "^exit 1$"
said "stick.img.old is there too"
check "the kept image is untouched" [ "$(cat "$FAKE/stick.img.old")" = old ]
not_called "^(stick-gadget|mkfs.vfat|mount)"

echo "== stick: formatting fails: the old image is put back and the stick plugged in again"
new_case stick_fails
bins "$FAKE/bin"; echo 0 > "$FAKE/uid"; echo old > "$FAKE/stick.img"; mkdir "$FAKE/gadget"; touch "$FAKE/mkfs_fails"
stick --replace "$FAKE/bin"
said "^exit 1$"
check "the old image is back" [ "$(cat "$FAKE/stick.img")" = old ]
check "no stick.img.old left" [ ! -e "$FAKE/stick.img.old" ]
before "^stick-gadget eject$" "^stick-gadget insert$"
said "the old image is back"
new_case stick_fails_new
bins "$FAKE/bin"; echo 0 > "$FAKE/uid"; touch "$FAKE/mkfs_fails"
stick "$FAKE/bin"
said "^exit 1$"
check "no unfinished image left" [ ! -e "$FAKE/stick.img" ]

[ $FAILED = 0 ] && echo "ALL PASSED" || echo "SOME FAILED"
exit $FAILED
