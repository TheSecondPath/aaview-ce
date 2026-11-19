# velosterAA (Beta builds)

Wireless Android Auto on the factory head unit of a 2014 Hyundai Veloster (base, non-nav; LG, Windows CE 6, MIPS),
using a Raspberry Pi Zero 2W, however a pi that has 5ghz wifi may be preferable for performance and stability reasons. Most pi's will not have enough power from the USB port and will need a second power source in PWR.

The Pi plugs into the car's USB port. To the head unit it is a USB stick, and to the phone it is a wireless Android Auto receiver.
The process is Bluetooth handshake -> nudge to Wi-Fi access point for the session -> Android Auto over wifi.

A small program on the head unit, started from the USB stick, shows the phone's screen, plays its sound and sends back touches, keys and the
cabin microphone. Flow:

Android phone<-All->Pi Android Auto receiver<-video,audio,inputs->USB Stick(RAM partition)<-audio,video,touch->aaview
                                                                                          ↖︎-buttons,lights,signals->matmessage

## Features

- Wireless Android Auto on the car's own screen without any additional APK and only minimal hardware additions.
- Touch, music and navigation, navigation audio cues, and the dash and steering-wheel keys.
- Day and night mode follow the headlights.
- The car's own phone calls keep working, and music comes back after a call. (still beta, please report odd behaviors or issues.)
- The rear camera still takes over the screen when you reverse.
- The head unit's firmware is not modified. One launcher is copied to its storage card (ISV folder) and switched on in Test Mode, and switching it off again undoes it.
- Settings live in one text file on the SD card, which you can edit with Notepad.

## Supported hardware

- **Car:** 2014 Hyundai Veloster with the base LG head unit (no navigation). Other LG units on Windows CE 6 with the same software may work but have not been test yet.
- **Board:** Raspberry Pi Zero 2 W. It needs USB device mode, Bluetooth and Wi-Fi.
- **Phone:** an Android phone that supports wireless Android Auto. 
- A micro-SD card (8 GB or more) and a micro-USB data cable from the Pi's **USB** port (not PWR) to the car.

## Install and run

There are prebuilt releases now and I will make a more generalized version so this may work with any Windows CE headunit with 
some additional pieces for each car or version. 

**Start at step 3** when using the prebuilt versions.

To build the two halves yourself:

### 1. Build the receiver (any PC with Docker)

```
pi/aarecvd/build.sh arm64
```

This takes several minutes. It fetches aasdk at a fixed commit and makes `pi/aarecvd/out/aarecvd-arm64`. See
`pi/aarecvd/README.md` for the details.

### 2. Build the head-unit programs (Windows)

These need Visual Studio 2008 Professional SP1 (with its Smart Device support) and the Windows CE 5.0 Standard SDK,
which builds MIPSII programs the head unit runs. `tools/ce-env.bat` finds them: set `TOOLROOT` to a folder holding
`vs9` and `sdk`, or set `VS9` and `SDKROOT` to where they are installed. Then run `build.bat` in each of these folders:

| Folder | Output |
|---|---|
| `apps/aaview/` | `out/aaview.exe`: Android Auto's screen, sound, touch and microphone |
| `apps/matmsg/` | `out/MatMessage.dll`: passes keys, lights and call messages to aaview |
| `apps/isvagent/` | `out/isvagent.exe`: the launcher the head unit starts at boot |

### 3. Set up the Pi

1. Write **Raspberry Pi OS Lite (64-bit)** to the SD card with Raspberry Pi Imager. In Imager's settings, choose a
   hostname (for example `velosterAA`; the phone will see this name over Bluetooth), set a user name and password,
   turn on SSH, and enter your home Wi-Fi so you can reach the Pi for the install.
2. Boot the Pi and copy the `pi` folder to it, including `aarecvd/out/aarecvd-arm64`. Copy the three head-unit
   programs from step 2 into one folder on the Pi too, for example `~/bin`.
3. On the Pi:

   ```
   cd pi
   sudo sh install.sh --keep-wifi
   sudo sh make_release_stick.sh ~/bin
   sudo reboot
   ```

`install.sh` installs the packages it needs (it needs the internet once), switches the USB port to device mode and
sets up the services. `make_release_stick.sh` writes the stick image the head unit will see.

Use `sudo sh install.sh` without `--keep-wifi` if you want the Pi to stop joining your home Wi-Fi. the wifi will be used only for the phone, and auto time updates and SSH over Wi-Fi stops working. On 2.4ghz wifi only devices like the Pi Zero 2w to do both. To turn home Wi-Fi back on later, see
[Configurations](#configurations).

### 4. Set up the head unit (once)

1. Plug the Pi into the car's USB port and turn the car on.
2. Open **Explorer Mode**: hold SETUP, tap the screen's corners upper left, lower left, upper right, lower right.
Then depending on the car and firmware version enter the explorer mode code (2014 veloster on newest firmware is `73148320`)
3. Copy the folder `\MD\ISV_install\ISV` into `\Storage Card`, so that `\Storage Card\ISV\ISVAgentKronos.exe` and
   `\Storage Card\ISV\MatMessage.dll` exist.
4. Open **Test Mode** the same way, (2014 veloster code `95889588`), and switch **ISV** on.
5. Restart the head unit.

A cyan strip shows at the bottom left for a short time, and the Android Auto icon appears at the top center of the screen. It is grey
with no Pi, blue while it waits for the phone, and green when the phone is connected and purple in pairing mode (which you hold down 
on the icon for 3 seconds to start pairing).

**To undo:** in Test Mode switch ISV off, then delete `\Storage Card\ISV`. Nothing else on the head unit changes.

## First-time connection

1. With the car on and the Pi plugged in, hold the Android Auto icon on the head unit for 3 seconds, until it turns purple.
2. On the phone, open Bluetooth settings and pair with the Pi (the hostname you gave it) within 3 minutes.
3. Check that the code on the phone matches the one on the icon.
4. Android Auto starts on the car's screen. If the phone asks, allow Android Auto to connect wirelessly.

## Subsequent connections

The Pi starts with the car. As long as the phone's Bluetooth and Wi-Fi are on, it connects by itself, usually within a minute 
of starting the car.

## Configurations

Settings live in `veloaa.txt` on the SD card's boot partition. That is the `bootfs` drive when the card is in a PC, and 
`/boot/firmware/veloaa.txt` on the Pi. Changes take effect the next time the Pi starts.

| Key | Values | Default | What it does |
|---|---|---|---|
| `home_wifi` | `on`, `off` | `off` | join your home network when the car is parked in range |
| `home_wifi_ssid` | the network's name | blank | the home network's name, as the phone shows it |
| `log_level` | `error`, `warning`, `info`, `debug` | `info` | how much goes into the logs |
| `set_clock` | `on`, `off` | `on` | set the head unit's clock from the Pi's (only with home Wi-Fi) |

The Wi-Fi password is not kept in this file. Set the network up once on the Pi itself, either in Raspberry Pi Imager 
when you write the card or with `sudo nmcli device wifi connect <name> --ask`. `pi/README.md` has more on the commands.


### Getting logs

- **On the head unit:** aaview writes `aaview_log.txt` next to itself on the stick, at
  `\MD\aaview\aaview_log.txt`. You can copy it off in Explorer Mode.
- **On the Pi:** with home Wi-Fi on, SSH in and run:

  ```
  journalctl -u aarecvd -u rmwork-aa -u aa-ap -b
  ```

  To read the head unit's log on the pi, mount the stick image read-only:

  ```
  sudo mount -o ro,loop /srv/stick.img /mnt
  cat /mnt/aaview/aaview_log.txt
  sudo umount /mnt
  ```

LG's firmware and aasdk's source are not in this repo. aasdk comes with a head-unit certificate for Android Auto's TLS 
handshake. To use a different one, put it in `/etc/aarecv/hu.crt` and `hu.key` on the Pi, and never commit either file to the repo.

## Limitations

This has been tested in one car with one phone. Still being tested: the assistant's microphone, the switch back to the 
car's Bluetooth audio when the phone leaves, and occasional audio stalls. Other head units, cars and phones may behave differently.

## License

GPL-3.0, see `LICENSE`. The receiver links aasdk, which is GPL-3.0 too.
