# Talking to a running Cue

`cue run` answers on a control socket, so another program can see what it is
doing and tell it what to do: the app ([app.md](app.md)), and `cue ctl` from a terminal
(on the same machine, or over ssh from another).

```sh
cue ctl status                 # one line: DSP load, margin, xruns, the chain playing, the controls
cue ctl watch                  # the same twice a second, and every xrun or slow-block line, until Ctrl-C
cue ctl tuner                  # the built-in tuner's reading, until Ctrl-C
cue ctl select Slap            # play a chain: by name, by number (1 = the first), or next / previous
cue ctl do "Wah = Auto"        # any action of the mapping language (docs/config.md)
cue ctl save                   # save what is playing, plug-in states included, to its .cue file
cue ctl save ~/shows/fri.cue   # ...or to another one (later saves go there too)
cue ctl performance            # the performance as JSON, without the states
cue ctl plugin 2 1             # chain 2's first plug-in: whether it is bypassed, its parameters and values
cue ctl editor 2 1             # open that plug-in's own editor window (editor 2 1 close shuts it)
cue ctl bypass 2 1 on          # ...bypass it (off: back on); kept when the performance is saved
cue ctl param 2 1 54 0.5       # ...set its parameter 54
cue ctl quit                   # stop cue run
```

`--json` prints the replies and events as they come. `--socket PATH` talks to
a Cue started with `cue run ... --socket PATH`.

`cue run ... --audio none` runs everything without a sound card: the engine
plays on a timer and hears silence. It is for a machine without the
interface, to load a performance, change it and save it. With `--tone DB`
every input hears a 110 Hz sine at that level instead (`--tone -20`), so
the meters and Auto can be tried too.

`cue run` takes the sound card from whatever else has it. It reserves the
card on the desktop's session bus (`org.freedesktop.ReserveDevice1`, as
JACK does), so PipeWire, PulseAudio or JACK let go of it and do not take it
back while Cue plays. A program that still has it open (REAPER using ALSA)
is asked to quit, and stopped if it has not after five seconds; unsaved
work in it is lost. What it did goes to the log and to `hello`'s
`audio_notes`. `--leave-device` turns all this off: Cue then stops with
"Device or resource busy" when the card is taken, as before 0.9.2.

## The socket

`$XDG_RUNTIME_DIR/cue/engine.sock` (or `/tmp/cue-<uid>/engine.sock`), in a
folder only your user can open. A second `cue run` refuses to start while one
answers there; a socket left by one that crashed is replaced. `--socket none`
runs without one.

Each message, in both directions, is a 4-byte big-endian length followed by
that many bytes of JSON (at most 16 MB). Requests carry an `id`, which the
reply repeats:

```json
{"id": 7, "cmd": "do", "action": "Wah = Auto"}
{"id": 7, "ok": true}
{"id": 8, "cmd": "select", "chain": "Nope"}
{"id": 8, "ok": false, "error": "no chain named 'Nope'"}
```

| cmd | with | answers |
| --- | --- | --- |
| `hello` | | `cue` (version), `protocol` (1), `performance`, `file`, `audio` (a description), `audio_settings` (`device`, `rate`, `period`, `periods`), `audio_notes` (what was done to get the sound card, such as closing REAPER), `midi` (`enabled`, `in`, `profile`, `ports` connected) |
| `status` | | `status`: as the status event below |
| `performance` | | `performance`: the .cue's performance.json, without states |
| `select` | `chain` (a name, or its number as `performance` gives it), `index` (its place among all the chains, scene by scene, from 0) or `step` (+1, -1) | `selected`: `index`, `name`, `chain` (its number) |
| `do` | `action` | |
| `save` | `path` (optional: else the .cue it was loaded from) | `path`, `warnings` |
| `tuner` | any of `reference` (415-466 Hz), `range` (guitar, bass, vocals, any), `tolerance` (cents), `names` (sharps, flats) | `tuner`: as the tuner event; kept with the performance when it is saved |
| `plugin` | `chain` (its number: the chain's `index` in `performance`), `slot` (the plug-in's place in the chain, line by line, from 0, as performance.json lists them) | `plugin`: `label`, `name`, `format`, `bypassed`, `editor` (it has one of its own), `editor_open`, and `params`: `id`, `name`, `module`, `min`, `max`, `default`, `steps`, `value` now and its `text`, and `tied` when a control's `drive` (`control`, what it `follows`, its `pedal`) or a trigger's `param` action (`trigger`) moves it; `shown` once the app has picked parameters |
| `bypass` | `chain`, `slot`, and `bypassed` (true or false) or `mode` (`thru` or `mute`: what it passes on while bypassed, its input or silence), or both | `plugin` as above, and `performance` with a `mode`; kept when the performance is saved. A mode changed while the plug-in is heard fades, as a bypass does |
| `param` | `chain`, `slot`, `param` (the parameter's `id` from `plugin`), and `value` (the plug-in's own value for CLAP, 0 to 1 for VST3) or `text` (a value as typed, "-3 dB", which the plug-in reads; a plain number if it cannot, in % for VST3) | `value`, and `text`: how the plug-in writes it; kept in its state when saved |
| `editor` | `chain`, `slot`, `open` (true to open, false to close; true by default); from an app on the same screen, `above` (its X11 window) and `display` (its `DISPLAY`) | `plugin` as above. The window opens on the screen of the computer running the engine, not the app's, so it is the stage computer that shows it. With `above` it stays over the app's window and never takes its focus |
| `shown` | `chain`, `slot`, `params` (parameter ids, in the order the app shows them) | `plugin` as above; kept as the plug-in's `shown` when the performance is saved |
| `add` | `chain`, `plugin` (a plugin line's text as in [config.md](config.md), or `format` with `path` and `id`, or `args` for a built-in), optional `label`, `bypassed`, `x` and `y` (where the app draws it), and `on` (a wire, as for `wire`, to put it on) | `slot`, `performance`. Without `on` it is added unwired |
| `remove` | `chain`, `slot`, optional `token` | `performance`. With a `token`, what was taken out (the plug-in with its state, and its wires) is kept for `restore`; the last 100 are kept |
| `restore` | `token` | `slot`, `performance`: the plug-in back in its slot, its state and the wiring it had |
| `wire` | `chain`, `from` (a slot, from 0, or `in` or the chain's input by name), `out` (which of its outputs: 0 for 1-2, the default, 1 for 3-4...), `to` (a slot, or an output by name or `out1`, `out2`...), `key` (true: the plug-in's side-chain input), and any of `jump` (true: the app draws it as a jump; nothing else changes, and it can be set on a wire it has), `feedback` (true: heard one block late, so it may lead back to its own plug-in or an earlier one; only between plug-ins). A wire has no level, width, pan or mute of its own (asking for one is refused): it carries the output it leaves as that output has them | a new wire: `performance`. A wire it has: only how it is drawn changes, and the reply and the `wire` event carry `wire` |
| `unwire` | `chain`, `from`, `out`, `to`, `key` | `performance` |
| `wires` | `chain`, `wires` (each as for `wire`) and or `loose` (each a wire with one end: `from` (and its `out`) or `to` as for `wire`, `key`, `x` and `y` where its loose end lies on the board, and `feedback`) | `performance`: the chain wired so, all at once. Loose wires carry nothing: they are what is left where a plug-in was pulled out. Without `wires`, only the loose ones change, and nothing heard does |
| `output` | `chain`, `from` (a slot, or `in`: the chain's input), `out` (0 for 1-2, the default, 1 for 3-4...), and any of `level_db` (up to +24; -96 or less is silence), `width` (0 to 200 %), `pan` (-100 to 100), `muted`; or `remove` (true) | One output of a plug-in or of the chain's input: every wire leaving it carries it so, moving with a short ramp. The reply and the `output` event carry `output` (`from`, `out`, `level_db`, `width`, `pan`, `muted`). With `remove`, it goes with its wires (and loose wires), the outputs after it move up one (their wires with them), and the reply and event are `performance`; the last one stays |
| `outputs` | `chain`, `from`, and `count` (1 to 8: new ones start at 0 dB, 100 %, centre) or `outputs` (the whole list, each as `output` takes them) | `performance`. Every plug-in and the chain's input start with one output, 1-2, and have up to 8 (15-16). Fewer: the wires of those it no longer has go |
| `place` | `chain`, `slot` or `port` (`in`, or an output by name or `out1`, `out2`...), `x`, `y` | nothing: where the block, the input or the output is drawn on that chain's board, kept when saved |
| `rename` | `input` or `output` (its number, from 0), `name` | `performance`: the performance's input or output under its new name, which its chains, wires and tuner follow. Refused if it is empty, another input (or output) has it, or it is `in` or another's `in2`/`out2`. The sound card channel stays |
| `level` | `input` (its number, from 0) with any of `gain_db` and `auto`; or `output` with any of `level_db`, `muted`, `limiter` (any of `on`, `ceiling_db` -60 to 0, `release_ms` 1 to 5000) and `auto`; or `wires` (true) with `auto`, the settings of Auto on the plug-ins' outputs, which also take `match`. `auto` is Auto's settings: `measure` (`peak`, `rms`, `lufs`), `target_db` (-60 to 0), `seconds` (1 to 60) | `io`: every input's and output's levels, as the `io` event. The performance's, the same whichever chain plays: an input's gain comes before every chain (-100 dB or less is silence, up to +60), an output's level and mute after every chain is mixed, then its limiter. Each moves over 20 ms; kept when saved |
| `auto` | `input` or `output`, optional `auto` (settings, as for `level`, kept), optional `cancel` | `auto` (the run, as the `auto` event, `waiting`), `io`. Auto as the Gain Stage plug-in does it: it waits until it hears more than -50 dB, listens for its `seconds`, then sets the gain or level that brings what it heard to the target: an input as it arrives, an output before its level. Peak is the highest peak; RMS leaves out 10 ms stretches more than 20 dB below the loudest; LUFS is BS.1770 integrated loudness (400 ms blocks, gated). Below -70 dB it fails and nothing changes. One at a time on each input or output; the first 16 of each |
| `auto` (a plug-in's output) | `chain`, `from` and `out` as for `output`: an output of a plug-in, or of the chain's input; optional `cancel` | `auto` (the run, as the `auto` event, `waiting`). The chain must be playing (active, warm or pre-roll). Auto on the output, with the outputs' settings (`level` with `wires`): it listens to the plug-in before the output's gains and, matching (`match`, the default), sets the output's level so the plug-in plays as loud as it was fed, measured the same way; else to the target. From the chain's input it always goes to the target. The level goes from -60 to +24 dB and is set as by `output` (an `output` event); below -70 dB it fails and nothing changes. One at a time in each chain; a new one stops the one before |
| `add-chain` | `scene` (from 0), optional `at` (its place in the scene; the end if left out), `name`, `copy` (a chain's number: its plug-ins with their states as they are now, its wires and its settings), `input` (an empty chain's, by name) | `chain` (its number), `warnings`, `performance`. An empty chain has its input wired to every output, named `Chain N`; a copy in the same scene is named `Slap 2`. A copy keeps its pedal only in another scene where that pedal is free, and never a program change. Up to 64 chains can be added while it runs; saved and started again, there is room for 64 more |
| `remove-chain` | `chain`, optional `token` | `performance`. Refused for the chain playing and the last chain of a scene. With a `token`, it is kept (with its plug-ins' states) for `restore-chain` |
| `restore-chain` | `token` | `chain` (its number again, unless a chain added since has taken it), `performance` |
| `set-chain` | `chain` and any of `name`, `display` (two letters or digits; three with a leading 1, - or +), `pedal` (1-10; 0 or null for none), `program` (0-127; -1 or null for none), `warm` (`next`, `none`, or a chain of its scene by name), `on_select` (a list of actions, as [config.md](config.md) writes them) | `warnings` (a pedal another control also uses, say), `performance`. A pedal given to a chain is taken from any other chain of its scene that had it; a program change from any other chain. A new name is followed by every chain that kept it warm by name |
| `move-chain` | `chain`, `scene`, `at` (its place there; -1 for the end) | `performance`. Refused for the last chain of a scene |
| `add-scene` | optional `at` (-1 for the end), `name`, `copy` (a scene: each chain as `add-chain` copies one, pedals kept, and the triggers that work only there) | `scene`, `chains` (their numbers), `performance`. Without `copy`, it has one empty chain |
| `remove-scene` | `scene`, optional `token` | `performance`. Refused for the scene playing and the only scene. Every chain of it goes, and the triggers that work only there; with a `token`, all of it is kept for `restore-scene` |
| `restore-scene` | `token` | `scene`, `performance` |
| `set-scene` | `scene` and any of `name`, `display`, `pedal`, `program` | `warnings`, `performance`. A pedal or program change is taken from any other scene that had it. A new name is followed by the triggers that work only there |
| `move-scene` | `scene`, `at` | `performance` |
| `plugins` | optional `scan`: `changed` (new and changed files), `all`, or one file's path; optional `folders` (the user's folders to look in, kept on this machine) | `plugins`: `scanning`, `done`, `total`, `current` (the file being opened), `scanned_at` (seconds since 1970), `folders` (`clap`, `vst3`: the usual ones; `extra`: the user's), and `files`: each `path`, `format`, `error` when it was skipped, and its `plugins`: `format`, `path`, `id`, `name`, `vendor`, `version`, `categories`, `bridged` (through yabridge), and when the scan made it, `opened` with `sidechain`, `editor` and `midi`; `error` for one that fell over as it was made |
| `quit` | | |
| `subscribe`, `unsubscribe` | `events`: `["status", "log", "tuner", "performance", "wire", "output", "scan", "plugins", "edited", "io", "auto"]` | `events` now subscribed |

Events have an `event` field instead of an `id`:

- `status`, about 30 times a second: `dsp` (`avg`, `max`: % of a block's time),
  `margin_ms` (null until measured; never with `--audio none`), `xruns`,
  `period_ms`, `buffer_ms`, `scene` (`index`, `name`) and `selected` (`index`,
  `name`, `chain`: as `select` answers), `muted`,
  `display`, `leds` (pedals 1-10), `controls` (`name`, `value`, `text`),
  `chains`, by number (`name`, empty for a number no chain has; `state`: idle, pre-roll, warm, active, tail (ringing out) or
  stopping; `avg_us`, `max_us` per block since the last event; `bypassed`: the
  slots bypassed now; `wire_peaks`, for a chain that is not idle: each
  wire's highest peak after the gains of the output it carries, in dB, in
  the order the performance lists its wires), and `levels`: `inputs` and `outputs`, each `name` and
  `peak_db`, the highest peak since the event before (-120 for silence): an
  input after its gain, an output as it goes out. Outputs have `limit_db`
  too: how far the limiter turned it down (0 when it did not).
- `log`: `level` (info, warning, error) and `text`, for each xrun, slow block
  and start-up stall line `cue run` prints.
- `tuner`, about 30 times a second: the built-in tuner (the TwoStorms Tuner's
  pitch detection on one input, before any plug-in). `note` (`"E"`, `"F#"`,
  or null before one is heard), `octave`, `midi`, `cents` (settled, against
  the note), `hz`, `held` (no pitch for a quarter second: the last note),
  `in_tune`, `signal` and `level_db` of the input, and its settings:
  `input`, `reference`, `range`, `tolerance`, `names`. The tuner only listens
  while a client is subscribed to it, so it costs nothing otherwise.
- `performance`: the performance, as the `performance` command answers it,
  each time a plug-in is added, taken out or put back, a chain's wires
  change, or a chain or scene is added, set, moved or taken out.
- `wire`: `chain` and `wire` (as the `wire` command answers it), when only
  how a wire is drawn changed.
- `output`: `chain` and `output` (as the `output` command answers it), when
  an output's level, width, pan or mute changed, by a request or by Auto.
- `edited`: a knob turned in a plug-in's own editor, once it is let go (or
  still for 0.6 s, for a plug-in that does not say when it is let go):
  `chain`, `slot`, `param`, its `name`, `from` and `to` (as `param` takes
  them) and the `text` of where it went. Only while the editor is open.
- `io`: every input (`name`, `gain_db`, `auto`) and output (`name`,
  `level_db`, `muted`, `limiter` with `on`, `ceiling_db`, `release_ms`,
  `auto`), in order, and `wires` (`auto`: the settings of Auto on the
  plug-ins' outputs, with `match`), when one changes. The `performance` command's answer
  carries the same as `io`.
- `auto`: a run on an `input` or `output` (its number), or on a plug-in's
  output (`chain`, `from` as the request named it, `out`, and `match`): `state` (`waiting`, `listening`, `done`, `failed`, `cancelled`),
  `seconds` listened and `of` how many, five times a second while it runs.
  Once `done`, `level_db` (what it heard), matching `fed_db` (what the
  plug-in was fed), and the gain or level it went `from_db` and `to_db`;
  `failed` has `error`.
- `scan`: how far a plug-in scan is (`scanning`, `done`, `total`, `current`),
  as each file is opened.
- `plugins`: as the `plugins` command answers it, when a scan ends.

A scan never runs unless asked for. Each file is opened by `cue scan-file`,
a helper process of its own, one at a time; one that crashes or says nothing
for 30 s (120 s for a yabridge plug-in, as Wine starts) is skipped with the
reason. What was found is kept in `~/.cache/cue/plugins.json`, and the user's
folders in `~/.config/cue/plugins.json`. `cue scan` does the same from a
terminal.

A loop, a wire to a plug-in or output the chain does not have, or a key on
a plug-in without a side-chain input is refused with the reason, and
nothing changes. A playing chain's wiring changes between a 10 ms fade out
and a fade in; gains move with a short ramp and no fade; a chain not playing
changes at once. In the performance JSON each block says `has_key` when the
plug-in has a side-chain input.

Each chain has a number, its `index` in the performance JSON, which
`plugin`, `bypass`, `wire` and the rest take as `chain`. Loaded, the chains
are numbered scene by scene from 0; a chain added later takes a number of
its own, and none is ever renumbered while Cue runs, whatever is added,
moved or taken out around it. Saved and loaded again, they are numbered in
order again. A chain added, copied or put back while another plays is
loaded and warmed beside it, and the one playing never stops.

Everything runs on `cue run`'s main thread, never on the audio thread: a slow
or stuck client cannot cause a click. A client that stops reading is
disconnected once 8 MB is waiting for it.
