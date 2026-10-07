#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// One place for the colour system and the custom look-and-feel.
namespace theme
{
    inline const juce::Colour bg        { 0xff0d1015 };
    inline const juce::Colour panel     { 0xff151a21 };
    inline const juce::Colour panelHi   { 0xff1b2129 };
    inline const juce::Colour border    { 0xff252c36 };
    inline const juce::Colour grid      { 0xff1e252e };
    inline const juce::Colour text      { 0xffd8dee6 };
    inline const juce::Colour textDim   { 0xff7c8794 };
    inline const juce::Colour textFaint { 0xff4c5561 };

    inline const juce::Colour eq        { 0xff2ec4b6 };   // teal
    inline const juce::Colour feedback  { 0xffff8a3d };   // orange
    inline const juce::Colour deess     { 0xffa78bfa };   // violet
    inline const juce::Colour pse       { 0xff8bd450 };   // green
    inline const juce::Colour comp      { 0xfff6c453 };   // amber
    inline const juce::Colour input     { 0xff5b6676 };
    inline const juce::Colour output    { 0xff4c8dff };
    inline const juce::Colour good      { 0xff3ecf6e };
    inline const juce::Colour warn      { 0xfff2c94c };
    inline const juce::Colour danger    { 0xffef4c4c };

    inline const juce::Colour bandColours[6] {
        juce::Colour (0xff2ec4b6), juce::Colour (0xff4cc9f0), juce::Colour (0xff7bdff2),
        juce::Colour (0xff80ed99), juce::Colour (0xfff7d070), juce::Colour (0xfff4978e)
    };

    inline juce::Font font (float size, bool bold = false)
    {
        return juce::Font (juce::FontOptions (size, bold ? juce::Font::bold : juce::Font::plain));
    }

    class LookAndFeel : public juce::LookAndFeel_V4
    {
    public:
        LookAndFeel()
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
            setColour (juce::PopupMenu::highlightedTextColourId, juce::Colours::white);
            setColour (juce::TextButton::buttonColourId, panelHi);
            setColour (juce::TextButton::textColourOffId, text);
            setColour (juce::TextButton::textColourOnId, bg);
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
