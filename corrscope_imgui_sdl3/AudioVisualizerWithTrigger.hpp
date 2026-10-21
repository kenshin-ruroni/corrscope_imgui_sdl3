#pragma once

#include "imgui.h"
#include "imfilebrowser.h" // Fourni via FetchContent de CMake
#include <SDL3/SDL.h>
#include <vector>
#include <string>
#include <algorithm>
#include <optional>
#include <memory>
#include <iostream>

#define DR_WAV_IMPLEMENTATION
#define DR_FLAC_IMPLEMENTATION
#define DR_MP3_IMPLEMENTATION

// Inclusion des en-têtes dr_libs (Les implémentations se trouvent dans dr_libs.cpp)
#include "dr_wav.h"
#include "dr_flac.h"
#include "dr_mp3.h"

// Inclusion de nos modules de traitement SIMD/C++26 créés précédemment
#include "TriggerBufferUpdate.hpp" 
#include "CrossCorrelation26.hpp" 

class AudioVisualizerWithTrigger 
{
private:
    std::vector<float> channelLeft;
    std::vector<float> channelRight;
    
    unsigned int sampleRate = 0;
    unsigned int channelsCount = 0;
    unsigned long long totalSampleCount = 0;
    std::string currentFilePath = "Aucun fichier chargé";
    bool isAudioLoaded = false;

    std::unique_ptr<CorrelationTrigger> triggerLeft;
    std::unique_ptr<CorrelationTrigger> triggerRight;

    int currentPlaybackPos = 0;
    int viewWindowSize = 2048; 
    int samplesPerFrame = 441; 
    bool isPlaybackPaused = false;

    // Paramètres gérés par l'interface ImGui
    int uiTsamp = 1024;  
    int uiStride = 1;    

    // Moteur de streaming audio natif SDL3
    SDL_AudioStream* audioStream = nullptr;

    // Instance du navigateur de fichiers ImGui
    ImGui::FileBrowser fileDialog;

    /**
     * Ferme et détruit proprement le périphérique et le flux audio SDL3
     */
    void CleanAudioStream() {
        if (audioStream) {
            SDL_DestroyAudioStream(audioStream);
            audioStream = nullptr;
        }
    }

    /**
     * Réalloue dynamiquement les déclencheurs de corrélation lors des changements de paramètres
     */
    void ReallocateTriggers() {
        if (!isAudioLoaded) return;
        
        // Garantir les contraintes structurelles minimales
        if (uiTsamp < 2) uiTsamp = 2;
        if (uiTsamp % 2 != 0) uiTsamp++; // tsamp doit obligatoirement être pair (A = B = tsamp/2)
        if (uiStride < 1) uiStride = 1;

        triggerLeft = std::make_unique<CorrelationTrigger>(uiTsamp, uiStride, samplesPerFrame, 0.5f, 0.3f, 1.0f);
        triggerRight = std::make_unique<CorrelationTrigger>(uiTsamp, uiStride, samplesPerFrame, 0.5f, 0.3f, 1.0f);
    }

public:
    AudioVisualizerWithTrigger() {
        // Paramétrage de la boîte de dialogue d'imgui-filebrowser
        fileDialog.SetTitle("Sélectionner un fichier audio");
        fileDialog.SetTypeFilters({ ".wav", ".flac", ".mp3" });
    }

    ~AudioVisualizerWithTrigger() {
        CleanAudioStream();
        channelLeft.clear();
        channelRight.clear();
    }

    /**
     * Charge et décode un fichier audio universel (WAV, FLAC, MP3) en mémoire
     */
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
            channelsCount = wav.channels;
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
            channelsCount = pFlac->channels;
            totalSampleCount = pFlac->totalPCMFrameCount;
            ProcessInterleavedFrames(tempBuffer.data(), pFlac->totalPCMFrameCount, pFlac->channels);
            drflac_close(pFlac);
            return true;
        }
        else if (ext == "mp3") {
            drmp3 mp3;
            if (!drmp3_init_file(&mp3, filename.c_str(), nullptr)) return false;
            uint64_t totalFrames = mp3.totalPCMFrameCount; 
            std::vector<float> tempBuffer(totalFrames * mp3.channels);
            drmp3_read_pcm_frames_f32(&mp3, totalFrames, tempBuffer.data());
            sampleRate = mp3.sampleRate;
            channelsCount = mp3.channels;
            totalSampleCount = totalFrames;
            ProcessInterleavedFrames(tempBuffer.data(), totalSampleCount, mp3.channels);
            drmp3_uninit(&mp3);
            return true;
        }
        
        currentFilePath = "Format non supporté";
        return false;
    }

    /**
     * Sépare les canaux entrelacés et initialise le périphérique de sortie audio SDL3
     */
    void ProcessInterleavedFrames(const float* buffer, unsigned long long frameCount, unsigned int channels) 
    {
        channelLeft.clear();
        channelRight.clear();
        channelLeft.reserve(frameCount);
        channelRight.reserve(frameCount);

        for (unsigned long long i = 0; i < frameCount; ++i) {
            if (channels >= 2) {
                channelLeft.push_back(buffer[i * channels]);
                channelRight.push_back(buffer[i * channels + 1]);
            } else { // Fichier Mono
                channelLeft.push_back(buffer[i]);
                channelRight.push_back(buffer[i]);
            }
        }

        // Configuration matérielle de la lecture audio via SDL3
        CleanAudioStream();
        SDL_AudioSpec srcSpec = { SDL_AUDIO_F32, (int)channels, (int)sampleRate };
        audioStream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &srcSpec, nullptr, nullptr);
        
        if (audioStream) {
            // Remplir le tampon matériel et démarrer immédiatement le flux
            SDL_PutAudioStreamData(audioStream, buffer, (int)(frameCount * channels * sizeof(float)));
            if (!isPlaybackPaused) {
                SDL_ResumeAudioStreamDevice(audioStream);
            }
        } else {
            std::cerr << "Erreur d'ouverture du périphérique audio SDL3 : " << SDL_GetError() << std::endl;
        }

        isAudioLoaded = true;
        currentPlaybackPos = 0;
        ReallocateTriggers();
    }

    /**
     * Génère l'analyse de synchronisation de phase et trace l'oscilloscope stabilisé
     */
    void DrawTriggeredOscilloscope(const char* label, const std::vector<float>& channelData, 
                                   CorrelationTrigger& triggerObj, ImVec2 size, ImU32 color) 
    {
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

        // 1. Calcul du point d'accroche stable via le déclencheur de corrélation croisée
        TriggerResult tr = triggerObj.get_trigger(channelData, currentPlaybackPos, 120);
        int stableStartIndex = tr.trigger;

        // 2. Traitement mathématique et mise à jour dynamique du tampon historique (TriggerBufferUpdate)
        CorrelationTrigger::PerFrameCache cache;
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
        cache.period = 40.0f; // Valeur par défaut pour l'écart-type de la Gaussienne

        std::vector<float> mathSegment(uiTsamp, 0.0f);
        for (int i = 0; i < uiTsamp; ++i) {
            size_t idx = stableStartIndex + i;
            if (idx < channelData.size()) {
                mathSegment[i] = channelData[idx];
            }
        }
        triggerObj._update_buffer(mathSegment, cache);

        // 3. Rendu géométrique de la forme d'onde avec Downsampling Min/Max anti-aliasing
        int widthPixels = static_cast<int>(size.x);
        float samplesPerPixel = static_cast<float>(viewWindowSize) / size.x;
        float midY = p0.y + (size.y / 2.0f);

        for (int x = 0; x < widthPixels - 1; ++x) {
            size_t startOffset = static_cast<size_t>(x * samplesPerPixel);
            size_t endOffset = static_cast<size_t>((x + 1) * samplesPerPixel);

            float minVal = 0.0f;
            float maxVal = 0.0f;
            bool hasData = false;

            for (size_t s = startOffset; s < endOffset && s < static_cast<size_t>(viewWindowSize); ++s) {
                size_t idx = stableStartIndex + s;
                if (idx < channelData.size()) {
                    float val = channelData[idx];
                    if (!hasData) { 
                        minVal = maxVal = val; 
                        hasData = true; 
                    } else { 
                        if (val < minVal) minVal = val; 
                        if (val > maxVal) maxVal = val; 
                    }
                }
            }

            float yTop = midY - (maxVal * (size.y / 2.0f));
            float yBottom = midY - (minVal * (size.y / 2.0f));
            drawList->AddLine(ImVec2(p0.x + x, yTop), ImVec2(p0.x + x, yBottom), color, 1.5f);
        }
        ImGui::Dummy(size);
    }

    /**
     * Construit et restitue la vue de l'interface utilisateur ImGui à chaque itération
     */
    void RenderUI() 
    {
        ImGui::Begin("Oscilloscope Audio Synchronisé (SIMD Trigger)");

        // Explorateur de fichiers
        ImGui::Text("Fichier actuel : %s", currentFilePath.c_str());
        ImGui::SameLine();
        if (ImGui::Button("Parcourir...")) {
            fileDialog.Open();
        }

        ImGui::Separator();

        if (isAudioLoaded) {
            // Informations fichier
            ImGui::Text("Fréquence : %u Hz | Canaux : %u | Taille : %llu échantillons",
                        sampleRate, channelsCount, totalSampleCount);

            // --- CURSEURS INTERACTIFS DES PARAMÈTRES (441, 1024, 1) ---
            if (ImGui::CollapsingHeader("Configuration Avancée du Déclencheur", ImGuiTreeNodeFlags_DefaultOpen)) {
                bool triggerParamsChanged = false;
                if (ImGui::SliderInt("Avance par Image", &samplesPerFrame, 64, 2048, "%d samples")) triggerParamsChanged = true;
                if (ImGui::SliderInt("Taille Analyse (tsamp)", &uiTsamp, 128, 4096, "%d samples")) {
                    if (uiTsamp % 2 != 0) uiTsamp++;
                    triggerParamsChanged = true;
                }
                if (ImGui::SliderInt("Sous-échantillonnage (stride)", &uiStride, 1, 8, "1/%d")) triggerParamsChanged = true;

                if (triggerParamsChanged) {
                    ReallocateTriggers();
                }
            }

            ImGui::Separator();

            // --- CONTRÔLES DE LECTURE AUDIO ---
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
            }

            // Avancement de la tête de lecture selon le timing matériel
            if (!isPlaybackPaused) {
                currentPlaybackPos += samplesPerFrame;
                // CORRECTION 1 : Ajout de <int> pour le static_cast
                if (currentPlaybackPos >= static_cast<int>(channelLeft.size())) {
                    currentPlaybackPos = 0; // Boucler au début du fichier
                }
            }

            // Slider de Zoom et barre de progression
            ImGui::SliderInt("Fenêtre d'affichage (Zoom)", &viewWindowSize, 256, 4096, "%d samples");

            // CORRECTION 2 : Ajout de <float> pour le static_cast
            float progress = static_cast<float>(currentPlaybackPos) / channelLeft.size();
            ImGui::ProgressBar(progress, ImVec2(-1.0f, 0.0f));

            // Rendu graphique des canaux séparés Gauche / Droite
            ImVec2 widgetSize(ImGui::GetContentRegionAvail().x, 160.0f);
            ImGui::Spacing();
            DrawTriggeredOscilloscope("Canal GAUCHE (L) - Stabilisé via SIMD", channelLeft, *triggerLeft, widgetSize, IM_COL32(0, 255, 200, 255));
            ImGui::Spacing();
            DrawTriggeredOscilloscope("Canal DROIT (R) - Stabilisé via SIMD", channelRight, *triggerRight, widgetSize, IM_COL32(255, 180, 0, 255));
        } else {
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.3f, 1.0f), "Veuillez charger un fichier pour démarrer l'analyse de phase.");
        }

        ImGui::End();

        // Rendu impératif et gestion de la sélection d'imgui-filebrowser
        fileDialog.Display();
        if (fileDialog.HasSelected()) {
            LoadAudioFile(fileDialog.GetSelected().string());
            fileDialog.ClearSelected();
        }
    }

};






