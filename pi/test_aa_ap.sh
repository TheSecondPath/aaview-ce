#!/bin/sh
# test_aa_ap.sh: runs aa-ap's "watch" against stand-ins for nmcli, iw, ip, nft, ss and sleep, and checks what it
# asked of them. It touches nothing real and needs no root. Run it on Linux from pi: sh test_aa_ap.sh
# aa-ap reads its settings through aarecvd (VELOAA_AARECVD, else aarecvd/out/aarecvd-amd64), so this runs in
# aarecvd's amd64 build image: aarecvd/build.sh shtest
# Each case gets its own folder, with the pretend state kept as files:
#   profiles (uuid:type), p.<uuid>.<field> (a profile's values), active (uuid:device), uap0 (the AP interface
#   exists), sta_connected, sta_channel, phone (a phone is on the AP), scan (what a scan prints), ss, calls (log).
# The stand-in for sleep ends the watch loop after $ROUNDS rounds, and runs ./round.<n> before round n.
HERE=$(cd "$(dirname "$0")" && pwd)
AARECVD=${VELOAA_AARECVD:-$HERE/aarecvd/out/aarecvd-amd64}
"$AARECVD" --show-settings --shell --settings /nonexistent > /dev/null 2>&1 ||
    { echo "test_aa_ap.sh: cannot run $AARECVD (build it: aarecvd/build.sh amd64; run this in its image)"; exit 2; }
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
FAILED=0

mkdir "$T/bin"
cat > "$T/bin/fake" <<'EOF'
#!/bin/sh
ME=$(basename "$0")
echo "$ME $*" >> "$FAKE/calls"
case "$ME" in
sleep)
    [ "$1" = "$AA_AP_LOOP" ] || exit 0
    N=$(($(cat "$FAKE/rounds" 2>/dev/null || echo 0) + 1))
    echo $N > "$FAKE/rounds"
    [ $N -le "$ROUNDS" ] || exit 1
    [ -f "$FAKE/round.$N" ] && . "$FAKE/round.$N"
    exit 0 ;;
nmcli)
    A="$*"
    case "$A" in
    "-t -f UUID,TYPE con show") cat "$FAKE/profiles" 2>/dev/null ;;
    "-t con show") sed 's/^\([^:]*\):.*/name-\1:\1:802-11-wireless:/' "$FAKE/profiles" 2>/dev/null
        [ -f "$FAKE/have_ap" ] && echo "aa-ap:u-ap:802-11-wireless:uap0" ;;
    "-t -f UUID,DEVICE con show --active") cat "$FAKE/active" 2>/dev/null ;;
    "con add "*) touch "$FAKE/have_ap"; echo "Connection 'aa-ap' successfully added." ;;
    "con up aa-ap") [ -f "$FAKE/ap_fails" ] && exit 4; echo "Connection successfully activated" ;;
    *)
        if [ "$1" = "--escape" ]; then shift 2; fi
        if [ "$1" = "-g" ] && [ "$3 $4" = "con show" ]; then
            cat "$FAKE/p.$5.$2" 2>/dev/null; echo
        elif [ "$1 $2 $3" = "con modify --temporary" ]; then
            [ -f "$FAKE/modify_fails" ] && exit 1
            echo "$6" > "$FAKE/p.$4.$5"
        fi ;;
    esac ;;
iw)
    case "$*" in
    "dev uap0 info") [ -f "$FAKE/uap0" ] || exit 1
        printf 'Interface uap0\n\ttype AP\n\tchannel 6 (2437 MHz), width: 20 MHz, center1: 2437 MHz\n' ;;
    "dev wlan0 interface add uap0 type __ap") [ -f "$FAKE/blocked" ] && exit 1; touch "$FAKE/uap0" ;;
    "dev uap0 del") rm -f "$FAKE/uap0" ;;
    "dev wlan0 link") if [ -f "$FAKE/sta_connected" ]; then echo "Connected to 00:11:22:33:44:55 (on wlan0)"; else echo "Not connected."; fi ;;
    "dev wlan0 info") printf 'Interface wlan0\n\tchannel %s (2462 MHz), width: 20 MHz\n' "$(cat "$FAKE/sta_channel" 2>/dev/null || echo 1)" ;;
    "dev uap0 station dump") [ -f "$FAKE/phone" ] && printf 'Station aa:bb:cc:dd:ee:ff (on uap0)\n\tauthorized:\tyes\n' ;;
    "dev wlan0 scan freq 2437") cat "$FAKE/scan" 2>/dev/null ;;
    "list") cat "$FAKE/iwlist" 2>/dev/null ;;
    esac ;;
ss) cat "$FAKE/ss" 2>/dev/null ;;
wpa_cli)
    case "$3 $4 $5" in
    "status  ") [ -f "$FAKE/sta_connected" ] && printf 'bssid=00:11:22:33:44:55
id=0
wpa_state=COMPLETED
' ;;
    "get_network 0 bgscan") if [ -f "$FAKE/bgscan" ]; then cat "$FAKE/bgscan"; else echo FAIL; fi ;;
    "set_network 0 bgscan") [ -f "$FAKE/wpa_fails" ] && { echo FAIL; exit 0; }; printf '%s' "$6" > "$FAKE/bgscan"; echo OK ;;
    esac ;;
esac
exit 0
EOF
chmod +x "$T/bin/fake"
for c in nmcli iw ip nft ss sleep wpa_cli; do cp "$T/bin/fake" "$T/bin/$c"; done

# new_case <name>: a fresh pretend Pi with three saved Wi-Fi profiles: home ("My Net"), another network and the AP.
new_case() {
    CASE=$1
    FAKE="$T/$CASE"
    mkdir -p "$FAKE/sys/wlan0" "$FAKE/sys/uap0" "$FAKE/state"
    echo "b8:27:eb:00:00:01" > "$FAKE/sys/wlan0/address"
    echo "02:27:eb:00:00:01" > "$FAKE/sys/uap0/address"
    printf 'u-home:802-11-wireless\nu-wired:802-3-ethernet\nu-other:802-11-wireless\nu-ap:802-11-wireless\n' > "$FAKE/profiles"
    printf 'My Net' > "$FAKE/p.u-home.802-11-wireless.ssid"
    printf 'Other Net' > "$FAKE/p.u-other.802-11-wireless.ssid"
    printf 'VeloAA' > "$FAKE/p.u-ap.802-11-wireless.ssid"
    printf 'ap' > "$FAKE/p.u-ap.802-11-wireless.mode"
    printf 'aa-ap' > "$FAKE/p.u-ap.connection.id"
    printf '6' > "$FAKE/p.aa-ap.802-11-wireless.channel"
    touch "$FAKE/have_ap"
    : > "$FAKE/calls"
    rm -f "$FAKE/veloaa.txt" "$FAKE/loglevel"
}

# run [VAR=value ...]: run aa-ap watch for $ROUNDS rounds, with its output in $FAKE/out
run() {
    env PATH="$T/bin:$PATH" FAKE="$FAKE" ROUNDS="${ROUNDS:-1}" JOURNAL_STREAM=8:1 \
        AA_AP_IW=iw AA_AP_NFT=nft AA_AP_WPA_CLI=wpa_cli AA_AP_PSKF="$FAKE/psk" AA_AP_CONF="$FAKE/aarecv.conf" AA_AP_STATE="$FAKE/state" \
        AA_AP_HELD="$FAKE/held" AA_AP_SYSNET="$FAKE/sys" AA_AP_LOOP=77 AA_AP_JOINING="$FAKE/joining" \
        VELOAA_AARECVD="$AARECVD" \
        VELOAA_SETTINGS="$FAKE/veloaa.txt" VELOAA_LOGLEVEL="$FAKE/loglevel" "$@" \
        sh "$HERE/aa-ap" "${VERB:-watch}" > "$FAKE/out" 2>&1
    echo "exit $?" >> "$FAKE/out"
}

ok() { echo "  ok    $CASE: $1"; }
bad() { echo "  FAIL  $CASE: $1"; FAILED=1; sed 's/^/        | /' "$FAKE/out"; sed 's/^/        > /' "$FAKE/calls"; }
called() { if grep -Fxq -- "$1" "$FAKE/calls"; then ok "did: $1"; else bad "should have done: $1"; fi; }
not_called() { if grep -Fq -- "$1" "$FAKE/calls"; then bad "should not have done: $1"; else ok "did not: $1"; fi; }
said() { if grep -Eq -- "$1" "$FAKE/out"; then ok "said: $1"; else bad "should have said: $1"; fi; }
not_said() { if grep -Eq -- "$1" "$FAKE/out"; then bad "should not have said: $1"; else ok "did not say: $1"; fi; }
count() { N=$(grep -Ec -- "$1" "$FAKE/$3"); if [ "$N" = "$2" ]; then ok "$2 x $1"; else bad "$1: $N times in $3, expected $2"; fi; }

echo "== no settings file: home Wi-Fi off, wlan0 for the AP only"
new_case nofile; rm "$FAKE/have_ap"
echo 'rm -f "$FAKE/uap0"' > "$FAKE/round.2"                # the AP interface vanishes and must be brought back
ROUNDS=3 run
called "nmcli device set wlan0 managed no"
not_called "set wlan0 managed yes"
not_called "con modify --temporary"
not_called "scan"
called "iw dev wlan0 interface add uap0 type __ap"
count "^iw dev wlan0 interface add uap0" 2 calls
count "^nmcli con add type wifi ifname uap0 .*band bg 802-11-wireless.channel 0 " 1 calls
said "^<6>settings: home Wi-Fi off .*log level info"
said "^<6>AP VeloAA up on channel 6 \(picked by NetworkManager\)"
not_said "^<4>"                                             # the missing file is aarecvd's to report
said "^exit 0$"                                             # the stand-in sleep ended the loop
if grep -q "^key=................\$" "$FAKE/aarecv.conf" && grep -q "^ip=10.42.0.1\$" "$FAKE/aarecv.conf"; then ok "aarecv.conf written"; else bad "aarecv.conf"; fi

echo "== garbled, empty and unreadable settings files: the same, and it keeps running"
for kind in garbled empty folder; do
    new_case "bad_$kind"
    case $kind in
        garbled) head -c 3000 /dev/urandom > "$FAKE/veloaa.txt" ;;
        empty) : > "$FAKE/veloaa.txt" ;;
        folder) mkdir "$FAKE/veloaa.txt" ;;
    esac
    ROUNDS=2 run
    called "nmcli device set wlan0 managed no"
    not_called "set wlan0 managed yes"
    said "^<6>AP VeloAA up"
    count "^sleep 77" 3 calls
done

echo "== the settings reader cannot be run: defaults and a warning"
new_case noreader
run VELOAA_AARECVD=/nonexistent/aarecvd
said "^<4>settings: could not run /nonexistent/aarecvd"
called "nmcli device set wlan0 managed no"
said "^<6>AP VeloAA up"

echo "== home_wifi=off with a name given: off"
new_case off_named
printf 'home_wifi=off\nhome_wifi_ssid=My Net\n' > "$FAKE/veloaa.txt"
run
called "nmcli device set wlan0 managed no"
not_called "con modify --temporary"
not_said "^<4>"

echo "== home_wifi=on, blank name: off, one warning"
new_case on_blank
printf 'home_wifi=on\nhome_wifi_ssid=\n' > "$FAKE/veloaa.txt"
ROUNDS=3 run
count "^<4>settings: .*home_wifi_ssid is blank.*stays off" 1 out
called "nmcli device set wlan0 managed no"
not_called "set wlan0 managed yes"

echo "== home_wifi=on, a name this Pi has no saved network for: off, one warning"
new_case on_unknown
printf 'home_wifi=on\nhome_wifi_ssid=Somewhere Else\n' > "$FAKE/veloaa.txt"
ROUNDS=3 run
count "^<4>settings: home_wifi=on, but this Pi has no saved Wi-Fi network" 1 out
called "nmcli device set wlan0 managed no"
not_called "set wlan0 managed yes"
not_called "con modify --temporary"
not_called "scan"

echo "== home_wifi=on, name is the AP's own: not taken for home"
new_case on_ap_name
printf 'home_wifi=on\nhome_wifi_ssid=VeloAA\n' > "$FAKE/veloaa.txt"
run
said "^<4>settings: home_wifi=on, but this Pi has no saved"
called "nmcli device set wlan0 managed no"

echo "== home_wifi=on, the saved network: joined, the other saved network held back, the AP on home's channel"
new_case on
printf 'Home_WiFi = ON\r\nhome_wifi_ssid = My Net\r\n' > "$FAKE/veloaa.txt"
touch "$FAKE/sta_connected"; echo 11 > "$FAKE/sta_channel"
echo "u-other:wlan0" > "$FAKE/active"
run
said "^<6>settings: home Wi-Fi on, network \"My Net\""
called "nmcli con modify --temporary u-other connection.autoconnect no"
called "nmcli con down u-other"
not_called "con modify --temporary u-home"
not_called "con modify --temporary u-ap"
not_called "con modify --temporary u-wired"
called "nmcli device set wlan0 managed yes"
not_called "set wlan0 managed no"
not_called "con up u-home"                                  # connected: nothing to ask for
called "nmcli con modify aa-ap 802-11-wireless.band bg 802-11-wireless.channel 11 wifi-sec.psk $(cat "$FAKE/psk") wifi-sec.pmf disable"
if [ "$(cat "$FAKE/held")" = u-other ]; then ok "held list"; else bad "held list"; fi
VERB=down run
called "nmcli con modify --temporary u-other connection.autoconnect yes"
if [ -f "$FAKE/held" ]; then bad "held list left behind"; else ok "held list removed"; fi

echo "== home on, away with the phone: wlan0 stops scanning; only the named network brings it back"
new_case away
printf 'home_wifi=on\nhome_wifi_ssid=My Net\n' > "$FAKE/veloaa.txt"
touch "$FAKE/phone"
echo "tcp ESTAB 10.42.0.1:5288" > "$FAKE/ss"
printf '\tSSID: Other Net\n\tSSID: My Net 5G\n\tSSID: \\x20My Net\n' > "$FAKE/scan"    # near misses only
echo ': > "$FAKE/calls"' > "$FAKE/round.4"
cat > "$FAKE/round.7" <<'EOF'
grep -q "managed yes" "$FAKE/calls" && touch "$FAKE/rejoined_early"
printf '\tSSID: Other Net\n\tSSID: My Net\n' > "$FAKE/scan"
rm -f "$FAKE/ss"
EOF
ROUNDS=8 run
said "^<6>the phone is connected and home Wi-Fi is not"
if [ -f "$FAKE/rejoined_early" ]; then bad "a network that is not the named one brought wlan0 back"; else ok "near-miss names ignored"; fi
called "iw dev wlan0 scan freq 2437"
said "^<6>home Wi-Fi in sight and no Android Auto session: wlan0 back to NetworkManager"
/bin/sleep 0.3                                              # the "con up" runs in the background
called "nmcli --wait 60 con up u-home"                      # NetworkManager may have stopped retrying it

echo "== home on, home down and no phone: NetworkManager asked to connect every 2 minutes (it gives up by itself)"
new_case home_down
printf 'home_wifi=on\nhome_wifi_ssid=My Net\n' > "$FAKE/veloaa.txt"
ROUNDS=9 run
/bin/sleep 0.3
count "^nmcli --wait 60 con up u-home$" 2 calls
count "^<6>home Wi-Fi down and no phone for 2 minutes" 1 out
not_called "set wlan0 managed no"

echo "== home on and down, a phone starts joining (aarecvd touches the joining file): wlan0 stops trying for home at once"
new_case joining
printf 'home_wifi=on
home_wifi_ssid=My Net
' > "$FAKE/veloaa.txt"
echo 'touch "$FAKE/joining"' > "$FAKE/round.2"
echo 'touch "$FAKE/phone"; echo "tcp ESTAB 10.42.0.1:5288" > "$FAKE/ss"' > "$FAKE/round.3"
ROUNDS=4 run
said "^<6>a phone is joining the AP and home Wi-Fi is not connected: wlan0 stops trying for home$"
count "^nmcli device set wlan0 managed no$" 1 calls
not_said "wlan0 back to NetworkManager"                       # the phone came and stayed
not_said "the phone is connected and home Wi-Fi is not"       # already quiet
not_called "scan"                                           # no look for home in the first 2 minutes (4 rounds)

echo "== ... and a phone that never shows up: home tried again after about a minute and a half"
new_case joining_nophone
printf 'home_wifi=on
home_wifi_ssid=My Net
' > "$FAKE/veloaa.txt"
echo 'touch "$FAKE/joining"' > "$FAKE/round.2"
ROUNDS=6 run
said "^<6>a phone is joining the AP"
said "^<6>the phone has been gone for a minute: wlan0 back to NetworkManager"
count "^<6>a phone is joining|wlan0 back to" 2 out

echo "== ... with home connected: nothing changes"
new_case joining_home
printf 'home_wifi=on
home_wifi_ssid=My Net
' > "$FAKE/veloaa.txt"
touch "$FAKE/sta_connected"; echo 6 > "$FAKE/sta_channel"
echo 'touch "$FAKE/joining"; touch "$FAKE/phone"' > "$FAKE/round.2"
ROUNDS=4 run
not_said "a phone is joining"
not_called "set wlan0 managed no"

echo "== home connected, a phone joins: background scans for home off at once, and back on when it has gone"
new_case bgscan
printf 'home_wifi=on
home_wifi_ssid=My Net
' > "$FAKE/veloaa.txt"
touch "$FAKE/sta_connected"; echo 6 > "$FAKE/sta_channel"
printf '"simple:30:-70:86400"' > "$FAKE/bgscan"
echo 'touch "$FAKE/joining"' > "$FAKE/round.2"
cat > "$FAKE/round.3" <<'EOF'
touch "$FAKE/phone"
[ "$(cat "$FAKE/bgscan")" = '""' ] && touch "$FAKE/off_at_join"
EOF
echo '[ "$(cat "$FAKE/bgscan")" = '"'"'""'"'"' ] && touch "$FAKE/off_with_phone"; rm -f "$FAKE/phone"' > "$FAKE/round.5"
ROUNDS=6 run
if [ -f "$FAKE/off_at_join" ]; then ok "off as the phone starts joining"; else bad "not off as the phone joined"; fi
if [ -f "$FAKE/off_with_phone" ]; then ok "stays off while the phone is on the AP"; else bad "not off with the phone on the AP"; fi
count '^wpa_cli -i wlan0 set_network 0 bgscan ""$' 1 calls
said "^<6>a phone is on the AP: background scans for home switched off"
said "^<6>no phone on the AP: background scans for home back on"
if [ "$(cat "$FAKE/bgscan")" = '"simple:30:-70:86400"' ]; then ok "put back as it was"; else bad "bgscan now: $(cat "$FAKE/bgscan")"; fi
not_called "set wlan0 managed no"                           # home is connected: wlan0 stays with NetworkManager
count "^<6>a phone is on the AP|^<6>no phone on the AP" 2 out

echo "== ... none set (the profile is locked to one access point): nothing to switch, nothing said"
new_case bgscan_none
printf 'home_wifi=on
home_wifi_ssid=My Net
' > "$FAKE/veloaa.txt"
touch "$FAKE/sta_connected" "$FAKE/phone"; echo 6 > "$FAKE/sta_channel"
ROUNDS=3 run
not_called "set_network"
not_said "background scans"

echo "== ... wpa_cli refuses: a warning, and it keeps running"
new_case bgscan_fails
printf 'home_wifi=on
home_wifi_ssid=My Net
' > "$FAKE/veloaa.txt"
touch "$FAKE/sta_connected" "$FAKE/phone" "$FAKE/wpa_fails"; echo 6 > "$FAKE/sta_channel"
printf '"simple:30:-70:86400"' > "$FAKE/bgscan"
ROUNDS=2 run
said "^<4>warning: could not switch off background scans for home"
not_said "back on"
said "^exit 0$"

echo "== a name with spaces at the ends, a backslash and non-ASCII letters is matched as iw prints it"
new_case odd_name
printf 'home_wifi=on\nhome_wifi_ssid=" Caf\303\251 \\ #1 "\n' > "$FAKE/veloaa.txt"
printf ' Caf\303\251 \\ #1 ' > "$FAKE/p.u-home.802-11-wireless.ssid"
touch "$FAKE/phone"
printf '\tSSID: \\x20Caf\\xc3\\xa9 \\x5c #1\\x20\n' > "$FAKE/scan"
ROUNDS=5 run
not_said "^<4>"
said "^<6>home Wi-Fi in sight and no Android Auto session"

echo "== log level: warning drops the info lines; a change is followed without a restart"
new_case level
printf 'log_level=warning\n' > "$FAKE/veloaa.txt"
echo 'printf "log_level=debug\nhome_wifi=on\nhome_wifi_ssid=My Net\n" > "$FAKE/veloaa.txt"' > "$FAKE/round.2"
echo 'rm -f "$FAKE/uap0"' > "$FAKE/round.3"
ROUNDS=4 run
count "^<6>settings: home Wi-Fi off" 0 out
count "^<4>settings: log level debug" 1 out
count "^<4>settings: the home Wi-Fi settings have changed" 1 out
count "^<6>AP VeloAA up" 1 out                              # only the one logged after the level went up
not_called "set wlan0 managed yes"                          # the change of home Wi-Fi is not applied mid-run

echo "== log level from the dev file when veloaa.txt sets none"
new_case devlevel
printf 'home_wifi=off\n' > "$FAKE/veloaa.txt"; echo error > "$FAKE/loglevel"
run
not_said "^<[4-7]>"
new_case devlevel2
echo error > "$FAKE/loglevel"; printf 'log_level=info\n' > "$FAKE/veloaa.txt"
run
said "^<6>settings: home Wi-Fi off .*log level info"

echo "== home Wi-Fi off, the chip can use 5 GHz: NetworkManager picks a 5 GHz channel"
new_case five; rm "$FAKE/have_ap"
printf '\t\tFrequencies:\n\t\t\t* 2437.0 MHz [6] (20.0 dBm)\n\t\t\t* 5180.0 MHz [36] (20.0 dBm)\n' > "$FAKE/iwlist"
run
count "^nmcli con add type wifi ifname uap0 .*band a 802-11-wireless.channel 0 " 1 calls
said "^<6>AP VeloAA up on channel .* \(picked by NetworkManager\)"

echo "== home Wi-Fi off, 5 GHz listed but not usable here: NetworkManager picks a 2.4 GHz channel"
for flag in "(no IR)" "(disabled)"; do
    new_case "five_$(echo "$flag" | tr -dc a-z)"; rm "$FAKE/have_ap"
    case $flag in
        *IR*) printf '\t\t\t* 5180.0 MHz [36] (20.0 dBm) %s\n' "$flag" > "$FAKE/iwlist" ;;
        *) printf '\t\t\t* 5180 MHz [36] %s\n' "$flag" > "$FAKE/iwlist" ;;
    esac
    run
    count "^nmcli con add type wifi ifname uap0 .*band bg 802-11-wireless.channel 0 " 1 calls
done

echo "== home on 5 GHz: the AP follows it there with the band changed too"
new_case follow5
printf 'home_wifi=on\nhome_wifi_ssid=My Net\n' > "$FAKE/veloaa.txt"
touch "$FAKE/sta_connected"; echo 36 > "$FAKE/sta_channel"
ROUNDS=4 run
called "nmcli con modify aa-ap 802-11-wireless.band a 802-11-wireless.channel 36 wifi-sec.psk $(cat "$FAKE/psk") wifi-sec.pmf disable"
said "^<6>home Wi-Fi on channel 36 for a minute \(AP on 6\): moving the AP"
called "nmcli con modify aa-ap 802-11-wireless.band a 802-11-wireless.channel 36"

echo "== Wi-Fi blocked: said as an error, exit 1 (systemd starts it again)"
new_case blocked; touch "$FAKE/blocked"
run
said "^<3>could not create the AP interface"
said "^exit 1$"
count "^sleep 77" 0 calls

echo "== by hand: up and status still work without the settings"
new_case byhand
touch "$FAKE/sta_connected"; echo 3 > "$FAKE/sta_channel"
VERB=up run env -u JOURNAL_STREAM
said "^AP VeloAA up on channel 3"
VERB=status run
said "^exit 0$"

[ $FAILED = 0 ] && echo "ALL PASSED" || echo "SOME FAILED"
exit $FAILED
