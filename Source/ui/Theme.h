#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// One place for the colour system and the custom look-and-feel.
namespace theme
{
    // Live palette. Everything paints from these at paint time, so switching
    // theme is: apply a palette, remap colours already baked into widgets, repaint.
    inline juce::Colour bg, panel, panelHi, border, grid, text, textDim, textFaint, onAccent;
    inline juce::Colour eq, feedback, deess, pse, comp, input, output, good, warn, danger;
    inline juce::Colour bandColours[6];

    struct Palette
    {
        juce::uint32 bg, panel, panelHi, border, grid, text, textDim, textFaint, onAccent;
        juce::uint32 eq, feedback, deess, pse, comp, input, output, good, warn, danger;
        juce::uint32 bands[6];
    };

    inline constexpr Palette darkPalette {
        0xff0d1015, 0xff151a21, 0xff1b2129, 0xff252c36, 0xff1e252e, 0xffd8dee6, 0xff7c8794, 0xff4c5561, 0xff0d1015,
        0xff2ec4b6, 0xffff8a3d, 0xffa78bfa, 0xff8bd450, 0xfff6c453, 0xff5b6676, 0xff4c8dff, 0xff3ecf6e, 0xfff2c94c, 0xffef4c4c,
        { 0xff2ec4b6, 0xff4cc9f0, 0xff7bdff2, 0xff80ed99, 0xfff7d070, 0xfff4978e } };

    // Light: soft grey page, white cards, accents darkened for contrast on white.
    inline constexpr Palette lightPalette {
        0xffe9edf2, 0xfffbfcfd, 0xfff0f3f6, 0xffd3dae3, 0xffe3e8ee, 0xff18202b, 0xff586474, 0xff97a1ae, 0xffffffff,
        0xff0f9488, 0xffe0661a, 0xff7553e0, 0xff4b9b25, 0xffc98a0a, 0xff9aa3af, 0xff2563d6, 0xff23a056, 0xffcf9a10, 0xffd63232,
        { 0xff0f9488, 0xff1b8fc4, 0xff3a9fb5, 0xff3f9d55, 0xffc08a10, 0xffd2584c } };

    inline bool lightMode = false;

    inline Palette currentPalette()
    {
        auto u = [] (juce::Colour c) { return c.getARGB(); };
        return { u (bg), u (panel), u (panelHi), u (border), u (grid), u (text), u (textDim), u (textFaint), u (onAccent),
                 u (eq), u (feedback), u (deess), u (pse), u (comp), u (input), u (output), u (good), u (warn), u (danger),
                 { u (bandColours[0]), u (bandColours[1]), u (bandColours[2]), u (bandColours[3]), u (bandColours[4]), u (bandColours[5]) } };
    }

    inline void applyPalette (const Palette& p)
    {
        using C = juce::Colour;
        bg = C (p.bg); panel = C (p.panel); panelHi = C (p.panelHi); border = C (p.border); grid = C (p.grid);
        text = C (p.text); textDim = C (p.textDim); textFaint = C (p.textFaint); onAccent = C (p.onAccent);
        eq = C (p.eq); feedback = C (p.feedback); deess = C (p.deess); pse = C (p.pse); comp = C (p.comp);
        input = C (p.input); output = C (p.output); good = C (p.good); warn = C (p.warn); danger = C (p.danger);
        for (int i = 0; i < 6; ++i) bandColours[i] = C (p.bands[i]);
    }

    inline void setLightMode (bool light)
    {
        lightMode = light;
        applyPalette (light ? lightPalette : darkPalette);
    }

    // Swaps any colour from `from` baked into widgets (labels, buttons, sliders...) for its counterpart in `to`.
    inline void remapColours (juce::Component& root, const Palette& from, const Palette& to)
    {
        const juce::uint32* a = &from.bg;
        const juce::uint32* b = &to.bg;
        constexpr int n = (int) (sizeof (Palette) / sizeof (juce::uint32));
        static const int ids[] {
            juce::Label::textColourId, juce::Label::backgroundColourId,
            juce::TextButton::buttonColourId, juce::TextButton::buttonOnColourId,
            juce::TextButton::textColourOffId, juce::TextButton::textColourOnId,
            juce::Slider::rotarySliderFillColourId, juce::Slider::textBoxTextColourId,
            juce::ComboBox::backgroundColourId, juce::ComboBox::textColourId };

        std::function<void (juce::Component&)> visit = [&] (juce::Component& c)
        {
            for (int id : ids)
                if (c.isColourSpecified (id))
                {
                    const auto argb = c.findColour (id).getARGB();
                    for (int i = 0; i < n; ++i)
                        if (a[i] == argb) { c.setColour (id, juce::Colour (b[i])); break; }
                }
            for (auto* child : c.getChildren()) visit (*child);
        };
        visit (root);
    }

    // Theme choice is remembered across sessions and plugin instances.
    inline juce::File settingsFile()
    {
        auto base = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
       #if JUCE_MAC
        base = base.getChildFile ("Application Support");
       #endif
        return base.getChildFile ("LiveVoice").getChildFile ("settings.xml");
    }

    inline bool loadLightModePreference()
    {
        if (auto xml = juce::XmlDocument::parse (settingsFile()))
            return xml->getBoolAttribute ("lightMode", false);
        return false;
    }

    inline void saveLightModePreference (bool light)
    {
        auto f = settingsFile();
        f.getParentDirectory().createDirectory();
        juce::XmlElement xml ("LiveVoiceSettings");
        xml.setAttribute ("lightMode", light);
        xml.writeTo (f);
    }

    struct PaletteInit { PaletteInit() { applyPalette (darkPalette); } };
    inline PaletteInit paletteInit;

    inline juce::Font font (float size, bool bold = false)
    {
        return juce::Font (juce::FontOptions (size, bold ? juce::Font::bold : juce::Font::plain));
    }

    class LookAndFeel : public juce::LookAndFeel_V4
    {
    public:
        LookAndFeel() { applyColours(); }

        void applyColours()
        {
            setColour (juce::ResizableWindow::backgroundColourId, bg);
            setColour (juce::Slider::textBoxTextColourId, text);
            setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
            setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
            setColour (juce::Slider::rotarySliderOutlineColourId, border);
            setColour (juce::Label::textColourId, text);
            setColour (juce::ComboBox::backgroundColourId, panelHi);
            setColour (juce::ComboBox::outlineColourId, border);
            setColour (juce::ComboBox::textColourId, text);
            setColour (juce::ComboBox::arrowColourId, textDim);
            setColour (juce::PopupMenu::backgroundColourId, panelHi);
            setColour (juce::PopupMenu::textColourId, text);
            setColour (juce::PopupMenu::highlightedBackgroundColourId, border);
            setColour (juce::PopupMenu::highlightedTextColourId, text);
            setColour (juce::TextButton::buttonColourId, panelHi);
            setColour (juce::TextButton::textColourOffId, text);
            setColour (juce::TextButton::textColourOnId, onAccent);
            setColour (juce::TextEditor::backgroundColourId, panelHi);
            setColour (juce::TextEditor::outlineColourId, border);
            setColour (juce::TextEditor::textColourId, text);
            setColour (juce::AlertWindow::backgroundColourId, panel);
            setColour (juce::AlertWindow::textColourId, text);
        }

        void drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos,
                               float startAngle, float endAngle, juce::Slider& s) override
        {
            auto bounds = juce::Rectangle<int> (x, y, w, h).toFloat().reduced (4.0f);
            const float r = std::min (bounds.getWidth(), bounds.getHeight()) * 0.5f;
            const auto c = bounds.getCentre();
            const float track = std::max (2.5f, r * 0.14f);
            const float angle = startAngle + pos * (endAngle - startAngle);
            const auto accent = s.findColour (juce::Slider::rotarySliderFillColourId);
            const bool enabled = s.isEnabled();

            juce::Path bgArc;
            bgArc.addCentredArc (c.x, c.y, r - track, r - track, 0.0f, startAngle, endAngle, true);
            g.setColour (border);
            g.strokePath (bgArc, juce::PathStrokeType (track, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

            // bipolar parameters fill from the centre
            const bool bipolar = s.getMinimum() < 0.0 && s.getMaximum() > 0.0 && std::abs (s.getMinimum() + s.getMaximum()) < 1.0e-3;
            const float from = bipolar ? (startAngle + endAngle) * 0.5f : startAngle;

            juce::Path valArc;
            valArc.addCentredArc (c.x, c.y, r - track, r - track, 0.0f, std::min (from, angle), std::max (from, angle), true);
            g.setColour (enabled ? accent : accent.withSaturation (0.0f).withAlpha (0.35f));
            g.strokePath (valArc, juce::PathStrokeType (track, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

            const float knobR = r - track * 2.2f;
            g.setColour (panelHi);
            g.fillEllipse (c.x - knobR, c.y - knobR, knobR * 2.0f, knobR * 2.0f);
            g.setColour (border.brighter (0.2f));
            g.drawEllipse (c.x - knobR, c.y - knobR, knobR * 2.0f, knobR * 2.0f, 1.0f);

            const auto tip = c.getPointOnCircumference (knobR * 0.78f, angle);
            const auto base = c.getPointOnCircumference (knobR * 0.25f, angle);
            g.setColour (enabled ? text : textFaint);
            g.drawLine ({ base, tip }, 2.0f);
        }

        void drawLinearSlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float, float,
                               juce::Slider::SliderStyle, juce::Slider& s) override
        {
            const float cy = (float) y + (float) h * 0.5f;
            const float x0 = (float) x + 6.0f, x1 = (float) (x + w) - 6.0f;
            g.setColour (border);
            g.fillRoundedRectangle (x0, cy - 2.0f, x1 - x0, 4.0f, 2.0f);
            const bool bipolar = s.getMinimum() < 0.0 && s.getMaximum() > 0.0 && std::abs (s.getMinimum() + s.getMaximum()) < 1.0e-3;
            const float from = bipolar ? (x0 + x1) * 0.5f : (s.getMaximum() <= 0.0 ? x1 : x0);
            const auto accent = s.findColour (juce::Slider::rotarySliderFillColourId);
            g.setColour (accent);
            g.fillRoundedRectangle (std::min (from, pos), cy - 2.0f, std::abs (pos - from), 4.0f, 2.0f);
            g.setColour (text);
            g.fillEllipse (pos - 6.0f, cy - 6.0f, 12.0f, 12.0f);
            g.setColour (bg);
            g.drawEllipse (pos - 6.0f, cy - 6.0f, 12.0f, 12.0f, 1.5f);
        }

        juce::Label* createSliderTextBox (juce::Slider& s) override
        {
            auto* l = LookAndFeel_V4::createSliderTextBox (s);
            l->setFont (font (11.5f));
            l->setJustificationType (juce::Justification::centred);
            return l;
        }

        void drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour& bgc,
                                   bool over, bool down) override
        {
            auto r = b.getLocalBounds().toFloat().reduced (0.5f);
            auto fill = b.getToggleState() ? b.findColour (juce::TextButton::buttonOnColourId) : bgc;
            if (down) fill = fill.brighter (0.15f);
            else if (over) fill = fill.brighter (0.07f);
            g.setColour (fill);
            g.fillRoundedRectangle (r, 5.0f);
            g.setColour (b.getToggleState() ? fill.brighter (0.3f) : border);
            g.drawRoundedRectangle (r, 5.0f, 1.0f);
        }

        juce::Font getTextButtonFont (juce::TextButton&, int h) override { return font (std::min (13.0f, h * 0.48f), true); }

        void drawComboBox (juce::Graphics& g, int w, int h, bool, int, int, int, int, juce::ComboBox& box) override
        {
            auto r = juce::Rectangle<float> (0, 0, (float) w, (float) h).reduced (0.5f);
            g.setColour (box.findColour (juce::ComboBox::backgroundColourId));
            g.fillRoundedRectangle (r, 5.0f);
            g.setColour (box.findColour (juce::ComboBox::outlineColourId));
            g.drawRoundedRectangle (r, 5.0f, 1.0f);
            juce::Path arrow;
            const float ax = (float) w - 14.0f, ay = (float) h * 0.5f;
            arrow.addTriangle (ax - 4.0f, ay - 2.0f, ax + 4.0f, ay - 2.0f, ax, ay + 3.0f);
            g.setColour (textDim);
            g.fillPath (arrow);
        }

        juce::Font getComboBoxFont (juce::ComboBox&) override { return font (13.0f); }
        juce::Font getPopupMenuFont() override { return font (13.5f); }

        void positionComboBoxText (juce::ComboBox& box, juce::Label& label) override
        {
            label.setBounds (8, 1, box.getWidth() - 26, box.getHeight() - 2);
            label.setFont (getComboBoxFont (box));
        }
    };
}
