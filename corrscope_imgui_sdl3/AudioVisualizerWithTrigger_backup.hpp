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

    // Banque de données pour le TreeView (Historique des morceaux importés)
    std::vector<AudioTrack> playlist;
    int selectedTrackIndex = -1;

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

public:
    AudioVisualizerWithTrigger() {
        fileDialog.SetTitle("Ajouter un morceau à la liste");
        fileDialog.SetTypeFilters({ ".wav", ".flac", ".mp3" });
    }

    ~AudioVisualizerWithTrigger() {
        CleanAudioStream();
        channelLeft.clear();
        channelRight.clear();
    }


    bool LoadAudioFile(const std::string& filename) {
        std::string ext = filename.substr(filename.find_last_of(".") + 1);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        AudioTrack track;
        track.filePath = filename;
        track.fileName = filename.substr(filename.find_last_of("/\\") + 1);

        if (ext == "wav") {
            drwav wav;
            if (!drwav_init_file(&wav, filename.c_str(), nullptr)) {
                std::cerr << "[Erreur] Impossible d'initialiser le fichier WAV: " << filename << std::endl;
                return false;
            }
            
            // SÉCURITÉ : Validation de la taille des données avant allocation
            if (wav.totalPCMFrameCount == 0 || wav.channels == 0 || wav.totalPCMFrameCount > 200000000ULL) {
                std::cerr << "[Erreur] Taille WAV invalide ou corrompue." << std::endl;
                drwav_uninit(&wav);
                return false;
            }

            std::vector<float> tempBuffer;
            try {
                tempBuffer.resize(wav.totalPCMFrameCount * wav.channels);
            } catch (const std::exception& e) {
                std::cerr << "[Erreur Allocation WAV] " << e.what() << std::endl;
                drwav_uninit(&wav);
                return false;
            }

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
                std::cerr << "[Erreur] Impossible d'ouvrir le fichier FLAC: " << filename << std::endl;
                return false;
            }
            
            // CORRECTION : Extraction fiable du nombre de trames via les métadonnées FLAC
            unsigned long long flacFrames = pFlac->totalPCMFrameCount;
            if (flacFrames == 0) {
                // Si l'en-tête est incomplet, on ne bloque pas, on estime ou on refuse poliment
                std::cerr << "[Erreur] Le fichier FLAC ne contient pas d'information de taille d'en-tête." << std::endl;
                drflac_close(pFlac);
                return false;
            }

            // Sécurité anti-débordement réaliste (autorise jusqu'à 3h de musique)
            if (pFlac->channels == 0 || flacFrames > 600000000ULL) {
                std::cerr << "[Erreur] Taille FLAC en dehors des limites autorisées." << std::endl;
                drflac_close(pFlac);
                return false;
            }

            std::vector<float> tempBuffer;
            try {
                tempBuffer.resize(flacFrames * pFlac->channels);
            } catch (const std::exception& e) {
                std::cerr << "[Erreur Allocation FLAC] " << e.what() << std::endl;
                drflac_close(pFlac);
                return false;
            }

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
                std::cerr << "[Erreur] Impossible d'initialiser le fichier MP3: " << filename << std::endl;
                return false;
            }
            
            // CORRECTION MAJEURE : Forcer dr_mp3 à scanner linéairement le fichier pour compter les trames réelles
            uint64_t totalFrames = drmp3_get_pcm_frame_count(&mp3); 
            
            // Validation avec la valeur calculée par le scan complet
            if (totalFrames == 0 || mp3.channels == 0 || totalFrames > 600000000ULL) {
                std::cerr << "[Erreur] Impossible de déterminer la taille du MP3 (Scan vicié ou fichier corrompu)." << std::endl;
                drmp3_uninit(&mp3);
                return false;
            }

            std::vector<float> tempBuffer;
            try {
                tempBuffer.resize(totalFrames * mp3.channels);
            } catch (const std::exception& e) {
                std::cerr << "[Erreur Allocation MP3] " << e.what() << std::endl;
                drmp3_uninit(&mp3);
                return false;
            }

            // Lire les données depuis le début du flux
            drmp3_read_pcm_frames_f32(&mp3, totalFrames, tempBuffer.data());
            
            track.sampleRate = mp3.sampleRate;
            track.channels = mp3.channels;
            track.bitDepth = 0; 
            track.totalFrames = totalFrames;
            track.ComputeDurationString();

            ProcessInterleavedFrames(tempBuffer.data(), totalFrames, mp3.channels, track);
            drmp3_uninit(&mp3);
        } else {
            return false;
        }
        // Ajouter à la liste de lecture si valide
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
        return true;
    }


    void ProcessInterleavedFrames(const float* buffer, unsigned long long frameCount, unsigned int channels, const AudioTrack& track) {
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
        
        if (audioStream) {
            SDL_PutAudioStreamData(audioStream, buffer, (int)(frameCount * channels * sizeof(float)));
            if (!isPlaybackPaused) SDL_ResumeAudioStreamDevice(audioStream);
        }

        isAudioLoaded = true;
        currentPlaybackPos = 0;
        ReallocateTriggers();
    }

    void DrawTriggeredOscilloscope(const char* label, const std::vector<float>& channelData, 
                                   CorrelationTrigger& triggerObj, ImVec2 size, ImU32 color) 
    {
        // ... (Conserver l'implémentation robuste anti-aliasing Min/Max identique) ...
        ImGui::Text("%s", label);
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImVec2 p1 = ImVec2(p0.x + size.x, p0.y + size.y);
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        
        drawList->AddRectFilled(p0, p1, IM_COL32(15, 15, 15, 255));
        drawList->AddLine(ImVec2(p0.x, p0.y + size.y / 2.0f), ImVec2(p1.x, p0.y + size.y / 2.0f), IM_COL32(45, 45, 45, 255));

        if (channelData.empty()) { ImGui::Dummy(size); return; }

        TriggerResult tr = triggerObj.get_trigger(channelData, currentPlaybackPos, 120);
        int stableStartIndex = tr.trigger;

        CorrelationTrigger::PerFrameCache cache;
        float sum = 0.0f; int count = 0;
        for (int i = 0; i < uiTsamp; ++i) {
            size_t idx = stableStartIndex + i;
            if (idx < channelData.size()) { sum += channelData[idx]; count++; }
        }
        cache.result_mean = (count > 0) ? (sum / count) : 0.0f;
        cache.period = 40.0f; 

        std::vector<float> mathSegment(uiTsamp, 0.0f);
        for (int i = 0; i < uiTsamp; ++i) {
            size_t idx = stableStartIndex + i;
            if (idx < channelData.size()) mathSegment[i] = channelData[idx];
        }
        triggerObj._update_buffer(mathSegment, cache);

        int widthPixels = static_cast<int>(size.x);
        float samplesPerPixel = static_cast<float>(viewWindowSize) / size.x;
        float midY = p0.y + (size.y / 2.0f);

        for (int x = 0; x < widthPixels - 1; ++x) {
            size_t startOffset = static_cast<size_t>(x * samplesPerPixel);
            size_t endOffset = static_cast<size_t>((x + 1) * samplesPerPixel);
            float minVal = 0.0f; float maxVal = 0.0f; bool hasData = false;

            for (size_t s = startOffset; s < endOffset && s < static_cast<size_t>(viewWindowSize); ++s) {
                size_t idx = stableStartIndex + s;
                if (idx < channelData.size()) {
                    float val = channelData[idx];
        if (!hasData) { minVal = maxVal = val; hasData = true; }
        else { if (val < minVal) minVal = val; if (val > maxVal) maxVal = val; }
        }
        }
        float yTop = midY - (maxVal * (size.y / 2.0f));
        float yBottom = midY - (minVal * (size.y / 2.0f));
        drawList->AddLine(ImVec2(p0.x + x, yTop), ImVec2(p0.x + x, yBottom), color, 1.5f);
        }
        ImGui::Dummy(size);
    }

    void RenderUI() {
        ImGui::Begin("Oscilloscope Audio Synchronisé (SIMD Trigger)");

        // 1. BANQUE DE LECTURE (TREEVIEW DE LA PLAYLIST)
        ImGui::Text("Bibliothèque de morceaux chargés :");
        ImVec2 treeSize(ImGui::GetContentRegionAvail().x, 150.0f);
        
        // Cadre défilant pour accueillir le TreeView
        ImGui::BeginChild("TreeViewChild", treeSize, true);
        if (ImGui::TreeNodeEx("Mes Fichiers Audio", ImGuiTreeNodeFlags_DefaultOpen)) {
            for (size_t i = 0; i < playlist.size(); ++i) {
                const auto& track = playlist[i];
                
                // Formater l'affichage textuel enrichi de chaque TreeItem
                std::string bitDepthInfo = (track.bitDepth > 0) ? std::to_string(track.bitDepth) + "-bit" : "VBR";
                std::string chInfo = (track.channels == 2) ? "Stéréo" : (track.channels == 1 ? "Mono" : std::to_string(track.channels) + "ch");
                
                std::string itemText = track.fileName + " [" + chInfo + " | " + track.durationStr + " | " + bitDepthInfo + "]";
                
                ImGuiTreeNodeFlags nodeFlags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
                // CORRECTION : static_cast<int> explicite
                if (static_cast<int>(i) == selectedTrackIndex) {
                    nodeFlags |= ImGuiTreeNodeFlags_Selected;
                }

                // Afficher le TreeItem
                ImGui::TreeNodeEx((void*)(intptr_t)i, nodeFlags, "%s", itemText.c_str());
                
                // Si l'utilisateur clique sur un item, on déclenche la lecture immédiate du morceau
                if (ImGui::IsItemClicked()) {
                    // CORRECTION : static_cast<int> explicite
                    selectedTrackIndex = static_cast<int>(i);
                    LoadAudioFile(track.filePath);
                }
            }
            ImGui::TreePop();
        }
        ImGui::EndChild();

        if (ImGui::Button("Importer un nouveau fichier...")) {
            fileDialog.Open();
        }

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
                // CORRECTION : static_cast<int> explicite (et suppression de la ligne parasite "Dot Net Perls")
                if (currentPlaybackPos >= static_cast<int>(channelLeft.size())) {
                    currentPlaybackPos = 0;
                }
            }

            ImGui::SliderInt("Fenêtre d'affichage (Zoom)", &viewWindowSize, 256, 4096, "%d samples");
            
            // CORRECTION : static_cast<float> explicite
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
            LoadAudioFile(fileDialog.GetSelected().string());
            fileDialog.ClearSelected();
        }
    }

};

