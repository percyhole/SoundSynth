/*
  ==============================================================================

   This file is part of the JUCE framework.
   Copyright (c) Raw Material Software Limited

   JUCE is an open source framework subject to commercial or open source
   licensing.

   By downloading, installing, or using the JUCE framework, or combining the
   JUCE framework with any other source code, object code, content or any other
   copyrightable work, you agree to the terms of the JUCE End User Licence
   Agreement, and all incorporated terms including the JUCE Privacy Policy and
   the JUCE Website Terms of Service, as applicable, which will bind you. If you
   do not agree to the terms of these agreements, we will not license the JUCE
   framework to you, and you must discontinue the installation or download
   process and cease use of the JUCE framework.

   JUCE End User Licence Agreement: https://juce.com/legal/juce-8-licence/
   JUCE Privacy Policy: https://juce.com/juce-privacy-policy
   JUCE Website Terms of Service: https://juce.com/juce-website-terms-of-service/

   Or:

   You may also use this code under the terms of the AGPLv3:
   https://www.gnu.org/licenses/agpl-3.0.en.html

   THE JUCE FRAMEWORK IS PROVIDED "AS IS" WITHOUT ANY WARRANTY, AND ALL
   WARRANTIES, WHETHER EXPRESSED OR IMPLIED, INCLUDING WARRANTY OF
   MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE, ARE DISCLAIMED.

  ==============================================================================
*/

namespace juce::dsp
{

//==============================================================================
template <typename SampleType>
LadderFilter<SampleType>::LadderFilter()  : state (2)
{
    setSampleRate (SampleType (1000));  // intentionally setting unrealistic default
                                        // sample rate to catch missing initialisation bugs
    setResonance (SampleType (0));
    setDrive     (SampleType (1.2));

    mode = Mode::LPF24;
    setMode (Mode::LPF12);
}

//==============================================================================
template <typename SampleType>
void LadderFilter<SampleType>::setMode (Mode newMode) noexcept
{
    if (newMode == mode)
        return;

    switch (newMode)
    {
        case Mode::LPF12:   A = {{ SampleType (0), SampleType (0),  SampleType (1), SampleType (0),  SampleType (0) }}; comp = SampleType (0.5);  break;
        case Mode::HPF12:   A = {{ SampleType (1), SampleType (-2), SampleType (1), SampleType (0),  SampleType (0) }}; comp = SampleType (0);    break;
        case Mode::BPF12:   A = {{ SampleType (0), SampleType (1), SampleType (-1), SampleType (0),  SampleType (0) }}; comp = SampleType (0.5);  break;
        case Mode::LPF24:   A = {{ SampleType (0), SampleType (0),  SampleType (0), SampleType (0),  SampleType (1) }}; comp = SampleType (0.5);  break;
        case Mode::HPF24:   A = {{ SampleType (1), SampleType (-4), SampleType (6), SampleType (-4), SampleType (1) }}; comp = SampleType (0);    break;
        case Mode::BPF24:   A = {{ SampleType (0), SampleType (0),  SampleType (1), SampleType (-2), SampleType (1) }}; comp = SampleType (0.5);  break;
        default:            jassertfalse;                                                                                                         break;
    }

    static constexpr auto outputGain = SampleType (1.2);

    for (auto& a : A)
        a *= outputGain;

    mode = newMode;
    reset();
}

//==============================================================================
template <typename SampleType>
void LadderFilter<SampleType>::prepare (const ProcessSpec& spec)
{
    setSampleRate (SampleType (spec.sampleRate));
    setNumChannels (spec.numChannels);
    reset();
}

//==============================================================================
template <typename SampleType>
void LadderFilter<SampleType>::reset() noexcept
{
    for (auto& s : state)
        s.fill (SampleType (0));

    cutoffTransformSmoother.setCurrentAndTargetValue (cutoffTransformSmoother.getTargetValue());
    scaledResonanceSmoother.setCurrentAndTargetValue (scaledResonanceSmoother.getTargetValue());
}

//==============================================================================
template <typename SampleType>
void LadderFilter<SampleType>::setCutoffFrequencyHz (SampleType newCutoff) noexcept
{
    jassert (newCutoff > SampleType (0));
    cutoffFreqHz = newCutoff;
    updateCutoffFreq();
}

//==============================================================================
template <typename SampleType>
void LadderFilter<SampleType>::setResonance (SampleType newResonance) noexcept
{
    jassert (newResonance >= SampleType (0) && newResonance <= SampleType (1));
    resonance = newResonance;
    updateResonance();
}

//==============================================================================
template <typename SampleType>
void LadderFilter<SampleType>::setDrive (SampleType newDrive) noexcept
{
    jassert (newDrive >= SampleType (1));

    drive = newDrive;
    gain = std::pow (drive, SampleType (-2.642))   * SampleType (0.6103) + SampleType (0.3903);
    drive2 = drive                                 * SampleType (0.04)   + SampleType (0.96);
    gain2 = std::pow (drive2, SampleType (-2.642)) * SampleType (0.6103) + SampleType (0.3903);
}

//==============================================================================
template <typename SampleType>
SampleType LadderFilter<SampleType>::processSample (SampleType inputValue, size_t channelToUse) noexcept
{
    auto& s = state[channelToUse];

    const auto a1 = cutoffTransformValue;
    const auto g = a1 * SampleType (-1) + SampleType (1);
    const auto b0 = g * SampleType (0.76923076923);
    const auto b1 = g * SampleType (0.23076923076);

    const auto dx = gain * saturationLUT (drive * inputValue);
    const auto a  = dx + scaledResonanceValue * SampleType (-4) * (gain2 * saturationLUT (drive2 * s[4]) - dx * comp);

    const auto b = b1 * s[0] + a1 * s[1] + b0 * a;
    const auto c = b1 * s[1] + a1 * s[2] + b0 * b;
    const auto d = b1 * s[2] + a1 * s[3] + b0 * c;
    const auto e = b1 * s[3] + a1 * s[4] + b0 * d;

    s[0] = a;
    s[1] = b;
    s[2] = c;
    s[3] = d;
    s[4] = e;

    return a * A[0] + b * A[1] + c * A[2] + d * A[3] + e * A[4];
}

//==============================================================================
template <typename SampleType>
void LadderFilter<SampleType>::updateSmoothers() noexcept
{
    cutoffTransformValue = cutoffTransformSmoother.getNextValue();
    scaledResonanceValue = scaledResonanceSmoother.getNextValue();
}

//==============================================================================
template <typename SampleType>
void LadderFilter<SampleType>::setSampleRate (SampleType newValue) noexcept
{
    jassert (newValue > SampleType (0));
    cutoffFreqScaler = SampleType (-2.0 * juce::MathConstants<double>::pi) / newValue;

    static constexpr SampleType smootherRampTimeSec = SampleType (0.05);
    cutoffTransformSmoother.reset (newValue, smootherRampTimeSec);
    scaledResonanceSmoother.reset (newValue, smootherRampTimeSec);

    updateCutoffFreq();
}

//==============================================================================
template class LadderFilter<float>;
template class LadderFilter<double>;

} // namespace juce::dsp
