#include "LooperEngine.h"

LooperEngine::LooperEngine()
{
    // Default layout: pads on consecutive notes from C1 (36), the usual drum-pad range.
    for (int i = 0; i < numPads; ++i)
        setBinding (i, MidiBinding::note (36 + i));

    for (auto& ch : noteOwner)
        ch.fill (-1);
    sustainOwner.fill (-1);

    perfMidi.ensureSize (4096);
    for (auto& b : padMidi)
        b.ensureSize (4096);

    for (auto& r : stripRamps)
        for (auto* v : { &r.left, &r.right })
        {
            v->reset (44100.0, fadeSeconds);
            v->setCurrentAndTargetValue (1.0f);     // unity, centre
        }
}

LooperEngine::~LooperEngine()
{
    for (auto& p : pads)
    {
        if (p.recClip != nullptr && p.recClip != p.clip)
            delete p.recClip;

        delete p.clip;
        delete p.layer;
        delete p.fx.chain;
    }

    delete monitorFx.chain;

    // Commands the audio thread never got to
    int c1, m1, c2, m2;
    cmdFifo.prepareToRead (cmdFifo.getNumReady(), c1, m1, c2, m2);
    for (auto [start, count] : { std::pair { c1, m1 }, std::pair { c2, m2 } })
        for (int i = 0; i < count; ++i)
        {
            delete cmds[(size_t) (start + i)].clip;
            delete cmds[(size_t) (start + i)].fx;
        }
    cmdFifo.finishedRead (m1 + m2);

    pollEvents ({});     // frees anything still waiting in the retire queue

    int s1, n1, s2, n2;
    poolFifo.prepareToRead (poolFifo.getNumReady(), s1, n1, s2, n2);
    for (int i = 0; i < n1; ++i) delete pool[(size_t) (s1 + i)];
    for (int i = 0; i < n2; ++i) delete pool[(size_t) (s2 + i)];
    poolFifo.finishedRead (n1 + n2);
}

void LooperEngine::prepareToPlay (int samplesPerBlockExpected, double sr)
{
    sampleRate = sr;
    blockSize = juce::jmax (1, samplesPerBlockExpected);
    collector.reset (sr);
    perfCollector.reset (sr);
    midiBuffer.ensureSize (2048);
    inputScratch.setSize (2, juce::jmax (samplesPerBlockExpected, 16384), false, true, false);
    fxScratch.setSize (2, juce::jmax (samplesPerBlockExpected, 16384), false, true, false);
    padOut.setSize (2, juce::jmax (samplesPerBlockExpected, 16384), false, true, false);

    for (auto& r : stripRamps)
    {
        r.left.reset (sr, fadeSeconds);
        r.right.reset (sr, fadeSeconds);
    }

    // The device is stopped, so the running chains can be prepared again here. Chains still on their way
    // in were prepared for the old settings; the audio thread sends those back (Event::fxStale).
    {
        const std::lock_guard<std::mutex> lock (fxPrepareLock);

        auto reprepare = [&] (FxRun& r)
        {
            if (r.chain != nullptr)
                r.chain->reprepare (sr, blockSize.load());

            r.running = false;
            r.silent = r.tail = 0;
        };

        for (auto& p : pads)
            reprepare (p.fx);

        reprepare (monitorFx);
    }

    // Sample positions are meaningless after a rate change, so restart the clock.
    transportPos = 0;
    for (auto& p : pads)
    {
        p.pending = Pending::none;
        cancelRecording (p);
        cancelOverdub (p);
    }
}

//==============================================================================
bool LooperEngine::push (const Command& c)
{
    int s1, n1, s2, n2;
    cmdFifo.prepareToWrite (1, s1, n1, s2, n2);
    if (n1 + n2 == 0)
        return false;

    cmds[(size_t) (n1 > 0 ? s1 : s2)] = c;
    cmdFifo.finishedWrite (1);
    return true;
}

bool LooperEngine::assignClip (int pad, std::unique_ptr<Clip> clip, bool keepPhase, bool replacesLayer)
{
    Command c;
    c.type = Command::Type::assignClip;
    c.pad = pad;
    c.clip = clip.get();
    c.keepPhase = keepPhase;
    c.replacesLayer = replacesLayer;

    if (! push (c))
        return false;

    clip.release();
    return true;
}

void LooperEngine::triggerPad (int pad, float velocity)
{
    Command c; c.type = Command::Type::trigger; c.pad = pad; c.velocity = velocity;
    push (c);
}

void LooperEngine::releasePad (int pad)
{
    Command c; c.type = Command::Type::release; c.pad = pad;
    push (c);
}

void LooperEngine::setPadMode (int pad, PadMode mode)
{
    Command c; c.type = Command::Type::setMode; c.pad = pad; c.mode = mode;
    push (c);
}

void LooperEngine::setChokeGroup (int pad, int group)
{
    Command c; c.type = Command::Type::setChoke; c.pad = pad; c.group = group;
    push (c);
}

void LooperEngine::clearPad (int pad)
{
    Command c; c.type = Command::Type::clear; c.pad = pad;
    push (c);
}

void LooperEngine::setTempo (double newBpm)
{
    Command c; c.type = Command::Type::setTempo; c.value = newBpm;
    push (c);
}

void LooperEngine::setRecordMode (bool on)
{
    Command c; c.type = Command::Type::setRecordMode; c.value = on ? 1.0 : 0.0;
    push (c);
}

bool LooperEngine::setEffects (int target, std::unique_ptr<FxChain> chain)
{
    if (chain != nullptr && chain->slots.empty())
        chain = nullptr;

    Command c; c.type = Command::Type::setFx; c.pad = target; c.fx = chain.get();
    if (! push (c))
        return false;

    chain.release();
    return true;
}

//==============================================================================
// Mixer

void LooperEngine::setFader (int pad, float pos) noexcept
{
    auto& s = strips[(size_t) pad];
    s.fader.store (juce::jlimit (0.0f, 1.0f, pos));
    s.faderGen.fetch_add (1);
}

void LooperEngine::setPan (int pad, float pan) noexcept
{
    auto& s = strips[(size_t) pad];
    s.pan.store (juce::jlimit (-1.0f, 1.0f, pan));
    s.panGen.fetch_add (1);
}

void LooperEngine::muteAll() noexcept
{
    for (auto& s : strips)
        s.mute.store (true);
}

void LooperEngine::unmuteAll() noexcept
{
    for (auto& s : strips)
    {
        s.mute.store (false);
        s.solo.store (false);
    }
}

void LooperEngine::resetMixer() noexcept
{
    unmuteAll();

    for (int i = 0; i < numPads; ++i)
    {
        setFader (i, Mixer::unityFader());
        setPan (i, 0.0f);
    }
}

bool LooperEngine::isAnySoloed() const noexcept
{
    return std::any_of (strips.begin(), strips.end(), [] (const Strip& s) { return s.solo.load (std::memory_order_relaxed); });
}

// Audio thread. A CC on a volume or pan knob: moves it once soft takeover has picked the value up. True if any knob
// took the CC (it then isn't a button press as well).
bool LooperEngine::applyKnob (int cc, int value, int channel) noexcept
{
    const int code = MidiBinding::cc (cc).control();
    bool used = false;

    for (int pad = 0; pad < numPads; ++pad)
    {
        for (const auto c : { MixerControl::volume, MixerControl::pan })
        {
            if (! MidiBinding::matches (bindings[(size_t) mixerTarget (pad, c)].load (std::memory_order_relaxed), code, channel))
                continue;

            used = true;
            auto& s = strips[(size_t) pad];
            const bool volume = c == MixerControl::volume;
            auto& value01 = volume ? s.fader : s.pan;
            auto& t = takeovers[(size_t) (pad * 2 + (volume ? 0 : 1))];

            if (const int gen = (volume ? s.faderGen : s.panGen).load(); gen != t.seenGen)
            {
                t.seenGen = gen;            // the UI set it since: pick it up again
                t.engaged = false;
                t.lastValue = -1;
            }

            if (! t.engaged)
            {
                // Engage once the knob is within a step of the value, or has jumped across it since the last CC.
                const float current = volume ? value01.load() * 127.0f : Mixer::panToMidi (value01.load());
                const float offset = (float) value - current;

                t.engaged = std::abs (offset) <= 1.5f || (t.lastValue >= 0 && ((float) t.lastValue - current) * offset < 0.0f);
                t.lastValue = value;

                if (! t.engaged)
                    continue;
            }

            value01.store (volume ? (float) value / 127.0f : Mixer::panFromMidi (value));
        }
    }

    return used;
}

// Audio thread. Mute / solo toggles and Mute all / Unmute all; true if `control` is mapped to any of them.
bool LooperEngine::pressMixerButton (int control, int channel) noexcept
{
    auto bound = [&] (int target) { return MidiBinding::matches (bindings[(size_t) target].load (std::memory_order_relaxed), control, channel); };
    bool used = false;

    for (int pad = 0; pad < numPads; ++pad)
    {
        auto& s = strips[(size_t) pad];

        if (bound (mixerTarget (pad, MixerControl::mute))) { s.mute.store (! s.mute.load()); used = true; }
        if (bound (mixerTarget (pad, MixerControl::solo))) { s.solo.store (! s.solo.load()); used = true; }
    }

    if (bound (muteAllControl))   { muteAll();   used = true; }
    if (bound (unmuteAllControl)) { unmuteAll(); used = true; }
    return used;
}

// Audio thread. padOut holds pad `index`'s playback for this block (if it `sounded`); runs it through the strip and
// adds it to the main outputs. Gain changes ramp, except on a pad that was silent until this block: that starts
// straight at the new gains (no blip from a pad muted just as it starts).
void LooperEngine::mixStrip (int index, bool wasSounding, bool sounded, bool anySolo, juce::AudioBuffer<float>& out,
                             int destStart, int n)
{
    auto& s = strips[(size_t) index];
    auto& ramp = stripRamps[(size_t) index];
    float left = 0.0f, right = 0.0f;

    if (! s.mute.load (std::memory_order_relaxed) && (! anySolo || s.solo.load (std::memory_order_relaxed)))
    {
        const float gain = Mixer::faderToGain (s.fader.load (std::memory_order_relaxed));
        Mixer::panGains (s.pan.load (std::memory_order_relaxed), left, right);
        left *= gain;
        right *= gain;
    }

    if (! sounded || ! wasSounding)
    {
        ramp.left.setCurrentAndTargetValue (left);
        ramp.right.setCurrentAndTargetValue (right);

        if (! sounded)
            return;
    }

    // Per-pad outputs (JACK): a copy of the pad, before or after the strip.
    const auto padBus = OutputBuses::padChannels (index, out.getNumChannels());
    const bool preFader = OutputBuses::padsPreFader.load (std::memory_order_relaxed);

    if (preFader && ! padBus.isEmpty())
        for (int ch = 0; ch < 2; ++ch)
            out.copyFrom (padBus.getStart() + ch, destStart, padOut, ch, 0, n);
    else
    {
        ramp.left.setTargetValue (left);
        ramp.right.setTargetValue (right);
    }

    float* chans[2] = { padOut.getWritePointer (0), padOut.getWritePointer (1) };

    if (ramp.left.isSmoothing() || ramp.right.isSmoothing())
    {
        for (int i = 0; i < n; ++i)
        {
            chans[0][i] *= ramp.left.getNextValue();
            chans[1][i] *= ramp.right.getNextValue();
        }
    }
    else
    {
        juce::FloatVectorOperations::multiply (chans[0], left, n);
        juce::FloatVectorOperations::multiply (chans[1], right, n);
    }

    for (int ch = 0; ch < 2; ++ch)
    {
        auto& peak = s.peak[ch];
        peak.store (juce::jmax (peak.load (std::memory_order_relaxed), padOut.getMagnitude (ch, 0, n)), std::memory_order_relaxed);
    }

    if (! preFader && ! padBus.isEmpty())
        for (int ch = 0; ch < 2; ++ch)
            out.copyFrom (padBus.getStart() + ch, destStart, padOut, ch, 0, n);

    const auto bus = OutputBuses::channels (OutputBuses::main, out.getNumChannels());

    for (int ch = bus.getStart(); ch < bus.getEnd(); ++ch)
        out.addFrom (ch, destStart, padOut, juce::jmin (ch, 1), 0, n);
}

juce::Optional<juce::AudioPlayHead::PositionInfo> LooperEngine::getPosition() const
{
    const double ppq = (double) transportPos / samplesPerBeat();

    PositionInfo info;
    info.setBpm (audioBpm);
    info.setTimeSignature (TimeSignature { beatsPerBar, 4 });
    info.setTimeInSamples (transportPos);
    info.setTimeInSeconds ((double) transportPos / sampleRate.load());
    info.setPpqPosition (ppq);
    info.setPpqPositionOfLastBarStart (std::floor (ppq / beatsPerBar) * beatsPerBar);
    info.setIsPlaying (true);
    return info;
}

//==============================================================================
// Audio thread -> UI plumbing

bool LooperEngine::postEvent (const Event& e)
{
    int s1, n1, s2, n2;
    evFifo.prepareToWrite (1, s1, n1, s2, n2);
    if (n1 + n2 == 0)
        return false;

    events[(size_t) (n1 > 0 ? s1 : s2)] = e;
    evFifo.finishedWrite (1);
    return true;
}

void LooperEngine::retire (Clip* c)
{
    if (c == nullptr)
        return;

    Event e;
    e.type = Event::Type::retire;
    e.clip = c;
    postEvent (e);      // if the queue is ever full the clip leaks rather than blocking the audio thread
}

void LooperEngine::pollEvents (const std::function<void (const Event&)>& handler)
{
    int s1, n1, s2, n2;
    evFifo.prepareToRead (evFifo.getNumReady(), s1, n1, s2, n2);

    auto handle = [&] (const Event& e)
    {
        if (e.type == Event::Type::retire)    { delete e.clip; return; }
        if (e.type == Event::Type::retireFx)  { delete e.fx;   return; }

        if (handler)
            handler (e);

        if (e.type == Event::Type::fxStale)
            delete e.fx;
    };

    for (int i = 0; i < n1; ++i) handle (events[(size_t) (s1 + i)]);
    for (int i = 0; i < n2; ++i) handle (events[(size_t) (s2 + i)]);

    evFifo.finishedRead (n1 + n2);
}

bool LooperEngine::supplyRecordBuffer (std::unique_ptr<Clip> buffer)
{
    if (buffer == nullptr || buffer->audio.getNumChannels() < 2)
        return false;

    int s1, n1, s2, n2;
    poolFifo.prepareToWrite (1, s1, n1, s2, n2);
    if (n1 + n2 == 0)
        return false;

    pool[(size_t) (n1 > 0 ? s1 : s2)] = buffer.release();
    poolFifo.finishedWrite (1);
    return true;
}

Clip* LooperEngine::popRecordBuffer()
{
    int s1, n1, s2, n2;
    poolFifo.prepareToRead (1, s1, n1, s2, n2);
    if (n1 + n2 == 0)
        return nullptr;

    auto* c = pool[(size_t) (n1 > 0 ? s1 : s2)];
    poolFifo.finishedRead (1);
    return c;
}

//==============================================================================
// Clock helpers (audio thread)

double LooperEngine::gridSamples() const noexcept
{
    double beats = 0.0;

    switch ((Quantize) quantize.load())
    {
        case Quantize::bar:       beats = beatsPerBar; break;
        case Quantize::beat:      beats = 1.0;         break;
        case Quantize::eighth:    beats = 0.5;         break;
        case Quantize::sixteenth: beats = 0.25;        break;
        case Quantize::off:       break;
    }
    return beats * samplesPerBeat();
}

// Next grid line at or after t. A hit up to ~10 ms *after* a line counts as on the line
// (fires immediately instead of waiting a whole grid step).
juce::int64 LooperEngine::quantizeTime (juce::int64 t) const noexcept
{
    const double grid = gridSamples();
    if (grid <= 0.0)
        return t;

    const auto grace = (juce::int64) (0.010 * sampleRate.load());
    const double k = std::ceil ((double) (t - grace) / grid);
    const auto line = (juce::int64) std::llround (k * grid);
    return juce::jmax (t, line);
}

// Strictly the next line at or after t, with its index. Recording can't start in the past (the audio
// before the press is gone), and a take must start exactly on a line, so there is no grace here.
juce::int64 LooperEngine::lineAtOrAfter (juce::int64 t, double grid, juce::int64& k) noexcept
{
    k = (juce::int64) std::ceil ((double) t / grid);

    while (k > 0 && (juce::int64) std::llround ((double) (k - 1) * grid) >= t)
        --k;

    auto line = (juce::int64) std::llround ((double) k * grid);

    while (line < t)
        line = (juce::int64) std::llround ((double) ++k * grid);

    return line;
}

bool LooperEngine::anyRecordingBusy() const noexcept
{
    for (auto& p : pads)
        if (p.recActive || p.odActive || p.mergePending)
            return true;

    return false;
}

//==============================================================================
void LooperEngine::requestTrigger (PadState& p, float velocity, juce::int64 when)
{
    if (p.clip == nullptr || p.clip->numSamples() == 0)
        return;

    if (p.mode == PadMode::loop)
    {
        if (p.pending != Pending::none)         // a second hit cancels whatever was queued
        {
            cancelPending (p);
            return;
        }

        p.pending   = p.playing() ? Pending::stop : Pending::start;
        p.pendingAt = quantizeTime (when);
        p.pendingGain = loopVelocity;
    }
    else    // one-shot and gate: immediate
    {
        p.pending   = Pending::start;
        p.pendingAt = when;
        p.pendingGain = juce::jlimit (0.0f, 1.0f, velocity);
    }

    p.chokedBy = -1;

    if (p.pending == Pending::start)
        chokeOthers (p);
}

void LooperEngine::releaseNow (PadState& p)
{
    if (p.mode == PadMode::gate)
    {
        if (p.pending == Pending::start)     // let go before it sounded
            cancelPending (p);

        p.pending = Pending::none;           // a choke stop is moot now; the pad that caused it still starts
        p.chokedBy = -1;
        fadeOut (p);
    }
}

// Drops whatever the pad has queued, together with what it is linked to by a choke: a queued start
// takes back the stops it caused, and a choke stop takes back the start that caused it (latest hit wins).
void LooperEngine::cancelPending (PadState& p)
{
    if (p.pending == Pending::start)
        releaseChokes (indexOf (p));
    else if (p.pending == Pending::stop && juce::isPositiveAndBelow (p.chokedBy, numPads))
        if (auto& by = pads[(size_t) p.chokedBy]; by.pending == Pending::start)
            cancelPending (by);

    if (p.pending != Pending::recFinal)
        p.pending = Pending::none;

    p.chokedBy = -1;
}

void LooperEngine::chokeOthers (PadState& p)
{
    if (p.chokeGroup == 0)
        return;

    const int self = indexOf (p);
    auto inGroup = [&] (const PadState& q) { return &q != &p && q.chokeGroup == p.chokeGroup && ! q.recActive && ! q.odActive; };

    // Starts queued in the group are replaced by this hit. Cancel them all first: that also takes back
    // the stops they caused, which this hit then schedules again for itself.
    for (auto& q : pads)
        if (inGroup (q) && q.pending == Pending::start)
            cancelPending (q);

    for (auto& q : pads)
    {
        if (! inGroup (q))
            continue;

        if (q.playing() && (q.pending == Pending::none || (q.pending == Pending::stop && q.pendingAt > p.pendingAt)))
        {
            q.pending   = Pending::stop;
            q.pendingAt = p.pendingAt;
            q.chokedBy  = self;
        }
    }
}

void LooperEngine::releaseChokes (int by)
{
    for (auto& q : pads)
    {
        if (q.pending == Pending::stop && q.chokedBy == by)
        {
            q.pending = Pending::none;
            q.chokedBy = -1;
        }
    }
}

void LooperEngine::fadeOut (PadState& p)
{
    if (! p.playing())
        return;

    p.fadeLen  = juce::jmax (1, (int) std::lround (fadeSeconds * sampleRate.load()));
    p.fadeLeft = p.fadeLen;
}

void LooperEngine::toggleRecordMode() noexcept
{
    recordMode.store (! recordMode.load());
}

void LooperEngine::padPressed (PadState& p, float velocity, juce::int64 when)
{
    activePad.store (indexOf (p), std::memory_order_relaxed);

    // A pad that is mid-recording keeps owning its taps even if REC mode was switched off,
    // so the take/overdub can always be ended from the pad that started it.
    if (recordMode.load() || p.recActive || p.odActive)
        recordPress (p, when);
    else
        requestTrigger (p, velocity, when);
}

//==============================================================================
// Recording (audio thread)

void LooperEngine::recordPress (PadState& p, juce::int64 when)
{
    if (p.recActive)
    {
        if (p.recPromoted || p.recStopAt >= 0)
            return;                                     // already ending

        if (when < p.recStartAt)                        // tapped again before it began: cancel
        {
            cancelRecording (p);
            return;
        }

        juce::int64 k = 0;
        lineAtOrAfter (when, p.recGrid, k);
        scheduleRecStop (p, juce::jmax (k, p.recStartK + 1));
        return;
    }

    if (p.odActive)
    {
        if (p.odStopAt >= 0)
            return;

        if (when < p.odStartAt)
        {
            cancelOverdub (p);
            return;
        }

        juce::int64 k = 0;
        lineAtOrAfter (when, p.odGrid, k);
        p.odStopAt = (juce::int64) std::llround ((double) juce::jmax (k, p.odStartK + 1) * p.odGrid);
        return;
    }

    if (p.mergePending || (numInputChannels.load() == 0 && ! hasInstrument (p)))
        return;

    if (p.playing() && p.mode == PadMode::loop && p.clip != nullptr && p.clip->numSamples() > 0)
        armOverdub (p, when);
    else
        armTake (p, when);
}

void LooperEngine::armTake (PadState& p, juce::int64 when)
{
    auto* c = popRecordBuffer();
    if (c == nullptr)
        return;                                         // no buffer ready (the UI tops the pool up)

    const double grid = recGridSamples();
    const double spb  = samplesPerBeat();
    const int perLine = juce::jmax (1, (int) std::llround (grid / spb));
    const int maxLines = (int) std::floor ((double) (c->audio.getNumSamples() - 1) / grid);

    if (maxLines < 1)
    {
        retire (c);
        return;
    }

    cancelPending (p);

    juce::int64 k = 0;
    p.recStartAt = lineAtOrAfter (when, grid, k);
    p.recStartK = k;
    p.recGrid = grid;
    p.recBeatsPerLine = perLine;
    p.recClip = c;
    p.recStopAt = -1;
    p.recLen = c->audio.getNumSamples();
    p.recBeats = 0;
    p.recIdeal = 0.0;
    p.recAutoK = k + maxLines;
    p.recAutoAt = (juce::int64) std::llround ((double) p.recAutoK * grid);
    p.recPromoted = false;
    p.recActive = true;
}

void LooperEngine::armOverdub (PadState& p, juce::int64 when)
{
    if (p.pending != Pending::none)
        return;

    const int len = p.clip->numSamples();
    auto* layer = popRecordBuffer();
    if (layer == nullptr)
        return;

    if (layer->audio.getNumSamples() < len)             // loop longer than a record buffer
    {
        retire (layer);
        return;
    }

    layer->length = len;
    layer->idealLength = p.clip->idealLength;

    const double grid = recGridSamples();
    juce::int64 k = 0;
    p.odStartAt = lineAtOrAfter (when, grid, k);
    p.odStartK = k;
    p.odGrid = grid;
    p.odStopAt = -1;
    p.layer = layer;
    p.mergePending = false;
    p.odActive = true;
}

void LooperEngine::scheduleRecStop (PadState& p, juce::int64 stopK)
{
    p.recStopAt = (juce::int64) std::llround ((double) stopK * p.recGrid);
    p.recBeats = (int) ((stopK - p.recStartK) * p.recBeatsPerLine);

    // Same expression the UI uses for a loop's ideal length, so both sides agree on the sample count.
    p.recIdeal = (double) p.recBeats * 60.0 / audioBpm * sampleRate.load();
    p.recLen = juce::jmin (p.recClip->audio.getNumSamples(), (int) std::ceil (p.recIdeal));

    p.pending = Pending::recFinal;
    p.pendingAt = p.recStopAt;
}

// The finished take becomes the pad's clip and starts looping at the stop line. Capture of the last
// `latency` samples carries on into the same buffer for a few more milliseconds (see finishRecording);
// that is safe because the playhead only reaches those samples a whole loop later.
void LooperEngine::promoteTake (PadState& p, juce::int64 at)
{
    if (p.clip != nullptr && p.clip != p.recClip)
        retire (p.clip);

    if (p.layer != nullptr)
    {
        retire (p.layer);
        p.layer = nullptr;
        p.mergePending = false;
    }

    p.clip = p.recClip;
    p.clip->length = p.recLen;
    p.clip->idealLength = p.recIdeal;
    p.mode = PadMode::loop;
    p.pos = 0;
    p.gain = 1.0f;
    p.active = true;
    p.fadeLeft = 0;
    p.startAt = at;
    p.lagDebt = 0.0;
    p.recPromoted = true;
}

void LooperEngine::cancelRecording (PadState& p)
{
    if (! p.recActive)
        return;

    if (! p.recPromoted)
        retire (p.recClip);

    if (p.pending == Pending::recFinal)
        p.pending = Pending::none;

    p.recClip = nullptr;
    p.recActive = false;
    p.recPromoted = false;
    p.recStopAt = -1;
}

void LooperEngine::cancelOverdub (PadState& p)
{
    if (p.layer != nullptr)
        retire (p.layer);

    p.layer = nullptr;
    p.odActive = false;
    p.mergePending = false;
    p.odStopAt = -1;
}

// The round trip plus the input's effects (recordings are taken before the pad's own). An instrument isn't played
// from the input, so only the output half of the trip counts, plus the instrument's own latency.
int LooperEngine::inputLatency (const PadState& p) const noexcept
{
    if (hasInstrument (p))
        return instrumentLatency.load() + p.fx.chain->latency - p.fx.chain->effectsLatency;

    return recordLatency.load() + (monitorFx.chain != nullptr ? monitorFx.chain->latency : 0);
}

// Input at transport time T becomes take index T - latency - recStartAt.
void LooperEngine::captureTake (PadState& p, const juce::AudioBuffer<float>* input, int n, juce::int64 blockStart)
{
    if (! p.recActive || input == nullptr || p.recClip == nullptr)
        return;

    const juce::int64 k0 = blockStart - (juce::int64) inputLatency (p) - p.recStartAt;
    const int iStart = (int) juce::jlimit ((juce::int64) 0, (juce::int64) n, -k0);
    const int iEnd   = (int) juce::jlimit ((juce::int64) 0, (juce::int64) n, (juce::int64) p.recLen - k0);

    if (iEnd <= iStart)
        return;

    for (int ch = 0; ch < 2; ++ch)
        p.recClip->audio.copyFrom (ch, (int) (k0 + iStart), *input, ch, iStart, iEnd - iStart);
}

// Input at transport time T is added at the loop position that was audible at T - latency.
void LooperEngine::captureOverdub (PadState& p, const juce::AudioBuffer<float>* input, int n, juce::int64 blockStart)
{
    if (! p.odActive || input == nullptr || p.layer == nullptr || p.clip == nullptr)
        return;

    const int len = p.layer->length;
    if (len <= 0)
        return;

    const juce::int64 heard0 = blockStart - (juce::int64) inputLatency (p);   // heard time of input sample 0
    const int iStart = (int) juce::jlimit ((juce::int64) 0, (juce::int64) n, p.odStartAt - heard0);
    const int iEnd   = p.odStopAt >= 0 ? (int) juce::jlimit ((juce::int64) 0, (juce::int64) n, p.odStopAt - heard0)
                                       : n;
    if (iEnd <= iStart)
        return;

    const double ideal = p.clip->idealLength > 0.0 ? p.clip->idealLength : (double) len;
    double phase = std::fmod ((double) (heard0 + iStart - p.startAt), ideal);
    if (phase < 0.0)
        phase += ideal;

    int k = (int) std::llround (phase);

    float* dst[2] = { p.layer->audio.getWritePointer (0), p.layer->audio.getWritePointer (1) };

    for (int i = iStart; i < iEnd; ++i)
    {
        if (k >= len)
            k -= len;

        dst[0][k] += input->getSample (0, i);
        dst[1][k] += input->getSample (1, i);
        ++k;
    }
}

// Once the last of the delayed input has arrived, hand the result to the UI.
void LooperEngine::finishRecording (int index, PadState& p, juce::int64 blockEnd)
{
    const juce::int64 latency = inputLatency (p);

    if (p.recActive && p.recPromoted && blockEnd - latency - p.recStartAt >= p.recLen)
    {
        Event e;
        e.type = Event::Type::recorded;
        e.pad = index;
        e.clip = p.clip;
        e.length = p.recLen;
        e.beats = p.recBeats;
        e.ideal = p.recIdeal;

        if (postEvent (e))          // if the queue is full, try again next block
        {
            p.recActive = false;
            p.recClip = nullptr;
            p.recPromoted = false;
            p.recStopAt = -1;
        }
    }

    if (p.odActive && p.odStopAt >= 0 && blockEnd >= p.odStopAt + latency)
    {
        Event e;
        e.type = Event::Type::overdubbed;
        e.pad = index;
        e.clip = p.layer;
        e.length = p.layer->length;
        e.ideal = p.clip != nullptr ? p.clip->idealLength : 0.0;

        if (postEvent (e))
        {
            p.odActive = false;
            p.mergePending = true;      // the layer keeps playing until the merged clip arrives
        }
    }
}

//==============================================================================
void LooperEngine::apply (const Command& c)
{
    if (c.type == Command::Type::setRecordMode)
    {
        recordMode.store (c.value > 0.5);
        return;
    }

    if (c.type == Command::Type::setTempo)
    {
        if (anyRecordingBusy())     // recordings are locked to the tempo they were made at
            return;

        const double newBpm = juce::jlimit (30.0, 300.0, c.value);
        const double ratio = audioBpm / newBpm;       // new samples-per-beat / old

        // Rescale the clock and anything scheduled on it so beat position is preserved.
        transportPos = (juce::int64) std::llround ((double) transportPos * ratio);
        for (auto& p : pads)
        {
            p.startAt = (juce::int64) std::llround ((double) p.startAt * ratio);

            if (p.pending != Pending::none)
                p.pendingAt = (juce::int64) std::llround ((double) p.pendingAt * ratio);
        }

        audioBpm = newBpm;
        bpm.store (newBpm);
        return;
    }

    if (c.type == Command::Type::setFx)
    {
        auto* run = c.pad == monitorTarget ? &monitorFx
                  : juce::isPositiveAndBelow (c.pad, numPads) ? &pads[(size_t) c.pad].fx : nullptr;

        Event e;
        e.pad = c.pad;

        if (run == nullptr)
        {
            e.type = Event::Type::retireFx;
            e.fx = c.fx;
        }
        else if (c.fx != nullptr && (! juce::exactlyEqual (c.fx->rate, sampleRate.load()) || c.fx->block != blockSize.load()))
        {
            e.type = Event::Type::fxStale;      // prepared for a device setup that's gone
            e.fx = c.fx;
        }
        else
        {
            e.type = Event::Type::retireFx;
            e.fx = run->chain;
            run->chain = c.fx;
            run->running = false;
            run->silent = run->tail = 0;
        }

        if (e.fx != nullptr)
            postEvent (e);      // if the queue is ever full the chain leaks rather than blocking the audio thread

        return;
    }

    if (! juce::isPositiveAndBelow (c.pad, numPads))
    {
        if (c.type == Command::Type::assignClip)
            retire (c.clip);

        return;
    }

    auto& p = pads[(size_t) c.pad];

    switch (c.type)
    {
        case Command::Type::assignClip:
        {
            // Refuse the swap while the pad's clip is still being filled by a take, or while an overdub
            // layer is tied to its length (only the overdub's own merge may replace it). The UI resends.
            const bool takeFilling = p.recActive && p.recPromoted;
            const bool layerTied   = c.keepPhase && (p.odActive || (p.mergePending && ! c.replacesLayer));

            if (takeFilling || layerTied)
            {
                retire (c.clip);

                Event e;
                e.type = Event::Type::swapRejected;
                e.pad = c.pad;
                postEvent (e);
                break;
            }

            if (c.keepPhase)
            {
                // A finished overdub's layer is replaced by the merged clip that arrives this way.
                if (c.replacesLayer && p.layer != nullptr)
                {
                    retire (p.layer);
                    p.layer = nullptr;
                    p.mergePending = false;
                }
            }
            else
            {
                cancelRecording (p);
                cancelOverdub (p);
            }

            if (p.clip != nullptr && p.clip != c.clip)
                retire (p.clip);

            p.clip = c.clip;

            if (c.keepPhase)
            {
                // Re-stretched version of a loop. If it's playing, keep its place in the bar by
                // deriving the position from the clock, so it stays on the grid after a tempo change.
                if (p.active && c.clip != nullptr)
                {
                    const int newLen = c.clip->numSamples();
                    int newPos = 0;

                    if (c.clip->idealLength > 0.0)
                    {
                        double phase = std::fmod ((double) (transportPos - p.startAt), c.clip->idealLength);
                        if (phase < 0.0) phase += c.clip->idealLength;
                        newPos = (int) std::llround (phase);
                    }
                    p.pos = juce::jlimit (0, juce::jmax (0, newLen - 1), newPos);
                    p.lagDebt = 0.0;
                }
            }
            else
            {
                cancelPending (p);
                p.active = false;
                p.fadeLeft = 0;
                p.pos = 0;
                p.lagDebt = 0.0;
            }
            break;
        }
        case Command::Type::trigger:  padPressed (p, c.velocity, transportPos); break;
        case Command::Type::release:  releaseNow (p); break;
        case Command::Type::setMode:  p.mode = c.mode; cancelPending (p); break;
        case Command::Type::setChoke: p.chokeGroup = juce::jlimit (0, numChokeGroups, c.group); break;

        case Command::Type::clear:
            cancelRecording (p);        // a take that already became the pad's clip is retired below
            cancelOverdub (p);
            retire (p.clip);
            p.clip = nullptr;
            cancelPending (p);
            p.active = false;
            p.fadeLeft = 0;
            p.pos = 0;
            p.lagDebt = 0.0;
            break;

        case Command::Type::setTempo:
        case Command::Type::setRecordMode:
        case Command::Type::setFx:         break;     // handled above
    }
}

//==============================================================================
void LooperEngine::renderPad (PadState& p, juce::AudioBuffer<float>& out, int destStart, int count)
{
    if (! p.active || p.clip == nullptr || count <= 0)
        return;

    const auto& src = p.clip->audio;
    const int srcLen = p.clip->numSamples();
    const int srcCh  = src.getNumChannels();

    const Clip* layer = p.layer;
    const int layerLen = layer != nullptr ? layer->numSamples() : 0;

    if (srcLen == 0) { p.active = false; return; }

    const auto chans = OutputBuses::channels (OutputBuses::main, out.getNumChannels());
    int done = 0;
    while (done < count)
    {
        int n = juce::jmin (count - done, srcLen - p.pos);
        float g0 = p.gain, g1 = p.gain;

        if (p.fadeLeft > 0)
        {
            n  = juce::jmin (n, p.fadeLeft);
            g0 = p.gain * (float) p.fadeLeft / (float) p.fadeLen;
            g1 = p.gain * (float) (p.fadeLeft - n) / (float) p.fadeLen;
        }

        for (int ch = chans.getStart(); ch < chans.getEnd(); ++ch)
            out.addFromWithRamp (ch, destStart + done, src.getReadPointer (juce::jmin (ch, srcCh - 1), p.pos), n, g0, g1);

        if (layer != nullptr && p.pos < layerLen)
        {
            const int m = juce::jmin (n, layerLen - p.pos);
            const float gm = g0 + (g1 - g0) * (float) m / (float) n;

            for (int ch = chans.getStart(); ch < chans.getEnd(); ++ch)
                out.addFromWithRamp (ch, destStart + done,
                                     layer->audio.getReadPointer (juce::jmin (ch, layer->audio.getNumChannels() - 1), p.pos),
                                     m, g0, gm);
        }

        p.pos += n;
        done += n;

        if (p.fadeLeft > 0)
        {
            p.fadeLeft -= n;

            if (p.fadeLeft == 0)
            {
                p.active = false;
                p.pos = 0;
                break;
            }
        }

        if (p.pos >= srcLen)
        {
            p.pos = 0;
            if (p.mode != PadMode::loop)
            {
                p.active = false;
                break;
            }

            // A beat-locked loop is stretched up to a whole sample count, so it runs a fraction of a
            // sample long each pass. Skip the excess a sample at a time so it never drifts off the grid.
            if (p.clip->idealLength > 0.0)
            {
                p.lagDebt = juce::jmax (0.0, p.lagDebt + (double) srcLen - p.clip->idealLength);
                const int skip = (int) p.lagDebt;

                if (skip > 0)
                {
                    p.pos = juce::jmin (skip, srcLen - 1);
                    p.lagDebt -= (double) skip;
                }
            }
        }
    }
}

// padOut holds `n` samples of the pad's playback. Adds the live sound to it (the instrument playing `notes`, or the
// monitored input if `live`) and runs it all through the pad's effects in place. Returns whether the chain ran. Once
// nothing feeds it, the chain rings out until a stretch of silence, then idles.
bool LooperEngine::runPadEffects (PadState& p, bool playing, bool live, int n, const juce::MidiBuffer& notes)
{
    auto& run = p.fx;
    const bool instrument = run.chain->instrument;
    const bool fed = playing || live || ! notes.isEmpty();

    if (! fed && ! run.running)
        return false;

    if (instrument)
    {
        fxScratch.clear (0, n);
        run.chain->process (fxScratch, n, &notes, 0, 1);

        for (int ch = 0; ch < 2; ++ch)
            padOut.addFrom (ch, 0, fxScratch, ch, 0, n);
    }
    else if (live)
    {
        for (int ch = 0; ch < 2; ++ch)
            padOut.addFrom (ch, 0, inputScratch, ch, 0, n);
    }

    run.chain->process (padOut, n, nullptr, instrument ? 1 : 0);

    if (fed)
    {
        run.running = true;
        run.silent = run.tail = 0;
        return true;
    }

    const double sr = sampleRate.load();
    run.tail += n;
    run.silent = juce::jmax (padOut.getMagnitude (0, 0, n), padOut.getMagnitude (1, 0, n)) < 1.0e-4f ? run.silent + n : 0;  // -80 dB

    if (run.silent >= (int) (fxIdleSeconds * sr) || run.tail >= (int) (fxMaxTailSeconds * sr))
        run.running = false;

    return true;
}

// Splits this block's performance-channel MIDI into the pads' instrument buffers: notes go to the active pad, and
// each note-off (and a sustain release) to the pad that got the matching note-on, wherever the active pad is now.
void LooperEngine::routePerformanceMidi (int n)
{
    for (auto& b : padMidi)
        b.clear();

    perfMidi.clear();
    perfCollector.removeNextBlockOfMessages (perfMidi, n);

    const int active = activePad.load (std::memory_order_relaxed);
    const bool activePlays = juce::isPositiveAndBelow (active, numPads) && hasInstrument (pads[(size_t) active]);

    auto send = [this] (int pad, const juce::MidiMessage& m, int pos)
    {
        if (juce::isPositiveAndBelow (pad, numPads) && hasInstrument (pads[(size_t) pad]))
            padMidi[(size_t) pad].addEvent (m, pos);
    };

    for (const auto meta : perfMidi)
    {
        const auto m = meta.getMessage();
        const int pos = meta.samplePosition;
        const int ch = m.getChannel() - 1;

        if (! juce::isPositiveAndBelow (ch, 16))
            continue;

        if (m.isNoteOn() || m.isNoteOff())
        {
            auto& owner = noteOwner[(size_t) ch][(size_t) m.getNoteNumber()];

            if (m.isNoteOn())
            {
                if (owner >= 0 && owner != active)      // still sounding on a pad that's no longer active
                    send (owner, juce::MidiMessage::noteOff (ch + 1, m.getNoteNumber()), pos);

                send (active, m, pos);
                owner = (juce::int8) (activePlays ? active : -1);
            }
            else
            {
                send (owner, m, pos);
                owner = -1;
            }
        }
        else if (m.isSustainPedalOn() || m.isSustainPedalOff())
        {
            auto& owner = sustainOwner[(size_t) ch];
            send (active, m, pos);

            if (m.isSustainPedalOff() && owner != active)
                send (owner, m, pos);

            owner = (juce::int8) (m.isSustainPedalOn() && activePlays ? active : -1);
        }
        else if (m.isAllNotesOff() || m.isAllSoundOff())
        {
            for (int i = 0; i < numPads; ++i)
                send (i, m, pos);

            noteOwner[(size_t) ch].fill (-1);
            sustainOwner[(size_t) ch] = -1;
        }
        else
        {
            send (active, m, pos);
        }
    }
}

void LooperEngine::renderMetronome (juce::AudioBuffer<float>& out, int destStart, int count, juce::int64 blockStart)
{
    const double sr  = sampleRate.load();
    const double spb = samplesPerBeat();
    const auto chans = OutputBuses::channels (OutputBuses::metronome, out.getNumChannels());

    auto beat = (juce::int64) std::ceil ((double) blockStart / spb);
    auto nextOffset = [&] { return (int) (std::llround ((double) beat * spb) - blockStart); };
    int nextOff = nextOffset();

    for (int i = 0; i < count; ++i)
    {
        while (nextOff <= i)
        {
            clickPos = 0;
            clickLen = (int) (0.03 * sr);
            clickPhase = 0.0;
            clickFreq = (beat % beatsPerBar == 0) ? 1500.0 : 1000.0;   // accent the downbeat
            ++beat;
            nextOff = nextOffset();
        }

        if (clickPos < clickLen)
        {
            const float s = (float) (std::sin (clickPhase) * 0.4 * (1.0 - (double) clickPos / clickLen));
            clickPhase += juce::MathConstants<double>::twoPi * clickFreq / sr;
            ++clickPos;

            for (int ch = chans.getStart(); ch < chans.getEnd(); ++ch)
                out.addSample (ch, destStart + i, s);
        }
    }
}

void LooperEngine::getNextAudioBlock (const juce::AudioSourceChannelInfo& info)
{
    const int n = info.numSamples;
    auto& out = *info.buffer;

    // 0. The player has copied the input channels into the buffer; take them before it is cleared.
    haveInput = false;
    {
        const int nIn = juce::jmin (numInputChannels.load(), out.getNumChannels());

        if (nIn > 0 && n <= inputScratch.getNumSamples())
        {
            inputScratch.copyFrom (0, 0, out, 0, info.startSample, n);
            inputScratch.copyFrom (1, 0, out, nIn > 1 ? 1 : 0, info.startSample, n);
            haveInput = true;
        }
    }

    info.clearActiveBufferRegion();

    // 1. UI commands (may rescale the clock on a tempo change, so do this before reading transportPos)
    {
        int s1, n1, s2, n2;
        cmdFifo.prepareToRead (cmdFifo.getNumReady(), s1, n1, s2, n2);
        for (int i = 0; i < n1; ++i) apply (cmds[(size_t) (s1 + i)]);
        for (int i = 0; i < n2; ++i) apply (cmds[(size_t) (s2 + i)]);
        cmdFifo.finishedRead (n1 + n2);
    }

    const juce::int64 blockStart = transportPos;
    const juce::int64 blockEnd   = blockStart + n;

    // 2. MIDI, timestamped on the transport so quantizing is sample-accurate
    midiBuffer.clear();
    collector.removeNextBlockOfMessages (midiBuffer, n);

    const int recCode = bindings[(size_t) recControl].load();

    for (const auto meta : midiBuffer)
    {
        const auto m = meta.getMessage();
        const int channel = m.getChannel();     // 1..16 (the MIDI thread already dropped the performance channel)
        bool press = false, release = false;
        float velocity = 1.0f;
        int code = 0;                           // MidiBinding::control()

        if (channel == 0)
            continue;

        if (m.isNoteOnOrOff())
        {
            code = MidiBinding::note (m.getNoteNumber()).control();
            press = m.isNoteOn();
            release = ! press;
            velocity = m.getFloatVelocity();
        }
        else if (m.isController())
        {
            if (applyKnob (m.getControllerNumber(), m.getControllerValue(), channel))
                continue;

            // A CC acts as a button: crossing up through 64 presses, falling back below releases.
            // Some buttons (often transport ones) send 127 on press and nothing on release, so a
            // high value repeated exactly is a fresh press too; a knob never repeats a value.
            const int num = m.getControllerNumber();
            const int value = m.getControllerValue();
            const bool high = value >= 64;
            auto& wasHigh = ccHigh[(size_t) channel - 1][(size_t) num];
            auto& last    = ccLast[(size_t) channel - 1][(size_t) num];
            const bool repeat = high && value == last;

            code = MidiBinding::cc (num).control();
            press = high && (! wasHigh || repeat);
            release = ! high && wasHigh;
            velocity = (float) value / 127.0f;
            wasHigh = high;
            last = value;
        }

        if (code == 0 || ! (press || release))
            continue;

        // The REC control wins over a pad mapped to the same control.
        if (MidiBinding::matches (recCode, code, channel))
        {
            if (press)
                toggleRecordMode();

            continue;
        }

        if (press)
            pressMixerButton (code, channel);

        for (int i = 0; i < numPads; ++i)
        {
            if (! MidiBinding::matches (bindings[(size_t) i].load (std::memory_order_relaxed), code, channel))
                continue;

            auto& p = pads[(size_t) i];

            if (press) padPressed (p, velocity, blockStart + meta.samplePosition);
            else       releaseNow (p);
        }
    }

    // 3. The performance channel, to the instruments
    routePerformanceMidi (n);

    // 4. The input's own effects, whenever it's heard or recorded
    const bool monitoring = monitorOn.load() && haveInput;
    const int active = activePad.load (std::memory_order_relaxed);
    const bool fxFits = n <= fxScratch.getNumSamples() && n <= padOut.getNumSamples();

    if (haveInput && monitorFx.chain != nullptr && fxFits)
    {
        bool needed = monitoring;
        for (const auto& p : pads)
            needed = needed || ((p.recActive || p.odActive) && ! hasInstrument (p));

        if (needed)
            monitorFx.chain->process (inputScratch, n);
    }

    bool inputHeard = false;    // the active pad already played the monitored input
    const bool anySolo = isAnySoloed();
    const bool mixFits = fxFits;

    // 5. Render pads, splitting the block where a pending start/stop lands, then record into them
    for (int i = 0; i < numPads; ++i)
    {
        auto& p = pads[(size_t) i];

        // A take that is about to fill its buffer ends itself on the last line that fits.
        if (p.recActive && ! p.recPromoted && p.recStopAt < 0 && p.recAutoAt <= blockEnd + n)
            scheduleRecStop (p, p.recAutoK);

        // Playback goes through the pad's effects and mixer strip (padOut), unless the block is too big for them.
        // A pad whose effects add latency acts that much early, so it's heard on time.
        auto& dest = mixFits ? padOut : out;
        const int base = mixFits ? 0 : info.startSample;
        const bool wasActive = p.active;
        const int lead = mixFits ? playbackLead (p) : 0;

        if (mixFits)
            padOut.clear (0, n);

        if (p.pending != Pending::none && p.pendingAt - lead < blockEnd)
        {
            const int split = (int) juce::jmax ((juce::int64) 0, p.pendingAt - lead - blockStart);

            renderPad (p, dest, base, split);

            switch (p.pending)
            {
                case Pending::start:
                    p.pos = 0;
                    p.gain = p.pendingGain;
                    p.active = true;
                    p.fadeLeft = 0;
                    p.startAt = blockStart + split + lead;
                    p.lagDebt = 0.0;
                    break;

                case Pending::stop:      fadeOut (p); break;
                case Pending::recFinal:  promoteTake (p, blockStart + split + lead); break;
                case Pending::none:      break;
            }
            p.pending = Pending::none;
            p.chokedBy = -1;

            renderPad (p, dest, base + split, n - split);
        }
        else
        {
            renderPad (p, dest, base, n);
        }

        // Recordings are dry: the input (after its own effects), or an instrument's output before the pad's effects.
        // The live sound goes through the active pad (an instrument pad's while it's recording too).
        const bool instrument = hasInstrument (p);
        const bool live = instrument ? (i == active || p.recActive || p.odActive) : (i == active && monitoring && mixFits);
        const juce::AudioBuffer<float>* recordFrom = instrument || ! haveInput ? nullptr : &inputScratch;
        bool sounded = wasActive || p.active;

        if (p.fx.chain != nullptr && mixFits)
        {
            if (runPadEffects (p, sounded, live, n, padMidi[(size_t) i]))
            {
                sounded = true;

                if (instrument)
                    recordFrom = &fxScratch;
            }
        }
        else if (live && ! instrument)
        {
            for (int ch = 0; ch < 2; ++ch)
                padOut.addFrom (ch, 0, inputScratch, ch, 0, n);

            sounded = true;
        }

        inputHeard = inputHeard || (live && ! instrument);

        if (mixFits)
        {
            mixStrip (i, p.sounded, sounded, anySolo, out, info.startSample, n);
            p.sounded = sounded;
        }

        captureTake (p, recordFrom, n, blockStart);
        captureOverdub (p, recordFrom, n, blockStart);
        finishRecording (i, p, blockEnd);

        PadStatus s = PadStatus::idle;

        if (p.recActive && ! p.recPromoted)   s = blockEnd <= p.recStartAt ? PadStatus::recQueued : PadStatus::recording;
        else if (p.odActive)                  s = blockEnd <= p.odStartAt  ? PadStatus::overdubQueued : PadStatus::overdubbing;
        else if (p.pending != Pending::none)  s = PadStatus::queued;
        else if (p.playing())                 s = PadStatus::playing;

        status[(size_t) i].store ((int) s, std::memory_order_relaxed);
    }

    recBusy.store (anyRecordingBusy(), std::memory_order_relaxed);

    // 6. Metronome (on its own outputs under JACK)
    if (metronomeOn.load())
        renderMetronome (out, info.startSample, n, blockStart);

    // 7. Input monitoring, unless the active pad already played it
    if (monitoring && ! inputHeard)
    {
        const auto chans = OutputBuses::channels (OutputBuses::main, out.getNumChannels());

        for (int ch = chans.getStart(); ch < chans.getEnd(); ++ch)
            out.addFrom (ch, info.startSample, inputScratch, juce::jmin (ch, 1), 0, n);
    }

    // 8. Advance the clock
    transportPos = blockEnd;
    beatInBar.store (std::fmod ((double) transportPos / samplesPerBeat(), (double) beatsPerBar),
                     std::memory_order_relaxed);
}
