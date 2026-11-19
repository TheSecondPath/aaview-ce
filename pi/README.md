# Pi side

A Raspberry Pi Zero 2 W lives in the car. It acts as the head unit's USB stick and as the phone's wireless
Android Auto receiver. On the dev build it also points a camera at the screen.

## Two builds

| | Release build | Dev build (`velosterAA`) |
|---|---|---|
| For | anyone's car: Android Auto and nothing else | working on the project |
| Pi services | `veloaa-ramfiles`, `stick-gadget`, `aa-ap`, `veloaa-aa`, `aarecvd` | the same, plus `veloaa-web` (web page, `vjob`) and `veloaa-input` (game-controller input) |
| The stick | `isv\launch.txt` starts `\MD\aaview\aaview.exe` | `isv\launch.txt` starts `\MD\vagent\vagent.exe`, which starts aaview from `autostart.txt` |
| On the stick | aaview, the launcher to copy by hand, a readme | also vagent and its jobs, the test tools |
| Home Wi-Fi | off unless `veloaa.txt` says otherwise | on |
| Log level | `info` | `debug` (`/etc/aarecv/loglevel`) |
| Install | `sudo sh install.sh [--keep-wifi]`, `sudo sh make_release_stick.sh BIN` | `sudo sh install.sh --dev`, `setup_stick.sh`, `load_stick.sh` |

Both builds run the same `aarecvd`, `aa-ap` and `aaview.exe`. The release build only leaves things out; there is
no second code path. Python is only needed on the dev build, for the web page and the input service.

## Setting up a Pi

First build the receiver on a PC with Docker:

```
aarecvd/build.sh arm64
```

This takes several minutes and produces `aarecvd/out/aarecvd-arm64` (see `aarecvd/README.md`).

You need a Pi Zero 2 W with Raspberry Pi OS Lite 64-bit (trixie or bookworm; NetworkManager is required). Copy
this folder to it, including `aarecvd/out/aarecvd-arm64` (or put that file next to `install.sh` as
`aarecvd-arm64`), then run one of:

```
sudo sh install.sh               # release build
sudo sh install.sh --keep-wifi   # release build that keeps joining the Wi-Fi network the Pi is on now
sudo sh install.sh --dev         # dev build (implies --keep-wifi)
```

Other options:

- `--force` replaces installed unit files that differ from the ones here. The old ones are kept in
  `/var/lib/veloaa/replaced`.
- `--country XX` sets the Wi-Fi country (two letters, e.g. `US`). Wi-Fi stays switched off until one is set.

The script installs missing packages (it needs the internet once), switches the USB port to device mode, copies
the programs and unit files and enables them. It does not start or stop anything, so reboot afterwards.

Running it again is safe. It brings the programs up to date and leaves `veloaa.txt`, passwords and changed unit
files alone. Unit files that an earlier run of `install.sh` put there are updated. At the end it lists what is
still to do and says whether home Wi-Fi will be on or off after the reboot.

Without `--keep-wifi`, the Pi joins no Wi-Fi network after the reboot, so SSH over Wi-Fi stops working. That is
the release default, because the Zero's single radio is then used only for the phone's access point. To get back
in, put the SD card in a PC and edit `veloaa.txt` on the `bootfs` drive.

`install.sh` does not do the following, because they are not in this repo or need the car:

- **The stick image.** Run `sudo sh make_release_stick.sh BIN`, where `BIN` holds `aaview.exe`, `isvagent.exe` and
  `MatMessage.dll` (or is a checkout that was built on Windows). It writes `/srv/stick.img` and refuses to
  overwrite an existing one. `--replace` keeps the old image as `stick.img.old` and puts it back if anything
  fails. `--dir OUT` only lays the files out in a folder, to look at or to copy to a real USB stick. The stick's
  `README_VELOAA.txt` describes the one-time steps on the head unit: copy `ISV_install\ISV` to `\Storage Card`,
  enter Test Mode and turn ISV on.
- **Pairing the phone.** In the car, hold the Android Auto icon for 3 seconds, or use `aarecvd --bt-only --pair`
  (see [Pairing](#pairing)).
- **The Wi-Fi country**, if Wi-Fi is still switched off (`--country US`).

## Settings

Settings live in `/boot/firmware/veloaa.txt`, a text file on the SD card's FAT boot partition (the `bootfs` drive
on a PC), so it can be edited with any text editor. `veloaa.txt.example` is the template. aarecvd reads the file
directly, and aa-ap reads it through `aarecvd --show-settings --shell`.

To see the settings in force:

```
sudo /usr/local/lib/veloaa/aarecvd --show-settings
aa-ap status
```

| Key | Values | Default | Meaning |
|---|---|---|---|
| `home_wifi` | `on`, `off` | `off` | join the network named below when in range |
| `home_wifi_ssid` | the network's name | blank | used only with `home_wifi=on` |
| `log_level` | `error`, `warning`, `info`, `debug` | `info` | for aarecvd, aa-ap and aaview |
| `set_clock` | `on`, `off` | `on` | set the head unit's clock from the Pi's when they are more than 3 minutes apart |

A missing, empty or garbled file, an unknown value, or `home_wifi=on` without a name all fall back to the
defaults, with one warning in the journal. Neither service stops because of it.

The Wi-Fi password is not in this file. The network has to be set up once on the Pi so that NetworkManager has a
saved profile for it, either in Raspberry Pi Imager's settings when writing the card or with
`sudo nmcli device wifi connect <name> --ask` on the Pi. A `wpa_supplicant.conf` on the boot partition is not read
by trixie. `veloaa.txt` only names which saved network to use.

Home Wi-Fi settings apply at the next boot. A changed log level is picked up within 30 seconds.

## Tests

None of the tests touches a real Pi or needs root. They use stand-ins for `nmcli`, `iw`, `systemctl`, `apt-get`,
`mount` and so on, and run in aarecvd's amd64 build image (Docker) with an amd64 build of aarecvd:

```
aarecvd/build.sh settingstest   # the settings reader (aarecvd --show-settings) and --quit-aaview
aarecvd/build.sh shtest         # test_aa_ap.sh (aa-ap watch, home Wi-Fi off and on) and
                                # test_install.sh (install.sh into a pretend root, make_release_stick.sh)
aarecvd/build.sh test           # a whole session against a fake phone
aarecvd/build.sh bttest         # the Bluetooth start-up against a pretend BlueZ
aarecvd/build.sh alltest        # all of them
```

## Hardware

- A Pi Zero 2 W. The dev build adds a Camera Module v1 (OV5647) on the CSI connector, mounted upside down and
  aimed at the screen.
- The Zero's middle USB port goes to the car's USB data port. That one cable powers the Pi and carries data. The
  PWR port has no data lines.
- Raspberry Pi OS Lite 64-bit (trixie). The dev Pi is called `velosterAA`, with SSH key login only and
  password-free sudo for its user.
- USB device mode: `install.sh` adds `dtoverlay=dwc2,dr_mode=peripheral` under `[all]` in
  `/boot/firmware/config.txt`, and `/etc/modules-load.d/usb-gadget.conf` loads `dwc2` and `libcomposite`.

## The stick

`stick-gadget` (installed in `/usr/local/sbin`) presents three USB drives to the car: the stick image
`/srv/stick.img`, the input mailbox and the stream. The stick image is a 2 GB FAT32 volume labelled VELOSTERAA.
Windows CE mounts it as `\MD`. `stick-gadget.service` runs `stick-gadget up` at boot.

```
sudo stick-gadget status|up|eject|insert|reinsert [secs]|down
```

Reading the image is safe at any time. Edit it only while it is ejected, and only after CE has noticed the unplug:
allow 5 seconds, or check that the vagent strip reads "stick out - waiting". If the stick comes back too quickly,
CE can keep its cached file table and write it back over the new files.

On the dev build, vagent notices the stick coming back and runs any new `vagent\jobs\*.job`. vagent writes a
heartbeat to `status.txt`, so the image changes every few seconds while it runs.

Dev build tools:

- `setup_stick.sh` is a one-time setup after `install.sh --dev`. It creates the image with vagent's job folders and
  connects it.
- `load_stick.sh` copies `/tmp/load/` onto the image while it is ejected, then reconnects it.
- `vjob 'cmd' ['cmd' ...]` (install in `/usr/local/bin`) runs vagent commands as a job and prints the results. Each
  round trip takes about 10 seconds. Example: `vjob 'dir \Storage Card\ISV' mem`.

`vjob`, `load_stick.sh` and `make_release_stick.sh` take a lock on the image with `flock`, so only one of them uses
the stick at a time. A second one waits up to `LOCK_WAIT` seconds (default 600). If one fails or is stopped
part-way, it unmounts the image and plugs the stick back into the car before exiting.

### Mailbox and stream in RAM

`/run/veloaa/mailbox.img` (64 KB) and `/run/veloaa/stream.img` (4 MB) are created at boot by
`veloaa-ramfiles.service`, which runs before `stick-gadget`, `veloaa-input`, `veloaa-aa` and `aarecvd`. The mailbox
is rewritten 125 times a second and the stream carries about 0.4 MB/s, so keeping them in RAM saves the SD card
from constant writes. Nothing in them needs to survive a reboot.

## Wireless Android Auto

`aarecvd` (`/usr/local/lib/veloaa/aarecvd`) is the phone-facing half of the receiver. It is written in C++ on
aasdk; the source and details are in `aarecvd/`. Its screen is `apps/aaview` on the head unit. It runs as two
services, the way OpenAuto runs `btservice` next to `autoapp`:

- `veloaa-aa` runs `aarecvd --bt-only`: Bluetooth pairing, waking the phone and telling it which Wi-Fi to join.
- `aarecvd` runs the Android Auto session itself.

Logs: `journalctl -u aarecvd -u veloaa-aa -f`.

### Bluetooth

aarecvd registers the Android Auto wireless RFCOMM profile on channel 22 (channel 8 is BlueZ's SIM Access), an HSP
headset profile and a pairing agent. Outside pairing mode the agent refuses pairing, the adapter is not pairable,
and only paired devices get the Wi-Fi details.

The headset link is only used to wake the phone's wireless Android Auto. It is closed as soon as the Android Auto
profile connects, so the phone never routes a call to the Pi. Android Auto does not carry call audio, and the
session offers the phone no Bluetooth service, so calls stay with the car's own hands-free.

### Pairing

In the car, hold aaview's icon still for 3 seconds. It turns purple and shows "BT", and the Pi is discoverable and
pairable for 3 minutes. Pair the Pi (its hostname) from the phone's Bluetooth settings. The icon then shows a
6-digit code. If the phone shows the same code, confirm on the phone and tap the icon; without the tap the Pi
refuses the phone after 30 seconds. Pairing closes 20 seconds after a phone pairs, after the 3
minutes, or when the icon is held again. Only one device can pair each time pairing is opened. This also works
during a session, to add a second phone.

Internally, aaview sends event 9 (x = 1 to open, 0 to close, 2 for the tap), aarecvd passes "pair on",
"pair off" or "pair yes" to
`aarecvd --bt-only`, and that writes `/run/veloaa/pairing` (until, passkey) while pairing is open. aarecvd puts
this in the stream header (flags bit 4, the code at byte 52).

Over SSH instead:

```
sudo systemctl stop veloaa-aa
sudo /usr/local/lib/veloaa/aarecvd --bt-only --pair    # 3 minutes; logs the passkey
# pair the Pi from the phone, tap the icon when the codes match, then Ctrl-C
sudo systemctl start veloaa-aa
```

### Reconnecting

With a paired phone and no session, aarecvd connects the phone's headset profile every 10 seconds (20 second
limit per try). A phone that stays connected for a minute without starting a session gets a nudge: a full
Bluetooth disconnect, then a connect. If nudges don't help, they repeat after 1, 2 and 4 minutes, then every 5
minutes. A try that gets no answer, or "in progress" twice, also ends in a disconnect.

After three unanswered tries in 5 minutes, aarecvd sends the Bluetooth chip a command directly (HCI
Read_BD_ADDR). No answer means the chip is hung, so it is reset and the service restarts. If the chip answers, the
phone is simply out of range (BlueZ never answers a connect to an absent phone), and the tries continue, with a
reset only after 15 minutes.

### Session

1. Over RFCOMM: WifiStartRequest (Pi IP, port 5288), then WifiInfoResponse with the access point's details from
   `/etc/aarecv.conf` (`ssid=`, `key=`, `bssid=`, `ip=`, written by aa-ap). Without that file no session starts;
   the network the Pi itself is on is never handed out.
2. The phone connects to TCP 5288. Connections are accepted only from the access point's /24 unless
   `--any-client` is given. Protocol version 1.6, TLS 1.2 with the head unit as client. The certificate is
   `/etc/aarecv/hu.crt` and `hu.key` if present, otherwise aasdk's built-in one (see `aarecvd/README.md`).
3. Service discovery: 800x480 touch input, sensors, 800x480 video at 30 fps, left-hand drive.
4. Video goes to the stream LUN with the SPS level rewritten to 12 and SPS/PPS repeated before every IDR frame.
   Touches from aaview go back to the phone as InputEventIndication.

aarecvd pings the phone every 5 seconds (`--ping`) and the socket has a 15 second timeout (`--timeout`), so a
phone that leaves without closing the connection ends the session within 15 seconds.

`aarecvd/build.sh test` runs a session against a fake phone (real TLS, split frames, and the microphone path with
chunks written the way aaview writes them) and prints `SELFTEST PASSED`.

### Microphone

The microphone is used for the assistant only. When the phone opens the microphone channel, aarecvd sets flag
bit 3 and a new `mic_gen` in the stream header. aaview records the car's hands-free microphone (wave-in device 1,
16 kHz 16-bit mono) into sectors 2 to 13 of the stream LUN in 62.5 ms chunks, and aarecvd forwards them to the
phone as they appear (polled every 5 ms). The layout is in `aarecvd/src/Projection/StreamRing.hpp`. The log reports
how many chunks went through when the phone closes the channel, and warns after 2 seconds with none.

### Late audio

Media audio that arrives more than 150 ms after the previous packet is counted in the 10-second statistics line
("media audio late N times, worst gap M ms"). Gaps over 250 ms get their own line. aaview buffers 600 ms.

### Head unit clock

When the Pi's clock is set from the internet (home Wi-Fi, with the kernel's maximum error under 16 seconds as
`timedatectl` reports it), the stream header carries the time and the Pi's time zone offset. aaview then sets the
head unit's clock if it is more than 3 minutes off and `set_clock` is on. The time zone is the Pi's own
(`timedatectl`; Raspberry Pi Imager sets it). Away from home nothing is sent, because the Zero has no battery
clock and the phone does not provide the date.

### Log level

The levels are `debug` (everything), `info` (no protocol chatter, keys, touches or statistics), `warning` (only
what went wrong or was slow) and `error`. The level is taken from the first of:

1. `log_level=` in `/boot/firmware/veloaa.txt`, if present and valid
2. one word in `/etc/aarecv/loglevel`, e.g. `echo warning | sudo tee /etc/aarecv/loglevel`
3. `info`

`install.sh --dev` writes `debug` into `/etc/aarecv/loglevel` and comments `log_level` out of `veloaa.txt`.

aarecvd checks every 5 seconds and aa-ap every 30 seconds, so nothing needs restarting. aaview follows aarecvd's
level for its own log on the stick (header flags bits 16-17). Each line carries its syslog priority, so
`journalctl -u aarecvd -u veloaa-aa -p warning` (or `-u aa-ap`) shows only warnings and errors at any level.

### Journal

Raspberry Pi OS normally keeps the journal in RAM only, which would lose every drive's log when the car cuts the
power. `install.sh` installs `60-veloaa-journald.conf` as `/etc/systemd/journald.conf.d/60-veloaa.conf`:
`Storage=persistent`, at most 200 MB in 20 MB files, 30 days, synced every 30 seconds. To read earlier boots:

```
journalctl --list-boots
journalctl -b -1 -u aarecvd -u veloaa-aa
```

## The phone's access point (aa-ap)

The `aa-ap` service adds `uap0` next to `wlan0` and runs a NetworkManager hotspot called "VeloAA" on it (WPA2 only,
10.42.0.1, DHCP). It then writes `/etc/aarecv.conf` so that `aarecvd --bt-only` tells the phone to join it. The
phone talks to the Pi directly, with much lower latency and about twice the video frame rate compared with going
through a home network. Log: `journalctl -u aa-ap`.

`aa-ap watch` reads the settings when the service starts:

- `home_wifi=off` (the default, also with no file): `wlan0` is taken away from NetworkManager for good
  (`nmcli device set wlan0 managed no`). The Pi joins no network, never scans, and the access point stays on
  channel 6. The rest of this section does not apply.
- `home_wifi=on` with `home_wifi_ssid`: the Pi joins that network through its saved NetworkManager profile. Every
  other saved Wi-Fi profile has autoconnect switched off until the next reboot (`nmcli con modify --temporary`,
  which does not rewrite the saved files), so the Pi never wanders onto another known network. If no saved profile
  has that name, aa-ap logs a warning and behaves as if home Wi-Fi were off.

With home Wi-Fi on:

- **One channel.** The chip only allows access point and client on the same channel, so the access point follows
  the home link's channel (checked by a watchdog after 1 minute). The channel is saved in
  `/var/lib/aa-ap/home_channel`, and the access point starts there when the home link is not up yet.
- **No scanning on the road.** Away from home, NetworkManager would scan for the home network every couple of
  minutes, and each scan takes the radio off the access point's channel and stalls the phone's audio. Once the
  phone has been connected for 90 seconds with no home link, `aa-ap watch` takes `wlan0` away from NetworkManager.
  The access point, the phone's link and the firewall table stay as they are.
- **Getting home.** `wlan0` is handed back when the phone has been gone for a minute, or when a quick look on the
  access point's own channel finds a saved network. That happens at once if no Android Auto session is running,
  or after 5 minutes in sight if one is, because NetworkManager rejoining drops the phone for about 25 seconds. If
  home was in sight but NetworkManager did not join it, the next look is 10 minutes later. So after a drive, the Pi
  can stay off the home network for up to 5 minutes while the phone is still connected.
- **No home tries while a phone joins.** `aarecvd --bt-only` touches `/run/veloaa/joining` when a phone opens the
  Android Auto wireless link. `aa-ap watch` checks it every second and, if home Wi-Fi is not connected, takes
  `wlan0` away from NetworkManager at once instead of after 90 seconds, and does not look for home for the next 2
  minutes. If the phone never shows up, `wlan0` is handed back after about 90 seconds. Nothing changes while the
  home link is up.
- **Mesh networks.** If the home network has several access points, every move between them drops the home link
  and moves the phone's access point to another channel, which kicks the phone off. Pinning the home profile to
  one access point (`nmcli con modify <profile> 802-11-wireless.bssid <address>`) avoids this.

`aa-ap down`, and so stopping the service, always gives `wlan0` back.

Other settings:

- **No routing.** NetworkManager's shared mode would NAT the access point's clients onto the home network.
  `aa-ap up` adds an nftables table (`inet veloaa`) that drops everything forwarded to or from `uap0`.
- **No roaming.** `options brcmfmac roamoff=1` (`/etc/modprobe.d/brcmfmac-roamoff.conf`) stops the chip's firmware
  from roaming the home link between access points. That churn also starves Bluetooth on the shared chip
  (`hci0: command 0x0c52 tx timeout`), so the phone cannot connect.
- **WPA2 only, PMF off.** With PMF optional, NetworkManager advertises WPA3/SAE transition mode and phones may
  fail to associate.
- **Wi-Fi power save off** (in `aarecvd.service`'s `ExecStartPre`), because it holds packets for 50-400 ms.

`aa-ap trial` brings the access point up and rolls it back after 3 minutes unless `/run/aa-ap.ok` appears. Run it
detached (`sh -c "nohup ... < /dev/null &"`), otherwise an SSH session that starts it in the background never
returns.

## Dev build extras

### Web page (`veloaa_web.py`)

Open `http://velosterAA.local:8080/` (or the Pi's address on the home network). The login is `veloaa` and the
password is in `/etc/veloaa-web.pass`. It runs as `veloaa-web.service` (as root, starting at boot) and uses only
Python's standard library.

- A live camera view of the head unit's screen at `/stream.mjpg` (800x600 at about 8 fps, fixed exposure, rotated
  180°). `/snapshot.jpg` gives the latest frame. While the service runs it owns the camera, so use the snapshot
  instead of `rpicam-still`.
- A box for vagent commands, plus quick buttons: Start/Stop Android Auto (`aarecvd --unquit-aaview` /
  `--quit-aaview` on the Pi, then aaview on the head unit), AA log, Processes, Memory and Start MyShell. Jobs run
  one at a time through `vjob` (about 10 seconds each), and the page shows the last 30 with their output.

### Game-controller input (`veloaa_input.py`)

`veloaa-input.service` grabs each input device for itself (EVIOCGRAB), so its keys reach only the head unit and
not the Pi's console. Without that, Ctrl+Alt+Del (for the head unit's task manager) would reboot the Pi.

### Camera

Auto exposure blows out the screen, so use fixed settings:

```
rpicam-still -n -t 1200 --width 1296 --height 972 --shutter 4000 --gain 1 --awb indoor --rotation 180 -o shot.jpg
```

The lens is wide-angle and distorts the edges, which doesn't matter for checking whether video plays.

## Caveats

- The Pi loses power with the car and gets no clean shutdown. A read-only root filesystem (overlayfs) with a
  separate partition for `stick.img` is the proper fix once the setup is stable. Until then, a power cut can
  corrupt files being written at that moment. Keeping the home Wi-Fi profile as a NetworkManager keyfile in
  `/etc/NetworkManager/system-connections/` and making it immutable (`chattr +i`; unlock it to change it) protects
  it.
