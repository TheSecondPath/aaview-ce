# aaview

aaview is the head unit's half of the Android Auto receiver. The Pi handles everything that faces the phone
(Bluetooth, Wi-Fi, TLS and the Android Auto protocol). aaview shows the video, plays the audio, records the
microphone when the phone asks for it, and sends touches and keys back to the Pi.

It runs from `\MD\aaview\aaview.exe`. On start it copies itself to `\Temp` and restarts from there, so the stick can
be unplugged under it. The release build starts it from the launcher (`apps/isvagent`); the dev build starts it from
vagent's `autostart.txt`.

## Usage

```
aaview.exe [/auto] [hold|nohold] [noicon] [icon=X,Y] [ts0|tsN] [clock] [abufN] [arateN|noarate] [micN|nomic]
           [nograb] [norestore] [yuvcamN]
```

| Option | Meaning |
|---|---|
| `/auto` | what the launchers pass; it changes nothing |
| `hold` / `nohold` | take the foreground back when another full-screen window takes it (default `hold`) |
| `noicon` | no floating icon |
| `icon=X,Y` | where the floating icon sits |
| `tsN` / `ts0` | timestamp = arrival + N ms (default 40); `ts0` sends no timestamps |
| `clock` | keep the graph clock (by default frames are drawn as soon as they are decoded) |
| `abufN` | media audio jitter buffer in ms (default 600) |
| `arateN` / `noarate` | where the audio clock correction starts, in ppm (default 7000); `noarate` plays samples as they come |
| `micN` / `nomic` | wave-in device for the assistant's microphone (default 1); `nomic` never records |
| `nograb` | leave the head unit's audio source alone |
| `norestore` | leave the audio source on USB media when the phone goes |
| `yuvcamN` | cap on the decoder's frame region, in MB (default 6) |

The same list is at the top of `aaview.cpp`.

## The stream LUN

Everything between the Pi and aaview goes through a raw "stream" LUN on the Pi's USB gadget: `/srv/stream.img`,
4 MB (8192 sectors), LUN 2 of the stick. It has no file system. aaview finds it by its size and reads and writes it by
sector through the store handle (`DISK_IOCTL_READ`, uncached). The link carries about 11.7 MB/s from the Pi to the
head unit, and Android Auto video needs roughly 0.5 MB/s.

| Sectors | Written by | Contents |
|---|---|---|
| 0 | Pi | header: ring positions, video size, flags, the Pi's clock, the pairing code |
| 1 | aaview | status: read position, frame counters, the last 16 touch and key events with a cumulative count |
| 2 to 13 | aaview | microphone audio while the phone wants it |
| 16 to 4111 | Pi | the ring (2 MiB): H.264 access units and PCM audio records |
| 4112 to 8191 | | unused |

The byte layout is at the top of `aaview.cpp`. The Pi's side is `pi/aarecvd/src/Projection/StreamRing`.

aaview publishes its read position and the Pi never overwrites data that has not been read. When the ring is full
the Pi drops frames up to the next IDR. If aaview falls more than 384 KB behind and a newer IDR is waiting, it skips
ahead to it. A record bigger than one read of the ring, or an access unit bigger than the decoder's buffer, is
skipped, and video then waits for the next IDR.

## Video

aaview feeds the access units to the Alchemy hardware decoder through its own DirectShow source filter
(`dshow_filters.h` and `dshow_min.h`), and the Alchemy renderer shows them full screen. The decoder's
working memory is in region 1 and its frames are in region 4, capped at 6 MB. The Pi rewrites the SPS level_idc to
12 so the decoder's picture pool fits.

The renderer never enables its video plane, so aaview enables overlay plane 1 itself. It does so again when the
rear camera is over, when its window comes back in front of another full screen, and on an icon tap, because the
plane can be left off after reversing.

## Audio

Each audio stream (media, speech, system) plays through waveOut. Media audio has a jitter buffer of 600 ms by
default. "audio stream 0: ran dry" in the log is a hiccup, usually a Wi-Fi stall longer than the buffer.

The head unit's "48000 Hz" output plays about 0.6% fast, so every stream is resampled by 1 + ppm/1e6 (4-point
cubic). The rate has two parts:

- A measured part: the device's play position (`waveOutGetPosition`) against `GetTickCount` over at least a minute
  of uninterrupted music. It is logged as "the head unit plays N ppm fast (measured over M s of music)" and kept in
  `aaview_clock.txt` for the next start. `arateN` overrides it.
- A proportional part, steered by how much music is queued, sampled every 50 ms.

About once a minute the log shows "audio clock: N ppm ...".

When the phone connects, aaview switches the head unit's audio source to USB media, since the head unit only plays
waveOut audio from that source. When the phone leaves, it asks for Bluetooth Audio. `nograb` and `norestore` turn
these off.

## Microphone

When the Pi's header has flag bit 3 set (the phone's assistant is listening), aaview records wave-in device 1 at
16 kHz mono and writes 62.5 ms chunks to sectors 2 to 13 until the flag clears. `micN` picks another device and
`nomic` turns it off.

## Touch and keys

Touches on aaview's window (down, move, up) and keyboard keys go into the status sector as events. Steering-wheel
and dash keys, the headlight messages and the car's phone events come from `MatMessage.dll` (`apps/matmsg`), which
runs inside kronos and posts them to aaview's window (class `aaview`):

| Message | wParam, lParam |
|---|---|
| `WM_APP+0x20` | hard key: code, data |
| `WM_APP+0x21` | another ISV event (power, AV off, eject, ...): message, code |
| `WM_APP+0x22` | lights: phoenix command (ILL_ON 0x77, ILL_OFF 0x78, NIGHT_MODE 0x7c), data |
| `WM_APP+0x23` | the car's phone (hyperion's call and Bluetooth audio messages): function, data |

Keys, ISV events and lights are passed on to the Pi as events. The event actions are listed at the top of
`aaview.cpp`.

## Sharing the screen with the car

With `hold`, aaview takes the foreground back when one of LG's full screens takes it. It leaves the screen alone in
two cases.

**Rear camera.** The camera's picture is on its own overlay plane, but venus.exe draws the guide lines in an ordinary
full-screen window. From the moment a window of venus.exe or layer_control.exe takes the foreground, aaview does not
raise or activate its window. venus opens two full-screen windows within a second, so the camera counts as up while
any window of those apps has the foreground or shows full screen. It is over when another full screen takes the
foreground, when none of their full-screen windows has shown for 1 s, or when aaview has had the foreground for 2 s.
The log says "the rear camera's screen is up (venus.exe): leaving the screen alone" and later "the rear camera's
screen is over".

**The car's phone.** Calls stay on the car's Bluetooth hands-free and LG's phone apps (hermes.exe, hyperion.exe)
show them. While one of their full screens is in front, aaview does not take the screen back and does not switch the
audio source. The floating icon brings Android Auto back. A source switch held back meanwhile is done 2 s after the phone screen
goes.

A phone app's full screen counts as a call only if it took the screen from Android Auto. One that was simply there
(at start-up, when projection begins, or put back by LG after reversing) is LG's Bluetooth Audio or mode screen, and
aaview treats it as an ordinary screen: "(hyperion.exe: LG's mode screen, not a call: ...)". A call already in
progress when projection starts is covered by Android Auto.

For each full-screen window of a phone app, the log lists its child windows by class, id and position, never their
text.

## The floating icon

While Android Auto is not on screen, a small icon floats over LG's screens. Its colour is the state:

| Colour | State | A tap |
|---|---|---|
| grey | the Pi isn't answering (stick gone, Pi off or still starting) | |
| blue | the Pi is there, no phone yet | asks the Pi to connect to the phone |
| green | the phone is connected but not projecting | brings Android Auto back |
| amber | tapped, waiting (up to 6 s) | |
| red | the decoder's memory is missing: no picture until the head unit restarts, sound still plays | tries again |
| purple | Bluetooth pairing is open | |

Drag the icon to move it. Its position is kept in `aaview_icon.txt`, and without one it sits in the middle of the
top edge. It is hidden while the rear camera is up.

To pair a phone, hold the icon still for 3 s. It turns purple with "BT" and the Pi is discoverable and pairable for
3 minutes. When a phone pairs, its 6-digit code shows on the icon in two rows of three digits: confirm on the phone
only if it shows the same code. Holding the icon again closes pairing, and the Pi closes it 20 s after a phone pairs.
A hold sends event 9 (x = 1 open, 0 close). The Pi's header says pairing is open with flag bit 4 and carries the
code at byte 52. The icon is shown while pairing is open, even during projection.

## Ending aaview

aaview never ends on its own. When the phone leaves, Android Auto's Exit is used, the Pi's receiver stops, the decoder
fails or a `WM_CLOSE` arrives ("WM_CLOSE ignored" in the log), it hides the picture, shows the floating icon and
waits. In the release build nothing else is there to start it again.

The only way out is the Pi's quit flag: `aarecvd --quit-aaview` on the Pi, or the Stop button on the dev build's web
page. aaview then takes the decoder down in order. The release build has nothing that sets the flag.

The Pi rewrites the stream header every 2 s. If the header still says "projecting" but has not changed for 20 s, the
Pi's receiver has stopped or hung with the stick still plugged in. aaview then stops projecting, gives the screen back
to the car and waits for the Pi to write again.

vagent's `kill aaview.exe` is a forced end for the dev build and a last resort. It leaves the decoder's memory
allocated, and the next aaview shows the red icon with no picture until the head unit restarts.

## The head unit's clock

The head unit comes up at 2009-01-01 after a reset and has no time source of its own. When the Pi's clock has been
set from the internet (the Pi on home Wi-Fi since it started), the Pi puts the time and its time zone's offset in the
stream header at bytes 44 and 48. Otherwise, or with `set_clock=off` in `veloaa.txt`, those are zero. If the head
unit's local time is more than 3 minutes off, aaview sets it (`SetLocalTime`, then `WM_TIMECHANGE` to every window),
at most 3 times a run and a minute apart, so it never fights something that sets it back. The log says "clock: the
head unit said ..., set to ...".

Away from home the Pi has no trustworthy time (it has no battery clock), so nothing is set. The phone doesn't help:
its Android Auto pings carry its time since boot, not the date.

## Log

`aaview_log.txt` is in the exe's folder on the stick. At 4 MB it becomes `aaview_log.old.txt` (replacing the one
before) and a new one is started. Lines collect in memory and the main thread writes them about once a second,
because a write to the stick can stall the stream reader for seconds.

The log level is set on the Pi (`veloaa.txt` or `/etc/aarecv/loglevel`, see `pi/README.md`) and arrives in the
header flags, bits 16 and 17:

- everything (debug), also when the Pi sets nothing
- no chatter (info): no statistics, window tracking, keys or graph building
- only what goes wrong (warning and error), plus each start's banner

Each start logs everything until the first header is read. A change shows as "log level: ... (set on the Pi)" and
takes effect within about 7 s without a restart.

In any 5 s with a reader stall of 150 ms or more, or under 150 ms of music queued, the log has "longest stall N ms
(decoder, disk, log write); least music queued M ms". This tells hiccups made on the head unit from audio that
arrived late from the phone, which the Pi logs as "media audio late".

## Build

Run `build.bat` (Visual Studio 2008 and the Windows CE 5.0 SDK, found by `tools/ce-env.bat`). The output is
`out\aaview.exe`.
