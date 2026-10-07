#pragma once

#include <array>
#include <juce_core/juce_core.h>

// Single-producer / single-consumer queue built on juce::AbstractFifo.
namespace dsp
{
    template <typename T, int Capacity>
    class LockFreeQueue
    {
    public:
        bool push (const T& item) noexcept
        {
            int s1, n1, s2, n2;
            fifo.prepareToWrite (1, s1, n1, s2, n2);
            if (n1 + n2 < 1) return false;
            data[(size_t) (n1 > 0 ? s1 : s2)] = item;
            fifo.finishedWrite (1);
            return true;
        }

        bool pop (T& item) noexcept
        {
            int s1, n1, s2, n2;
            fifo.prepareToRead (1, s1, n1, s2, n2);
            if (n1 + n2 < 1) return false;
            item = data[(size_t) (n1 > 0 ? s1 : s2)];
            fifo.finishedRead (1);
            return true;
        }

    private:
        juce::AbstractFifo fifo { Capacity };
        std::array<T, (size_t) Capacity> data {};
    };
}
