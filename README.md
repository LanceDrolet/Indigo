# Indigo

Indigo is a 16-pad live looper and sampler in the spirit of the MPC. Load samples onto pads, record loops on the fly,
play plugin instruments from a MIDI controller, and mix it all on a per-pad mixer. It runs on Linux (ALSA or JACK)
and is built with JUCE 8.

## The window

- **Pads** (centre): a 4x4 grid. Click a pad, or hit its mapped MIDI note or CC, to play it.
- **Transport bar** (top): tempo, quantize, metronome, REC, Monitor and the input's FX, and recording latency.
- **Browser** (left pane): your audio files. Double-click a file to preview it, or tick auto-preview to hear each
  file as you select it. Drag a file onto a pad to load it (you can also drop files from your desktop).
- **Mixer** (bottom pane): one strip per pad.

Both side panes can be hidden with the **–** button on their tab bar (a slim bar brings them back), or from the
**View** menu. Drag the dividers to resize them. The layout and window size are remembered.

## Playing pads

Right-click a pad to set how it plays:

| Mode | What a press does |
|---|---|
| **One-shot** | Plays the whole sound at once, at the velocity you hit it. Hitting it again restarts it. |
| **Loop (quantized)** | Starts on the next grid line and loops; press again to stop on a grid line. A second press while it's waiting cancels it. |
| **Gate (hold)** | Plays while the pad is held and stops when you let go. |

**Quantize** sets the grid that loops start and stop on (1 bar, 1 beat, 1/8, 1/16, or off). The **metronome**
clicks on the beat.

Also in the pad's right-click menu:
- **Loop length:** how many beats the sound spans. Loops are stretched to the project tempo without changing pitch;
  *Auto* works it out from the file.
- **Choke group (1–8):** starting a pad stops the other pads in its group on the same sample, so loop swaps land
  together.
- **Instrument & Effects...:** see below.
- **Clear:** empties the pad.

## Recording

1. Turn on **REC**.
2. Tap an **empty** pad: recording starts on the next grid line. Tap it again: recording stops on a grid line and the
   pad starts looping what you played. Takes are always a whole number of beats.
3. Tap a **playing loop** to **overdub**: your input is layered onto the loop, starting and stopping on grid lines.

**Monitor** lets you hear your input through Indigo. Use headphones, because speakers and a mic will feed back. If
recordings land early or late against the beat, adjust **Rec latency** (the round-trip delay of your audio
interface).

## Plugins: effects and instruments

Indigo hosts **VST3** and **LV2** plugins. Windows VST3s work through yabridge, though some editors misbehave under
Wine.

- **Edit > Plugins...** scans your plugin folders. Scanning runs in a separate process, so a crashing plugin can't
  take Indigo down. Plugins can be enabled or disabled there.
- **Pad right-click > Instrument & Effects...** gives a pad an instrument and a chain of effects. Open a plugin's
  own editor from that window.
- **FX** (next to Monitor) is an effects chain on the input itself.
- **A pad's effects are live.** The pad's sound (a loaded file or a take) plays through its effects, so you can
  change them at any time, even on a take you've already recorded. Recordings are dry: only the input's own **FX**
  chain is recorded. With Monitor on you hear your input through the active pad's effects.
- **Instrument pads:** the **active pad** (the last one pressed, outlined in blue) receives the notes you play on the
  **performance channel** (set in Settings > MIDI Mapping). Record what you play as a take, just like audio; the
  take is the instrument's dry sound, and the pad's effects are applied as it plays. An empty active pad lets you
  play its instrument and effects before recording.
- Plugins that add delay (e.g. lookahead limiters) are compensated: the input chain's delay is taken out of
  recordings, and a pad whose effects add delay plays early so it stays on the beat. The effects window shows how
  much delay they add.

## Mixer

Each pad has a **volume fader**, **pan** knob, **level meter**, **M** (mute) and **S** (solo).

- Faders and pan shape playback only; recordings are captured before them. Double-click resets a fader to 0 dB or
  the pan to centre.
- What you play live (the monitored input or an instrument) goes through the active pad's strip, like a track in a
  DAW, so muting that pad silences it too.
- Muting a pad keeps it running silently, so a loop stays in time and comes back in sync. Muted pads are dimmed and
  tagged MUTED on the grid.
- Any number of pads can be soloed. **Unmute all** clears every mute and solo; **Mute all** mutes everything.
- **Normalise on load** evens out the level of files you load (it doesn't touch recordings).
- The metronome and previews never go through the mixer.
- Right-click any mixer control for **MIDI Learn**. Knobs use soft takeover: a control only follows your knob once
  the knob reaches its current value, so nothing jumps.

## MIDI and settings

**Edit > Settings...** has three tabs:
- **MIDI Mapping:** a note or CC (and channel) for each pad, REC, and every mixer control, with Learn buttons. Save
  mappings as named presets. Controllers plugged in while Indigo runs are picked up automatically.
- **Audio:** ALSA or JACK, device, sample rate, buffer size and channels. Under JACK, the main mix, metronome and
  preview have their own outputs, so you can route the click to your headphones. **Per-pad outputs** (JACK only) adds
  an output pair for each pad, pre- or post-fader, to record a performance into a DAW with the pads on separate tracks.
- **Browser:** the folder the browser starts in.

## Sessions and templates

- **Sessions** (File > Save Session) keep everything: tempo, quantize, each pad's sound, mode and settings, plugins
  and their settings, and the mixer. Recorded takes are saved as WAV files next to the `.indigo` file. Indigo asks
  before closing if you have unsaved takes.
- **Templates** keep a pad setup (modes, choke groups, plugins) without any sounds. Indigo restores your last setup
  when it starts.

## Download

Prebuilt Linux (x86-64) binaries are on the [Releases](https://github.com/LanceDrolet/Indigo/releases) page. They
run on distros with glibc 2.35 or newer (Ubuntu 22.04, Debian 12, Fedora 36 and later). Unpack and run:

```sh
tar xzf Indigo-*-linux-x86_64.tar.gz
./Indigo-*-linux-x86_64/Indigo
```

You need ALSA, freetype and fontconfig, which almost every desktop already has. JACK is optional.

## Feedback and bugs

Indigo is early software. Please report bugs and ideas on the
[issue tracker](https://github.com/LanceDrolet/Indigo/issues). For a bug, say what you did, what happened, your distro,
your audio setup (ALSA, JACK or PipeWire, and the interface), and the plugin involved if there is one.

## Building

Requires CMake 3.24+, a C++17 compiler, and the usual JUCE Linux dependencies (ALSA, JACK, X11, freetype, etc.).
JUCE and signalsmith-stretch are downloaded by CMake.

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/Indigo_artefacts/Release/Indigo
```

See [ROADMAP.md](ROADMAP.md) for what's done and what's planned.
