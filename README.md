# Cue

A live plug-in host for a Linux bass rig, built to replace REAPER + ReaLearn on stage. This is the **prototype**: it exists to prove one claim on the real mini PC before anything else is built.

> Every scene stays loaded, only the scene you hear is processed, and switching is instant and pop-free: the new scene is reset and pre-rolled on live input while muted, then crossfaded in, while the old scene's delays and reverbs ring out.

Cue is the working name, after the repository.

## What is in the prototype

| Part | State |
| --- | --- |
| Scene engine: pre-roll, crossfade, tail ring-out, warm next scene | done, tested offline |
| CLAP hosting (in process, one library instance per file) | done, tested with test plug-ins |
| VST3 hosting (native and yabridge-bridged) | done, tested with a test plug-in |
| ALSA direct audio, real-time thread pinned to a performance core | done, needs the M2 to test |
| FCB1010 with EurekaPROM 3.0: pedals switch scenes, LEDs and display follow | done, needs the FCB to test |
| Out 2 tapped anywhere in a chain (for example before the IR loader) | done |
| `doctor` real-time settings check | done |
| Per-plug-in process isolation | **not yet**: next step, after we know the in-process numbers |
| Stage UI, routing grid, mapping system, setlist, looper, LV2, JACK, PipeWire | later |

Everything runs in one process for now. That is deliberate: the first measurement is what your chains cost in process, and isolation is then added where it is worth its cost.

## Build

On the mini PC (AV Linux MXE-25, Debian based):

```sh
sudo apt install build-essential cmake ninja-build pkg-config libasound2-dev
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/cue_tests
```

All third-party code is vendored under `third_party/`: the CLAP headers (MIT) and the VST3 SDK's interfaces and hosting helpers (MIT since SDK 3.8).

## Commands

```sh
cue doctor                       # real-time settings: priority, governor, IRQs, USB, sound servers
cue devices                      # sound cards and MIDI ports
cue scan "<file.clap or bundle.vst3>"   # what is inside a plug-in file
cue midi-monitor FCB             # print what the FCB sends, decoded
cue fcb-test FCB --channel 1     # walk the FCB's LEDs and display
cue bench configs/my-rig.ini --wav out.wav   # offline: CPU per scene + a WAV of the switching
cue run configs/my-rig.ini       # live
```

While `run` is going: keys `1`-`9` pick a scene, `n`/`p` step, `q` quits. The status line shows the active scene, DSP load as a share of the period, xruns, and each scene's state and cost (`avg/max` microseconds per block).

## Config

An INI file with `[audio]`, `[switching]`, `[midi]` and one `[scene Name]` per scene. Use `;` for comments after a value. See `configs/builtin-demo.ini` (no plug-ins needed), `configs/test-plugins.ini` (bundled test plug-ins) and `configs/my-rig.ini` (a template for your chains).

```ini
[scene Verse]
pedal = 2                                   ; FCB pedal 1-10
plugin = clap:~/.clap/TwoStorms Gain Stage.clap
plugin = vst3:~/.vst3/yabridge/Archetype Nolly X.vst3
tap = out2                                  ; out 2 takes the signal here
plugin = clap:~/.clap/TwoStorms IR Loader.clap
```

Plug-in lines are `clap:<file>[#plugin id]`, `vst3:<bundle>[#effect name]` or `builtin:<name> <numbers>` (gain, drive, lowpass, delay, burn). `param = id:value` after a plug-in sets a parameter (CLAP: plain value, VST3: normalised 0-1). `state = file` loads a saved plug-in state.

## How switching works

1. A switch request arrives (FCB pedal, program change or key).
2. If the target scene is idle, its plug-ins are reset and it starts processing live input, muted, for `preroll_ms`. Its input is faded in, so a delay line never starts on an abrupt edge that would echo back as a click.
3. The new scene's output fades in over `crossfade_ms` (linear: the scenes share one input, so their signals are correlated). At the same time the old scene's **input** fades out in dB; its output stays up, so its delays and reverbs ring out naturally.
4. The old scene keeps processing silence until it is below `tail_quiet_db` for `tail_hold_ms`, or until `tail_cap_ms`, then fades to nothing and stops costing CPU.
5. With `warm = next`, the scene after the active one keeps processing in the background, muted, so switching to it skips the pre-roll. Its delays are cleared at the switch so it does not replay notes from before.

Idle scenes are never processed. Parameter changes still reach idle CLAP plug-ins (flushed every 5 ms); idle VST3 plug-ins receive them when their scene starts, during pre-roll.

## Known limits of the prototype

- **VST3 reset.** VST3 has no real reset call. The host toggles processing off and on, which the spec suggests plug-ins use to clear buffers, but not all do. If a VST3 delay replays an old tail when its scene comes back, that plug-in needs a deeper reset (deactivate and reactivate while idle), which is planned.
- **No isolation yet.** A plug-in crash takes the host down. Isolation is the next milestone.
- **Plug-in GUIs** are not shown. Set sounds up in REAPER and load the saved state, or set parameters in the config.
- **Bench numbers in a virtual machine** are noisy; only numbers from the mini PC count.

See `TESTING.md` for the test procedure on the rig.
