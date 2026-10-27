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

**Tuner.** The TwoStorms Tuner, built in: its pitch detection on the
performance's tuner input, before any plug-in, drawn as the plug-in draws it.
The note with its sharp or flat and octave, chevrons on the side it is off to
(more of them the further off), the cents and hertz under it, the needle from
-50 to +50 cents over the in-tune zone, and the strobe, which drifts with the
error and stands still in tune. Green is in tune; a note that has gone is
drawn dark. The stage view's strip is the same tuner, small and plainer: the
note with its chevrons and the needle, nothing else. The **Mute**
button goes through the control that mutes (your tuner pedal, whose number it
shows), so its LED and the screen agree; while muted the tuner is framed in
red. Muting silences the inputs, not the outputs: what is ringing (a delay, a
reverb) rings out, and a chain that has gone quiet costs no CPU until the
inputs come back. When the inputs are muted the tuner view comes up by itself
and goes back when they are live again (Settings can turn this off).

**Chains.** A chain's plug-ins as blocks and wires, as the routing board
draws them: inputs and outputs are rectangles, Gain Stage a circle, Act a
small circle, other plug-ins hexagons. Green is a plug-in that is on, or a
wire that carries sound to an output (or would, in a chain not playing);
grey is bypassed or silent, and every wire while the inputs are muted;
orange is what you selected. Tap a chain in the list to see it, tap it twice
to play it. The wheel, two fingers or the **−**,
**Fit** and **+** buttons zoom; dragging open space or two fingers move it; a
double tap on open space fits the whole chain again. Tap a block to move its
parameters with sliders; tap it twice to switch it on or off. Tap a value to type
one ("-3 dB", "250 ms": the plug-in reads it), and tap it twice to set it back to
its default. A slider moves while the plug-in is switched off too. It shows the parameters
you picked, in your order (drag a row by its grip to move it), and always the
ones a control or trigger moves, purple, with what moves them; until you
pick, the first eight. **Show all parameters** lists every one in the
plug-in's order, checked if shown: check the ones to show. Long lists fold by
the plug-in's sections and have a search box. **Open editor** opens the plug-in's own
window, the one it has in a DAW: Neural DSP through yabridge, the TwoStorms
plug-ins, anything with an editor. It opens on the screen of the computer
running the engine, which on stage is the one in front of you, and stays
over the app (full screen too) until you close it; tapping the app does not
hide it. What you change there is heard at once and kept by **Save**. The sound never waits on
an editor: it is drawn between blocks, never on the audio thread, and
closing one changes nothing about what is heard. **Play this chain** plays the chain shown.

The chain view is also where a chain is wired. A plug-in's inputs are the
dots on its left, its outputs the dots on its right, and the small triangle
low on its left is its key (side-chain input), for the plug-ins that have
one. Drag a block to move it; it stays where you put it. Drag from a dot to
another to draw a wire: from the input or a plug-in's output, to a plug-in
or an output. One output can feed any number of wires, each a copy of the
same sound, and wires into one input add together. A wire that would make
a loop is refused. Tap a wire to select it (orange): the panel gives its
**level** (silent to +12 dB, 0 dB at the tick), **width** (100% is stereo
as it comes, 0% folds left and right to mono) and **pan** (a balance: it
turns one side down, never moves sound across), in that order; a double tap
on a fader sets it back. **Mute**, **Insert** and **Delete** are under them.
A wire whose level, width or pan is changed is drawn thick, with a tag
saying what is changed (Settings can hide the tags). The **+** on the
selected wire, or **Insert**, puts a plug-in on it: the blocks after it make
room. **+ Plug-in** adds one to the board unwired. Both offer your
favourite plug-ins, those the performance already has and the built-ins
(gain, drive, lowpass, delay), and **Browse all plug-ins** for the rest. **Remove** on a block's panel, or Delete,
takes a plug-in out with its wires. A plug-in that the input does not reach
or that no output hears is loaded but not processed, drawn grey and marked
"not connected". The sound changes as you wire: a playing chain fades out
and back in over 10 ms, a level, width or pan moves smoothly with no fade at
all, and a chain not playing changes at once.

The column on the left lists one scene's chains: the scene is picked at its
top (a green dot when it is the scene playing), and the list shows which
chain is playing and which is kept warm. **+ Chain** adds an empty chain to
the scene, its input wired to every output. **Copy from scene…** lists the
other scenes' chains: the one picked is copied into this scene with its
plug-ins as they sound now, its wiring and its settings (its pedal too, if
no chain here has it). **+ Scene** adds a scene after this one, with one
empty chain. All of it happens while a chain plays: a new chain is loaded and
warmed beside it, and nothing you hear stops.

Under the board are the shown chain's settings:
its **name**, the FCB pedal that **selects** it (the menu says what else
each pedal does; a pedal given to it is taken from the chain here that had
it), its 2-letter **FCB display**, the chain **kept warm while it plays**
(the next one, none, or one you pick, so the switch to it is instant), and
**when selected, do**: the actions done each time it is picked, each a chip
(tap one to take it out or move it; **+ Action** offers the controls' values
or one typed, such as `group Reset`). **Duplicate** copies the chain just
after itself; **Move** puts it earlier, later or into another scene; **Take
out** takes it away (not the chain playing, nor the last of a scene). The
**Scene** tab beside the title gives the scene's name, pedal and display the
same way, and its own **Duplicate** (every chain, plug-in settings, pedals and
the triggers that work only there), **Move** and **Take out**.

**Undo** takes back what you changed here (a slider, a bypass, the parameters
picked, a wire, a plug-in added, moved or taken out, a chain or scene added,
copied, set, moved or taken out, and each knob turned in a plug-in's own
editor, from where it was grabbed to where it was let go), showing where, and
**Redo** makes it again; what a pedal changes is not in it. A plug-in taken out comes back with its settings and its wires;
a chain or a scene with everything it had.
**Save** saves the performance, plug-in settings, wiring and where each
block is drawn included, to its `.cue` file.

**Plug-ins.** Every plug-in on the computer running the engine: CLAP and
VST3, native and Windows ones through yabridge. Opening the view scans what
is new or changed since the last time; **Rescan** opens every plug-in again.
Each file is opened by a helper of its own, one at a time, so a plug-in that
crashes or hangs is skipped and listed with the reason (**Skipped**), never
taking Cue down; **Try again** opens it once more. On the left, the vendors
and the collections (**Favourites**, **In this performance**, **Skipped**);
over the list, a search (names, vendors, kinds and file names) and the
formats. Tap a plug-in to see it: its id and file, its inputs (with a
side-chain or not), whether it has an editor of its own and a MIDI input
(for a Windows plug-in, known once it is added: the scan does not start Wine
for each). The orange button adds it to the chain the chain view shows, on
its selected wire if one is, and goes back there; a double tap on a row
does the same. **Folders…** shows where Cue looks (`~/.clap`, `~/.vst3`,
`/usr/lib` and `/usr/local/lib`, `CLAP_PATH`, `VST3_PATH`) and keeps
folders of your own, on the engine's computer. Favourites are kept by the
app, in its settings.

**Settings.** For this machine: the view the app starts in (Stage, Tuner or
Chains), CPU and margin in ms or as % of what a block allows, the size of
everything, the tuner while muted, full screen, and whether the routing
board tags the wires whose level, width or pan is changed. For the performance: the
tuner's range, reference, tolerance and note names, saved with it.

## Touch, mouse and keys

Everything is a tap: it acts when the finger lifts inside the button, so a
finger that slides off cancels. Sliders follow the finger from the first
touch. Lists scroll by dragging or with the mouse wheel. The chain view's
canvas zooms with the wheel or two fingers and moves when you drag open
space. The layout fits any
screen, landscape or portrait; Settings > Size makes everything bigger or
smaller on top.

| Key | Does |
| --- | --- |
| Left, Right, Page Up, Page Down | previous or next chain (on the stage view) |
| T | the tuner, and back |
| Esc | the stage view (on the chain view, first lets go of what is selected; on the plug-ins view, back to the chains) |
| Delete, Backspace | on the chain view: take away the selected wire or plug-in |
| Ctrl+S | save |
| Ctrl+Z | undo |
| Ctrl+Shift+Z, Ctrl+Y | redo |
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
