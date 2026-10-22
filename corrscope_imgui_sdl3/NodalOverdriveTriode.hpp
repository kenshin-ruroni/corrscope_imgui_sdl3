#pragma once
#include <vector>
#include <algorithm>
#include <cmath>

using f32 = float;

class NodalOverdriveTriode
{
private:
    f32 m_gain = 5.0f;
    f32 m_bias = -1.2f; // Tension de polarisation de la grille (généralement négative)
    f32 m_mix = 1.0f;

    // Paramètres empiriques du modèle de Koren pour une triode 12AX7
    const f32 mu = 100.0f;     // Facteur d'amplification théorique
    const f32 Kp = 600.0f;     // Paramètre de courbure de l'anode
    const f32 Kv = 1.4f;       // Facteur d'asymétrie de grille
    const f32 Exponent = 1.5f; // Exposant de l'espace de charge (Loi de Child-Langmuir)

public:
    NodalOverdriveTriode(f32 gain, f32 bias, f32 mix)
        : m_gain(gain), m_bias(bias), m_mix(mix) {}

    void ProcessSIMD(f32 *samples, size_t count)
    {
        if (count == 0)
            return;

        f32 g = m_gain;
        f32 b = m_bias;
        f32 wet = m_mix;
        f32 dry = 1.0f - wet;

        // Tension d'anode fixe simulée (B+ @ 250V converti en échelle relative)
        f32 V_anode = 1.0f;

#pragma omp simd
        for (size_t i = 0; i < count; ++i)
        {
            f32 v_grid = (samples[i] * g) + b;

            // Équation de Koren pour l'évaluation de la forme d'onde nodale de la lampe
            f32 E1 = (V_anode / Kp) * std::log(1.0f + std::exp(Kp * (1.0f / mu + v_grid / std::sqrt(Kv + V_anode * V_anode))));

            f32 i_anode = 0.0f;
            if (E1 > 0.0f)
            {
                i_anode = std::pow(E1, Exponent) * 2.5f;
            }

            // Inversion de phase et compression intrinsèque du préampli à lampe
            f32 v_out = 0.5f - i_anode;

            // Soft-clipping de sécurité et retrait du décalage continu (DC bias compensation)
            v_out = std::clamp(v_out, -0.9f, 0.9f) + 0.15f;

            samples[i] = (v_out * wet) + (samples[i] * dry);
        }
    }

    void UpdateParameters(f32 gain, f32 bias, f32 mix)
    {
        m_gain = std::max(0.1f, gain);
        m_bias = std::clamp(bias, -3.0f, -0.1f);
        m_mix = std::clamp(mix, 0.0f, 1.0f);
    }
};
