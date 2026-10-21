#pragma once
#include "imgui.h"
#include <vector>
#include <string>
#include <algorithm>
#include <optional>
#include <memory>

#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"

#define DR_FLAC_IMPLEMENTATION
#include "dr_flac.h"

#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"

#include "TriggerBufferUpdate.hpp" 
#include "CrossCorrelation26.hpp" 

class AudioVisualizerWithTrigger {
private:
    std::vector<float> channelLeft;
    std::vector<float> channelRight;
    
    unsigned int sampleRate = 0;
    size_t totalSampleCount = 0;
    unsigned int channelsCount = 0;

    std::string currentFilePath = "Aucun fichier charge";
    bool isAudioLoaded = false;

    std::unique_ptr<CorrelationTrigger> triggerLeft;
    std::unique_ptr<CorrelationTrigger> triggerRight;

    int currentPlaybackPos = 0;
    int viewWindowSize = 2048; 
    int samplesPerFrame = 441; 
    bool isPlaybackPaused = false;

    int uiTsamp = 1024;  
    int uiStride = 1;    

    void ReallocateTriggers() {
        if (!isAudioLoaded) return;

        if (uiTsamp < 2) uiTsamp = 2;
        if (uiTsamp % 2 != 0) uiTsamp++;
        if (uiStride < 1) uiStride = 1;

        triggerLeft = std::make_unique<CorrelationTrigger>(uiTsamp, uiStride, samplesPerFrame, 0.5f, 0.3f, 1.0f);
        triggerRight = std::make_unique<CorrelationTrigger>(uiTsamp, uiStride, samplesPerFrame, 0.5f, 0.3f, 1.0f);
    }

    void ProcessInterleavedFrames(const float* buffer, unsigned long long frameCount, unsigned int channels) {
        channelLeft.clear();
        channelRight.clear();
        channelLeft.reserve(frameCount);
        channelRight.reserve(frameCount);

        for (unsigned long long i = 0; i < frameCount; ++i) {
            if (channels >= 2) {
                channelLeft.push_back(buffer[i * channels]);
                channelRight.push_back(buffer[i * channels + 1]);
            } else {
                channelLeft.push_back(buffer[i]);
                channelRight.push_back(buffer[i]);
            }
        }

        isAudioLoaded = true;
        currentPlaybackPos = 0;


        CleanAudioStream(); // Fermer l'ancien flux si existant

        // Spécification du format d'entrée (celui de dr_libs : float32, canaux d'origine, sample rate d'origine)
        SDL_AudioSpec srcSpec = { SDL_AUDIO_F32, (int)channels, (int)sampleRate };
        
        // Demander à SDL3 d'ouvrir la sortie par défaut en adaptant automatiquement le format si nécessaire
        audioStream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &srcSpec, nullptr, nullptr);
        
        if (audioStream) {
            // Envoyer la totalité du morceau décodé dans le tampon de la carte son
            SDL_PutAudioStreamData(audioStream, buffer, (int)(frameCount * channels * sizeof(float)));
            
            // Démarrer la lecture si la pause n'est pas active
            if (!isPlaybackPaused) {
                SDL_ResumeAudioStreamDevice(audioStream);
            }
        }

        ReallocateTriggers();


    }

    SDL_AudioStream* audioStream = nullptr;

    // Ajoutez également cette petite méthode de nettoyage pour fermer la carte son proprement
    void CleanAudioStream() {
        if (audioStream) {
            SDL_DestroyAudioStream(audioStream);
            audioStream = nullptr;
        }
    }

public:
    AudioVisualizerWithTrigger() = default;

    ~AudioVisualizerWithTrigger() 
    {
    CleanAudioStream();
    channelLeft.clear();
    channelRight.clear();
    }
    bool LoadAudioFile(const std::string& filename) {
        isAudioLoaded = false;
        currentFilePath = filename;
        std::string ext = filename.substr(filename.find_last_of(".") + 1);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        if (ext == "wav") {
            drwav wav;
            if (!drwav_init_file(&wav, filename.c_str(), nullptr)) return false;
            std::vector<float> tempBuffer(wav.totalPCMFrameCount * wav.channels);
            drwav_read_pcm_frames_f32(&wav, wav.totalPCMFrameCount, tempBuffer.data());
            sampleRate = wav.sampleRate;
            totalSampleCount = wav.totalPCMFrameCount;
            ProcessInterleavedFrames(tempBuffer.data(), wav.totalPCMFrameCount, wav.channels);
            drwav_uninit(&wav);
            return true;
        } 
        else if (ext == "flac") {
            drflac* pFlac = drflac_open_file(filename.c_str(), nullptr);
            if (!pFlac) return false;
            std::vector<float> tempBuffer(pFlac->totalPCMFrameCount * pFlac->channels);
            drflac_read_pcm_frames_f32(pFlac, pFlac->totalPCMFrameCount, tempBuffer.data());
            sampleRate = pFlac->sampleRate;
            totalSampleCount = pFlac->totalPCMFrameCount;
            ProcessInterleavedFrames(tempBuffer.data(), pFlac->totalPCMFrameCount, pFlac->channels);
            drflac_close(pFlac);
            return true;
        }
        else if (ext == "mp3") {
            drmp3 mp3;
            if (!drmp3_init_file(&mp3, filename.c_str(), nullptr)) return false;

            // CORRECTION ICI : Remplacer drmp3_get_total_pcm_frame_count par mp3.totalPCMFrameCount
            uint64_t totalFrames = mp3.totalPCMFrameCount; 

            std::vector<float> tempBuffer(totalFrames * mp3.channels);
            drmp3_read_pcm_frames_f32(&mp3, totalFrames, tempBuffer.data());

            sampleRate = mp3.sampleRate;
            channelsCount = mp3.channels; // S'assurer que cette variable membre existe bien ou la déclarer localement
            totalSampleCount = totalFrames;

            ProcessInterleavedFrames(tempBuffer.data(), totalSampleCount, mp3.channels);
            drmp3_uninit(&mp3);
            return true;
        }
        return false;
    }

    void DrawTriggeredOscilloscope(const char* label, const std::vector<float>& channelData, 
                                   CorrelationTrigger& triggerObj, ImVec2 size, ImU32 color) {
        ImGui::Text("%s", label);
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImVec2 p1 = ImVec2(p0.x + size.x, p0.y + size.y);
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        
        drawList->AddRectFilled(p0, p1, IM_COL32(15, 15, 15, 255));
        drawList->AddLine(ImVec2(p0.x, p0.y + size.y / 2.0f), ImVec2(p1.x, p0.y + size.y / 2.0f), IM_COL32(45, 45, 45, 255));

        if (channelData.empty()) {
            ImGui::Dummy(size);
            return;
        }

        // 1. Appel au CorrelationTrigger pour trouver l'index aligné
        TriggerResult tr = triggerObj.get_trigger(channelData, currentPlaybackPos, 120);
        int stableStartIndex = tr.trigger;

        // 2. Préparation du cache pour la mise à jour dynamique du tampon
        CorrelationTrigger::PerFrameCache cache;
        
        // CORRECTION 1 : Le calcul de la moyenne et du buffer doit se faire sur la taille d'analyse 'uiTsamp'
        float sum = 0.0f;
        int count = 0;
        for (int i = 0; i < uiTsamp; ++i) {
            size_t idx = stableStartIndex + i;
            if (idx < channelData.size()) {
                sum += channelData[idx];
                count++;
            }
        }
        cache.result_mean = (count > 0) ? (sum / count) : 0.0f;
        cache.period = 40.0f; 

        // CORRECTION 2 : On alloue 'uiTsamp' éléments pour l'analyse mathématique du Trigger
        std::vector<float> mathSegment(uiTsamp, 0.0f);
        for (int i = 0; i < uiTsamp; ++i) {
            size_t idx = stableStartIndex + i;
            if (idx < channelData.size()) {
                mathSegment[i] = channelData[idx];
            }
        }
        // Cette fonction reçoit maintenant exactement un vecteur de taille uiTsamp. Plus de crash !
        triggerObj._update_buffer(mathSegment, cache);

        // =================================================================
        // 3. EXTRACTION SÉPARÉE POUR L'AFFICHAGE GRAPHIQUE (Zoom temporel)
        // =================================================================
        int widthPixels = static_cast<int>(size.x);
        float samplesPerPixel = static_cast<float>(viewWindowSize) / size.x;
        float midY = p0.y + (size.y / 2.0f);

        for (int x = 0; x < widthPixels - 1; ++x) {
            size_t startOffset = static_cast<size_t>(x * samplesPerPixel);
            size_t endOffset = static_cast<size_t>((x + 1) * samplesPerPixel);

            float minVal = 1.0f;
            float maxVal = -1.0f;

            // On pioche directement dans channelData sur la longueur de viewWindowSize
            for (size_t s = startOffset; s < endOffset && s < static_cast<size_t>(viewWindowSize); ++s) {
                size_t idx = stableStartIndex + s;
                if (idx < channelData.size()) {
                    float val = channelData[idx];
                    if (val < minVal) minVal = val;
                    if (val > maxVal) maxVal = val;
                }
            }

            if (minVal > maxVal) { minVal = 0.0f; maxVal = 0.0f; }

            float yTop = midY - (maxVal * (size.y / 2.0f));
            float yBottom = midY - (minVal * (size.y / 2.0f));

            drawList->AddLine(ImVec2(p0.x + x, yTop), ImVec2(p0.x + x, yBottom), color, 1.2f);
        }

        ImGui::Dummy(size);
    }



    // Callback interne requis pour le File Dialog de SDL3
    static void SDLCALL OnFileSelected(void* userdata, const char* const* filelist, int filter) {
        auto* self = static_cast<AudioVisualizerWithTrigger*>(userdata);
        if (filelist && filelist[0]) {
            self->LoadAudioFile(filelist[0]);
        }
    }

    void RenderUI() {
        ImGui::Begin("Oscilloscope Audio Synchronisé (SIMD Trigger)");

        // =================================================================
        // 1. EXPLORATEUR DE FICHIERS (FILE BROWSER NATIVE VIA SDL3)
        // =================================================================
        ImGui::Text("Fichier : %s", currentFilePath.c_str());
        ImGui::SameLine();
        
        if (ImGui::Button("Parcourir...")) {
            // Configuration des filtres de fichiers pour SDL3
            const SDL_DialogFileFilter filters[] = {
                { "Fichiers Audio (.wav, .flac, .mp3)", "wav;flac;mp3" },
                { "Tous les fichiers", "*" }
            };
            
            // Appel non-bloquant du File Dialog natif de SDL3
            SDL_ShowOpenFileDialog(OnFileSelected, this, nullptr, filters, 1, nullptr, false);
        }

        ImGui::Separator();

        if (isAudioLoaded) {

            // =================================================================
            // 2. SLIDERS DYNAMIQUES POUR LES PARAMÈTRES (441, 1024, 1)
            // =================================================================
            if (ImGui::CollapsingHeader("Paramètres du Moteur Temporel & Déclencheur", ImGuiTreeNodeFlags_DefaultOpen)) {
                bool parametersChanged = false;

                // Slider pour l'avance de trame (Anciennement 441 codé en dur)
                if (ImGui::SliderInt("Avance par Image (Trame)", &samplesPerFrame, 64, 2048, "%d samples")) {
                    parametersChanged = true;
                }
                ImGui::SameLine(); ImGui::TextDisabled("(Défaut: 441)");

                // Slider pour la taille d'analyse tsamp (Anciennement 1024 codé en dur)
                if (ImGui::SliderInt("Taille Analyse (tsamp)", &uiTsamp, 128, 4096, "%d samples")) {
                    // Contrainte algorithmique : forcer une valeur paire
                    if (uiTsamp % 2 != 0) uiTsamp++;
                    parametersChanged = true;
                }
                ImGui::SameLine(); ImGui::TextDisabled("(Défaut: 1024)");

                // Slider pour le sous-échantillonnage stride (Anciennement 1 codé en dur)
                if (ImGui::SliderInt("Sous-échantillonnage (stride)", &uiStride, 1, 8, "1/%d")) {
                    parametersChanged = true;
                }
                ImGui::SameLine(); ImGui::TextDisabled("(Défaut: 1)");

                // Si l'utilisateur manipule un des 3 sliders, on réalloue les buffers du déclencheur à la volée
                if (parametersChanged) {
                    ReallocateTriggers();
                }
            }

            ImGui::Separator();

            // =================================================================
            // 3. CONTRÔLES DE LECTURE ET OSCILLOSCOPES
            // =================================================================
            // --- CONTRÔLES DE LECTURE MODIFIÉS ---
            if (ImGui::Button(isPlaybackPaused ? "Reprendre" : "Pause")) {
                isPlaybackPaused = !isPlaybackPaused;
                if (audioStream) {
                    if (isPlaybackPaused) SDL_PauseAudioStreamDevice(audioStream);
                    else SDL_ResumeAudioStreamDevice(audioStream);
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Reset Position")) {
                currentPlaybackPos = 0;
                if (audioStream) SDL_ClearAudioStream(audioStream); 
                // Note : Pour un vrai Seek complet, il faudrait re-Put les données, 
                // mais Clear permet déjà de couper le son instantanément pour le reset.
            }

            // --- SYNCHRONISATION TECHNIQUE DE L'OSCILLOSCOPE ---
            if (!isPlaybackPaused && audioStream) 
            {
                // Au lieu de faire "+= samplesPerFrame", on demande à SDL combien d'octets 
                // ont déjà été consommés/joués par les haut-parleurs
                int queuedBytes = SDL_GetAudioStreamQueued(audioStream);
                int totalBytesInFile = totalSampleCount * channelsCount * sizeof(float);
                int playedBytes = totalBytesInFile - queuedBytes;
                
                if (playedBytes > 0) {
                    currentPlaybackPos = (playedBytes / sizeof(float)) / channelsCount;
                }

                if (queuedBytes == 0) {
                    currentPlaybackPos = 0; // Boucler visuellement si le morceau est fini
                }
            }

            ImGui::SliderInt("Fenêtre d'affichage (Zoom)", &viewWindowSize, 256, 4096);
            
            float progress = static_cast<float>(currentPlaybackPos) / channelLeft.size();
            ImGui::ProgressBar(progress, ImVec2(-1.0f, 0.0f));

            // Rendu des deux oscilloscopes géométriques stabilisés
            ImVec2 widgetSize(ImGui::GetContentRegionAvail().x, 160.0f);

            ImGui::Spacing();
            DrawTriggeredOscilloscope("Canal GAUCHE (L) - Stabilisé", channelLeft, *triggerLeft, widgetSize, IM_COL32(0, 255, 200, 255));
            
            ImGui::Spacing();
            DrawTriggeredOscilloscope("Canal DROIT (R) - Stabilisé", channelRight, *triggerRight, widgetSize, IM_COL32(255, 180, 0, 255));
        } else {
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.3f, 1.0f), "Veuillez ouvrir un fichier pour configurer le défilement des phases.");
        }

        ImGui::End();
    }
};
