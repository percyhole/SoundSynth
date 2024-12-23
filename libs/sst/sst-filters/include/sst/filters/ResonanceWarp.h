/*
 * sst-filters - A header-only collection of SIMD filter
 * implementations by the Surge Synth Team
 *
 * Copyright 2019-2024, various authors, as described in the GitHub
 * transaction log.
 *
 * sst-filters is released under the Gnu General Public Licens
 * version 3 or later. Some of the filters in this package
 * originated in the version of Surge open sourced in 2018.
 *
 * All source in sst-filters available at
 * https://github.com/surge-synthesizer/sst-filters
 */
#ifndef INCLUDE_SST_FILTERS_RESONANCEWARP_H
#define INCLUDE_SST_FILTERS_RESONANCEWARP_H

#include "QuadFilterUnit.h"
#include "FilterCoefficientMaker.h"
#include "sst/basic-blocks/dsp/FastMath.h"
#include "sst/basic-blocks/dsp/Clippers.h"

/**
 * This contains an adaptation of the filter found at
 * https://ccrma.stanford.edu/~jatin/ComplexNonlinearities/NLBiquad.html
 * with coefficient calculation from
 * https://webaudio.github.io/Audio-EQ-Cookbook/audio-eq-cookbook.html
 *
 * A lot of code here is duplicated from NonlinearFeedback.cpp, perhaps in future they
 * could be merged, but for the time being they're separate and nothing is shared.
 */
namespace sst::filters::ResonanceWarp
{
template <typename TuningProvider>
static float clampedFrequency(float pitch, float sampleRate, TuningProvider *provider)
{
    auto freq =
        provider->note_to_pitch_ignoring_tuning(pitch + 69) * (float)TuningProvider::MIDI_0_FREQ;
    freq = std::clamp(freq, 5.f, sampleRate * 0.3f);
    return freq;
}

#define F(a) SIMD_MM(set_ps1)(a)
#define M(a, b) SIMD_MM(mul_ps)(a, b)
#define A(a, b) SIMD_MM(add_ps)(a, b)
#define S(a, b) SIMD_MM(sub_ps)(a, b)

enum Saturator
{
    SAT_TANH = 0,
    SAT_SOFT
};

static inline SIMD_M128 doNLFilter(const SIMD_M128 input, const SIMD_M128 a1, const SIMD_M128 a2,
                                   const SIMD_M128 b0, const SIMD_M128 b1, const SIMD_M128 b2,
                                   const int sat, SIMD_M128 &z1, SIMD_M128 &z2) noexcept
{
    // out = z1 + b0 * input
    const auto out = A(z1, M(b0, input));

    // z1 = z2 + b1 * input - a1 * out
    z1 = A(z2, S(M(b1, input), M(a1, out)));
    // z2 = b2 * input - a2 * out
    z2 = S(M(b2, input), M(a2, out));

    // now apply a nonlinearity to z1 and z2
    switch (sat)
    {
    case SAT_TANH:
        z1 = basic_blocks::dsp::fasttanhSSEclamped(z1);
        z2 = basic_blocks::dsp::fasttanhSSEclamped(z2);
        break;
    default:
        z1 = basic_blocks::dsp::softclip_ps(
            z1); // note, this is a bit different to Jatin's softclipper
        z2 = basic_blocks::dsp::softclip_ps(z2);
        break;
    }
    return out;
}

enum nls_coeffs
{
    nls_a1 = 0,
    nls_a2,
    nls_b0,
    nls_b1,
    nls_b2,
    n_nls_coeff
};

enum dlf_state
{
    nls_z1, // 1st z-1 state for first  stage
    nls_z2, // 2nd z-1 state for first  stage
    nls_z3, // 1st z-1 state for second stage
    nls_z4, // 2nd z-1 state for second stage
    nls_z5, // 1st z-1 state for third  stage
    nls_z6, // 2nd z-1 state for third  stage
    nls_z7, // 1st z-1 state for fourth stage
    nls_z8, // 2nd z-1 state for fourth stage
};

template <typename TuningProvider>
void makeCoefficients(FilterCoefficientMaker<TuningProvider> *cm, float freq, float reso, int type,
                      float sampleRate, TuningProvider *provider)
{
    float C[n_cm_coeffs]{};

    reso = std::clamp(reso, 0.f, 1.f);

    const float q = ((reso * reso * reso) * 18.0f + 0.1f);

    const float wc = 2.0f * (float)M_PI * clampedFrequency(freq, sampleRate, provider) / sampleRate;

    const float wsin = basic_blocks::dsp::fastsin(wc);
    const float wcos = basic_blocks::dsp::fastcos(wc);
    const float alpha = wsin / (2.0f * q);

    // note we actually calculate the reciprocal of a0 because we only use a0 to normalize the
    // other coefficients, and multiplication by reciprocal is cheaper than dividing.
    const float a0r = 1.0f / (1.0f + alpha);

    C[nls_a1] = -2.0f * wcos * a0r;
    C[nls_a2] = (1.0f - alpha) * a0r;

    switch (type)
    {
    case fut_resonancewarp_lp: // lowpass
        C[nls_b1] = (1.0f - wcos) * a0r;
        C[nls_b0] = C[nls_b1] * 0.5f;
        C[nls_b2] = C[nls_b0];
        break;
    case fut_resonancewarp_hp: // highpass
        C[nls_b1] = -(1.0f + wcos) * a0r;
        C[nls_b0] = C[nls_b1] * -0.5f;
        C[nls_b2] = C[nls_b0];
        break;
    case fut_resonancewarp_n: // notch
        C[nls_b0] = a0r;
        C[nls_b1] = -2.0f * wcos * a0r;
        C[nls_b2] = C[nls_b0];
        break;
    case fut_resonancewarp_bp: // bandpass
        C[nls_b0] = wsin * 0.5f * a0r;
        C[nls_b1] = 0.0f;
        C[nls_b2] = -C[nls_b0];
        break;
    default: // allpass
        C[nls_b0] = C[nls_a2];
        C[nls_b1] = C[nls_a1];
        C[nls_b2] = 1.0f; // (1+a) / (1+a) = 1 (from normalising by a0)
        break;
    }

    cm->FromDirect(C);
}

template <FilterSubType subtype>
inline SIMD_M128 process(QuadFilterUnitState *__restrict f, SIMD_M128 input)
{
    // lower 2 bits of subtype is the stage count
    const int stages = subtype & 3;
    // next two bits after that select the saturator
    const int sat = ((subtype >> 2) & 3);

    // n.b. stages is zero-indexed so use <=
    for (int stage = 0; stage <= stages; ++stage)
    {
        input = doNLFilter(input, f->C[nls_a1], f->C[nls_a2], f->C[nls_b0], f->C[nls_b1],
                           f->C[nls_b2], sat, f->R[nls_z1 + stage * 2], f->R[nls_z2 + stage * 2]);
    }

    for (int i = 0; i < n_nls_coeff; ++i)
    {
        f->C[i] = A(f->C[i], f->dC[i]);
    }

    return input;
}

#undef F
#undef M
#undef A
#undef S

} // namespace sst::filters::ResonanceWarp

#endif // SST_FILTERS_RESONANCEWARP_H
