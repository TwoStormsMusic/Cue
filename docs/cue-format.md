# .cue performances and rig profiles

A performance is saved as one `.cue` file that opens on any machine. What
belongs to the machine is kept apart, in that machine's **rig profile**: the
sound card, the buffer, the threads, which channel each input and output is,
and the MIDI ports. Opening someone else's performance takes their scenes,
chains, plug-ins and mappings, and plays them through your sound card.

```sh
cue convert my-rig.ini                        # writes my-rig.cue beside it
cue convert my-rig.ini show.cue --rig ~/.config/cue/rig.json   # and keeps its audio and MIDI settings as this machine's rig
cue check show.cue
cue run show.cue                              # with ~/.config/cue/rig.json, if there is one
cue run show.cue --rig other-rig.json
```

INI configs keep working as they are (`cue run my-rig.ini`); their `[audio]` and
`[midi]` settings apply to them and the rig profile is not read.

## Inside a .cue

A `.cue` file is a zip:

| File | Holds |
| --- | --- |
| `performance.json` | everything below |
| `states/<scene>/<chain>/<NN> <plug-in>.clapstate` | each plug-in's saved state, exactly as the plug-in wrote it |
| `.vst3state`, `.vst3ctrl` | a VST3 plug-in's processor and edit controller states |

`performance.json`, shortened:

```json
{
  "cue": 2,
  "saved_by": "Cue 0.7.0",
  "name": "Friday",
  "inputs": ["Bass"],
  "outputs": ["FOH", "Amp"],
  "output_mix": "sum",
  "switching": {"preroll_ms": 50, "crossfade_ms": 20, "tail_cap_ms": 10000, "tail_hold_ms": 300,
                "tail_quiet_db": -90, "bypass_fade_ms": 10, "mute_fade_ms": 10, "reset_tails_on_warm": true},
  "controller": {"profile": "eurekaprom", "channel": 1, "forward": true},
  "tuner": {"input": "Bass", "reference": 440, "range": "bass", "tolerance": 2, "names": "sharps"},
  "scenes": [{
    "name": "Covers", "display": "CV", "pedal": 2,
    "chains": [{
      "name": "Punk", "display": "PU", "pedal": 6, "program": 4,
      "on_select": ["group Start", "Wah = Off"],
      "warm": "next",
      "blocks": [
        {"format": "clap", "path": "~/.clap/TwoStorms Gain Stage.clap", "id": "com.twostorms.gainstage",
         "label": "Gain Stage", "bypassed": false, "bypass_mode": "thru", "midi": true,
         "params": {"54": 1}, "state": "states/01 Covers/01 Punk/01 Gain Stage.clapstate", "x": 196, "y": 109},
        {"format": "vst3", "path": "~/.vst3/yabridge/Neural DSP/Darkglass Ultra.vst3", "...": "..."},
        {"format": "clap", "path": "~/.clap/TwoStorms IR Loader.clap", "...": "..."}
      ],
      "wires": [
        {"from": "Bass", "to": 0},
        {"from": 0, "to": 1},
        {"from": 1, "to": 2},
        {"from": 2, "to": "FOH"},
        {"from": 1, "to": "Amp", "level_db": -3, "width": 50, "pan": -30}
      ]
    }]
  }],
  "controls": [{"name": "Wah", "type": "discrete", "values": ["Off", "Auto", "Pedal"], "default": 0, "wrap": true,
                "pedal": 4, "enable": ["TwoStorms Wah:1 when Auto"], "drive": [], "mute": []}],
  "triggers": [{"name": "Next song", "on": ["cc 102 = 14"], "do": ["scene next", "midi B0 6D 0F"]}],
  "groups": [{"name": "Start", "do": ["Effect 1 = on"]}],
  "fcb": {"leds": [{"led": 18, "when": "Wah is not Off"}], "displays": ["TU when Tuner"]}
}
```

- **Chains, not presets.** A chain carries what a preset did in an INI: its
  `pedal`, `program`, `display`, the actions it runs when selected
  (`on_select`), and `warm`, the chain kept running muted while it plays so
  switching to it is instant: `next` (the next chain), `none`, or the name of
  a chain of the same scene. `chain Punk` and `preset Punk` both select it in
  actions and conditions.
- **Blocks and wires.** `blocks` are the chain's plug-ins, numbered from 0
  in the order listed; `wires` join them. A wire comes `from` the chain's
  input (by its name, or `in`) or a block's output (its number), and goes
  `to` a block's input, its side-chain input with `"key": true`, or an
  output (by name). A block's output can feed any number of wires, and wires
  into one input are summed. Each wire has a `level_db` (0 when left out;
  -96 or less is off), a `width` (100 % when left out; 0 folds to mono, up
  to 200), a `pan` (a balance, -100 left to 100 right, 0 when left out),
  and `muted`. Width comes before pan. A loop is refused as the file loads.
  A block that the input does not reach, or no output hears, is loaded but
  not processed. `x` and `y` are where the app's routing board draws a
  block the user put somewhere; a block without them is laid out from its
  wires.
- **Loose wires.** `loose` (left out when there are none) lists wires
  attached at one end only, left where a plug-in was pulled out of them:
  each has `from` or `to` (as a wire's), `key`, `x` and `y` (where its loose
  end lies on the board) and the wire's `level_db`, `width`, `pan`, `muted`
  and `feedback`, kept for when it is plugged in again. They carry nothing.
- **Board.** `board` (left out until something is put there) says where the
  routing board draws the chain's `input` and its `outputs` (by name), each
  `{"x": ..., "y": ...}`; what is missing is laid out from the wires.
- **Format 1** files (Cue 0.4 to 0.6) have `lines` and `outputs` instead: the
  main line first, then branches, each taking a copy of the line named in
  `from` (`""` is the main line) after that many of its plug-ins, and each
  output taken from a line after a number of its plug-ins. They still open,
  as the blocks and wires they describe, and are saved as format 2.
- **input** is the performance input a chain plays. It is written only when
  the performance has more than one; without it, a chain plays the first.
- **Plug-ins** are found by `path`. Paths under your home folder are written
  with `~`, so they work for another user name. `label` is what the mappings
  call the plug-in. `params` are set after the state, by parameter id
  (`cue params` lists them).
  `shown` is the parameters the app shows under the selected block, by id
  in the order picked there; without it the app shows the first eight.
- **tuner** is the built-in tuner: the input it listens to and its settings,
  as `[tuner]` in [config.md](config.md).
- The mapping strings (`on_select`, controls, triggers, groups, `fcb`) are the
  language of [config.md](config.md), unchanged.
- `cue` is the format's version. A file from a newer Cue is refused with a
  message saying so, rather than half read.

Saving goes to a temporary file beside the old one and replaces it only once
it is complete, so a failed save never leaves a broken performance.

## Converting an INI config

`cue convert` turns each preset into its chain's selection: the chain takes the
preset's name, pedal, display, actions and warm choice, and chains follow the
presets' order. A chain no preset played gets one of its own, so stepping
through the chains now reaches it (`convert` says so). A chain that two
presets played cannot be converted: give each preset its own copy of the
chain first. Every state file is copied into the `.cue`, so it no longer
needs them.

## The rig profile

`~/.config/cue/rig.json` (or `$XDG_CONFIG_HOME/cue/rig.json`):

```json
{
  "cue_rig": 1,
  "audio": {"device": "hw:M2", "rate": 48000, "period": 64, "periods": 3, "priority": 80, "workers": 2, "cpu": "auto"},
  "inputs": {"Bass": 1},
  "outputs": {"FOH": 1, "Amp": 2},
  "midi": {"enabled": true, "in": "M2", "out": "M2"}
}
```

`inputs` and `outputs` give the sound card channel (from 1) of each name a
performance uses. An input or output the rig does not list (one renamed in
the app, say) takes the channel of its place (the second output plays on
channel 2), or the first channel no other has, and `cue check` says which
names it could not find. Two outputs cannot share a channel. Every
setting can be left out; what is missing keeps its default (`[audio]` and
`[midi]` in [config.md](config.md) describe each one).
