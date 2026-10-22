#pragma once
#include <vector>
#include <algorithm>
#include <cmath>

using f32 = float;

class NodalOverdriveStatic
{
private:
    f32 m_drive = 1.0f;     // Gain d'entrée (Saturateur)
    f32 m_asymmetry = 0.1f; // Décalage DC pour générer des harmoniques paires
    f32 m_mix = 1.0f;       // Mélange Dry/Wet (0.0f à 1.0f)

public:
    NodalOverdriveStatic(f32 drive, f32 asymmetry, f32 mix)
        : m_drive(drive), m_asymmetry(asymmetry), m_mix(mix) {}

    void ProcessSIMD(f32 *samples, size_t count)
    {
        if (count == 0)
            return;

        f32 drv = m_drive;
        f32 asym = m_asymmetry;
        f32 wet = m_mix;
        f32 dry = 1.0f - wet;

        // Compensation automatique de gain pour éviter les explosions de volume
        f32 makeupGain = 1.0f / (1.0f + std::sqrt(drv));

#pragma omp simd
        for (size_t i = 0; i < count; ++i)
        {
            f32 x = samples[i];

            // 1. Application du Drive et de l'asymétrie (simule la polarisation d'un tube)
            f32 x_driven = (x * drv) + asym;

            // 2. Fonction de transfert cubique (Soft-clipping)
            f32 x_saturated = 0.0f;
            if (x_driven > 1.2f)
            {
                x_saturated = 1.0f;
            }
            else if (x_driven < -1.2f)
            {
                x_saturated = -1.0f;
            }
            else
            {
                x_saturated = x_driven - (0.231f * x_driven * x_driven * x_driven);
            }

            // 3. Suppression de la composante continue générée par l'asymétrie & application du gain
            f32 clean_wet = (x_saturated - asym) * makeupGain;

            // 4. Interpolation linéaire (Mélange)
            samples[i] = (clean_wet * wet) + (x * dry);
        }
    }

    void UpdateParameters(f32 drive, f32 asymmetry, f32 mix)
    {
        m_drive = std::max(1.0f, drive);
        m_asymmetry = std::clamp(asymmetry, -0.5f, 0.5f);
        m_mix = std::clamp(mix, 0.0f, 1.0f);
    }
};
