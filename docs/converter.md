# Cue Converter

`cue-convert` turns a REAPER project into a Cue performance and a Cue
performance into a REAPER project. Drop a `.RPP` or a `.cue` on its window,
or click to choose one; the other file is written beside it (as
`Song (2).RPP` when `Song.RPP` is already there, so it never replaces the
project you started from) and the window lists what it left out.

```sh
cue-convert                    # the window
cue-convert Song.RPP           # the window, converting Song.RPP straight away
cue export-rpp show.cue [out.RPP] [--rig rig.json]   # the same .cue -> .RPP without a window
```

It is built from the same code as `cue` itself (`src/app/convert.cpp`,
`rpp.cpp`, `rpp_export.cpp`, `realearn.cpp`), so a change to the `.cue`
format reaches the converter in the same commit, and the tests convert
performances both ways on every build.

## REAPER project to .cue

As `cue import-rpp` (see [config.md](config.md#importing-from-reaper)),
saved as one `.cue` file.

## .cue to REAPER project

| Cue | REAPER |
| --- | --- |
| a scene | a folder of tracks (no folder when there is only one scene) |
| a chain | a track, armed and monitoring the chain's input |
| the main line and each branch | a pair of the track's channels: main line 1-2, the first branch 3-4, ... A branch's first plug-in reads its parent's channels and writes its own, where it leaves its parent |
| an output | a mono hardware send of the left channel of the line it is tapped on, to the output's channel in the rig profile |
| a side-chain | the source plug-in also writes a spare channel pair, which the keyed plug-in's side-chain pins read |
| CLAP and VST3 plug-ins | the same plug-ins with their saved states, bypass and names |
| pedals | one ReaLearn instance on the **Cue pedals** track (below) |

Left out, and listed: Cue's built-in plug-ins, warm chains and crossfades,
`bypass_mode = mute`, the parameters a chain sets after loading a plug-in
(its saved state is kept), and a tap in the middle of a line (REAPER takes
the end of that line). REAPER runs every track at once, so all chains but
the first start muted.

A VST3 plug-in needs REAPER's number for it, which comes from REAPER's
plug-in list (`~/.config/REAPER/reaper-vstplugins64.ini`). Converting on a
machine without REAPER writes 0, and REAPER should still find the plug-in by
its class ID.

### Pedals in ReaLearn

ReaLearn is laid out the same way for every performance:

- **Parameter 1, Chain**: chain *k* (counted through every scene) is *k*/100.
  A chain's pedal or program change sets it, and each chain's track is
  unmuted while it holds that chain's value and muted otherwise. With more
  than one scene, a chain's pedal works only while its scene plays. The up
  and down pedals step through the chains unless something else uses them.
- **One parameter per discrete control** (Wah, Effect 1...): its value *i*
  is *i*/100. The control's pedal steps it, wrapping if the control wraps,
  and `enable = ... when ...` lines switch those plug-ins on and off from it.
- **On select**: a chain's `do` actions run when parameter 1 reaches it:
  setting controls, switching plug-ins, sending MIDI, running groups.
- **Triggers**: pedals, controllers, program changes and notes doing the
  same actions; `when` and `scene` become ReaLearn activation conditions.
- **FCB1010 (EurekaPROM)**: each pedal's LED follows its chain or control,
  `[fcb]` LED lines that read one control or the chain are kept, and
  selecting a chain writes its display letters.

Not in REAPER, and listed: continuous controls, `follows` and `drive`,
`mute` (muting the outputs, as the tuner does), display lines in `[fcb]`,
conditions that read more than one control inside an `enable`, and pedal
releases.

ReaLearn listens to and lights REAPER's MIDI device 1. If your controller is
another device, pick it in ReaLearn's input and output once.

### Back again

The project's notes carry the performance's pedals, names and settings, and
converting the project back to a `.cue` takes them from there: a performance
comes back as it went, and the converter's own ReaLearn mappings are not read.
Pedals changed in ReaLearn are therefore not read back; change them in Cue.
Any other ReaLearn mappings in the project are read as simple mappings are.

### A pedal file instead of ReaLearn

A project whose ReaLearn works through formulas and parameters (like the
template) keeps few of its pedals. Choose a Cue pedal file under the drop area
(**Pedals for .RPP**, or `--mapping` on `cue import-rpp`) and every `.RPP`
converted takes its pedals, controls and FCB settings from that file instead,
ignoring the project's ReaLearn. **Use ReaLearn** goes back to reading it. The
choice is remembered.
