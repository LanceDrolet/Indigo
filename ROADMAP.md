# Indigo roadmap

User-facing behaviour is in the README. This file keeps what's next, and the decisions behind what's built.

## Planned

### Pad groups
Link several pads so one move of a control applies to all of them, e.g. one fader for a drum kit spread
over four pads, or muting all the backing loops at once. Separate from choke groups: a pad can be in both.

Decided 2026-10-25:
- Groups 1–8, like choke groups: right-click a pad > Group; a coloured tag on the pad and on its mixer strip.
  A pad is in at most one group.
- Each group has its own set of linked controls, chosen with checkboxes (Settings > Groups, one row per group):
  Volume, Pan, Mute, Solo, Start, Stop. Defaults: Volume, Mute, Solo on.
- No grouped recording: REC stays per pad (no need for it yet).
- Volume and pan are relative: moving one member's fader moves the others by the same amount in dB, so the
  balance within the group is kept. A member that reaches the bottom or top stops there while the others carry on.
- No separate group (VCA) fader: the linked member faders are enough.
- Hold a modifier (Alt) while moving a control to change that pad on its own.
- Mute / Solo / Start / Stop on one member apply to the whole group on the same sample (one engine command,
  not one per pad), so grouped loops start and stop on the same grid line.
- Choke groups don't act between pads started together by their pad group (two members sharing a choke group
  would otherwise choke each other). Pads outside the group are choked as usual.
- MIDI: a CC mapped to a grouped control moves the group, with the usual soft takeover.
- Group membership and settings are saved in templates and sessions (pad behaviour, like choke groups).

Implementation notes:
- Engine: a group id and a linked-controls mask per pad in LooperEngine; trigger / release / mute / solo
  commands expand to every member inside the audio thread (like chokeOthers()).
- Mixer.h / MixerPanel.h: relative fader and pan links; group tag on the strip.
- Session.h: save `group` per pad and the groups' settings, next to `choke`.
- SettingsWindow.h: new Groups tab.

## Ideas / smaller items
- More sync: follow JACK transport. MIDI clock and MTC out are built (see Done).
- Plugin latency changed live (e.g. a lookahead setting in the plugin's editor) is only picked up when the
  chain is next sent (add / remove / move an effect); follow it as it changes.
- Sessions: option to reopen the last session at startup; Ctrl+S / Ctrl+O shortcuts.
- Preview: volume control; remember the auto-preview checkbox between sessions.
- Plugin menus: sort by type (distortion, reverb, etc.) instead of by manufacturer.
- Mixer: Mute all / Unmute all are provisional and may be reworked.
- Windows build: in CMakeLists.txt make JUCE_JACK=1 Linux-only and add JUCE_ASIO=1 for WIN32 with a
  cache variable for the Steinberg ASIO SDK path (SDK downloaded separately; GPLv3 or Steinberg licence).
  WASAPI needs nothing. Check MSVC warnings. Note: classic WinMM MIDI is single-client, so a controller
  held by a DAW won't reach Indigo.

## Long term

### State feedback to the floor controller
Indigo sends state back to the Pico floor controller (8 pedals) so each pedal shows its mode: recording,
playing, muted, Rec Lock, etc. Today the only MIDI out is the sync port (SyncOutput.h); nothing goes back
to a controller.

Undecided: LEDs or a 16x2 I2C display per pedal.
- LEDs: Indigo sends the mapped note / CC back with the state as the value; the Pico lights accordingly.
  Simple on both sides.
- Displays: cheap, and new info means changing text instead of adding hardware. Needs SysEx (see below).

Leaning MIDI, not OSC (considered 2026-11-07): USB MIDI keeps footswitch timing tight and plug-and-play;
OSC would mean a Pico W on Wi-Fi (jitter, dropouts, IP setup). OSC is only worth it for phone / tablet
control or a richer display.

If displays, a sketch:
- Indigo owns the text; the Pico only draws it, so new modes don't need a firmware update.
- SysEx `F0 7D <note-or-cc> <line 0|1> <16 ASCII bytes> F7` (7D = non-commercial ID). Pedals are addressed
  by the note / CC they send, which Indigo already knows from MIDI Learn, so there's no pedal table.
- Pico sends a hello SysEx on boot; Indigo replies with every pedal's text, so either side can start first.
- Indigo: open an output to the controller; send text when a mapped control's state changes.
- Hardware: PCF8574 backpacks have 8 addresses (0x20–0x27; PCF8574A 0x38–0x3F), so exactly 8 per bus,
  or split across the Pico's two I2C buses / a TCA9548A mux. LCDs are 5 V and the Pico is 3.3 V: remove the
  backpack pull-ups and pull up to 3.3 V, or level-shift. I2C LCD writes are slow, so drive them from
  core 1 and only rewrite changed characters. 8 backlights ≈ 150–250 mA.

## Done

### Pads and recording
- Pad grid with one-shot / loop / gate modes; quantized master clock, metronome.
- Play / Stop transport (2026-10-31).
- Pitch-preserving loop stretch to the project tempo.
- Live recording and overdub with latency compensation; input monitoring.
- REC ends with the recording; Rec Lock keeps it on.
- A loop length set on an empty pad: the recording loops at that length, each pass layered on the last.
- Pad Clear Sound / Clear Sound & Length / Wipe (right-click menu).
- Choke groups 1–8: starting a pad stops the others in its group on the same sample, so loop swaps land on one
  grid line. All stops fade out over 5 ms.

### Sessions, browser, settings
- Menu bar; Settings window.
- Settings > Audio (ALSA or JACK; device, sample rate, buffer size, channels), remembered between runs. A device
  change re-reads the input channels and latency and resamples loaded pads to the new rate. If the saved input
  device is missing, Indigo starts with no input.
- Templates (*.indigotemplate: pad behaviour) and sessions (*.indigo: tempo, quantize, modes, lengths, sounds; takes
  saved as WAVs beside it). Save prompt for unsaved takes.
- File browser: preview (double-click, or auto-preview on selection), navigation (Back / Up / Home / Browse...,
  typed path), a starting folder or reopen the last one.

### MIDI
- Per-pad note / CC mapping, MIDI learn, named presets.
- Channels: each mapping has a channel (Any or 1-16; Learn picks up the controller's), so two controllers sending
  the same notes can drive different pads. A Performance channel is ignored by the controls and saved with the
  preset. Old presets load as "Any".
- Hot-plug: a controller plugged in while Indigo is running is enabled automatically.

### VST3 / LV2 plugins and MIDI pads
Done 2026-10-24. Hosting comes from juce_audio_processors. VST3 and LV2 only: Steinberg no longer licenses the
VST2 SDK.
- Scanning in a child process, so a crashing plugin can't take Indigo down. Plugin manager window (Edit > Plugins)
  with each plugin enabled or disabled.
- A chain per pad (instrument and effects), and one on the input. Saved in templates and sessions with each
  plugin's state; a plugin missing on session load is removed from the pad and the user is told which.
- Pad chains are on the pad's OUTPUT, so effects can be changed at any time and recordings are dry. Only the input
  chain is recorded. Instrument pads record the instrument dry, before their effects. (Pad chains started out on
  the recording path; moved 2026-10-24.)
- Active pad: exactly one, the last pad pressed, clicked, hit, or triggered by a MIDI note (blue outline). With
  Monitor on you hear the input through its chain.
- MIDI on the Performance channel goes to the active pad's instrument. A pad with an instrument can record that
  instrument as a take and then be triggered like any other pad.
- Audition isn't a separate mode: an empty active pad passes live MIDI / audio through its chain.
- Idle chains use no CPU: a stopped pad's chain runs until its effect tail dies away, then stops.
- Plugin latency: recordings are shifted by the input chain's latency; a pad plays its clip early by its own
  effects' latency, so it stays on the beat. You still hear yourself late, so the effects window warns.
- LV2 dynamic manifests are off (cmake/PatchJuce.cmake): with them on, JUCE loads every naspro-bridged LADSPA/DSSI
  plugin into Indigo at startup and a bad one crashed it. So bridged LADSPA/DSSI plugins aren't offered.

Quirks found in testing:
- Plugin editors that briefly report a 128x128 placeholder while being embedded (e.g. Black Pearl Drumkit) no longer
  open as a tiny window in the top-left corner.
- VST3 plugins scanned from a moduleinfo.json (e.g. the Vox Samples effects) have no channel counts; they are offered
  in the menus anyway. A yabridge plugin asking for a restart from its own thread no longer stops a debug build
  (assertion patched out in cmake/PatchJuce.cmake).
- Plugins with extra buses (multi-outs, sidechains) keep them switched on: AI_Bass_Mini (16 stereo outs, via yabridge)
  crashed Indigo when they were switched off. A VST3's main output is used; an LV2's outputs are all summed.

### Output mixer
Done 2026-10-24 (Mixer.h, MixerPanel.h; engine side in LooperEngine).
- A strip per pad: volume fader, pan, Mute, Solo, level meter (after the fader). Mute all / Unmute all; Unmute all
  also clears every solo.
- Recordings capture the signal before the strip, so moving a fader never changes a take.
- The live sound of the active pad goes through its strip too (like a DAW track), after its effects.
- Fader: cubic taper from silence to +6 dB, 0 dB at about 80 % travel; double-click resets. Pan is constant-power, so
  a centred pad doesn't drop in level. CC 0-127 maps straight onto the travel; pan CC 64 is centre.
- Mute keeps the pad running silently (a loop comes back in sync); it acts at once, not on the grid, with the 5 ms
  fade. A muted or soloed-out pad is dimmed on the grid with a MUTED tag.
- Metronome and preview are never affected by mute or solo. A muted pad still chokes the others in its group.
- Every control can be mapped to MIDI (saved with the MIDI presets), with soft takeover: a control only follows the
  knob once the knob passes its current value, so nothing jumps.
- Mixer settings are saved in sessions only, not templates. Clearing a pad keeps its strip; File > New Session resets
  the mixer.
- Normalise on load (checkbox on the panel, saved in ~/.config/Indigo/Mixer.xml): loaded files only, target -18 dBFS
  RMS over the audible parts (10 ms windows below -50 dB ignored), peaks under -1 dBFS, at most +24 dB of boost. A
  session remembers which pads were normalised and does it again when it's opened.

### Tabbed view panes
Done 2026-10-24 (Panes.h).
- A tabbed left pane (file browser) and bottom pane (mixer), so more views can be added to either later.
- Each has a hide button and a View menu toggle; hidden, it shrinks to a slim bar. The dividers can be dragged.
- Visibility, sizes, tabs and the window size are remembered in ~/.config/Indigo/Layout.xml. The window opens at
  1100 x 900 (or as tall as the screen allows).

### Outputs for a patchbay (JACK / PipeWire)
Done 2026-10-30. Outputs are routed in an external patchbay (qpwgraph, Carla, etc.), like Ardour's: one Indigo node
with a named port pair per source. No in-app routing and no ALSA multi-output: on ALSA everything stays on outputs 1/2.
- main, metronome and preview have their own ports, so the click and the cue can go to headphones only and never
  reach the main mix. Only main is connected at startup.
- Per-pad outputs (Settings > Audio, off by default, so the patchbay isn't cluttered): 16 extra pairs, pad_01_L /
  pad_01_R … pad_16_R, not connected at startup. Port names reach JACK through juce::jackOutputPortNames
  (cmake/PatchJuce.cmake).
- A copy, not a reroute: every pad still plays on main, whether or not anything is patched.
- Pre / Post fader, one setting for all (default Post), changes at once. Post is the same signal as the strip's
  meter; Pre is after the pad's effects, before fader, pan and mute/solo. The active pad's live sound goes to its
  output too.
- Ticking the box reopens the device: it restarts the clock, cancels a recording in progress, and may drop patchbay
  connections to Indigo. Settings in ~/.config/Indigo/Outputs.xml.

### MIDI sync out
Built 2026-11 (user testing continues; SyncOutput.h, Mtc.h, LooperEngine::sendSync; Settings > Sync, saved in
~/.config/Indigo/Sync.xml).
- A virtual MIDI port named "Indigo", created only while MIDI clock or MTC is on.
- MIDI clock: Start from bar 1, 24 clocks per beat, Stop.
- MTC: a Full Frame at 00:00:00:00 on bar 1, then quarter frames at 24, 25, 29.97 drop or 30 fps. It has its own
  sample count: transportPos is rescaled by a tempo change, and timecode has to keep real time.
- The port sends in queue order, so clock and quarter frames are merged by time in each block.
- No Song Position after Stop: Start already means "from the top", and a DAW that locates on both can trip over it.
- Sent from a thread of its own, timed for when the audio is heard (the output half of the latency), plus a
  sync offset (one for both).
- Hot-plug leaves the port alone: Indigo's own sync would only come straight back in.
