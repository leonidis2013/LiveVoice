#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

// Factory presets (built in) plus user presets saved as files.
//
// A preset holds the *voice* settings: feedback behaviour, dynamic EQ and
// de-esser. It deliberately does NOT touch input/output gain, the safety
// limiter, bypass, Learn/Freeze, or the learned notches — those belong to
// the room and the channel, so switching voices mid-service never changes
// gain staging or forgets the ring-out.
class PresetManager
{
public:
    explicit PresetManager (juce::AudioProcessorValueTreeState& state);

    static juce::File getUserPresetDirectory();

    juce::StringArray getFactoryNames() const;
    juce::StringArray getUserNames() const;       // sorted, without extension

    bool loadFactory (int index);
    bool loadUser (const juce::String& name);
    bool saveUser (const juce::String& name);     // overwrites an existing preset of that name
    bool deleteUser (const juce::String& name);

    juce::String getCurrentName() const { return currentName; }
    void setCurrentName (const juce::String& n) { currentName = n; }

    static bool isPresetParameter (const juce::String& paramID);

    // voice settings as (id, real value) pairs: used by A/B compare
    using Values = std::vector<std::pair<juce::String, float>>;
    Values captureValues() const;
    void applyValues (const Values& values);

private:
    struct Factory
    {
        juce::String name;
        std::vector<std::pair<juce::String, float>> values;   // real (not normalised) values
    };

    static std::vector<Factory> buildFactoryPresets();

    juce::AudioProcessorValueTreeState& apvts;
    std::vector<Factory> factory;
    juce::String currentName { "Default" };
};
