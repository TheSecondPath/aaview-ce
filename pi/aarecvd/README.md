# aarecvd

The Pi's wireless Android Auto receiver, written in C++17 on [aasdk](https://github.com/opencardev/aasdk) and
structured like [OpenAuto](https://github.com/opencardev/openauto). Both are GPLv3, and so is this.

It runs as two processes, the way OpenAuto runs `btservice` next to `autoapp`: `aarecvd --bt-only` handles the
Bluetooth start-up, and `aarecvd` runs the Android Auto session.

```
phone --Bluetooth RFCOMM--> aarecvd --bt-only     "join this Wi-Fi, connect to <ip>:5288"
phone --TCP 5288, TLS-----> aarecvd               every Android Auto channel (aasdk)
aarecvd --stream LUN------> aaview on the head unit: H.264, PCM, microphone back, touches/keys/lights back
aarecvd --/run/veloaa/aarecv.sock--> aarecvd --bt-only   "session up" / "session ended" / "icon <state>"
```

## Layout

| aarecvd | OpenAuto counterpart | What it does |
|---|---|---|
| `src/main.cpp` | `autoapp.cpp` | options, logging, thread pool |
| `src/App` | `App` | TCP 5288 (access point subnet only unless `--any-client`), one session at a time (a new connection replaces the old one), notify socket |
| `src/Service/AndroidAutoEntity` | `AndroidAutoEntity` | control channel: version, TLS, service discovery, audio focus (GAIN; RELEASE gets LOSS), nav focus, pings both ways, bye-bye, dropping a session that has gone silent for 15 s |
| `src/Service/ServiceFactory` | `ServiceFactory`, `AndroidAutoEntityFactory` | builds a session; `SessionLink` hands aaview's events to it |
| `src/Service/Pinger` | `Pinger` | our ping every `--ping` seconds |
| `src/Service/Transport` | none | `WatchedTCPEndpoint` (when bytes last arrived, how long a send has been blocked), `TlsWrapper` (TLS 1.2, AES-GCM) |
| `src/Service/MediaSink/VideoService` | `VideoMediaSinkService`, `VideoService` | video channel 3, `--unacked`, focus, icon tap, keyframe watch |
| `src/Service/MediaSink/AudioService` | `AudioMediaSinkService` and subclasses | channel 4 media 48 kHz stereo (stream 0), 5 guidance 16 kHz (1), 6 system 16 kHz (2), `--audio-unacked`, lateness and loudest-sample log lines |
| `src/Service/MediaSource/MicrophoneService` | `MediaSourceService` | microphone channel 7, gated by the header flag |
| `src/Service/Sensor/SensorService` | `SensorService` | driving status (unrestricted), night mode from the headlights |
| `src/Service/InputSource/InputSourceService` | `InputSourceService` | touches and keys |
| `src/Service/{PhoneStatus,MediaPlaybackStatus,NavigationStatus}` | same names | log what the phone sends (only with `--status-services`) |
| `src/Projection/StreamRing` | none | the stream LUN layout; `--quit-aaview` / `--unquit-aaview` |
| `src/Projection/RingOutputs` | `QtVideoOutput`, `QtAudioOutput`, `QtAudioInput` | `RingVideoOutput` (SPS/PPS before IDRs, level_idc 12), `RingAudioOutput(stream)`, `RingAudioInput` (microphone slots) |
| `src/Projection/RingInputDevice` | `InputDevice` | reads aaview's status sector on its own thread (every 5 ms in a session, 100 ms idle), header heartbeat every 2 s |
| `src/Config`, `src/Log` | `Configuration` | `veloaa.txt` and `/etc/aarecv/loglevel` (re-read every 5 s; `--show-settings [--shell]` for aa-ap and install.sh), `aarecv.conf` (`ip=` only), `aarecv-keys.conf`; journald `<N>` priority prefixes |
| `src/Bluetooth/Bluetooth` | `btservice/BluetoothHandler`, `AndroidBluetoothService` | `--bt-only`: BlueZ over D-Bus with sd-bus (OpenAuto uses Qt Bluetooth). Android Auto wireless profile (RFCOMM 22) and HSP headset profile, pairing agent (`--pair`), auto-connect every 10 s, nudges, closing the headset link, telling a hung chip from a phone out of range |
| `src/Bluetooth/WirelessStart` | `btservice/AndroidBluetoothServer` | the RFCOMM start-up exchange: WifiStartRequest, then WifiInfoResponse from `/etc/aarecv.conf` (hand-written protobuf with security mode 8, where aasdk's enum would say 5) |
| `src/Bluetooth/BtOnly` | `btservice.cpp` | `--bt-only`'s main loop: sd-event and the notify socket |

## Build

You need Docker and, the first time, network access. `build.sh` creates two images per architecture if they are
missing:

- `veloaa-aasdk-build:<arch>` from `docker/aasdk/Dockerfile`: aasdk, fetched at commit
  `9bf6adf933665dee26532201719fac14a047ccf1` and built from Debian 13 packages into `/opt/aasdk`. aasdk's unit
  tests run during the build, and a failure stops it. The aasdk source is not kept in this repo.
- `veloaa-aarecvd-build:<arch>` from `docker/Dockerfile.build`, on top of the first: libsystemd, plus Python with
  D-Bus for the desk tests.

`./build.sh aasdk` rebuilds both images for amd64 and arm64. From scratch, amd64 takes about 5 minutes and arm64
(under qemu) about an hour and a half.

```
./build.sh            # out/aarecvd-amd64 (dynamic, for the desk tests) and out/aarecvd-arm64 (for the Pi)
./build.sh arm64      # only the Pi binary
```

The arm64 binary links aasdk, protobuf, OpenSSL, zlib/zstd, libsystemd and libstdc++ statically and needs only
glibc. Its build runs under qemu and takes several minutes. `out/` is not in git.

## Desk tests

```
./build.sh test          # builds amd64, then runs test/aarecvd_selftest.py (the session) in the amd64 image
./build.sh bttest        # builds amd64, then runs test/aarecvd_bt_selftest.py (--bt-only) in the amd64 image
./build.sh settingstest  # test/aarecvd_settings_test.py: veloaa.txt (--show-settings) and --quit-aaview
./build.sh shtest        # pi's test_aa_ap.sh and test_install.sh, which read the settings through aarecvd
./build.sh alltest       # all four
```

The tests are written in Python and run only in the build image. Nothing on the Pi needs Python.

`aarecvd_selftest.py` starts aarecvd on localhost with a temporary stream image and plays the phone, as a TLS
server with a throwaway certificate. It checks:

- version exchange, TLS and service discovery, field by field against recorded reference output
- every video frame in the ring, byte for byte, including fragmented ones
- media and speech records with their stream ids, and audio focus
- touches, keys and headlights written into aaview's status sector reaching the phone
- the microphone path and bye-bye
- an icon tap with no session reaching the notify socket
- a silent phone being dropped after 15 seconds
- the stream header bytes, and the `--status-services` logging

`aarecvd_bt_selftest.py` runs a pretend BlueZ on a private D-Bus daemon and plays a recorded scenario against
`aarecvd --bt-only`. It checks:

- profile and agent registration and the adapter settings
- pairing refused without `--pair`, and accepted (device trusted) with it
- an unpaired device's link being closed
- the start-up messages, byte for byte
- the headset link being dropped when the Android Auto link opens, after 10 seconds, or after 1 second in a session
- auto-connect after 10 seconds, to the paired phone only, and none during a session
- the nudge on an icon tap, and "in progress" once versus twice
- three NoReply results with a silent chip ending in exit code 3

The last case points at a UART device that does not exist, and the test refuses to run on a Pi.

`aarecvd_settings_test.py` runs the settings reader's test cases against `--show-settings`.

## Running it

`install.sh` (in `pi/`) puts the arm64 build in `/usr/local/lib/veloaa/aarecvd` and enables two units:

- `rmwork-aa.service` runs `aarecvd --bt-only`, the Bluetooth start-up only. It binds `/run/veloaa/aarecv.sock`,
  uses the session's "session up" and "session ended" messages for its nudges and auto-connects, and connects to
  the phone on "icon <state>".
- `aarecvd.service` runs the session as root, since it writes `/run/veloaa/stream.img`. Extra options go in
  `AARECVD_ARGS=` in `/etc/default/aarecvd`, e.g. `--status-services`.

Logs: `journalctl -u aarecvd -u rmwork-aa`.

### Pairing a phone

On the head unit, hold aaview's icon still for 3 seconds. It turns purple and pairing is open for 3 minutes. When
the phone pairs, the code shows on the icon. If it matches the phone's, confirm on the phone and tap the icon.

Over SSH:

```
sudo systemctl stop rmwork-aa
sudo /usr/local/lib/veloaa/aarecvd --bt-only --pair    # discoverable and pairable for 3 minutes; logs the passkey
# pair from the phone, tap the icon when the codes match, then Ctrl-C
sudo systemctl start rmwork-aa
```

The icon hold reaches `--bt-only` as "pair on" or "pair off" on its notify socket. While pairing is open it
writes `/run/veloaa/pairing` (`--pairing-file`), and the session process puts that in the stream header for
aaview.

### Helper commands

These are for the shell scripts and the web page, and exit at once:

- `aarecvd --show-settings [--shell]` prints what `veloaa.txt` and `/etc/aarecv/loglevel` say, in words or as
  `key=value` lines. `--settings` and `--loglevel-file` point it at other files.
- `aarecvd --quit-aaview` / `--unquit-aaview` set or clear the quit flag in the stream header, which makes aaview
  on the head unit quit cleanly. The web page's Stop and Start buttons use them.

## Protocol details

- **TLS certificate.** The head unit's certificate and key come from `/etc/aarecv/hu.crt` and `hu.key` (PEM;
  `--cert` / `--key`) when both exist and match. Otherwise aasdk's built-in pair is used (O=JVC Kenwood, valid
  to 2045). The log says which one is in use, with only the certificate's name and dates, never the key or the PEM
  text. A pair that does not match is logged as a warning and the built-in one is used. aasdk's Cryptor reads its
  PEM text through `TlsWrapper`, which hands it the files' contents instead (`Transport.cpp`).
- **Channel numbers.** Video is 3, audio 4, 5 and 6, the microphone 7, input 8 and sensors 1. aasdk fixes the
  input and sensor channels. The phone learns all of them from service discovery.
- **Service discovery.** Key codes are sent packed, as protobuf encodes them. There is no Bluetooth service, so
  the car's own hands-free handles calls. aarecvd asks for 30 fps and left-hand drive (`--fps`,
  `--driver-position`).
- **Pings.** The phone's pings are answered in plain text, as aasdk does.
- **Optional services.** Phone status, media playback status and navigation status are off by default
  (`--status-services`).
- **aasdk logging.** aasdk's own log (`--aasdk-log`) is off by default. Messages aasdk does not handle are not
  logged by aarecvd.
