// Offline tests for LiveVoice. Run:  LiveVoiceTests            (DSP tests)
//                                    LiveVoiceTests --snapshot out.png
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <random>

using namespace params;

namespace
{
    int failures = 0;
    void check (bool ok, const juce::String& what)
    {
        std::cout << (ok ? "  PASS  " : "  FAIL  ") << what << std::endl;
        if (! ok) ++failures;
    }

    constexpr double fs = 48000.0;
    constexpr int blockSize = 256;

    struct Harness
    {
        LiveVoiceProcessor p;
        juce::AudioBuffer<float> buf { 1, blockSize };
        juce::MidiBuffer midi;

        Harness()
        {
            p.setPlayConfigDetails (1, 1, fs, blockSize);
            p.prepareToPlay (fs, blockSize);
            p.detector.stopThreadForTest();     // run detection synchronously, deterministically
        }

        void set (const juce::String& id, float v)
        {
            auto* rp = p.apvts.getParameter (id);
            rp->setValueNotifyingHost (rp->convertTo0to1 (v));
        }

        void allModulesOff()
        {
            set (fbOn, 0); set (eqOn, 0); set (dsOn, 0); set (limitOn, 0);
        }

        // Runs `input(n)` through the plugin; returns the output.
        std::vector<float> run (const std::function<float (int64_t)>& input, double seconds)
        {
            const int64_t total = (int64_t) (seconds * fs);
            std::vector<float> out ((size_t) total);
            for (int64_t start = 0; start < total; start += blockSize)
            {
                const int n = (int) std::min<int64_t> (blockSize, total - start);
                buf.setSize (1, n, false, false, true);
                for (int i = 0; i < n; ++i) buf.setSample (0, i, input (start + i));
                p.processBlock (buf, midi);
                p.detector.processPendingForTest();
                for (int i = 0; i < n; ++i) out[(size_t) (start + i)] = buf.getSample (0, i);
            }
            return out;
        }

        std::vector<dsp::FeedbackSuppressor::NotchInfo> notches() const
        {
            std::vector<dsp::FeedbackSuppressor::NotchInfo> v;
            for (int i = 0; i < dsp::FeedbackSuppressor::maxSlots; ++i)
            {
                auto n = p.feedback.getNotch (i);
                if (n.kind != dsp::FeedbackSuppressor::kindInactive && n.targetDb < -0.01f) v.push_back (n);
            }
            return v;
        }
    };

    double rmsDb (const std::vector<float>& x, size_t from, size_t to)
    {
        double s = 0.0;
        for (size_t i = from; i < to; ++i) s += (double) x[i] * x[i];
        return 10.0 * std::log10 (s / (double) std::max<size_t> (1, to - from) + 1e-20);
    }

    // Amplitude (dBFS, sine-peak) of one frequency via Goertzel over a window.
    double toneDb (const std::vector<float>& x, size_t from, size_t to, double freq)
    {
        const double w = 2.0 * juce::MathConstants<double>::pi * freq / fs;
        double s1 = 0, s2 = 0;
        const double c = 2.0 * std::cos (w);
        for (size_t i = from; i < to; ++i)
        {
            const double win = 0.5 - 0.5 * std::cos (2.0 * juce::MathConstants<double>::pi * (double) (i - from) / (double) (to - from));
            const double s0 = x[i] * win + c * s1 - s2; s2 = s1; s1 = s0;
        }
        const double power = s1 * s1 + s2 * s2 - c * s1 * s2;
        return 20.0 * std::log10 (std::sqrt (power) * 4.0 / (double) (to - from) + 1e-12);
    }

    double peakDb (const std::vector<float>& x, size_t from = 0)
    {
        float m = 0; for (size_t i = from; i < x.size(); ++i) m = std::max (m, std::abs (x[i]));
        return 20.0 * std::log10 (m + 1e-12);
    }

    // Crude but useful voice: vibrato, harmonics with a formant tilt, syllables, and occasional "s" noise.
    struct Voice
    {
        double f0, phase = 0.0;
        bool sustained;   // singing: long notes; speech: syllables + pitch glides
        std::mt19937 rng { 7 };
        std::normal_distribution<float> noise { 0.0f, 1.0f };
        dsp::SvfCoeffs sCoeffs; dsp::SvfState sState;
        float level;

        Voice (double baseF0, bool sing, float levelDb) : f0 (baseF0), sustained (sing), level (dsp::dbToGain (levelDb))
        {
            sCoeffs.bandPass (7000.0, 1.5, fs);
        }

        float operator() (int64_t n)
        {
            const double t = (double) n / fs;
            double f = f0;
            double env;
            if (sustained)
            {
                const int note = (int) (t / 1.6);                         // a new note every 1.6 s
                const double semis[] { 0, 2, 4, 5, 7, 5, 4, 2 };
                f = f0 * std::pow (2.0, semis[note % 8] / 12.0) * (1.0 + 0.012 * std::sin (2.0 * juce::MathConstants<double>::pi * 5.5 * t));
                const double tn = std::fmod (t, 1.6);
                env = std::min (1.0, tn / 0.08) * (tn > 1.45 ? std::max (0.0, (1.6 - tn) / 0.15) : 1.0);
            }
            else
            {
                f = f0 * (1.0 + 0.15 * std::sin (2.0 * juce::MathConstants<double>::pi * 0.7 * t) + 0.02 * std::sin (2.0 * juce::MathConstants<double>::pi * 4.0 * t));
                env = std::max (0.0, std::sin (2.0 * juce::MathConstants<double>::pi * 3.3 * t)) * (std::fmod (t, 4.0) < 3.4 ? 1.0 : 0.0);
            }
            phase += f / fs;
            double v = 0.0;
            for (int h = 1; h <= 30; ++h)
            {
                const double fh = f * h;
                if (fh > 9000.0) break;
                const double formant = 1.0 + 1.5 * std::exp (-std::pow ((fh - 600.0) / 250.0, 2.0)) + 1.0 * std::exp (-std::pow ((fh - 2400.0) / 500.0, 2.0));
                v += formant / h * std::sin (2.0 * juce::MathConstants<double>::pi * h * phase);
            }
            float out = (float) (v * env * 0.25);
            // sibilant bursts in speech
            if (! sustained && std::fmod (t, 1.9) < 0.12)
                out += 0.6f * sState.process (noise (rng), sCoeffs);
            return out * level;
        }
    };

    // Closed loop: mic = voice + G * room(speakerOut delayed). Room has two resonances.
    struct Room
    {
        std::vector<float> history;
        int64_t delay;
        dsp::SvfCoeffs r1, r2; dsp::SvfState s1, s2;
        float g;
        explicit Room (double loopGainDbAtResonance)
            : history (1 << 16, 0.0f), delay ((int64_t) (0.0123 * fs))
        {
            r1.bell (2512.0, 6.0, 9.0, fs);
            r2.bell (840.0, 5.0, 8.0, fs);
            g = dsp::dbToGain ((float) loopGainDbAtResonance - 9.0f);   // unity at resonance + margin
        }
        float feedback (int64_t n)
        {
            const float d = n >= delay ? history[(size_t) ((n - delay) & 0xffff)] : 0.0f;
            return g * s2.process (s1.process (d, r1), r2);
        }
        void store (int64_t n, float y) { history[(size_t) (n & 0xffff)] = y; }
    };

    std::vector<float> runClosedLoop (Harness& h, Voice& v, Room& room, double seconds)
    {
        // block-wise loop; room delay (12.3 ms) is longer than one block, so it is causal
        const int64_t total = (int64_t) (seconds * fs);
        std::vector<float> out ((size_t) total);
        for (int64_t start = 0; start < total; start += blockSize)
        {
            const int n = (int) std::min<int64_t> (blockSize, total - start);
            h.buf.setSize (1, n, false, false, true);
            for (int i = 0; i < n; ++i) h.buf.setSample (0, i, v (start + i) + room.feedback (start + i));
            h.p.processBlock (h.buf, h.midi);
            h.p.detector.processPendingForTest();
            for (int i = 0; i < n; ++i) { out[(size_t) (start + i)] = h.buf.getSample (0, i); room.store (start + i, h.buf.getSample (0, i)); }
        }
        return out;
    }

    juce::String notchList (const std::vector<dsp::FeedbackSuppressor::NotchInfo>& ns)
    {
        juce::String s;
        for (auto& n : ns) s << juce::String (n.freq, 0) << "Hz/" << juce::String (n.targetDb, 0) << (n.kind == 2 ? "F " : "f ");
        return s.isEmpty() ? "(none)" : s;
    }

    bool hasNotchNear (const std::vector<dsp::FeedbackSuppressor::NotchInfo>& ns, double f, double oct)
    {
        for (auto& n : ns) if (std::abs (std::log2 (n.freq / f)) < oct) return true;
        return false;
    }
}

static void testLatency()
{
    std::cout << "\nLatency" << std::endl;
    Harness h;
    check (h.p.getLatencySamples() == 0, "reported latency is 0 samples");
    h.allModulesOff();
    auto out = h.run ([] (int64_t n) { return n == 0 ? 1.0f : 0.0f; }, 0.05);
    check (std::abs (out[0] - 1.0f) < 1e-4f, "modules off: impulse passes at sample 0, unchanged");

    Harness h2;   // defaults: HP filter, de-esser, limiter on
    h2.set (fbOn, 1);
    auto out2 = h2.run ([] (int64_t n) { return n == 0 ? 0.5f : 0.0f; }, 0.05);
    size_t peakIdx = 0;
    for (size_t i = 0; i < out2.size(); ++i) if (std::abs (out2[i]) > std::abs (out2[peakIdx])) peakIdx = i;
    check (peakIdx == 0, "all modules on: impulse response peaks at sample 0 (no added delay)");
}

static void testClosedLoop()
{
    std::cout << "\nFeedback suppression (simulated mic -> speaker -> room loop, +1.5 dB loop gain at 840 Hz and 2.5 kHz)" << std::endl;

    for (int mode : { 0, 1, 2 })
    {
        const char* names[] { "Speech", "Singing", "Multipurpose" };
        // reference: suppression off -> runaway
        double runawayDb = 0.0;
        {
            Harness h; h.set (fbOn, 0);
            Voice v (mode == 1 ? 196.0 : 120.0, mode == 1, -24.0f);
            Room room (1.5);
            auto out = runClosedLoop (h, v, room, 8.0);
            runawayDb = rmsDb (out, out.size() - (size_t) fs * 2, out.size());
        }

        Harness h; h.set (fbOn, 1); h.set (fbMode, (float) mode);
        Voice v (mode == 1 ? 196.0 : 120.0, mode == 1, -24.0f);
        Room room (1.5);
        auto out = runClosedLoop (h, v, room, 8.0);
        const double finalDb = rmsDb (out, out.size() - (size_t) fs * 2, out.size());
        const auto ns = h.notches();
        std::cout << "  [" << names[mode] << "] no suppression: " << juce::String (runawayDb, 1) << " dBFS RMS,  with: "
                  << juce::String (finalDb, 1) << " dBFS RMS,  notches: " << notchList (ns) << std::endl;
        {
            juce::String trace ("    ring level over time (818 Hz / 2.52 kHz, dBFS):");
            for (double t0 = 0.0; t0 < 8.0; t0 += 1.0)
                trace << "  " << juce::String (toneDb (out, (size_t) (t0 * fs), (size_t) ((t0 + 1.0) * fs), 818.6), 0)
                      << "/" << juce::String (toneDb (out, (size_t) (t0 * fs), (size_t) ((t0 + 1.0) * fs), 2518.0), 0);
            std::cout << trace << std::endl;
        }
        check (runawayDb > -6.0, juce::String (names[mode]) + ": test rig really feeds back without suppression");
        check (finalDb < runawayDb - 10.0, juce::String (names[mode]) + ": suppression stops the runaway (>10 dB lower)");
        const double ringLate = std::max (toneDb (out, out.size() - (size_t) fs * 3, out.size(), 818.6), toneDb (out, out.size() - (size_t) fs * 3, out.size(), 2518.0));
        check (hasNotchNear (ns, 2512.0, 0.25), juce::String (names[mode]) + ": notch lands on the 2.5 kHz ring");
        check (ringLate < -50.0, juce::String (names[mode]) + ": no ring left in the last 3 s (" + juce::String (ringLate, 1) + " dBFS)");
        check (ns.size() <= 6, juce::String (names[mode]) + ": uses few notches (" + juce::String ((int) ns.size()) + ")");
    }
}

static void testFalsePositives()
{
    std::cout << "\nFalse positives (voice only, no feedback path, 30 s)" << std::endl;
    for (int mode : { 0, 1, 2 })
        for (bool singing : { false, true })
        {
            Harness h; h.set (fbOn, 1); h.set (fbMode, (float) mode);
            Voice v (singing ? 220.0 : 110.0, singing, -12.0f);
            h.run ([&v] (int64_t n) { return v (n); }, 30.0);
            const int detections = h.p.detector.getDetectionCount();
            const char* names[] { "Speech", "Singing", "Multipurpose" };
            std::cout << "  mode " << names[mode] << ", " << (singing ? "sung notes" : "speech") << ": " << detections
                      << " detections, notches " << notchList (h.notches()) << std::endl;
            const int allowed = (mode == 0 && singing) ? 6 : 1;   // speech mode isn't meant for singing
            check (detections <= allowed, juce::String ("mode ") + names[mode] + " / " + (singing ? "singing" : "speech")
                                           + ": <= " + juce::String (allowed) + " false detections");
        }
}

static void testLearnAndState()
{
    std::cout << "\nRing-out Learn + session recall" << std::endl;
    juce::MemoryBlock state;
    {
        Harness h; h.set (fbOn, 1); h.set (fbLearn, 1);
        Voice v (120.0, false, -60.0f);
        Room room (1.0);
        runClosedLoop (h, v, room, 6.0);
        const auto ns = h.notches();
        int fixed = 0; for (auto& n : ns) fixed += n.kind == dsp::FeedbackSuppressor::kindFixed;
        std::cout << "  learned: " << notchList (ns) << std::endl;
        check (fixed >= 1, "Learn mode creates fixed notches");
        h.p.getStateInformation (state);
    }
    {
        Harness h;
        h.p.setStateInformation (state.getData(), (int) state.getSize());
        h.run ([] (int64_t) { return 0.0f; }, 0.1);
        const auto ns = h.notches();
        int fixed = 0; for (auto& n : ns) fixed += n.kind == dsp::FeedbackSuppressor::kindFixed;
        std::cout << "  restored: " << notchList (ns) << std::endl;
        check (fixed >= 1, "fixed notches are restored from the saved session");
        check (h.p.apvts.getRawParameterValue (fbLearn)->load() > 0.5f, "parameters restored (Learn still on in that session)");
    }
    {
        Harness h; h.set (fbOn, 1); h.set (fbMaxFloat, 6); h.set (fbHold, 2.0f);
        Voice v (120.0, false, -60.0f);
        Room room (1.5);
        runClosedLoop (h, v, room, 5.0);
        const size_t during = h.notches().size();
        h.run ([] (int64_t) { return 0.0f; }, 5.0);   // feedback path gone (mic muted)
        std::cout << "  floating notches during feedback: " << during << ", 5 s after it stops: " << h.notches().size() << std::endl;
        check (during >= 1 && h.notches().empty(), "floating notches release after the hold time");
    }
}

static void testDynamicEq()
{
    std::cout << "\nDynamic EQ" << std::endl;
    auto measure = [] (float inDb, bool dyn)
    {
        Harness h; h.allModulesOff(); h.set (eqOn, 1);
        h.set (bOn (0), 0);
        h.set (bOn (2), 1); h.set (bFreq (2), 1000.0f); h.set (bGain (2), 0.0f); h.set (bQ (2), 1.0f);
        h.set (bDyn (2), dyn ? 1.0f : 0.0f); h.set (bThr (2), -30.0f); h.set (bRange (2), -6.0f); h.set (bRatio (2), 10.0f);
        const float a = dsp::dbToGain (inDb);
        auto out = h.run ([a] (int64_t n) { return a * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 1000.0 * (double) n / fs); }, 1.0);
        return toneDb (out, out.size() - 9600, out.size(), 1000.0) - inDb;
    };
    const double loud = measure (-10.0f, true), quiet = measure (-45.0f, true), staticBand = measure (-10.0f, false);
    std::cout << "  1 kHz band, thr -30, range -6:  loud input change " << juce::String (loud, 2) << " dB, quiet input change "
              << juce::String (quiet, 2) << " dB" << std::endl;
    check (std::abs (loud + 6.0) < 0.6, "loud signal is cut by the full range (-6 dB)");
    check (std::abs (quiet) < 0.3, "signal below threshold is untouched");
    check (std::abs (staticBand) < 0.3, "dynamic off + 0 dB gain = transparent");

    // static response matches what the UI draws
    Harness h; h.allModulesOff(); h.set (eqOn, 1); h.set (bOn (0), 0);
    h.set (bOn (3), 1); h.set (bFreq (3), 2000.0f); h.set (bGain (3), 6.0f); h.set (bQ (3), 2.0f);
    auto out = h.run ([] (int64_t n) { return 0.1f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 2500.0 * (double) n / fs); }, 0.5);
    dsp::SvfCoeffs c; c.bell (2000.0, 2.0, 6.0, fs);
    const double measured = toneDb (out, out.size() - 9600, out.size(), 2500.0) + 20.0;
    std::cout << "  bell 2 kHz +6 dB Q2 at 2.5 kHz: measured " << juce::String (measured, 2) << " dB, drawn " << juce::String (c.magnitudeDb (2500.0, fs), 2) << " dB" << std::endl;
    check (std::abs (measured - c.magnitudeDb (2500.0, fs)) < 0.2, "drawn EQ curve matches the audio");
}

static void testDeEsser()
{
    std::cout << "\nDe-esser" << std::endl;
    auto change = [] (double freq, int mode)
    {
        Harness h; h.allModulesOff(); h.set (dsOn, 1); h.set (dsMode, (float) mode);
        h.set (dsFreq, 6000.0f); h.set (dsThr, -30.0f); h.set (dsRange, 8.0f);
        auto out = h.run ([freq] (int64_t n) { return 0.3f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * freq * (double) n / fs); }, 1.0);
        return toneDb (out, out.size() - 9600, out.size(), freq) - dsp::gainToDb (0.3f);
    };
    const double s8k = change (8000.0, 0), lowSplit = change (300.0, 0), s8kWide = change (8000.0, 1);
    std::cout << "  split: 8 kHz " << juce::String (s8k, 2) << " dB, 300 Hz " << juce::String (lowSplit, 2)
              << " dB;  wideband: 8 kHz " << juce::String (s8kWide, 2) << " dB" << std::endl;
    check (s8k < -5.0, "split mode reduces sibilant band");
    check (std::abs (lowSplit) < 0.3, "split mode leaves the body of the voice alone");
    check (s8kWide < -5.0, "wideband mode reduces too");
}

static void testLimiter()
{
    std::cout << "\nSafety limiter" << std::endl;
    Harness h; h.allModulesOff(); h.set (limitOn, 1); h.set (ceiling, -1.0f);
    auto out = h.run ([] (int64_t n) { return 2.0f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 440.0 * (double) n / fs); }, 0.5);
    std::cout << "  +6 dBFS input -> output peak " << juce::String (peakDb (out), 2) << " dBFS" << std::endl;
    check (peakDb (out) <= -0.99, "output never exceeds the ceiling");
}

static void testPresets()
{
    std::cout << "\nPresets" << std::endl;
    Harness h;
    auto names = h.p.presets.getFactoryNames();
    std::cout << "  factory: " << names.joinIntoString (" | ") << std::endl;
    h.set (inGain, 5.0f);
    bool ok = true;
    for (int i = 0; i < names.size(); ++i) ok &= h.p.presets.loadFactory (i);
    check (ok, "all factory presets load");
    check (std::abs (h.p.apvts.getRawParameterValue (inGain)->load() - 5.0f) < 0.01f, "loading a preset leaves input gain alone");

    h.p.presets.loadFactory (1);
    h.set (bGain (3), 4.5f);
    const juce::String name = "Test Preset (auto)";
    check (h.p.presets.saveUser (name), "user preset saved to " + PresetManager::getUserPresetDirectory().getFullPathName());
    h.p.presets.loadFactory (0);
    check (h.p.presets.loadUser (name) && std::abs (h.p.apvts.getRawParameterValue (bGain (3))->load() - 4.5f) < 0.01f, "user preset reloads its values");
    check (h.p.presets.deleteUser (name) && ! h.p.presets.getUserNames().contains (name), "user preset deleted");
}

static void testPerformance()
{
    std::cout << "\nCPU" << std::endl;
    Harness h;
    h.set (fbOn, 1); h.set (eqOn, 1);
    for (int b = 0; b < numEqBands; ++b) { h.set (bOn (b), 1); h.set (bDyn (b), 1); }
    Voice v (120.0, false, -18.0f);
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    h.run ([&v] (int64_t n) { return v (n); }, 20.0);
    const double ms = juce::Time::getMillisecondCounterHiRes() - t0;
    std::cout << "  20 s of audio (all bands dynamic, detector inline, incl. test signal synthesis): " << juce::String (ms, 0) << " ms  ("
              << juce::String (ms / 200.0, 2) << "% of real time)" << std::endl;
    check (ms < 2000.0, "well under real time");
}

static int snapshot (const juce::String& path)
{
    juce::ScopedJuceInitialiser_GUI gui;
    Harness h;
    h.set (fbOn, 1); h.set (eqOn, 1);
    h.p.presets.loadFactory (1);
    h.set (fbMode, 0);
    Voice v (120.0, false, -20.0f);
    Room room (1.2);
    runClosedLoop (h, v, room, 6.0);

    std::unique_ptr<LiveVoiceEditor> ed (dynamic_cast<LiveVoiceEditor*> (h.p.createEditor()));
    ed->setSize (1280, 860);
    // keep audio running so the RTA and histogram fill in
    int64_t pos = (int64_t) (6.0 * fs);
    for (int frame = 0; frame < 120; ++frame)
    {
        for (int b = 0; b < 6; ++b)
        {
            h.buf.setSize (1, blockSize, false, false, true);
            for (int i = 0; i < blockSize; ++i) h.buf.setSample (0, i, v (pos + i) + room.feedback (pos + i));
            h.p.processBlock (h.buf, h.midi);
            h.p.detector.processPendingForTest();
            for (int i = 0; i < blockSize; ++i) room.store (pos + i, h.buf.getSample (0, i));
            pos += blockSize;
        }
        ed->refreshNow();
    }
    auto img = ed->createComponentSnapshot (ed->getLocalBounds(), true, 1.0f);
    juce::File f (path);
    f.deleteFile();
    juce::FileOutputStream os (f);
    juce::PNGImageFormat png;
    const bool ok = png.writeImageToStream (img, os);
    std::cout << (ok ? "wrote " : "FAILED ") << f.getFullPathName() << std::endl;
    ed.reset();
    return ok ? 0 : 1;
}

int main (int argc, char** argv)
{
    if (argc >= 3 && juce::String (argv[1]) == "--snapshot")
        return snapshot (argv[2]);

    juce::ScopedJuceInitialiser_GUI gui;   // parameters/attachments need a message manager
    testLatency();
    testDynamicEq();
    testDeEsser();
    testLimiter();
    testPresets();
    testClosedLoop();
    testFalsePositives();
    testLearnAndState();
    testPerformance();

    std::cout << "\n" << (failures == 0 ? "ALL TESTS PASSED" : juce::String (failures) + " TEST(S) FAILED") << std::endl;
    return failures == 0 ? 0 : 1;
}
