# The Cue app

`cue-app` shows what a running Cue is doing and plays it by touch or mouse.
It talks to `cue run` over its control socket ([control.md](control.md)), so
the sound never depends on it: close it, or let it crash, and the engine plays
on; open it again and it picks up where things are.

```sh
cue run show.cue          # the engine, as before
cue-app                   # the app, on the same machine; it finds the engine itself
```

Or one command, for the stage computer:

```sh
cue-app --start ~/shows/friday.cue --fullscreen
```

`--start` starts `cue run` with that performance when no engine answers, in a
session of its own so it keeps playing when the app closes. What the engine
prints goes to `~/.local/state/cue/engine.log`; while the app waits for it,
it shows the engine's last lines (or its errors). When an engine is already
running, `--start` leaves it alone.

To look at or change a performance on a computer without the sound card:

```sh
cue-app --start show.cue --audio none
```

## Views

**Stage.** The chain playing, as big as the screen allows, with the FCB's
display beside it and the chains before and after it. The levels of every
input and output. The tuner strip. A button for each chain of the scene, with
its pedal and a dashed **warm** tag on the chain kept warm, and a button for
each control: a switch shows ON or OFF, a control with values shows them
all, a continuous one is a slider. A tap does what the pedal does, so the
screen and the FCB always agree.

**Tuner.** The built-in tuner (the TwoStorms Tuner's pitch detection) on the
performance's tuner input, before any plug-in: the note, cents, hertz and the
string, and a bar from -50 to +50 cents with the in-tune zone. Green is in
tune. The mute button goes through the control that mutes (your tuner pedal),
so its LED and the screen agree. When the outputs are muted the tuner view
comes up by itself and goes back when they are live again (Settings can turn
this off).

**Chains.** A chain's plug-ins as blocks and wires, as the routing board
draws them: inputs and outputs are rectangles, Gain Stage a circle, Act a
small circle, other plug-ins hexagons. Green is a plug-in that is on, or a
wire carrying sound to an output in the chain playing; grey is bypassed or
silent; orange is what you selected. Tap a block to switch it on or off and
move its parameters with sliders. **Play this chain** plays the chain shown;
**Save** saves the performance, plug-in settings included, to its `.cue`
file. Moving and wiring blocks comes with the routing editor.

**Settings.** For this machine: the view the app starts in (Stage, Tuner or
Chains), CPU and margin in ms or as % of what a block allows, the size of
everything, the tuner while muted, full screen. For the performance: the
tuner's range, reference, tolerance and note names, saved with it.

## Touch, mouse and keys

Everything is a tap: it acts when the finger lifts inside the button, so a
finger that slides off cancels. Sliders follow the finger from the first
touch. Lists scroll by dragging or with the mouse wheel. The layout fits any
screen, landscape or portrait; Settings > Size makes everything bigger or
smaller on top.

| Key | Does |
| --- | --- |
| Left, Right, Page Up, Page Down | previous or next chain (on the stage view) |
| T | the tuner, and back |
| Esc | the stage view |
| Ctrl+S | save |
| F11 | full screen |
| Ctrl+Q | quit the app (the engine plays on) |

## Options

| Option | |
| --- | --- |
| `--start FILE` | start `cue run FILE` if no engine answers |
| `--audio none` | with `--start`: without a sound card |
| `--view stage\|tuner\|chains` | the view to open with, this time |
| `--fullscreen`, `--windowed` | this time |
| `--scale N` | size of everything, 0.5 to 2.5 |
| `--socket PATH` | the engine's control socket, as `cue run --socket` |
| `--size WxH`, `--screenshot FILE` | for testing: the window's size; save a picture once the engine answered, then quit |

The app's settings are kept in `~/.config/cue/app.ini` (they belong to the
machine, not the performance).

## Starting on boot

On the stage computer, start the app with the desktop session, for example
with a file `~/.config/autostart/cue.desktop`:

```ini
[Desktop Entry]
Type=Application
Name=Cue
Exec=/home/you/cue/bin/cue-app --start /home/you/shows/friday.cue --fullscreen
```

## What it needs

Qt 6 (Widgets and Network) and its X11 platform plug-in. On Debian 13 and AV
Linux MXE-25 these are usually there already; if `cue-app --version` says a
`libQt6` library is missing:

```sh
sudo apt install qt6-qpa-plugins libqt6widgets6t64 libqt6network6t64
```

(On a distribution where those names are not found, `libqt6widgets6 libqt6network6`.)
The fonts (Inter, Barlow Condensed, JetBrains Mono, SIL Open Font License)
are built into the app.
