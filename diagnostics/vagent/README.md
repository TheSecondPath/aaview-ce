# vagent

vagent is the dev build's remote agent on the head unit. Start it once and it runs job files from the USB stick,
writes the results back to the stick, and keeps running while the stick is removed and reinserted. The Pi, acting as
the stick, swaps jobs and results over Wi-Fi.

## Stick layout

```
<stick>\vagent\vagent.exe
<stick>\vagent\autostart.txt      programs to start once vagent is up
<stick>\vagent\jobs\*.job         run in name order; renamed to *.job.done when finished
<stick>\vagent\results\           <job>.txt, plus screenshots and collected files
<stick>\vagent\status.txt         idle / running <job> line N: <command> / waiting; heartbeat counter
<stick>\vagent\vagent_log.txt     agent events (also kept in \Temp\vagent_log.txt)
```

## Running

1. Start `<stick>\vagent\vagent.exe`, either from Explorer Mode or from the launcher (`apps/isvagent`, with
   `\MD\vagent\vagent.exe` in `launch.txt`).
2. vagent copies itself to `\Temp` (RAM) and restarts from there. A yellow-on-black status strip appears at the top
   right of the screen.
3. To stop vagent, tap the strip and answer Yes, or use the `quit` job command. Restarting the head unit also
   removes it.

## autostart.txt

`autostart.txt` lists programs to start once vagent is up, one `exe [args]` per line. Blank lines and lines starting
with `#` are skipped. Programs that keep a single instance, like aaview, exit if they are already running, so a
vagent hand-over does not start them twice. The dev stick's file starts aaview.

## Job commands

One command per line; `#` starts a comment. Quote paths that contain spaces. Relative paths are relative to the
stick root (the folder that holds `vagent\`).

| Command | What it does |
|---|---|
| `log <text>` | write a line to the results file |
| `timeout <seconds>` | time limit for the following `run` commands (default 300) |
| `run <exe> [args]` | start a program and wait for it; it is killed at the time limit |
| `start <exe> [args]` | start a program and don't wait |
| `kill [name]` | end the last program this job started, or every process with that exe name |
| `ipc <title> <wParam hex> [lParam hex]` | post one LG IPC message (0x8096) to the window with that title |
| `sleep <ms>` | wait |
| `screenshot [label]` | save the desktop as `<job>_<label>.bmp` in results |
| `procs` | list running processes |
| `collect <file>` | copy a file into results as `<job>_<file name>` |
| `copy <src> <dst>` | copy a file; the destination must be on the stick or under `\Temp` |
| `dir [path]` | list a folder or pattern (names, sizes, dates, d/r/h attributes) |
| `type <file>` | copy a text file (first 32 KB) into the results |
| `del`, `mkdir`, `rmdir <path>` | file operations, only on the stick or under `\Temp` |
| `reg <HKLM\key> [value]` | read-only: list a registry key's subkeys and values, or one value |
| `mem` | RAM and free space (stick, `\Temp`, Storage Card 1 to 3) |
| `waitreinsert <sec>` | wait until the stick is removed and inserted again |
| `quit` | stop vagent after this job |

`kill` refuses CE core processes, LG's `kronos.exe` and vagent itself. The same list is at the top of `vagent.c`.

From the Pi, `vjob 'cmd' ...` runs commands as a job and prints the results (see `pi/README.md`).

## Safety

vagent never writes to `\Storage Card` or the registry. `copy`, `del`, `mkdir` and `rmdir` only write inside the
stick or `\Temp`. The match ends at a folder boundary (`\MD` does not admit `\MD2\...`), and a path with a `..` step
or a forward slash is refused.

## Screenshots

Avoid `screenshot` while the LG shell is running: MyShell's photo viewer opens any BMP it finds on the stick.

Screenshots capture the desktop only. The video overlay planes are composited by the display hardware, so video
shows up only in camera footage.

## Build

Run `.\build.bat` (the same toolchain as aaview). The output is `out\vagent.exe`.
