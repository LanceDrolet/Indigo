#pragma once
#include <JuceHeader.h>
#include <array>
#include <atomic>
#include <functional>
#include "Effects.h"
#include "OutputBuses.h"
#include "Mixer.h"

struct Clip
{
    juce::AudioBuffer<float> audio;
    double idealLength = 0.0;   // beat-locked loop period in samples (0 = not beat-locked)
    int length = 0;             // valid samples when shorter than the buffer (record buffers); 0 = whole buffer

    int numSamples() const noexcept
    {
        return length > 0 ? juce::jmin (length, audio.getNumSamples()) : audio.getNumSamples();
    }
};

// The MIDI control a pad (or the REC toggle) responds to. Packs into one int so the audio thread can
// read it from an atomic: control + channel * channelStride, where control is 0 = none,
// 1..128 = note 0..127, 129..256 = CC 0..127, and channel is 0 = any, 1..16.
struct MidiBinding
{
    enum class Kind { none, note, cc };
    static constexpr int channelStride = 257;

    Kind kind = Kind::none;
    int number = 0;                 // 0..127
    int channel = 0;                // 0 = any channel, 1..16

    static MidiBinding note (int n, int ch = 0) noexcept { return { Kind::note, juce::jlimit (0, 127, n), juce::jlimit (0, 16, ch) }; }
    static MidiBinding cc (int n, int ch = 0) noexcept   { return { Kind::cc,   juce::jlimit (0, 127, n), juce::jlimit (0, 16, ch) }; }

    int control() const noexcept
    {
        return kind == Kind::none ? 0 : (kind == Kind::note ? 1 : 129) + juce::jlimit (0, 127, number);
    }

    int encode() const noexcept
    {
        return kind == Kind::none ? 0 : control() + juce::jlimit (0, 16, channel) * channelStride;
    }

    static MidiBinding decode (int v) noexcept
    {
        const int c = v % channelStride, ch = v / channelStride;
        if (c >= 1 && c <= 128)   return note (c - 1, ch);
        if (c >= 129 && c <= 256) return cc (c - 129, ch);
        return {};
    }

    // Does an encoded binding respond to `control` (see control()) arriving on `channel` (1..16)?
    static bool matches (int code, int control, int channel) noexcept
    {
        const int ch = code / channelStride;
        return control != 0 && code % channelStride == control && (ch == 0 || ch == channel);
    }

    // Would some message trigger both? (Same control, and the channels are equal or either is "any".)
    bool overlaps (const MidiBinding& o) const noexcept
    {
        return kind != Kind::none && control() == o.control()
            && (channel == 0 || o.channel == 0 || channel == o.channel);
    }

    bool operator== (const MidiBinding& o) const noexcept { return encode() == o.encode(); }
    bool operator!= (const MidiBinding& o) const noexcept { return ! operator== (o); }

    juce::String describe() const
    {
        const auto ch = channel == 0 ? juce::String() : ", ch " + juce::String (channel);

        switch (kind)
        {
            case Kind::note: return "Note " + juce::String (number) + " (" + juce::MidiMessage::getMidiNoteName (number, true, true, 3) + ")" + ch;
            case Kind::cc:   return "CC " + juce::String (number) + ch;
            case Kind::none: break;
        }
        return "None";
    }
};

enum class PadMode   { oneShot, loop, gate };
enum class Quantize  { off, bar, beat, eighth, sixteenth };   // order matches the UI combo box
enum class PadStatus { idle, playing, queued, recQueued, recording, overdubQueued, overdubbing };
enum class MixerControl { volume, pan, mute, solo };     // a pad's mixer strip, as MIDI binding targets

//==============================================================================
// Real-time engine: master clock + 16 pads, with quantized recording and overdub.
//
// Master clock: a sample counter (transportPos) advanced once per audio block.
// Loop pads launch/stop on quantize grid lines; one-shot and gate pads fire immediately.
// Triggers are scheduled as "pending" actions at an absolute transport sample, and the
// render loop splits the block at that sample, so starts are sample-accurate. Stops fade out over a
// few milliseconds so they don't click.
//
// Choke groups: when a pad in a group is scheduled to start, the other sounding pads in that group are
// scheduled to stop on the same sample (so a loop swap lands on one grid line). The latest hit wins:
// cancelling the queued start also cancels the stops it caused. Pads recording or overdubbing are never choked.
//
// Recording (REC mode on, or a take/overdub already in progress on that pad):
//   - pad empty / stopped:   TAKE. Tap -> recording starts on the next grid line. Tap again -> it ends
//                            on a later grid line and the pad starts looping the take immediately.
//   - loop pad playing:      OVERDUB. Tap -> a layer starts summing the input into the loop on the
//                            next grid line. Tap again -> it ends on a later grid line.
//   Start/stop lines are never finer than one beat, so a take is always a whole number of beats.
//   While a take/overdub is in progress (and until its merge lands) the tempo is locked.
//
// Latency: the input that arrives at transport time T was played in response to what was heard
// roughly one round-trip earlier, so everything is shifted by recordLatency samples.
//
// Memory: the audio thread never allocates. The UI thread supplies pre-allocated, cleared record
// buffers (supplyRecordBuffer); finished work and retired clips flow back through an ordered
// event queue that the UI drains with pollEvents().
//
// Effects: the input, and each pad, can have a chain of effect plugins (FxChain). The input's own chain
// (monitorTarget) is recorded: takes and overdubs capture the input after it. A pad's chain is on its output: the
// pad's playback (loaded file or take) runs through it live, so it can be changed at any time, and recordings are
// dry. While Monitor is on you hear the input through the active pad (the last pad pressed): its effects and its
// mixer strip. When a pad stops sounding its chain keeps running until its output has been silent for a moment
// (reverb and delay tails), then idles and costs nothing. Recordings are shifted by the input chain's reported
// latency; a pad whose effects add latency plays its clip that much early (starts, stops and the end of a take
// are brought forward), so it stays on the beat.
// Chains are swapped in whole through the command queue; a chain prepared at a sample rate or block size the
// device is no longer running at is sent back (Event::fxStale) for the UI to rebuild.
//
// Instrument pads: a pad's chain can start with an instrument plugin. MIDI on the performance channel plays the
// active pad's instrument (a note-off goes to the pad that got the note-on, so switching pads mid-note can't hang
// it). The instrument then takes the input's place for that pad: you hear it through the pad's effects while the pad
// is active or recording (Monitor is only for the audio input), and takes / overdubs record it dry, before the
// effects. It needs no audio input. Its recordings are shifted by instrumentLatency (the output half of the round
// trip) plus the instrument's own.
//
// Mixer: each pad's playback goes through a strip (fader, constant-power pan, mute, solo) on its way to the main
// outputs, after the pad's effects. It is playback-only: recordings are taken before it. The live sound of the
// monitored input or an instrument goes through the active pad's strip too (like a DAW track). Mute keeps the pad running silently, so a muted loop stays in time. Changes ramp over
// fadeSeconds so they don't click. Solo silences every pad that isn't soloed (the metronome and preview are never
// affected). Volume and pan can follow a MIDI knob with soft takeover: after the UI sets a value, the knob has to
// pass through it before it takes over, so nothing jumps.
class LooperEngine : public juce::AudioSource,
                     public juce::MidiInputCallback,
                     public juce::AudioPlayHead
{
public:
    static constexpr int numPads = 16;
    static constexpr int beatsPerBar = 4;
    static constexpr double maxRecordSeconds = 60.0;     // size of each record buffer

    // Audio thread -> UI thread. Pointers are valid only inside the pollEvents handler.
    struct Event
    {
        enum class Type { retire,       // internal: a clip to delete (pollEvents handles it itself)
                          recorded,     // a take finished and is already looping; `clip` holds `length` samples
                          overdubbed,   // an overdub layer finished; `clip` is the layer, `length` samples
                          swapRejected, // a clip swap for `pad` was refused (recording/overdub busy); resend later
                          retireFx,     // internal: a replaced effects chain to delete
                          fxStale }     // the chain sent for `pad` (or monitorTarget) was prepared for another
                                        // rate / block size and was dropped: build it again and resend
            type = Type::retire;

        int pad = 0;
        Clip* clip = nullptr;
        FxChain* fx = nullptr;   // retireFx / fxStale (deleted by pollEvents after the handler)
        int length = 0;
        int beats = 0;           // recorded: beats spanned
        double ideal = 0.0;      // exact (fractional) loop period in samples
    };

    // MIDI binding / learn targets: the pads (0..numPads-1), the REC toggle, each pad's mixer controls, and the
    // mixer's Mute all / Unmute all buttons.
    static constexpr int recControl = numPads;
    static constexpr int numMixerControls = 4;
    static constexpr int mixerControlBase = recControl + 1;
    static constexpr int muteAllControl = mixerControlBase + numPads * numMixerControls;
    static constexpr int unmuteAllControl = muteAllControl + 1;
    static constexpr int numBindingTargets = unmuteAllControl + 1;

    static constexpr int mixerTarget (int pad, MixerControl c) noexcept
    {
        return mixerControlBase + pad * numMixerControls + (int) c;
    }

    // Volume and pan follow a knob or fader (any CC value); every other target is a button.
    static constexpr bool isContinuous (int target) noexcept
    {
        return target >= mixerControlBase && target < muteAllControl
            && (target - mixerControlBase) % numMixerControls <= (int) MixerControl::pan;
    }

    static constexpr float loopVelocity = 101.0f / 127.0f;  // loop pads ignore velocity and always start at this level
    static constexpr int numChokeGroups = 8;             // groups 1..8; 0 = no group
    static constexpr double fadeSeconds = 0.005;         // fade-out on every stop
    static constexpr int monitorTarget = numPads;        // effects target id of the input monitor
    static constexpr double fxIdleSeconds = 0.5;         // a stopped chain idles after this much silence...
    static constexpr double fxMaxTailSeconds = 30.0;     // ...or this long, for effects that never go quiet

    LooperEngine();
    ~LooperEngine() override;

    // AudioSource
    void prepareToPlay (int samplesPerBlockExpected, double sampleRate) override;
    void releaseResources() override {}
    void getNextAudioBlock (const juce::AudioSourceChannelInfo&) override;

    // MidiInputCallback (MIDI thread)
    void handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage& m) override
    {
        // The performance channel is for playing the active pad's instrument, not for Indigo's controls.
        if (m.getChannel() != 0 && m.getChannel() == performanceChannel.load())
        {
            perfCollector.addMessageToQueue (m);
            return;
        }

        // MIDI learn: the next note-on / CC press (any CC, for a knob target) is taken for the target (with its
        // channel) and swallowed.
        const int target = learnTarget.load();
        const bool learns = isContinuous (target) ? m.isController()
                                                  : m.isNoteOn() || (m.isController() && m.getControllerValue() >= 64);

        if (target >= 0 && learns)
        {
            const auto b = m.isNoteOn() ? MidiBinding::note (m.getNoteNumber(), m.getChannel())
                                        : MidiBinding::cc (m.getControllerNumber(), m.getChannel());

            // One control drives one thing: take it away from whatever else would also respond to it.
            for (int t = 0; t < numBindingTargets; ++t)
                if (t != target && getBinding (t).overlaps (b))
                    setBinding (t, {});

            setBinding (target, b);
            learnTarget.store (-1);
            return;
        }

        collector.addMessageToQueue (m);
    }

    //==== MIDI mapping (any thread) ====
    // target: see recControl. A knob target (isContinuous) only takes a CC.
    void setBinding (int target, MidiBinding b) noexcept
    {
        if (isContinuous (target) && b.kind == MidiBinding::Kind::note)
            b = {};

        if (juce::isPositiveAndBelow (target, numBindingTargets))
            bindings[(size_t) target].store (b.encode());
    }

    MidiBinding getBinding (int target) const noexcept
    {
        if (juce::isPositiveAndBelow (target, numBindingTargets))
            return MidiBinding::decode (bindings[(size_t) target].load());
        return {};
    }

    void startMidiLearn (int target) noexcept        { learnTarget.store (target); }
    void cancelMidiLearn() noexcept                  { learnTarget.store (-1); }
    int  getLearnTarget() const noexcept             { return learnTarget.load(); }     // -1 = not learning

    // MIDI on this channel (1..16; 0 = none) plays the active pad's instrument. It never reaches the pad / REC
    // controls or MIDI learn.
    void setPerformanceChannel (int ch) noexcept     { performanceChannel.store (juce::jlimit (0, 16, ch)); }
    int  getPerformanceChannel() const noexcept      { return performanceChannel.load(); }

    //==== Message-thread API ====
    // keepPhase: swap a re-stretched loop in without stopping it (keeps its place in the bar).
    // replacesLayer: this is the merge of a finished overdub, so it takes over from the playing layer.
    // While a take or overdub depends on the pad's clip, other swaps are refused (Event::swapRejected).
    bool assignClip (int pad, std::unique_ptr<Clip> clip, bool keepPhase = false, bool replacesLayer = false);
    void triggerPad (int pad, float velocity = 1.0f);
    void releasePad (int pad);
    void setPadMode (int pad, PadMode mode);
    void setChokeGroup (int pad, int group);    // 0 = none
    void clearPad (int pad);       // stop it, drop its clip, cancel any take/overdub on it
    void setTempo (double bpm);
    void setRecordMode (bool on);

    // Replaces the effects of a pad (or monitorTarget). Every plugin in the chain must already be prepared
    // at getSampleRate() / getBlockSize(); see getFxPrepareLock(). An empty chain (or nullptr) removes them.
    bool setEffects (int target, std::unique_ptr<FxChain> chain);

    // Held while plugins are prepared, by the UI (new plugins) and by prepareToPlay (the running chains),
    // so a device change can never prepare a plugin twice at once.
    std::mutex& getFxPrepareLock() noexcept          { return fxPrepareLock; }
    int getBlockSize() const noexcept                { return blockSize.load(); }

    // AudioPlayHead, for tempo-synced plugins (audio thread)
    juce::Optional<PositionInfo> getPosition() const override;

    // Delivers recorded/overdubbed events to `handler`, in order, and frees retired clips and chains.
    void pollEvents (const std::function<void (const Event&)>& handler);

    // The UI keeps a couple of cleared 2-channel buffers of maxRecordSeconds queued for the engine.
    bool supplyRecordBuffer (std::unique_ptr<Clip> buffer);
    int  getNumRecordBuffers() const noexcept        { return poolFifo.getNumReady(); }

    void setQuantize (Quantize q) noexcept           { quantize.store ((int) q); }
    void setMetronome (bool on) noexcept             { metronomeOn.store (on); }
    void setMonitor (bool on) noexcept               { monitorOn.store (on); }     // pass the input through

    // Round-trip latency (output + input) that recordings are shifted by.
    void setRecordLatency (int samples) noexcept     { recordLatency.store (juce::jmax (0, samples)); }
    int  getRecordLatency() const noexcept           { return recordLatency.load(); }
    // What instrument recordings are shifted by: from rendering a block to hearing it.
    void setInstrumentLatency (int samples) noexcept { instrumentLatency.store (juce::jmax (0, samples)); }
    void setNumInputChannels (int n) noexcept        { numInputChannels.store (juce::jlimit (0, 2, n)); }
    int  getNumInputChannels() const noexcept        { return numInputChannels.load(); }

    PadStatus getPadStatus (int pad) const noexcept  { return (PadStatus) status[(size_t) pad].load(); }
    bool isRecordMode() const noexcept               { return recordMode.load(); }
    bool isRecordingBusy() const noexcept            { return recBusy.load(); }   // tempo is locked while true
    double getTempo() const noexcept                 { return bpm.load(); }
    double getBeatInBar() const noexcept             { return beatInBar.load(); }   // 0 .. beatsPerBar
    double getSampleRate() const noexcept            { return sampleRate.load(); }

    //==== Mixer (any thread) ====
    // Fader positions are 0..1 (Mixer::faderToGain), pan -1 (left) .. 1 (right). Setting a value here (the UI does)
    // makes a MIDI knob on that control pick it up again before it moves it.
    void setFader (int pad, float pos) noexcept;
    void setPan (int pad, float pan) noexcept;
    void setMute (int pad, bool on) noexcept         { strips[(size_t) pad].mute.store (on); }
    void setSolo (int pad, bool on) noexcept         { strips[(size_t) pad].solo.store (on); }
    void muteAll() noexcept;
    void unmuteAll() noexcept;                       // also clears every solo
    void resetMixer() noexcept;                      // every strip to unity, centre, unmuted, not soloed

    float getFader (int pad) const noexcept          { return strips[(size_t) pad].fader.load(); }
    float getPan (int pad) const noexcept            { return strips[(size_t) pad].pan.load(); }
    bool isMuted (int pad) const noexcept            { return strips[(size_t) pad].mute.load(); }
    bool isSoloed (int pad) const noexcept           { return strips[(size_t) pad].solo.load(); }
    bool isAnySoloed() const noexcept;

    // The pad's highest output level (after its strip) on `channel` since the last call (UI meters).
    float takePeak (int pad, int channel) noexcept   { return strips[(size_t) pad].peak[channel & 1].exchange (0.0f); }

    // The last pad pressed, clicked or played from MIDI (-1 = none yet). Monitored input goes through its effects,
    // and the performance channel plays its instrument.
    int getActivePad() const noexcept                { return activePad.load (std::memory_order_relaxed); }

private:
    enum class Pending { none, start, stop, recFinal };

    struct Command
    {
        enum class Type { assignClip, trigger, release, setMode, setChoke, setTempo, setRecordMode, clear, setFx } type = Type::trigger;
        int pad = 0;
        int group = 0;
        Clip* clip = nullptr;
        FxChain* fx = nullptr;
        PadMode mode = PadMode::oneShot;
        float velocity = 1.0f;
        double value = 0.0;
        bool keepPhase = false;
        bool replacesLayer = false;
    };

    // An effects chain and whether it is running (audio thread).
    struct FxRun
    {
        FxChain* chain = nullptr;
        bool running = false;
        int silent = 0, tail = 0;       // samples of silence / of tail since it stopped being fed
    };

    struct PadState
    {
        FxRun fx;
        bool sounded = false;            // went through its strip last block (so gain changes ramp)

        Clip* clip = nullptr;
        PadMode mode = PadMode::oneShot;
        bool active = false;
        int pos = 0;
        float gain = 1.0f;
        int fadeLeft = 0, fadeLen = 0;   // fading out: active until fadeLeft reaches 0

        Pending pending = Pending::none;
        juce::int64 pendingAt = 0;       // absolute transport sample
        float pendingGain = 1.0f;

        int chokeGroup = 0;
        int chokedBy = -1;               // pad whose queued start caused this pending stop, -1 if none

        bool playing() const noexcept    { return active && fadeLeft == 0; }

        juce::int64 startAt = 0;         // transport sample where the current pass started (pos == 0)
        double lagDebt = 0.0;            // fractional-sample drift of a beat-locked loop

        // Take (first recording). Input arriving at transport time T lands at take index T - L - recStartAt.
        bool recActive = false;
        bool recPromoted = false;        // the take has become the pad's clip and is playing
        Clip* recClip = nullptr;
        juce::int64 recStartAt = 0, recStartK = 0;
        juce::int64 recStopAt = -1;      // -1 until a stop is scheduled
        juce::int64 recAutoAt = 0, recAutoK = 0;   // forced stop when the buffer would overflow
        double recGrid = 0.0, recIdeal = 0.0;
        int recBeatsPerLine = 1, recBeats = 0, recLen = 0;

        // Overdub. Input arriving at T is summed into layer index (loop position that was heard at T - L).
        bool odActive = false;
        bool mergePending = false;       // layer finished, waiting for the UI to hand back the merged clip
        Clip* layer = nullptr;
        juce::int64 odStartAt = 0, odStartK = 0;
        juce::int64 odStopAt = -1;
        double odGrid = 0.0;
    };

    bool push (const Command&);
    void apply (const Command&);
    void padPressed (PadState&, float velocity, juce::int64 when);
    void requestTrigger (PadState&, float velocity, juce::int64 when);
    void releaseNow (PadState&);
    void cancelPending (PadState&);
    void chokeOthers (PadState&);
    void releaseChokes (int by);
    void fadeOut (PadState&);
    int  indexOf (const PadState& p) const noexcept  { return (int) (&p - pads.data()); }
    void toggleRecordMode() noexcept;
    bool anyRecordingBusy() const noexcept;

    void recordPress (PadState&, juce::int64 when);
    void armTake (PadState&, juce::int64 when);
    void armOverdub (PadState&, juce::int64 when);
    void scheduleRecStop (PadState&, juce::int64 stopK);
    void promoteTake (PadState&, juce::int64 at);
    void cancelRecording (PadState&);
    void cancelOverdub (PadState&);

    static bool hasInstrument (const PadState& p) noexcept { return p.fx.chain != nullptr && p.fx.chain->instrument; }
    int  inputLatency (const PadState&) const noexcept;
    static int playbackLead (const PadState& p) noexcept { return p.fx.chain != nullptr ? p.fx.chain->effectsLatency : 0; }
    void routePerformanceMidi (int n);
    void captureTake (PadState&, const juce::AudioBuffer<float>* input, int n, juce::int64 blockStart);
    void captureOverdub (PadState&, const juce::AudioBuffer<float>* input, int n, juce::int64 blockStart);
    void finishRecording (int index, PadState&, juce::int64 blockEnd);
    void renderPad (PadState&, juce::AudioBuffer<float>&, int destStart, int count);
    void renderMetronome (juce::AudioBuffer<float>&, int destStart, int count, juce::int64 blockStart);
    bool applyKnob (int cc, int value, int channel) noexcept;
    bool pressMixerButton (int control, int channel) noexcept;
    void mixStrip (int index, bool wasSounding, bool sounded, bool anySolo, juce::AudioBuffer<float>& out, int destStart, int n);
    bool runPadEffects (PadState&, bool playing, bool live, int n, const juce::MidiBuffer& notes);

    void retire (Clip*);
    bool postEvent (const Event&);
    Clip* popRecordBuffer();

    double samplesPerBeat() const noexcept   { return sampleRate.load() * 60.0 / audioBpm; }
    double gridSamples() const noexcept;
    double recGridSamples() const noexcept   { return juce::jmax (gridSamples(), samplesPerBeat()); }
    juce::int64 quantizeTime (juce::int64 t) const noexcept;
    static juce::int64 lineAtOrAfter (juce::int64 t, double grid, juce::int64& k) noexcept;

    juce::AbstractFifo cmdFifo { 256 };
    std::array<Command, 256> cmds;

    juce::AbstractFifo evFifo { 128 };
    std::array<Event, 128> events;

    juce::AbstractFifo poolFifo { 8 };
    std::array<Clip*, 8> pool {};

    juce::MidiMessageCollector collector;
    juce::MidiBuffer midiBuffer;
    juce::MidiMessageCollector perfCollector;    // the performance channel
    juce::MidiBuffer perfMidi;
    std::array<juce::MidiBuffer, numPads> padMidi;   // this block's notes for each pad's instrument
    juce::AudioBuffer<float> inputScratch;       // this block's input, copied before the buffer is cleared
    bool haveInput = false;
    juce::AudioBuffer<float> fxScratch;          // an instrument's output, before the pad's effects
    FxRun monitorFx;                             // the input's own effects (only `chain` is used)
    juce::AudioBuffer<float> padOut;             // one pad's playback on its way through its effects and mixer strip
    std::mutex fxPrepareLock;

    std::array<PadState, numPads> pads;

    // Audio-thread-only state
    juce::int64 transportPos = 0;
    double audioBpm = 120.0;
    double clickPhase = 0.0, clickFreq = 1000.0;
    int clickPos = 0, clickLen = 0;
    std::array<std::array<bool, 128>, 16> ccHigh {};   // [channel][cc] last state (>= 64), for press/release edges
    std::array<std::array<int, 128>, 16> ccLast {};    // [channel][cc] last value, to spot buttons that never send a release
    std::array<std::array<juce::int8, 128>, 16> noteOwner;   // [channel][note] pad whose instrument got the note-on, -1 = none
    std::array<juce::int8, 16> sustainOwner;                  // [channel] pad holding the sustain pedal down, -1 = none

    // Mixer strips: the gains being ramped to, and soft takeover for each pad's volume and pan knob ([pad * 2 + 0/1]).
    struct StripRamp { juce::SmoothedValue<float> left, right; };
    std::array<StripRamp, numPads> stripRamps;

    struct Takeover
    {
        int seenGen = -1;               // the value's generation when the knob last moved (see Strip)
        bool engaged = false;           // the knob has reached the value and now moves it
        int lastValue = -1;             // the knob's previous CC value, to spot it passing the value
    };
    std::array<Takeover, numPads * 2> takeovers;

    // Shared with UI / MIDI thread
    std::array<std::atomic<int>, numPads> status {};
    std::atomic<double> sampleRate { 44100.0 };
    std::atomic<int> blockSize { 512 };
    std::atomic<double> bpm { 120.0 };
    std::atomic<double> beatInBar { 0.0 };
    std::atomic<int> quantize { (int) Quantize::bar };
    std::atomic<bool> metronomeOn { false };
    std::atomic<bool> monitorOn { false };
    std::atomic<bool> recordMode { false };      // written by the audio thread only
    std::atomic<bool> recBusy { false };
    std::atomic<int> recordLatency { 0 };
    std::atomic<int> instrumentLatency { 0 };
    std::atomic<int> numInputChannels { 2 };
    std::array<std::atomic<int>, numBindingTargets> bindings {};   // MidiBinding::encode()

    struct Strip
    {
        std::atomic<float> fader { Mixer::unityFader() }, pan { 0.0f };
        std::atomic<bool> mute { false }, solo { false };
        std::atomic<int> faderGen { 0 }, panGen { 0 };     // bumped when the UI sets the value (soft takeover)
        std::atomic<float> peak[2] {};
    };
    std::array<Strip, numPads> strips;
    std::atomic<int> learnTarget { -1 };
    std::atomic<int> performanceChannel { 0 };
    std::atomic<int> activePad { -1 };
};
