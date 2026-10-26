# The Cue config

One INI file describes a whole **performance**: the audio and MIDI settings, the scenes in the order you play them, and what every pedal does. `cue check <file>` loads it and says what it understood; `cue check <file> --no-load` does the same without loading plug-ins, so it is quick and works away from the rig.

Comments start with `;` anywhere on a line, or `#` at the start of a line. Names (of scenes, chains, presets, controls, groups and plug-ins) may contain spaces and are matched without regard to case.

```ini
include = more.ini      ; anywhere: reads that file here, relative to this one
```

## Words

| Word | Means |
| --- | --- |
| **performance** | The whole file: scenes in order. |
| **scene** | One routing grid: the chains that can play, and the presets that pick between them. |
| **chain** | Plug-ins in series from one input to every output, with optional branches. Only the chain you hear is processed; the others cost nothing. |
| **branch** | A copy of a chain's signal from one point, with its own plug-ins: for example the IR loader that only FOH gets. |
| **preset** | Which chain plays, and what to set when it is selected. |
| **control** | A named value you move with pedals and MIDI, which switches plug-ins, moves their parameters, mutes the outputs and lights LEDs. |
| **trigger** | An input and the actions it runs. |
| **group** | Named actions that any trigger, preset or other group can run. |

## Audio, switching, MIDI

```ini
[audio]
device = hw:M2      ; ALSA device; cue devices lists them
rate = 48000
period = 64         ; frames per block
periods = 2         ; 3 adds a period of latency (1.3 ms at 64) and lets a late block catch up; the import writes 3, as REAPER uses on the M2
input = 1           ; the sound card channel of the performance's first input (the bass); in2 = 3 for a second, and so on
out1 = 1            ; the channel of its first output (FOH)
out2 = 2            ; ...of its second (amp); out3 = 5 for a third, and so on
cpu = auto          ; auto (last performance core), none, or a CPU number
priority = 80       ; real-time priority of the audio thread (and its helpers)
workers = 2         ; helper threads: chains playing at once (a switch, the warm chain) are processed side by side, and the warm chain never holds up the one heard. 0 = all on the audio thread
mix = sum           ; how a chain's stereo becomes each mono output: left, right or sum

[switching]
preroll_ms = 50     ; a new chain runs muted this long before it is heard
crossfade_ms = 20
tail_cap_ms = 10000 ; the old chain's tail rings out at most this long
tail_hold_ms = 300
tail_quiet_db = -90
bypass_fade_ms = 10 ; crossfade when a plug-in is switched on or off
mute_fade_ms = 10
warm = next         ; next: the next preset's chain runs muted in the background; none. A preset's own warm = wins
reset_tails_on_warm = yes ; delays and reverbs in the warm chain hear silence until the switch
bypass_mode = thru  ; what a bypassed plug-in passes on, unless it says otherwise: thru (its input) or mute (nothing)

[midi]
in = M2             ; any MIDI port whose name contains this; 'any' for all
out = M2            ; where LEDs and display go; defaults to 'in'
profile = eurekaprom ; FCB1010 with EurekaPROM: pedal numbers, LEDs, display. 'none' for plain MIDI
channel = 1         ; the channel Cue sends on (input is accepted on any channel)
forward = yes       ; pass incoming MIDI on to plug-ins that take MIDI (a wah's pedal)
```

## Scenes, chains and branches

```ini
[performance Friday]          ; optional: names the performance
inputs = Bass                 ; optional: its inputs and outputs, in order (default In 1; Out 1, Out 2),
outputs = FOH, Amp            ; the names a .cue keeps and a machine's rig profile gives channels to

[scene Covers]
display = CV                  ; optional: FCB display when no preset says otherwise
pedal = 1                     ; optional: a pedal that selects the scene (its first preset)
program = 0                   ; optional: a program change that does the same

[chain Punk]                  ; a chain of the scene above
input = Bass                  ; optional: the input it plays, by name or as in1, in2... (default: the first)
plugin = clap:~/.clap/TwoStorms Gain Stage.clap#com.twostorms.gainstage
name = Gain Stage             ; what mappings call this plug-in
param = 54:1                  ; parameter id : value (cue params lists the ids)
plugin = vst3:~/.vst3/yabridge/Neural DSP/Darkglass Ultra.vst3
state = states/punk-ultra.vst3state
controller_state = states/punk-ultra.vst3ctrl
plugin = clap:~/.clap/TwoStorms Wah.clap#com.twostorms.wah
name = Wah
bypass = yes                  ; loaded, but off until a mapping switches it on
midi = no                     ; do not pass MIDI to this one
branch = FOH                  ; the branch called FOH leaves here
tap = Amp                     ; the Amp output takes the signal here (by name, or as out1, out2...)
plugin = clap:~/.clap/TwoStorms Delay.clap
plugin = clap:~/.clap/CLAP Plugins Linux/TwoStorms Gate.clap
sidechain = TwoStorms Gain Stage ; its key input hears the signal after that earlier plug-in

[branch FOH]                  ; a branch of the chain above
plugin = clap:~/.clap/CLAP Plugins Linux/TwoStorms IR Loader.clap
bypass_mode = mute            ; bypassed, it passes nothing instead of its input
tap = FOH                     ; FOH takes the end of this branch
```

- **Plug-ins**: `clap:<file>[#id]`, `vst3:<bundle>[#name or class id]`, or `builtin:<name> <numbers>` (gain, drive, lowpass, delay, burn). Paths may start with `~`; relative paths are relative to the file they are written in.
- **param** values are the plug-in's own values for CLAP, and 0 to 1 for VST3. They are applied after `state`.
- **Inputs and outputs**: a performance has any number of each (up to 64), named in `[performance]`. `[audio]` gives each a sound card channel, `in1`/`input`, `in2`... and `out1`, `out2`... by their order; one without a line takes the channel of its place, and two outputs cannot share a channel. A chain plays one input. Every chain feeds every output: each output takes the end of the main line unless a `tap` line says otherwise. A tap can sit on the main line or on a branch, after any plug-in.
- Branches can leave a branch too. A chain's plug-ins are all loaded at start; a bypassed plug-in costs nothing once its tail has died away.
- **bypass_mode** is `thru` or `mute`: whether a bypassed plug-in passes its input on, or silence. Either way it fades over `bypass_fade_ms`. Without it, the `[switching]` default applies (thru unless set).
- **sidechain** feeds the plug-in's side-chain input (its second input, like a gate's key) with the signal after an earlier plug-in of the same chain, named as mappings name it: `Gate` is the nearest Gate before this one, `Gate:2` the chain's second Gate. Earlier means on the main line before it, or for a plug-in on a branch, anywhere on the main line or an earlier branch. `cue check` reports a plug-in with no side-chain input.
- An old-style scene with `plugin` lines directly under `[scene Name]` is still read: it is a scene with one chain and one preset of the same name.

## Presets

```ini
[preset Punk]                 ; a preset of the scene above
chain = Punk                  ; default: the chain of the same name, else the scene's first
pedal = 6                     ; selects it while its scene is playing
program = 5                   ; a program change that selects it from anywhere
display = PU                  ; up to two letters or digits (three with a leading 1, - or +)
do = group Preset start       ; actions, in order, every time it is selected
do = Wah = Off
warm = next                   ; optional: the chain kept warm while this plays: next, none or a chain's name
```

A scene without `[preset]` sections gets one preset per chain. In a scene with several chains, `cue check` warns about a preset that names no chain and has no chain of its own name, since it would play the first chain. Presets are numbered through the whole performance; keys 1 to 9 pick them in `cue run`, and the FCB's up and down pedals step through them (unless something else uses those pedals). Selecting the preset that is already playing runs its actions again, which is handy for "back to how this song starts".

## Controls

```ini
[control Wah]
values = Off, Auto, Pedal     ; named steps: 0, 1, 2
pedal = 4                     ; each press steps to the next value; its LED is lit when not at the first
enable = TwoStorms Wah:1 when Auto
enable = TwoStorms Wah:2 when Pedal

[control Effect 1]            ; no values or range: an on/off switch (values off, on)
pedal = 1
default = on

[control Delay mix]
type = continuous             ; any value in the range (or discrete: steps only)
range = 0..1                  ; or min = / max =
step = 0.1                    ; how far next/previous move it (continuous default: a tenth of the range)
wrap = yes                    ; past the end, back to the start (default yes)
follows = expression 1        ; the expression pedal sets it in proportion
drive = TwoStorms Delay param 12 from 0 to 100

[control Tuner]
pedal = 5
mute = on                     ; every output is muted while this holds
```

- **Stepping** goes value by value; when a step would pass the end, the control first lands on the end, and the next step wraps to the start (or stays, with `wrap = no`). Going down works the same way.
- **enable = \<plug-ins> [when \<condition>]**: those plug-ins run while the condition holds and are bypassed otherwise, with a short crossfade. Without `when`, while the control is not at its first value. A plug-in named by several enables runs if any of them holds. Do not also switch it with `bypass`/`enable` actions: the control decides.
- **drive = \<plug-ins> param \<id> [from \<a> to \<b>]**: sets that parameter whenever the control changes, mapping the control's range onto a..b (default: the control's own value).
- **mute = \<condition>**: mutes every output while it holds.
- Inside a control, a bare value is a condition on that control: `when Auto` means "when this control is Auto", `mute = on` means "when this control is on".

## Naming plug-ins

Mappings name plug-ins by their `name =` (or, without one, the plug-in's own name):

| Written | Means |
| --- | --- |
| `Wah` | every plug-in called Wah, in every chain |
| `Punk/Wah` | only in the chain Punk |
| `TwoStorms Wah:2` | the second plug-in called TwoStorms Wah in each chain |

Naming the same thing the same way in every chain is what lets one pedal switch the wah whichever chain is playing.

## Triggers

```ini
[trigger Big ending]
on = pedal 3                  ; one or more inputs
on = cc 64 = 127
when = preset is Djent and not Tuner   ; optional: only while this holds
scene = Covers                ; optional: only in this scene
do = Effect 3 = on            ; every action, in order
do = Wah = Auto
do = group Preset start
do = preset next
```

**Inputs**

| Input | Means |
| --- | --- |
| `pedal 1` .. `pedal 10`, `pedal up`, `pedal down` | an FCB pedal pressed (EurekaPROM); add `release` for letting go |
| `expression 1`, `expression 2` | the left or right expression pedal moving |
| `cc 64`, `cc 64 = 127`, `cc 64 127` | a controller, any value or one value |
| `program 5`, `note 36` | a program change, a note-on |
| `key t` | a key on the computer's keyboard in `cue run` |

`cc`, `program` and `note` take `channel N` at the end; without it any channel matches. When one input matches several triggers, they all run, each seeing the state from before any of them ran. A pedal used by two things in the same scene is reported by `cue check`.

`pedal =` lines on presets, scenes and controls are shortcuts for triggers.

## Actions

| Action | Does |
| --- | --- |
| `Wah = Auto`, `Mix = 0.5`, `Effect 1 = on` | sets a control |
| `Wah = next`, `Wah = previous` | steps it by its step, wrapping as above |
| `Mix = up 0.05`, `Mix = down 0.05` | steps it by that much |
| `Effect 1 = toggle` | first value if it is not there, else the last |
| `Wah = default` | back to its default |
| `group Preset start` | runs a group |
| `preset Djent`, `preset 3`, `preset next`, `preset previous` | selects a preset (`chain Djent` is the same: in a .cue file, each chain is selected by itself) |
| `scene Covers`, `scene next`, `scene previous` | selects a scene's first preset |
| `bypass Delay`, `enable Delay`, `toggle Delay` | switches plug-ins directly |
| `param Delay 12 = 40` | sets a plug-in parameter |
| `mute on`, `mute off`, `mute toggle` | mutes every output |
| `midi B0 6D 0F` | sends a MIDI message (hex) to the controller |

## Groups

```ini
[group Preset start]
do = Effect 1 = on
do = Effect 2 = off
do = Wah = Off
```

A group can run other groups (up to eight deep, so a group that runs itself stops).

## Conditions

Used by `when`, `enable ... when`, `mute`, and the `[fcb]` section.

| Condition | True when |
| --- | --- |
| `Tuner` | the control is not at its first value |
| `Wah is Auto`, `Wah = Auto`, `Wah Auto` | it has that value (`is not`, `!=` for the opposite) |
| `Mix > 0.5` | `<`, `<=`, `>`, `>=` compare numbers |
| `preset is Djent`, `chain is Djent`, `scene is Covers` | that preset (chain) or scene is playing |
| `not ...`, `... and ...`, `... or ...`, `( ... )` | combinations |
| `always` | always |

## FCB feedback

LEDs and the display follow the state by themselves:

- a preset's pedal is lit while it plays, a scene's pedal while it plays;
- a control's pedal is lit while the control is not at its first value;
- the display shows the playing preset's `display`, else the scene's, else the preset's number.

The `[fcb]` section overrides them:

```ini
[fcb]
led 4 = Wah is not Off        ; LED 4 follows this condition instead
led value 2 = Wah is Pedal    ; EurekaPROM's other LEDs, by name or number (value 2 = 19)
display = TU when Tuner       ; the first display line whose condition holds wins
```

The LEDs past the pedals are numbered as EurekaPROM numbers them: 11 `expr a`, 12 `expr b`, 13 `sw1`, 14 `sw2`, 15 `bank`, 16 `select`, 17 `number`, 18 `value 1`, 19 `value 2`, 20 `dir sel`, 21 `midi fn`, 22 `midi ch`, 23 `config`. Cue drives one of them only when an `[fcb]` line names it.

## Importing from REAPER

```sh
cue import-rpp "Cue Template.RPP" [--out DIR] [--mapping FILE] [--search DIR]... [--no-scan]
```

Writes `DIR/<project>.ini` (default `~/cue/<project>/`) and the plug-ins' settings in `DIR/states/`. Every track that plays to a hardware output becomes a chain; a REAPER folder of tracks is a scene, and tracks outside folders make one scene named after the project:

- CLAP and VST3 plug-ins keep their settings, bypass state and REAPER name; VST2, JS and LV2 are left out and listed.
- A plug-in that writes other channels than the main pair (an IR loader writing channels 3-4 for FOH) starts a branch, and each output is tapped where REAPER's hardware send took its channel. A plug-in whose side-chain pins (3-4) read channels an earlier plug-in wrote gets a `sidechain` line.
- A TwoStorms IR Loader names its IRs by file but only plays the samples saved in its state; the import reads each named WAV and adds its samples, so the IR plays in Cue. A file it cannot read is listed.
- Plug-ins are found through REAPER's plug-in caches and paths, `~/.clap`, `~/.vst3` (including yabridge's folders), `/usr/lib` and any `--search` folder; as a last resort native `.clap` files are opened to read their ids (`--no-scan` skips that). Anything not found is written with a guessed path and marked `NOT FOUND`.
- With `--mapping FILE`, that file is included after the chains instead of a generated preset per chain, so its `[preset]` sections pick the imported chains by name. `configs/cue-template-mapping.ini` is the one for the Cue Template project.
- Re-running the import refreshes the states; the previous config is kept as `.bak`.
- Simple ReaLearn mappings become triggers: a pedal or controller that switches a plug-in, selects a track or sends MIDI. Mappings built on ReaLearn parameters and conditions, and its LED and display feedback, are listed instead.
- A project that Cue Converter wrote carries its performance's pedals, names and settings in its notes, and the import takes them from there, so a performance comes back as it was. [converter.md](converter.md) has the details.
