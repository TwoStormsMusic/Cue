# Testing the prototype on the rig

The goal is one answer: on the mini PC, with two or three of your heavy chains loaded, is switching from the FCB1010 instant and pop-free, with CPU spent only on what is playing? Each step below takes a few minutes. Send back the output of the steps marked **send**.

Close REAPER first: the host opens the M2 directly and the device can only have one owner.

## 1. Offline tests (no hardware)

```sh
./run-tests.sh
```

Runs the 29 unit tests (switching without clicks, idle scenes never processed, tails, warm scenes, CLAP and VST3 hosting) and two benches. Every test should say `OK`. **Send** the last 30 lines if anything fails.

## 2. Real-time settings

```sh
bin/stagehost doctor
```

**Send** the output. `FAIL` lines must be fixed before live tests mean anything; each line says how. Expect to need real-time priority for your user and probably `threadirqs`.

## 3. Devices and the FCB

```sh
bin/stagehost devices
bin/stagehost midi-monitor any
```

Press pedals 1, 2, 10, Up and Down, and move both expression pedals. You should see lines like `EurekaPROM: pedal 3 pressed`. Note the channel number and the name of the FCB's port. Then:

```sh
bin/stagehost fcb-test "<part of the port name>" --channel <channel>
```

The LEDs should light 1 to 10 in turn, the display count 1 to 10, and end on `OK`. **Send** what the monitor printed and whether the LEDs worked.

If the M2 is not called `hw:M2`, use the name `devices` shows in each config's `device =` line.

## 4. Switching with built-in blocks

```sh
bin/stagehost run configs/builtin-demo.ini
```

Three scenes made of built-in blocks, pedals 1 to 3 (or keys 1 to 3). Play, and switch while notes ring and while the delay in scene 3 is repeating. Listen for any click at the moment you press, and about 380 ms later (one delay time). The status line should show `xruns 0`. Leave it running for 10 minutes of switching. **Send** a few status lines and anything you heard.

## 5. Switching with your chains

Edit `configs/my-rig.ini`: two or three scenes from your real chains. The `rig-info.txt` you sent tells me the paths, so I can fill this in for you if you prefer.

First offline, which also shows each scene's cost:

```sh
bin/stagehost bench configs/my-rig.ini --seconds 60 --wav my-rig.wav
```

**Send** the output. Listen to `my-rig.wav` (left = out 1, right = out 2): it is your chains on a synthetic bass line, switching every 2 seconds.

Then live:

```sh
bin/stagehost run configs/my-rig.ini
```

## What passes

| Check | Pass when |
| --- | --- |
| No pops | No click at the switch or one delay time later, on out 1 or out 2 |
| No dropouts | `xruns 0` after 30 minutes, switching every few seconds |
| CPU only for the active scene | bench: "blocks processed by the idle scenes" is 0, and the scene-alone cost matches one chain |
| Switch delay | The new sound arrives after the pre-roll (50 ms) or at once for the warm scene |
| Tails | A delay or reverb tail carries on after a switch |
| Headroom | bench: the 99.9% block time stays under about 70% of the period |

If you hear a pop, tell me which scene you switched from and to, and whether it was at the press or a moment later. A pop a moment later points at a VST3 delay that did not clear its buffer.
