#pragma once

#include "imgui.h"
#include "imfilebrowser.h"
#include <SDL3/SDL.h>
#include <vector>
#include <string>
#include <algorithm>
#include <optional>
#include <memory>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <thread>  // AJOUT : Pour la gestion du Threading
#include <atomic> 

#define DR_WAV_IMPLEMENTATION
#define DR_FLAC_IMPLEMENTATION
#define DR_MP3_IMPLEMENTATION

#include "dr_wav.h"
#include "dr_flac.h"
#include "dr_mp3.h"

#include "TriggerBufferUpdate.hpp" 
#include "CrossCorrelation26.hpp" 

// Structure pour stocker l'historique et les métadonnées des morceaux chargés
struct AudioTrack {
    std::string filePath;
    std::string fileName;
    unsigned int sampleRate = 0;
    unsigned int channels = 0;
    unsigned int bitDepth = 0; // 0 pour le MP3 (débit variable)
    unsigned long long totalFrames = 0;
    std::string durationStr;

    // Calcule la chaîne de caractères au format mm:ss
    void ComputeDurationString() {
        if (sampleRate == 0) { durationStr = "00:00"; return; }
        unsigned long long totalSeconds = totalFrames / sampleRate;
        unsigned int minutes = static_cast<unsigned int>(totalSeconds / 60);
        unsigned int seconds = static_cast<unsigned int>(totalSeconds % 60);
        
        std::stringstream ss;
        ss << std::setw(2) << std::setfill('0') << minutes << ":"
           << std::setw(2) << std::setfill('0') << seconds;
        durationStr = ss.str();
    }
};

class AudioVisualizerWithTrigger {
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

    int uiTsamp = 1024;  
    int uiStride = 1;    

    SDL_AudioStream* audioStream = nullptr;
    ImGui::FileBrowser fileDialog;

    std::vector<AudioTrack> playlist;
    int selectedTrackIndex = -1;

    // --- VARIABLES DE THREADING ET CHARGEMENT ---
    std::thread loadingThread;
    std::atomic<bool> isFileLoading{false};
    std::string lastLoadingError = "";

    void CleanAudioStream() {
        if (audioStream) {
            SDL_DestroyAudioStream(audioStream);
            audioStream = nullptr;
        }
    }

    void ReallocateTriggers() {
        if (!isAudioLoaded) return;
        if (uiTsamp < 2) uiTsamp = 2;
        if (uiTsamp % 2 != 0) uiTsamp++;
        if (uiStride < 1) uiStride = 1;

        triggerLeft = std::make_unique<CorrelationTrigger>(uiTsamp, uiStride, samplesPerFrame, 0.5f, 0.3f, 1.0f);
        triggerRight = std::make_unique<CorrelationTrigger>(uiTsamp, uiStride, samplesPerFrame, 0.5f, 0.3f, 1.0f);
    }

    /**
     * Cœur binaire de l'ancien LoadAudioFile, s'exécutant désormais de façon asynchrone
     */
    void InternalLoadTrack(const std::string filename) {
        std::string ext = filename.substr(filename.find_last_of(".") + 1);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        AudioTrack track;
        track.filePath = filename;
        track.fileName = filename.substr(filename.find_last_of("/\\") + 1);

        if (ext == "wav") {
            drwav wav;
            if (!drwav_init_file(&wav, filename.c_str(), nullptr)) {
                lastLoadingError = "Impossible d'initialiser le fichier WAV.";
                isFileLoading = false;
                return;
            }
            if (wav.totalPCMFrameCount == 0 || wav.channels == 0 || wav.totalPCMFrameCount > 600000000ULL) {
                lastLoadingError = "Taille WAV invalide ou corrompue.";
                drwav_uninit(&wav);
                isFileLoading = false;
                return;
            }

            std::vector<float> tempBuffer(wav.totalPCMFrameCount * wav.channels);
            drwav_read_pcm_frames_f32(&wav, wav.totalPCMFrameCount, tempBuffer.data());
            
            track.sampleRate = wav.sampleRate;
            track.channels = wav.channels;
            track.bitDepth = wav.bitsPerSample;
            track.totalFrames = wav.totalPCMFrameCount;
            track.ComputeDurationString();

            ProcessInterleavedFrames(tempBuffer.data(), wav.totalPCMFrameCount, wav.channels, track);
            drwav_uninit(&wav);
        } 
        else if (ext == "flac") {
            drflac* pFlac = drflac_open_file(filename.c_str(), nullptr);
            if (!pFlac) {
                lastLoadingError = "Impossible d'ouvrir le fichier FLAC.";
                isFileLoading = false;
                return;
            }
            unsigned long long flacFrames = pFlac->totalPCMFrameCount;
            if (flacFrames == 0 || pFlac->channels == 0 || flacFrames > 600000000ULL) {
                lastLoadingError = "Taille FLAC invalide ou en dehors des limites.";
                drflac_close(pFlac);
                isFileLoading = false;
                return;
            }

            std::vector<float> tempBuffer(flacFrames * pFlac->channels);
            drflac_read_pcm_frames_f32(pFlac, flacFrames, tempBuffer.data());
            
            track.sampleRate = pFlac->sampleRate;
            track.channels = pFlac->channels;
            track.bitDepth = pFlac->bitsPerSample;
            track.totalFrames = flacFrames;
            track.ComputeDurationString();

            ProcessInterleavedFrames(tempBuffer.data(), flacFrames, pFlac->channels, track);
            drflac_close(pFlac);
        }
        else if (ext == "mp3") {
            drmp3 mp3;
            if (!drmp3_init_file(&mp3, filename.c_str(), nullptr)) {
                lastLoadingError = "Impossible d'initialiser le fichier MP3.";
                isFileLoading = false;
                return;
            }
            
            uint64_t totalFrames = drmp3_get_pcm_frame_count(&mp3); 
            if (totalFrames == 0 || mp3.channels == 0 || totalFrames > 600000000ULL) {
                lastLoadingError = "Taille MP3 invalide ou scan corrompu.";
                drmp3_uninit(&mp3);
                isFileLoading = false;
                return;
            }

            std::vector<float> tempBuffer(totalFrames * mp3.channels);
            drmp3_read_pcm_frames_f32(&mp3, totalFrames, tempBuffer.data());
            
            track.sampleRate = mp3.sampleRate;
            track.channels = mp3.channels;
            track.bitDepth = 0; 
            track.totalFrames = totalFrames;
            track.ComputeDurationString();

            ProcessInterleavedFrames(tempBuffer.data(), totalFrames, mp3.channels, track);
            drmp3_uninit(&mp3);
        } else {
            lastLoadingError = "Format d'extension non supporté.";
            isFileLoading = false;
            return;
        }

        // Section critique : Injection sécurisée dans la playlist principale
        auto it = std::find_if(playlist.begin(), playlist.end(), [&](const AudioTrack& t) {
            return t.filePath == filename;
        });
        if (it == playlist.end()) {
            playlist.push_back(track);
            selectedTrackIndex = static_cast<int>(playlist.size()) - 1;
        } else {
            selectedTrackIndex = static_cast<int>(std::distance(playlist.begin(), it));
        }

        currentFilePath = track.fileName;
        
        // Indiquer la fin définitive de l'accès au thread de chargement
        isFileLoading = false; 
    }

public:
    AudioVisualizerWithTrigger() {
        fileDialog.SetTitle("Ajouter un morceau à la liste");
        fileDialog.SetTypeFilters({ ".wav", ".flac", ".mp3" });
    }

    ~AudioVisualizerWithTrigger() {
        // Sécurité : Attendre l'arrêt du thread en cas de fermeture brutale de la fenêtre
        if (loadingThread.joinable()) {
            loadingThread.join();
        }
        CleanAudioStream();
        channelLeft.clear();
        channelRight.clear();
    }

    /**
     * Lance le décodage du fichier audio dans un thread détaché d'ImGui
     */
    bool StartAsyncLoad(const std::string& filename) {
        if (isFileLoading) return false; // Déjà un chargement en cours

        // Nettoyer les erreurs précédentes
        lastLoadingError = "";
        isFileLoading = true;

        // Si un thread précédent est resté actif, on le libère proprement
        if (loadingThread.joinable()) {
            loadingThread.join();
        }

        // Lancement du thread asynchrone C++
        loadingThread = std::thread(&AudioVisualizerWithTrigger::InternalLoadTrack, this, filename);
        return true;
    }

    void ProcessInterleavedFrames(const float* buffer, unsigned long long frameCount, unsigned int channels, const AudioTrack& track) 
    {
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

        CleanAudioStream();
        sampleRate = track.sampleRate;
        channelsCount = track.channels;
        totalSampleCount = track.totalFrames;

        SDL_AudioSpec srcSpec = { SDL_AUDIO_F32, (int)channels, (int)sampleRate };
        audioStream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &srcSpec, nullptr, nullptr);
        
        if (audioStream) 
        {
            SDL_PutAudioStreamData(audioStream, buffer, (int)(frameCount * channels * sizeof(float)));
            if (!isPlaybackPaused) SDL_ResumeAudioStreamDevice(audioStream);
        }
        isAudioLoaded = true;
        currentPlaybackPos = 0;
        ReallocateTriggers();
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

        TriggerResult tr = triggerObj.get_trigger(channelData, currentPlaybackPos, 120);
        int stableStartIndex = tr.trigger;

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
        cache.period = 40.0f; 

        std::vector<float> mathSegment(uiTsamp, 0.0f);
        for (int i = 0; i < uiTsamp; ++i) {
            size_t idx = stableStartIndex + i;
            if (idx < channelData.size()) mathSegment[i] = channelData[idx];
        }
        triggerObj._update_buffer(mathSegment, cache);

        // CORRECTION : static_cast<int> et static_cast<float> corrigés ici
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

    void RenderUI() 
    {
        ImGui::Begin("Oscilloscope Audio Synchronisé (SIMD Trigger)");

        // 1. GESTION DU BANDEAU D'ARRIÈRE-PLAN
        if (isFileLoading.load()) {
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 1.0f, 1.0f), "🔄 Loading file... Veuillez patienter.");
            ImGui::SameLine();
            ImGui::TextDisabled("(Le décodage s'exécute de façon asynchrone sur un thread CPU)");
        } else if (!lastLoadingError.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.2f, 0.2f, 1.0f), "❌ Erreur : %s", lastLoadingError.c_str());
        } else {
            ImGui::Text("Fichier actuel : %s", currentFilePath.c_str());
        }

        ImGui::SameLine();
        if (ImGui::Button("Importer un nouveau fichier...")) {
            if (!isFileLoading.load()) fileDialog.Open();
        }

        ImGui::Separator();

        // 2. BANQUE DE LECTURE (TREEVIEW DE LA PLAYLIST)
        ImGui::Text("Bibliothèque de morceaux chargés :");
        ImVec2 treeSize(ImGui::GetContentRegionAvail().x, 120.0f);
        
        ImGui::BeginChild("TreeViewChild", treeSize, true);
        if (ImGui::TreeNodeEx("Mes Fichiers Audio", ImGuiTreeNodeFlags_DefaultOpen)) 
        {
            for (size_t i = 0; i < playlist.size(); ++i) {
                const auto& track = playlist[i];
                
                std::string bitDepthInfo = (track.bitDepth > 0) ? std::to_string(track.bitDepth) + "-bit" : "VBR";
                std::string chInfo = (track.channels == 2) ? "Stéréo" : (track.channels == 1 ? "Mono" : std::to_string(track.channels) + "ch");
                std::string itemText = track.fileName + " [" + chInfo + " | " + track.durationStr + " | " + bitDepthInfo + "]";
                
                ImGuiTreeNodeFlags nodeFlags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
                
                // CORRECTION : static_cast<int> réintégré
                if (static_cast<int>(i) == selectedTrackIndex) nodeFlags |= ImGuiTreeNodeFlags_Selected;

                ImGui::TreeNodeEx((void*)(intptr_t)i, nodeFlags, "%s", itemText.c_str());
                
                if (ImGui::IsItemClicked() && !isFileLoading.load()) {
                    // CORRECTION : static_cast<int> réintégré
                    selectedTrackIndex = static_cast<int>(i);
                    StartAsyncLoad(track.filePath);
                }
            }
            ImGui::TreePop();
        }
        ImGui::EndChild();

        ImGui::Separator();

        if (isAudioLoaded) {
            // --- CURSEURS INTERACTIFS DES PARAMÈTRES (441, 1024, 1) ---
            if (ImGui::CollapsingHeader("Configuration Avancée du Déclencheur", ImGuiTreeNodeFlags_DefaultOpen)) {
                bool triggerParamsChanged = false;
                if (ImGui::SliderInt("Avance par Image", &samplesPerFrame, 64, 2048, "%d samples")) triggerParamsChanged = true;
                if (ImGui::SliderInt("Taille Analyse (tsamp)", &uiTsamp, 128, 4096, "%d samples")) {
                    if (uiTsamp % 2 != 0) uiTsamp++;
                    triggerParamsChanged = true;
                }
                if (ImGui::SliderInt("Sous-échantillonnage (stride)", &uiStride, 1, 8, "1/%d")) triggerParamsChanged = true;
                if (triggerParamsChanged) ReallocateTriggers();
            }

            ImGui::Separator();

            // --- CONTRÔLES DE LECTURE ---
            if (ImGui::Button(isPlaybackPaused ? "Reprendre" : "Pause")) {
                isPlaybackPaused = !isPlaybackPaused;
                if (audioStream) {
                    if (isPlaybackPaused) SDL_PauseAudioStreamDevice(audioStream);
                    else SDL_ResumeAudioStreamDevice(audioStream);
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Reset Position")) currentPlaybackPos = 0;

            if (!isPlaybackPaused) {
                currentPlaybackPos += samplesPerFrame;
                // CORRECTION : static_cast<int> réintégré
                if (currentPlaybackPos >= static_cast<int>(channelLeft.size())) currentPlaybackPos = 0;
            }

            ImGui::SliderInt("Fenêtre d'affichage (Zoom)", &viewWindowSize, 256, 4096, "%d samples");
            
            // CORRECTION : static_cast<float> réintégré
            float progress = static_cast<float>(currentPlaybackPos) / channelLeft.size();
            ImGui::ProgressBar(progress, ImVec2(-1.0f, 0.0f));

            ImVec2 widgetSize(ImGui::GetContentRegionAvail().x, 160.0f);
            ImGui::Spacing();
            DrawTriggeredOscilloscope("Canal GAUCHE (L) - Aligné SIMD", channelLeft, *triggerLeft, widgetSize, IM_COL32(0, 255, 200, 255));
            ImGui::Spacing();
            DrawTriggeredOscilloscope("Canal DROIT (R) - Aligné SIMD", channelRight, *triggerRight, widgetSize, IM_COL32(255, 180, 0, 255));
        } else {
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.3f, 1.0f), "Veuillez importer un fichier pour démarrer l'analyse de phase.");
        }

        ImGui::End();

        fileDialog.Display();
        if (fileDialog.HasSelected()) {
            StartAsyncLoad(fileDialog.GetSelected().string());
            fileDialog.ClearSelected();
        }
    }
};