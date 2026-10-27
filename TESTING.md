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

Runs the 110 unit tests (switching without clicks, idle chains never processed, tails, warm chains, branches and wires, live rewiring while audio plays, the plug-in scan with plug-ins that crash or hang, bypass thru and mute, side-chains, background resets, CLAP and VST3 hosting, the config, the mapping, the REAPER import, .cue files and rig profiles, the control socket, several inputs and outputs, the built-in tuner), the app's 23 tests (every view at six screen sizes, taps, the routing board, the plug-in browser, and the app against a running engine that is stopped and started again) and two benches. Every test should say `OK` or `PASS`. **Send** the last 30 lines if anything fails.

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

## 7. Performance files and remote control (0.4.0)

0.4.0 saves a performance as one `.cue` file, keeps this machine's sound card settings in a rig profile, and lets another program watch and drive a running Cue (the app will, later; for now `cue ctl` does). The sound should not change at all. Use the TwoStorms template from step 6; nothing needs importing again.

**Convert it**, keeping its audio and MIDI settings as this machine's rig profile:

```sh
bin/cue convert "$HOME/cue/Cue Template 2S/Cue Template 2S.ini" --rig ~/.config/cue/rig.json
cat ~/.config/cue/rig.json
```

It writes `Cue Template 2S.cue` beside the INI, with every plug-in state inside it, so it no longer needs `states/`. The rig profile should say `hw:M2`, 64 frames, 3 periods, `In 1` on channel 1, `Out 1` on 1 and `Out 2` on 2. **Send** what both printed.

**Same sound.** Check the .cue, then bench it and the INI the same way and compare the two recordings:

```sh
bin/cue check "$HOME/cue/Cue Template 2S/Cue Template 2S.cue"
bin/cue bench "$HOME/cue/Cue Template 2S/Cue Template 2S.ini" --seconds 30 --fast yes --wav ini.wav
bin/cue bench "$HOME/cue/Cue Template 2S/Cue Template 2S.cue" --seconds 30 --fast yes --wav cue.wav
cmp ini.wav cue.wav && echo same
```

`same` means the .cue plays exactly the samples the INI does. If they differ, bench the INI twice and compare those: a plug-in with randomness of its own differs a little on every run, and then listening to both is the test. **Send** the output.

**Live, driven from a second terminal.** Play the .cue with the pedals as in step 5:

```sh
bin/cue run "$HOME/cue/Cue Template 2S/Cue Template 2S.cue"
```

In a second terminal on the mini PC, or over ssh from another computer:

```sh
bin/cue ctl status
bin/cue ctl watch                # the status twice a second and every xrun line, until Ctrl-C
bin/cue ctl select Slap          # or next, previous, or a number
bin/cue ctl do "Wah = Auto"
bin/cue ctl do "chain Djent"
```

A switch from `cue ctl` should sound exactly like one from a pedal, and the LEDs and display should follow. Leave `watch` running for 10 minutes while you play and switch: it should add no xruns. **Send** a few `watch` lines and anything you heard.

**Save from the running Cue.** While it plays, and while you play:

```sh
bin/cue ctl save "$HOME/cue/saved.cue"
bin/cue ctl quit
bin/cue check "$HOME/cue/saved.cue"
```

The save takes every plug-in's settings as they are at that moment. Listen while it saves: there should be no click and no xrun. `check` should list the same chains and plug-ins as before. **Send** what `save` and `check` printed, and whether you heard anything.

**Without the sound card.** With REAPER or PipeWire holding the M2:

```sh
bin/cue run "$HOME/cue/Cue Template 2S/Cue Template 2S.cue" --audio none
```

It loads every plug-in and runs them on a timer, hearing silence; `cue ctl` works as above. This is how the app will edit a performance on a laptop. **Send** the lines it printed at start.

## 8. The app (0.5.0)

0.5.0 adds `bin/cue-app`, the app for the touchscreen ([docs/app.md](docs/app.md)). It shows and plays what `cue run` is doing over its control socket, so the sound never depends on it. The engine is the same as 0.4.0 plus level meters and the built-in tuner, so the sound should not change. Use a copy of the .cue from step 7, because the app's **Save** writes to the file it plays:

```sh
cp "$HOME/cue/Cue Template 2S/Cue Template 2S.cue" "$HOME/cue/app-test.cue"
bin/cue-app --version
```

If that says a `libQt6` library is missing: `sudo apt install qt6-qpa-plugins libqt6widgets6t64 libqt6network6t64` (or without `t64` if apt cannot find those names). **Send** what it printed.

**Start both with one command**, on the touchscreen's desktop:

```sh
bin/cue-app --start "$HOME/cue/app-test.cue" --fullscreen
```

It starts `cue run` in the background (its lines go to `~/.local/state/cue/engine.log`) and shows "Waiting for Cue" until the plug-ins are loaded, then the stage view. F11 leaves full screen; Settings > Size makes everything bigger or smaller.

**Stage view, in step with the FCB.** Play, and check:

- The chain name, the FCB DISPLAY box and the LEDs say the same thing.
- The levels move with your playing: Bass (the input), then FOH and Amp.
- Tap a chain button: it switches exactly like its pedal, and the FCB's display and LEDs follow. Press the pedals: the screen follows at once.
- Tap Effect 1-3 and the Wah's Off / Auto / Pedal: the sound and the LEDs follow. Press their pedals: the buttons follow.
- The dashed **warm** tag sits on the chain after the one playing.

**Tuner.** Tune a string or two on the strip, then tap it for the tuner view. The note should settle quickly and hold steady, green within 2 cents. Press the tuner pedal: the outputs mute and the tuner view comes up by itself; press it again and it goes back. Compare with a tuner you trust if you have one at hand.

**Chains.** Tap **Chains**: the chain playing is drawn as blocks and wires, as on the routing mockup. Tap a plug-in: **Bypass** switches it (the block goes grey, the sound changes), and its parameters move with the sliders (try the Gain Stage). Tap **Save**, then:

```sh
bin/cue check "$HOME/cue/app-test.cue"
```

`check` should show the plug-in you bypassed as bypassed. **Send** what it printed.

**The app is not the sound.** Mid-song, close the app from ssh or a terminal, the hard way:

```sh
pkill -9 cue-app
```

The sound carries on, and the pedals still switch, light and show. Start it again (`bin/cue-app --fullscreen` on the touchscreen, or `DISPLAY=:0 bin/cue-app --fullscreen` from ssh): within a second it shows the chain playing. **Send** whether anything at all was heard.

**Costs nothing.** Leave the app on the stage view for 30 minutes while you play and switch, then look at the Xruns pill (or `bin/cue ctl status` over ssh). It should still say 0. **Send** the number and, if you have it, `top` showing `cue-app`'s CPU.

When done, `bin/cue ctl quit` stops the engine (closing the app does not).

## 9. Tuner, mute and sliders (0.5.1)

0.5.1 answers the notes on 0.5.0. Same start as step 8 (`bin/cue-app --version` should say 0.5.1):

```sh
bin/cue-app --start "$HOME/cue/app-test.cue" --fullscreen
```

**The tuner looks like the TwoStorms Tuner.** Tap **Tuner**. The note, its sharp or flat and octave, the chevrons, the cents and hertz under the note, the needle and the strobe should all look and move as in the TwoStorms Tuner plug-in (open it in REAPER beside it if you can). The stage view's strip is the same, small. **Send** a photo if anything still looks off.

**The tuner pedal mutes the inputs.** On a chain with a delay or a reverb, play a note and press the tuner pedal while it rings:

- The delay or reverb rings out instead of stopping, and the tuner still reads your strings. The pane is framed in red with **INPUTS MUTED**, and the **Mute** button turns red.
- Once the tail has died away the chain rests, so the CPU pill should drop and `bin/cue ctl status` over ssh should show that chain's time at 0. A plug-in that keeps making a little noise of its own keeps its chain awake; **send** which one if so.
- Press the pedal again: the sound is back at once with no click. Switch chains while muted, then unmute: the new chain plays.

**Sliders stay where you leave them.** Tap **Chains**, tap the Gain Stage, and move a slider: it stays where you let go and the sound follows. Do the same on a Neural DSP plug-in. Tap **Save**, then `bin/cue check "$HOME/cue/app-test.cue"`. **Send** what it printed.

Drawing wires and adding plug-ins are not in 0.5 yet: they come with the routing editor.

## 10. Plug-in editors (0.6.0)

Each plug-in's own window, the one you know from REAPER, opened by Cue. The engine opens it on the screen of the computer it runs on, so do this on the mini PC's own desktop, not over ssh. **This is the step that matters most: Neural DSP through yabridge is the one thing only your rig can prove.** `bin/cue-app --version` should say 0.6.0.

```sh
bin/cue-app --start "$HOME/cue/app-test.cue" --fullscreen
```

**From the app.** Tap **Chains**, tap a plug-in, then **Open editor**. Expect its window on the screen, drawing and taking the mouse. The button becomes **Close editor**. Try, in this order:

- A TwoStorms plug-in (Gain Stage, Delay). It should look exactly as it does in REAPER.
- **A Neural DSP plug-in (Parallax, Darkglass).** This goes through yabridge and Wine, which is the part I cannot test here. **Send** what happens, whatever it is: the window, a blank window, nothing at all, or a crash. If it does crash, say whether the sound carried on.
- Two editors at once, from different chains. Both should draw; closing one should leave the other alone.

**It is heard at once, and kept.** With an editor open, play and turn a knob in it: the sound follows. Then tap **Save** in the app and, over ssh or in another terminal:

```sh
bin/cue check "$HOME/cue/app-test.cue"
```

Load the file again (`bin/cue-app --start "$HOME/cue/app-test.cue"`) and open that plug-in's editor: the knob should be where you left it, not back where it was.

**The sound does not wait on it.** With an editor open, play for a few minutes, switch chains, and watch the Xruns pill. It should stay at 0. Drag the editor's window around and resize it while playing: still 0. **Send** the number, and the CPU pill with an editor open and closed.

**It closes cleanly.** Close an editor with its window's own X button: the app's button should go back to **Open editor** within a second. Then `bin/cue ctl quit` with editors open: everything should go away without a crash. **Send** the last lines it printed.

From ssh you can also drive it: `bin/cue ctl editor 2 1` opens chain 2's first plug-in, `bin/cue ctl editor 2 1 close` shuts it. Over ssh the window still opens on the mini PC's screen (or it says there is no screen to open it on, if you started the engine over ssh without one).

Note: `./run-tests.sh` now opens and closes a few small test windows when it is run on a desktop. That is the editor test, and it is quick.

## 11. Zoom, picked parameters, the stage strip (0.6.1)

Same start as step 8 (`bin/cue-app --version` should say 0.6.1; it has the plug-in editors of step 10 too). Try each on the touchscreen and with a mouse.

**The chain zooms and moves.** Tap **Chains**.

- The mouse wheel zooms in and out where the pointer is. On the touchscreen, pinch with two fingers; the two fingers also move it. The **−**, **Fit** and **+** buttons at the bottom right of the canvas do the same, and the number beside them says how big it is.
- Drag open space (not a block) to move the chain around. A double tap on open space fits the whole chain again.
- A tap on a block still selects it, zoomed or not.

**Pick the parameters a block shows.** Tap a Neural DSP plug-in.

- Before you pick anything it shows its first eight parameters. A parameter a control drives (`drive =`) or a trigger sets (`param`) always shows, its slider purple, with what moves it under its name.
- Tap **Show all parameters**: every parameter in the plug-in's order, a checkmark on the ones shown, **Collapse all parameters** at the top. Long lists have a search box, and the plug-in's own sections fold (tap a section's name).
- Check and uncheck a few, then **Collapse all parameters**: only the checked ones show, newly checked ones at the end.
- Drag a row by its grip (the three lines at its left) up or down to put it in its place.
- Tap **Save**, quit the app and the engine, start them again: the same parameters show in the same order.

**The stage strip is only the note and the needle.** On the stage view, the tuner strip shows the note with its chevrons and the needle, no strobe, cents or hertz. The tuner view is unchanged.

**Send** what feels wrong, with a photo if it is about the look.

## 12. Editors over Cue, double taps, undo (0.6.2)

Same start as step 8 (`bin/cue-app --version` should say 0.6.2). Try each on the touchscreen and with a mouse.

**Editors stay over Cue.** Open a TwoStorms editor and a Neural DSP one. Use the editor, then tap or click in Cue.

- The editor stays in front of Cue, full screen or not. It goes only when you close it.
- **Close editor** closes it on the first tap, with a TwoStorms plug-in as with Neural DSP.
- A slider in Cue's panel moves on the first touch and the editor stays where it is.
- Type a value into a TwoStorms editor's text box: the keys should go to it. **Send** it if they do not.
- With two editors open, tapping the one underneath brings it to the front.

**Double taps.** Tap a chain in the list twice: it plays. Tap a plug-in block twice: it switches off (or on) and stays selected. One tap on the selected block still lets it go.

**Wires.** Look at a chain that is not playing: its wires are green where they would carry sound, grey after a bypassed plug-in that passes silence. Mute with the tuner pedal: every wire goes grey.

**Undo.** Move a slider, bypass a plug-in, pick a parameter. Then **Undo** (top right, or Ctrl+Z) three times: each comes back, in the reverse order, and the panel shows the plug-in it was on. **Redo** (or Ctrl+Shift+Z) makes them again. Changes made with a pedal or in a plug-in's own editor are not undone by it.

## 13. The routing board (0.7.0)

Same start as step 8 (`bin/cue-app --version` should say 0.7.0). Copy your `.cue` first: once 0.7.0 saves it, it is format 2, and 0.6 cannot open it. Try each on the touchscreen and with a mouse.

**First, what 0.6.2 left to check** (step 12 has the details):

1. With a TwoStorms editor open, **Close editor** and the panel's sliders work on the first tap.
2. Typing into a TwoStorms editor's text box, and a Neural DSP one's, works.
3. Tapping a chain twice plays it; tapping a plug-in twice switches it off or on.
4. On a chain that is not playing, wires are green where sound would pass and grey where it would not.
5. **Undo**, Ctrl+Z and Ctrl+Y (redo) work.

**Your performance opens as it was.** Open your 0.6 `.cue`. Every chain should sound the same as before, and its board should show the same plug-ins in the same order, branches on rows below.

**Wires.** On a chain that is playing:

- Drag from a plug-in's right dot to an output: a new wire. You should hear it join with no click. Drag from the input or a plug-in's right dot to another plug-in's left dot: two wires into one input add up.
- A wire from a plug-in back to one before it is refused with a message.
- Tap a wire: it turns orange and the panel shows its level, width and pan. Move each fader while you play: the sound follows smoothly, with no click and no gap. A double tap on a fader sets it back. The wire gets thick, with a tag saying what changed; Settings > **Show modified wire parameters** hides the tags.
- Width at 0% makes that wire mono; pan then turns one side down, like a balance.
- **Mute** silences that wire only; **Delete** (or the Delete key) takes it away.

**Plug-ins.**

- Tap the **+** on the selected wire and pick a plug-in: it goes onto that wire and the blocks after it move along. You should hear a short fade, no click.
- **+ Plug-in** puts one on the board unwired: grey, "not connected", and it costs no CPU until it is wired from the input to an output.
- Drag a block somewhere else. Wire it. The board stays as you left it.
- Select a plug-in and **Remove** it (or press Delete). Then **Undo**: it comes back with its settings and its wires. **Redo** takes it out again.
- With a plug-in on the board that has a side-chain input (a gate, a compressor with a key), drag a wire to the small triangle low on its left.

**Save and open again.** Tap **Save**, quit the app and the engine, start them again: the wires, their levels, widths and pans, and where you put each block are all as you left them.

**Rig check.** Then play for 30 minutes, switching chains every few seconds, with a wire edit or two every few minutes. **Send** the status line at the end: `xruns 0`, as in step 8.

## 14. The plug-in browser (0.8.0)

0.8.0 has everything of step 13 too: do step 13 first with it (`bin/cue-app --version` should say 0.8.0), then this.

**The scan.** On the mini PC, from a terminal:

```sh
bin/cue scan
```

It lists every file as it opens it, then every plug-in, the TwoStorms ones and the Neural DSP ones through yabridge among them. Each yabridge plug-in starts Wine once, so the first scan takes a while. **Send** the last lines (how many plug-ins, how many skipped, and why each was skipped). Run it again: it should be done at once, as nothing changed.

**The view.** In the app, on the chain view, tap **Plug-ins** (top right).

- Every plug-in from the scan is listed. Tap the vendors and collections on the left, the formats over the list, and type in the search: the list follows.
- Tap a TwoStorms plug-in: its details show on the right (inputs, editor, MIDI in). A Neural DSP one says "known once added" for those.
- **Add to favourites** on two or three you use often.
- **Folders…** lists where Cue looks. Add a folder you keep plug-ins in, if any, and see its plug-ins appear.

**Adding.** On a chain that is not playing:

- Select a wire, then **Plug-ins**, pick a Neural DSP plug-in and tap the orange **Insert on …** button. You are back on the same chain with it on that wire. Open its editor: it works as any other.
- With nothing selected, a double tap on a plug-in in the list adds it to the chain unwired.
- **+ Plug-in** on the board now lists your favourites first, and **Browse all plug-ins** goes to the list.
- **Undo** takes each of them out again.

Play while you do all of this, then check the status line: still `xruns 0`. **Send** anything skipped that should not be, and what feels wrong.

## 15. Chains and scenes (0.9.0)

0.9.0 has everything of steps 13 and 14 too (`bin/cue-app --version` should say 0.9.0). Copy your `.cue` first. Try each on the touchscreen and with a mouse.

**First, the five checks 0.6.2 left** (step 12 has the details). Please do these before anything else:

1. With a TwoStorms editor open, **Close editor** and the panel's sliders work on the first tap.
2. Typing into a TwoStorms editor's text box, and a Neural DSP one's, works.
3. Tapping a chain twice plays it; tapping a plug-in twice switches it off or on.
4. On a chain that is not playing, wires are green where sound would pass and grey where it would not.
5. **Undo**, Ctrl+Z and Ctrl+Y (redo) work.

**Then steps 13 and 14**, if you have not done them with 0.7.0 or 0.8.0.

**New chains.** On the chain view, while a chain plays:

- **+ Chain** (bottom left): a new empty chain at the end of the list, shown, its input wired to both outputs. Nothing you hear changes.
- Put a plug-in or two on it, then tap it twice in the list: it plays, like any other.
- Show another chain and tap **Duplicate** (under the board): a copy just after it ("Slap 2"), sounding the same. Move a parameter on the copy: the original keeps its own.
- **Undo** takes the copy out; **Redo** brings it back.
- **Copy from scene…** (once you have two scenes) copies a chain from another scene into this one.

**A chain's settings**, under the board:

- **Name**: tap it, type, Enter (Esc leaves it as it was). The list and the stage view follow.
- **Selected by**: pick a pedal. The menu says what else each pedal does. Pick one another chain uses: that chain loses it, and **Undo** gives it back. Press the pedal on the FCB: this chain plays.
- **FCB display**: up to 2 letters, shown on the FCB while it plays.
- **Keep warm while … plays**: pick a chain. Play this one, then switch to the one you picked: it comes in at once, with no pre-roll. The list says "warm" beside it.
- **When selected, do**: **+ Action** and pick "Wah = Auto" (or one of yours). Play another chain, turn Wah off, play this one again: Wah goes to Auto. Tap the chip to take it out or move it.
- **Move**: up, down, or into another scene. **Remove**: gone (not the chain playing; Cue says why). **Undo** brings it back with its plug-ins and settings.

**Scenes.**

- **+ Scene** (bottom left): a new scene after this one, with one empty chain; its settings are shown under the board so you can name it. The picker at the top of the column lists it.
- Pick your first scene in the picker, then the **Scene** tab under the board, then **Duplicate**: a copy of the whole scene, every chain with its plug-in settings and pedals.
- Give the copy a pedal in its **Selected by**, and switch scenes with the FCB. The stage view shows the scene's chains.
- **Remove** the copy (not while it plays): its chains go. **Undo** brings it all back.

**Save and open again.** **Save**, quit the app and the engine, start them again: the chains, scenes and settings are as you left them, and the pedals and displays work.

**Rig check.** Play for 30 minutes, switching chains every few seconds, and add or copy a chain now and then while you play. **Send** the status line at the end: still `xruns 0`. Send too anything that feels wrong or that you would lay out differently.

## 16. Your 0.9.0 notes (0.9.1)

`bin/cue-app --version` should say 0.9.1. Steps 13 to 15 still apply; this checks what changed.

1. **Typing in a Neural DSP editor.** Open a Neural DSP plug-in's editor and type into one of its text boxes (a preset name, a value). The keys go there. Then check **Close editor** and the panel's sliders still work on the first tap with a TwoStorms editor open (step 15, check 1), as that is what the change touched.
2. **A switched-off plug-in.** Bypass a TwoStorms plug-in, move one of its sliders in the panel: it stays where you put it. Turn the plug-in on: it sounds as set.
3. **Typing a value.** Tap a parameter's value (right of its slider): after a moment a box opens; type a value as the plug-in writes it ("-6 dB", "250 ms", or just a number) and press Enter. Tap a value twice: it goes back to its default. **Undo** takes back both.
4. **Undo for a plug-in's own window.** Open an editor, turn a knob, let go. **Undo** (in Cue) turns it back, and the knob in the window follows; **Redo** turns it again. One undo per knob grab. Try a TwoStorms plug-in and a Neural DSP one.
5. **FCB display.** The display field takes two letters.
6. **Scanning while playing.** With a chain playing, run `bin/cue scan --all` in a terminal. **Send** whether any xruns show up while it runs (they should not now, as the scan stays off the audio core; a Windows plug-in starting Wine can still load the machine, so tell me if it does).

## 17. Sound card and feedback wires (0.9.2)

`bin/cue-app --version` should say 0.9.2. Steps 13 to 16 still apply.

1. **REAPER open.** Save anything open in REAPER, leave REAPER playing on the M2, then start Cue the way you do (`bin/cue-app --start your.cue`). Cue starts, REAPER closes, and the app says what it closed. **Send** the `audio:` lines from `~/.local/state/cue/engine.log`.
2. **PipeWire.** Play something from a browser through the M2 (if PipeWire has it there), then start Cue. Cue gets the card; the browser goes quiet while Cue runs and plays again after Cue quits.
3. **Turned off.** Settings > **Take the sound card** > Off, quit the app and the engine (`bin/cue ctl quit`), open REAPER on the M2 and start Cue again: it does not start and the log says the card is busy. REAPER keeps playing. Turn the setting back on.
4. **No loops while drawing.** Drag a wire from a plug-in's output dot across its own input and key, and across a plug-in before it in the chain: none of them light up, and letting go there makes nothing.
5. **Feedback wires.** Settings > **Feedback wires** > On. Draw a wire from a plug-in's output to its own input, or to one before it: it is drawn purple. Turn its level down (try -12 dB) and play: it sounds like a very short echo or comb filter. Then, at low volume and not through FOH, turn it up past 0 dB: it builds up to a loud steady level and stops there rather than growing for ever. **Delete** it, and turn the setting off again.

## 18. Board, stage header and jumps (0.9.3)

`bin/cue-app --version` should say 0.9.3. Steps 16 and 17 still apply.

1. **Board buttons.** In Chains, **+ Plug-in** is at the right of the chain's name, with smaller **− Fit +** and the zoom % just left of it, off the board.
2. **Stage header.** Settings > **Stage shows**: tap CPU, Margin, Xruns, Sound card or Controller to hide or show each; the stage view follows. Xruns and an FCB that is not found still show when they happen.
3. **Values while dragging.** Drag a Neural DSP parameter's slider in the panel: the value stays in the plug-in's own form ("-6.0 dB", "Off") while you drag, a moment behind your finger, rather than turning into %.
4. **Jumps.** Lay a long chain out as two rows (drag the plug-ins), tap the wire that turns back from the end of the first row to the start of the second, and tap **Draw as a jump**: it becomes a lettered stub out of the first row's end and the same letter into the second row's start. Tap either stub to select it again. It sounds the same. **Undo**, **Save** and open again: it stays a jump.

## 19. Column, Remove, jump cuts and moving several (0.9.4)

`bin/cue-app --version` should say 0.9.4.

1. **Column.** In Chains, the **‹** left of the chain's name hides the scenes and chains column; **›** brings it back, and the app remembers which.
2. **Remove.** The chain and scene settings say **Remove** (not "Take out"), and undo says "Removed ...".
3. **Editors close with the app.** Open two plug-in editors, then quit the app (Ctrl+Q or close its window): both editors close and the sound plays on. Start the app again: the chains are as they were.
4. **Jump Cut.** Select a wire, tap **Jump Cut** (it lights up while on; tap again to make it a plain wire). Make two jump cuts from one plug-in: their letters start one above the other. Drag a letter somewhere else: it stays there after **Save** and opening again; **Undo** puts it back. The letters run A, B, C with none missing.
5. **Moving several.** Shift-click two or three plug-ins (they all turn orange), drag one: they all move; **Undo** moves them all back. Shift-drag across open space: what the box touches is selected. On the touchscreen, turn on **Select** (beside the zoom), tap plug-ins or drag a box, then turn it off. Tap a plug-in on its own, or open space, to let go.

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
| Same sound | `cmp` says `same`, or the .cue sounds the same as the INI |
| Remote control | `cue ctl` switches as cleanly as the pedals, and `watch` adds no xruns |
| Plug-in editors | Each plug-in's own window opens, is heard at once, is kept by Save, and leaves Xruns at 0 |
| Save | Saving while playing makes no click; the saved file checks and plays |
| App in step | Taps and pedals switch the same way, and the screen and the FCB always agree |
| Tuner | The note settles fast and holds steady; the tuner pedal brings the tuner view up and back |
| App closed | `pkill -9 cue-app` mid-song makes no sound at all; the app comes back within a second |
| App costs nothing | 30 minutes with the app on the stage view: still `xruns 0` |
| Routing | Wiring, inserting and removing while playing makes no click; fader moves on a wire are smooth; 30 minutes of it, still `xruns 0` |
| Plug-in browser | `cue scan` finds every plug-in REAPER has, skips nothing that works, and is quick the second time; adding from the list works on any chain |
| Chains and scenes | Adding, copying or taking out a chain or a scene while playing makes no click and no gap; pedals, displays and warm chains follow; 30 minutes of it, still `xruns 0` |

If a plug-in takes most of a period in one block, `cue run` prints a `slow block:` line naming it, and `cue bench` names each chain's slowest plug-in. Each xrun gets an `xrun` line: when it happened, how long the block before it took and how late it started, and the slowest block around it with each chain's time. A late start with a short block points at something else holding the CPU; a long block points at the plug-ins. The status line's `margin` is the least audio left to play when a block was written: the closer to 0, the closer to an xrun. A stall in the first quarter second, before anything is heard, gets a `start-up:` line instead and is not counted. If you hear a pop, send those lines and tell me which chain you switched from and to, and whether it was at the press or a moment later. A pop a moment later points at a VST3 delay that did not clear its buffer.
