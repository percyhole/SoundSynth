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
#ifndef INCLUDE_SST_FILTERS_K35FILTER_H
#define INCLUDE_SST_FILTERS_K35FILTER_H

#include "sst/basic-blocks/dsp/FastMath.h"
#include "QuadFilterUnit.h"
#include "FilterCoefficientMaker.h"

/**
 * This namespace contains an adaptation of the filter from
 * https://github.com/TheWaveWarden/odin2/blob/master/Source/audio/Filters/Korg35Filter.cpp
 */
namespace sst::filters::K35Filter
{

template <typename TuningProvider>
static float clampedFrequency(float pitch, float sampleRate, TuningProvider *provider)
{
    auto freq =
        provider->note_to_pitch_ignoring_tuning(pitch + 69.0f) * (float)TuningProvider::MIDI_0_FREQ;
    return std::clamp(freq, 5.f, (sampleRate * 0.3f));
}

#define F(a) SIMD_MM(set_ps1)(a)
#define M(a, b) SIMD_MM(mul_ps)(a, b)
#define D(a, b) SIMD_MM(div_ps)(a, b)
#define A(a, b) SIMD_MM(add_ps)(a, b)
#define S(a, b) SIMD_MM(sub_ps)(a, b)

// note that things that were NOPs in the Odin code have been removed.
// m_gamma remains 1.0 so xn * m_gamma == xn; that's a NOP
// m_feedback remains 0, that's a NOP
// m_epsilon remains 0, that's a NOP
// m_a_0 remains 1 so that's also a NOP
// so we only need to compute:
// (xn - z) * alpha + za
static inline SIMD_M128 doLpf(const SIMD_M128 &G, const SIMD_M128 &input, SIMD_M128 &z) noexcept
{
    const auto v = M(S(input, z), G);
    const auto result = A(v, z);
    z = A(v, result);
    return result;
}
static inline SIMD_M128 doHpf(const SIMD_M128 &G, const SIMD_M128 &input, SIMD_M128 &z) noexcept
{
    return S(input, doLpf(G, input, z));
}

enum k35_coeffs
{
    k35_G = 0,                // aka alpha
    k35_lb,                   // LPF beta
    k35_hb,                   // HPF beta
    k35_k,                    // k (m_k_modded)
    k35_alpha,                // aka m_alpha
    k35_saturation,           // amount of saturation to apply (scaling before tanh)
    k35_saturation_blend,     // above but clamped to 0..1, used to blend tanh version when <1
    k35_saturation_blend_inv, // above but inverted, used to blend non-tanh version when <1
};

enum k35_state
{
    k35_lz, // LPF1 z-1 storage
    k35_hz, // HPF1 z-1 storage
    k35_2z, // xPF2 z-1 storage
};

template <typename TuningProvider>
void makeCoefficients(FilterCoefficientMaker<TuningProvider> *cm, float freq, float reso,
                      bool is_lowpass, float saturation, float sampleRate, float sampleRateInv,
                      TuningProvider *provider)
{
    float C[n_cm_coeffs]{};

    const float wd = clampedFrequency(freq, sampleRate, provider) * 2.0f * (float)M_PI;
    const float wa = (2.0f * sampleRate) * basic_blocks::dsp::fasttan(wd * sampleRateInv * 0.5f);
    const float g = wa * sampleRateInv * 0.5f;
    const float gp1 = (1.0f + g); // g plus 1
    const float G = g / gp1;

    const float k = reso * 1.96f;
    // clamp to [0.01..1.96]
    const float mk = (k > 1.96f) ? 1.96f : ((k < 0.01f) ? 0.01f : k);

    C[k35_G] = G;

    if (is_lowpass)
    {
        C[k35_lb] = (mk - mk * G) / gp1;
        C[k35_hb] = -1.0f / gp1;
    }
    else
    {
        C[k35_lb] = 1.0f / gp1;
        C[k35_hb] = -G / gp1;
    }

    C[k35_k] = mk;

    C[k35_alpha] = 1.0f / (1.0f - mk * G + mk * G * G);

    C[k35_saturation] = saturation;
    C[k35_saturation_blend] = fminf(saturation, 1.0f);
    C[k35_saturation_blend_inv] = 1.0f - C[k35_saturation_blend];

    cm->FromDirect(C);
}

inline void processCoeffs(QuadFilterUnitState *__restrict f)
{
    for (int i = 0; i < n_cm_coeffs; ++i)
        f->C[i] = A(f->C[i], f->dC[i]);
}

inline SIMD_M128 process_lp(QuadFilterUnitState *__restrict f, SIMD_M128 input)
{
    processCoeffs(f);

    const auto y1 = doLpf(f->C[k35_G], input, f->R[k35_lz]);
    // (lpf beta * lpf2 feedback) + (hpf beta * hpf1 feedback)
    const auto s35 = A(M(f->C[k35_lb], f->R[k35_2z]), M(f->C[k35_hb], f->R[k35_hz]));
    // alpha * (y1 + s35)
    const auto u_clean = M(f->C[k35_alpha], A(y1, s35));
    const auto u_driven = basic_blocks::dsp::fasttanhSSEclamped(M(u_clean, f->C[k35_saturation]));
    const auto u =
        A(M(u_clean, f->C[k35_saturation_blend_inv]), M(u_driven, f->C[k35_saturation_blend]));

    // mk * lpf2(u)
    const auto y = M(f->C[k35_k], doLpf(f->C[k35_G], u, f->R[k35_2z]));
    doHpf(f->C[k35_G], y, f->R[k35_hz]);

    const auto result = D(y, f->C[k35_k]);

    return result;
}

inline SIMD_M128 process_hp(QuadFilterUnitState *__restrict f, SIMD_M128 input)
{
    processCoeffs(f);

    const auto y1 = doHpf(f->C[k35_G], input, f->R[k35_hz]);
    // (lpf beta * lpf2 feedback) + (hpf beta * hpf1 feedback)
    const auto s35 = A(M(f->C[k35_hb], f->R[k35_2z]), M(f->C[k35_lb], f->R[k35_lz]));
    // alpha * (y1 + s35)
    const auto u = M(f->C[k35_alpha], A(y1, s35));

    // mk * lpf2(u)
    const auto y_clean = M(f->C[k35_k], u);
    const auto y_driven = basic_blocks::dsp::fasttanhSSEclamped(M(y_clean, f->C[k35_saturation]));
    const auto y =
        A(M(y_clean, f->C[k35_saturation_blend_inv]), M(y_driven, f->C[k35_saturation_blend]));

    doLpf(f->C[k35_G], doHpf(f->C[k35_G], y, f->R[k35_2z]), f->R[k35_lz]);

    const auto result = D(y, f->C[k35_k]);

    return result;
}
#undef F
#undef M
#undef D
#undef A
#undef S
} // namespace sst::filters::K35Filter

#endif // SST_FILTERS_K35FILTER_H