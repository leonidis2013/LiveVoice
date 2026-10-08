#pragma once

#include <array>
#include <atomic>
#include <algorithm>

// Lock-free ring of (level, gain reduction) columns written by the audio
// thread and read by the UI for scrolling histograms.
namespace dsp
{
    class LevelHistory
    {
    public:
        static constexpr int size = 1024;
        struct Point { float levelDb; float grDb; };

        void prepare (double sampleRate, int columnMs)
        {
            samplesPerColumn = std::max (1, (int) (sampleRate * columnMs / 1000.0));
            colCount = 0; colLevel = -100.0f; colGr = 0.0f;
            for (auto& s : slots) { s.level.store (-100.0f); s.gr.store (0.0f); }
        }

        // audio thread, once per sample (cheap)
        inline void add (float levelDb, float grDb) noexcept
        {
            colLevel = std::max (colLevel, levelDb);
            colGr = std::max (colGr, grDb);
            if (++colCount >= samplesPerColumn)
            {
                const int w = writeIndex.load (std::memory_order_relaxed);
                slots[(size_t) w].level.store (colLevel, std::memory_order_relaxed);
                slots[(size_t) w].gr.store (colGr, std::memory_order_relaxed);
                writeIndex.store ((w + 1) % size, std::memory_order_release);
                colCount = 0; colLevel = -100.0f; colGr = 0.0f;
            }
        }

        // Copies the most recent `count` columns, oldest first.
        void read (Point* dest, int count) const noexcept
        {
            count = std::min (count, size);
            const int w = writeIndex.load (std::memory_order_acquire);
            for (int i = 0; i < count; ++i)
            {
                const int idx = (w - count + i + size) % size;
                dest[i] = { slots[(size_t) idx].level.load (std::memory_order_relaxed),
                            slots[(size_t) idx].gr.load (std::memory_order_relaxed) };
            }
        }

    private:
        struct Slot { std::atomic<float> level { -100.0f }, gr { 0.0f }; };
        std::array<Slot, size> slots;
        std::atomic<int> writeIndex { 0 };
        int samplesPerColumn = 1440, colCount = 0;
        float colLevel = -100.0f, colGr = 0.0f;
    };
}
