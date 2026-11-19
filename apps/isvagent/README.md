# isvagent

isvagent is the launcher. It takes kronos' ISV agent slot, the head unit's built-in auto-launch hook: with Test
Mode's "ISV On" set, kronos starts `\Storage Card\ISV\ISVAgentKronos.exe` at boot, after LG's own apps. isvagent
changes nothing on the head unit. It:

- waits up to 10 minutes for the Pi's stick (`\MD`), then starts the program named in `\MD\isv\launch.txt`
- logs to `\Temp\isvagent_log.txt`, and to `\MD\isv\isvagent_log.txt` once the stick appears
- keeps a hidden window titled `isvAgent` and logs the messages sent to it (kronos forwards hard keys and power
  events to the agent), and keeps running so kronos sees the agent alive

It copies nothing. vagent and aaview each copy themselves to `\Temp` and restart from there, so the stick can be
unplugged under them.

## The bar

A bar at the top left of the screen shows isvagent's version and what it did, for example "started vagent.exe".
vagent's bar is at the top right. The bar is not topmost, so Android Auto and other full-screen programs cover it. A line
`bar=brief` in `launch.txt` hides it 30 s after the start; the release stick has this line.

Tapping the bar asks "Stop isvagent?". Stopping it frees `\Storage Card\ISV\ISVAgentKronos.exe` so a new version can
be copied over it (a running program's file gives a sharing violation). Whatever it started keeps running.

## launch.txt

The first line names the program, optionally followed by its arguments:

```
\MD\vagent\vagent.exe                         dev build: vagent, which starts aaview from its autostart.txt
\MD\aaview\aaview.exe                         release build: aaview itself (pi/make_release_stick.sh writes this)
"\Storage Card\some folder\tool.exe" one two  quotes for a program path with spaces before its end
```

Without quotes, the program ends at the first `.exe` that is followed by a space or the end of the line. If there is
no `.exe` in the line, the whole line is the program.

## Install (Explorer Mode, by hand)

1. Copy the folder `\MD\ISV_install\ISV` into `\Storage Card` so you get `\Storage Card\ISV\ISVAgentKronos.exe`
   (kronos' name) and `\Storage Card\ISV\ISVAgent.exe` (vulcan's name). They are the same file, and only one copy runs.
2. Open Test Mode (hold SETUP, tap the corners UL, LL, UR, LR, code 95889588) and choose **ISV On**.
3. Restart the head unit. The bar appears over the LG screen shortly after boot, and the program in `launch.txt`
   starts by itself.

To undo: Test Mode, ISV Off, and delete `\Storage Card\ISV`.

## Build

Run `.\build.bat` (the same toolchain as aaview: Visual Studio 2008 and the Windows CE 5.0 SDK, found by
`tools/ce-env.bat`). The output is `out\isvagent.exe`.
