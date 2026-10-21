#pragma once

#include <vector>
#include <algorithm>
#include <cmath>

using f32 = float;

class NodalFDNProcessor {
private:
    // Deux lignes de retard distinctes (tailles de nombres premiers pour éviter les résonances)
    std::vector<f32> m_delayLine1;
    std::vector<f32> m_delayLine2;
    
    size_t m_readIdx1 = 0, m_writeIdx1 = 0;
    size_t m_readIdx2 = 0, m_writeIdx2 = 0;

    int m_delay1Samples;
    int m_delay2Samples;
    f32 m_feedbackGain;

    // Constante de normalisation de la matrice de Hadamard (1 / sqrt(2))
    const f32 INV_SQRT2 = 1.0f/sqrtf(2.0f);

public:
    NodalFDNProcessor(int delay1, int delay2, f32 feedback)
        : m_delay1Samples(delay1), m_delay2Samples(delay2), m_feedbackGain(feedback) {
        if (m_delay1Samples < 1) m_delay1Samples = 1;
        if (m_delay2Samples < 1) m_delay2Samples = 1;
        
        m_delayLine1.assign(m_delay1Samples, 0.0f);
        m_delayLine2.assign(m_delay2Samples, 0.0f);
    }

    /**
     * Résout le système FDN Nodal Stéréo par blocs via OpenMP SIMD.
     * Reçoit le canal Gauche et Droit entrelacés ou synchrones.
     */
    void ProcessFDNBufferSIMD(f32* leftSamples, f32* rightSamples, size_t count) {
        if (count == 0) return;

        f32 g = m_feedbackGain;
        f32* ring1 = m_delayLine1.data();
        f32* ring2 = m_delayLine2.data();

        size_t samplesProcessed = 0;

        while (samplesProcessed < count) {
            // Détermination du chunk de traitement contigu maximal
            size_t remRing1 = m_delayLine1.size() - std::max(m_readIdx1, m_writeIdx1);
            size_t remRing2 = m_delayLine2.size() - std::max(m_readIdx2, m_writeIdx2);
            size_t remBlock = count - samplesProcessed;
            
            size_t chunkSize = std::min({remRing1, remRing2, remBlock});

            f32* outL = &leftSamples[samplesProcessed];
            f32* outR = &rightSamples[samplesProcessed];

            f32* w1 = &ring1[m_readIdx1];
            f32* w2 = &ring2[m_readIdx2];

            f32* vac1 = &ring1[m_writeIdx1];
            f32* vac2 = &ring2[m_writeIdx2];

            // Résolution de la matrice de diffusion nodale en SIMD
            #pragma omp simd
            for (size_t i = 0; i < chunkSize; ++i) {
                f32 v1_input_L = outL[i];
                f32 v1_input_R = outR[i];

                // 1. Lecture des tensions historiques des branches de retard
                f32 w1_hist = w1[i];
                f32 w2_hist = w2[i];

                // 2. Application de la matrice de diffusion de Hadamard (Mélange croisé)
                f32 mixed1 = (w1_hist + w2_hist) * INV_SQRT2;
                f32 mixed2 = (w1_hist - w2_hist) * INV_SQRT2;

                // 3. Résolution de la loi des nœuds (Normalisation passive G=1 -> multiplication par 0.5)
                f32 vA_output = (v1_input_L + (g * mixed1)) * 0.5f;
                f32 vB_output = (v1_input_R + (g * mixed2)) * 0.5f;

                // 4. Stockage des états et mise à jour du signal audio de sortie
                vac1[i] = vA_output;
                vac2[i] = vB_output;

                outL[i] = vA_output;
                outR[i] = vB_output;
            }

            // Progression des curseurs circulaires du réseau
            m_readIdx1 += chunkSize; m_writeIdx1 += chunkSize;
            m_readIdx2 += chunkSize; m_writeIdx2 += chunkSize;

            if (m_readIdx1 >= m_delayLine1.size()) m_readIdx1 = 0;
            if (m_writeIdx1 >= m_delayLine1.size()) m_writeIdx1 = 0;
            if (m_readIdx2 >= m_delayLine2.size()) m_readIdx2 = 0;
            if (m_writeIdx2 >= m_delayLine2.size()) m_writeIdx2 = 0;

            samplesProcessed += chunkSize;
        }
    }

    void Reset() {
        std::fill(m_delayLine1.begin(), m_delayLine1.end(), 0.0f);
        std::fill(m_delayLine2.begin(), m_delayLine2.end(), 0.0f);
        m_readIdx1 = m_writeIdx1 = m_readIdx2 = m_writeIdx2 = 0;
    }

    void UpdateParameters(int delay1, int delay2, f32 feedback) {
        if (delay1 != m_delay1Samples) {
            m_delay1Samples = delay1;
            m_delayLine1.assign(m_delay1Samples, 0.0f);
            m_readIdx1 = m_writeIdx1 = 0;
        }
        if (delay2 != m_delay2Samples) {
            m_delay2Samples = delay2;
            m_delayLine2.assign(m_delay2Samples, 0.0f);
            m_readIdx2 = m_writeIdx2 = 0;
        }
        m_feedbackGain = std::clamp(feedback, 0.0f, 0.98f);
    }
};


