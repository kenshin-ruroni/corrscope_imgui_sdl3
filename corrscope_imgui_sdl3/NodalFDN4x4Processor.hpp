#pragma once

#include <vector>
#include <algorithm>
#include <cmath>

using f32 = float;

class NodalFDN4x4Processor
{
private:
    // 4 lignes de retard distinctes (tailles asymétriques pour maximiser la densité d'échos)
    std::vector<f32> m_delayLine1;
    std::vector<f32> m_delayLine2;
    std::vector<f32> m_delayLine3;
    std::vector<f32> m_delayLine4;

    size_t m_readIdx1 = 0, m_writeIdx1 = 0;
    size_t m_readIdx2 = 0, m_writeIdx2 = 0;
    size_t m_readIdx3 = 0, m_writeIdx3 = 0;
    size_t m_readIdx4 = 0, m_writeIdx4 = 0;

    int m_d1, m_d2, m_d3, m_d4;
    f32 m_feedbackGain;

public:
    NodalFDN4x4Processor(int d1, int d2, int d3, int d4, f32 feedback)
        : m_d1(d1), m_d2(d2), m_d3(d3), m_d4(d4), m_feedbackGain(feedback)
    {
        m_delayLine1.assign(m_d1 > 0 ? m_d1 : 1, 0.0f);
        m_delayLine2.assign(m_d2 > 0 ? m_d2 : 1, 0.0f);
        m_delayLine3.assign(m_d3 > 0 ? m_d3 : 1, 0.0f);
        m_delayLine4.assign(m_d4 > 0 ? m_d4 : 1, 0.0f);
    }

    NodalFDN4x4Processor(int baseDelay = 2800, f32 feedback = 0.70f)
        : NodalFDN4x4Processor(
              baseDelay,
              static_cast<int>(baseDelay * 1.31f),
              static_cast<int>(baseDelay * 1.63f),
              static_cast<int>(baseDelay * 2.11f),
              feedback) {}

    /**
     * Résout la matrice de diffusion Hadamard 4x4 en OpenMP SIMD par blocs.
     * Les entrées L et R sont réparties sur les 4 nœuds pour créer une spatialisation immersive.
     */
    void ProcessFDN4x4BufferSIMD(f32 *leftSamples, f32 *rightSamples, size_t count)
    {
        if (count == 0)
            return;

        f32 g = m_feedbackGain;
        f32 *r1 = m_delayLine1.data();
        f32 *r2 = m_delayLine2.data();
        f32 *r3 = m_delayLine3.data();
        f32 *r4 = m_delayLine4.data();

        size_t samplesProcessed = 0;

        while (samplesProcessed < count)
        {
            // Détermination du chunk mémoire contigu minimal
            size_t rem1 = m_delayLine1.size() - std::max(m_readIdx1, m_writeIdx1);
            size_t rem2 = m_delayLine2.size() - std::max(m_readIdx2, m_writeIdx2);
            size_t rem3 = m_delayLine3.size() - std::max(m_readIdx3, m_writeIdx3);
            size_t rem4 = m_delayLine4.size() - std::max(m_readIdx4, m_writeIdx4);
            size_t remBlock = count - samplesProcessed;

            size_t chunkSize = std::min({rem1, rem2, rem3, rem4, remBlock});

            f32 *outL = &leftSamples[samplesProcessed];
            f32 *outR = &rightSamples[samplesProcessed];

// Résolution vectorielle de l'analyse nodale 4x4
#pragma omp simd
            for (size_t i = 0; i < chunkSize; ++i)
            {
                // Tensions d'entrée
                f32 xL = outL[i];
                f32 xR = outR[i];

                // 1. Extraction des tensions historiques passées
                f32 w1 = r1[m_readIdx1 + i];
                f32 w2 = r2[m_readIdx2 + i];
                f32 w3 = r3[m_readIdx3 + i];
                f32 f32_w4 = r4[m_readIdx4 + i];

                // 2. Application de la matrice orthogonale de Hadamard 4x4 (Mélange parfait sans perte)
                // Note : Le facteur de normalisation de la matrice 4x4 est 0.5f
                f32 mixed1 = (w1 + w2 + w3 + f32_w4) * 0.5f;
                f32 mixed2 = (w1 - w2 + w3 - f32_w4) * 0.5f;
                f32 mixed3 = (w1 + w2 - w3 - f32_w4) * 0.5f;
                f32 mixed4 = (w1 - w2 - w3 + f32_w4) * 0.5f;

                // 3. Résolution de la loi des nœuds (Normalisation passive G=1 -> facteur 0.5f)
                // On injecte l'entrée stéréo de manière croisée pour maximiser l'effet 3D
                f32 vA = (xL + (g * mixed1)) * 0.5f;
                f32 vB = (xR + (g * mixed2)) * 0.5f;
                f32 vC = (xL + (g * mixed3)) * 0.5f;
                f32 vD = (xR + (g * mixed4)) * 0.5f;

                // 4. Écriture des nouvelles variables d'état
                r1[m_writeIdx1 + i] = vA;
                r2[m_writeIdx2 + i] = vB;
                r3[m_writeIdx3 + i] = vC;
                r4[m_writeIdx4 + i] = vD;

                // 5. Reconstruction de la sortie stéréo finale (Mélange acoustique des sommateurs)
                outL[i] = (vA + vC) * 0.707f;
                outR[i] = (vB + vD) * 0.707f;
            }

            // Progression des curseurs circulaires
            m_readIdx1 += chunkSize;
            m_writeIdx1 += chunkSize;
            m_readIdx2 += chunkSize;
            m_writeIdx2 += chunkSize;
            m_readIdx3 += chunkSize;
            m_writeIdx3 += chunkSize;
            m_readIdx4 += chunkSize;
            m_writeIdx4 += chunkSize;

            if (m_readIdx1 >= m_delayLine1.size())
                m_readIdx1 = 0;
            if (m_writeIdx1 >= m_delayLine1.size())
                m_writeIdx1 = 0;
            if (m_readIdx2 >= m_delayLine2.size())
                m_readIdx2 = 0;
            if (m_writeIdx2 >= m_delayLine2.size())
                m_writeIdx2 = 0;
            if (m_readIdx3 >= m_delayLine3.size())
                m_readIdx3 = 0;
            if (m_writeIdx3 >= m_delayLine3.size())
                m_writeIdx3 = 0;
            if (m_readIdx4 >= m_delayLine4.size())
                m_readIdx4 = 0;
            if (m_writeIdx4 >= m_delayLine4.size())
                m_writeIdx4 = 0;

            samplesProcessed += chunkSize;
        }
    }

    void Reset()
    {
        std::fill(m_delayLine1.begin(), m_delayLine1.end(), 0.0f);
        std::fill(m_delayLine2.begin(), m_delayLine2.end(), 0.0f);
        std::fill(m_delayLine3.begin(), m_delayLine3.end(), 0.0f);
        std::fill(m_delayLine4.begin(), m_delayLine4.end(), 0.0f);
        m_readIdx1 = m_writeIdx1 = m_readIdx2 = m_writeIdx2 = 0;
        m_readIdx3 = m_writeIdx3 = m_readIdx4 = m_writeIdx4 = 0;
    }

    void UpdateParameters(int d1, int d2, int d3, int d4, f32 feedback)
    {
        if (d1 != m_d1)
        {
            m_d1 = d1;
            m_delayLine1.assign(m_d1 > 0 ? m_d1 : 1, 0.0f);
            m_readIdx1 = m_writeIdx1 = 0;
        }
        if (d2 != m_d2)
        {
            m_d2 = d2;
            m_delayLine2.assign(m_d2 > 0 ? m_d2 : 1, 0.0f);
            m_readIdx2 = m_writeIdx2 = 0;
        }
        if (d3 != m_d3)
        {
            m_d3 = d3;
            m_delayLine3.assign(m_d3 > 0 ? m_d3 : 1, 0.0f);
            m_readIdx3 = m_writeIdx3 = 0;
        }
        if (d4 != m_d4)
        {
            m_d4 = d4;
            m_delayLine4.assign(m_d4 > 0 ? m_d4 : 1, 0.0f);
            m_readIdx4 = m_writeIdx4 = 0;
        }
        m_feedbackGain = std::clamp(feedback, 0.0f, 0.98f);
    }
};
