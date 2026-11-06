#pragma once
#include <array>
#include <cstdint>

//==============================================================================
// MIDI Time Code: turning a count of frames since 00:00:00:00 into the messages that carry it. Quarter frames run at
// four per frame and spell the time out over eight messages, so a full time arrives every two frames. The time they
// spell is that of the frame the first one went out on.
namespace Mtc
{
    enum Rate : int { fps24 = 0, fps25 = 1, fps2997Drop = 2, fps30 = 3 };     // the values are MTC's own rate codes

    struct Time { int hours = 0, minutes = 0, seconds = 0, frames = 0; };

    inline double framesPerSecond (Rate rate) noexcept
    {
        switch (rate)
        {
            case fps24:       return 24.0;
            case fps25:       return 25.0;
            case fps2997Drop: return 30000.0 / 1001.0;
            case fps30:       break;
        }
        return 30.0;
    }

    inline Time timeOfFrame (std::int64_t frame, Rate rate) noexcept
    {
        int nominal = rate == fps24 ? 24 : rate == fps25 ? 25 : 30;

        // Drop-frame labels skip 00 and 01 at the start of every minute except each tenth, to stay on the wall clock.
        if (rate == fps2997Drop)
        {
            constexpr std::int64_t per10Minutes = 17982, perMinute = 1798;
            const auto tens = frame / per10Minutes;
            const auto rest = frame % per10Minutes;
            frame += 18 * tens + 2 * ((rest - 2) / perMinute);
        }

        const auto seconds = frame / nominal;

        Time t;
        t.frames  = (int) (frame % nominal);
        t.seconds = (int) (seconds % 60);
        t.minutes = (int) (seconds / 60 % 60);
        t.hours   = (int) (seconds / 3600 % 24);
        return t;
    }

    // The data byte of quarter frame number `index`, counted from frame 0 (status byte 0xf1).
    inline std::uint8_t quarterFrame (std::int64_t index, Rate rate) noexcept
    {
        const int piece = (int) (index % 8);
        const auto t = timeOfFrame ((index - piece) / 4, rate);

        const int nibble = [&]
        {
            switch (piece)
            {
                case 0:  return t.frames & 15;
                case 1:  return t.frames >> 4;
                case 2:  return t.seconds & 15;
                case 3:  return t.seconds >> 4;
                case 4:  return t.minutes & 15;
                case 5:  return t.minutes >> 4;
                case 6:  return t.hours & 15;
                default: return ((int) rate << 1) | (t.hours >> 4);
            }
        }();

        return (std::uint8_t) ((piece << 4) | nibble);
    }

    // A Full Frame message: tells a follower to locate there.
    inline std::array<std::uint8_t, 10> fullFrame (std::int64_t frame, Rate rate) noexcept
    {
        const auto t = timeOfFrame (frame, rate);

        return { 0xf0, 0x7f, 0x7f, 0x01, 0x01, (std::uint8_t) (((int) rate << 5) | t.hours), (std::uint8_t) t.minutes,
                 (std::uint8_t) t.seconds, (std::uint8_t) t.frames, 0xf7 };
    }
}
