#pragma once

#include <vector>
#include <algorithm>
#include <cmath>

using f32 = float;

class NodalReverbProcessor {
private:
    std::vector<f32> m_delayLine; // Registre d'état des tensions de nœuds passées
    size_t m_readIndex = 0;        // Index de lecture circulaire
    size_t m_writeIndex = 0;       // Index d'écriture circulaire
    
    int m_delaySamples;           // m : Longueur du retard (taille du circuit étendu)
    f32 m_feedbackGain;           // g : Coefficient de rétroaction nodale

public:
    /**
     * @param delay m : retard en échantillons (longueur du Ring Buffer nodal)
     * @param feedback g : gain de réinjection nodale
     */
    NodalReverbProcessor(int delay, f32 feedback) 
        : m_delaySamples(delay), m_feedbackGain(feedback) {
        if (m_delaySamples < 1) m_delaySamples = 1;
        m_delayLine.assign(m_delaySamples, 0.0f);
    }

    /**
     * Applique la résolution de l'analyse nodale sur un bloc d'échantillons.
     * Le code est optimisé pour permettre la vectorisation de la gestion du tampon.
     * 
     * @param samples Tableau d'échantillons d'entrée/sortie (In-place processing)
     * @param count Taille du bloc de trame (ex: 441)
     */
    void ProcessNodalBufferSIMD(f32* samples, size_t count) {
        if (count == 0) return;

        f32 g = m_feedbackGain;
        size_t delaySize = m_delayLine.size();
        f32* ring_ptr = m_delayLine.data();

        // Variables d'analyse nodale résolues algébriquement à l'avance pour la matrice A
        // Le dénominateur nodal combiné de notre matrice d'admittance inversée :
        f32 inverse_nodal_admittance = 1.0f; 

        size_t samplesProcessed = 0;

        while (samplesProcessed < count) {
            // Tronçonner le bloc selon les limites du tampon circulaire pour garantir
            // des accès mémoire alignés, contigus et vectorisables par le CPU
            size_t remainingInRing = delaySize - std::max(m_readIndex, m_writeIndex);
            size_t remainingInBlock = count - samplesProcessed;
            size_t chunkSize = std::min(remainingInRing, remainingInBlock);

            f32* inputOutputSegment = &samples[samplesProcessed];
            f32* stateVectorSegment = &ring_ptr[m_readIndex];
            f32* updateVectorSegment = &ring_ptr[m_writeIndex];

            // Exécution vectorielle de la résolution du système matriciel nodal
            #pragma omp simd
            for (size_t i = 0; i < chunkSize; ++i) {
                // 1. Extraction du vecteur source historique B(z) depuis le nœud 3 (V3)
                f32 v3_historical = stateVectorSegment[i];

                // 2. Résolution instantanée de la tension au nœud d'entrée V1
                f32 v1_input = inputOutputSegment[i];

                // 3. Résolution du système linéaire nodale pour le nœud de sortie V2 / Sortie audio Y(n)
                // Équivalent à multiplier le vecteur B par la matrice d'admittance inversée A^-1
                f32 v2_output = (v1_input + (g * v3_historical)) * inverse_nodal_admittance;

                // 4. Écriture de la nouvelle tension nodale calculée dans le tampon d'état
                updateVectorSegment[i] = v2_output;

                // 5. Mise à jour de l'échantillon audio sur place
                inputOutputSegment[i] = v2_output;
            }

            // Progression des index circulaires nodaux
            m_readIndex += chunkSize;
            m_writeIndex += chunkSize;

            if (m_readIndex >= delaySize) m_readIndex = 0;
            if (m_writeIndex >= delaySize) m_writeIndex = 0;

            samplesProcessed += chunkSize;
        }
    }

    /**
     * Purge instantanée des nœuds du circuit (Utile lors d'un seek ou arrêt de lecture)
     */
    void ResetNodalCircuit() {
        std::fill(m_delayLine.begin(), m_delayLine.end(), 0.0f);
        m_readIndex = 0;
        m_writeIndex = 0;
    }

    void UpdateNodalParameters(int delay, f32 feedback) {
        if (delay != m_delaySamples) {
            m_delaySamples = delay;
            if (m_delaySamples < 1) m_delaySamples = 1;
            m_delayLine.assign(m_delaySamples, 0.0f);
            m_readIndex = 0;
            m_writeIndex = 0;
        }
        m_feedbackGain = std::clamp(feedback, 0.0f, 0.99f);
    }
};
