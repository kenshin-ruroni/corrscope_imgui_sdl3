#pragma once
#include <vector>
#include <algorithm>
#include <cmath>

using f32 = float;

class NodalOverdriveShockley
{
private:
    f32 m_drive = 2.0f;
    f32 m_mix = 1.0f;

    // Constantes physiques de la diode (Silicium standard)
    const f32 Is = 1e-12f;       // Courant de saturation inverse
    const f32 Vt = 0.026f;       // Tension thermique (26mV)
    const f32 InvVt = 1.0f / Vt; // Optimisation pour éviter les divisions
    const f32 Ge = 0.05f;        // Admittance équivalente du circuit environnant

public:
    NodalOverdriveShockley(f32 drive, f32 mix) : m_drive(drive), m_mix(mix) {}

    void ProcessSIMD(f32 *samples, size_t count)
    {
        if (count == 0)
            return;

        f32 drv = m_drive;
        f32 wet = m_mix;
        f32 dry = 1.0f - wet;

// Note : #pragma omp simd s'exécute sur Newton-Raphson car la boucle interne a une taille fixe (4)
#pragma omp simd
        for (size_t i = 0; i < count; ++i)
        {
            f32 v_input = samples[i] * drv;

            // Estimation initiale (predictor)
            f32 v_node = v_input * 0.5f;

            // Solveur de Newton-Raphson (Résolution de la loi des nœuds non linéaire)
            for (int iter = 0; iter < 4; ++iter)
            {
                f32 exp_p = std::exp(v_node * InvVt);
                f32 exp_n = std::exp(-v_node * InvVt);

                // Courant total passant par la paire de diodes tête-bêche
                f32 i_diode = Is * (exp_p - exp_n);

                // Dérivée du courant par rapport à la tension (Conductance dynamique)
                f32 di_diode = Is * InvVt * (exp_p + exp_n);

                // Équation d'erreur nodale F(v) = 0
                f32 F = Ge * (v_node - v_input) + i_diode;
                f32 dF = Ge + di_diode;

                // Correction de la tension du nœud
                v_node = v_node - (F / dF);
            }

            // Normalisation douce de la sortie
            f32 v_out = v_node * 0.4f;

            samples[i] = (v_out * wet) + (samples[i] * dry);
        }
    }

    void UpdateParameters(f32 drive, f32 mix)
    {
        m_drive = std::max(1.0f, drive);
        m_mix = std::clamp(mix, 0.0f, 1.0f);
    }
};
