# Talking to a running Cue

`cue run` answers on a control socket, so another program can see what it is
doing and tell it what to do: the desktop app, and `cue ctl` from a terminal
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
cue ctl quit                   # stop cue run
```

`--json` prints the replies and events as they come. `--socket PATH` talks to
a Cue started with `cue run ... --socket PATH`.

`cue run ... --audio none` runs everything without a sound card: the engine
plays on a timer and hears silence. It is for a machine without the
interface, to load a performance, change it and save it.

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
| `hello` | | `cue` (version), `protocol` (1), `performance`, `file`, `audio` |
| `status` | | `status`: as the status event below |
| `performance` | | `performance`: the .cue's performance.json, without states |
| `select` | `chain` (a name), `index` (from 0) or `step` (+1, -1) | `selected`: `index`, `name` |
| `do` | `action` | |
| `save` | `path` (optional: else the .cue it was loaded from) | `path`, `warnings` |
| `tuner` | any of `reference` (415-466 Hz), `range` (guitar, bass, vocals, any), `tolerance` (cents), `names` (sharps, flats) | `tuner`: as the tuner event; kept with the performance when it is saved |
| `quit` | | |
| `subscribe`, `unsubscribe` | `events`: `["status", "log", "tuner"]` | `events` now subscribed |

Events have an `event` field instead of an `id`:

- `status`, about 30 times a second: `dsp` (`avg`, `max`: % of a block's time),
  `margin_ms` (null until measured; never with `--audio none`), `xruns`,
  `period_ms`, `buffer_ms`, `scene` and `selected` (`index`, `name`), `muted`,
  `display`, `leds` (pedals 1-10), `controls` (`name`, `value`, `text`), and
  `chains` (`name`, `state`: idle, pre-roll, warm, active, tail (ringing out) or
  stopping; `avg_us`, `max_us` per block since the last event).
- `log`: `level` (info, warning, error) and `text`, for each xrun, slow block
  and start-up stall line `cue run` prints.
- `tuner`, about 30 times a second: the built-in tuner (the TwoStorms Tuner's
  pitch detection on one input, before any plug-in). `note` (`"E"`, `"F#"`,
  or null before one is heard), `octave`, `midi`, `cents` (settled, against
  the note), `hz`, `held` (no pitch for a quarter second: the last note),
  `in_tune`, `signal` and `level_db` of the input, and its settings:
  `input`, `reference`, `range`, `tolerance`, `names`. The tuner only listens
  while a client is subscribed to it, so it costs nothing otherwise.

Everything runs on `cue run`'s main thread, never on the audio thread: a slow
or stuck client cannot cause a click. A client that stops reading is
disconnected once 8 MB is waiting for it.
