#pragma once
#include <JuceHeader.h>
#include <array>
#include <atomic>

//==============================================================================
// Indigo's MIDI output for syncing a DAW: a virtual port named "Indigo" (an ALSA sequencer port, which PipeWire shows
// in its patchbay through the Midi-Bridge node). The audio thread queues timestamped messages (MIDI clock, MTC,
// Start / Stop) without blocking, and a thread of its own sends each one when it is due: the moment the audio
// rendered alongside it is heard.
class SyncOutput : private juce::Thread
{
public:
    struct Message
    {
        std::array<juce::uint8, 10> data {};     // long enough for an MTC Full Frame
        int size = 0;
        double timeMs = 0.0;     // juce::Time::getMillisecondCounterHiRes() when it should go out
    };

    static constexpr const char* portName = "Indigo";

    SyncOutput() : juce::Thread ("MIDI sync output") {}
    ~SyncOutput() override { setOpen (false); }

    // Message thread: creates or removes the port. Nothing is queued while it is closed.
    void setOpen (bool on)
    {
        if (on == isOpen())
            return;

        if (on)
        {
            output = juce::MidiOutput::createNewDevice (portName);

            if (output != nullptr)
            {
                fifo.finishedRead (fifo.getNumReady());     // drop anything left from last time
                open.store (true);
                startThread (juce::Thread::Priority::highest);
            }
        }
        else
        {
            open.store (false);
            stopThread (1000);
            output = nullptr;
        }
    }

    bool isOpen() const noexcept    { return open.load(); }

    // Audio thread. A message that doesn't fit (the queue is full) is dropped.
    bool push (const Message& m) noexcept
    {
        if (! open.load (std::memory_order_relaxed))
            return false;

        int s1, n1, s2, n2;
        fifo.prepareToWrite (1, s1, n1, s2, n2);
        if (n1 + n2 == 0)
            return false;

        queue[(size_t) (n1 > 0 ? s1 : s2)] = m;
        fifo.finishedWrite (1);
        return true;
    }

private:
    void run() override
    {
        while (! threadShouldExit())
        {
            if (fifo.getNumReady() == 0)
            {
                wait (1);
                continue;
            }

            int s1, n1, s2, n2;
            fifo.prepareToRead (1, s1, n1, s2, n2);
            const auto& m = queue[(size_t) (n1 > 0 ? s1 : s2)];

            // Sleep most of the way, then yield until it's due (Thread::wait is only good to a millisecond or so).
            for (double left = m.timeMs - juce::Time::getMillisecondCounterHiRes(); left > 0.0 && ! threadShouldExit();
                 left = m.timeMs - juce::Time::getMillisecondCounterHiRes())
            {
                if (left > 2.0) wait ((int) left - 1);
                else            juce::Thread::yield();
            }

            output->sendMessageNow (juce::MidiMessage (m.data.data(), m.size));
            fifo.finishedRead (1);
        }
    }

    std::unique_ptr<juce::MidiOutput> output;
    std::atomic<bool> open { false };
    juce::AbstractFifo fifo { 1024 };
    std::array<Message, 1024> queue;
};
