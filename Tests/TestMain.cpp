// Offline tests for LiveVoice. Run:  LiveVoiceTests            (DSP tests)
//                                    LiveVoiceTests --snapshot out.png
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "dsp/VoiceAnalysis.h"
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

        explicit Harness (bool withMonitor = false)
        {
            juce::AudioProcessor::BusesLayout l;
            l.inputBuses.add (juce::AudioChannelSet::mono());
            l.outputBuses.add (withMonitor ? juce::AudioChannelSet::stereo() : juce::AudioChannelSet::mono());
            p.setBusesLayout (l);
            p.setRateAndBufferSizeDetails (fs, blockSize);
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
            set (fbOn, 0); set (eqOn, 0); set (dsOn, 0); set (pseOn, 0); set (compOn, 0);
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
            for (int i = 0; i < n; ++i)
            {
                // the power amp clips at full scale (there is no limiter in the plugin), so a runaway stays finite
                const float y = juce::jlimit (-1.0f, 1.0f, h.buf.getSample (0, i));
                out[(size_t) (start + i)] = y;
                room.store (start + i, y);
            }
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

static double sineChange (Harness& h, double freq, float inDb, double seconds);

static void testLatency()
{
    std::cout << "\nLatency" << std::endl;
    Harness h;
    check (h.p.getLatencySamples() == 0, "reported latency is 0 samples");
    h.allModulesOff();
    auto out = h.run ([] (int64_t n) { return n == 0 ? 1.0f : 0.0f; }, 0.05);
    check (std::abs (out[0] - 1.0f) < 1e-4f, "modules off: impulse passes at sample 0, unchanged");

    Harness h2;   // defaults: HP filter, de-esser on + everything else switched on
    h2.set (fbOn, 1); h2.set (pseOn, 1); h2.set (pseThr, -80.0f); h2.set (compOn, 1); h2.set (dsRelative, 1);
    h2.set (lvlOn, 1); h2.set (proxOn, 1); h2.set (warmth, 50.0f);
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
    auto measure = [] (float inDb, bool dyn, float ratio = 10.0f, float range = -6.0f)
    {
        Harness h; h.allModulesOff(); h.set (eqOn, 1);
        h.set (bOn (0), 0);
        h.set (bOn (2), 1); h.set (bFreq (2), 1000.0f); h.set (bGain (2), 0.0f); h.set (bQ (2), 1.0f);
        h.set (bDyn (2), dyn ? 1.0f : 0.0f); h.set (bThr (2), -30.0f); h.set (bRange (2), range); h.set (bRatio (2), ratio);
        const float a = dsp::dbToGain (inDb);
        auto out = h.run ([a] (int64_t n) { return a * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 1000.0 * (double) n / fs); }, 1.0);
        return toneDb (out, out.size() - 9600, out.size(), 1000.0) - inDb;
    };
    const double loud = measure (-10.0f, true), quiet = measure (-45.0f, true), staticBand = measure (-10.0f, false);
    std::cout << "  1 kHz band, thr -30, range -6, 10:1:  loud input change " << juce::String (loud, 2) << " dB, quiet input change "
              << juce::String (quiet, 2) << " dB" << std::endl;
    check (std::abs (loud + 6.0) < 0.6, "loud signal is cut by (almost) the full range");
    check (std::abs (quiet) < 0.3, "signal below threshold is untouched");
    check (std::abs (staticBand) < 0.3, "dynamic off + 0 dB gain = transparent");

    // proportional: sweep the band level past the threshold, gain should follow gradually
    {
        dsp::DynamicEq::BandSettings bs; bs.thresholdDb = -30.0f; bs.ratio = 2.0f; bs.rangeDb = -12.0f;
        juce::String row ("  level -> cut (2:1, range -12):");
        double prev = 1.0, maxStep = 0.0, maxErr = 0.0;
        bool monotonic = true;
        for (float lvl = -42.0f; lvl <= -6.0f; lvl += 4.0f)
        {
            const double c = measure (lvl, true, 2.0f, -12.0f);
            const double expected = dsp::DynamicEq::dynamicGainFor (lvl, bs);
            row << "  " << juce::String ((int) lvl) << ":" << juce::String (c, 1);
            if (c > prev + 0.05) monotonic = false;
            if (prev <= 0.5) maxStep = std::max (maxStep, prev - c);
            maxErr = std::max (maxErr, std::abs (c - expected));
            prev = c;
        }
        std::cout << row << std::endl;
        std::cout << "  largest change per 4 dB of input: " << juce::String (maxStep, 2) << " dB; worst error vs curve: " << juce::String (maxErr, 2) << " dB" << std::endl;
        check (monotonic, "cut grows steadily as the band gets louder (no jumps back)");
        check (maxStep < 2.6, "gradual: never more than ~2.5 dB of extra cut per 4 dB louder (no on/off switching)");
        check (maxErr < 0.6, "matches the proportional curve shown in the UI");
    }

    // static response matches what the UI draws
    Harness h; h.allModulesOff(); h.set (eqOn, 1); h.set (bOn (0), 0);
    h.set (bOn (3), 1); h.set (bFreq (3), 2000.0f); h.set (bGain (3), 6.0f); h.set (bQ (3), 2.0f);
    auto out = h.run ([] (int64_t n) { return 0.1f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 2500.0 * (double) n / fs); }, 0.5);
    dsp::SvfCoeffs c; c.bell (2000.0, 2.0, 6.0, fs);
    const double measured = toneDb (out, out.size() - 9600, out.size(), 2500.0) + 20.0;
    std::cout << "  bell 2 kHz +6 dB Q2 at 2.5 kHz: measured " << juce::String (measured, 2) << " dB, drawn " << juce::String (c.magnitudeDb (2500.0, fs), 2) << " dB" << std::endl;
    check (std::abs (measured - c.magnitudeDb (2500.0, fs)) < 0.2, "drawn EQ curve matches the audio");

    // high-pass slopes, one octave below a 200 Hz corner (Butterworth: -10log10(1 + 4^n))
    juce::String slopes ("  HP at 200 Hz, level at 100 Hz:");
    bool slopesOk = true;
    for (int choice = 2; choice <= 7; ++choice)
    {
        const int dbOct = params::slopeDbPerOct[choice - 2];
        Harness hs; hs.allModulesOff(); hs.set (eqOn, 1);
        for (int b = 1; b < numEqBands; ++b) hs.set (bOn (b), 0);
        hs.set (bOn (0), 1); hs.set (bType (0), (float) choice); hs.set (bFreq (0), 200.0f); hs.set (bQ (0), 0.71f);
        const double got = sineChange (hs, 100.0, -6.0f, 0.6);
        const double order = dbOct / 6.0;
        const double expected = -10.0 * std::log10 (1.0 + std::pow (4.0, order));
        slopes << "  " << dbOct << "dB/oct:" << juce::String (got, 1);
        slopesOk &= std::abs (got - expected) < (dbOct == 12 ? 0.8 : 0.5);
    }
    std::cout << slopes << std::endl;
    check (slopesOk, "6/12/18/24/36/48 dB/oct slopes measure as designed");
}

static void testSolo()
{
    std::cout << "\nSolo / listen (fallback: solo to main output)" << std::endl;
    auto level = [] (Harness& h, double freq)
    {
        return sineChange (h, freq, -12.0f, 0.5);
    };
    double in450, out3k, ds7k, ds300;
    {
        Harness h; h.allModulesOff(); h.set (eqOn, 1); h.p.soloToMain = true; h.p.soloTarget = 2;   // band 3 = 450 Hz box band
        in450 = level (h, 450.0);
    }
    {
        Harness h; h.allModulesOff(); h.set (eqOn, 1); h.p.soloToMain = true; h.p.soloTarget = 2;
        out3k = level (h, 3000.0);
    }
    { Harness h; h.allModulesOff(); h.set (dsFreq, 7000.0f); h.p.soloToMain = true; h.p.soloTarget = LiveVoiceProcessor::soloDeEss; ds7k = level (h, 7000.0); }
    { Harness h; h.allModulesOff(); h.set (dsFreq, 7000.0f); h.p.soloToMain = true; h.p.soloTarget = LiveVoiceProcessor::soloDeEss; ds300 = level (h, 300.0); }
    std::cout << "  EQ band 3 solo: 450 Hz " << juce::String (in450, 1) << " dB, 3 kHz " << juce::String (out3k, 1)
              << " dB;  de-ess solo (7 kHz): 7 kHz " << juce::String (ds7k, 1) << " dB, 300 Hz " << juce::String (ds300, 1) << " dB" << std::endl;
    check (std::abs (in450) < 0.5 && out3k < -15.0, "EQ solo passes only the band's region");
    check (std::abs (ds7k) < 0.5 && ds300 < -25.0, "de-ess solo plays only the sibilance band");

    Harness h; h.p.soloTarget = 1;
    juce::MemoryBlock state; h.p.getStateInformation (state);
    Harness h2; h2.p.setStateInformation (state.getData(), (int) state.getSize());
    check (h2.p.soloTarget.load() == LiveVoiceProcessor::soloNone, "solo is never recalled with a session");
}

// Runs a 2-channel buffer (ch1 = mic in / FOH out, ch2 = Monitor out); returns { main, monitor }.
static std::pair<std::vector<float>, std::vector<float>> runWithMonitor (Harness& h, const std::function<float (int64_t)>& input, double seconds)
{
    const int64_t total = (int64_t) (seconds * fs);
    std::vector<float> mainOut ((size_t) total), monOut ((size_t) total);
    juce::AudioBuffer<float> b (2, blockSize);
    for (int64_t start = 0; start < total; start += blockSize)
    {
        const int n = (int) std::min<int64_t> (blockSize, total - start);
        b.setSize (2, n, false, false, true);
        b.clear();
        for (int i = 0; i < n; ++i) b.setSample (0, i, input (start + i));
        h.p.processBlock (b, h.midi);
        for (int i = 0; i < n; ++i) { mainOut[(size_t) (start + i)] = b.getSample (0, i); monOut[(size_t) (start + i)] = b.getSample (1, i); }
    }
    return { mainOut, monOut };
}

static void testMonitorBus()
{
    std::cout << "\nMonitor output (solo in place, FOH untouched)" << std::endl;
    auto twoTones = [] (int64_t n)
    {
        const double t = (double) n / fs;
        return 0.2f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 450.0 * t)
             + 0.2f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 3000.0 * t);
    };
    auto setup = [] (Harness& h) { h.allModulesOff(); h.set (eqOn, 1); h.set (bGain (2), -4.0f); };

    Harness ref (true); setup (ref);
    const auto [refMain, refMon] = runWithMonitor (ref, twoTones, 1.0);
    check (ref.p.isMonitorBusActive() && ref.p.getTotalNumOutputChannels() == 2 && ref.p.getBusCount (false) == 1,
           "one output with 2 channels: channel 1 FOH, channel 2 Monitor");

    Harness h (true); setup (h); h.p.soloTarget = 2;   // solo band 3 (450 Hz)
    const auto [main, mon] = runWithMonitor (h, twoTones, 1.0);
    double maxDiff = 0.0;
    for (size_t i = 0; i < main.size(); ++i) maxDiff = std::max (maxDiff, (double) std::abs (main[i] - refMain[i]));
    const size_t a = main.size() - 9600, e = main.size();
    const double mon450 = toneDb (mon, a, e, 450.0), mon3k = toneDb (mon, a, e, 3000.0);
    const double main450 = toneDb (main, a, e, 450.0), main3k = toneDb (main, a, e, 3000.0);
    std::cout << "  soloing band 3: FOH max difference " << juce::String (maxDiff, 7) << ";  monitor 450 Hz " << juce::String (mon450, 1)
              << " dB, 3 kHz " << juce::String (mon3k, 1) << " dB;  FOH 450 Hz " << juce::String (main450, 1) << " dB, 3 kHz " << juce::String (main3k, 1) << " dB" << std::endl;
    check (maxDiff < 1.0e-6, "FOH output is bit-identical while soloing on the monitor");
    check (mon450 - mon3k > 15.0, "monitor carries only the soloed band");

    Harness d (true); setup (d); d.p.monitorDry = true;
    const auto [dMain, dMon] = runWithMonitor (d, twoTones, 0.5);
    double dryDiff = 0.0, fxDiff = 0.0;
    for (size_t i = 0; i < dMon.size(); ++i) { dryDiff = std::max (dryDiff, (double) std::abs (dMon[i] - twoTones ((int64_t) i))); fxDiff = std::max (fxDiff, (double) std::abs (dMain[i] - refMain[i])); }
    std::cout << "  monitor A/B on DRY: monitor vs input max diff " << juce::String (dryDiff, 7) << ", FOH change " << juce::String (fxDiff, 7) << std::endl;
    check (dryDiff < 1.0e-6 && fxDiff < 1.0e-6, "monitor A/B plays the untouched input while FOH stays processed");

    Harness f (true); setup (f);
    const auto [fMain, fMon] = runWithMonitor (f, twoTones, 0.5);
    double follow = 0.0;
    for (size_t i = 0; i < fMon.size(); ++i) follow = std::max (follow, (double) std::abs (fMon[i] - fMain[i]));
    check (follow < 1.0e-6, "with nothing soloed the monitor follows FOH");
}

static void testSuggest()
{
    std::cout << "\nSuggest (de-esser and EQ assistants)" << std::endl;
    // de-esser: the synthetic talker's "s" bursts are centred at 7 kHz
    {
        Voice v (120.0, false, -18.0f);
        std::vector<float> x ((size_t) fs * 20);
        for (size_t i = 0; i < x.size(); ++i) x[i] = v ((int64_t) i);
        const auto s = dsp::analysis::suggestDeEss (x.data(), (int) x.size(), fs);
        std::cout << "  de-ess: " << s.sibilantFrames << " sibilant frames -> " << juce::String (s.freq, 0) << " Hz, Q "
                  << juce::String (s.q, 2) << ", thr " << juce::String (s.thresholdDb, 1) << " dB / rel " << juce::String (s.relThresholdDb, 1) << " dB" << std::endl;
        check (s.ok && std::abs (std::log2 (s.freq / 7000.0f)) < 1.0f / 3.0f, "finds the sibilance frequency (within 1/3 octave of 7 kHz)");

        // and the suggested settings actually de-ess this voice by a sensible amount
        Harness h; h.allModulesOff(); h.set (dsOn, 1); h.set (dsMode, 0.0f); h.set (dsFreq, s.freq); h.set (dsQ, s.q); h.set (dsThr, s.thresholdDb);
        h.set (dsRelThr, s.relThresholdDb);
        float maxGr = 0.0f;
        for (size_t start = 0; start + blockSize <= x.size(); start += blockSize)
        {
            h.buf.setSize (1, blockSize, false, false, true);
            h.buf.copyFrom (0, 0, x.data() + start, blockSize);
            h.p.processBlock (h.buf, h.midi);
            maxGr = std::max (maxGr, h.p.deEsser.getCurrentGainReductionDb());
        }
        std::cout << "  with the suggestion applied: up to " << juce::String (maxGr, 1) << " dB of de-essing on the s sounds" << std::endl;
        check (maxGr > 2.0f && maxGr <= 8.01f, "suggested threshold produces real but moderate de-essing");

        std::vector<float> speechOnly ((size_t) fs * 20);
        for (size_t i = 0; i < speechOnly.size(); ++i)
            speechOnly[i] = 0.1f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 220.0 * (double) i / fs);
        check (! dsp::analysis::suggestDeEss (speechOnly.data(), (int) speechOnly.size(), fs).ok, "says so when there are no s sounds");
    }
    // EQ: same voice through a +8 dB resonance at 450 Hz ("boxy" mic / room)
    {
        Voice v (120.0, false, -18.0f);
        dsp::SvfCoeffs box; box.bell (450.0, 3.0, 8.0, fs);
        dsp::SvfState st;
        std::vector<float> x ((size_t) fs * 20);
        for (size_t i = 0; i < x.size(); ++i) x[i] = st.process (v ((int64_t) i), box);
        const auto s = dsp::analysis::suggestEq (x.data(), (int) x.size(), fs);
        juce::String row ("  EQ: HPF " + juce::String (s.hpFreq, 0) + " Hz;");
        for (auto& b : s.bands) row << "  " << juce::String (b.freq, 0) << " Hz " << juce::String (b.gainDb, 1) << " dB";
        row << ";  air " << juce::String (s.airGainDb, 1) << " dB";
        std::cout << row << std::endl;
        check (s.ok && s.bands[1].gainDb < -1.0f && std::abs (std::log2 (s.bands[1].freq / 450.0f)) < 1.0f / 3.0f,
               "finds the boxy build-up at 450 Hz and suggests a cut");
        bool gentle = s.hpFreq >= 60.0f && s.hpFreq <= 150.0f;
        for (auto& b : s.bands) gentle &= b.gainDb >= -4.01f && b.gainDb <= 0.0f;
        check (gentle, "suggestions stay gentle (cuts of 4 dB or less, HPF 60-150 Hz)");
    }
    // capture through the processor
    {
        Harness h; h.p.startCapture (LiveVoiceProcessor::CaptureTarget::deEss, 1.0);
        Voice v (120.0, false, -18.0f);
        h.run ([&v] (int64_t n) { return v (n); }, 1.2);
        const auto cap = h.p.takeCapture();
        check (cap.size() == (size_t) fs && std::any_of (cap.begin(), cap.end(), [] (float s) { return std::abs (s) > 1.0e-4f; }),
               "the plugin records the voice for Suggest without touching the audio");
    }
}

static void testUndoAndAB()
{
    std::cout << "\nUndo / redo and A/B settings" << std::endl;
    Harness h;
    auto gain3 = [&h] { return h.p.apvts.getRawParameterValue (bGain (2))->load(); };
    h.p.undoManager.beginNewTransaction();
    h.set (bGain (2), 3.0f);
    h.p.apvts.copyState();   // flush to the undo history (the editor's timer does this live)
    h.p.undoManager.beginNewTransaction();
    h.set (bGain (2), -5.0f);
    h.p.apvts.copyState();
    h.p.undoManager.undo();
    const float afterUndo = gain3();
    h.p.undoManager.redo();
    const float afterRedo = gain3();
    std::cout << "  band 3 gain: set +3, then -5;  undo -> " << juce::String (afterUndo, 1) << ", redo -> " << juce::String (afterRedo, 1) << std::endl;
    check (std::abs (afterUndo - 3.0f) < 0.05f && std::abs (afterRedo + 5.0f) < 0.05f, "undo and redo restore setting changes");

    Harness a;
    a.set (bGain (2), 2.0f);
    a.p.selectSlot (1);                   // B starts as a copy of A
    const float bStart = a.p.apvts.getRawParameterValue (bGain (2))->load();
    a.set (bGain (2), 6.0f);
    a.set (inGain, 4.0f);                 // gain staging is not part of A/B
    a.p.selectSlot (0);
    const float inA = a.p.apvts.getRawParameterValue (bGain (2))->load();
    a.p.selectSlot (1);
    const float inB = a.p.apvts.getRawParameterValue (bGain (2))->load();
    std::cout << "  A = +2 dB, B edited to +6 dB:  B started at " << juce::String (bStart, 1) << ", back to A -> " << juce::String (inA, 1)
              << ", B again -> " << juce::String (inB, 1) << std::endl;
    check (std::abs (bStart - 2.0f) < 0.05f && std::abs (inA - 2.0f) < 0.05f && std::abs (inB - 6.0f) < 0.05f, "A/B switches between two complete settings");
    check (std::abs (a.p.apvts.getRawParameterValue (inGain)->load() - 4.0f) < 0.05f, "A/B leaves input gain alone");

    juce::MemoryBlock state; a.p.getStateInformation (state);
    Harness r; r.p.setStateInformation (state.getData(), (int) state.getSize());
    r.p.selectSlot (0);
    check (r.p.getActiveSlot() == 0 && std::abs (r.p.apvts.getRawParameterValue (bGain (2))->load() - 2.0f) < 0.05f, "both A/B slots are saved with the session");
}

static void testDeEsser()
{
    std::cout << "\nDe-esser" << std::endl;
    auto change = [] (double freq, int mode)
    {
        Harness h; h.allModulesOff(); h.set (dsOn, 1); h.set (dsMode, (float) mode);
        h.set (dsFreq, 7000.0f); h.set (dsQ, 3.0f); h.set (dsThr, -30.0f); h.set (dsRange, 8.0f);
        auto out = h.run ([freq] (int64_t n) { return 0.3f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * freq * (double) n / fs); }, 1.0);
        return toneDb (out, out.size() - 9600, out.size(), freq) - dsp::gainToDb (0.3f);
    };
    const double band7k = change (7000.0, 0), band300 = change (300.0, 0), bandOctBelow = change (3500.0, 0);
    const double shelf12k = change (12000.0, 1), wide7k = change (7000.0, 2);
    std::cout << "  band (7 kHz, Q3): 7 kHz " << juce::String (band7k, 2) << " dB, 3.5 kHz " << juce::String (bandOctBelow, 2)
              << " dB, 300 Hz " << juce::String (band300, 2) << " dB;  shelf: 12 kHz " << juce::String (shelf12k, 2)
              << " dB;  wideband: 7 kHz " << juce::String (wide7k, 2) << " dB" << std::endl;
    check (band7k < -5.0, "band mode reduces the sibilance band");
    check (bandOctBelow > -1.0, "band mode is narrow: an octave below is barely touched");
    check (std::abs (band300) < 0.2, "band mode leaves the body of the voice alone");
    check (shelf12k < -5.0 && wide7k < -5.0, "shelf (all highs) and wideband modes reduce too");
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

static double sineChange (Harness& h, double freq, float inDb, double seconds)
{
    const float a = dsp::dbToGain (inDb);
    auto out = h.run ([a, freq] (int64_t n) { return a * (float) std::sin (2.0 * juce::MathConstants<double>::pi * freq * (double) n / fs); }, seconds);
    return toneDb (out, out.size() - 9600, out.size(), freq) - inDb;
}

static void testPse()
{
    std::cout << "\nPSE (primary source expander)" << std::endl;
    auto make = [] (Harness& h) { h.allModulesOff(); h.set (pseOn, 1); h.set (pseThr, -45.0f); h.set (pseRange, 12.0f); };
    double quiet, loud;
    { Harness h; make (h); quiet = sineChange (h, 1000.0, -70.0f, 2.0); }
    { Harness h; make (h); loud = sineChange (h, 1000.0, -20.0f, 1.0); }
    std::cout << "  idle mic (-70 dBFS): " << juce::String (quiet, 2) << " dB,  talking (-20 dBFS): " << juce::String (loud, 2) << " dB" << std::endl;
    check (std::abs (quiet + 12.0) < 0.5, "pulls an idle mic down by the full range");
    check (std::abs (loud) < 0.1, "leaves the talker untouched");

    // opening speed: phrase starts after silence
    Harness h; make (h);
    auto out = h.run ([] (int64_t n)
    {
        const float a = n < (int64_t) fs ? 0.0003f : 0.1f;   // ~-70 dBFS then -20 dBFS
        return a * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 300.0 * (double) n / fs);
    }, 1.2);
    // find the first 1 ms window after onset whose peak is within 1 dB of the input
    int openedAtMs = -1;
    for (int ms = 0; ms < 100 && openedAtMs < 0; ++ms)
    {
        float pk = 0.0f;
        for (int i = 0; i < 160; ++i) pk = std::max (pk, std::abs (out[(size_t) fs + (size_t) (ms * 48 + i)]));
        if (pk > 0.1f * 0.89f) openedAtMs = ms;
    }
    std::cout << "  opens to within 1 dB " << openedAtMs << " ms after the first word starts" << std::endl;
    check (openedAtMs >= 0 && openedAtMs <= 8, "opens fast enough not to clip first syllables");

    // stays open through short gaps between words (hold + hysteresis)
    Harness h2; make (h2); h2.set (pseHold, 150.0f);
    auto out2 = h2.run ([] (int64_t n)
    {
        const double t = (double) n / fs;
        const bool gap = std::fmod (t, 0.5) > 0.42;               // 80 ms gap every 0.5 s
        return (gap ? 0.0003f : 0.1f) * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 300.0 * t);
    }, 3.0);
    juce::ignoreUnused (out2);
    check (h2.p.pse.getGainReductionDb() < 1.0f, "does not chatter on short pauses between words");
}

static void testCompressor()
{
    std::cout << "\nVocal compressor" << std::endl;
    auto make = [] (Harness& h, float knee)
    {
        h.allModulesOff(); h.set (compOn, 1); h.set (compThr, -20.0f); h.set (compRatio, 4.0f);
        h.set (compKnee, knee); h.set (compMakeup, 0.0f); h.set (compAttack, 5.0f); h.set (compRelease, 200.0f);
    };
    double above, below, inKnee;
    { Harness h; make (h, 0.0f); above = sineChange (h, 1000.0, -4.0f, 1.0); }
    { Harness h; make (h, 10.0f); below = sineChange (h, 1000.0, -30.0f, 1.0); }
    { Harness h; make (h, 10.0f); inKnee = sineChange (h, 1000.0, -20.0f, 1.0); }
    std::cout << "  thr -20, 4:1:  16 dB over -> " << juce::String (above, 2) << " dB (expect -12),  10 dB under -> "
              << juce::String (below, 2) << " dB,  at threshold w/ 10 dB knee -> " << juce::String (inKnee, 2) << " dB (expect -0.94)" << std::endl;
    check (std::abs (above + 12.0) < 0.3, "ratio is accurate");
    check (std::abs (below) < 0.05, "transparent below the knee");
    check (std::abs (inKnee + 0.9375) < 0.25, "soft knee eases in gradually");

    // gain ripple on a sustained low note (a source of distortion in fast compressors)
    {
        Harness h; make (h, 10.0f); h.set (compScHp, 100.0f);
        const float a = dsp::dbToGain (-6.0f);
        auto out = h.run ([a] (int64_t n) { return a * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 150.0 * (double) n / fs); }, 1.5);
        // per-cycle peak variation over the last 0.5 s
        float lo = 10.0f, hi = 0.0f;
        const int period = 320;
        for (size_t start = out.size() - 24000; start + (size_t) period <= out.size(); start += (size_t) period)
        {
            float pk = 0.0f;
            for (int i = 0; i < period; ++i) pk = std::max (pk, std::abs (out[start + (size_t) i]));
            lo = std::min (lo, pk); hi = std::max (hi, pk);
        }
        const double ripple = 20.0 * std::log10 (hi / lo);
        std::cout << "  gain ripple on a 150 Hz held note: " << juce::String (ripple, 3) << " dB p-p" << std::endl;
        check (ripple < 0.15, "smooth: negligible gain ripple on low notes");
    }

    // program-dependent release: quick after a short peak, slower after sustained compression
    auto recoveryMs = [&] (double burstSeconds)
    {
        Harness h; make (h, 6.0f); h.set (compAuto, 1);
        const int64_t burstEnd = (int64_t) ((0.5 + burstSeconds) * fs);
        h.run ([burstEnd] (int64_t n)
        {
            const float a = (n >= (int64_t) (0.5 * fs) && n < burstEnd) ? 0.5f : 0.02f;
            return a * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 1000.0 * (double) n / fs);
        }, 0.5 + burstSeconds);
        const float grStart = h.p.comp.getGainReductionDb();
        int ms = 0;
        while (ms < 5000)
        {
            h.run ([] (int64_t n) { return 0.02f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 1000.0 * (double) n / fs); }, 0.005);
            ms += 5;
            if (h.p.comp.getGainReductionDb() < grStart * 0.25f) break;
        }
        return ms;
    };
    const int shortRec = recoveryMs (0.06), longRec = recoveryMs (2.5);
    std::cout << "  auto release: recovers from a 60 ms peak in " << shortRec << " ms, from 2.5 s of compression in " << longRec << " ms" << std::endl;
    check (longRec > shortRec * 1.5, "auto release is program dependent (opto-style)");

    { Harness h; make (h, 0.0f); h.set (compMix, 50.0f);
      const double half = sineChange (h, 1000.0, -4.0f, 1.0);
      const double expected = 20.0 * std::log10 (0.5 + 0.5 * std::pow (10.0, -12.0 / 20.0));
      std::cout << "  50% mix: " << juce::String (half, 2) << " dB (expect " << juce::String (expected, 2) << ")" << std::endl;
      check (std::abs (half - expected) < 0.3, "parallel mix blends correctly"); }
}

static void testRelativeDeEss()
{
    std::cout << "\nDe-esser relative detection" << std::endl;
    // an "s" on top of a voice = 300 Hz body + 7 kHz sibilance 3 dB louder, at a soft and a loud level
    auto run = [] (bool relative, float levelDb)
    {
        Harness h; h.allModulesOff(); h.set (dsOn, 1); h.set (dsRelative, relative ? 1.0f : 0.0f);
        h.set (dsThr, -30.0f); h.set (dsRange, 8.0f); h.set (dsFreq, 7000.0f);   // default relative threshold
        const float a = dsp::dbToGain (levelDb), b = dsp::dbToGain (levelDb + 3.0f);
        auto out = h.run ([a, b] (int64_t n)
        {
            const double t = (double) n / fs;
            return a * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 300.0 * t)
                 + b * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 7000.0 * t);
        }, 1.0);
        return toneDb (out, out.size() - 9600, out.size(), 7000.0) - (levelDb + 3.0f);
    };
    const double absSoft = run (false, -45.0f), absLoud = run (false, -12.0f);
    const double relSoft = run (true, -45.0f), relLoud = run (true, -12.0f);
    std::cout << "  sibilance reduction  absolute: soft " << juce::String (absSoft, 1) << " / loud " << juce::String (absLoud, 1)
              << " dB;   relative: soft " << juce::String (relSoft, 1) << " / loud " << juce::String (relLoud, 1) << " dB" << std::endl;
    check (std::abs (absSoft) < 0.3 && absLoud < -4.0, "absolute mode (relative off): only loud speech is de-essed");
    check (relSoft < -4.0 && std::abs (relSoft - relLoud) < 1.0, "relative mode: soft and loud speech treated the same");

    Harness h; h.allModulesOff(); h.set (dsOn, 1); h.set (dsRelative, 1);
    h.run ([] (int64_t n) { return 1.0e-4f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 7000.0 * (double) n / fs); }, 0.5);
    check (h.p.deEsser.getCurrentGainReductionDb() < 0.1f, "relative mode ignores near-silence (no reduction on noise)");
}

static void testLearnFalsePositives()
{
    std::cout << "\nRing-out Learn while someone talks into the mic (no feedback path, 20 s)" << std::endl;
    for (float lvl : { -40.0f, -20.0f })
    {
        Harness h; h.set (fbOn, 1); h.set (fbLearn, 1);
        Voice v (120.0, false, lvl);
        h.run ([&v] (int64_t n) { return v (n); }, 20.0);
        std::cout << "  voice at " << lvl << " dBFS: " << h.p.detector.getDetectionCount() << " detections " << notchList (h.notches()) << std::endl;
        check (h.p.detector.getDetectionCount() == 0, "Learn mode does not notch the voice (" + juce::String (lvl, 0) + " dBFS)");
    }
}

static void testPerformance()
{
    std::cout << "\nCPU" << std::endl;
    Harness h;
    h.set (fbOn, 1); h.set (eqOn, 1);
    for (int b = 0; b < numEqBands; ++b) { h.set (bOn (b), 1); h.set (bDyn (b), 1); }
    h.set (pseOn, 1); h.set (compOn, 1); h.set (dsRelative, 1);
    Voice v (120.0, false, -18.0f);
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    h.run ([&v] (int64_t n) { return v (n); }, 20.0);
    const double ms = juce::Time::getMillisecondCounterHiRes() - t0;
    std::cout << "  20 s of audio (all bands dynamic, detector inline, incl. test signal synthesis): " << juce::String (ms, 0) << " ms  ("
              << juce::String (ms / 200.0, 2) << "% of real time)" << std::endl;
    check (ms < 2000.0, "well under real time");
}

// Realistic-ish sibilance material: a voiced vowel (120 Hz, formants) plus broadband "s" bursts
// (noise band-limited 4-10 kHz, peaking ~6.5 kHz), roughly as loud as the vowel - a harsh talker.
struct SibilantTalker
{
    std::mt19937 rng { 3 };
    std::normal_distribution<float> nz { 0.0f, 1.0f };
    dsp::SvfCoeffs hp, lp, pk; dsp::SvfState s1, s2, s3;
    double phase = 0.0;
    float level;
    explicit SibilantTalker (float levelDb) : level (dsp::dbToGain (levelDb))
    {
        hp.highPass (4000.0, 0.707, fs); lp.lowPass (10000.0, 0.707, fs); pk.bell (6500.0, 1.2, 6.0, fs);
    }
    static bool isS (double t) { const double c = std::fmod (t, 0.8); return c > 0.55 && c < 0.70; }      // 150 ms "s" every 0.8 s
    static bool isVowel (double t) { const double c = std::fmod (t, 0.8); return c > 0.05 && c < 0.50; }
    float operator() (int64_t n)
    {
        const double t = (double) n / fs;
        float out = 0.0f;
        if (isVowel (t) || (std::fmod (t, 0.8) > 0.02 && std::fmod (t, 0.8) < 0.53))
        {
            phase += 120.0 / fs;
            double v = 0.0;
            for (int h = 1; h <= 60; ++h)
            {
                const double fh = 120.0 * h;
                if (fh > 9000.0) break;
                const double formant = 1.0 + 2.0 * std::exp (-std::pow ((fh - 650.0) / 250.0, 2.0)) + 1.2 * std::exp (-std::pow ((fh - 2600.0) / 600.0, 2.0));
                v += formant / h * std::sin (2.0 * juce::MathConstants<double>::pi * h * phase);
            }
            out += (float) (0.22 * v);
        }
        const float noise = s3.process (s2.process (s1.process (nz (rng), hp), lp), pk);
        if (isS (t)) out += 0.35f * noise;
        return out * level;
    }
};

static double bandEnergyDb (const std::vector<float>& x, double lo, double hi, const std::function<bool (double)>& when)
{
    dsp::SvfCoeffs a, b; a.highPass (lo, 0.707, fs); b.lowPass (hi, 0.707, fs);
    dsp::SvfState sa, sb, sc, sd;
    double e = 0.0; int64_t n = 0;
    for (size_t i = 0; i < x.size(); ++i)
    {
        const float y = sd.process (sc.process (sb.process (sa.process (x[i], a), a), b), b);
        if (i > (size_t) fs && when ((double) i / fs)) { e += (double) y * y; ++n; }
    }
    return 10.0 * std::log10 (e / std::max<int64_t> (1, n) + 1e-20);
}

struct DeEssResult { double sRed, vBody, vTop; };

static DeEssResult deEssReport (const char* name, const std::function<void (Harness&)> setup)
{
    auto render = [&] (bool on)
    {
        Harness h; h.allModulesOff(); h.set (dsOn, on ? 1.0f : 0.0f); setup (h);
        SibilantTalker v (-12.0f);
        return h.run ([&v] (int64_t n) { return v (n); }, 8.0);
    };
    const auto dry = render (false), wet = render (true);
    const double sRed = bandEnergyDb (wet, 4000, 10000, SibilantTalker::isS) - bandEnergyDb (dry, 4000, 10000, SibilantTalker::isS);
    const double vBody = bandEnergyDb (wet, 200, 3000, SibilantTalker::isVowel) - bandEnergyDb (dry, 200, 3000, SibilantTalker::isVowel);
    const double vTop = bandEnergyDb (wet, 3000, 9000, SibilantTalker::isVowel) - bandEnergyDb (dry, 3000, 9000, SibilantTalker::isVowel);
    std::cout << "  " << juce::String (name).paddedRight (' ', 44) << "s (4-10k): " << juce::String (sRed, 1).paddedLeft (' ', 6)
              << " dB   vowel body: " << juce::String (vBody, 2).paddedLeft (' ', 6) << " dB   vowel presence: " << juce::String (vTop, 2).paddedLeft (' ', 6) << " dB" << std::endl;
    return { sRed, vBody, vTop };
}

static int deEssProbe()
{
    juce::ScopedJuceInitialiser_GUI gui;
    std::cout << "De-esser on a harsh synthetic talker (s energy 4-10 kHz reduced; vowels should be untouched)" << std::endl;
    for (float thr : { -12.0f, -6.0f, 0.0f, 6.0f })
        deEssReport (("new defaults, rel thr " + juce::String (thr, 0)).toRawUTF8(), [thr] (Harness& h) { h.set (dsRelThr, thr); });
    deEssReport ("Q 2.5", [] (Harness& h) { h.set (dsQ, 2.5f); });
    deEssReport ("Q 1.4", [] (Harness& h) { h.set (dsQ, 1.4f); });
    deEssReport ("Q 0.7", [] (Harness& h) { h.set (dsQ, 0.7f); });
    deEssReport ("shelf", [] (Harness& h) { h.set (dsMode, 1.0f); });
    deEssReport ("wideband", [] (Harness& h) { h.set (dsMode, 2.0f); });
    deEssReport ("absolute, thr -30", [] (Harness& h) { h.set (dsRelative, 0); });
    deEssReport ("range 10", [] (Harness& h) { h.set (dsRange, 10.0f); });
    {
        SibilantTalker t (-12.0f);
        std::vector<float> x ((size_t) fs * 20);
        for (size_t i = 0; i < x.size(); ++i) x[i] = t ((int64_t) i);
        const auto sg = dsp::analysis::suggestDeEss (x.data(), (int) x.size(), fs);
        std::cout << "  Suggest on this talker: " << juce::String (sg.freq, 0) << " Hz, Q " << juce::String (sg.q, 2)
                  << ", rel thr " << juce::String (sg.relThresholdDb, 0) << " dB" << std::endl;
        deEssReport ("with the suggestion", [sg] (Harness& h) { h.set (dsFreq, sg.freq); h.set (dsQ, sg.q); h.set (dsRelThr, sg.relThresholdDb); });
    }
    return 0;
}

static double rmsDb (const std::vector<float>& x, double fromSec, double toSec)
{
    double e = 0.0; int64_t n = 0;
    for (size_t i = (size_t) (fromSec * fs); i < std::min (x.size(), (size_t) (toSec * fs)); ++i) { e += (double) x[i] * x[i]; ++n; }
    return 10.0 * std::log10 (e / std::max<int64_t> (1, n) + 1e-20);
}

static void testDeEssQualityAndLevels()
{
    std::cout << "\nDe-esser on a harsh talker, compressor auto gain, output level match" << std::endl;
    {
        const auto r = deEssReport ("default settings", [] (Harness&) {});
        check (r.sRed < -4.0, "de-esser takes the s sounds down by more than 4 dB at default settings");
        check (std::abs (r.vBody) < 0.5 && r.vTop > -1.5, "vowels are left alone (body and presence)");

        SibilantTalker t (-12.0f);
        std::vector<float> x ((size_t) fs * 20);
        for (size_t i = 0; i < x.size(); ++i) x[i] = t ((int64_t) i);
        const auto sg = dsp::analysis::suggestDeEss (x.data(), (int) x.size(), fs);
        const auto rs = deEssReport ("with Suggest", [sg] (Harness& h) { h.set (dsFreq, sg.freq); h.set (dsQ, sg.q); h.set (dsRelThr, sg.relThresholdDb); });
        check (sg.ok && rs.sRed < -4.0 && std::abs (rs.vBody) < 0.5, "Suggest on this talker de-esses well without touching vowels");
    }

    auto compRun = [] (bool autoGain)
    {
        Harness h; h.allModulesOff(); h.set (compOn, 1); h.set (compThr, -30.0f); h.set (compRatio, 4.0f);
        h.set (compMakeup, 0.0f); h.set (compAutoGain, autoGain ? 1.0f : 0.0f);
        Voice v (120.0, false, -12.0f);
        std::vector<float> in;
        auto out = h.run ([&v, &in] (int64_t n) { const float s = v (n); in.push_back (s); return s; }, 10.0);
        return rmsDb (out, 6.0, 10.0) - rmsDb (in, 6.0, 10.0);
    };
    const double plain = compRun (false), withAuto = compRun (true);
    std::cout << "  compressor thr -30, 4:1 on a voice: level change " << juce::String (plain, 1) << " dB, with auto gain "
              << juce::String (withAuto, 1) << " dB" << std::endl;
    check (plain < -3.0 && std::abs (withAuto) < 1.5, "auto gain keeps the compressed voice as loud as the input");

    {
        Harness h; h.allModulesOff(); h.set (eqOn, 1); h.set (bOn (2), 1); h.set (bGain (2), 6.0f); h.set (bDyn (2), 0);
        h.set (bFreq (2), 600.0f); h.set (bQ (2), 0.7f);
        Voice v (120.0, false, -18.0f);
        h.run ([&v] (int64_t n) { return v (n); }, 5.0);
        const float before = h.p.getOutputVsInputDb();
        const bool valid = h.p.levelMatchValid();
        h.p.matchOutputToInput();
        h.run ([&v] (int64_t n) { return v (n); }, 5.0);
        const float after = h.p.getOutputVsInputDb();
        std::cout << "  +6 dB EQ boost: output vs input " << juce::String (before, 1) << " dB; after MATCH " << juce::String (after, 1)
                  << " dB (OUTPUT " << juce::String (h.p.apvts.getRawParameterValue (outGain)->load(), 1) << " dB)" << std::endl;
        check (valid && before > 1.0f && std::abs (after) < 1.0f, "level indicator sees the boost and MATCH levels the output");
    }
}

static void testLevelTone()
{
    std::cout << "\nLeveler, proximity compensation, warmth" << std::endl;
    // leveler: a talker who alternates between soft and loud (16 dB apart) every 6 s
    {
        auto render = [] (bool on)
        {
            Harness h; h.allModulesOff(); h.set (lvlOn, on ? 1.0f : 0.0f); h.set (lvlTarget, -36.0f); h.set (lvlRange, 6.0f);
            Voice soft (120.0, false, -26.0f), loud (120.0, false, -10.0f);
            return h.run ([&] (int64_t n) { return ((n / (int64_t) (6 * fs)) % 2 == 0) ? soft (n) : loud (n); }, 24.0);
        };
        const auto dry = render (false), wet = render (true);
        const double inSpread = rmsDb (dry, 21.0, 24.0) - rmsDb (dry, 15.0, 18.0);
        const double outSpread = rmsDb (wet, 21.0, 24.0) - rmsDb (wet, 15.0, 18.0);
        std::cout << "  soft/loud talker: level difference " << juce::String (inSpread, 1) << " dB in, " << juce::String (outSpread, 1)
                  << " dB with the leveler (range 6)" << std::endl;
        check (outSpread < inSpread - 8.0, "leveler evens out a soft and a loud talker");
    }
    {
        Harness h; h.allModulesOff(); h.set (lvlOn, 1); h.set (lvlTarget, -36.0f); h.set (lvlRange, 6.0f);
        Voice v (120.0, false, -26.0f);
        h.run ([&v] (int64_t n) { return v (n); }, 8.0);
        const float before = h.p.leveler.getGainDb();
        std::mt19937 rng (5); std::normal_distribution<float> nz (0.0f, 1.0f);
        h.run ([&] (int64_t) { return 3.0e-4f * nz (rng); }, 6.0);   // -70 dBFS room noise between phrases
        const float after = h.p.leveler.getGainDb();
        std::cout << "  leveler gain while talking +" << juce::String (before, 1) << " dB, after 6 s of silence +" << juce::String (after, 1) << " dB" << std::endl;
        check (before > 4.0f && std::abs (after - before) < 0.5f, "leveler holds between phrases (never raises the room)");
    }
    // proximity: 12 s at normal distance, then 10 s close (low end +9 dB below 200 Hz)
    {
        auto render = [] (bool on)
        {
            Harness h; h.allModulesOff(); h.set (proxOn, on ? 1.0f : 0.0f); h.set (proxAmount, 9.0f);
            Voice v (120.0, false, -18.0f);
            dsp::SvfCoeffs close; close.lowShelf (200.0, 0.707, 9.0, fs);
            dsp::SvfState st;
            return h.run ([&] (int64_t n)
            {
                const float x = v (n);
                const float y = st.process (x, close);
                return n < (int64_t) (12 * fs) ? x : y;
            }, 22.0);
        };
        const auto dry = render (false), wet = render (true);
        auto lf = [] (const std::vector<float>& x, double a, double b)
        {
            return bandEnergyDb (x, 80.0, 250.0, [a, b] (double t) { return t >= a && t < b; });
        };
        const double normal = lf (wet, 8.0, 12.0) - lf (dry, 8.0, 12.0);
        const double close = lf (wet, 17.0, 22.0) - lf (dry, 17.0, 22.0);
        std::cout << "  proximity: low end at normal distance " << juce::String (normal, 1) << " dB, when close "
                  << juce::String (close, 1) << " dB" << std::endl;
        check (std::abs (normal) < 1.0, "proximity: leaves the voice alone at its normal distance");
        check (close < -4.0, "proximity: takes the boom back out when the talker gets close");
    }
    // warmth
    {
        auto sine = [] (double f, float db) { const float a = dsp::dbToGain (db); return [f, a] (int64_t n) { return a * (float) std::sin (2.0 * juce::MathConstants<double>::pi * f * (double) n / fs); }; };
        Harness a; a.allModulesOff();
        Harness b; b.allModulesOff(); b.set (warmth, 0.0f);
        Voice v1 (120.0, false, -18.0f), v2 (120.0, false, -18.0f);
        const auto x1 = a.run ([&v1] (int64_t n) { return v1 (n); }, 1.0), x2 = b.run ([&v2] (int64_t n) { return v2 (n); }, 1.0);
        check (x1 == x2, "warmth at 0 % is an exact bypass");

        Harness w; w.allModulesOff(); w.set (warmth, 100.0f);
        const auto o = w.run (sine (200.0, -20.0f), 1.0);
        const double fund = toneDb (o, o.size() - 9600, o.size(), 200.0);
        const double h2 = toneDb (o, o.size() - 9600, o.size(), 400.0) - fund, h3 = toneDb (o, o.size() - 9600, o.size(), 600.0) - fund;
        Harness w2; w2.allModulesOff(); w2.set (warmth, 100.0f);
        const auto hi = w2.run (sine (7000.0, -20.0f), 1.0);
        const double hiChange = toneDb (hi, hi.size() - 9600, hi.size(), 7000.0) + 20.0;
        std::cout << "  warmth 100 % on 200 Hz: 2nd harmonic " << juce::String (h2, 1) << " dB, 3rd " << juce::String (h3, 1)
                  << " dB;  7 kHz changed by " << juce::String (hiChange, 2) << " dB" << std::endl;
        check (h2 > -30.0 && h2 < -14.0 && h3 < h2, "warmth adds a subtle 2nd harmonic (and less 3rd)");
        check (std::abs (hiChange) < 0.1, "warmth leaves the highs alone");
    }
}

static int profile()
{
    juce::ScopedJuceInitialiser_GUI gui;
    // ---- audio thread cost, per module, typical block ----
    auto timeIt = [] (const char* name, std::function<void (Harness&)> setup)
    {
        Harness h; setup (h);
        Voice v (120.0, false, -18.0f);
        std::vector<float> sig ((size_t) fs * 10);
        for (size_t i = 0; i < sig.size(); ++i) sig[i] = v ((int64_t) i);
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        for (size_t start = 0; start + blockSize <= sig.size(); start += blockSize)
        {
            h.buf.setSize (1, blockSize, false, false, true);
            h.buf.copyFrom (0, 0, sig.data() + start, blockSize);
            h.p.processBlock (h.buf, h.midi);
        }
        const double ms = juce::Time::getMillisecondCounterHiRes() - t0;
        std::cout << "  " << juce::String (name).paddedRight (' ', 34) << juce::String (ms / 100.0, 2) << "% of one core" << std::endl;
    };
    std::cout << "AUDIO (10 s of voice, 48 kHz, 256-sample blocks; detector thread excluded)" << std::endl;
    timeIt ("everything off", [] (Harness& h) { h.allModulesOff(); });
    timeIt ("PSE only", [] (Harness& h) { h.allModulesOff(); h.set (pseOn, 1); });
    timeIt ("feedback (24 notches active)", [] (Harness& h) { h.allModulesOff(); h.set (fbOn, 1);
        for (int i = 0; i < 24; ++i) { dsp::FeedbackSuppressor::Command c; c.type = dsp::FeedbackSuppressor::Command::Type::setFixed; c.freq = 300.0f + 300.0f * i; c.depthDb = -9.0f; h.p.sendNotchCommand (c); } });
    timeIt ("dynamic EQ, 6 bands dynamic, HP48", [] (Harness& h) { h.allModulesOff(); h.set (eqOn, 1); h.set (bType (0), 7.0f);
        for (int b = 0; b < 6; ++b) { h.set (bOn (b), 1); h.set (bDyn (b), b > 0 ? 1.0f : 0.0f); } });
    timeIt ("de-esser (relative)", [] (Harness& h) { h.allModulesOff(); h.set (dsOn, 1); h.set (dsRelative, 1); });
    timeIt ("compressor", [] (Harness& h) { h.allModulesOff(); h.set (compOn, 1); });
    timeIt ("EVERYTHING ON", [] (Harness& h) { h.set (fbOn, 1); h.set (eqOn, 1); h.set (pseOn, 1); h.set (compOn, 1); h.set (dsRelative, 1);
        for (int b = 0; b < 6; ++b) { h.set (bOn (b), 1); h.set (bDyn (b), b > 0 ? 1.0f : 0.0f); } });

    // ---- GUI cost per frame ----
    Harness h; h.p.presets.loadFactory (1);
    Voice v (120.0, false, -18.0f);
    int64_t pos = 0;
    auto feed = [&] { for (int b = 0; b < 6; ++b) { h.buf.setSize (1, blockSize, false, false, true);
        for (int i = 0; i < blockSize; ++i) h.buf.setSample (0, i, v (pos + i)); pos += blockSize; h.p.processBlock (h.buf, h.midi); } };
    for (int i = 0; i < 200; ++i) feed();
    std::unique_ptr<LiveVoiceEditor> ed (dynamic_cast<LiveVoiceEditor*> (h.p.createEditor()));
    juce::Image img (juce::Image::ARGB, LiveVoiceEditor::designWidth, LiveVoiceEditor::designHeight, true);
    for (float scale : { 1.0f, 0.8f })
    {
        ed->setSize (juce::roundToInt (LiveVoiceEditor::designWidth * scale), juce::roundToInt (LiveVoiceEditor::designHeight * scale));
        double tUpdate = 0, tPaint = 0;
        const int frames = 60;
        for (int f = 0; f < frames; ++f)
        {
            feed();
            auto t0 = juce::Time::getMillisecondCounterHiRes();
            ed->refreshNow();
            auto t1 = juce::Time::getMillisecondCounterHiRes();
            juce::Graphics g (img);
            ed->paintEntireComponent (g, true);
            auto t2 = juce::Time::getMillisecondCounterHiRes();
            tUpdate += t1 - t0; tPaint += t2 - t1;
        }
        std::cout << "GUI at " << (int) (scale * 100) << "% size: update " << juce::String (tUpdate / frames, 2) << " ms + full repaint "
                  << juce::String (tPaint / frames, 2) << " ms per frame -> at 30 fps = "
                  << juce::String ((tUpdate + tPaint) / frames * 30.0 / 10.0, 1) << "% of one core" << std::endl;
    }
    // which parts of the paint cost most
    ed->setSize (LiveVoiceEditor::designWidth, LiveVoiceEditor::designHeight);
    std::function<void (juce::Component&, int)> walk = [&] (juce::Component& c, int depth)
    {
        for (auto* ch : c.getChildren())
        {
            if (depth == 1)
            {
                juce::Image im (juce::Image::ARGB, std::max (1, ch->getWidth()), std::max (1, ch->getHeight()), true);
                auto t0 = juce::Time::getMillisecondCounterHiRes();
                for (int i = 0; i < 20; ++i) { juce::Graphics g (im); ch->paintEntireComponent (g, true); }
                const double ms = (juce::Time::getMillisecondCounterHiRes() - t0) / 20.0;
                std::cout << "    paint " << juce::String (typeid (*ch).name()).paddedRight (' ', 22) << juce::String (ms, 2) << " ms" << std::endl;
            }
            if (depth < 1) walk (*ch, depth + 1);
        }
    };
    walk (*ed, 0);
    ed.reset();
    return 0;
}

static int snapshot (const juce::String& path, bool light, bool toggle)
{
    juce::ScopedJuceInitialiser_GUI gui;
    const bool before = theme::loadLightModePreference();
    theme::saveLightModePreference (light);
    Harness h;
    h.set (fbOn, 1); h.set (eqOn, 1);
    h.p.presets.loadFactory (1);
    h.set (fbMode, 0);
    h.set (pseOn, 1); h.set (pseThr, -50.0f);
    h.set (bDyn (2), 1); h.set (bThr (2), -40.0f); h.set (bRange (2), -6.0f);
    h.set (compThr, -30.0f);
    h.set (lvlOn, 1); h.set (lvlTarget, -36.0f); h.set (proxOn, 1); h.set (warmth, 30.0f);
    h.p.selectedBand = 2;
    Voice v (120.0, false, -20.0f);
    Room room (1.2);
    runClosedLoop (h, v, room, 6.0);

    std::unique_ptr<LiveVoiceEditor> ed (dynamic_cast<LiveVoiceEditor*> (h.p.createEditor()));
    ed->setSize (LiveVoiceEditor::designWidth, LiveVoiceEditor::designHeight);
    if (toggle) ed->toggleThemeForTest();   // exercise the live switch, not just startup
    const juce::String name = juce::File (path).getFileName();
    if (name.contains ("eqsolo"))     h.p.soloTarget = 2;
    else if (name.contains ("solo"))  h.p.soloTarget = LiveVoiceProcessor::soloDeEss;
    if (name.contains ("off"))        { h.set (dsOn, 0); h.set (compOn, 0); }
    if (name.contains ("settings"))   ed->showSettingsForTest (true);
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
            for (int i = 0; i < blockSize; ++i) room.store (pos + i, juce::jlimit (-1.0f, 1.0f, h.buf.getSample (0, i)));
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
    theme::saveLightModePreference (before);
    return ok ? 0 : 1;
}

int main (int argc, char** argv)
{
    if (argc >= 2 && juce::String (argv[1]) == "--profile") return profile();
    if (argc >= 2 && juce::String (argv[1]) == "--deess-probe") return deEssProbe();
    if (argc >= 3 && juce::String (argv[1]) == "--snapshot")
        return snapshot (argv[2], argc >= 4 && juce::String (argv[3]) == "light", argc >= 4 && juce::String (argv[3]) == "toggle");

    juce::ScopedJuceInitialiser_GUI gui;   // parameters/attachments need a message manager
    testLatency();
    testDynamicEq();
    testSolo();
    testMonitorBus();
    testSuggest();
    testUndoAndAB();
    testDeEsser();
    testPresets();
    testPse();
    testCompressor();
    testRelativeDeEss();
    testDeEssQualityAndLevels();
    testLevelTone();
    testClosedLoop();
    testFalsePositives();
    testLearnAndState();
    testLearnFalsePositives();
    testPerformance();

    std::cout << "\n" << (failures == 0 ? "ALL TESTS PASSED" : juce::String (failures) + " TEST(S) FAILED") << std::endl;
    return failures == 0 ? 0 : 1;
}
