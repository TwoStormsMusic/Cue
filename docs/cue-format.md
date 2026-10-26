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
  "cue": 1,
  "saved_by": "Cue 0.4.0",
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
      "lines": [
        {"plugins": [
          {"format": "clap", "path": "~/.clap/TwoStorms Gain Stage.clap", "id": "com.twostorms.gainstage",
           "label": "Gain Stage", "bypassed": false, "bypass_mode": "thru", "midi": true,
           "params": {"54": 1}, "state": "states/01 Covers/01 Punk/01 Gain Stage.clapstate"},
          {"format": "vst3", "path": "~/.vst3/yabridge/Neural DSP/Darkglass Ultra.vst3", "...": "..."}
        ]},
        {"name": "FOH", "from": "", "after": 2, "plugins": ["..."]}
      ],
      "outputs": {"FOH": {"line": "FOH", "after": 1}, "Amp": {"line": "", "after": 2}}
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
- **Lines** are the main line first, then branches: each branch takes a copy
  of the line named in `from` (`""` is the main line) after that many of its
  plug-ins. **outputs** says where each of the performance's outputs is taken,
  by line and number of plug-ins before it; an output a chain leaves out takes
  the end of the main line.
- **input** is the performance input a chain plays. It is written only when
  the performance has more than one; without it, a chain plays the first.
- **Plug-ins** are found by `path`. Paths under your home folder are written
  with `~`, so they work for another user name. `label` is what the mappings
  call the plug-in. `params` are set after the state, by parameter id
  (`cue params` lists them).
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
performance uses. An input or output the rig does not list takes the channel
of its place (the second output plays on channel 2), and `cue check` says
which names it could not find. Two outputs cannot share a channel. Every
setting can be left out; what is missing keeps its default (`[audio]` and
`[midi]` in [config.md](config.md) describe each one).
