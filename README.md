# Cue

A live plug-in host for a Linux bass rig, built to replace REAPER + ReaLearn on stage. This is the **prototype**: it exists to prove one claim on the real mini PC before anything else is built.

> Every chain stays loaded, only the chain you hear is processed, and switching is instant and pop-free: the new chain is reset and pre-rolled on live input while muted, then crossfaded in, while the old chain's delays and reverbs ring out.

Cue is the working name, after the repository.

## Words

A **performance** is the whole config: **scenes** in the order you play them. A scene is one routing grid: **chains** of plug-ins (with **branches**, such as an IR loader only FOH gets) and **presets** that pick which chain plays and what is switched on. Named **controls** (Wah: Off, Auto, Pedal) are moved by pedals and MIDI and switch plug-ins, move parameters, mute and light LEDs. A **trigger** is one input and everything it does; a **group** is a named list of actions any trigger or preset can run. `docs/config.md` explains every line.

## What is in the prototype

| Part | State |
| --- | --- |
| Engine: pre-roll, crossfade, tail ring-out, warm next chain | done, tested offline |
| Branches, outputs tapped anywhere, plug-in bypass and input mute (for the tuner: tails ring out, then chains rest) with short fades | done, tested offline |
| Bypass mode per plug-in: thru (passes its input) or mute (passes nothing); bypassed plug-ins ring out, then cost nothing | done, tested offline |
| Side-chains: a plug-in's key input fed from an earlier plug-in (the Djent keyed gate) | done, tested with test plug-ins |
| Switches never wait on a plug-in: resets run on a helper thread while a chain is idle, every plug-in is primed at load | done, benched with the TwoStorms plug-ins |
| CLAP hosting (in process, one library instance per file), MIDI into plug-ins | done, tested with test plug-ins |
| VST3 hosting (native and yabridge-bridged) | done, tested with a test plug-in |
| Performance, scenes, presets | done, tested offline |
| Mapping: named controls (on/off, steps, continuous with range and wrap), triggers with many actions, groups, conditions | done, tested offline |
| FCB1010 with EurekaPROM 3.0: pedals, expression, LEDs and display follow the state | done, needs the FCB to test |
| Import from REAPER: tracks become chains with plug-in settings, routing and side-chains; TwoStorms IRs embedded from their files | done, tested offline on Cue Template.RPP and Cue Template 2S.RPP |
| ALSA direct audio, real-time thread pinned to a performance core | done, needs the M2 to test |
| Takes the sound card at start: reserved from PipeWire, PulseAudio or JACK, or closed in REAPER (`--leave-device` or Settings to turn off) | done, reservation tested offline; needs the rig with REAPER |
| `doctor` real-time settings check | done |
| Performances as one `.cue` file, a rig profile per machine, a control socket (`cue ctl`) | done, needs the rig to test |
| Built-in tuner: the TwoStorms Tuner's pitch detection on an input, before any plug-in | done, tested offline |
| The app (`cue-app`): stage view, tuner, chains with bypass, parameter sliders, zoom and undo, settings; by touch and mouse, any screen size ([docs/app.md](docs/app.md)) | done, tested on the rig |
| Plug-in editors: each plug-in's own window, opened by the engine on the stage computer's screen over the app, drawn between blocks and never on the audio thread | done, Neural DSP through yabridge tested on the rig |
| Routing board in the app: wires drawn between plug-ins, splits and merges, side-chains, each wire's level, width and pan, feedback wires (one block late, off unless Settings allows them), wires drawn as lettered jump cuts, several plug-ins moved at once, plug-ins ejected from their wires (or removed) leaving them loose, and plugged back in (loose wires that snap), Thru or Mute when bypassed, inputs and outputs moved and renamed per performance; a plug-in scanner (each file opened in a helper process) and browser | done, tested offline |
| Chains and scenes added, copied, moved, set (pedal, FCB display, warm chain, actions) and taken out from the app while a chain plays, all with undo | done, tested offline |
| Gain Stage in the app (the input box): every input's gain and every output's level, mute and zero-latency limiter, the same in every chain, with meters and Auto (peak, RMS or LUFS to a target) | done, tested offline |
| Per-plug-in process isolation | **not yet** |
| MIDI learn, looper, LV2, JACK, PipeWire | later |

Everything runs in one process for now. That is deliberate: the first measurement is what your chains cost in process, and isolation is then added where it is worth its cost.

## Build

On the mini PC (AV Linux MXE-25, Debian based):

```sh
sudo apt install build-essential cmake ninja-build pkg-config libasound2-dev qt6-base-dev libx11-dev
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/cue_tests
QT_QPA_PLATFORM=offscreen ./build/cue_app_tests
```

The app is built when Qt 6 is found (`-DCUE_BUILD_APP=OFF` leaves it out); the engine needs only ALSA.

All third-party code is vendored under `third_party/`: the CLAP headers (MIT), the VST3 SDK's interfaces and hosting helpers (MIT since SDK 3.8), for `.cue` files miniz (MIT) and nlohmann/json (MIT), and the app's fonts, Inter, Barlow Condensed and JetBrains Mono (SIL Open Font License).

## Commands

```sh
cue doctor                       # real-time settings: priority, governor, IRQs, USB, sound servers
cue devices                      # sound cards and MIDI ports
cue scan                                  # every plug-in in the usual folders, each opened by a helper (--all: again)
cue scan "<file.clap or bundle.vst3>"     # what is inside a plug-in file
cue params "<file.clap or bundle.vst3>"   # a plug-in's parameters and their ids
cue import-rpp "Cue Template.RPP" --mapping configs/cue-template-mapping.ini
cue export-rpp show.cue                     # a performance as a REAPER project (docs/converter.md)
cue-convert                                 # the converter's window: drop a .RPP or .cue on it
cue convert <config.ini> [out.cue] [--rig rig.json]  # save it as one .cue file (and this machine's settings as a rig profile)
cue check <config.ini|file.cue>  # load everything and print what Cue understood
cue check <config.ini> --no-load # the same without loading plug-ins
cue midi-monitor M2              # print what the FCB sends, decoded
cue fcb-test M2 --channel 1      # walk the FCB's LEDs and display
cue bench <config.ini> --wav out.wav   # offline: CPU per chain + a WAV of the switching
cue run <config.ini|file.cue>    # live; a .cue plays with ~/.config/cue/rig.json, or --rig FILE
cue run show.cue --audio none    # the same without a sound card (to edit and save on another machine)
cue ctl status | watch | select Slap | do "Wah = Auto" | save | quit   # talk to a running cue (docs/control.md)
cue-app                          # the app, for a running cue (docs/app.md)
cue-app --start show.cue --fullscreen   # ...starting cue run first if none is running
```

While `run` is going: keys `1`-`9` pick a preset, `n`/`p` step, `q` quits; `key` triggers in the config add more. The status line shows DSP load as a share of the period, the margin (the least audio still queued for playback when a block was written; at zero the sound card runs dry), xruns, the preset and controls, and the cost of each chain that is running (`avg/max` microseconds per block), cut to the terminal's width. Each xrun also gets a line of its own: when it happened, how long the block before it took and how late it started, and the slowest block around it with the chains that played in it.

## Config

An INI file; `docs/config.md` is the full reference. See `configs/builtin-demo.ini` (no plug-ins needed), `configs/test-plugins.ini` (bundled test plug-ins), `configs/my-rig.ini` (a hand-written template) and `configs/cue-template-mapping.ini` (your ReaLearn pedals).

```ini
[scene Covers]

[chain Punk]
plugin = clap:~/.clap/TwoStorms Gain Stage.clap
plugin = vst3:~/.vst3/yabridge/Neural DSP/Darkglass Ultra.vst3
state = states/punk-ultra.vst3state
plugin = clap:~/.clap/TwoStorms Wah.clap
name = Wah
bypass = yes
branch = FOH                 ; out 1 gets the IR, out 2 does not
tap = out2

[branch FOH]
plugin = clap:~/.clap/CLAP Plugins Linux/TwoStorms IR Loader.clap
bypass_mode = mute           ; bypassing the IR silences FOH instead of sending it the raw amp sim
tap = out1

[preset Punk]
pedal = 6
display = PU
do = Wah = Off

[control Wah]
values = Off, Auto, Pedal
pedal = 4
enable = Wah when Auto
```

## How switching works

Presets switch chains; two presets on the same chain switch without a crossfade, by their actions alone.

1. A switch request arrives (FCB pedal, program change or key).
2. If the target chain is idle, it starts processing live input, muted, for `preroll_ms`. Its input is faded in, so a delay line never starts on an abrupt edge that would echo back as a click. The chain is already clean: a chain whose tail rang out to silence needs nothing, and one that was cut off is cleared by a helper thread while it sits idle, so a slow plug-in reset (a delay clearing seconds of buffer) never lands in an audio block.
3. The new chain's output fades in over `crossfade_ms` (linear: the chains share one input, so their signals are correlated). At the same time the old chain's **input** fades out in dB; its output stays up, so its delays and reverbs ring out naturally.
4. The old chain keeps processing silence until it is below `tail_quiet_db` for `tail_hold_ms`, or until `tail_cap_ms`, then fades to nothing and stops costing CPU.
5. With `warm = next`, the chain of the next preset keeps processing in the background, muted, so switching to it skips the pre-roll. Its delays and reverbs hear silence until the switch, so it never replays notes from before and has nothing to clear.

A bypassed plug-in, with `bypass_mode = thru` (the default), passes its input on; with `mute` it passes nothing, so whatever follows it goes quiet. Either way it is fed silence until its tail dies away, then no longer processed, so bringing a delay back starts it clean.

Idle chains are never processed. Parameter changes still reach idle CLAP plug-ins (flushed every 5 ms); idle VST3 plug-ins receive them when their chain starts, during pre-roll.

## Known limits of the prototype

- **VST3 reset.** VST3 has no real reset call. Processing stays on while a plug-in is loaded (with yabridge every call is a round trip to Wine, too slow for a switch), and an idle chain's VST3 plug-ins are reset by toggling processing off and on from the helper thread, which the spec suggests plug-ins use to clear buffers, but not all do.
- **yabridge timing.** A bridged plug-in's block takes a round trip to its Wine process, which adds jitter. Cue keeps its other threads, and the Wine hosts it starts, off the audio core and on the performance cores. Chains that play at the same time (the old one ringing out, the new one, the warm one) are processed side by side on the audio thread and `workers` helper threads, as REAPER spreads its tracks over cores, so a switch costs about as much as its slowest chain. Chains not heard yet (the warm one, one pre-rolling) may finish while the next block starts, so a heavy or cold-starting one never holds up the chain being played. Plug-ins in series in one chain still have to fit in one period together; for two heavy amp sims in one chain use `periods = 3` or `period = 128`. If xruns remain at 64 frames, `periods = 3` in `[audio]` gives each block an extra period of slack for 1.3 ms more latency.
- **No isolation yet.** A plug-in crash takes the host down. Isolation is the next milestone.
- **Plug-in windows** are not shown yet: the app moves parameters with plain sliders. Set sounds up in REAPER and import the project, or in the app's chain view.
- **Tuner.** The built-in tuner shows in the app (the stage view's strip and the tuner view). On the FCB the Tuner pedal mutes and shows TU; a tuner on the FCB display is planned.
- **VST2** plug-ins (ReaEQ, ReaGate, ReaXcomp) are not hosted and are left out of an import. `Cue Template 2S` replaces them, and the LSP and FabFilter plug-ins, with TwoStorms ones.
- **Side-chains** come from an earlier plug-in of the same chain (main line first, then each branch), not from another input.
- **Bench numbers in a virtual machine** are noisy; only numbers from the mini PC count.

See `TESTING.md` for the test procedure on the rig.
