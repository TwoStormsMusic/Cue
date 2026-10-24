# Testing the prototype on the rig

The goal is one answer: on the mini PC, with your template's five chains loaded, is switching from the FCB1010 instant and pop-free, with CPU spent only on what is playing? Each step below takes a few minutes. Send back the output of the steps marked **send**.

Cue opens the M2 directly, and the device can only have one owner. Close REAPER first, and stop PipeWire from holding the M2 (`rig-info.txt` showed it does):

```sh
systemctl --user stop pipewire.socket pipewire.service wireplumber.service pipewire-pulse.socket pipewire-pulse.service
```

They come back at the next login, or with `systemctl --user start pipewire.socket pipewire-pulse.socket wireplumber.service`.

## 1. Offline tests (no hardware)

```sh
./run-tests.sh
```

Runs the 64 unit tests (switching without clicks, idle chains never processed, tails, warm chains, branches, bypass thru and mute, side-chains, background resets, CLAP and VST3 hosting, the config, the mapping, the REAPER import) and two benches. Every test should say `OK`. **Send** the last 30 lines if anything fails.

## 2. Real-time settings

```sh
bin/cue doctor
```

**Send** the output. `FAIL` lines must be fixed before live tests mean anything; each line says how. Expect to need real-time priority for your user and probably `threadirqs`.

## 3. Devices and the FCB

```sh
bin/cue devices
bin/cue midi-monitor M2
```

Press pedals 1, 2, 10, Up and Down, and move both expression pedals. You should see lines like `EurekaPROM: pedal 3 pressed` and the channel. Then:

```sh
bin/cue fcb-test M2 --channel 1
```

The LEDs should light 1 to 10 in turn, the display count 1 to 10, and end on `OK`. **Send** what the monitor printed and whether the LEDs worked. If the FCB sends on another channel, put that channel in the configs' `[midi] channel =` line (Cue listens on every channel, and sends LEDs and display on that one).

If the M2 is not called `hw:M2`, use the name `devices` shows in each config's `device =` line.

## 4. Switching with built-in blocks

```sh
bin/cue run configs/builtin-demo.ini
```

Three chains made of built-in blocks, no plug-ins. Pedals 6 to 8 (or keys 1 to 3) pick Clean, Drive and Delay; pedal 1 switches the delay, pedal 4 steps the drive setting, pedal 5 mutes and shows MU. Play, and switch while notes ring and while the delay in the third preset is repeating. Listen for any click at the moment you press, and about 380 ms later (one delay time). The LEDs and display should follow. The status line should show `xruns 0`. Leave it running for 10 minutes of switching. **Send** a few status lines and anything you heard.

## 5. Your template

Import the REAPER project with your pedals:

```sh
bin/cue import-rpp "Cue Template.RPP" --mapping configs/cue-template-mapping.ini
```

It writes `~/cue/Cue Template/Cue Template.ini` and the plug-in settings in `states/`, and prints what it left out (the VST2 ReaEQ, ReaGate and ReaXcomp, and the tuner track) and anything it could not find. If a plug-in is marked not found, add `--search <folder where it is>` and run it again. **Send** what it printed.

Then check it, which loads every plug-in once:

```sh
bin/cue check "$HOME/cue/Cue Template/Cue Template.ini"
```

**Send** the output. Then offline, which also shows each chain's cost:

```sh
bin/cue bench "$HOME/cue/Cue Template/Cue Template.ini" --seconds 60 --wav template.wav
```

**Send** the output. Listen to `template.wav` (left = out 1, right = out 2): your chains on a synthetic bass line, switching every 2 seconds.

Then live:

```sh
bin/cue run "$HOME/cue/Cue Template/Cue Template.ini"
```

The pedals do what ReaLearn did:

| Pedal | Does |
| --- | --- |
| 6-10 | Punk, Slap, 2000s, Motown, Djent (display PU, SL, OO, MO, DJ). Each starts with Effect 1 on, Effects 2 and 3 off and no wah |
| 1-3 | Effect 1-3 on and off (LEDs only, as in REAPER: they switch nothing yet) |
| 4 | Wah: Off, Auto, Pedal, Off ... The pedal wah follows the right expression pedal (it listens to CC 103) |
| 5 | Tuner: mutes both outputs and shows TU; press again to unmute |
| Up, Down | next and previous preset |

## 6. The TwoStorms template

`Cue Template 2S.RPP` is your template with every plug-in that has a TwoStorms counterpart replaced by it (everything but the Neural DSP ones). In REAPER, open each IR Loader once and load its IR again: the converted project names the IRs but has no samples saved yet, and the IR Loader only plays saved samples. Save the project, then import it like the first one:

```sh
bin/cue import-rpp "Cue Template 2S.RPP" --mapping configs/cue-template-mapping.ini
bin/cue check "$HOME/cue/Cue Template 2S/Cue Template 2S.ini"
bin/cue bench "$HOME/cue/Cue Template 2S/Cue Template 2S.ini" --seconds 60 --wav template-2s.wav
```

The import reads each IR file named in the project and stores its samples in the IR Loader's state, so the IRs play in Cue even if you skipped the step above (it lists any file it could not read). `check` should print `side-chain from 'TwoStorms Gate:1'` for Djent's second gate and `primed every plug-in in ... ms`. **Send** what the import, `check` and `bench` printed. Then run it live as in step 5 and listen for the same things, plus:

- **Out 2 (amp)** gets everything but the IR and the Saturator after it, chorus, delay and tremolo included; **out 1 (FOH)** gets all of it. The time effects run twice for that (one copy on each side of the IR), and every mapping that names them switches both.
- A bypassed IR Loader is set to pass its input (thru) unless you add `bypass_mode = mute` under it.

## What passes

| Check | Pass when |
| --- | --- |
| No pops | No click at the switch or one delay time later, on out 1 or out 2 |
| No dropouts | `xruns 0` after 30 minutes, switching every few seconds |
| CPU only for what plays | bench: "blocks processed by the idle chains" is 0, and the chain-alone cost matches one chain |
| Switch delay | The new sound arrives after the pre-roll (50 ms) or at once for the warm chain |
| Tails | A delay or reverb tail carries on after a switch |
| Headroom | bench: the 99.9% block time stays under about 70% of the period |
| Pedals | LEDs and display match the table above |

If you hear a pop, tell me which preset you switched from and to, and whether it was at the press or a moment later. A pop a moment later points at a VST3 delay that did not clear its buffer.
