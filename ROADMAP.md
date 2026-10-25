# Indigo roadmap

## Planned

### Headphone (cue) output
A second stereo output for the performer only, so the metronome and file previews go to the
headphones and never reach the main mix.

- Settings: choose the main output pair and the headphone pair (e.g. outputs 1/2 and 3/4 on a
  multi-output interface).
- Per-source routing: metronome and preview default to headphones only; pads default to main.
  Consider a "pads to headphones too" option so the performer hears the mix in their cans.
- Input monitoring (the Monitor toggle) should probably follow the headphone output as well.
- Needs an interface with 4+ outputs. With only 2, fall back to today's behaviour (everything on 1/2).
- Settings > Audio currently limits outputs to one stereo pair (AudioSettingsPanel in SettingsWindow.h);
  raise the selector's max outputs to 4 for this.
- Implementation note: open the device with 4 output channels and have the engine render the
  cue sources into channels 3/4 instead of 1/2; the preview mixer input moves to the cue bus.
- Partly done: under JACK, Indigo already has separate named outputs (main, metronome, preview; see
  OutputBuses.h) that can be connected to the headphones in a patchbay. What's left is the ALSA /
  multi-output-interface case above.

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

### Per-pad outputs (JACK / PipeWire)
One stereo output per pad, so each pad can be patched to its own track in a DAW (e.g. Ardour) to capture a
performance with the pads kept separate for editing and mixing afterwards.

Decided 2026-10-25:
- 16 extra stereo ports, pad_01_L / pad_01_R … pad_16_L / pad_16_R, alongside main, metronome and preview.
  Not connected at startup.
- A copy, not a reroute: every pad still plays on main as now, so what you hear doesn't change whether
  or not anything is patched, and the main mix can be recorded alongside the pads.
- Pre / Post fader, one setting for all pad outputs (default Post):
  - Post: the pad after its effects, fader, pan and mute/solo, the same signal as the mixer strip's meter, so
    mutes and fader moves during the performance are captured.
  - Pre: the pad after its effects, before the fader, pan and mute/solo, for raw tracks to mix afterwards.
- The live sound of the active pad (monitored input or instrument through its chain) goes to that pad's output
  too, since it already goes through that pad's strip.
- Metronome and preview are unchanged and never go to a pad output.
- The whole feature is a setting (Settings > Audio, "Per-pad outputs", off by default) so the patchbay isn't
  cluttered with 32 unused ports; switching it on or off reopens the device. Pre / Post changes at once.
- JACK only, like the existing buses. On ALSA the setting is hidden.

Implementation notes:
- OutputBuses.h: numChannels becomes 6 + 32 when per-pad outputs are on; port names via
  juce::jackOutputPortNames as now (cmake/PatchJuce.cmake).
- LooperEngine: copy each pad's render buffer to its pair (before or after its strip) as well as summing into main.

## Ideas / smaller items
- MIDI sync with a DAW (to explore): send MIDI clock and/or follow JACK transport so Indigo's tempo and grid
  line up with Ardour's timeline when recording a performance through the per-pad outputs.
- Plugin latency changed live (e.g. a lookahead setting in the plugin's editor) is only picked up when the
  chain is next sent (add / remove / move an effect); follow it as it changes.
- Sessions: option to reopen the last session at startup; Ctrl+S / Ctrl+O shortcuts.
- Preview: volume control; remember the auto-preview checkbox between sessions.
- Windows build: in CMakeLists.txt make JUCE_JACK=1 Linux-only and add JUCE_ASIO=1 for WIN32 with a
  cache variable for the Steinberg ASIO SDK path (SDK downloaded separately; GPLv3 or Steinberg licence).
  WASAPI needs nothing. Check MSVC warnings. Note: classic WinMM MIDI is single-client, so a controller
  held by a DAW won't reach Indigo.
- Don't sort plugins by manufacturer name. Sort by type (distortion, reverb, etc...)

## Done
- Pad grid with one-shot / loop / gate modes; quantized master clock, metronome
- Pitch-preserving loop stretch to the project tempo
- Live recording and overdub with latency compensation; input monitoring
- Pad Clear (right-click menu)
- Menu bar; Settings window with per-pad MIDI note/CC mapping, MIDI learn, named presets
- File browser preview (double-click, or auto-preview on selection)
- File browser navigation (Back / Up / Home / Browse..., typed path) and a starting folder (Settings > Browser, or right-click a folder), or reopen the last folder
- Settings > Audio tab (ALSA or JACK; device, sample rate, buffer size, channels), remembered between runs; a device
  change re-reads the input channels and latency and resamples loaded pads to a new rate
- Pad modes restored at startup; templates (pad modes, *.indigotemplate) and sessions (tempo, quantize,
  modes, lengths, sounds; takes saved as WAVs beside the *.indigo file); save prompt for unsaved takes
- Choke groups 1–8 (right-click > Choke group; coloured tag on the pad; saved in templates and sessions): starting a
  pad stops the others in its group on the same sample, so loop swaps land on one grid line. All stops fade
  out over 5 ms.
- MIDI channels: each pad / REC mapping has a channel (Any or 1-16; Learn picks up the controller's channel),
  so two controllers sending the same notes can drive different pads. Settings > MIDI Mapping has a
  Performance channel that the controls ignore (saved with the preset). Old presets load as "Any".
- MIDI hot-plug: a controller plugged in while Indigo is running is enabled automatically (2026-10-23).

### VST3 / LV2 plugins and MIDI pads
Done 2026-10-24 (user testing continues).

VST3 and LV2 only: Steinberg no longer licenses the VST2 SDK. Hosting comes from JUCE's juce_audio_processors
(JUCE_PLUGINHOST_VST3=1, JUCE_PLUGINHOST_LV2=1).

Decided:
- Scan folders: a set of directories Indigo scans for effects and instruments. Scan in a child process so a
  crashing plugin can't take Indigo down.
- Plugin manager window: the scanned list, with each plugin enabled or disabled.
- Per-pad chain: a pop-up for each pad to pick and re-order its instrument and effects.
- The input monitor gets a chain too.
- Active pad: exactly one at a time, the last pad pressed, clicked, hit, or triggered by a MIDI note.
- Performance channel (see MIDI channels above): MIDI on it is ignored by Indigo's controls; it is routed to the
  active pad's instrument.
- MIDI pad: a pad with an instrument assigned. Its output can be recorded as a take and then triggered like
  any other pad (the instrument is the pad's record source).
- Audition mode: not a separate mode, just the active pad when it has no file or recording. Live MIDI / audio
  goes through its chain so you can hear it before recording.
- LV2 dynamic manifests are switched off (cmake/PatchJuce.cmake): with them on, JUCE loads every naspro-bridged
  LADSPA/DSSI plugin into Indigo at startup and a bad one crashed it. So bridged LADSPA/DSSI plugins aren't offered.
- Missing plugin on session load: it is removed from the pad (tell the user which ones).
- Idle chains use no CPU: a stopped pad's chain runs until its effect tail dies away, then stops processing.
- Plugins that add latency (lookahead etc.): recordings are compensated for the input chain, and a pad plays early
  by its own effects' latency; you still hear yourself late, so the effects window warns.

Built in this order:
1. Scanning + plugin manager window. (Done 2026-10-19, Edit > Plugins; user confirmed VST3 + LV2 scans incl. LV2 instruments.)
2. Per-pad and monitor effect chains (per-pad render buffers; load/unload via the command queue; tails;
   saved in sessions with each plugin's state). (Built 2026-10-19: pad right-click > Effects, FX button by
   Monitor; chains saved in templates incl. the startup one, and in sessions; plugins get Indigo's tempo via a
   play head; latency shown in the effects window.)
   Changed 2026-10-19 at the user's request: effects are RECORDED. A pad's chain shapes the input going into
   that pad (input chain first, then the pad's), takes/overdubs capture the wet result, and loops play back as
   recorded (so loaded samples no longer go through pad effects). With Monitor on you hear the input through
   the active pad's chain (last pad pressed; blue outline). Recordings are shifted by the chains' latency.
   Use case: chorus pad for rhythm guitar, then a distortion pad for the lead over it.
   Changed again 2026-10-24: a pad's chain is on its OUTPUT. The pad's playback (loaded file or take) runs
   through it live, so effects can be changed at any time, and recordings are dry; only the input's own chain is
   recorded. Instrument pads record the instrument dry, before their effects. Recordings are shifted by the input
   chain's latency; a pad whose effects add latency plays its clip that much early, so it stays on the beat.
3. Active pad, instrument pads, performance-channel routing, audition mode. (Instrument pads and
   performance-channel routing are in the engine; the user tested a VSTi 2026-10-23 and it works well.
   Audition mode isn't a separate feature: an empty pad that is the active pad is auditioning; confirmed
   working by the user 2026-10-23.)
4. Latency warning. (The effects window shows each plugin's
   added delay and warns with the total; a pad without an instrument counts the input chain too; LV2 latency now read correctly, so
   recordings are compensated for it. Confirmed by the user 2026-10-23.)

Fixes after testing:
- Plugin editors that briefly report a 128x128 placeholder while being embedded (e.g. Black Pearl Drumkit) no longer
  open as a tiny window in the top-left corner (2026-10-24).
- VST3 plugins scanned from a moduleinfo.json (e.g. the Vox Samples effects) have no channel counts; they are now
  offered in the effect / instrument menus instead of being hidden. A yabridge plugin asking for a restart from its own
  thread no longer stops a debug build (assertion patched out in cmake/PatchJuce.cmake) (2026-10-24).
- Plugins with extra buses (multi-outs, sidechains) keep them switched on: AI_Bass_Mini (16 stereo outs, via yabridge)
  crashed Indigo when they were switched off. A VST3's main output is used; an LV2's outputs are all summed, as before
  (2026-10-24).

### Output Mixer
Samples and loops are rarely normalised to the same level, so each pad needs its own output controls.

Done 2026-10-24 (user testing continues). Decided 2026-10-23:
- A strip per pad (16): volume fader, pan, Mute, Solo, level meter.
- Volume and pan are playback-only: recordings capture the signal before them, so moving a fader never
  changes a take.
- Pan is constant-power, so a centred pad doesn't drop in level.
- Mute keeps the pad running silently (a loop stays in time and comes back in sync); it acts at once, not on
  the grid, with the 5 ms fade so it doesn't click.
- Any number of pads can be soloed at once.
- Mute all / Unmute all buttons. Unmute all also clears every solo (one press back to normal). Provisional:
  may be reworked later.
- Metronome and preview are never affected by mute or solo.
- Choke groups are independent of mute: a muted pad still chokes the others in its group.
- Level meters show each pad after its fader.
- Every mixer control can be mapped to MIDI CC (in Settings, plus Learn on a right-click of the control), with
  soft takeover: a control only follows the knob once the knob passes its current value, so nothing jumps.
- Mixer settings (levels, pan, mute, solo) are saved in sessions only, not templates.
- Normalise on load (a setting, on/off): loaded files are matched by RMS level with a peak limit so nothing
  clips. Loaded files only; recorded takes are left alone.
- Placement: in a tabbed, hideable panel along the bottom of the window, below the pads and the file
  browser (see "Tabbed view panes").

Built 2026-10-23 (Mixer.h, MixerPanel.h; engine side in LooperEngine). Engine tested with a console harness (strip gain/pan, mute ramp and loop timing, solo, Mute/Unmute all, chokes,
soft takeover, MIDI learn, recording before the strip, session and preset round trips). Details settled while
building:
- Fader: cubic taper from silence to +6 dB, 0 dB at about 80 % travel; double-click resets to 0 dB (pan: centre).
  CC 0-127 maps straight onto the fader travel; pan CC 64 is centre.
- Only clip playback goes through the strip. The live sound of the monitored input or an instrument through a pad's
  chain does not (so mute / solo never cut what you're playing). Revisit if instrument pads should follow their fader.
  Changed 2026-10-24: the live sound now goes through the active pad's strip too (like a DAW track), after its
  effects, so the fader, pan, mute and solo apply to it.
- A pad that is muted or soloed out is dimmed on the grid with a MUTED tag.
- Mixer MIDI mappings are saved with the MIDI presets; Settings > MIDI Mapping lists them below the pads (volume and
  pan only take CCs). Right-click any mixer control: MIDI Learn / Clear.
- Normalise on load: target -18 dBFS RMS over the audible parts (10 ms windows below -50 dB ignored), peaks kept
  under -1 dBFS, at most +24 dB of boost. The checkbox is on the mixer panel (saved in ~/.config/Indigo/Mixer.xml).
  A session remembers which pads were normalised and does it again when it's opened.
- Clearing a pad keeps its strip settings; File > New Session resets the mixer.

### Tabbed view panes
Done 2026-10-24 (user testing continues).

- The left pane (file browser today) and a new bottom pane (Output Mixer first) are both tabbed, so more
  views can be added to either later.
- Each pane can be hidden or minimised to give the pad grid more room.
- The file browser stays on the left; it does not move to the bottom.

Built 2026-10-23 (Panes.h): each pane has a hide button (top right of its tab
bar) and a View menu toggle; hidden, it shrinks to a slim bar whose button brings it back. The dividers between the
panes and the pads can be dragged. Pane visibility, sizes, tabs and the window size are remembered in
~/.config/Indigo/Layout.xml. The window now opens at 1100 x 900 (or as tall as the screen allows).
